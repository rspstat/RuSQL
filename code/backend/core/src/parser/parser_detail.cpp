#include "parser_detail.hpp"

#include <cctype>
#include <cstring>

namespace engine::detail {

std::string expand_alias_str(const std::string& s, const std::unordered_map<std::string, std::string>& map) {
    // an aggregate call inside a name or a function's argument text -- a condition keeps `SUM(o.amount)` as one name, and an
    // argument such as `SUM(o.v)/COUNT(*)` is text: the alias in each call's argument becomes its table
    if (s.find('(') != std::string::npos) {
        static const char* names[] = {"COUNT", "SUM", "AVG", "MIN", "MAX"};
        std::string out;
        bool changed = false, in_string = false; // a quoted string that reads `SUM(o.x)` is text
        for (std::size_t i = 0; i < s.size(); i++) {
            bool expanded = false;
            if (s[i] == '\'') in_string = !in_string;
            if (!in_string && (i == 0 || !(std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_' || s[i - 1] == '.'))) {
                for (const char* name : names) {
                    const std::size_t n = std::strlen(name);
                    if (i + n >= s.size() || s[i + n] != '(') continue;
                    bool same = true;
                    for (std::size_t k = 0; k < n; k++) same = same && std::toupper(static_cast<unsigned char>(s[i + k])) == name[k];
                    if (!same) continue;
                    const std::size_t close = s.find(')', i + n + 1);
                    if (close == std::string::npos || s.find('(', i + n + 1) < close) break;
                    const std::string inner = s.substr(i + n + 1, close - (i + n + 1));
                    const std::string inner_expanded = expand_alias_str(inner, map);
                    out += s.substr(i, n + 1) + inner_expanded + ")";
                    changed = changed || inner_expanded != inner;
                    i = close;
                    expanded = true;
                    break;
                }
            }
            if (!expanded) out += s[i];
        }
        if (changed) return out;
    }
    // every `alias.column` in the text: a function's argument may be an expression (`ROUND(y.g * y.g, 2)` keeps `y.g*y.g` as text)
    if (s.find('.') == std::string::npos) return s;
    std::string out;
    bool in_string = false;
    for (std::size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\'') in_string = !in_string;
        if (!in_string && (std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_') &&
            (i == 0 || !(std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_' || s[i - 1] == '.'))) {
            std::size_t end = i;
            while (end < s.size() && (std::isalnum(static_cast<unsigned char>(s[end])) || s[end] == '_')) end++;
            if (end < s.size() && s[end] == '.') {
                if (auto it = map.find(s.substr(i, end - i)); it != map.end()) {
                    out += it->second;
                    i = end - 1;
                    continue;
                }
            }
        }
        out += s[i];
    }
    return out;
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
            } else if constexpr (std::is_same_v<T, ArithExpr::Pred>) {
                return ArithExpr(ArithExpr::Pred{std::make_unique<CondExpr>(expand_condexpr(*alt.cond, map))});
            } else if constexpr (std::is_same_v<T, ArithExpr::Subquery>) {
                return ArithExpr(ArithExpr::Subquery{std::make_unique<Statement>(*alt.query), alt.cls}); // (its aliases are its own)
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
            } else if constexpr (std::is_same_v<T, SelectColumn::All>) {
                // `alias.*` names the table
                if (alt.table.empty()) return col;
                auto it = map.find(alt.table);
                return SelectColumn(SelectColumn::All{it != map.end() ? it->second : alt.table});
            } else {
                // Subquery passes through unchanged — copy via
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
                return ConditionValue(ConditionValue::Literal{alt.quoted ? alt.value : expand_alias_str(alt.value, map), alt.quoted}); // (a string is not a column)
            } else if constexpr (std::is_same_v<T, ConditionValue::Between>) {
                return ConditionValue(ConditionValue::Between{alt.lo_quoted ? alt.lo : expand_alias_str(alt.lo, map), alt.hi_quoted ? alt.hi : expand_alias_str(alt.hi, map), alt.lo_quoted, alt.hi_quoted});
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
