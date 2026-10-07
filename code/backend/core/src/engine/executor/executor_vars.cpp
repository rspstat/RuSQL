// Variables in statements. A stored procedure's parameters and DECLAREd variables, a session's @variables and a trigger's NEW.x / OLD.x used
// to be known only to the procedural statements (IF, WHILE, SET) and to a SELECT without FROM: `UPDATE t SET v = 1 WHERE id = p_id` looked for a
// column named p_id, found none and updated nothing ("0 row(s) updated"), `WHERE v > @x` found no row, `INSERT ... VALUES (@x, 1)` did not
// parse. Before a statement runs, every place that names a variable is replaced by its value -- a column of the same name loses to the variable,
// as in MySQL. The text of a condition's right side, an INSERT value or a function argument is kept without its quotes by the parser, so there
// a string that equals a variable's name is read as the variable.

#include <algorithm>
#include <cctype>

#include "engine/column_text.hpp"
#include "engine/executor/executor.hpp"
#include "engine/parser/ast_json.hpp"
#include "engine/parser/parser.hpp"
#include "engine/subquery_walk.hpp"

namespace engine {

namespace {

struct Vars {
    const std::unordered_map<std::string, std::string>& proc;
    const std::unordered_map<std::string, std::string>& user;
    const std::unordered_map<std::string, std::string>* row; // `NEW.id` / `OLD.id` of a trigger's row, or null

    std::optional<std::string> value(const std::string& name) const {
        if (name.empty()) return std::nullopt;
        if (name[0] == '@') {
            // (text that merely starts with @ -- `@n + 1`, the argument of a function -- is not a variable's name)
            if (name.size() < 2 || !std::all_of(name.begin() + 1, name.end(), [](unsigned char c) { return std::isalnum(c) || c == '_'; })) return std::nullopt;
            auto it = user.find(name.substr(1));
            return it != user.end() ? it->second : std::string(EXECUTOR_NULL_VALUE); // an unset @variable is NULL
        }
        if (row) {
            if (auto it = row->find(name); it != row->end()) return it->second; // (the parser writes NEW.x / OLD.x in capitals)
        }
        if (auto it = proc.find(name); it != proc.end()) return it->second;
        return std::nullopt;
    }
};

bool plain_number(const std::string& v) {
    std::size_t i = 0;
    if (i < v.size() && (v[i] == '-' || v[i] == '+')) i++;
    std::size_t digits = 0;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) i++, digits++;
    if (i < v.size() && v[i] == '.') {
        i++;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) i++, digits++;
    }
    return digits > 0 && i == v.size();
}

// the text of a literal, an INSERT value or a function argument: the variable's value when it names one
std::string text_of(const Vars& v, const std::string& s) {
    if (auto value = v.value(s)) return *value;
    return s;
}

// A function argument is text (`@n / 2`, `LENGTH(s)`) in which a quoted word is a string and a bare one a column: a variable named in it is
// replaced by its value, written the way its kind needs (a string in quotes).
std::optional<std::string> argument_value(const Vars& v, const std::string& name) {
    auto value = v.value(name);
    if (!value) return std::nullopt;
    if (*value == EXECUTOR_NULL_VALUE || plain_number(*value)) return *value;
    return "'" + *value + "'"; // (the way the parser writes a string argument: its quotes are not escaped)
}

std::string argument_of(const Vars& v, const std::string& s) {
    if (auto whole = argument_value(v, s)) return *whole;
    auto word_char = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
    std::string out;
    bool changed = false;
    for (std::size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\'') { // a quoted string is copied as it is ('' is an escaped quote)
            std::size_t j = i + 1;
            while (j < s.size()) {
                if (s[j] == '\'') {
                    if (j + 1 < s.size() && s[j + 1] == '\'') { j += 2; continue; }
                    break;
                }
                j++;
            }
            out.append(s, i, j + 1 - i);
            i = j + 1;
            continue;
        }
        if (c == '@' || std::isalpha(c) || c == '_') {
            std::size_t j = i + 1;
            while (j < s.size() && word_char(static_cast<unsigned char>(s[j]))) j++;
            while (j + 1 < s.size() && s[j] == '.' && word_char(static_cast<unsigned char>(s[j + 1]))) { // `t.col`, `NEW.x`: one name
                j++;
                while (j < s.size() && word_char(static_cast<unsigned char>(s[j]))) j++;
            }
            const std::string word = s.substr(i, j - i);
            if (auto value = argument_value(v, word)) {
                out += *value;
                changed = true;
            } else {
                out += word;
            }
            i = j;
            continue;
        }
        out += s[i++];
    }
    return changed ? out : s;
}

void statement(const Vars& v, Statement& st);
void cond(const Vars& v, CondExpr& e);

void arith(const Vars& v, ArithExpr& e) {
    if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) {
        if (auto value = v.value(col->name)) e = Executor::value_constant(*value);
    } else if (auto* a = std::get_if<ArithExpr::Add>(&e.data)) { arith(v, *a->lhs); arith(v, *a->rhs); }
    else if (auto* s = std::get_if<ArithExpr::Sub>(&e.data)) { arith(v, *s->lhs); arith(v, *s->rhs); }
    else if (auto* m = std::get_if<ArithExpr::Mul>(&e.data)) { arith(v, *m->lhs); arith(v, *m->rhs); }
    else if (auto* d = std::get_if<ArithExpr::Div>(&e.data)) { arith(v, *d->lhs); arith(v, *d->rhs); }
    else if (auto* c = std::get_if<ArithExpr::Cmp>(&e.data)) { arith(v, *c->lhs); arith(v, *c->rhs); }
    else if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) {
        for (auto& arg : f->args) arith(v, arg);
    } else if (auto* p = std::get_if<ArithExpr::Pred>(&e.data)) {
        cond(v, *p->cond);
    } else if (auto* sq = std::get_if<ArithExpr::Subquery>(&e.data)) {
        if (sq->query) statement(v, *sq->query);
        sq->correlated = -1; // (its variables may have a new value)
        sq->answered_in = 0;
    }
}

// `SUM(price * @rate)`: a variable in the expression an aggregate takes as its argument is replaced by its value. `source` is what the rows
// hold; the result column keeps the name as typed (`col`).
void aggregate_argument(const Vars& v, const std::string& col, std::string& source) {
    const std::string text = source.empty() ? col : source;
    if (!is_expression_argument(text)) return;
    ArithExpr e = Parser::str_to_arith(text);
    if (auto* c = std::get_if<ArithExpr::Col>(&e.data); c && c->name == text) return; // (not readable: the executor says so)
    arith(v, e);
    const std::string replaced = Parser::aggregate_argument_text(e);
    if (replaced != text) source = replaced;
}

void cond(const Vars& v, CondExpr& e) {
    if (auto* a = std::get_if<CondExpr::And>(&e.data)) { cond(v, *a->lhs); cond(v, *a->rhs); }
    else if (auto* o = std::get_if<CondExpr::Or>(&e.data)) { cond(v, *o->lhs); cond(v, *o->rhs); }
    else if (auto* n = std::get_if<CondExpr::Not>(&e.data)) cond(v, *n->inner);
    else if (auto* leaf = std::get_if<CondExpr::Leaf>(&e.data)) {
        Condition& c = leaf->condition;
        arith(v, c.left);
        // (what a variable holds is a string unless it is a number: not the name of a column)
        if (auto* lit = std::get_if<ConditionValue::Literal>(&c.value.data)) {
            if (auto value = v.value(lit->value)) {
                lit->value = *value == EXECUTOR_NULL_VALUE ? "__NULL__" : *value;
                lit->quoted = *value != EXECUTOR_NULL_VALUE && !parse_number(*value);
            }
        } else if (auto* list = std::get_if<ConditionValue::LiteralList>(&c.value.data)) {
            list->quoted.resize(list->values.size(), false);
            for (std::size_t i = 0; i < list->values.size(); i++) {
                std::string replaced = text_of(v, list->values[i]);
                if (replaced == list->values[i]) continue;
                list->quoted[i] = replaced != EXECUTOR_NULL_VALUE && !parse_number(replaced);
                list->values[i] = std::move(replaced);
            }
        } else if (auto* between = std::get_if<ConditionValue::Between>(&c.value.data)) {
            std::string lo = text_of(v, between->lo), hi = text_of(v, between->hi);
            if (lo != between->lo) between->lo_quoted = !parse_number(lo);
            if (hi != between->hi) between->hi_quoted = !parse_number(hi);
            between->lo = std::move(lo);
            between->hi = std::move(hi);
        } else if (auto* value = std::get_if<ConditionValue::Arith>(&c.value.data)) {
            arith(v, value->expr);
        } else if (auto* sub = std::get_if<ConditionValue::Subquery>(&c.value.data)) {
            if (sub->query) statement(v, *sub->query);
        }
    }
}

void select_columns(const Vars& v, std::vector<SelectColumn>& columns) {
    for (auto& c : columns) {
        if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) {
            if (auto value = v.value(col->name)) c = SelectColumn(SelectColumn::Expr{Executor::value_constant(*value), col->name});
        } else if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) {
            if (auto value = v.value(ca->name)) c = SelectColumn(SelectColumn::Expr{Executor::value_constant(*value), ca->alias});
        } else if (auto* ex = std::get_if<SelectColumn::Expr>(&c.data)) {
            arith(v, ex->expr);
        } else if (auto* fn = std::get_if<SelectColumn::Func>(&c.data)) {
            for (auto& arg : fn->args) arg = argument_of(v, arg);
        } else if (auto* cw = std::get_if<SelectColumn::CaseWhen>(&c.data)) {
            for (auto& b : cw->branches) {
                cond(v, b.condition);
                b.result = text_of(v, b.result);
            }
            if (cw->else_val) cw->else_val = text_of(v, *cw->else_val);
        } else if (auto* agg = std::get_if<SelectColumn::Agg>(&c.data)) {
            aggregate_argument(v, agg->col, agg->source);
            if (agg->filter) cond(v, *agg->filter);
        } else if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) {
            aggregate_argument(v, aa->col, aa->source);
            if (aa->filter) cond(v, *aa->filter);
        } else if (auto* sq = std::get_if<SelectColumn::Subquery>(&c.data)) {
            if (sq->query) statement(v, *sq->query);
        }
    }
}

void joins_of(const Vars& v, std::vector<Join>& joins) {
    for (auto& j : joins) {
        cond(v, j.on_expr);
        if (j.subquery) statement(v, *j.subquery->first);
    }
}

void conflict(const Vars& v, InsertConflict& c) {
    if (auto* update = std::get_if<InsertConflict::Update>(&c.data)) {
        for (auto& [col, expr] : update->assignments) arith(v, expr);
    }
}

void statement(const Vars& v, Statement& st) {
    if (auto* sel = std::get_if<Statement::Select>(&st.data)) {
        select_columns(v, sel->columns);
        if (sel->condition) cond(v, *sel->condition);
        joins_of(v, sel->joins);
        if (sel->having) cond(v, *sel->having);
        if (sel->subquery) statement(v, *sel->subquery->first);
    } else if (auto* ins = std::get_if<Statement::Insert>(&st.data)) {
        for (auto& row : ins->values) {
            for (auto& value : row) {
                if (!value.empty() && value[0] == '\x01') { // a value written as an expression: its variables are replaced inside it
                    ArithExpr expr = nlohmann::json::parse(value.substr(1)).get<ArithExpr>();
                    arith(v, expr);
                    value = "\x01" + nlohmann::json(expr).dump();
                } else {
                    value = text_of(v, value);
                }
            }
        }
        conflict(v, ins->on_conflict);
    } else if (auto* is = std::get_if<Statement::InsertSelect>(&st.data)) {
        if (is->query) statement(v, *is->query);
        conflict(v, is->on_conflict);
    } else if (auto* up = std::get_if<Statement::Update>(&st.data)) {
        for (auto& [col, expr] : up->assignments) arith(v, expr);
        if (up->condition) cond(v, *up->condition);
    } else if (auto* del = std::get_if<Statement::Delete>(&st.data)) {
        if (del->condition) cond(v, *del->condition);
    } else if (auto* mu = std::get_if<Statement::MultiUpdate>(&st.data)) {
        for (auto& [col, expr] : mu->assignments) arith(v, expr);
        joins_of(v, mu->joins);
        if (mu->condition) cond(v, *mu->condition);
    } else if (auto* md = std::get_if<Statement::MultiDelete>(&st.data)) {
        joins_of(v, md->joins);
        if (md->condition) cond(v, *md->condition);
    } else if (auto* u = std::get_if<Statement::Union>(&st.data)) {
        statement(v, *u->left);
        statement(v, *u->right);
    } else if (auto* i = std::get_if<Statement::Intersect>(&st.data)) {
        statement(v, *i->left);
        statement(v, *i->right);
    } else if (auto* x = std::get_if<Statement::Except>(&st.data)) {
        statement(v, *x->left);
        statement(v, *x->right);
    } else if (auto* w = std::get_if<Statement::With>(&st.data)) {
        for (auto& cte : w->ctes) statement(v, *cte.second);
        if (w->query) statement(v, *w->query);
    } else if (auto* call = std::get_if<Statement::CallProcedure>(&st.data)) {
        // (an @variable stays a name: CALL reads it, and writes an OUT parameter back to it)
        for (auto& arg : call->args) {
            if (arg.empty() || arg[0] == '@' || arg[0] == '\x01') continue;
            arg = text_of(v, arg);
        }
    }
}

} // namespace

ArithExpr Executor::value_constant(const std::string& value) {
    if (value == EXECUTOR_NULL_VALUE) return ArithExpr(ArithExpr::Str{EXECUTOR_NULL_VALUE});
    if (plain_number(value)) return ArithExpr(ArithExpr::Num{value});
    return ArithExpr(ArithExpr::Str{value});
}

void Executor::substitute_variables(Statement& stmt, const std::unordered_map<std::string, std::string>* row) const {
    if (proc_vars.empty() && user_vars.empty() && !row) return;
    statement(Vars{proc_vars, user_vars, row}, stmt);
}

void Executor::substitute_variables(ArithExpr& expr, const std::unordered_map<std::string, std::string>* row) const {
    arith(Vars{proc_vars, user_vars, row}, expr);
}

void Executor::bind_expression(SharedDatabase& s, ArithExpr& expr, bool check) {
    std::vector<const Statement*> found;
    collect_arith_subqueries(expr, found);
    if (found.empty()) return;
    // (bound as the one item of `SELECT <expression>`, like the expressions of every other statement)
    qualify_arith_inplace(s, expr);
    Statement::Select sel;
    sel.table = "_dual_";
    sel.columns.push_back(SelectColumn(SelectColumn::Expr{std::move(expr), std::nullopt}));
    Statement wrapped(std::move(sel));
    if (auto error = bind_statement(s, wrapped, check)) throw StatementError(*error);
    expr = std::move(std::get<SelectColumn::Expr>(std::get<Statement::Select>(wrapped.data).columns[0].data).expr);
}

void Executor::bind_condition(SharedDatabase& s, CondExpr& cond, bool check) {
    if (!cond_has_any_subquery(cond)) return;
    Statement::Select sel;
    sel.table = "_dual_";
    sel.columns.push_back(SelectColumn(SelectColumn::Expr{ArithExpr(ArithExpr::Num{"1"}), std::nullopt}));
    sel.condition = qualify_condexpr(s, std::move(cond));
    Statement wrapped(std::move(sel));
    if (auto error = bind_statement(s, wrapped, check)) throw StatementError(*error);
    cond = std::move(*std::get<Statement::Select>(wrapped.data).condition);
}

// An INSERT value that was written as an expression (`1 + 2`, `UPPER('x')`, `NOW()`) is kept by the parser as "\x01" + the JSON of the expression:
// it is computed here (after the variables in it have been replaced) and becomes the text of the value.
void Executor::evaluate_insert_expressions(Statement& stmt, SharedDatabase* s, bool check) {
    auto* ins = std::get_if<Statement::Insert>(&stmt.data);
    if (!ins) return;
    for (auto& row : ins->values) {
        for (auto& value : row) {
            if (value.empty() || value[0] != '\x01') continue;
            if (!s && value.find("\"Subquery\"") != std::string::npos) continue; // (computed once the statement runs: it reads tables)
            ArithExpr expr = nlohmann::json::parse(value.substr(1)).get<ArithExpr>();
            substitute_variables(expr);
            if (s) bind_expression(*s, expr, check);
            value = eval_arith(Row{}, expr);
        }
    }
}

} // namespace engine
