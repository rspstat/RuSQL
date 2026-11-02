// Faithful port of MERGE from rusql-core/src/engine/executor.rs (Phase 8f): exec_merge.

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "engine/join_equality.hpp"
#include "engine/statement_deadline.hpp"
#include "engine/storage/numeric_key.hpp"

namespace engine {

namespace {
// An AND-ed part of the ON of a MERGE of the form `<target column> = <source column>`, each side named by the target's (or the source's) name or alias, that a hash can answer:
// both sides numbers, or both text (what the binder says of the columns). Every pair of rows that meets the whole ON meets it.
struct MergeEquality {
    std::string target_col, source_col;
};

std::optional<MergeEquality> merge_equality(const CondExpr& on, const std::vector<std::string>& target_names, const std::vector<std::string>& source_names,
                                            const std::unordered_set<std::string>& target_cols, const std::unordered_set<std::string>& source_cols) {
    for (const auto& name : target_names) {
        if (std::find(source_names.begin(), source_names.end(), name) != source_names.end()) return std::nullopt; // (the same name for both: a reference says nothing)
    }
    auto contains = [](const std::vector<std::string>& names, const std::string& q) { return std::find(names.begin(), names.end(), q) != names.end(); };
    // 1: a column of the target, 2: a column of the source, 0: neither
    auto side = [&](const std::string& ref) -> std::pair<int, std::string> {
        auto cut = ref.rfind('.');
        if (cut == std::string::npos) return {0, ""};
        const std::string q = ref.substr(0, cut), c = ref.substr(cut + 1);
        if (contains(target_names, q) && target_cols.count(c)) return {1, c};
        if (contains(source_names, q) && source_cols.count(c)) return {2, c};
        return {0, ""};
    };
    std::vector<const CondExpr*> parts;
    and_conjuncts(on, parts);
    for (const CondExpr* part : parts) {
        auto* leaf = std::get_if<CondExpr::Leaf>(&part->data);
        if (!leaf || leaf->condition.op != Operator::Eq) continue;
        auto* l = std::get_if<ArithExpr::Col>(&leaf->condition.left.data);
        auto* r = std::get_if<ConditionValue::Literal>(&leaf->condition.value.data);
        if (!l || !r || r->quoted) continue;
        const auto a = side(l->name), b = side(r->value);
        if (!a.first || !b.first || a.first == b.first) continue;
        const ValueClass lc = leaf->condition.left_class, rc = leaf->condition.right_class;
        const bool both_text = lc == ValueClass::Text && rc == ValueClass::Text;
        const bool both_number = lc == ValueClass::Number && rc == ValueClass::Number;
        if (!both_text && !both_number) continue;
        return MergeEquality{a.first == 1 ? a.second : b.second, a.first == 1 ? b.second : a.second};
    }
    return std::nullopt;
}

std::string bare_name(const std::string& qualified) {
    auto pos = qualified.rfind('.');
    return pos == std::string::npos ? qualified : qualified.substr(pos + 1);
}

std::string trim_quotes(const std::string& v) {
    if (v.size() >= 2 && v.front() == '\'' && v.back() == '\'') return v.substr(1, v.size() - 2);
    return v;
}
} // namespace

StringResult Executor::exec_merge(SharedDatabase& s, std::string target, std::optional<std::string> target_alias, std::string source,
                                   std::optional<std::string> source_alias, CondExpr on,
                                   std::optional<std::vector<std::pair<std::string, ArithExpr>>> when_matched_update, bool when_matched_delete,
                                   std::optional<CondExpr> when_matched_delete_cond, std::optional<CondExpr> when_matched_update_cond,
                                   bool when_matched_update_first, std::optional<std::vector<std::string>> when_not_matched_columns,
                                   std::vector<std::string> when_not_matched_values) {
    // Until the first change is written the statement only reads (it matches the source rows to the target rows, then checks what it is going to write): KILL QUERY may stop
    // it there (the first word of this statement is MERGE)
    std::optional<deadline::ReadPhase> reads;
    reads.emplace();
    auto sit = s.tables.find(source);
    if (sit == s.tables.end()) return StringResult::Err("Table '" + source + "' not found");
    std::vector<Row> source_rows;
    for (auto& r : sit->second) {
        if (is_visible(r)) source_rows.push_back(r);
    }

    auto tit = s.tables.find(target);
    if (tit == s.tables.end()) return StringResult::Err("Table '" + target + "' not found");
    std::vector<Row> target_rows;
    for (auto& r : tit->second) {
        if (is_visible(r)) target_rows.push_back(r);
    }

    const TableSchema* schema0 = s.catalog.get_table(target);
    if (!schema0) return StringResult::Err("Table '" + target + "' not found");
    std::vector<std::string> pk_cols;
    for (auto& c : schema0->columns) {
        if (c.primary_key) pk_cols.push_back(c.name);
    }
    if (pk_cols.empty()) pk_cols.push_back("id");
    // Composite identity key, same fix/reasoning as UPDATE (executor_update.cpp) and
    // multi-table UPDATE/DELETE (executor_multi.cpp) -- a composite-PK target table's
    // rows must be identified by ALL of its PK columns, not just the first, or two
    // distinct rows sharing the leading column's value get conflated (MERGE could then
    // update/delete the wrong physical row, or delete both).
    auto row_key = [&pk_cols](const Row& r) {
        std::string key;
        for (std::size_t i = 0; i < pk_cols.size(); i++) {
            if (i) key += '\x00';
            auto it = r.find(pk_cols[i]);
            key += (it != r.end() ? it->second : std::string());
        }
        return key;
    };
    std::vector<std::string> target_col_names;
    for (auto& c : schema0->columns) target_col_names.push_back(c.name);

    std::string target_base = bare_name(target);
    std::string source_base = bare_name(source);

    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> update_rows;
    std::vector<std::string> delete_pks;
    const std::vector<std::string> insert_cols = when_not_matched_columns.value_or(target_col_names);
    std::vector<std::vector<std::string>> insert_values;

    // The target rows hashed on the equality of the ON that a hash can answer: a source row then meets only the target rows whose key equals its own, in the order of the table,
    // and the first of them that meets the whole ON is the one used -- as in the loop over every target row. Every pair the hash lets through is still checked against the whole ON, so
    // the answer is the loop's; what it saves is a merged row for each of the 300 x 300,000 pairs of rows, which took minutes.
    std::optional<MergeEquality> keyed;
    std::unordered_map<std::string, std::vector<std::size_t>> targets_of_key;
    auto key_of = [](const std::string& value) { return normalize_numeric_key(value); }; // (a number by its value, a text by the weights of the collation)
    if (const TableSchema* source_schema = s.catalog.get_table(source)) {
        std::vector<std::string> target_names{target_base}, source_names{source_base};
        if (target_alias) target_names.push_back(*target_alias);
        if (source_alias) source_names.push_back(*source_alias);
        std::unordered_set<std::string> target_cols(target_col_names.begin(), target_col_names.end()), source_cols;
        for (auto& c : source_schema->columns) source_cols.insert(c.name);
        keyed = merge_equality(on, target_names, source_names, target_cols, source_cols);
    }
    if (keyed) {
        for (std::size_t i = 0; i < target_rows.size() && keyed; i++) {
            auto value = target_rows[i].find(keyed->target_col);
            if (value == target_rows[i].end()) keyed.reset(); // (a row that does not hold the column: the loop over every target row does it)
            else if (value->second != EXECUTOR_NULL_VALUE) targets_of_key[key_of(value->second)].push_back(i);
        }
    }
    static const std::vector<std::size_t> no_targets;

    for (auto& src_row : source_rows) {
        bool found = false;
        const std::vector<std::size_t>* only = nullptr; // the target rows this source row can meet (nullptr: every one)
        if (keyed) {
            if (auto value = src_row.find(keyed->source_col); value != src_row.end()) {
                only = &no_targets; // (a NULL key meets nothing)
                if (value->second != EXECUTOR_NULL_VALUE) {
                    if (auto meets = targets_of_key.find(key_of(value->second)); meets != targets_of_key.end()) only = &meets->second;
                }
            }
        }
        const std::size_t candidates = only ? only->size() : target_rows.size();
        for (std::size_t c = 0; c < candidates; c++) {
            const Row& tgt_row = target_rows[only ? (*only)[c] : c];
            Row merged = tgt_row;
            for (auto& [k, v] : tgt_row) merged[target_base + "." + k] = v;
            if (target_alias) {
                for (auto& [k, v] : tgt_row) merged[*target_alias + "." + k] = v;
            }
            for (auto& [k, v] : src_row) {
                if (!merged.count(k)) merged[k] = v;
                merged[source_base + "." + k] = v;
            }
            if (source_alias) {
                for (auto& [k, v] : src_row) merged[*source_alias + "." + k] = v;
            }

            if (eval_condexpr_with_subquery(s, merged, on)) {
                std::string pk = row_key(tgt_row);
                found = true;
                // the first WHEN MATCHED clause, in the order they were written, whose condition holds is the one used
                const bool delete_ok = when_matched_delete && matches_condition_with_subquery(s, merged, when_matched_delete_cond);
                const bool update_ok = when_matched_update && matches_condition_with_subquery(s, merged, when_matched_update_cond);
                const bool do_update = when_matched_update_first ? update_ok : (update_ok && !delete_ok);
                const bool do_delete = when_matched_update_first ? (delete_ok && !update_ok) : delete_ok;
                if (do_delete) {
                    delete_pks.push_back(pk);
                } else if (do_update) {
                    std::vector<std::pair<std::string, std::string>> resolved;
                    for (auto& [col, expr] : *when_matched_update) resolved.emplace_back(col, eval_arith(merged, expr));
                    update_rows.emplace_back(pk, std::move(resolved));
                }
                break;
            }
        }
        if (!found && !when_not_matched_values.empty()) {
            std::vector<std::string> values;
            for (std::size_t i = 0; i < insert_cols.size(); i++) {
                std::string raw = i < when_not_matched_values.size() ? when_not_matched_values[i] : std::string();
                if (raw.size() >= 2 && raw.front() == '\'' && raw.back() == '\'') {
                    values.push_back(trim_quotes(raw));
                } else if (auto it = src_row.find(raw); it != src_row.end()) {
                    values.push_back(it->second);
                } else if (auto dot = raw.find('.'); dot != std::string::npos) {
                    auto it2 = src_row.find(raw.substr(dot + 1));
                    values.push_back(it2 != src_row.end() ? it2->second : trim_quotes(raw));
                } else {
                    values.push_back(trim_quotes(raw));
                }
            }
            insert_values.push_back(std::move(values));
        }
    }

    // Each kind of change is one statement of the one-table paths (MVCC, undo log, constraints, indexes, triggers, foreign-key
    // actions). They are ordered so that a failure changes nothing: the rows to insert are checked without writing, the deletes are
    // checked against ON DELETE RESTRICT, the update is one statement (checked as a whole before any row is rewritten); then
    // the update, the deletes and the inserts run. (The old code rewrote, erased and appended rows in place, checking nothing.)
    const std::size_t update_count = update_rows.size();
    const std::size_t delete_count = delete_pks.size();
    const std::size_t insert_count = insert_values.size();

    std::vector<Row> doomed;
    for (auto& r : target_rows) {
        if (std::find(delete_pks.begin(), delete_pks.end(), row_key(r)) != delete_pks.end()) doomed.push_back(r);
    }
    if (!insert_values.empty()) {
        if (auto checked = exec_insert_inner(s, target, insert_cols, insert_values, InsertConflict(InsertConflict::Abort{}), std::nullopt, /*validate_only=*/true);
            checked.is_err()) {
            return checked;
        }
    }
    if (!doomed.empty()) { // (the whole chain of foreign keys below the rows is followed, so that the UPDATE before the deletes is not made for nothing)
        std::vector<std::string> below = fk_children_closure(s, target);
        below.erase(std::remove(below.begin(), below.end(), target), below.end());
        auto child_lock = acquire_table_data_locks(s, below, /*exclusive=*/false);
        CascadePlan plan;
        if (auto refused = plan_cascade_delete(s, target, doomed, nullptr, nullptr, std::nullopt, plan)) return StringResult::Err(*refused);
    }

    // composite keys travel as the \x00-joined values of the primary-key columns, in the order of `pk_cols`
    auto key_values = [&](const std::string& key) {
        std::vector<std::string> out;
        std::size_t from = 0;
        for (std::size_t i = 0; i < pk_cols.size(); i++) {
            std::size_t to = key.find('\x00', from);
            out.push_back(key.substr(from, to == std::string::npos ? std::string::npos : to - from));
            from = to == std::string::npos ? key.size() : to + 1;
        }
        return out;
    };

    reads.reset(); // (what follows writes: each statement of the one-table paths stops, or not, as it does on its own)
    if (!update_rows.empty()) {
        PerRowValues per_row;
        std::vector<std::pair<std::string, ArithExpr>> columns;
        for (auto& [pk, resolved] : update_rows) {
            auto& values = per_row[pk];
            for (auto& [col, val] : resolved) {
                values[col] = val; // a target row matched by several source rows takes the last one's values
                if (std::none_of(columns.begin(), columns.end(), [&](auto& c) { return c.first == col; })) columns.emplace_back(col, ArithExpr(ArithExpr::Str{""}));
            }
        }
        // a row that one source row updates and another does not name a column of: leave that column as it is
        for (auto& [pk, values] : per_row) {
            for (auto& [col, _] : columns) {
                if (!values.count(col)) {
                    for (auto& r : target_rows) {
                        if (row_key(r) == pk) {
                            auto it = r.find(col);
                            values[col] = it != r.end() ? it->second : std::string(EXECUTOR_NULL_VALUE);
                            break;
                        }
                    }
                }
            }
        }
        if (auto r = exec_update(s, target, std::move(columns), std::nullopt, std::nullopt, &per_row); r.is_err()) return r;
    }

    if (!delete_pks.empty()) {
        std::optional<CondExpr> cond;
        for (auto& pk : delete_pks) {
            auto vals = key_values(pk);
            std::optional<CondExpr> one;
            for (std::size_t i = 0; i < pk_cols.size(); i++) {
                CondExpr leaf = CondExpr(CondExpr::Leaf{Condition{ArithExpr(ArithExpr::Col{pk_cols[i]}), Operator::Eq, ConditionValue(ConditionValue::Literal{vals[i], true})}});
                one = one ? CondExpr(CondExpr::And{std::make_unique<CondExpr>(std::move(*one)), std::make_unique<CondExpr>(std::move(leaf))}) : std::move(leaf);
            }
            cond = cond ? CondExpr(CondExpr::Or{std::make_unique<CondExpr>(std::move(*cond)), std::make_unique<CondExpr>(std::move(*one))}) : std::move(*one);
        }
        if (auto r = exec_delete(s, target, std::move(cond), std::nullopt); r.is_err()) return r;
    }

    if (!insert_values.empty()) {
        if (auto r = exec_insert(s, target, insert_cols, std::move(insert_values), InsertConflict(InsertConflict::Abort{}), std::nullopt); r.is_err()) return r;
    }

    return StringResult::Ok("MERGE: " + std::to_string(update_count) + " updated, " + std::to_string(delete_count) + " deleted, " +
                             std::to_string(insert_count) + " inserted.");
}

} // namespace engine
