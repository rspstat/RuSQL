// Outer references. A subquery can name a column of the query around it (`WHERE b.a_id = a.id`, `SELECT a.v + b.k`, `HAVING COUNT(*) >= a.w`) and
// then it is run once for every row of that query, with the row's values in place of those columns. It used to be found by looking at the
// shape of the text -- a dotted name on the right of a comparison -- which missed a column on the left (`WHERE a.id = b.a_id` was true for every
// row, a DELETE with it deleted them all), in a function, in the select list, an alias of the outer table, a column named without its table,
// and a subquery inside a subquery. Now the binder (executor_bind.cpp) knows what every name refers to and marks the references to an enclosing
// query -- ArithExpr::Col::outer, ConditionValue::Literal::outer and SelectColumn::Column::outer are how many queries out the column is -- and
// this puts the values in.

#include <functional>

#include "engine/executor/executor.hpp"

namespace engine {

namespace {

struct Outer {
    std::function<std::string(const std::string&)> lookup; // the value of a column of the outer row; empty when the statement is only looked at
    bool escapes = false;                                    // a reference points out of the statement that was walked

    // A reference `flag` queries out, found `depth` queries inside the statement that is walked, is to the row when it is to the query around
    // that statement (flag == depth + 1); with a larger flag it points further out still.
    bool takes(const std::string& name, int flag, int depth, std::string& value) {
        if (flag <= 0) return false;
        if (flag > depth) escapes = true;
        if (!lookup || flag != depth + 1) return false;
        value = lookup(name);
        return true;
    }

    void arith(ArithExpr& e, int depth) {
        if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) {
            std::string value;
            if (takes(col->name, col->outer, depth, value)) e = Executor::value_constant(value);
        } else if (auto* a = std::get_if<ArithExpr::Add>(&e.data)) { arith(*a->lhs, depth); arith(*a->rhs, depth); }
        else if (auto* s = std::get_if<ArithExpr::Sub>(&e.data)) { arith(*s->lhs, depth); arith(*s->rhs, depth); }
        else if (auto* m = std::get_if<ArithExpr::Mul>(&e.data)) { arith(*m->lhs, depth); arith(*m->rhs, depth); }
        else if (auto* d = std::get_if<ArithExpr::Div>(&e.data)) { arith(*d->lhs, depth); arith(*d->rhs, depth); }
        else if (auto* c = std::get_if<ArithExpr::Cmp>(&e.data)) { arith(*c->lhs, depth); arith(*c->rhs, depth); }
        else if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) {
            for (auto& arg : f->args) arith(arg, depth);
        } else if (auto* p = std::get_if<ArithExpr::Pred>(&e.data)) {
            cond(*p->cond, depth);
        }
    }

    // The index and hash paths look for `column <op> value`: a comparison whose left side became a value (`a.id = b.a_id`) is turned round
    // (`b.a_id = <the value>`) so that they still find the column on the left.
    static void turn_round(Condition& c) {
        Operator flipped;
        switch (c.op) {
            case Operator::Eq: case Operator::Ne: flipped = c.op; break;
            case Operator::Gt: flipped = Operator::Lt; break;
            case Operator::Lt: flipped = Operator::Gt; break;
            case Operator::Gte: flipped = Operator::Lte; break;
            case Operator::Lte: flipped = Operator::Gte; break;
            default: return;
        }
        auto* lit = std::get_if<ConditionValue::Literal>(&c.value.data);
        if (!lit || lit->quoted || lit->value.empty() || lit->value == "__NULL__" || parse_number(lit->value)) return; // (not a column)
        const std::string* text = nullptr;
        if (auto* n = std::get_if<ArithExpr::Num>(&c.left.data)) text = &n->value;
        else if (auto* s = std::get_if<ArithExpr::Str>(&c.left.data)) text = &s->value;
        if (!text || *text == EXECUTOR_NULL_VALUE) return;
        const bool text_class = c.left_class == ValueClass::Text;
        ConditionValue value(ConditionValue::Literal{*text, text_class || !parse_number(*text), 0});
        c.left = ArithExpr(ArithExpr::Col{lit->value, c.right_class, 0});
        c.value = std::move(value);
        c.op = flipped;
        std::swap(c.left_class, c.right_class);
    }

    void cond(CondExpr& e, int depth) {
        if (auto* a = std::get_if<CondExpr::And>(&e.data)) { cond(*a->lhs, depth); cond(*a->rhs, depth); }
        else if (auto* o = std::get_if<CondExpr::Or>(&e.data)) { cond(*o->lhs, depth); cond(*o->rhs, depth); }
        else if (auto* n = std::get_if<CondExpr::Not>(&e.data)) cond(*n->inner, depth);
        else if (auto* leaf = std::get_if<CondExpr::Leaf>(&e.data)) {
            Condition& c = leaf->condition;
            const bool left_is_outer = lookup && std::holds_alternative<ArithExpr::Col>(c.left.data) &&
                                       std::get<ArithExpr::Col>(c.left.data).outer == depth + 1;
            arith(c.left, depth);
            if (auto* lit = std::get_if<ConditionValue::Literal>(&c.value.data)) {
                std::string value;
                if (takes(lit->value, lit->outer, depth, value)) {
                    // (what the outer row holds is a value, never the name of a column)
                    lit->quoted = value != EXECUTOR_NULL_VALUE && (c.right_class == ValueClass::Text || !parse_number(value));
                    lit->value = value == EXECUTOR_NULL_VALUE ? "__NULL__" : std::move(value);
                    lit->outer = 0;
                }
            } else if (auto* value = std::get_if<ConditionValue::Arith>(&c.value.data)) {
                arith(value->expr, depth);
            } else if (auto* sub = std::get_if<ConditionValue::Subquery>(&c.value.data)) {
                if (sub->query) statement(*sub->query, depth + 1);
            }
            if (left_is_outer) turn_round(c);
        }
    }

    void select_columns(std::vector<SelectColumn>& columns, int depth) {
        for (auto& c : columns) {
            std::string value;
            if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) {
                if (takes(col->name, col->outer, depth, value)) c = SelectColumn(SelectColumn::Expr{Executor::value_constant(value), col->name});
            } else if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) {
                if (takes(ca->name, ca->outer, depth, value)) c = SelectColumn(SelectColumn::Expr{Executor::value_constant(value), ca->alias});
            } else if (auto* ex = std::get_if<SelectColumn::Expr>(&c.data)) {
                arith(ex->expr, depth);
            } else if (auto* cw = std::get_if<SelectColumn::CaseWhen>(&c.data)) {
                for (auto& b : cw->branches) cond(b.condition, depth);
            } else if (auto* agg = std::get_if<SelectColumn::Agg>(&c.data)) {
                if (agg->filter) cond(*agg->filter, depth);
            } else if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) {
                if (aa->filter) cond(*aa->filter, depth);
            } else if (auto* sq = std::get_if<SelectColumn::Subquery>(&c.data)) {
                if (sq->query) statement(*sq->query, depth + 1);
            }
        }
    }

    void statement(Statement& st, int depth) {
        if (auto* sel = std::get_if<Statement::Select>(&st.data)) {
            select_columns(sel->columns, depth);
            if (sel->condition) cond(*sel->condition, depth);
            for (auto& j : sel->joins) {
                cond(j.on_expr, depth);
                if (j.subquery) statement(*j.subquery->first, depth + 1);
            }
            if (sel->having) cond(*sel->having, depth);
            if (sel->subquery) statement(*sel->subquery->first, depth + 1);
        } else if (auto* u = std::get_if<Statement::Union>(&st.data)) {
            statement(*u->left, depth);
            statement(*u->right, depth);
        } else if (auto* i = std::get_if<Statement::Intersect>(&st.data)) {
            statement(*i->left, depth);
            statement(*i->right, depth);
        } else if (auto* x = std::get_if<Statement::Except>(&st.data)) {
            statement(*x->left, depth);
            statement(*x->right, depth);
        } else if (auto* w = std::get_if<Statement::With>(&st.data)) {
            for (auto& cte : w->ctes) statement(*cte.second, depth);
            if (w->query) statement(*w->query, depth);
        }
    }
};

} // namespace

bool Executor::refers_outside(const Statement& st) {
    Statement copy(st); // (the walk is the one that puts values in; here it only looks)
    Outer outer;
    outer.statement(copy, 0);
    return outer.escapes;
}

void Executor::substitute_outer(Statement& st, const Row& outer_row) {
    Outer outer;
    outer.lookup = [&outer_row](const std::string& name) {
        const std::string* value = get_col(outer_row, name);
        return value ? *value : std::string(EXECUTOR_NULL_VALUE);
    };
    outer.statement(st, 0);
}

} // namespace engine
