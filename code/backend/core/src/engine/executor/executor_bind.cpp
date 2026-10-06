// Binding: what the names of a statement refer to. It runs once for every statement before it is executed (execute_with_s, after the names are
// qualified) and does two things.
//
// Unknown columns. A column name that none of the statement's tables has used to be accepted: SELECT gave an empty column, WHERE found no
// row, ORDER BY sorted by nothing and a typo in a query looked like "no data". MySQL answers error 1054, "Unknown column 'x' in
// 'where clause'", and so does this check, for each top-level statement.
//
// What is checked is what is certain to be a column: a plain `name`, `table.name` or `db.table.name` in the select list, ON, WHERE, GROUP BY,
// HAVING and ORDER BY of a SELECT (through subqueries, with the columns of the enclosing queries in sight), and in the WHERE and the SET
// expressions of an UPDATE / DELETE. What cannot be told from a literal (the right-hand side of a comparison that is not quoted) or is not a
// column at all (function calls, a JSON path, `@variable`, a number) is not. A table the catalog does not know -- an information_schema
// table, a derived table whose columns cannot be named -- accepts any column name.
//
// Value classes. Values are stored as text, but MySQL compares by type: two strings as strings ('10' < '9', '007' and '7' differ), a number
// and anything else as numbers. So every column reference, every comparison, every ORDER BY column and every aggregate argument gets the
// class its declared type (or its quotes, or its operators) gives it -- the evaluators read it (see value_class.hpp). A column the binder
// cannot place (a function result of no fixed type, a derived column of a mixed expression) stays Unknown and compares the old way.

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

#include "engine/column_text.hpp"
#include "engine/executor/executor.hpp"
#include "engine/parser/parser.hpp"

namespace engine {

namespace {

struct BindTable {
    std::string full; // as the engine names it ("db.t"), or the alias of a derived table / a table used twice
    std::string bare; // after the last dot
    bool open = false; // any column name is accepted
    const TableSchema* schema = nullptr;
    std::vector<std::string> own_columns; // a derived table's (a view's, a CTE's)
    std::vector<ValueClass> own_classes;  // what each of them holds
};

struct BindScope {
    std::vector<BindTable> tables;
    std::unordered_set<std::string> aliases; // the names the select list gives, which GROUP BY / HAVING / ORDER BY may use
    std::unordered_map<std::string, ValueClass> alias_classes;
    // an UPDATE / DELETE keeps the aliases of its tables in the SET expressions (the parser expands them in the WHERE only), so a
    // qualifier that names no table may still be one of them
    bool lenient_qualifier = false;
};

std::string last_part(const std::string& qualified) {
    auto cut = qualified.rfind('.');
    return cut == std::string::npos ? qualified : qualified.substr(cut + 1);
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool has_column(const BindTable& t, const std::string& column) {
    if (t.open) return true;
    if (t.schema) return std::any_of(t.schema->columns.begin(), t.schema->columns.end(), [&](const ColumnDef& c) { return c.name == column; });
    return std::find(t.own_columns.begin(), t.own_columns.end(), column) != t.own_columns.end();
}

ValueClass column_class(const BindTable& t, const std::string& column) {
    if (t.schema) {
        for (auto& c : t.schema->columns) {
            if (c.name == column) return class_of_type(c.data_type);
        }
        return ValueClass::Unknown;
    }
    for (std::size_t i = 0; i < t.own_columns.size() && i < t.own_classes.size(); i++) {
        if (t.own_columns[i] == column) return t.own_classes[i];
    }
    return ValueClass::Unknown;
}

ValueClass aggregate_function_class(const AggFunc& f) {
    if (std::holds_alternative<AggFunc::GroupConcat>(f.data) || std::holds_alternative<AggFunc::JsonAgg>(f.data) ||
        std::holds_alternative<AggFunc::ArrayAgg>(f.data)) {
        return ValueClass::Text;
    }
    if (std::holds_alternative<AggFunc::Min>(f.data) || std::holds_alternative<AggFunc::Max>(f.data)) return ValueClass::Unknown; // the argument's
    return ValueClass::Number;
}

// What a window function gives: the rank / count / sum kinds are numbers, the others (FIRST_VALUE, LAG, MIN ...) the class of their column.
ValueClass window_function_class(const SelectColumn::WinFunc& w) {
    switch (w.func) {
        case WindowFunc::RowNumber: case WindowFunc::Rank: case WindowFunc::DenseRank: case WindowFunc::Ntile: case WindowFunc::PercentRank:
        case WindowFunc::CumeDist: case WindowFunc::Sum: case WindowFunc::Avg: case WindowFunc::Count:
            return ValueClass::Number;
        default:
            return w.col_class;
    }
}

} // namespace

std::optional<std::string> Executor::bind_statement(SharedDatabase& s, Statement& stmt, bool check) {
    struct Binder {
        Executor& ex;
        SharedDatabase& s;
        bool check;
        std::vector<const BindScope*> chain; // the scopes of the enclosing queries, outermost first, then this one
        std::optional<std::string> error;
        std::unordered_map<std::string, BindTable> ctes;
        std::vector<std::pair<std::string, ValueClass>> outputs; // what the last query bound gives, column by column

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

        // What `AGG(arg)` -- the text a HAVING or a select-list expression uses for an aggregate -- gives; nullopt when `name` is not one.
        std::optional<ValueClass> aggregate_reference_class(const std::string& name) const {
            const std::size_t lp = name.find('('), rp = name.rfind(')');
            if (lp == std::string::npos || rp == std::string::npos || rp != name.size() - 1 || lp == 0) return std::nullopt;
            const std::string function = upper(name.substr(0, lp));
            if (function == "MIN" || function == "MAX") {
                std::string arg = name.substr(lp + 1, rp - lp - 1);
                if (arg.rfind("DISTINCT ", 0) == 0) arg.erase(0, 9);
                return class_of_name(arg);
            }
            if (function == "GROUP_CONCAT" || function == "JSON_AGG" || function == "ARRAY_AGG") return ValueClass::Text;
            if (function == "COUNT" || function == "SUM" || function == "AVG" || function == "STDDEV" || function == "VARIANCE" ||
                function == "MEDIAN" || function == "BIT_AND" || function == "BIT_OR") {
                return ValueClass::Number;
            }
            return std::nullopt;
        }

        // What the column `name` (`name`, `t.name`, `db.t.name`), a name the select list gives, or an aggregate reference holds.
        ValueClass class_of_name(const std::string& name) const {
            if (auto agg = aggregate_reference_class(name)) return *agg;
            if (!plain_reference(name)) return ValueClass::Unknown;
            const std::size_t dot = name.rfind('.');
            if (dot != std::string::npos) {
                const std::string qualifier = name.substr(0, dot), column = name.substr(dot + 1);
                for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                    for (const BindTable& t : (*it)->tables) {
                        if (t.full == qualifier || t.bare == qualifier) return column_class(t, column);
                    }
                }
                return ValueClass::Unknown;
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                if (auto alias = (*it)->alias_classes.find(name); alias != (*it)->alias_classes.end()) return alias->second;
                for (const BindTable& t : (*it)->tables) {
                    if (!t.open && has_column(t, name)) return column_class(t, name);
                }
            }
            return ValueClass::Unknown;
        }

        void name(const std::string& n, const char* clause) {
            if (check && !error && !known(n)) error = "Unknown column '" + n + "' in '" + clause + "'";
        }

        void arith(ArithExpr& e, const char* clause) {
            if (error) return;
            if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) {
                name(col->name, clause);
                col->cls = class_of_name(col->name);
            }
            else if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) { arith(*v->lhs, clause); arith(*v->rhs, clause); }
            else if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) { arith(*v->lhs, clause); arith(*v->rhs, clause); }
            else if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) { arith(*v->lhs, clause); arith(*v->rhs, clause); }
            else if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) { arith(*v->lhs, clause); arith(*v->rhs, clause); }
            else if (auto* v = std::get_if<ArithExpr::Cmp>(&e.data)) { arith(*v->lhs, clause); arith(*v->rhs, clause); }
            else if (auto* v = std::get_if<ArithExpr::Pred>(&e.data)) cond(*v->cond, clause);
            else if (auto* v = std::get_if<ArithExpr::Func>(&e.data); v && v->name == "CASE") {
                for (auto& a : v->args) arith(a, clause); // (its conditions and results are expressions)
            } else if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) {
                // the arguments of any other function are not checked (they may name a unit or a type as well as a column), but a column in them
                // still holds what its type says: IFNULL(y, w) over two number columns is a number
                for (auto& a : f->args) annotate(a);
            }
        }

        // what the columns in `e` hold, without asking for them to exist
        void annotate(ArithExpr& e) {
            if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) col->cls = class_of_name(col->name);
            else if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) { annotate(*v->lhs); annotate(*v->rhs); }
            else if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) { annotate(*v->lhs); annotate(*v->rhs); }
            else if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) { annotate(*v->lhs); annotate(*v->rhs); }
            else if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) { annotate(*v->lhs); annotate(*v->rhs); }
            else if (auto* v = std::get_if<ArithExpr::Func>(&e.data)) { for (auto& a : v->args) annotate(a); }
        }

        // Binds a statement nested in this one (a subquery, a derived table): the columns of this query are in sight of it. Its output
        // columns are returned.
        std::vector<std::pair<std::string, ValueClass>> nested(Statement& inner, bool sees_outer = true) {
            Binder sub{ex, s, check, sees_outer ? chain : std::vector<const BindScope*>{}, std::nullopt, ctes, {}};
            sub.statement(inner);
            if (sub.error && !error) error = sub.error;
            return std::move(sub.outputs);
        }

        void cond(CondExpr& e, const char* clause) {
            if (error) return;
            if (auto* a = std::get_if<CondExpr::And>(&e.data)) { cond(*a->lhs, clause); cond(*a->rhs, clause); }
            else if (auto* o = std::get_if<CondExpr::Or>(&e.data)) { cond(*o->lhs, clause); cond(*o->rhs, clause); }
            else if (auto* n = std::get_if<CondExpr::Not>(&e.data)) cond(*n->inner, clause);
            else if (auto* leaf = std::get_if<CondExpr::Leaf>(&e.data)) {
                Condition& c = leaf->condition;
                arith(c.left, clause);
                c.left_class = class_of_expr(c.left);
                c.right_class = ValueClass::Unknown;
                // `a.x = b.y`: the parser keeps the right side as text; a `table.column` of a table of the query is a column
                if (auto* lit = std::get_if<ConditionValue::Literal>(&c.value.data)) {
                    if (lit->quoted) {
                        c.right_class = ValueClass::Text;
                    } else if (parse_number(lit->value)) {
                        c.right_class = ValueClass::Number;
                    } else {
                        if (check && !error && lit->value.find('.') != std::string::npos && !known(lit->value, false)) {
                            error = "Unknown column '" + lit->value + "' in '" + clause + "'";
                        }
                        c.right_class = class_of_name(lit->value);
                    }
                } else if (auto* value = std::get_if<ConditionValue::Arith>(&c.value.data)) {
                    arith(value->expr, clause);
                    c.right_class = class_of_expr(value->expr);
                } else if (auto* sub = std::get_if<ConditionValue::Subquery>(&c.value.data)) {
                    if (sub->query) {
                        auto out = nested(*sub->query);
                        // a subquery compared with a value or listed after IN gives one column (EXISTS does not care); MySQL 1241
                        if (check && !error && out.size() > 1 && c.op != Operator::Exists && c.op != Operator::NotExists) error = "Operand should contain 1 column(s)";
                        if (!out.empty()) c.right_class = out.front().second;
                    }
                }
            }
        }

        BindTable table_of(const std::string& qualified_name, const std::string& as) {
            BindTable t;
            t.full = as.empty() ? qualified_name : as;
            t.bare = last_part(t.full);
            if (auto cte = ctes.find(qualified_name); cte != ctes.end()) {
                t.own_columns = cte->second.own_columns;
                t.own_classes = cte->second.own_classes;
                t.open = t.own_columns.empty();
            } else if (auto view = s.views.find(qualified_name); view != s.views.end()) {
                Statement body = view->second; // bound as a copy: the stored view stays as it is (and is not checked again here)
                Binder sub{ex, s, false, {}, std::nullopt, ctes, {}, {}};
                sub.statement(body);
                t.own_columns = ex.derived_column_names(s, body);
                for (auto& o : sub.outputs) t.own_classes.push_back(o.second);
                t.open = t.own_columns.empty() || t.own_columns.size() != t.own_classes.size();
            } else if (const TableSchema* schema = s.catalog.get_table(qualified_name)) {
                t.schema = schema;
            } else {
                t.open = true; // a temporary table, information_schema, ...
            }
            return t;
        }

        BindTable derived_of(const Statement& inner, const std::string& alias, const std::vector<std::pair<std::string, ValueClass>>& out) {
            BindTable t;
            t.full = t.bare = alias;
            t.own_columns = ex.derived_column_names(s, inner);
            for (auto& o : out) t.own_classes.push_back(o.second);
            t.open = t.own_columns.empty() || t.own_columns.size() != t.own_classes.size();
            return t;
        }

        // The argument of an aggregate that is an expression (`price * qty`): the columns in it have to exist; what it holds is what its operators say
        // (the executor reads the text again to compute it, so the expression bound here is a copy)
        ValueClass expression_argument(const std::string& text, const char* clause) {
            ArithExpr e = Parser::str_to_arith(text);
            if (auto* col = std::get_if<ArithExpr::Col>(&e.data); col && col->name == text) return ValueClass::Unknown; // (not readable: the executor says so)
            arith(e, clause);
            return class_of_expr(e);
        }

        // The argument of an aggregate: a column, `*`, an expression, or for SUM / COUNT of a CASE (or of a condition: SUM(v > 1)) the conditions of
        // the CASE -- the parser leaves the placeholder "__case__" in the column then.
        ValueClass aggregate(AggFunc& func, const std::string& column, std::optional<CondExpr>& filter) {
            std::vector<CaseWhenBranch>* branches = nullptr;
            if (auto* counted = std::get_if<AggFunc::CountCase>(&func.data)) branches = &counted->branches;
            else if (auto* summed = std::get_if<AggFunc::SumCase>(&func.data)) branches = &summed->branches;
            ValueClass arg = ValueClass::Unknown;
            if (branches) {
                for (auto& b : *branches) cond(b.condition, "field list");
            } else if (is_expression_argument(column)) {
                arg = expression_argument(column, "field list");
            } else {
                name(column, "field list");
                arg = class_of_name(column);
            }
            if (filter) cond(*filter, "field list");
            return arg;
        }

        // What the select item gives (after it has been bound).
        ValueClass class_of_item(SelectColumn& c) {
            if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) return col->cls;
            if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) return ca->cls;
            if (auto* agg = std::get_if<SelectColumn::Agg>(&c.data)) {
                ValueClass k = aggregate_function_class(agg->func);
                return k == ValueClass::Unknown ? agg->arg_class : k;
            }
            if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) {
                ValueClass k = aggregate_function_class(aa->func);
                return k == ValueClass::Unknown ? aa->arg_class : k;
            }
            if (auto* fn = std::get_if<SelectColumn::Func>(&c.data)) return function_result_class(fn->name);
            if (auto* e = std::get_if<SelectColumn::Expr>(&c.data)) return class_of_expr(e->expr);
            if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data)) return window_function_class(*wf);
            return ValueClass::Unknown; // a CASE, a subquery
        }

        void select_column(SelectColumn& c) {
            if (error) return;
            if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) { name(col->name, "field list"); col->cls = class_of_name(col->name); }
            else if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) { name(ca->name, "field list"); ca->cls = class_of_name(ca->name); }
            else if (auto* agg = std::get_if<SelectColumn::Agg>(&c.data)) agg->arg_class = aggregate(agg->func, agg->source.empty() ? agg->col : agg->source, agg->filter);
            else if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) aa->arg_class = aggregate(aa->func, aa->source.empty() ? aa->col : aa->source, aa->filter);
            else if (auto* ex_col = std::get_if<SelectColumn::Expr>(&c.data)) arith(ex_col->expr, "field list");
            else if (auto* cw = std::get_if<SelectColumn::CaseWhen>(&c.data)) {
                for (auto& b : cw->branches) cond(b.condition, "field list");
            } else if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data)) {
                if (wf->col) {
                    if (is_expression_argument(*wf->col)) {
                        wf->col_class = expression_argument(*wf->col, "field list");
                    } else {
                        name(*wf->col, "field list");
                        wf->col_class = class_of_name(*wf->col);
                    }
                }
                for (auto& p : wf->partition_by) name(p, "field list");
                for (auto& o : wf->order_by) { name(o.column, "field list"); o.cls = class_of_name(o.column); }
            } else if (auto* sq = std::get_if<SelectColumn::Subquery>(&c.data)) {
                if (sq->query) {
                    auto out = nested(*sq->query);
                    if (check && !error && out.size() > 1) error = "Operand should contain 1 column(s)"; // a scalar subquery is one column (MySQL 1241)
                }
            }
        }

        // The columns `select * from <the scope>` gives, in the order derived_column_names lists them.
        void star_columns(const BindScope& scope, const std::string& only_table, std::vector<std::pair<std::string, ValueClass>>& out) const {
            for (const BindTable& t : scope.tables) {
                if (!only_table.empty() && t.full != only_table && t.bare != only_table) continue;
                if (t.schema) {
                    for (auto& c : t.schema->columns) out.emplace_back(c.name, class_of_type(c.data_type));
                } else {
                    for (std::size_t i = 0; i < t.own_columns.size(); i++) out.emplace_back(t.own_columns[i], i < t.own_classes.size() ? t.own_classes[i] : ValueClass::Unknown);
                }
            }
        }

        void select(Statement::Select& sel) {
            BindScope scope;
            // the FROM list
            if (sel.subquery) {
                auto out = nested(*sel.subquery->first, false); // a derived table sees none of the columns around it
                if (error) return;
                scope.tables.push_back(derived_of(*sel.subquery->first, sel.subquery->second, out));
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
                    auto out = nested(*j.subquery->first, false);
                    if (error) return;
                    scope.tables.push_back(derived_of(*j.subquery->first, j.subquery->second, out));
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
            std::vector<std::pair<std::string, ValueClass>> out;
            std::vector<std::string> positions; // what each output column is called as an ORDER BY / GROUP BY item ("" when it cannot be one)
            std::unordered_map<std::string, std::string> given; // the names the select list gives, and what each stands for
            for (auto& c : sel.columns) {
                select_column(c);
                if (error) break;
                const ValueClass k = class_of_item(c);
                if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) scope.alias_classes[ca->alias] = k;
                else if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) scope.alias_classes[aa->alias] = k;
                else if (auto* fn = std::get_if<SelectColumn::Func>(&c.data); fn && fn->alias) scope.alias_classes[*fn->alias] = k;
                else if (auto* e = std::get_if<SelectColumn::Expr>(&c.data); e && e->alias) scope.alias_classes[*e->alias] = k;
                else if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data); wf && wf->alias) scope.alias_classes[*wf->alias] = k;
                if (auto* all = std::get_if<SelectColumn::All>(&c.data)) {
                    const std::size_t before = out.size();
                    star_columns(scope, all->table, out);
                    for (std::size_t i = before; i < out.size(); i++) positions.push_back(out[i].first);
                } else {
                    out.emplace_back(std::string(), k);
                    positions.push_back(sort_key(c));
                    if (const std::string alias = alias_of(c); !alias.empty() && !positions.back().empty()) given.emplace(alias, positions.back());
                }
            }
            for (auto& j : sel.joins) cond(j.on_expr, "on clause");
            if (sel.condition) cond(*sel.condition, "where clause");
            if (!sel.sort_resolved && !error) {
                for (auto& o : sel.order_by) resolve_sort_item(o.column, "order clause", positions, given);
                if (sel.group_by) {
                    for (auto& g : *sel.group_by) resolve_sort_item(g, "group statement", positions, given);
                }
                sel.sort_resolved = true;
            }
            if (sel.group_by) {
                for (auto& g : *sel.group_by) {
                    if (plain_reference(g)) name(g, "group statement");
                    else expression_argument(g, "group statement");
                }
            }
            if (sel.having) cond(*sel.having, "having clause");
            for (auto& o : sel.order_by) {
                if (plain_reference(o.column)) {
                    name(o.column, "order clause");
                    o.cls = class_of_name(o.column);
                } else {
                    o.cls = expression_argument(o.column, "order clause");
                }
            }
            chain.pop_back();
            outputs = std::move(out);
        }

        // The name an ORDER BY / GROUP BY item uses for a select-list column: a column by its name, an aggregate by its label, an expression by
        // its text; "" for what cannot be sorted by (a legacy function column, a subquery).
        std::string sort_key(SelectColumn& c) const {
            if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) return col->name;
            if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) return ca->name;
            if (auto* agg = std::get_if<SelectColumn::Agg>(&c.data)) return Executor::agg_label(agg->func, agg->col);
            if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) return aa->alias;
            if (auto* e = std::get_if<SelectColumn::Expr>(&c.data)) {
                try {
                    const std::string printed = Parser::arith_to_string(e->expr);
                    // (a number alone would read as a position: the parentheses make it the constant it is)
                    return printed.find_first_not_of("0123456789") == std::string::npos ? "(" + printed + ")" : printed;
                } catch (const ParseError&) {
                    return std::string();
                }
            }
            if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data)) return wf->alias.value_or(Executor::window_func_default_label(wf->func));
            return std::string();
        }

        static std::string alias_of(const SelectColumn& c) {
            if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) return ca->alias;
            if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) return aa->alias;
            if (auto* e = std::get_if<SelectColumn::Expr>(&c.data)) return e->alias.value_or(std::string());
            if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data)) return wf->alias.value_or(std::string());
            return std::string();
        }

        // `ORDER BY 2` is the second column of the select list, `ORDER BY total` the column the select list calls total (a name the select list
        // gives wins over a column of the same name, as in MySQL): either becomes what it stands for, the name of a column or the text of an
        // expression, which is all the executor has to know how to sort / group by.
        void resolve_sort_item(std::string& item, const char* clause, const std::vector<std::string>& positions,
                               const std::unordered_map<std::string, std::string>& given) {
            if (item.empty()) return;
            if (item.find_first_not_of("0123456789") == std::string::npos) {
                const std::size_t n = item.size() > 9 ? 0 : static_cast<std::size_t>(std::stoul(item));
                if (n < 1 || n > positions.size()) {
                    if (check && !error) error = "Unknown column '" + item + "' in '" + clause + "'";
                    return;
                }
                if (positions[n - 1].empty()) {
                    if (check && !error) error = std::string("Cannot use column ") + item + " of the select list in '" + clause + "'";
                    return;
                }
                item = positions[n - 1];
                return;
            }
            if (item.find('.') == std::string::npos) {
                if (auto it = given.find(item); it != given.end()) item = it->second;
            }
        }

        // an UPDATE / DELETE: the table (and the joined ones) as the one scope of its WHERE and SET expressions
        void write(const std::vector<std::string>& tables, std::vector<Join>& joins, std::optional<CondExpr>& where,
                   std::vector<std::pair<std::string, ArithExpr>>* assignments) {
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

        void statement(Statement& st) {
            if (error) return;
            outputs.clear();
            if (auto* sel = std::get_if<Statement::Select>(&st.data)) select(*sel);
            else if (auto* u = std::get_if<Statement::Union>(&st.data)) {
                statement(*u->left);
                auto first = std::move(outputs); // (the columns of a UNION are named and typed by its first query)
                statement(*u->right);
                outputs = std::move(first);
            } else if (auto* i = std::get_if<Statement::Intersect>(&st.data)) {
                statement(*i->left);
                auto first = std::move(outputs);
                statement(*i->right);
                outputs = std::move(first);
            } else if (auto* x = std::get_if<Statement::Except>(&st.data)) {
                statement(*x->left);
                auto first = std::move(outputs);
                statement(*x->right);
                outputs = std::move(first);
            } else if (auto* w = std::get_if<Statement::With>(&st.data)) {
                for (auto& cte : w->ctes) {
                    statement(*cte.second);
                    if (error) return;
                    BindTable t;
                    t.full = t.bare = cte.first;
                    t.own_columns = ex.derived_column_names(s, *cte.second);
                    for (auto& o : outputs) t.own_classes.push_back(o.second);
                    t.open = t.own_columns.empty() || t.own_columns.size() != t.own_classes.size();
                    ctes[cte.first] = std::move(t);
                }
                if (w->query) statement(*w->query);
            } else if (auto* up = std::get_if<Statement::Update>(&st.data)) write({up->table}, no_joins, up->condition, &up->assignments);
            else if (auto* del = std::get_if<Statement::Delete>(&st.data)) write({del->table}, no_joins, del->condition, nullptr);
            else if (auto* mu = std::get_if<Statement::MultiUpdate>(&st.data)) write(mu->tables, mu->joins, mu->condition, &mu->assignments);
            else if (auto* md = std::get_if<Statement::MultiDelete>(&st.data)) write({md->from_table}, md->joins, md->condition, nullptr);
            else if (auto* is = std::get_if<Statement::InsertSelect>(&st.data)) {
                if (is->query) statement(*is->query);
            } else if (auto* view = std::get_if<Statement::CreateView>(&st.data)) {
                if (view->query) statement(*view->query); // a view over a column that is not there is refused when it is made
            }
        }

        std::vector<Join> no_joins;
    };

    Binder binder{*this, s, check, {}, std::nullopt, {}, {}, {}};
    binder.statement(stmt);
    return binder.error;
}

ArithExpr Executor::parse_bound_expression(SharedDatabase& s, const std::string& text, const std::string& table, const std::vector<Join>& joins) {
    ArithExpr expr = Parser::str_to_arith(text);
    // (bound as the one select item of a query over the same tables)
    Statement::Select sel;
    sel.table = table;
    sel.joins = joins;
    sel.columns.push_back(SelectColumn(SelectColumn::Expr{std::move(expr), std::nullopt}));
    Statement stmt(std::move(sel));
    bind_statement(s, stmt, false);
    return std::move(std::get<SelectColumn::Expr>(std::get<Statement::Select>(stmt.data).columns[0].data).expr);
}

} // namespace engine
