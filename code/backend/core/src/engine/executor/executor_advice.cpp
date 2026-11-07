// SUGGEST INDEX <SELECT | UPDATE | DELETE> (R65): reads the statement, the columns and indexes of its tables and how many different values a column holds, and says which index would
// let the statement stop scanning -- the columns it searches by with `=` first, then one range column, then the ORDER BY -- and why, how many rows it would read with it, which
// indexes the tables have already, and what an index could not help with (a function around a column, a leading wildcard, an OR, a small table). The answer is a table with one row
// for each suggestion, existing index and note. Nothing is created. It is the reading the MCP tool suggest_indexes makes of the text of a statement, made on the parsed statement; the
// desktop client shows it under the plan of an EXPLAIN.

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_set>

#include "engine/numeric_text.hpp"
#include "engine/parser/parser.hpp"

namespace engine {

namespace {

constexpr std::size_t kSmallTableRows = 100; // a scan of fewer rows is cheap: no index is suggested
constexpr double kNotNarrowing = 0.2;        // an index that leaves more than this share of the table to read does not help
constexpr std::size_t kSampleRows = 100000;  // how many different values a column holds is measured on at most this many rows

bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string bare_of(const std::string& qualified) {
    const auto dot = qualified.rfind('.');
    return dot == std::string::npos ? qualified : qualified.substr(dot + 1);
}

std::string joined(const std::vector<std::string>& names) {
    std::string out;
    for (const auto& n : names) out += (out.empty() ? "" : ", ") + n;
    return out;
}

struct Index {
    std::string name;
    std::vector<std::string> columns;
};

// a table the statement names: what the statement searches, joins and sorts it by
struct Table {
    std::string key;  // as the catalog has it (database.table)
    std::string name; // the bare name
    std::vector<std::string> aliases;
    const TableSchema* schema = nullptr;
    std::size_t rows = 0;
    std::vector<Index> indexes;
    std::vector<std::string> eq, range, join;
};

struct Suggestion {
    std::string table, columns, why, create, block;
    std::size_t rows = 0, estimate = 0;
    bool has_estimate = false;
    bool needs_rewrite = false; // one branch of an OR: it can serve the branch only as a statement of its own
};

std::string lowered(std::string v) {
    for (auto& c : v) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v;
}

// What reading a whole statement found, shared by the blocks it is made of (the statement, each CTE, derived table and subquery): the suggestions, the indexes that exist
// already, the notes, and the names the CTEs give (they are no tables)
struct Reading {
    std::vector<Suggestion> suggestions, already;
    std::vector<std::string> notes;
    std::unordered_set<std::string> seen;
    std::unordered_map<std::string, std::size_t> distinct;
    std::unordered_set<std::string> cte_names; // lower case
    std::size_t tables_read = 0;
    void note(const std::string& text) {
        if (std::find(notes.begin(), notes.end(), text) == notes.end()) notes.push_back(text);
    }
};

// A query block of the statement that is read on its own, with its own tables: `role` says which ("" for the statement itself)
class Advisor {
public:
    // `outer`: the names (tables and aliases, lower case) of the blocks around this one, which a correlated subquery names in its tests
    Advisor(const SharedDatabase& s, Reading& reading, std::string role, std::vector<std::string> outer = {})
        : s_(s), r_(reading), role_(std::move(role)), outer_(std::move(outer)) {}

    // the blocks inside this one that the conditions named (a subquery after IN / EXISTS / a comparison), to be read on their own
    const std::vector<std::pair<const Statement*, std::string>>& nested() const { return nested_; }

    // what a block inside this one calls the tables around it: the names of this block's tables and aliases, and the outer ones this block has
    std::vector<std::string> names() const {
        std::vector<std::string> out = outer_;
        for (const Table& t : tables_) {
            out.push_back(lowered(t.name));
            for (const auto& a : t.aliases) out.push_back(lowered(a));
        }
        return out;
    }

    bool add_table(const std::string& name, const std::vector<std::string>& aliases) {
        if (r_.cte_names.count(lowered(bare_of(name)))) return false; // (a CTE is read where it is defined)
        auto key_it = s_.catalog.tables.find(name);
        if (key_it == s_.catalog.tables.end()) {
            note("The table " + bare_of(name) + " was not found.");
            return false;
        }
        r_.tables_read++;
        Table t;
        t.key = name;
        t.name = bare_of(name);
        t.aliases = aliases;
        t.schema = &key_it->second;
        if (auto data = s_.tables.find(name); data != s_.tables.end()) {
            for (const Row& row : data->second) {
                if (auto x = row.find("_xmax"); x != row.end() && x->second != "0") continue;
                t.rows++;
            }
        }
        const TableSchema& schema = *t.schema;
        std::vector<std::string> pk = schema.primary_key_columns;
        if (pk.empty()) {
            for (const auto& c : schema.columns) {
                if (c.primary_key) pk.push_back(c.name);
            }
        }
        if (!pk.empty()) t.indexes.push_back({"PRIMARY", pk});
        for (const auto& c : schema.columns) {
            if (c.unique && !c.primary_key) t.indexes.push_back({c.unique_constraint_name.value_or(c.name), {c.name}});
        }
        for (const auto& key : schema.unique_keys) t.indexes.push_back({key.name.value_or(key.columns.front()), key.columns});
        for (const auto& [index_name, meta] : s_.index_meta) {
            if (meta.first == schema.name) t.indexes.push_back({index_name, {meta.second}});
        }
        for (const auto& [index_name, meta] : s_.hash_index_meta) {
            if (meta.first == schema.name) t.indexes.push_back({index_name, {meta.second}});
        }
        for (const auto& [index_name, comp] : s_.composite_indexes) {
            if (comp.table == schema.name) t.indexes.push_back({index_name, comp.columns});
        }
        tables_.push_back(std::move(t));
        return true;
    }

    std::size_t table_count() const { return tables_.size(); }
    // a note of a block inside the statement says which one (before its full stop)
    void note(std::string text) {
        if (!role_.empty()) {
            if (!text.empty() && text.back() == '.') text.pop_back();
            text += " (in " + role_ + ").";
        }
        r_.note(text);
    }

    // [database.][table or alias.]column -> the table it belongs to, and the column as the table spells it (nothing: not a column of the statement's tables, or not told apart)
    Table* resolve(const std::string& written, std::string* column) {
        std::vector<std::string> parts;
        std::size_t from = 0;
        for (std::size_t dot; (dot = written.find('.', from)) != std::string::npos; from = dot + 1) parts.push_back(written.substr(from, dot - from));
        parts.push_back(written.substr(from));
        const std::string wanted = parts.back();
        const std::string qualifier = parts.size() >= 2 ? parts[parts.size() - 2] : std::string();
        Table* found = nullptr;
        std::string spelled;
        for (Table& t : tables_) {
            auto c = std::find_if(t.schema->columns.begin(), t.schema->columns.end(), [&](const ColumnDef& d) { return ieq(d.name, wanted); });
            if (c == t.schema->columns.end()) continue;
            if (!qualifier.empty()) {
                const bool named = ieq(t.name, qualifier) || std::any_of(t.aliases.begin(), t.aliases.end(), [&](const std::string& a) { return ieq(a, qualifier); });
                if (!named) continue;
            }
            if (found) return nullptr; // (two tables have it)
            found = &t;
            spelled = c->name;
        }
        if (found && column) *column = spelled;
        return found;
    }

    // the tests of a WHERE / ON: what each table is searched and joined by
    void walk(const CondExpr& e) {
        if (auto* a = std::get_if<CondExpr::And>(&e.data)) {
            walk(*a->lhs);
            walk(*a->rhs);
        } else if (std::holds_alternative<CondExpr::Or>(e.data)) {
            collect_nested(e);
            read_or(e);
        } else if (auto* n = std::get_if<CondExpr::Not>(&e.data)) {
            note("A NOT condition cannot be served by an index.");
            collect_nested(*n->inner);
        } else {
            leaf(std::get<CondExpr::Leaf>(e.data).condition);
        }
    }

    void using_columns(const std::vector<std::string>& columns, std::size_t joined_table) {
        for (const auto& c : columns) {
            for (std::size_t i = 0; i <= joined_table && i < tables_.size(); i++) {
                for (const auto& d : tables_[i].schema->columns) {
                    if (ieq(d.name, c) && std::find(tables_[i].join.begin(), tables_[i].join.end(), d.name) == tables_[i].join.end()) tables_[i].join.push_back(d.name);
                }
            }
        }
    }

    // ORDER BY / GROUP BY items: the table and the column of each that is a plain column
    std::vector<std::pair<Table*, std::string>> columns_of(const std::vector<std::string>& written) {
        std::vector<std::pair<Table*, std::string>> out;
        for (const auto& w : written) {
            std::string column;
            Table* t = resolve(w, &column);
            if (!t) break; // (an expression or a position ends what an index can serve)
            out.push_back({t, column});
        }
        return out;
    }

    void conclude(const std::vector<OrderBy>& order_by, const std::vector<std::string>& group_by, bool has_limit) {
        std::vector<std::string> order_names;
        for (const auto& o : order_by) order_names.push_back(o.column);
        auto sorted_by = columns_of(order_names);
        const bool one_direction = !order_by.empty() && std::all_of(order_by.begin(), order_by.end(), [&](const OrderBy& o) { return o.ascending == order_by.front().ascending; });
        const bool single = tables_.size() == 1;
        for (Table& t : tables_) {
            std::vector<std::string> counted = t.eq;
            std::stable_sort(counted.begin(), counted.end(), [&](const std::string& a, const std::string& b) { return distinct(t, a) > distinct(t, b); });
            std::vector<std::string> narrowing = counted;
            if (!t.range.empty()) narrowing.push_back(t.range.front());
            std::vector<std::string> order_columns;
            // the rows come out of the index in the order of the ORDER BY when the index is searched by equality only and the whole sort goes one way
            if (single && t.range.empty() && !sorted_by.empty() && sorted_by.size() == order_by.size() && one_direction &&
                std::all_of(sorted_by.begin(), sorted_by.end(), [&](const auto& p) { return p.first == &t; })) {
                for (const auto& p : sorted_by) {
                    if (std::find(narrowing.begin(), narrowing.end(), p.second) == narrowing.end()) order_columns.push_back(p.second);
                }
            }
            std::string why;
            if (!narrowing.empty()) {
                why = "searched by ";
                for (std::size_t i = 0; i < narrowing.size(); i++) {
                    const bool equal = std::find(t.eq.begin(), t.eq.end(), narrowing[i]) != t.eq.end();
                    why += (i ? ", " : "") + narrowing[i] + (equal ? " (=)" : " (range)");
                }
            }
            if (!order_columns.empty()) why += std::string(why.empty() ? "" : " and ") + "sorted by " + joined(order_columns) + " (the rows then come out of the index in order)";
            if (!narrowing.empty() || (!order_columns.empty() && has_limit)) {
                consider(t, narrowing, order_columns, why);
            } else if (!order_columns.empty()) {
                note("An index on " + t.name + "(" + joined(order_columns) + ") would spare the sort, but without a LIMIT the statement reads every row anyway.");
            }
            if (!t.join.empty()) consider(t, {t.join.front()}, {}, "joined on " + t.join.front() + ": the engine looks up matching rows through it");
        }
        // an OR over different columns: each branch could use an index of its own, as a statement of its own (the engine reads one index per statement)
        for (const auto& branches : or_branches_) {
            for (std::size_t k = 0; k < branches.per_branch.size(); k++) {
                consider(*branches.owner, branches.per_branch[k], {},
                         "branch " + std::to_string(k + 1) + " of an OR (searched by " + joined(branches.per_branch[k]) +
                             "): the engine reads one index per statement, so each branch has to be a statement of its own (see rewrites)",
                         true);
            }
        }
        if (single && !group_by.empty()) {
            Table& t = tables_.front();
            std::vector<std::string> group_columns;
            for (const auto& p : columns_of(group_by)) group_columns.push_back(p.second);
            if (!group_columns.empty() && t.eq.empty() && t.range.empty()) consider(t, group_columns, {}, "grouped by " + joined(group_columns));
        }
    }

private:
    // a column written with a qualifier that names a block around this one and none of this block's tables
    bool is_outer(const std::string& written) const {
        const auto dot = written.rfind('.');
        if (dot == std::string::npos) return false;
        std::string qualifier = written.substr(0, dot);
        if (const auto before = qualifier.rfind('.'); before != std::string::npos) qualifier = qualifier.substr(before + 1);
        qualifier = lowered(qualifier);
        for (const Table& t : tables_) {
            if (lowered(t.name) == qualifier) return false;
            for (const auto& a : t.aliases) {
                if (lowered(a) == qualifier) return false;
            }
        }
        return std::find(outer_.begin(), outer_.end(), qualifier) != outer_.end();
    }

    // the subqueries inside a condition (after IN / EXISTS / a comparison) are blocks of their own, read after this one
    void collect_nested(const CondExpr& e) {
        if (auto* a = std::get_if<CondExpr::And>(&e.data)) {
            collect_nested(*a->lhs);
            collect_nested(*a->rhs);
        } else if (auto* o = std::get_if<CondExpr::Or>(&e.data)) {
            collect_nested(*o->lhs);
            collect_nested(*o->rhs);
        } else if (auto* n = std::get_if<CondExpr::Not>(&e.data)) {
            collect_nested(*n->inner);
        } else {
            register_subquery(std::get<CondExpr::Leaf>(e.data).condition);
        }
    }

    void register_subquery(const Condition& c) {
        const auto* sub = std::get_if<ConditionValue::Subquery>(&c.value.data);
        if (!sub || !sub->query) return;
        std::string role = "a scalar subquery";
        if (c.op == Operator::Exists || c.op == Operator::NotExists) role = "an EXISTS subquery";
        else if (c.op == Operator::In || c.op == Operator::NotIn || c.quantifier != Quantifier::None) role = "an IN subquery";
        for (const auto& known : nested_) {
            if (known.first == sub->query.get()) return;
        }
        nested_.push_back({sub->query.get(), role});
    }

    void leaf(const Condition& c) {
        register_subquery(c);
        const auto* col = std::get_if<ArithExpr::Col>(&c.left.data);
        if (!col) {
            if (const ArithExpr::Col* inner = first_column(c.left); inner && resolve(inner->name, nullptr)) {
                note("A condition on " + bare_of(inner->name) + " wraps the column in an expression or a function, so no index can serve it as written (compare the bare column instead).");
            }
            return;
        }
        auto add = [](std::vector<std::string>& list, const std::string& name) {
            if (std::find(list.begin(), list.end(), name) == list.end()) list.push_back(name);
        };
        std::string column;
        Table* t = col->outer > 0 ? nullptr : resolve(col->name, &column);
        if (!t) {
            // `outer.x = inner.y` in a correlated subquery: the outer value is known for each run of the subquery, so it is a search on inner.y
            std::string inner_column;
            if (c.op == Operator::Eq && (col->outer > 0 || is_outer(col->name))) {
                if (Table* inner = column_value(c.value, &inner_column)) add(inner->eq, inner_column);
            }
            return;
        }
        switch (c.op) {
            case Operator::Eq: {
                std::string other_column;
                if (Table* other = column_value(c.value, &other_column)) { // a column on the other side: a join
                    if (other != t) {
                        add(t->join, column);
                        add(other->join, other_column);
                    }
                    return;
                }
                add(t->eq, column);
                return;
            }
            case Operator::IsNull:
            case Operator::In: add(t->eq, column); return;
            case Operator::Gt:
            case Operator::Lt:
            case Operator::Gte:
            case Operator::Lte:
            case Operator::Between: add(t->range, column); return;
            case Operator::Like: {
                const auto* lit = std::get_if<ConditionValue::Literal>(&c.value.data);
                if (lit && !lit->value.empty() && lit->value[0] != '%' && lit->value[0] != '_') add(t->range, column);
                else note("A LIKE on " + column + " starts with a wildcard, so an index cannot be used for it.");
                return;
            }
            default: return; // (<>, NOT IN, NOT LIKE, IS NOT NULL ... : an index does not narrow them)
        }
    }

    // an OR: the same column on every branch is an IN; tests of different columns cannot be served by one index (the engine reads one index per statement)
    void read_or(const CondExpr& e) {
        std::vector<const Condition*> leaves;
        bool simple = true;
        std::function<void(const CondExpr&)> gather = [&](const CondExpr& x) {
            if (auto* o = std::get_if<CondExpr::Or>(&x.data)) {
                gather(*o->lhs);
                gather(*o->rhs);
            } else if (auto* l = std::get_if<CondExpr::Leaf>(&x.data)) {
                leaves.push_back(&l->condition);
            } else {
                simple = false;
            }
        };
        gather(e);
        Table* owner = nullptr;
        std::string column;
        if (simple) {
            for (const Condition* c : leaves) {
                const auto* col = std::get_if<ArithExpr::Col>(&c->left.data);
                std::string spelled;
                Table* t = col ? resolve(col->name, &spelled) : nullptr;
                if (!t || (c->op != Operator::Eq && c->op != Operator::In) || (owner && (owner != t || column != spelled))) {
                    simple = false;
                    break;
                }
                owner = t;
                column = spelled;
            }
        }
        if (simple && owner) {
            if (std::find(owner->eq.begin(), owner->eq.end(), column) == owner->eq.end()) owner->eq.push_back(column);
            return;
        }
        // tests of different columns: each branch is read on its own (its `=` columns, then one range column), when every branch is a plain AND of tests of one table
        std::vector<const CondExpr*> branches;
        std::function<void(const CondExpr&)> split = [&](const CondExpr& x) {
            if (auto* o = std::get_if<CondExpr::Or>(&x.data)) {
                split(*o->lhs);
                split(*o->rhs);
            } else {
                branches.push_back(&x);
            }
        };
        split(e);
        Table* branch_owner = nullptr;
        bool readable = true;
        std::vector<std::vector<std::string>> per_branch;
        for (const CondExpr* branch : branches) {
            std::vector<const Condition*> tests;
            std::function<void(const CondExpr&)> flat = [&](const CondExpr& x) {
                if (auto* a = std::get_if<CondExpr::And>(&x.data)) {
                    flat(*a->lhs);
                    flat(*a->rhs);
                } else if (auto* l = std::get_if<CondExpr::Leaf>(&x.data)) {
                    tests.push_back(&l->condition);
                } else {
                    readable = false;
                }
            };
            flat(*branch);
            if (!readable) break;
            std::vector<std::string> eq, range;
            for (const Condition* c : tests) {
                const auto* col = std::get_if<ArithExpr::Col>(&c->left.data);
                std::string spelled;
                Table* t = col && col->outer == 0 ? resolve(col->name, &spelled) : nullptr;
                if (!t) continue;
                if (!branch_owner) branch_owner = t;
                if (t != branch_owner) continue;
                auto put = [&](std::vector<std::string>& list) {
                    if (std::find(list.begin(), list.end(), spelled) == list.end()) list.push_back(spelled);
                };
                switch (c->op) {
                    case Operator::Eq: if (!column_value(c->value, nullptr)) put(eq); break;
                    case Operator::In:
                    case Operator::IsNull: put(eq); break;
                    case Operator::Gt:
                    case Operator::Lt:
                    case Operator::Gte:
                    case Operator::Lte:
                    case Operator::Between: put(range); break;
                    case Operator::Like: {
                        const auto* lit = std::get_if<ConditionValue::Literal>(&c->value.data);
                        if (lit && !lit->value.empty() && lit->value[0] != '%' && lit->value[0] != '_') put(range);
                        break;
                    }
                    default: break;
                }
            }
            std::vector<std::string> columns = eq;
            if (!range.empty()) columns.push_back(range.front());
            per_branch.push_back(columns);
        }
        if (readable && branch_owner && std::all_of(per_branch.begin(), per_branch.end(), [](const auto& b) { return !b.empty(); })) {
            or_branches_.push_back({branch_owner, per_branch});
            return;
        }
        note("A condition combines columns with OR: one index cannot serve it (a UNION of its branches can use one index each).");
    }

    static const ArithExpr::Col* first_column(const ArithExpr& e) {
        if (auto* c = std::get_if<ArithExpr::Col>(&e.data)) return c;
        if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) {
            for (const auto& a : f->args) {
                if (auto* c = first_column(a)) return c;
            }
            return nullptr;
        }
        auto both = [](const std::unique_ptr<ArithExpr>& l, const std::unique_ptr<ArithExpr>& r) -> const ArithExpr::Col* {
            if (l) {
                if (auto* c = first_column(*l)) return c;
            }
            return r ? first_column(*r) : nullptr;
        };
        if (auto* a = std::get_if<ArithExpr::Add>(&e.data)) return both(a->lhs, a->rhs);
        if (auto* a = std::get_if<ArithExpr::Sub>(&e.data)) return both(a->lhs, a->rhs);
        if (auto* a = std::get_if<ArithExpr::Mul>(&e.data)) return both(a->lhs, a->rhs);
        if (auto* a = std::get_if<ArithExpr::Div>(&e.data)) return both(a->lhs, a->rhs);
        return nullptr;
    }

    // the right side of a comparison when it is a column of one of the statement's tables
    Table* column_value(const ConditionValue& v, std::string* column) {
        if (auto* lit = std::get_if<ConditionValue::Literal>(&v.data)) {
            if (lit->quoted || lit->outer > 0 || lit->value.empty() || parse_number(lit->value)) return nullptr;
            return resolve(lit->value, column);
        }
        if (auto* arith = std::get_if<ConditionValue::Arith>(&v.data)) {
            if (auto* c = std::get_if<ArithExpr::Col>(&arith->expr.data); c && c->outer == 0) return resolve(c->name, column);
        }
        return nullptr;
    }

    // how many different values a column holds (a NULL is one value); measured on a sample of the rows when there are very many
    std::size_t distinct(const Table& t, const std::string& column) {
        const std::string cache_key = t.key + '\x1f' + column;
        if (auto it = r_.distinct.find(cache_key); it != r_.distinct.end()) return it->second;
        std::size_t result = 0;
        if (auto data = s_.tables.find(t.key); data != s_.tables.end() && t.rows > 0) {
            const std::size_t step = std::max<std::size_t>(1, t.rows / kSampleRows);
            std::unordered_set<std::string> seen;
            std::size_t sampled = 0, index = 0;
            for (const Row& row : data->second) {
                if (auto x = row.find("_xmax"); x != row.end() && x->second != "0") continue;
                if (index++ % step != 0) continue;
                sampled++;
                auto v = row.find(column);
                seen.insert(collation::key(v != row.end() ? v->second : std::string(EXECUTOR_NULL_VALUE)));
            }
            result = seen.size();
            // a column that is nearly all different in the sample is nearly all different in the table
            if (step > 1 && sampled > 0 && seen.size() * 10 >= sampled * 9) result = std::max(result, t.rows * seen.size() / sampled);
        }
        r_.distinct[cache_key] = result;
        return result;
    }

    // the index that already serves a search on `columns` (its leading columns are those columns), or one on one of them that leaves only a few rows
    const Index* covering(const Table& t, const std::vector<std::string>& columns) {
        auto same = [&](const std::string& a, const std::string& b) { return ieq(a, b); };
        for (const Index& index : t.indexes) {
            if (index.columns.size() < columns.size()) continue;
            std::vector<std::string> lead(index.columns.begin(), index.columns.begin() + static_cast<std::ptrdiff_t>(columns.size()));
            const bool all_in = std::all_of(lead.begin(), lead.end(), [&](const std::string& l) {
                return std::any_of(columns.begin(), columns.end(), [&](const std::string& c) { return same(l, c); });
            });
            if (all_in) return &index;
        }
        if (t.rows) {
            for (const Index& index : t.indexes) {
                if (index.columns.empty()) continue;
                const bool searched = std::any_of(columns.begin(), columns.end(), [&](const std::string& c) { return same(index.columns.front(), c); });
                const std::size_t d = searched ? distinct(t, index.columns.front()) : 0;
                if (searched && d > 0 && t.rows / d <= 50) return &index;
            }
        }
        return nullptr;
    }

    // an index on the columns the statement searches by (`narrowing`) followed by the ones it sorts by (`order_columns`)
    void consider(Table& t, const std::vector<std::string>& narrowing, const std::vector<std::string>& order_columns, const std::string& why, bool needs_rewrite = false) {
        std::vector<std::string> columns = narrowing;
        columns.insert(columns.end(), order_columns.begin(), order_columns.end());
        if (columns.empty()) return;
        if (t.rows < kSmallTableRows) {
            note(t.name + " has only " + std::to_string(t.rows) + " rows: a scan is cheap, no index suggested.");
            return;
        }
        if (const Index* have = covering(t, columns)) {
            const std::string key = t.key + '\x1f' + joined(columns);
            if (r_.seen.insert(key).second) r_.already.push_back({t.name, joined(columns), "already served by the index " + have->name, "", role_, t.rows, 0, false, false});
            return;
        }
        // how many rows a search reads is only known for the `=` columns: how much a range keeps is not, so a range column adds no estimate and is never a reason to refuse
        std::vector<std::string> ranged, counted;
        for (const auto& c : narrowing) {
            const bool is_range = std::find(t.range.begin(), t.range.end(), c) != t.range.end() && std::find(t.eq.begin(), t.eq.end(), c) == t.eq.end();
            (is_range ? ranged : counted).push_back(c);
        }
        std::size_t estimate = 0;
        bool has_estimate = false;
        if (!counted.empty() && std::all_of(counted.begin(), counted.end(), [&](const std::string& c) { return distinct(t, c) > 0; })) {
            std::size_t combined = 1;
            for (const auto& c : counted) combined *= std::max<std::size_t>(1, distinct(t, c));
            estimate = std::max<std::size_t>(1, t.rows / std::max<std::size_t>(1, combined));
            has_estimate = true;
            if (static_cast<double>(estimate) > static_cast<double>(t.rows) * kNotNarrowing && ranged.empty()) {
                note("An index on " + t.name + "(" + joined(counted) + ") would still leave about " + std::to_string(estimate) + " of " + std::to_string(t.rows) + " rows to read: not suggested.");
                return;
            }
        }
        if (!r_.seen.insert(t.key + '\x1f' + joined(columns)).second) return;
        std::string name = "idx_" + t.name;
        for (const auto& c : columns) name += "_" + c;
        r_.suggestions.push_back({t.name, joined(columns), why + (role_.empty() ? std::string() : " (in " + role_ + ")"), "CREATE INDEX " + name + " ON " + t.name + " (" + joined(columns) + ");", role_, t.rows, estimate, has_estimate, needs_rewrite});
    }

    struct OrBranches {
        Table* owner;
        std::vector<std::vector<std::string>> per_branch;
    };

    const SharedDatabase& s_;
    Reading& r_;
    std::string role_;
    std::vector<std::string> outer_;
    std::vector<Table> tables_;
    std::vector<OrBranches> or_branches_;
    std::vector<std::pair<const Statement*, std::string>> nested_;
};

} // namespace

StringResult Executor::exec_suggest_indexes(const std::string& sql) {
    // SUGGEST INDEX [ES] [FOR] <statement>
    std::size_t pos = sql.find_first_not_of(" \t\r\n");
    auto skip_word = [&](const char* word) {
        const std::size_t n = std::char_traits<char>::length(word);
        if (pos == std::string::npos || pos + n > sql.size()) return false;
        for (std::size_t i = 0; i < n; i++) {
            if (std::toupper(static_cast<unsigned char>(sql[pos + i])) != word[i]) return false;
        }
        if (pos + n < sql.size() && (std::isalnum(static_cast<unsigned char>(sql[pos + n])) || sql[pos + n] == '_')) return false;
        pos = sql.find_first_not_of(" \t\r\n", pos + n);
        return true;
    };
    skip_word("SUGGEST");
    if (!skip_word("INDEXES")) skip_word("INDEX");
    skip_word("FOR");
    std::string inner = pos == std::string::npos ? std::string() : sql.substr(pos);
    while (!inner.empty() && (inner.back() == ';' || std::isspace(static_cast<unsigned char>(inner.back())))) inner.pop_back(); // (the ; that goes with a statement in an editor)
    Parser parser(inner, pipes_as_concat());
    if (!parser.has_tokens()) return StringResult::Err("SUGGEST INDEX needs a SELECT, UPDATE or DELETE statement");
    auto parsed = parser.parse();
    if (parsed.is_err()) return StringResult::Err(parsed.error());
    Statement stmt = std::move(parsed).value();

    auto guard = shared->read();
    const SharedDatabase& s = *guard;
    resolve_names(s, stmt);
    Reading reading;
    const std::vector<std::string> answer_columns = {"kind", "table_name", "columns", "detail", "create_index", "rows_in_table", "rows_read_with_index", "block", "needs_rewrite"};
    // the rows of the answer: one for each suggestion, existing index and note
    auto answer = [&]() {
        std::vector<Row> rows;
        auto put = [&](const char* kind, const Suggestion& v, const std::string& detail, bool has_numbers) {
            Row r;
            r["kind"] = kind;
            r["table_name"] = v.table;
            r["columns"] = v.columns;
            r["detail"] = detail;
            r["create_index"] = v.create;
            r["rows_in_table"] = has_numbers ? std::to_string(v.rows) : std::string();
            r["rows_read_with_index"] = has_numbers && v.has_estimate ? std::to_string(v.estimate) : std::string();
            r["block"] = v.block;
            r["needs_rewrite"] = v.needs_rewrite ? "yes" : "";
            rows.push_back(std::move(r));
        };
        for (const auto& v : reading.suggestions) put("suggest", v, v.why, true);
        for (const auto& v : reading.already) put("already", v, v.why, true);
        for (const auto& n : reading.notes) {
            Suggestion none;
            put("note", none, n, false);
        }
        if (rows.empty()) {
            Suggestion none;
            put("note", none, "Nothing to suggest: the statement has no condition, join or sort that an index could serve.", false);
        }
        return StringResult::Ok(format_set_result(answer_columns, rows));
    };

    auto aliases_of = [](const std::vector<std::pair<std::string, std::string>>& pairs, const std::string& table) {
        std::vector<std::string> out;
        for (const auto& [alias, name] : pairs) {
            if (name == table) out.push_back(alias);
        }
        return out;
    };
    auto is_dual = [](const std::string& table) { return table == "_dual_" || (table.size() > 7 && table.compare(table.size() - 7, 7, "._dual_") == 0); }; // (SELECT 1: no table is read)

    // A statement is read as blocks: each block (the statement, a CTE, a derived table, a subquery) is read on its own, with its own tables; the blocks inside it follow.
    std::function<void(const Statement&, const std::string&, const std::vector<std::string>&)> read =
        [&](const Statement& statement, const std::string& role, const std::vector<std::string>& outer) {
            auto read_inside = [&](Advisor& advisor) {
                const std::vector<std::string> names = advisor.names();
                for (const auto& [inner, inner_role] : advisor.nested()) read(*inner, inner_role, names);
            };
            if (auto* sel = std::get_if<Statement::Select>(&statement.data)) {
                Advisor advisor(s, reading, role, outer);
                std::vector<std::pair<const Statement*, std::string>> derived; // the derived tables of the FROM list, read after the tables
                if (sel->subquery) {
                    derived.push_back({sel->subquery->first.get(), "the derived table " + sel->subquery->second});
                } else if (!is_dual(sel->table)) {
                    std::vector<std::string> base_aliases = aliases_of(sel->table_aliases, sel->table);
                    if (!sel->table_alias.empty()) base_aliases.push_back(sel->table_alias);
                    advisor.add_table(qualify_name(sel->table), base_aliases);
                }
                std::vector<std::size_t> placed; // the index in the tables of each join (npos: it is not a table)
                for (const Join& j : sel->joins) {
                    if (j.subquery) {
                        derived.push_back({j.subquery->first.get(), std::string(j.lateral ? "the LATERAL subquery " : "the derived table ") + j.subquery->second});
                        placed.push_back(std::string::npos);
                        continue;
                    }
                    std::vector<std::string> aliases = aliases_of(sel->table_aliases, j.table);
                    if (!j.alias.empty()) aliases.push_back(j.alias);
                    const bool added = advisor.add_table(qualify_name(j.table), aliases);
                    placed.push_back(added ? advisor.table_count() - 1 : std::string::npos);
                }
                for (std::size_t i = 0; i < sel->joins.size(); i++) {
                    if (placed[i] == std::string::npos) continue;
                    if (!sel->joins[i].using_cols.empty()) advisor.using_columns(sel->joins[i].using_cols, placed[i]);
                    else advisor.walk(sel->joins[i].on_expr);
                }
                if (sel->condition) advisor.walk(*sel->condition);
                advisor.conclude(sel->order_by, sel->group_by.value_or(std::vector<std::string>{}), sel->limit.has_value());
                for (const auto& [inner, inner_role] : derived) read(*inner, inner_role, outer);
                read_inside(advisor);
            } else if (auto* upd = std::get_if<Statement::Update>(&statement.data)) {
                Advisor advisor(s, reading, role, outer);
                advisor.add_table(qualify_name(upd->table), aliases_of(upd->table_aliases, upd->table));
                if (upd->condition) advisor.walk(*upd->condition);
                advisor.conclude(upd->order_by, {}, upd->limit.has_value());
                read_inside(advisor);
            } else if (auto* del = std::get_if<Statement::Delete>(&statement.data)) {
                Advisor advisor(s, reading, role, outer);
                advisor.add_table(qualify_name(del->table), aliases_of(del->table_aliases, del->table));
                if (del->condition) advisor.walk(*del->condition);
                advisor.conclude(del->order_by, {}, del->limit.has_value());
                read_inside(advisor);
            } else if (auto* with = std::get_if<Statement::With>(&statement.data)) {
                for (const auto& [name, body] : with->ctes) reading.cte_names.insert(lowered(name));
                for (const auto& [name, body] : with->ctes) {
                    if (body) read(*body, "the CTE " + name, outer);
                }
                if (with->query) read(*with->query, role, outer);
            } else if (auto* u = std::get_if<Statement::Union>(&statement.data)) {
                if (u->left) read(*u->left, role, outer);
                if (u->right) read(*u->right, role, outer);
            } else if (auto* in = std::get_if<Statement::Intersect>(&statement.data)) {
                if (in->left) read(*in->left, role, outer);
                if (in->right) read(*in->right, role, outer);
            } else if (auto* ex = std::get_if<Statement::Except>(&statement.data)) {
                if (ex->left) read(*ex->left, role, outer);
                if (ex->right) read(*ex->right, role, outer);
            }
        };

    const bool readable = std::holds_alternative<Statement::Select>(stmt.data) || std::holds_alternative<Statement::Update>(stmt.data) ||
                          std::holds_alternative<Statement::Delete>(stmt.data) || std::holds_alternative<Statement::With>(stmt.data) ||
                          std::holds_alternative<Statement::Union>(stmt.data) || std::holds_alternative<Statement::Intersect>(stmt.data) ||
                          std::holds_alternative<Statement::Except>(stmt.data);
    if (!readable) return StringResult::Err("SUGGEST INDEX reads a SELECT, UPDATE or DELETE statement");
    read(stmt, "", {});
    if (reading.tables_read == 0 && reading.notes.empty()) reading.note("The statement reads no table.");
    return answer();
}

} // namespace engine
