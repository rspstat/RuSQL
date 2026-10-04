#include "parser_detail.hpp"

#include <cctype>

namespace engine::detail {

std::string expand_alias_str(const std::string& s, const std::unordered_map<std::string, std::string>& map) {
    // an aggregate in a condition is kept as one name, `SUM(o.amount)`: expand its argument
    if (s.size() > 4 && s.back() == ')') {
        auto lp = s.find('(');
        if (lp != std::string::npos && lp > 0 && s.find('(', lp + 1) == std::string::npos) {
            std::string fn = s.substr(0, lp);
            for (auto& c : fn) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (fn == "COUNT" || fn == "SUM" || fn == "AVG" || fn == "MIN" || fn == "MAX") {
                return s.substr(0, lp + 1) + expand_alias_str(s.substr(lp + 1, s.size() - lp - 2), map) + ")";
            }
        }
    }
    auto dot = s.find('.');
    if (dot != std::string::npos) {
        std::string prefix = s.substr(0, dot);
        auto it = map.find(prefix);
        if (it != map.end()) {
            return it->second + "." + s.substr(dot + 1);
        }
    }
    return s;
}

ArithExpr expand_arith(const ArithExpr& expr, const std::unordered_map<std::string, std::string>& map) {
    return std::visit(
        [&map](const auto& alt) -> ArithExpr {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, ArithExpr::Col>) {
                return ArithExpr(ArithExpr::Col{expand_alias_str(alt.name, map)});
            } else if constexpr (std::is_same_v<T, ArithExpr::Num>) {
                return ArithExpr(ArithExpr::Num{alt.value});
            } else if constexpr (std::is_same_v<T, ArithExpr::Str>) {
                return ArithExpr(ArithExpr::Str{alt.value});
            } else if constexpr (std::is_same_v<T, ArithExpr::Add>) {
                return ArithExpr(ArithExpr::Add{std::make_unique<ArithExpr>(expand_arith(*alt.lhs, map)),
                                                std::make_unique<ArithExpr>(expand_arith(*alt.rhs, map))});
            } else if constexpr (std::is_same_v<T, ArithExpr::Sub>) {
                return ArithExpr(ArithExpr::Sub{std::make_unique<ArithExpr>(expand_arith(*alt.lhs, map)),
                                                std::make_unique<ArithExpr>(expand_arith(*alt.rhs, map))});
            } else if constexpr (std::is_same_v<T, ArithExpr::Mul>) {
                return ArithExpr(ArithExpr::Mul{std::make_unique<ArithExpr>(expand_arith(*alt.lhs, map)),
                                                std::make_unique<ArithExpr>(expand_arith(*alt.rhs, map))});
            } else if constexpr (std::is_same_v<T, ArithExpr::Div>) {
                return ArithExpr(ArithExpr::Div{std::make_unique<ArithExpr>(expand_arith(*alt.lhs, map)),
                                                std::make_unique<ArithExpr>(expand_arith(*alt.rhs, map))});
            } else if constexpr (std::is_same_v<T, ArithExpr::Func>) {
                std::vector<ArithExpr> args;
                args.reserve(alt.args.size());
                for (auto& a : alt.args) args.push_back(expand_arith(a, map));
                return ArithExpr(ArithExpr::Func{alt.name, std::move(args)});
            } else if constexpr (std::is_same_v<T, ArithExpr::Cmp>) {
                return ArithExpr(ArithExpr::Cmp{std::make_unique<ArithExpr>(expand_arith(*alt.lhs, map)), alt.op,
                                                std::make_unique<ArithExpr>(expand_arith(*alt.rhs, map))});
            }
        },
        expr.data);
}

SelectColumn expand_select_column(const SelectColumn& col, const std::unordered_map<std::string, std::string>& map) {
    return std::visit(
        [&map, &col](const auto& alt) -> SelectColumn {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, SelectColumn::Column>) {
                return SelectColumn(SelectColumn::Column{expand_alias_str(alt.name, map)});
            } else if constexpr (std::is_same_v<T, SelectColumn::ColumnAlias>) {
                return SelectColumn(SelectColumn::ColumnAlias{expand_alias_str(alt.name, map), alt.alias});
            } else if constexpr (std::is_same_v<T, SelectColumn::Func>) {
                std::vector<std::string> args;
                args.reserve(alt.args.size());
                for (auto& a : alt.args) args.push_back(expand_alias_str(a, map));
                return SelectColumn(SelectColumn::Func{alt.name, std::move(args), alt.alias});
            } else if constexpr (std::is_same_v<T, SelectColumn::Expr>) {
                return SelectColumn(SelectColumn::Expr{expand_arith(alt.expr, map), alt.alias});
            } else if constexpr (std::is_same_v<T, SelectColumn::CaseWhen>) {
                std::vector<CaseWhenBranch> branches;
                branches.reserve(alt.branches.size());
                for (auto& b : alt.branches) branches.push_back(CaseWhenBranch{expand_condexpr(b.condition, map), b.result});
                return SelectColumn(SelectColumn::CaseWhen{std::move(branches), alt.else_val, alt.alias});
            } else if constexpr (std::is_same_v<T, SelectColumn::Agg>) {
                // `col` stays as typed (it is the result column's name); `source` is what the rows hold
                std::string source = expand_alias_str(alt.col, map);
                std::optional<CondExpr> filter;
                if (alt.filter) filter = expand_condexpr(*alt.filter, map);
                return SelectColumn(SelectColumn::Agg{alt.func, alt.col, std::move(filter), source == alt.col ? std::string() : source});
            } else if constexpr (std::is_same_v<T, SelectColumn::AggAlias>) {
                std::string source = expand_alias_str(alt.col, map);
                std::optional<CondExpr> filter;
                if (alt.filter) filter = expand_condexpr(*alt.filter, map);
                return SelectColumn(SelectColumn::AggAlias{alt.func, alt.col, alt.alias, std::move(filter), source == alt.col ? std::string() : source});
            } else if constexpr (std::is_same_v<T, SelectColumn::WinFunc>) {
                SelectColumn::WinFunc w = alt;
                if (w.col) w.col = expand_alias_str(*w.col, map);
                for (auto& c : w.partition_by) c = expand_alias_str(c, map);
                for (auto& o : w.order_by) o.column = expand_alias_str(o.column, map);
                return SelectColumn(std::move(w));
            } else {
                // All/Subquery pass through unchanged — copy via
                // SelectColumn's own deep-copy constructor (Subquery holds a
                // non-copyable unique_ptr<Statement>, so `alt` itself can't be copied
                // directly; `col` can, via SelectColumn::SelectColumn(const SelectColumn&)).
                return col;
            }
        },
        col.data);
}

Condition expand_leaf(const Condition& cond, const std::unordered_map<std::string, std::string>& map) {
    ConditionValue value = std::visit(
        [&map, &cond](const auto& alt) -> ConditionValue {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, ConditionValue::Literal>) {
                return ConditionValue(ConditionValue::Literal{expand_alias_str(alt.value, map)});
            } else if constexpr (std::is_same_v<T, ConditionValue::Between>) {
                return ConditionValue(ConditionValue::Between{expand_alias_str(alt.lo, map), expand_alias_str(alt.hi, map)});
            } else if constexpr (std::is_same_v<T, ConditionValue::Arith>) {
                return ConditionValue(ConditionValue::Arith{expand_arith(alt.expr, map)});
            } else {
                // Subquery holds a non-copyable unique_ptr<Statement>; copy via
                // ConditionValue's own deep-copy constructor instead of copying `alt` directly.
                return cond.value;
            }
        },
        cond.value.data);
    return Condition{expand_arith(cond.left, map), cond.op, std::move(value)};
}

CondExpr expand_condexpr(const CondExpr& expr, const std::unordered_map<std::string, std::string>& map) {
    return std::visit(
        [&map](const auto& alt) -> CondExpr {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, CondExpr::And>) {
                return CondExpr(CondExpr::And{std::make_unique<CondExpr>(expand_condexpr(*alt.lhs, map)),
                                              std::make_unique<CondExpr>(expand_condexpr(*alt.rhs, map))});
            } else if constexpr (std::is_same_v<T, CondExpr::Or>) {
                return CondExpr(CondExpr::Or{std::make_unique<CondExpr>(expand_condexpr(*alt.lhs, map)),
                                             std::make_unique<CondExpr>(expand_condexpr(*alt.rhs, map))});
            } else if constexpr (std::is_same_v<T, CondExpr::Not>) {
                return CondExpr(CondExpr::Not{std::make_unique<CondExpr>(expand_condexpr(*alt.inner, map))});
            } else {
                return CondExpr(CondExpr::Leaf{expand_leaf(alt.condition, map)});
            }
        },
        expr.data);
}

} // namespace engine::detail
