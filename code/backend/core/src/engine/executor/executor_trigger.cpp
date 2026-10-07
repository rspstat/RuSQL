// Triggers. CREATE TRIGGER ... FOR EACH ROW runs its body once for each row the statement touches, with the row's values as NEW.x (the row an INSERT
// adds, the row an UPDATE leaves) and OLD.x (the row an UPDATE or DELETE replaces). A trigger used to run once per statement whatever the number
// of rows -- also once for an INSERT that added nothing -- had no row to read, and a failing statement in its body was ignored.

#include "engine/executor/executor.hpp"

namespace engine {

namespace {
// A trigger body's own DML can fire further triggers (directly, or via a chain through
// another table) with no natural termination guarantee -- cap the depth so a runaway
// trigger chain fails loudly instead of recursing until stack overflow while holding
// SharedDatabase's exclusive write lock for the whole time. One level of nesting costs about 4.7 KB of stack in a
// Release build but 32 KB in a Debug build, which has a 1 MB stack: 32 levels no longer fit there.
constexpr std::size_t TRIGGER_MAX_DEPTH = 16;

struct TriggerDepthGuard {
    std::size_t& depth;
    explicit TriggerDepthGuard(std::size_t& d) : depth(d) { depth++; }
    ~TriggerDepthGuard() { depth--; }
};
} // namespace

bool Executor::has_trigger(const SharedDatabase& s, const std::string& table, const char* timing, const char* event) {
    for (auto& [name, def] : s.triggers) {
        (void)name;
        auto& [t, ti, ev, body] = def;
        (void)body;
        if (t == table && ti == timing && ev == event) return true;
    }
    return false;
}

StringResult Executor::fire_triggers(SharedDatabase& s, const std::string& table, const std::string& timing, const std::string& event,
                                     std::vector<TriggerRow>& rows) {
    if (trigger_depth_ >= TRIGGER_MAX_DEPTH) {
        return StringResult::Err("Trigger recursion exceeded maximum depth (" + std::to_string(TRIGGER_MAX_DEPTH) + ")");
    }
    std::vector<std::pair<std::string, std::vector<Statement>>> bodies;
    for (auto& [name, def] : s.triggers) {
        auto& [t, ti, ev, body] = def;
        if (t == table && ti == timing && ev == event) bodies.emplace_back(name, body);
    }
    if (bodies.empty() || rows.empty()) return StringResult::Ok("");

    TriggerDepthGuard guard(trigger_depth_);
    for (auto& [name, body] : bodies) {
        for (auto& row : rows) {
            std::unordered_map<std::string, std::string> vars;
            if (row.old_row) {
                for (auto& [column, value] : *row.old_row) {
                    if (!column.empty() && column[0] != '_') vars["OLD." + column] = value;
                }
            }
            if (row.new_row) {
                for (auto& [column, value] : *row.new_row) {
                    if (!column.empty() && column[0] != '_') vars["NEW." + column] = value;
                }
            }
            for (const Statement& original : body) {
                if (auto* set = std::get_if<Statement::ProcSet>(&original.data); set && set->name.size() > 4 && set->name[3] == '.') {
                    // SET NEW.x = expr: the value of the row that is inserted
                    std::string prefix = set->name.substr(0, 3);
                    for (char& c : prefix) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                    if (prefix != "NEW" || timing != "BEFORE" || event != "INSERT" || !row.new_row) {
                        return StringResult::Err("Trigger '" + name + "': SET " + prefix + ".x is supported in BEFORE INSERT triggers only");
                    }
                    ArithExpr expr = set->expr;
                    substitute_variables(expr, &vars);
                    bind_expression(s, expr, false);
                    const std::string column = set->name.substr(4);
                    const std::string value = eval_arith(Row{}, expr);
                    (*row.new_row)[column] = value;
                    vars["NEW." + column] = value;
                    continue;
                }
                Statement stmt = original;
                substitute_variables(stmt, &vars);
                auto result = execute_with_s(s, std::move(stmt));
                if (result.is_err()) {
                    return StringResult::Err("Trigger '" + name + "' failed" + (timing == "AFTER" ? " after the change was made" : "") + ": " + result.error());
                }
            }
        }
    }
    return StringResult::Ok("");
}

std::vector<Executor::TriggerRow> Executor::trigger_rows_for(SharedDatabase& s, const std::string& table, const std::optional<CondExpr>& condition,
                                                              const std::vector<std::pair<std::string, ArithExpr>>* assignments,
                                                              const PerRowValues* per_row) {
    std::vector<TriggerRow> out;
    auto it = s.tables.find(table);
    if (it == s.tables.end()) return out;
    const SnapshotCtx ctx = current_read_ctx(s);
    std::vector<std::string> key_columns;
    if (per_row) {
        if (const TableSchema* schema = s.catalog.get_table(table)) {
            for (auto& c : schema->columns) {
                if (c.primary_key) key_columns.push_back(c.name);
            }
        }
        if (key_columns.empty()) key_columns.push_back("id");
    }
    for (auto& r : it->second) {
        if (!is_visible_for_read(r, ctx)) continue;
        TriggerRow row;
        if (per_row) {
            std::string key; // the key a PerRowValues statement uses: the primary-key values joined with a NUL byte
            for (std::size_t i = 0; i < key_columns.size(); i++) {
                auto value = r.find(key_columns[i]);
                if (i) key += '\0';
                if (value != r.end()) key += value->second;
            }
            auto changed = per_row->find(key);
            if (changed == per_row->end()) continue;
            Row updated = r;
            for (auto& [column, value] : changed->second) updated[column] = value;
            row.new_row = std::move(updated);
        } else {
            if (!matches_condition_with_subquery(s, r, condition)) continue;
            if (assignments) {
                Row updated = r;
                for (auto& [column, expr] : *assignments) updated[column] = eval_arith(r, expr);
                row.new_row = std::move(updated);
            }
        }
        row.old_row = r;
        out.push_back(std::move(row));
    }
    return out;
}

} // namespace engine
