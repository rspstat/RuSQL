#pragma once

// The scalar subqueries a statement uses as values in its own expressions (`(SELECT MAX(k) FROM b) + 1`, `COALESCE((SELECT ...), 0)`,
// `SET v = (SELECT ...)`, `VALUES (1, (SELECT ...))`): ArithExpr::Subquery. The code that has to know which tables a statement reads (the locks it
// takes, whether it only reads, what a cached answer depends on) asks for them here.

#include <string>
#include <vector>

#include "engine/parser/ast.hpp"

namespace engine {

inline void collect_cond_subqueries(const CondExpr& e, std::vector<const Statement*>& out, bool leaf_values = false);

inline void collect_arith_subqueries(const ArithExpr& e, std::vector<const Statement*>& out) {
    if (auto* sq = std::get_if<ArithExpr::Subquery>(&e.data)) {
        if (sq->query) out.push_back(sq->query.get());
    } else if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) { collect_arith_subqueries(*v->lhs, out); collect_arith_subqueries(*v->rhs, out); }
    else if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) { collect_arith_subqueries(*v->lhs, out); collect_arith_subqueries(*v->rhs, out); }
    else if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) { collect_arith_subqueries(*v->lhs, out); collect_arith_subqueries(*v->rhs, out); }
    else if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) { collect_arith_subqueries(*v->lhs, out); collect_arith_subqueries(*v->rhs, out); }
    else if (auto* v = std::get_if<ArithExpr::Cmp>(&e.data)) { collect_arith_subqueries(*v->lhs, out); collect_arith_subqueries(*v->rhs, out); }
    else if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) {
        for (auto& a : f->args) collect_arith_subqueries(a, out);
    } else if (auto* p = std::get_if<ArithExpr::Pred>(&e.data)) {
        collect_cond_subqueries(*p->cond, out, true); // (a condition used as a value: nobody else looks at the subqueries it compares with)
    }
}

// (the subqueries a WHERE / HAVING / ON compares with -- IN (SELECT ...), EXISTS, `x > (SELECT ...)` -- are ConditionValue::Subquery, which the callers
// know: `leaf_values` false leaves them out; these are the ones inside the expressions of a condition)
inline void collect_cond_subqueries(const CondExpr& e, std::vector<const Statement*>& out, bool leaf_values) {
    if (auto* a = std::get_if<CondExpr::And>(&e.data)) { collect_cond_subqueries(*a->lhs, out, leaf_values); collect_cond_subqueries(*a->rhs, out, leaf_values); }
    else if (auto* o = std::get_if<CondExpr::Or>(&e.data)) { collect_cond_subqueries(*o->lhs, out, leaf_values); collect_cond_subqueries(*o->rhs, out, leaf_values); }
    else if (auto* n = std::get_if<CondExpr::Not>(&e.data)) collect_cond_subqueries(*n->inner, out, leaf_values);
    else if (auto* leaf = std::get_if<CondExpr::Leaf>(&e.data)) {
        if (leaf_values) {
            if (auto* sq = std::get_if<ConditionValue::Subquery>(&leaf->condition.value.data); sq && sq->query) out.push_back(sq->query.get());
        }
        collect_arith_subqueries(leaf->condition.left, out);
        if (auto* value = std::get_if<ConditionValue::Arith>(&leaf->condition.value.data)) collect_arith_subqueries(value->expr, out);
    }
}

// Does a condition hold a subquery anywhere: compared with, or inside an expression of it? (Such a condition is answered on the thread of the
// statement, never on the workers of a parallel scan.)
inline bool cond_has_any_subquery(const CondExpr& e) {
    std::vector<const Statement*> found;
    collect_cond_subqueries(e, found, true);
    return !found.empty();
}

// ... in the expressions of a SELECT
inline std::vector<const Statement*> select_expression_subqueries(const Statement::Select& sel) {
    std::vector<const Statement*> out;
    for (auto& c : sel.columns) {
        if (auto* e = std::get_if<SelectColumn::Expr>(&c.data)) collect_arith_subqueries(e->expr, out);
    }
    if (sel.condition) collect_cond_subqueries(*sel.condition, out);
    if (sel.having) collect_cond_subqueries(*sel.having, out);
    for (auto& j : sel.joins) collect_cond_subqueries(j.on_expr, out);
    return out;
}

// Every such subquery in the expressions of a statement (not in the subqueries themselves, nor in the statements it contains: those are looked at when
// they run).
inline std::vector<const Statement*> expression_subqueries(const Statement& stmt) {
    std::vector<const Statement*> out;
    auto cond = [&](const std::optional<CondExpr>& c) {
        if (c) collect_cond_subqueries(*c, out);
    };
    auto joins = [&](const std::vector<Join>& list) {
        for (auto& j : list) collect_cond_subqueries(j.on_expr, out);
    };
    if (auto* sel = std::get_if<Statement::Select>(&stmt.data)) {
        return select_expression_subqueries(*sel);
    } else if (auto* up = std::get_if<Statement::Update>(&stmt.data)) {
        for (auto& [column, value] : up->assignments) collect_arith_subqueries(value, out);
        cond(up->condition);
    } else if (auto* del = std::get_if<Statement::Delete>(&stmt.data)) {
        cond(del->condition);
    } else if (auto* mu = std::get_if<Statement::MultiUpdate>(&stmt.data)) {
        for (auto& [column, value] : mu->assignments) collect_arith_subqueries(value, out);
        joins(mu->joins);
        cond(mu->condition);
    } else if (auto* md = std::get_if<Statement::MultiDelete>(&stmt.data)) {
        joins(md->joins);
        cond(md->condition);
    } else if (auto* mg = std::get_if<Statement::Merge>(&stmt.data)) {
        collect_cond_subqueries(mg->on, out);
        cond(mg->when_matched_update_cond);
        cond(mg->when_matched_delete_cond);
        if (mg->when_matched_update) {
            for (auto& [column, value] : *mg->when_matched_update) collect_arith_subqueries(value, out);
        }
    }
    return out;
}

// Does the statement read a table through such a subquery? (An INSERT keeps the expression of a value as "\x01" and its JSON.)
inline bool has_expression_subquery(const Statement& stmt) {
    if (!expression_subqueries(stmt).empty()) return true;
    if (auto* ins = std::get_if<Statement::Insert>(&stmt.data)) {
        for (auto& row : ins->values) {
            for (auto& value : row) {
                if (!value.empty() && value[0] == '\x01' && value.find("\"Subquery\"") != std::string::npos) return true;
            }
        }
    }
    return false;
}

} // namespace engine
