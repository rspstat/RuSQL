// Triggers. CREATE TRIGGER ... FOR EACH ROW runs its body once for each row the statement touches, with the row's values as NEW.x (the row an INSERT
// adds, the row an UPDATE leaves) and OLD.x (the row an UPDATE or DELETE replaces). A trigger used to run once per statement whatever the number
// of rows -- also once for an INSERT that added nothing -- had no row to read, and a failing statement in its body was ignored.

#include "engine/executor/executor.hpp"

namespace engine {

namespace {
std::string to_lower_str(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

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
    // What a trigger's own INSERT numbers (an AUTO_INCREMENT column of the table it writes) is not what LAST_INSERT_ID() says afterwards: MySQL restores it when the
    // trigger ends
    struct InsertIdRestore {
        Executor& ex;
        std::uint64_t last = ex.last_insert_id_, statement = ex.statement_insert_id_;
        bool unset = ex.insert_id_unset_;
        ~InsertIdRestore() {
            ex.last_insert_id_ = last;
            ex.statement_insert_id_ = statement;
            ex.insert_id_unset_ = unset;
        }
    } restore_insert_id{*this};
    for (auto& [name, body] : bodies) {
        for (auto& row : rows) {
            std::unordered_map<std::string, std::string> vars;
            if (row.old_row) {
                for (auto& [column, value] : *row.old_row) {
                    if (!column.empty() && column[0] != '_') vars["OLD." + to_lower_str(column)] = value; // (NEW.x and OLD.x are read apart from case)
                }
            }
            if (row.new_row) {
                for (auto& [column, value] : *row.new_row) {
                    if (!column.empty() && column[0] != '_') vars["NEW." + to_lower_str(column)] = value;
                }
            }
            // The row's values are variables of the body while it runs: the condition of an IF or a WHILE, the expression a CALL is given and a SELECT ... INTO read NEW.x / OLD.x
            // as the statements of the body do (NEW and OLD are no columns of any table, so they read as NULL). The procedure that is called has variables of its own and does not
            // see them -- nor does the body see the variables of a procedure whose statement fired the trigger.
            struct BodyScope {
                std::unordered_map<std::string, std::string>& values;
                std::unordered_map<std::string, std::string>& types;
                std::unordered_map<std::string, std::string> outer_values, outer_types;
                BodyScope(std::unordered_map<std::string, std::string>& v, std::unordered_map<std::string, std::string>& t, const std::unordered_map<std::string, std::string>& row)
                    : values(v), types(t), outer_values(std::move(v)), outer_types(std::move(t)) {
                    values = row;
                    types.clear();
                }
                ~BodyScope() {
                    values = std::move(outer_values);
                    types = std::move(outer_types);
                }
            } scope(proc_vars, proc_var_types, vars);
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
                    std::string column = set->name.substr(4);
                    for (const auto& [stored, unused] : *row.new_row) { // (the column is the one of the table whatever case it is written in)
                        (void)unused;
                        if (to_lower_str(stored) == to_lower_str(column)) { column = stored; break; }
                    }
                    const std::string value = eval_arith(Row{}, expr);
                    (*row.new_row)[column] = value;
                    vars["NEW." + to_lower_str(column)] = value;
                    proc_vars["NEW." + to_lower_str(column)] = value;
                    continue;
                }
                Statement stmt = original;
                resolve_names(s, stmt);
                substitute_variables(stmt, &vars);
                auto result = execute_with_s(s, std::move(stmt));
                if (result.is_err()) {
                    return StringResult::Err("Trigger '" + name + "' failed" + (timing == "AFTER" ? " after the change was made" : "") + ": " + result.error());
                }
                // SET NEW.x inside an IF or a loop of the body: the row that is inserted takes the value, in a BEFORE INSERT trigger only (as at the top of the body)
                for (auto& [key, value] : vars) {
                    if (key.compare(0, 4, "NEW.") != 0) continue;
                    auto now = proc_vars.find(key);
                    if (now == proc_vars.end() || now->second == value) continue;
                    if (timing != "BEFORE" || event != "INSERT" || !row.new_row) {
                        return StringResult::Err("Trigger '" + name + "': SET NEW.x is supported in BEFORE INSERT triggers only");
                    }
                    for (auto& [stored, current] : *row.new_row) {
                        if (to_lower_str(stored) == key.substr(4)) {
                            current = now->second;
                            break;
                        }
                    }
                    value = now->second;
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
    // NEW.col is the value the column will hold: the value an assignment gives, as the column writes it (a DECIMAL(10,2) says 109.00, not 109)
    const TableSchema* table_schema = s.catalog.get_table(table);
    auto typed = [&](const std::string& column, std::string value) {
        if (!table_schema) return value;
        for (const ColumnDef& c : table_schema->columns) {
            if (c.name != column) continue;
            std::string coerced = value;
            if (!coerce_column_value(c, coerced, 1)) return coerced; // (a value the column refuses stays as it is: the statement reports it)
            break;
        }
        return value;
    };
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
            for (auto& [column, value] : changed->second) updated[column] = typed(column, value);
            row.new_row = std::move(updated);
        } else {
            if (!matches_condition_with_subquery(s, r, condition)) continue;
            if (assignments) {
                Row updated = r;
                for (auto& [column, expr] : *assignments) updated[column] = typed(column, eval_arith(r, expr));
                row.new_row = std::move(updated);
            }
        }
        row.old_row = r;
        out.push_back(std::move(row));
    }
    return out;
}

} // namespace engine
