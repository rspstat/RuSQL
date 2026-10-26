// A DELETE on a table whose foreign key points at the table itself (emp.mgr -> emp.id): which rows go, which are rewritten, which statement is refused.
//
// The other tables' children are found by the loops of exec_delete_inner, one level deep from the rows the statement names. A table that is its own child cannot be
// handled like that: a row that goes may have children that go with it (ON DELETE CASCADE), and those have children again; a child that is itself among the rows
// the statement names is deleted before the scan gets there; RESTRICT depends on which rows are already gone. InnoDB does all of it row by row, so this does the same
// -- on the rows in memory, writing nothing -- and hands exec_delete_inner the result.

#include <algorithm>
#include <functional>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

#include "engine/collation.hpp"
#include "engine/executor/executor.hpp"
#include "engine/numeric_text.hpp"
#include "engine/sql_value_util.hpp"

namespace engine {

namespace {
// How many levels of cascade InnoDB follows below a row the statement names: the 15th level fails (error 3008).
constexpr int kCascadeDepthLimit = 15;
const std::string kNull = EXECUTOR_NULL_VALUE;
} // namespace

std::optional<std::string> Executor::plan_self_delete(SharedDatabase& s, const std::string& table, const std::vector<Row>& targets, const std::vector<std::string>* order,
                                                       const std::optional<CondExpr>& condition, SelfDeletePlan& plan) {
    const TableSchema* schema = s.catalog.get_table(table);
    auto rows_it = s.tables.find(table);
    if (!schema || rows_it == s.tables.end()) return std::nullopt;

    struct SelfKey {
        std::string column, ref_column;
        FkAction action;
        std::string default_value;                                           // what SET DEFAULT writes
        std::unordered_map<std::string, std::vector<std::size_t>> children;  // positions of the live rows by the collation key of their value in `column`
    };
    std::vector<SelfKey> keys;
    for (auto& col : schema->columns) {
        if (!col.foreign_key || col.foreign_key->ref_table != table) continue;
        keys.push_back({col.name, col.foreign_key->ref_column, col.foreign_key->on_delete, col.default_value.value_or(kNull), {}});
    }
    if (keys.empty()) return std::nullopt;

    // (the cache of foreign key values says without a scan that no row refers to a value: the usual DELETE of a row nobody refers to ends here)
    const bool has_children = std::any_of(targets.begin(), targets.end(), [&](const Row& row) {
        return std::any_of(keys.begin(), keys.end(), [&](const SelfKey& key) {
            auto it = row.find(key.ref_column);
            return it != row.end() && it->second != kNull && !fk_children_absent(s, table, key.column, it->second);
        });
    });
    if (!has_children) return std::nullopt;
    plan.active = true;

    std::vector<Row>& rows = rows_it->second;
    const std::vector<std::string> identity = schema->identity_columns();
    auto identity_of = [&](const Row& row) {
        std::string out;
        if (identity.empty()) { // (no key: every column says which row it is)
            std::vector<std::string> parts;
            for (auto& [name, value] : row) {
                if (!name.empty() && name[0] != '_') parts.push_back(name + '\x01' + value);
            }
            std::sort(parts.begin(), parts.end());
            for (auto& part : parts) out += part + '\x00';
            return out;
        }
        for (auto& column : identity) {
            auto it = row.find(column);
            out += (it != row.end() ? it->second : kNull) + '\x00';
        }
        return out;
    };

    std::unordered_set<std::string> named;
    for (auto& row : targets) named.insert(identity_of(row));
    std::unordered_map<std::string, std::size_t> position_of; // the positions of the rows the statement names
    for (std::size_t pos = 0; pos < rows.size(); pos++) {
        const Row& row = rows[pos];
        if (!is_visible(row)) continue;
        if (std::string id = identity_of(row); named.count(id)) position_of.emplace(std::move(id), pos);
        for (auto& key : keys) {
            auto it = row.find(key.column);
            if (it != row.end() && it->second != kNull) key.children[collation::key(it->second)].push_back(pos);
        }
    }

    std::vector<char> gone(rows.size(), 0);
    std::unordered_map<std::size_t, Row> rewritten; // a row as the SET NULL / SET DEFAULT of this statement have left it
    auto image_of = [&](std::size_t pos) -> const Row& {
        auto it = rewritten.find(pos);
        return it != rewritten.end() ? it->second : rows[pos];
    };
    auto value_of = [&](std::size_t pos, const std::string& column) -> const std::string& {
        const Row& row = image_of(pos);
        auto it = row.find(column);
        return it != row.end() ? it->second : kNull;
    };

    std::function<std::optional<std::string>(std::size_t, int)> delete_row = [&](std::size_t pos, int level) -> std::optional<std::string> {
        gone[pos] = 1;
        plan.doomed.push_back(image_of(pos));
        if (level > 0) plan.cascaded.push_back(pos);
        for (auto& key : keys) {
            const std::string parent_value = value_of(pos, key.ref_column);
            if (parent_value == kNull) continue; // (a NULL is referred to by nothing)
            std::vector<std::size_t> children; // the live rows that refer to this one (the rows already gone, and this row, are not)
            if (auto it = key.children.find(collation::key(parent_value)); it != key.children.end()) {
                for (std::size_t child : it->second) {
                    if (!gone[child] && collation::equal(value_of(child, key.column), parent_value)) children.push_back(child);
                }
            }
            switch (key.action) {
                case FkAction::Restrict:
                    // (a row that is its own parent counts: it is there to see until it is gone)
                    if (!children.empty() || collation::equal(value_of(pos, key.column), parent_value)) {
                        return "Foreign key violation: row in '" + table + "' is referenced by '" + table + "'.'" + key.column + "'";
                    }
                    break;
                case FkAction::Cascade:
                    if (!children.empty() && level + 1 >= kCascadeDepthLimit) return std::string("Foreign key cascade delete/update exceeds max depth of 15.");
                    for (std::size_t child : children) {
                        if (gone[child]) continue; // (an earlier child's cascade has reached it)
                        if (auto refused = delete_row(child, level + 1)) return refused;
                    }
                    break;
                case FkAction::SetNull:
                case FkAction::SetDefault:
                    for (std::size_t child : children) {
                        Row image = image_of(child);
                        image[key.column] = key.action == FkAction::SetNull ? kNull : key.default_value;
                        rewritten[child] = std::move(image);
                    }
                    break;
            }
        }
        return std::nullopt;
    };

    // the order InnoDB reads the rows in: the ORDER BY of the statement, else the key
    std::vector<std::size_t> sequence(targets.size());
    std::iota(sequence.begin(), sequence.end(), std::size_t{0});
    if (order && !order->empty()) {
        std::unordered_map<std::string, std::size_t> rank;
        for (std::size_t i = 0; i < order->size(); i++) {
            // (an identity key of rows_of_limit joins the key columns with \x00; identity_of ends each with it)
            rank.emplace((*order)[i] + '\x00', i);
        }
        auto rank_of = [&](std::size_t i) {
            auto it = rank.find(identity_of(targets[i]));
            return it != rank.end() ? it->second : rank.size();
        };
        std::stable_sort(sequence.begin(), sequence.end(), [&](std::size_t a, std::size_t b) { return rank_of(a) < rank_of(b); });
    } else if (!identity.empty()) {
        std::vector<bool> numeric;
        for (auto& column : identity) {
            auto def = std::find_if(schema->columns.begin(), schema->columns.end(), [&](const ColumnDef& c) { return c.name == column; });
            numeric.push_back(def != schema->columns.end() && class_of_type(def->data_type) != ValueClass::Text);
        }
        std::stable_sort(sequence.begin(), sequence.end(), [&](std::size_t a, std::size_t b) {
            for (std::size_t i = 0; i < identity.size(); i++) {
                auto x = targets[a].find(identity[i]);
                auto y = targets[b].find(identity[i]);
                if (x == targets[a].end() || y == targets[b].end()) continue;
                int cmp;
                std::optional<std::int64_t> xi, yi;
                if (numeric[i] && (xi = parse_int64_text(x->second)) && (yi = parse_int64_text(y->second))) cmp = *xi < *yi ? -1 : (*xi > *yi ? 1 : 0);
                else cmp = collation::compare(x->second, y->second);
                if (cmp != 0) return cmp < 0;
            }
            return false;
        });
    }

    const bool reread = !condition_has_subquery(condition);
    for (std::size_t i : sequence) {
        auto at = position_of.find(identity_of(targets[i]));
        if (at == position_of.end()) continue;
        const std::size_t pos = at->second;
        if (gone[pos]) continue; // a cascade has deleted it before the scan got there
        // a SET NULL of an earlier row may have taken this row out of the WHERE: the scan reads the row as it is by then
        if (reread && rewritten.count(pos) && !matches_condexpr(rewritten[pos], condition)) continue;
        if (auto refused = delete_row(pos, 0)) return refused;
    }

    // the rows that stay but are rewritten (a row that goes is not worth rewriting)
    for (auto& [pos, image] : rewritten) {
        if (gone[pos]) continue;
        std::vector<std::pair<std::string, std::string>> changes;
        for (auto& key : keys) {
            if (key.action == FkAction::Restrict || key.action == FkAction::Cascade) continue;
            auto now = image.find(key.column);
            auto before = rows[pos].find(key.column);
            if (now != image.end() && (before == rows[pos].end() || now->second != before->second)) changes.emplace_back(key.column, now->second);
        }
        if (!changes.empty()) plan.rewrites[std::move(changes)].push_back(pos);
    }
    return std::nullopt;
}

} // namespace engine
