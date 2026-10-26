// What a DELETE (and an UPDATE of a key) does through the foreign keys that refer to the table: which rows go, which are rewritten, which statement is refused.
//
// A foreign key action does not stop at the child: a child that goes with its parent (ON DELETE CASCADE) has children of its own, a child that is rewritten (SET NULL, ON UPDATE
// CASCADE) may hold a value that a third table refers to, a table can be its own child, and RESTRICT depends on which rows are already gone. InnoDB does all of it row by row, so
// this does the same -- on the rows in memory, writing nothing -- and hands the executor the result: per table the rows that go and the rows that are rewritten, or the error that
// refuses the statement before anything has changed.

#include <algorithm>
#include <functional>
#include <numeric>
#include <string_view>
#include <tuple>
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
constexpr std::size_t kNoPosition = static_cast<std::size_t>(-1);
// the lookups of the children of one foreign key column that are answered by a scan before the column is indexed for the rest of the statement
constexpr std::size_t kScansBeforeIndex = 6;
} // namespace

struct Executor::CascadeSim {
    struct Edge {
        std::string table, column, ref_column; // the child's table and foreign key column, the column of the parent it refers to
        FkAction on_delete, on_update;
        std::string default_value; // what SET DEFAULT writes
    };
    struct State {
        std::vector<Row>* rows = nullptr;
        std::vector<char> gone;                          // (by position) the rows that go
        std::unordered_map<std::size_t, Row> rewritten;  // a row as the SET NULL / SET DEFAULT / ON UPDATE of this statement have left it
    };
    struct Lookup {
        std::size_t scans = 0;
        bool indexed = false;
        std::unordered_map<std::string, std::vector<std::size_t>> index; // positions of the live rows by the collation key of their value, as the statement found them
    };

    Executor& ex;
    SharedDatabase& s;
    CascadePlan& plan;
    std::unordered_map<std::string, std::vector<Edge>> edges; // by the table the keys refer to, in the order of the child table's name and of its columns
    std::map<std::string, State> states;
    std::map<std::pair<std::string, std::string>, Lookup> lookups;
    std::map<std::tuple<std::string, std::string, std::string>, bool> asked; // what the cache of the child's foreign key values said, per column and value (it is asked once per statement)
    std::vector<std::string> path; // the tables whose values are being changed by the cascade that is being followed (a cascade that comes back to one acts as RESTRICT)

    CascadeSim(Executor& e, SharedDatabase& db, CascadePlan& p) : ex(e), s(db), plan(p) {
        std::vector<std::string> names;
        for (auto& [name, schema] : s.catalog.tables) {
            (void)schema;
            names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        for (auto& name : names) {
            for (auto& col : s.catalog.tables.at(name).columns) {
                if (!col.foreign_key) continue;
                edges[col.foreign_key->ref_table].push_back({name, col.name, col.foreign_key->ref_column, col.foreign_key->on_delete, col.foreign_key->on_update,
                                                              col.default_value.value_or(kNull)});
            }
        }
    }

    const std::vector<Edge>& edges_of(const std::string& parent) const {
        static const std::vector<Edge> none;
        auto it = edges.find(parent);
        return it != edges.end() ? it->second : none;
    }

    State* state_of(const std::string& table) {
        auto it = states.find(table);
        if (it != states.end()) return &it->second;
        auto rows = s.tables.find(table);
        if (rows == s.tables.end()) return nullptr;
        State& st = states[table];
        st.rows = &rows->second;
        st.gone.assign(rows->second.size(), 0);
        return &st;
    }

    const Row& image_of(const std::string& table, std::size_t pos) {
        State& st = *state_of(table);
        auto it = st.rewritten.find(pos);
        return it != st.rewritten.end() ? it->second : (*st.rows)[pos];
    }

    const std::string& value_of(const std::string& table, std::size_t pos, const std::string& column) {
        const Row& row = image_of(table, pos);
        auto it = row.find(column);
        return it != row.end() ? it->second : kNull;
    }

    // Does the cache of the child table's foreign key values prove that no live row refers to `parent_value`? (Each question counts for the cache's decision to be made: once.)
    bool absent_by_cache(const Edge& e, const std::string& parent_value) {
        auto key = std::make_tuple(e.table, e.column, parent_value);
        if (auto it = asked.find(key); it != asked.end()) return it->second;
        const bool absent = ex.fk_children_absent(s, e.table, e.column, parent_value);
        asked.emplace(std::move(key), absent);
        return absent;
    }

    // The live rows of the edge's table that refer to `parent_value` -- as the statement found them: a row that an earlier step rewrote to that value is not one, a row that it
    // rewrote away from it, or deleted, no longer is.
    std::vector<std::size_t> children_of(const Edge& e, const std::string& parent_value) {
        std::vector<std::size_t> out;
        if (parent_value == kNull) return out;
        State* st = state_of(e.table);
        if (!st) return out;
        if (absent_by_cache(e, parent_value)) return out; // (the cache of the child's foreign key values: no live row refers to it)
        Lookup& lookup = lookups[{e.table, e.column}];
        if (!lookup.indexed && ++lookup.scans > kScansBeforeIndex) {
            lookup.indexed = true;
            for (std::size_t pos = 0; pos < st->rows->size(); pos++) {
                const Row& row = (*st->rows)[pos];
                if (!is_visible(row)) continue;
                auto it = row.find(e.column);
                if (it != row.end() && it->second != kNull) lookup.index[collation::key(it->second)].push_back(pos);
            }
        }
        auto consider = [&](std::size_t pos) {
            if (st->gone[pos] || !is_visible((*st->rows)[pos])) return;
            if (collation::equal(value_of(e.table, pos, e.column), parent_value)) out.push_back(pos);
        };
        if (lookup.indexed) {
            if (auto it = lookup.index.find(collation::key(parent_value)); it != lookup.index.end()) {
                for (std::size_t pos : it->second) consider(pos);
            }
        } else {
            for (std::size_t pos = 0; pos < st->rows->size(); pos++) {
                const Row& row = (*st->rows)[pos];
                auto it = row.find(e.column);
                if (it != row.end() && collation::equal(it->second, parent_value)) consider(pos);
            }
        }
        return out;
    }

    static std::string restricted_delete(const std::string& table, const Edge& e) {
        return "Foreign key violation: row in '" + table + "' is referenced by '" + e.table + "'.'" + e.column + "'";
    }
    static std::string restricted_update(const std::string& table, const std::string& column, const Edge& e) {
        return "Foreign key violation (ON UPDATE RESTRICT): '" + column + "' of '" + table + "' is referenced by '" + e.table + "'.'" + e.column + "'";
    }
    static std::string too_deep() { return "Foreign key cascade delete/update exceeds max depth of 15."; }

    // The value of `column` of the rows of `table` that held `old_value` becomes `new_value` (an UPDATE of a key, or the SET NULL / SET DEFAULT / ON UPDATE CASCADE of a foreign key
    // column that something else refers to): the rows that refer to the old value follow their foreign key's ON UPDATE.
    std::optional<std::string> change_key(const std::string& table, const std::string& column, const std::string& old_value, const std::string& new_value, int level) {
        if (old_value == kNull || old_value == new_value) return std::nullopt;
        path.push_back(table);
        struct Pop {
            std::vector<std::string>& path;
            ~Pop() { path.pop_back(); }
        } pop{path};
        for (const Edge& e : edges_of(table)) {
            if (e.ref_column != column) continue;
            std::vector<std::size_t> kids = children_of(e, old_value);
            if (kids.empty()) continue;
            FkAction action = e.on_update;
            // (MySQL: a cascade that comes back to a table it has already changed acts as RESTRICT -- no self-referential ON UPDATE CASCADE / SET NULL)
            if (action != FkAction::Restrict && std::find(path.begin(), path.end(), e.table) != path.end()) action = FkAction::Restrict;
            if (action == FkAction::Restrict) return restricted_update(table, column, e);
            if (level + 1 >= kCascadeDepthLimit) return too_deep();
            const std::string value = action == FkAction::Cascade ? new_value : (action == FkAction::SetNull ? kNull : e.default_value);
            for (std::size_t kid : kids) {
                if (state_of(e.table)->gone[kid]) continue;
                if (auto refused = rewrite_column(e.table, kid, e.column, value, level + 1)) return refused;
            }
        }
        return std::nullopt;
    }

    // One row of `table` takes `new_value` in `column`: what refers to the old value there is asked first (RESTRICT refuses before the row is changed), then the row is rewritten.
    std::optional<std::string> rewrite_column(const std::string& table, std::size_t pos, const std::string& column, const std::string& new_value, int level) {
        const std::string old_value = value_of(table, pos, column);
        if (old_value == new_value) return std::nullopt;
        if (auto refused = change_key(table, column, old_value, new_value, level)) return refused;
        State& st = *state_of(table);
        Row image = image_of(table, pos);
        image[column] = new_value;
        st.rewritten[pos] = std::move(image);
        return std::nullopt;
    }

    // The row at `pos` of `table` goes: its children go, or are rewritten, or refuse it, as their foreign keys say.
    std::optional<std::string> delete_row(const std::string& table, std::size_t pos, int level) {
        State& st = *state_of(table);
        st.gone[pos] = 1;
        CascadePlan::Table& planned = plan.tables[table];
        planned.doomed.push_back(image_of(table, pos));
        if (level > 0) planned.deleted.push_back(pos);
        for (const Edge& e : edges_of(table)) {
            const std::string parent_value = value_of(table, pos, e.ref_column);
            if (parent_value == kNull) continue; // (a NULL is referred to by nothing)
            std::vector<std::size_t> kids = children_of(e, parent_value); // (the rows already gone, and this row, are not children)
            switch (e.on_delete) {
                case FkAction::Restrict:
                    // (a row that is its own parent counts: it is there to see until it is gone)
                    if (!kids.empty() || (e.table == table && collation::equal(value_of(table, pos, e.column), parent_value))) return restricted_delete(table, e);
                    break;
                case FkAction::Cascade:
                    if (!kids.empty() && level + 1 >= kCascadeDepthLimit) return too_deep();
                    for (std::size_t kid : kids) {
                        if (state_of(e.table)->gone[kid]) continue; // (an earlier child's cascade has reached it)
                        if (auto refused = delete_row(e.table, kid, level + 1)) return refused;
                    }
                    break;
                case FkAction::SetNull:
                case FkAction::SetDefault: {
                    const std::string value = e.on_delete == FkAction::SetNull ? kNull : e.default_value;
                    for (std::size_t kid : kids) {
                        if (state_of(e.table)->gone[kid]) continue;
                        if (auto refused = rewrite_column(e.table, kid, e.column, value, level + 1)) return refused;
                    }
                    break;
                }
            }
        }
        return std::nullopt;
    }

    // The rows that stay but are rewritten (a row that goes is not worth rewriting), grouped by what they take; and whether the statement changes anything beyond the rows it names.
    void finish() {
        for (auto& [table, st] : states) {
            for (auto& [pos, image] : st.rewritten) {
                if (st.gone[pos]) continue;
                const Row& before = (*st.rows)[pos];
                std::vector<std::pair<std::string, std::string>> changes;
                for (auto& [column, value] : image) {
                    auto it = before.find(column);
                    if (it == before.end() || it->second != value) changes.emplace_back(column, value);
                }
                std::sort(changes.begin(), changes.end());
                if (!changes.empty()) plan.tables[table].rewrites[std::move(changes)].push_back(pos);
            }
        }
        for (auto& [table, planned] : plan.tables) {
            (void)table;
            if (!planned.deleted.empty() || !planned.rewrites.empty()) plan.active = true;
        }
    }
};

std::optional<std::string> Executor::plan_cascade_delete(SharedDatabase& s, const std::string& table, const std::vector<Row>& targets, const std::vector<std::size_t>* positions,
                                                          const std::vector<std::string>* order, const std::optional<CondExpr>& condition, CascadePlan& plan) {
    plan = CascadePlan{};
    const TableSchema* schema = s.catalog.get_table(table);
    auto rows_it = s.tables.find(table);
    plan.tables[table].doomed = targets; // (every row that goes holds claims on what refers to it: replaced below by the rows the simulation really takes)
    if (!schema || rows_it == s.tables.end()) return std::nullopt;
    CascadeSim sim(*this, s, plan);
    const auto& root_edges = sim.edges_of(table);
    if (root_edges.empty()) return std::nullopt;

    // (the cache of foreign key values says without a scan that no row refers to a value: the usual DELETE of a row nobody refers to ends here)
    const bool has_children = std::any_of(targets.begin(), targets.end(), [&](const Row& row) {
        return std::any_of(root_edges.begin(), root_edges.end(), [&](const CascadeSim::Edge& e) {
            auto it = row.find(e.ref_column);
            return it != row.end() && it->second != kNull && !sim.absent_by_cache(e, it->second);
        });
    });
    if (!has_children) return std::nullopt;
    plan.tables[table].doomed.clear();

    std::vector<Row>& rows = rows_it->second;
    CascadeSim::State& root = *sim.state_of(table);
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

    // where the rows the statement names are: where they were when they were read (checked: the row there must be live and hold the same key), else from the position cache
    // of the key, else by one pass over the table
    std::vector<std::size_t> position(targets.size(), kNoPosition);
    if (positions) {
        for (std::size_t i = 0; i < targets.size() && i < positions->size(); i++) {
            const std::size_t pos = (*positions)[i];
            if (pos >= rows.size() || !is_visible(rows[pos])) continue;
            bool same = !identity.empty();
            for (const std::string& column : identity) {
                auto mine = targets[i].find(column);
                auto there = rows[pos].find(column);
                same = same && mine != targets[i].end() && there != rows[pos].end() && mine->second == there->second;
            }
            if (same) position[i] = pos;
        }
    }
    if (identity.size() == 1) {
        const std::string& key_column = identity[0];
        auto cache = s.row_pk_pos.find(table);
        std::unordered_map<std::string_view, std::vector<std::size_t>> unresolved; // key -> the targets still to find
        for (std::size_t i = 0; i < targets.size(); i++) {
            if (position[i] != kNoPosition) continue;
            auto kit = targets[i].find(key_column);
            if (kit == targets[i].end()) continue;
            if (cache != s.row_pk_pos.end()) {
                if (auto hit = cache->second.find(kit->second); hit != cache->second.end() && hit->second < rows.size() && is_visible(rows[hit->second])) {
                    auto cell = rows[hit->second].find(key_column);
                    if (cell != rows[hit->second].end() && cell->second == kit->second) {
                        position[i] = hit->second;
                        continue;
                    }
                }
            }
            unresolved[kit->second].push_back(i);
        }
        if (!unresolved.empty()) {
            for (std::size_t pos = 0; pos < rows.size() && !unresolved.empty(); pos++) {
                if (!is_visible(rows[pos])) continue;
                auto cell = rows[pos].find(key_column);
                if (cell == rows[pos].end()) continue;
                if (auto it = unresolved.find(std::string_view(cell->second)); it != unresolved.end()) {
                    for (std::size_t i : it->second) position[i] = pos;
                    unresolved.erase(it);
                }
            }
        }
    } else {
        std::unordered_map<std::string, std::vector<std::size_t>> wanted;
        for (std::size_t i = 0; i < targets.size(); i++) {
            if (position[i] == kNoPosition) wanted[identity_of(targets[i])].push_back(i);
        }
        for (std::size_t pos = 0; pos < rows.size() && !wanted.empty(); pos++) {
            if (!is_visible(rows[pos])) continue;
            if (auto it = wanted.find(identity_of(rows[pos])); it != wanted.end()) {
                for (std::size_t i : it->second) position[i] = pos;
                wanted.erase(it);
            }
        }
    }

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
        const std::size_t pos = position[i];
        if (pos == kNoPosition) continue;
        if (root.gone[pos]) continue; // a cascade has deleted it before the scan got there
        // a SET NULL of an earlier row may have taken this row out of the WHERE: the scan reads the row as it is by then
        if (reread) {
            if (auto rewritten = root.rewritten.find(pos); rewritten != root.rewritten.end() && !matches_condexpr(rewritten->second, condition)) continue;
        }
        if (auto refused = sim.delete_row(table, pos, 0)) return refused;
    }
    sim.finish();
    return std::nullopt;
}

std::optional<std::string> Executor::plan_cascade_update(SharedDatabase& s, const std::string& table, const std::vector<std::pair<const Row*, const Row*>>& rows,
                                                          const std::vector<std::string>& changed_cols, CascadePlan& plan) {
    plan = CascadePlan{};
    CascadeSim sim(*this, s, plan);
    const auto& root_edges = sim.edges_of(table);
    if (root_edges.empty()) return std::nullopt;
    for (const auto& [old_row, new_row] : rows) {
        for (const std::string& column : changed_cols) {
            if (std::none_of(root_edges.begin(), root_edges.end(), [&](const CascadeSim::Edge& e) { return e.ref_column == column; })) continue; // (nothing refers to it)
            auto before = old_row->find(column);
            if (before == old_row->end()) continue;
            auto after = new_row->find(column);
            if (auto refused = sim.change_key(table, column, before->second, after != new_row->end() ? after->second : kNull, 0)) return refused;
        }
    }
    sim.finish();
    return std::nullopt;
}

} // namespace engine
