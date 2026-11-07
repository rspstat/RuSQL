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

} // namespace engine
