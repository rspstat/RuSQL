// Unknown columns. A column name that none of the statement's tables has used to be accepted: SELECT gave an empty column, WHERE found no
// row, ORDER BY sorted by nothing and a typo in a query looked like "no data". MySQL answers error 1054, "Unknown column 'x' in
// 'where clause'", and so does this check, once for each top-level statement before it runs.
//
// What is checked is what is certain to be a column: a plain `name`, `table.name` or `db.table.name` in the select list, ON, WHERE, GROUP BY,
// HAVING and ORDER BY of a SELECT (through subqueries, with the columns of the enclosing queries in sight), and in the WHERE and the SET
// expressions of an UPDATE / DELETE. What cannot be told from a literal (the right-hand side of a comparison, which the parser keeps
// without its quotes) or is not a column at all (function calls, a JSON path, `@variable`, a number) is not. A table the catalog does
// not know -- a view, a CTE, an information_schema table, a derived table whose columns cannot be named -- accepts any column name.

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include "engine/executor/executor.hpp"

namespace engine {

namespace {

struct BindTable {
    std::string full; // as the engine names it ("db.t"), or the alias of a derived table / a table used twice
    std::string bare; // after the last dot
    bool open = false; // any column name is accepted
    const TableSchema* schema = nullptr;
    std::vector<std::string> own_columns; // a derived table's
};

struct BindScope {
    std::vector<BindTable> tables;
    std::unordered_set<std::string> aliases; // the names the select list gives, which GROUP BY / HAVING / ORDER BY may use
    // an UPDATE / DELETE keeps the aliases of its tables in the SET expressions (the parser expands them in the WHERE only), so a
    // qualifier that names no table may still be one of them
    bool lenient_qualifier = false;
};

bool identifier_char(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

// A plain `name`, `table.name` or `db.table.name`; anything else (a call, a JSON path, `@variable`, `*`, a number) is not checked.
bool plain_reference(const std::string& name) {
    if (name.empty()) return false;
    bool segment_start = true;
    for (unsigned char c : name) {
        if (c == '.') {
            if (segment_start) return false;
            segment_start = true;
            continue;
        }
        if (!identifier_char(c)) return false;
        if (segment_start && std::isdigit(c)) return false;
        segment_start = false;
    }
    return !segment_start;
}

std::string last_part(const std::string& qualified) {
    auto cut = qualified.rfind('.');
    return cut == std::string::npos ? qualified : qualified.substr(cut + 1);
}

bool has_column(const BindTable& t, const std::string& column) {
    if (t.open) return true;
    if (t.schema) return std::any_of(t.schema->columns.begin(), t.schema->columns.end(), [&](const ColumnDef& c) { return c.name == column; });
    return std::find(t.own_columns.begin(), t.own_columns.end(), column) != t.own_columns.end();
}

} // namespace

std::optional<std::string> Executor::check_columns(SharedDatabase& s, const Statement& stmt) {
    struct Binder {
        Executor& ex;
        SharedDatabase& s;
        std::vector<const BindScope*> chain; // the scopes of the enclosing queries, outermost first, then this one
        std::optional<std::string> error;

        // `qualifier_is_table`: false for the text of a comparison's right-hand side, where `x.y` is a column only if x is a table of the query
        bool known(const std::string& name, bool qualifier_is_table = true) const {
            if (!plain_reference(name)) return true;
            const std::size_t dot = name.rfind('.');
            if (dot != std::string::npos) {
                const std::string qualifier = name.substr(0, dot), column = name.substr(dot + 1);
                for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                    for (const BindTable& t : (*it)->tables) {
                        if (t.full == qualifier || t.bare == qualifier) return has_column(t, column);
                    }
                }
                // no table of that name: in a subquery it may be the alias of a table of the query around it (the parser does not
                // expand an outer alias inside a subquery), in a write statement one of its own
                if (!qualifier_is_table || chain.size() > 1) return true;
                return std::any_of(chain.begin(), chain.end(), [](const BindScope* sc) { return sc->lenient_qualifier; });
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                if ((*it)->aliases.count(name)) return true;
                for (const BindTable& t : (*it)->tables) {
                    if (has_column(t, name)) return true;
                }
            }
            return false;
        }

        void name(const std::string& n, const char* clause) {
            if (!error && !known(n)) error = "Unknown column '" + n + "' in '" + clause + "'";
        }

        void arith(const ArithExpr& e, const char* clause) {
            if (error) return;
            if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) name(col->name, clause);
            else if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) { arith(*v->lhs, clause); arith(*v->rhs, clause); }
            else if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) { arith(*v->lhs, clause); arith(*v->rhs, clause); }
            else if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) { arith(*v->lhs, clause); arith(*v->rhs, clause); }
            else if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) { arith(*v->lhs, clause); arith(*v->rhs, clause); }
            else if (auto* v = std::get_if<ArithExpr::Cmp>(&e.data)) { arith(*v->lhs, clause); arith(*v->rhs, clause); }
            // (the arguments of a function are not looked at: they may name a unit or a type as well as a column)
        }

        void nested(const Statement& inner) {
            Binder sub{ex, s, chain, std::nullopt};
            sub.statement(inner);
            if (sub.error && !error) error = sub.error;
        }

        void cond(const CondExpr& e, const char* clause) {
            if (error) return;
            if (auto* a = std::get_if<CondExpr::And>(&e.data)) { cond(*a->lhs, clause); cond(*a->rhs, clause); }
            else if (auto* o = std::get_if<CondExpr::Or>(&e.data)) { cond(*o->lhs, clause); cond(*o->rhs, clause); }
            else if (auto* n = std::get_if<CondExpr::Not>(&e.data)) cond(*n->inner, clause);
            else if (auto* leaf = std::get_if<CondExpr::Leaf>(&e.data)) {
                arith(leaf->condition.left, clause);
                // `a.x = b.y`: the parser keeps the right side as text; a `table.column` of a table of the query is a column
                if (auto* lit = std::get_if<ConditionValue::Literal>(&leaf->condition.value.data)) {
                    if (!error && lit->value.find('.') != std::string::npos && !known(lit->value, false)) {
                        error = "Unknown column '" + lit->value + "' in '" + clause + "'";
                    }
                } else if (auto* value = std::get_if<ConditionValue::Arith>(&leaf->condition.value.data)) arith(value->expr, clause);
                else if (auto* sub = std::get_if<ConditionValue::Subquery>(&leaf->condition.value.data)) {
                    if (sub->query) nested(*sub->query);
                }
            }
        }

        BindTable table_of(const std::string& qualified_name, const std::string& as) {
            BindTable t;
            t.full = as.empty() ? qualified_name : as;
            t.bare = last_part(t.full);
            if (s.views.count(qualified_name)) t.open = true;
            else if (const TableSchema* schema = s.catalog.get_table(qualified_name)) t.schema = schema;
            else t.open = true; // a CTE, a temporary table, information_schema, ...
            return t;
        }

        BindTable derived_of(const Statement& inner, const std::string& alias) {
            BindTable t;
            t.full = t.bare = alias;
            t.own_columns = ex.derived_column_names(s, inner);
            t.open = t.own_columns.empty();
            return t;
        }

        void select_column(const SelectColumn& c) {
            if (error) return;
            if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) name(col->name, "field list");
            else if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) name(ca->name, "field list");
            else if (auto* agg = std::get_if<SelectColumn::Agg>(&c.data)) {
                name(agg->source.empty() ? agg->col : agg->source, "field list");
                if (agg->filter) cond(*agg->filter, "field list");
            } else if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) {
                name(aa->source.empty() ? aa->col : aa->source, "field list");
                if (aa->filter) cond(*aa->filter, "field list");
            } else if (auto* ex_col = std::get_if<SelectColumn::Expr>(&c.data)) arith(ex_col->expr, "field list");
            else if (auto* cw = std::get_if<SelectColumn::CaseWhen>(&c.data)) {
                for (auto& b : cw->branches) cond(b.condition, "field list");
            } else if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data)) {
                if (wf->col) name(*wf->col, "field list");
                for (auto& p : wf->partition_by) name(p, "field list");
                for (auto& o : wf->order_by) name(o.column, "field list");
            } else if (auto* sq = std::get_if<SelectColumn::Subquery>(&c.data)) {
                if (sq->query) nested(*sq->query);
            }
        }

        void select(const Statement::Select& sel) {
            BindScope scope;
            // the FROM list
            if (sel.subquery) {
                Binder sub{ex, s, {}, std::nullopt}; // a derived table sees none of the columns around it
                sub.statement(*sel.subquery->first);
                if (sub.error) { error = sub.error; return; }
                scope.tables.push_back(derived_of(*sel.subquery->first, sel.subquery->second));
            } else if (sel.table == "_dual_" || (sel.table.size() > 7 && sel.table.compare(sel.table.size() - 7, 7, "._dual_") == 0)) {
                // no table
            } else {
                scope.tables.push_back(table_of(sel.table, ""));
            }
            for (auto& j : sel.joins) {
                if (j.subquery && j.lateral) {
                    BindTable open;
                    open.full = open.bare = j.table;
                    open.open = true;
                    scope.tables.push_back(open);
                } else if (j.subquery) {
                    Binder sub{ex, s, {}, std::nullopt};
                    sub.statement(*j.subquery->first);
                    if (sub.error) { error = sub.error; return; }
                    scope.tables.push_back(derived_of(*j.subquery->first, j.subquery->second));
                } else {
                    scope.tables.push_back(table_of(j.table, j.alias));
                }
            }
            // the names the select list gives
            for (auto& c : sel.columns) {
                if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) scope.aliases.insert(ca->alias);
                else if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) scope.aliases.insert(aa->alias);
                else if (auto* fn = std::get_if<SelectColumn::Func>(&c.data); fn && fn->alias) scope.aliases.insert(*fn->alias);
                else if (auto* e = std::get_if<SelectColumn::Expr>(&c.data); e && e->alias) scope.aliases.insert(*e->alias);
                else if (auto* cw = std::get_if<SelectColumn::CaseWhen>(&c.data); cw && cw->alias) scope.aliases.insert(*cw->alias);
                else if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data); wf && wf->alias) scope.aliases.insert(*wf->alias);
                else if (auto* sq = std::get_if<SelectColumn::Subquery>(&c.data); sq && sq->alias) scope.aliases.insert(*sq->alias);
            }
            chain.push_back(&scope);
            for (auto& c : sel.columns) select_column(c);
            for (auto& j : sel.joins) cond(j.on_expr, "on clause");
            if (sel.condition) cond(*sel.condition, "where clause");
            if (sel.group_by) {
                for (auto& g : *sel.group_by) name(g, "group statement");
            }
            if (sel.having) cond(*sel.having, "having clause");
            for (auto& o : sel.order_by) name(o.column, "order clause");
            chain.pop_back();
        }

        // an UPDATE / DELETE: the table (and the joined ones) as the one scope of its WHERE and SET expressions
        void write(const std::vector<std::string>& tables, const std::vector<Join>& joins, const std::optional<CondExpr>& where,
                   const std::vector<std::pair<std::string, ArithExpr>>* assignments) {
            BindScope scope;
            for (auto& t : tables) scope.tables.push_back(table_of(t, ""));
            for (auto& j : joins) scope.tables.push_back(table_of(j.table, j.alias));
            scope.lenient_qualifier = true;
            chain.push_back(&scope);
            if (assignments) {
                for (auto& [target, value] : *assignments) {
                    if (tables.size() + joins.size() > 1) name(target, "field list"); // (a one-table UPDATE checks it as it runs)
                    arith(value, "field list");
                }
            }
            for (auto& j : joins) cond(j.on_expr, "on clause");
            if (where) cond(*where, "where clause");
            chain.pop_back();
        }

        void statement(const Statement& st) {
            if (error) return;
            if (auto* sel = std::get_if<Statement::Select>(&st.data)) select(*sel);
            else if (auto* u = std::get_if<Statement::Union>(&st.data)) { statement(*u->left); statement(*u->right); }
            else if (auto* i = std::get_if<Statement::Intersect>(&st.data)) { statement(*i->left); statement(*i->right); }
            else if (auto* x = std::get_if<Statement::Except>(&st.data)) { statement(*x->left); statement(*x->right); }
            else if (auto* w = std::get_if<Statement::With>(&st.data)) {
                for (auto& cte : w->ctes) statement(*cte.second);
                if (w->query) statement(*w->query);
            } else if (auto* up = std::get_if<Statement::Update>(&st.data)) write({up->table}, {}, up->condition, &up->assignments);
            else if (auto* del = std::get_if<Statement::Delete>(&st.data)) write({del->table}, {}, del->condition, nullptr);
            else if (auto* mu = std::get_if<Statement::MultiUpdate>(&st.data)) write(mu->tables, mu->joins, mu->condition, &mu->assignments);
            else if (auto* md = std::get_if<Statement::MultiDelete>(&st.data)) write({md->from_table}, md->joins, md->condition, nullptr);
            else if (auto* is = std::get_if<Statement::InsertSelect>(&st.data)) {
                if (is->query) statement(*is->query);
            } else if (auto* view = std::get_if<Statement::CreateView>(&st.data)) {
                if (view->query) statement(*view->query); // a view over a column that is not there is refused when it is made
            }
        }
    };

    Binder binder{*this, s, {}, std::nullopt};
    binder.statement(stmt);
    return binder.error;
}

} // namespace engine
