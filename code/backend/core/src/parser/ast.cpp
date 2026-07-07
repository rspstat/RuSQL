#include "engine/parser/ast.hpp"

namespace engine {
namespace {

template <typename T>
std::unique_ptr<T> clone_ptr(const std::unique_ptr<T>& p) {
    return p ? std::make_unique<T>(*p) : nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// ArithExpr
// ---------------------------------------------------------------------------
ArithExpr::ArithExpr(const ArithExpr& other)
    : data(std::visit(
          [](const auto& alt) -> Data {
              using T = std::decay_t<decltype(alt)>;
              if constexpr (std::is_same_v<T, Add>) return Data(Add{clone_ptr(alt.lhs), clone_ptr(alt.rhs)});
              else if constexpr (std::is_same_v<T, Sub>) return Data(Sub{clone_ptr(alt.lhs), clone_ptr(alt.rhs)});
              else if constexpr (std::is_same_v<T, Mul>) return Data(Mul{clone_ptr(alt.lhs), clone_ptr(alt.rhs)});
              else if constexpr (std::is_same_v<T, Div>) return Data(Div{clone_ptr(alt.lhs), clone_ptr(alt.rhs)});
              else if constexpr (std::is_same_v<T, Cmp>) return Data(Cmp{clone_ptr(alt.lhs), alt.op, clone_ptr(alt.rhs)});
              else return Data(alt); // Col/Num/Str copy trivially; Func's vector<ArithExpr> recurses via this ctor
          },
          other.data)) {}

ArithExpr& ArithExpr::operator=(const ArithExpr& other) {
    if (this != &other) {
        ArithExpr tmp(other);
        *this = std::move(tmp);
    }
    return *this;
}

// ---------------------------------------------------------------------------
// ConditionValue
// ---------------------------------------------------------------------------
ConditionValue::ConditionValue(const ConditionValue& other)
    : data(std::visit(
          [](const auto& alt) -> Data {
              using T = std::decay_t<decltype(alt)>;
              if constexpr (std::is_same_v<T, Subquery>) return Data(Subquery{clone_ptr(alt.query)});
              else return Data(alt);
          },
          other.data)) {}

ConditionValue& ConditionValue::operator=(const ConditionValue& other) {
    if (this != &other) {
        ConditionValue tmp(other);
        *this = std::move(tmp);
    }
    return *this;
}

// ---------------------------------------------------------------------------
// CondExpr
// ---------------------------------------------------------------------------
CondExpr::CondExpr(const CondExpr& other)
    : data(std::visit(
          [](const auto& alt) -> Data {
              using T = std::decay_t<decltype(alt)>;
              if constexpr (std::is_same_v<T, And>) return Data(And{clone_ptr(alt.lhs), clone_ptr(alt.rhs)});
              else if constexpr (std::is_same_v<T, Or>) return Data(Or{clone_ptr(alt.lhs), clone_ptr(alt.rhs)});
              else if constexpr (std::is_same_v<T, Not>) return Data(Not{clone_ptr(alt.inner)});
              else return Data(alt); // Leaf(Condition) copies via Condition's (compiler-generated) copy ctor
          },
          other.data)) {}

CondExpr& CondExpr::operator=(const CondExpr& other) {
    if (this != &other) {
        CondExpr tmp(other);
        *this = std::move(tmp);
    }
    return *this;
}

// ---------------------------------------------------------------------------
// SelectColumn
// ---------------------------------------------------------------------------
SelectColumn::SelectColumn(const SelectColumn& other)
    : data(std::visit(
          [](const auto& alt) -> Data {
              using T = std::decay_t<decltype(alt)>;
              if constexpr (std::is_same_v<T, Subquery>) return Data(Subquery{clone_ptr(alt.query), alt.alias});
              else return Data(alt);
          },
          other.data)) {}

SelectColumn& SelectColumn::operator=(const SelectColumn& other) {
    if (this != &other) {
        SelectColumn tmp(other);
        *this = std::move(tmp);
    }
    return *this;
}

// ---------------------------------------------------------------------------
// Statement
// ---------------------------------------------------------------------------
namespace {

std::optional<std::pair<StatementPtr, std::string>> clone_subquery_pair(
    const std::optional<std::pair<StatementPtr, std::string>>& p) {
    if (!p) return std::nullopt;
    return std::make_pair(clone_ptr(p->first), p->second);
}

std::vector<std::pair<std::string, StatementPtr>> clone_cte_vec(
    const std::vector<std::pair<std::string, StatementPtr>>& ctes) {
    std::vector<std::pair<std::string, StatementPtr>> out;
    out.reserve(ctes.size());
    for (const auto& [name, stmt] : ctes) out.emplace_back(name, clone_ptr(stmt));
    return out;
}

} // namespace

Statement::Statement(const Statement& other)
    : data(std::visit(
          [](const auto& alt) -> Data {
              using T = std::decay_t<decltype(alt)>;
              if constexpr (std::is_same_v<T, InsertSelect>) {
                  return Data(InsertSelect{alt.table, alt.columns, clone_ptr(alt.query), alt.on_conflict, alt.returning});
              } else if constexpr (std::is_same_v<T, Select>) {
                  return Data(Select{alt.table, clone_subquery_pair(alt.subquery), alt.columns, alt.distinct,
                                      alt.condition, alt.joins, alt.order_by, alt.group_by, alt.having, alt.limit,
                                      alt.offset, alt.for_update, alt.for_share});
              } else if constexpr (std::is_same_v<T, CreateView>) {
                  return Data(CreateView{alt.name, clone_ptr(alt.query), alt.raw_sql});
              } else if constexpr (std::is_same_v<T, Explain>) {
                  return Data(Explain{clone_ptr(alt.inner)});
              } else if constexpr (std::is_same_v<T, ExplainAnalyze>) {
                  return Data(ExplainAnalyze{clone_ptr(alt.inner)});
              } else if constexpr (std::is_same_v<T, With>) {
                  return Data(With{clone_cte_vec(alt.ctes), clone_ptr(alt.query), alt.recursive});
              } else if constexpr (std::is_same_v<T, Union>) {
                  return Data(Union{clone_ptr(alt.left), clone_ptr(alt.right), alt.all, alt.order_by, alt.limit, alt.offset});
              } else if constexpr (std::is_same_v<T, Intersect>) {
                  return Data(Intersect{clone_ptr(alt.left), clone_ptr(alt.right), alt.all, alt.order_by, alt.limit, alt.offset});
              } else if constexpr (std::is_same_v<T, Except>) {
                  return Data(Except{clone_ptr(alt.left), clone_ptr(alt.right), alt.all, alt.order_by, alt.limit, alt.offset});
              } else {
                  // Remaining ~71 variants contain no direct unique_ptr<Statement> field (Vec<Statement>
                  // body fields copy fine via std::vector's own copy ctor, which recurses into this one).
                  return Data(alt);
              }
          },
          other.data)) {}

Statement& Statement::operator=(const Statement& other) {
    if (this != &other) {
        Statement tmp(other);
        *this = std::move(tmp);
    }
    return *this;
}

} // namespace engine
