#pragma once

// The AND-ed parts of a condition and the equality of an ON condition that every pair of rows which meets the whole ON meets too: what lets a join be hashed on that
// equality (hashed_join_verified, join.hpp) instead of building a merged row for every pair of rows. Used by SELECT's joins, MERGE and the multi-table UPDATE / DELETE.

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "engine/parser/ast.hpp"
#include "engine/value_class.hpp"

namespace engine {

inline void and_conjuncts(const CondExpr& e, std::vector<const CondExpr*>& out) {
    if (auto* a = std::get_if<CondExpr::And>(&e.data)) {
        and_conjuncts(*a->lhs, out);
        and_conjuncts(*a->rhs, out);
    } else {
        out.push_back(&e);
    }
}

// A top-level AND-ed part of the ON condition of the form `<a> = <b>` where exactly one side is "<right table>.<column>"
// of the table being joined: {the other side, that column}. Every pair that satisfies the whole ON satisfies this part.
struct OnEquality {
    std::string left_ref;  // the side that is not a column of the joined table
    std::string right_col; // the column of the joined table
    ValueClass left_class, right_class; // what each side holds
};

inline std::optional<OnEquality> equality_part_of_on(const CondExpr& on, const std::string& right_full, const std::string& right_bare,
                                                     const std::unordered_set<std::string>& right_cols) {
    auto as_right = [&](const std::string& ref) -> std::optional<std::string> {
        auto cut = ref.rfind('.');
        if (cut == std::string::npos) return std::nullopt;
        std::string q = ref.substr(0, cut), c = ref.substr(cut + 1);
        if ((q == right_full || q == right_bare) && right_cols.count(c)) return c;
        return std::nullopt;
    };
    std::vector<const CondExpr*> parts;
    and_conjuncts(on, parts);
    for (const CondExpr* part : parts) {
        auto* leaf = std::get_if<CondExpr::Leaf>(&part->data);
        if (!leaf || leaf->condition.op != Operator::Eq) continue;
        auto* l = std::get_if<ArithExpr::Col>(&leaf->condition.left.data);
        auto* r = std::get_if<ConditionValue::Literal>(&leaf->condition.value.data);
        if (!l || !r || r->quoted) continue;
        auto rc = as_right(r->value), lc = as_right(l->name);
        if (rc && !lc) return OnEquality{l->name, *rc, leaf->condition.left_class, leaf->condition.right_class};
        if (lc && !rc) return OnEquality{r->value, *lc, leaf->condition.right_class, leaf->condition.left_class};
    }
    return std::nullopt;
}

} // namespace engine
