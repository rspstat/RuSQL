#pragma once

// A write to a view names the columns of the view; the table below it has its own names for them (`CREATE VIEW v AS SELECT id AS ident, name AS title FROM t`).
// These rename the column names of an expression or a condition from the view's to the table's (the pairs are (name in the view, name in the table), case apart).

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "engine/parser/ast.hpp"

namespace engine {

using ViewColumnMap = std::vector<std::pair<std::string, std::string>>;

inline bool view_names_equal(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
}

// the entry of the map for a column written as `name` or `qualifier.name` (null when the view has no such column)
inline const std::pair<std::string, std::string>* view_column_entry(const ViewColumnMap& map, const std::string& written) {
    const auto dot = written.rfind('.');
    const std::string bare = dot == std::string::npos ? written : written.substr(dot + 1);
    for (auto& entry : map) {
        if (!entry.first.empty() && view_names_equal(entry.first, bare)) return &entry;
    }
    return nullptr;
}

// true when every column of the view is the column of the table of the same name (nothing to rename)
inline bool view_columns_are_plain(const ViewColumnMap& map) {
    for (auto& entry : map) {
        if (entry.second.empty() || !view_names_equal(entry.first, entry.second)) return false;
    }
    return true;
}

inline void rename_cond_columns(CondExpr& e, const ViewColumnMap& map);

inline void rename_arith_columns(ArithExpr& e, const ViewColumnMap& map) {
    if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) {
        if (col->outer == 0) {
            if (auto* entry = view_column_entry(map, col->name); entry && !entry->second.empty()) col->name = entry->second;
        }
    } else if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) { rename_arith_columns(*v->lhs, map); rename_arith_columns(*v->rhs, map); }
    else if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) { rename_arith_columns(*v->lhs, map); rename_arith_columns(*v->rhs, map); }
    else if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) { rename_arith_columns(*v->lhs, map); rename_arith_columns(*v->rhs, map); }
    else if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) { rename_arith_columns(*v->lhs, map); rename_arith_columns(*v->rhs, map); }
    else if (auto* v = std::get_if<ArithExpr::Cmp>(&e.data)) { rename_arith_columns(*v->lhs, map); rename_arith_columns(*v->rhs, map); }
    else if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) {
        for (auto& a : f->args) rename_arith_columns(a, map);
    } else if (auto* p = std::get_if<ArithExpr::Pred>(&e.data)) {
        rename_cond_columns(*p->cond, map);
    }
}

inline void rename_cond_columns(CondExpr& e, const ViewColumnMap& map) {
    if (auto* a = std::get_if<CondExpr::And>(&e.data)) { rename_cond_columns(*a->lhs, map); rename_cond_columns(*a->rhs, map); }
    else if (auto* o = std::get_if<CondExpr::Or>(&e.data)) { rename_cond_columns(*o->lhs, map); rename_cond_columns(*o->rhs, map); }
    else if (auto* n = std::get_if<CondExpr::Not>(&e.data)) rename_cond_columns(*n->inner, map);
    else if (auto* leaf = std::get_if<CondExpr::Leaf>(&e.data)) {
        rename_arith_columns(leaf->condition.left, map);
        if (auto* value = std::get_if<ConditionValue::Arith>(&leaf->condition.value.data)) {
            rename_arith_columns(value->expr, map);
        } else if (auto* literal = std::get_if<ConditionValue::Literal>(&leaf->condition.value.data)) {
            // (a value that is not quoted may be the name of a column: `WHERE ident = other`)
            if (!literal->quoted && literal->outer == 0) {
                if (auto* entry = view_column_entry(map, literal->value); entry && !entry->second.empty()) literal->value = entry->second;
            }
        }
    }
}

// A column of a view that is `k + 1 AS k1` (R85): (its name, the expression, written with the columns of the table below). A write that reads that column puts the expression in
// its place, as MySQL does (`WHERE k1 = 11` is `WHERE k + 1 = 11`).
using ViewExpressions = std::vector<std::pair<std::string, ArithExpr>>;

inline const ArithExpr* view_expression(const ViewExpressions& expressions, const std::string& written) {
    const auto dot = written.rfind('.');
    const std::string bare = dot == std::string::npos ? written : written.substr(dot + 1);
    for (auto& entry : expressions) {
        if (view_names_equal(entry.first, bare)) return &entry.second;
    }
    return nullptr;
}

// The columns of an expression / a condition, one by one (R86). `v.col(node, col)` is asked for every column (not one of an outer query): `node` is the expression that holds it, and
// it may be put another expression in its place. `v.literal(leaf, literal)` is asked for every value that is not quoted (it may be the name of a column: `WHERE ident = k1`). The
// answer is false when any question was answered false.
template <class V>
bool walk_view_columns(CondExpr& e, const V& v);

template <class V>
bool walk_view_columns(ArithExpr& e, const V& v) {
    if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) {
        if (col->outer != 0) return true;
        return v.col(e, *col);
    }
    auto both = [&](ArithExpr& a, ArithExpr& b) {
        const bool left = walk_view_columns(a, v);
        const bool right = walk_view_columns(b, v);
        return left && right;
    };
    if (auto* x = std::get_if<ArithExpr::Add>(&e.data)) return both(*x->lhs, *x->rhs);
    if (auto* x = std::get_if<ArithExpr::Sub>(&e.data)) return both(*x->lhs, *x->rhs);
    if (auto* x = std::get_if<ArithExpr::Mul>(&e.data)) return both(*x->lhs, *x->rhs);
    if (auto* x = std::get_if<ArithExpr::Div>(&e.data)) return both(*x->lhs, *x->rhs);
    if (auto* x = std::get_if<ArithExpr::Cmp>(&e.data)) return both(*x->lhs, *x->rhs);
    if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) {
        bool all = true;
        for (auto& a : f->args) all = walk_view_columns(a, v) && all;
        return all;
    }
    if (auto* p = std::get_if<ArithExpr::Pred>(&e.data)) return walk_view_columns(*p->cond, v);
    return true;
}

template <class V>
bool walk_view_columns(CondExpr& e, const V& v) {
    if (auto* a = std::get_if<CondExpr::And>(&e.data)) {
        const bool left = walk_view_columns(*a->lhs, v);
        const bool right = walk_view_columns(*a->rhs, v);
        return left && right;
    }
    if (auto* o = std::get_if<CondExpr::Or>(&e.data)) {
        const bool left = walk_view_columns(*o->lhs, v);
        const bool right = walk_view_columns(*o->rhs, v);
        return left && right;
    }
    if (auto* n = std::get_if<CondExpr::Not>(&e.data)) return walk_view_columns(*n->inner, v);
    if (auto* leaf = std::get_if<CondExpr::Leaf>(&e.data)) {
        bool ok = walk_view_columns(leaf->condition.left, v);
        if (auto* value = std::get_if<ConditionValue::Arith>(&leaf->condition.value.data)) {
            ok = walk_view_columns(value->expr, v) && ok;
        } else if (auto* literal = std::get_if<ConditionValue::Literal>(&leaf->condition.value.data)) {
            if (!literal->quoted && literal->outer == 0) ok = v.literal(*leaf, *literal) && ok;
        }
        return ok;
    }
    return true;
}

// The names of the view's columns in an expression become the table's, and a column that is an expression becomes the expression
struct ViewColumnRewriter {
    const ViewColumnMap& map;
    const ViewExpressions& expressions;
    bool col(ArithExpr& node, ArithExpr::Col& col) const {
        const auto* entry = view_column_entry(map, col.name);
        if (!entry) return true;
        if (!entry->second.empty()) {
            col.name = entry->second;
            return true;
        }
        if (const ArithExpr* expr = view_expression(expressions, col.name)) {
            ArithExpr copy = *expr;
            node = std::move(copy);
            return true;
        }
        return false;
    }
    bool literal(CondExpr::Leaf& leaf, ConditionValue::Literal& literal) const {
        const auto* entry = view_column_entry(map, literal.value);
        if (!entry) return true;
        if (!entry->second.empty()) {
            literal.value = entry->second;
            return true;
        }
        if (const ArithExpr* expr = view_expression(expressions, literal.value)) {
            ArithExpr copy = *expr;
            leaf.condition.value.data = ConditionValue::Arith{std::move(copy)};
            return true;
        }
        return false;
    }
};

// False when a column of an expression is named that has no expression to put in its place (a function, a CASE): nothing can be done with it.
inline bool rewrite_view_columns(ArithExpr& e, const ViewColumnMap& map, const ViewExpressions& expressions) { return walk_view_columns(e, ViewColumnRewriter{map, expressions}); }
inline bool rewrite_view_columns(CondExpr& e, const ViewColumnMap& map, const ViewExpressions& expressions) { return walk_view_columns(e, ViewColumnRewriter{map, expressions}); }

// What stops a write through a view whatever it reads (R85): DISTINCT, GROUP BY, HAVING, LIMIT, an aggregate or a window function in the select list, an outer join. MySQL's
// IS_UPDATABLE says NO for each of them
inline bool view_has_write_blocker(const Statement::Select& sel) {
    const bool outer = std::any_of(sel.joins.begin(), sel.joins.end(), [](const Join& j) { return j.join_type == JoinType::Left || j.join_type == JoinType::Right || j.join_type == JoinType::FullOuter; });
    const bool aggregate = std::any_of(sel.columns.begin(), sel.columns.end(), [](const SelectColumn& c) {
        return std::holds_alternative<SelectColumn::Agg>(c.data) || std::holds_alternative<SelectColumn::AggAlias>(c.data) || std::holds_alternative<SelectColumn::WinFunc>(c.data);
    });
    return sel.distinct || (sel.group_by && !sel.group_by->empty()) || sel.having || sel.limit || sel.offset || outer || aggregate;
}

// A derived table in the FROM list or joined (`FROM (SELECT ...) d`): MySQL's IS_UPDATABLE says YES for such a view and every write is refused all the same (R85)
inline bool view_reads_derived_table(const Statement::Select& sel) {
    return sel.subquery.has_value() || std::any_of(sel.joins.begin(), sel.joins.end(), [](const Join& j) { return j.subquery.has_value(); });
}

} // namespace engine
