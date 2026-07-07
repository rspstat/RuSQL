#pragma once

// Internal helpers shared across the parser_*.cpp translation units — not part of the
// public engine:: API surface (mirrors free functions private to parser.rs's module).

#include <string>
#include <unordered_map>

#include "engine/parser/ast.hpp"

namespace engine::detail {

// "alias.col" → "real_table.col"으로 확장. 알 수 없는 접두사는 유지.
std::string expand_alias_str(const std::string& s, const std::unordered_map<std::string, std::string>& map);

ArithExpr expand_arith(const ArithExpr& expr, const std::unordered_map<std::string, std::string>& map);
SelectColumn expand_select_column(const SelectColumn& col, const std::unordered_map<std::string, std::string>& map);
Condition expand_leaf(const Condition& cond, const std::unordered_map<std::string, std::string>& map);
CondExpr expand_condexpr(const CondExpr& expr, const std::unordered_map<std::string, std::string>& map);

} // namespace engine::detail
