#include <unordered_map>

#include "engine/parser/parser.hpp"
#include "parser_detail.hpp"

namespace engine {

namespace {
std::string agg_display_string(const AggFunc& func, const std::string& col) {
    return std::visit(
        [&col](const auto& alt) -> std::string {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, AggFunc::Count>) return "COUNT(" + col + ")";
            else if constexpr (std::is_same_v<T, AggFunc::CountDistinct>) return "COUNT(DISTINCT " + col + ")";
            else if constexpr (std::is_same_v<T, AggFunc::Sum>) return "SUM(" + col + ")";
            else if constexpr (std::is_same_v<T, AggFunc::SumDistinct>) return "SUM(DISTINCT " + col + ")";
            else if constexpr (std::is_same_v<T, AggFunc::Avg>) return "AVG(" + col + ")";
            else if constexpr (std::is_same_v<T, AggFunc::AvgDistinct>) return "AVG(DISTINCT " + col + ")";
            else if constexpr (std::is_same_v<T, AggFunc::Min>) return "MIN(" + col + ")";
            else if constexpr (std::is_same_v<T, AggFunc::Max>) return "MAX(" + col + ")";
            else return "AGG(" + col + ")";
        },
        func.data);
}

bool is_arith_continuation(TokenKind k) {
    return k == TokenKind::Plus || k == TokenKind::Minus || k == TokenKind::Asterisk || k == TokenKind::Slash;
}
} // namespace

Statement Parser::parse_select() {
    // DISTINCT
    bool distinct = false;
    if (peek_is(TokenKind::Distinct)) { advance(); distinct = true; }

    // 컬럼 목록 (AS 별칭 포함)
    std::vector<SelectColumn> columns;
    for (;;) {
        SelectColumn col = [&]() -> SelectColumn {
            const Token* p = peek();

            if (p && p->kind == TokenKind::Asterisk) { advance(); return SelectColumn(SelectColumn::All{}); }

            if (p && (p->kind == TokenKind::Count || p->kind == TokenKind::Sum || p->kind == TokenKind::Avg ||
                      p->kind == TokenKind::Min || p->kind == TokenKind::Max ||
                      p->kind == TokenKind::Stddev || p->kind == TokenKind::Variance)) {
                const Token* ft = advance();
                AggFunc func = [&]() -> AggFunc {
                    switch (ft->kind) {
                        case TokenKind::Count: return AggFunc(AggFunc::Count{});
                        case TokenKind::Sum: return AggFunc(AggFunc::Sum{});
                        case TokenKind::Avg: return AggFunc(AggFunc::Avg{});
                        case TokenKind::Min: return AggFunc(AggFunc::Min{});
                        case TokenKind::Max: return AggFunc(AggFunc::Max{});
                        case TokenKind::Stddev: return AggFunc(AggFunc::Stddev{});
                        default: return AggFunc(AggFunc::Variance{});
                    }
                }();
                if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '('");
                advance();
                if (peek_is(TokenKind::Distinct)) {
                    advance();
                    func = std::visit([](const auto& alt) -> AggFunc {
                        using T = std::decay_t<decltype(alt)>;
                        if constexpr (std::is_same_v<T, AggFunc::Count>) return AggFunc(AggFunc::CountDistinct{});
                        else if constexpr (std::is_same_v<T, AggFunc::Sum>) return AggFunc(AggFunc::SumDistinct{});
                        else if constexpr (std::is_same_v<T, AggFunc::Avg>) return AggFunc(AggFunc::AvgDistinct{});
                        else return AggFunc(alt);
                    }, func.data);
                }
                bool rparen_consumed = false;
                std::string agg_col;
                if (peek_is(TokenKind::Asterisk)) { advance(); agg_col = "*"; }
                else if (peek_is(TokenKind::Case)) {
                    advance(); // consume CASE
                    auto [branches, else_val] = parse_case_when_inner();
                    if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after CASE WHEN in aggregate");
                    advance();
                    rparen_consumed = true;
                    func = std::visit([&](const auto& alt) -> AggFunc {
                        using T = std::decay_t<decltype(alt)>;
                        if constexpr (std::is_same_v<T, AggFunc::Count> || std::is_same_v<T, AggFunc::CountDistinct>)
                            return AggFunc(AggFunc::CountCase{branches, else_val});
                        else if constexpr (std::is_same_v<T, AggFunc::Sum> || std::is_same_v<T, AggFunc::SumDistinct>)
                            return AggFunc(AggFunc::SumCase{branches, else_val});
                        else return AggFunc(alt);
                    }, func.data);
                    agg_col = "__case__";
                } else if (peek_is(TokenKind::Ident)) {
                    std::string first = advance()->text;
                    std::string col_name = first;
                    if (peek_is(TokenKind::Dot)) { advance(); col_name = expect_ident(); }
                    // SUM(col IS NULL) / SUM(col IS NOT NULL) → SumCase/CountCase
                    if (peek_is(TokenKind::Is)) {
                        advance(); // consume IS
                        bool negated = false;
                        if (peek_is(TokenKind::Not)) { advance(); negated = true; }
                        if (!peek_is(TokenKind::Null)) throw ParseError("Expected NULL after IS [NOT]");
                        advance();
                        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after IS NULL expression");
                        advance();
                        rparen_consumed = true;
                        Operator op = negated ? Operator::IsNotNull : Operator::IsNull;
                        CondExpr cond = CondExpr(CondExpr::Leaf{
                            Condition{ArithExpr(ArithExpr::Col{col_name}), op, ConditionValue(ConditionValue::Literal{""})}});
                        std::vector<CaseWhenBranch> branches;
                        branches.push_back(CaseWhenBranch{std::move(cond), "1"});
                        func = std::visit([&](const auto& alt) -> AggFunc {
                            using T = std::decay_t<decltype(alt)>;
                            if constexpr (std::is_same_v<T, AggFunc::Count> || std::is_same_v<T, AggFunc::CountDistinct>)
                                return AggFunc(AggFunc::CountCase{branches, std::optional<std::string>("0")});
                            else if constexpr (std::is_same_v<T, AggFunc::Sum> || std::is_same_v<T, AggFunc::SumDistinct>)
                                return AggFunc(AggFunc::SumCase{branches, std::optional<std::string>("0")});
                            else return AggFunc(alt);
                        }, func.data);
                    }
                    agg_col = col_name;
                } else {
                    throw ParseError("Expected column");
                }
                if (!rparen_consumed) {
                    if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')'");
                    advance();
                }
                // 집계함수 + OVER → aggregate window function
                if (peek_is(TokenKind::Over)) {
                    advance(); // consume OVER
                    if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after OVER");
                    advance();
                    std::vector<std::string> partition_by;
                    if (peek_is(TokenKind::Partition)) {
                        advance();
                        if (!peek_is(TokenKind::By)) throw ParseError("Expected BY after PARTITION");
                        advance();
                        partition_by.push_back(expect_col_ref());
                        while (peek_is(TokenKind::Comma)) { advance(); partition_by.push_back(expect_col_ref()); }
                    }
                    std::vector<OrderBy> win_order_by;
                    if (peek_is(TokenKind::Order)) {
                        advance();
                        if (!peek_is(TokenKind::By)) throw ParseError("Expected BY after ORDER");
                        advance();
                        for (;;) {
                            std::string c = expect_col_ref();
                            bool asc = true;
                            if (peek_is(TokenKind::Desc)) { advance(); asc = false; }
                            else if (peek_is(TokenKind::Asc)) { advance(); asc = true; }
                            win_order_by.push_back(OrderBy{c, asc});
                            if (peek_is(TokenKind::Comma)) advance(); else break;
                        }
                    }
                    std::optional<WindowFrame> frame = parse_window_frame();
                    if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after OVER clause");
                    advance();
                    WindowFunc win_func = std::visit([](const auto& alt) -> WindowFunc {
                        using T = std::decay_t<decltype(alt)>;
                        if constexpr (std::is_same_v<T, AggFunc::Sum> || std::is_same_v<T, AggFunc::SumDistinct>) return WindowFunc::Sum;
                        else if constexpr (std::is_same_v<T, AggFunc::Avg> || std::is_same_v<T, AggFunc::AvgDistinct>) return WindowFunc::Avg;
                        else if constexpr (std::is_same_v<T, AggFunc::Count> || std::is_same_v<T, AggFunc::CountDistinct>) return WindowFunc::Count;
                        else if constexpr (std::is_same_v<T, AggFunc::Min>) return WindowFunc::Min;
                        else if constexpr (std::is_same_v<T, AggFunc::Max>) return WindowFunc::Max;
                        else return WindowFunc::Sum;
                    }, func.data);
                    std::optional<std::string> alias;
                    if (peek_is(TokenKind::As)) { advance(); alias = expect_alias_ident(); }
                    return SelectColumn(SelectColumn::WinFunc{win_func, std::optional<std::string>(agg_col), 0,
                                                              partition_by, win_order_by, alias, frame});
                }
                if (peek() && is_arith_continuation(peek()->kind)) {
                    // COUNT(a) - COUNT(b) 등 집계 간 산술 연산
                    std::string agg_str = agg_display_string(func, agg_col);
                    ArithExpr lhs = ArithExpr(ArithExpr::Col{agg_str});
                    for (;;) {
                        if (peek_is(TokenKind::Plus)) { advance(); lhs = ArithExpr(ArithExpr::Add{std::make_unique<ArithExpr>(std::move(lhs)), std::make_unique<ArithExpr>(parse_arith_term())}); }
                        else if (peek_is(TokenKind::Minus)) { advance(); lhs = ArithExpr(ArithExpr::Sub{std::make_unique<ArithExpr>(std::move(lhs)), std::make_unique<ArithExpr>(parse_arith_term())}); }
                        else if (peek_is(TokenKind::Asterisk)) { advance(); lhs = ArithExpr(ArithExpr::Mul{std::make_unique<ArithExpr>(std::move(lhs)), std::make_unique<ArithExpr>(parse_arith_term())}); }
                        else if (peek_is(TokenKind::Slash)) { advance(); lhs = ArithExpr(ArithExpr::Div{std::make_unique<ArithExpr>(std::move(lhs)), std::make_unique<ArithExpr>(parse_arith_term())}); }
                        else break;
                    }
                    std::optional<std::string> alias;
                    if (peek_is(TokenKind::As)) { advance(); alias = expect_alias_ident(); }
                    return SelectColumn(SelectColumn::Expr{std::move(lhs), alias});
                }
                // AS 별칭
                if (peek_is(TokenKind::As)) {
                    advance();
                    std::string alias = expect_alias_ident();
                    return SelectColumn(SelectColumn::AggAlias{func, agg_col, alias});
                }
                return SelectColumn(SelectColumn::Agg{func, agg_col});
            }

            // GROUP_CONCAT(col [SEPARATOR 'sep'])
            if (p && p->kind == TokenKind::GroupConcat) {
                advance();
                if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after GROUP_CONCAT");
                advance();
                std::string agg_col;
                {
                    const Token* it = advance();
                    if (!it || it->kind != TokenKind::Ident) throw ParseError("Expected column in GROUP_CONCAT");
                    std::string first = it->text;
                    if (peek_is(TokenKind::Dot)) { advance(); agg_col = expect_ident(); }
                    else agg_col = first;
                }
                std::string separator = ",";
                if (peek_is(TokenKind::Separator)) {
                    advance();
                    const Token* st = advance();
                    if (!st || st->kind != TokenKind::StringLit) throw ParseError("Expected string after SEPARATOR");
                    separator = st->text;
                }
                if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after GROUP_CONCAT");
                advance();
                AggFunc func = AggFunc(AggFunc::GroupConcat{separator});
                if (peek_is(TokenKind::As)) {
                    advance();
                    std::string alias = expect_alias_ident();
                    return SelectColumn(SelectColumn::AggAlias{func, agg_col, alias});
                }
                return SelectColumn(SelectColumn::Agg{func, agg_col});
            }

            // CASE WHEN ... THEN ... [ELSE ...] END
            if (p && p->kind == TokenKind::Case) {
                advance();
                return parse_case_when();
            }

            // IF(cond, true_val, false_val)
            if (p && p->kind == TokenKind::If) {
                advance();
                if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after IF");
                advance();
                CondExpr cond = parse_condexpr();
                if (!peek_is(TokenKind::Comma)) throw ParseError("Expected ',' in IF()");
                advance();
                auto read_val = [this]() -> std::string {
                    const Token* t = advance();
                    if (!t) throw ParseError("Expected value in IF()");
                    switch (t->kind) {
                        case TokenKind::StringLit: case TokenKind::NumberLit: case TokenKind::Ident: return t->text;
                        case TokenKind::Null: return "NULL";
                        default: throw ParseError("Expected value in IF()");
                    }
                };
                std::string true_val = read_val();
                if (!peek_is(TokenKind::Comma)) throw ParseError("Expected ',' in IF()");
                advance();
                std::string false_val = read_val();
                if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after IF()");
                advance();
                std::optional<std::string> alias;
                if (peek_is(TokenKind::As)) { advance(); alias = expect_alias_ident(); }
                std::vector<CaseWhenBranch> branches;
                branches.push_back(CaseWhenBranch{std::move(cond), true_val});
                return SelectColumn(SelectColumn::CaseWhen{std::move(branches), std::optional<std::string>(false_val), alias});
            }

            // CAST(expr AS type)
            if (p && p->kind == TokenKind::Cast) {
                advance();
                std::vector<std::string> args = parse_cast_args();
                std::optional<std::string> alias;
                if (peek_is(TokenKind::As)) { advance(); alias = expect_alias_ident(); }
                return SelectColumn(SelectColumn::Func{"CAST", std::move(args), alias});
            }

            // DATE_ADD / DATE_SUB (date, INTERVAL n unit)
            if (p && (p->kind == TokenKind::DateAdd || p->kind == TokenKind::DateSub)) {
                std::string fname = p->kind == TokenKind::DateAdd ? "DATE_ADD" : "DATE_SUB";
                advance();
                std::vector<std::string> args = parse_date_add_args();
                std::optional<std::string> alias;
                if (peek_is(TokenKind::As)) { advance(); alias = expect_alias_ident(); }
                return SelectColumn(SelectColumn::Func{fname, std::move(args), alias});
            }

            // 윈도우 함수
            if (p && (p->kind == TokenKind::RowNumber || p->kind == TokenKind::Rank || p->kind == TokenKind::DenseRank ||
                      p->kind == TokenKind::Lag || p->kind == TokenKind::Lead || p->kind == TokenKind::FirstValue ||
                      p->kind == TokenKind::LastValue || p->kind == TokenKind::NthValue || p->kind == TokenKind::Ntile ||
                      p->kind == TokenKind::PercentRank || p->kind == TokenKind::CumeDist)) {
                const Token* ft = advance();
                WindowFunc func;
                switch (ft->kind) {
                    case TokenKind::RowNumber: func = WindowFunc::RowNumber; break;
                    case TokenKind::Rank: func = WindowFunc::Rank; break;
                    case TokenKind::DenseRank: func = WindowFunc::DenseRank; break;
                    case TokenKind::Lag: func = WindowFunc::Lag; break;
                    case TokenKind::Lead: func = WindowFunc::Lead; break;
                    case TokenKind::FirstValue: func = WindowFunc::FirstValue; break;
                    case TokenKind::LastValue: func = WindowFunc::LastValue; break;
                    case TokenKind::NthValue: func = WindowFunc::NthValue; break;
                    case TokenKind::Ntile: func = WindowFunc::Ntile; break;
                    case TokenKind::PercentRank: func = WindowFunc::PercentRank; break;
                    default: func = WindowFunc::CumeDist; break;
                }
                if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after window function");
                advance();
                std::optional<std::string> wf_col;
                std::int64_t wf_offset = 0;
                if (func == WindowFunc::Lag || func == WindowFunc::Lead) {
                    wf_col = expect_col_ref();
                    if (peek_is(TokenKind::Comma)) {
                        advance();
                        const Token* n = advance();
                        if (!n || n->kind != TokenKind::NumberLit) throw ParseError("Expected offset number in LAG/LEAD");
                        try { wf_offset = std::stoll(n->text); } catch (...) { wf_offset = 1; }
                    } else {
                        wf_offset = 1;
                    }
                } else if (func == WindowFunc::FirstValue || func == WindowFunc::LastValue) {
                    wf_col = expect_col_ref();
                    wf_offset = 0;
                } else if (func == WindowFunc::NthValue) {
                    wf_col = expect_col_ref();
                    if (!peek_is(TokenKind::Comma)) throw ParseError("Expected ',' in NTH_VALUE");
                    advance();
                    const Token* n = advance();
                    if (!n || n->kind != TokenKind::NumberLit) throw ParseError("Expected N in NTH_VALUE");
                    try { wf_offset = std::stoll(n->text); } catch (...) { wf_offset = 1; }
                } else if (func == WindowFunc::Ntile) {
                    const Token* n = advance();
                    if (!n || n->kind != TokenKind::NumberLit) throw ParseError("Expected N in NTILE");
                    try { wf_offset = std::stoll(n->text); } catch (...) { wf_offset = 1; }
                }
                if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after window function args");
                advance();
                if (!peek_is(TokenKind::Over)) throw ParseError("Expected OVER");
                advance();
                if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after OVER");
                advance();
                std::vector<std::string> partition_by;
                if (peek_is(TokenKind::Partition)) {
                    advance();
                    if (!peek_is(TokenKind::By)) throw ParseError("Expected BY after PARTITION");
                    advance();
                    partition_by.push_back(expect_col_ref());
                    while (peek_is(TokenKind::Comma)) { advance(); partition_by.push_back(expect_col_ref()); }
                }
                std::vector<OrderBy> win_order_by;
                if (peek_is(TokenKind::Order)) {
                    advance();
                    if (!peek_is(TokenKind::By)) throw ParseError("Expected BY after ORDER");
                    advance();
                    for (;;) {
                        std::string c = expect_col_ref();
                        bool asc = true;
                        if (peek_is(TokenKind::Desc)) { advance(); asc = false; }
                        else if (peek_is(TokenKind::Asc)) { advance(); asc = true; }
                        win_order_by.push_back(OrderBy{c, asc});
                        if (peek_is(TokenKind::Comma)) advance(); else break;
                    }
                }
                std::optional<WindowFrame> frame = parse_window_frame();
                if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after OVER clause");
                advance();
                std::optional<std::string> alias;
                if (peek_is(TokenKind::As)) { advance(); alias = expect_alias_ident(); }
                return SelectColumn(SelectColumn::WinFunc{func, wf_col, wf_offset, partition_by, win_order_by, alias, frame});
            }

            // 스칼라 함수: UPPER(col), NOW(), CONCAT(a, b), ...
            if (p && (p->kind == TokenKind::Upper || p->kind == TokenKind::Lower || p->kind == TokenKind::Length ||
                      p->kind == TokenKind::Trim || p->kind == TokenKind::Concat || p->kind == TokenKind::Substr ||
                      p->kind == TokenKind::Substring || p->kind == TokenKind::Now || p->kind == TokenKind::Curdate ||
                      p->kind == TokenKind::DateFormat || p->kind == TokenKind::Coalesce || p->kind == TokenKind::Ifnull ||
                      p->kind == TokenKind::Replace || p->kind == TokenKind::Round || p->kind == TokenKind::Abs ||
                      p->kind == TokenKind::Ceil || p->kind == TokenKind::Floor || p->kind == TokenKind::Mod ||
                      p->kind == TokenKind::Nullif || p->kind == TokenKind::Lpad || p->kind == TokenKind::Rpad ||
                      p->kind == TokenKind::DateDiff || p->kind == TokenKind::Database)) {
                const Token* ft = advance();
                std::string fname;
                switch (ft->kind) {
                    case TokenKind::Upper: fname = "UPPER"; break;
                    case TokenKind::Lower: fname = "LOWER"; break;
                    case TokenKind::Length: fname = "LENGTH"; break;
                    case TokenKind::Trim: fname = "TRIM"; break;
                    case TokenKind::Concat: fname = "CONCAT"; break;
                    case TokenKind::Substr: fname = "SUBSTR"; break;
                    case TokenKind::Substring: fname = "SUBSTRING"; break;
                    case TokenKind::Now: fname = "NOW"; break;
                    case TokenKind::Curdate: fname = "CURDATE"; break;
                    case TokenKind::DateFormat: fname = "DATE_FORMAT"; break;
                    case TokenKind::Coalesce: fname = "COALESCE"; break;
                    case TokenKind::Ifnull: fname = "IFNULL"; break;
                    case TokenKind::Replace: fname = "REPLACE"; break;
                    case TokenKind::Round: fname = "ROUND"; break;
                    case TokenKind::Abs: fname = "ABS"; break;
                    case TokenKind::Ceil: fname = "CEIL"; break;
                    case TokenKind::Floor: fname = "FLOOR"; break;
                    case TokenKind::Mod: fname = "MOD"; break;
                    case TokenKind::Nullif: fname = "NULLIF"; break;
                    case TokenKind::Lpad: fname = "LPAD"; break;
                    case TokenKind::Rpad: fname = "RPAD"; break;
                    case TokenKind::DateDiff: fname = "DATEDIFF"; break;
                    case TokenKind::Database: fname = "DATABASE"; break;
                    default: break;
                }
                std::vector<std::string> args = parse_func_args();
                // detect comparison after scalar func: LENGTH(x) > 0 AS alias
                std::optional<std::string> cmp_op;
                if (peek_is(TokenKind::Gt)) { advance(); cmp_op = ">"; }
                else if (peek_is(TokenKind::Lt)) { advance(); cmp_op = "<"; }
                else if (peek_is(TokenKind::Gte)) { advance(); cmp_op = ">="; }
                else if (peek_is(TokenKind::Lte)) { advance(); cmp_op = "<="; }
                else if (peek_is(TokenKind::Eq)) { advance(); cmp_op = "="; }
                else if (peek_is(TokenKind::Ne)) { advance(); cmp_op = "!="; }
                if (cmp_op) {
                    ArithExpr rhs = parse_arith_expr();
                    std::optional<std::string> alias;
                    if (peek_is(TokenKind::As)) { advance(); alias = expect_alias_ident(); }
                    std::vector<ArithExpr> fargs;
                    for (auto& a : args) fargs.push_back(str_to_arith(a));
                    ArithExpr lhs = ArithExpr(ArithExpr::Func{fname, std::move(fargs)});
                    return SelectColumn(SelectColumn::Expr{
                        ArithExpr(ArithExpr::Cmp{std::make_unique<ArithExpr>(std::move(lhs)), *cmp_op, std::make_unique<ArithExpr>(std::move(rhs))}),
                        alias});
                }
                std::optional<std::string> alias;
                if (peek_is(TokenKind::As)) { advance(); alias = expect_alias_ident(); }
                return SelectColumn(SelectColumn::Func{fname, std::move(args), alias});
            }

            // 스칼라 서브쿼리: (SELECT ...) [AS alias]
            if (p && p->kind == TokenKind::LParen && peek_at_is(1, TokenKind::Select)) {
                advance(); // consume (
                advance(); // consume SELECT
                Statement inner = parse_select();
                if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after scalar subquery");
                advance();
                std::optional<std::string> alias;
                if (peek_is(TokenKind::As)) { advance(); alias = expect_alias_ident(); }
                return SelectColumn(SelectColumn::Subquery{std::make_unique<Statement>(std::move(inner)), alias});
            }

            // default: arithmetic expression, possibly Column/ColumnAlias/Expr/Cmp
            {
                ArithExpr expr = parse_arith_expr();
                std::optional<std::string> cmp_op;
                if (peek_is(TokenKind::Gt)) { advance(); cmp_op = ">"; }
                else if (peek_is(TokenKind::Lt)) { advance(); cmp_op = "<"; }
                else if (peek_is(TokenKind::Gte)) { advance(); cmp_op = ">="; }
                else if (peek_is(TokenKind::Lte)) { advance(); cmp_op = "<="; }
                else if (peek_is(TokenKind::Eq)) { advance(); cmp_op = "="; }
                else if (peek_is(TokenKind::Ne)) { advance(); cmp_op = "!="; }
                if (cmp_op) {
                    ArithExpr rhs = parse_arith_expr();
                    std::optional<std::string> alias;
                    if (peek_is(TokenKind::As)) { advance(); alias = expect_alias_ident(); }
                    return SelectColumn(SelectColumn::Expr{
                        ArithExpr(ArithExpr::Cmp{std::make_unique<ArithExpr>(std::move(expr)), *cmp_op, std::make_unique<ArithExpr>(std::move(rhs))}),
                        alias});
                }
                std::optional<std::string> alias;
                if (peek_is(TokenKind::As)) { advance(); alias = expect_alias_ident(); }
                if (std::holds_alternative<ArithExpr::Col>(expr.data) && !alias) {
                    return SelectColumn(SelectColumn::Column{std::get<ArithExpr::Col>(expr.data).name});
                }
                if (std::holds_alternative<ArithExpr::Col>(expr.data) && alias) {
                    return SelectColumn(SelectColumn::ColumnAlias{std::get<ArithExpr::Col>(expr.data).name, *alias});
                }
                return SelectColumn(SelectColumn::Expr{std::move(expr), alias});
            }
        }();

        columns.push_back(std::move(col));
        if (peek_is(TokenKind::Comma)) advance(); else break;
    }

    // FROM is optional: scalar SELECT (no FROM) is supported
    if (!peek_is(TokenKind::From)) {
        return Statement(Statement::Select{
            "_dual_", std::nullopt, std::move(columns), distinct, std::nullopt, {}, {}, std::nullopt,
            std::nullopt, std::nullopt, std::nullopt, false, false});
    }
    advance(); // consume FROM

    std::unordered_map<std::string, std::string> alias_map;

    std::string table;
    std::optional<std::pair<StatementPtr, std::string>> subquery;
    if (peek_is(TokenKind::LParen)) {
        advance();
        if (!peek_is(TokenKind::Select)) throw ParseError("Expected SELECT in subquery");
        advance();
        Statement inner = parse_select();
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after subquery");
        advance();
        if (peek_is(TokenKind::As)) advance();
        std::string alias = expect_ident();
        table = "";
        subquery = std::make_pair(std::make_unique<Statement>(std::move(inner)), alias);
    } else {
        table = expect_col_ref();
        if (peek_is(TokenKind::Ident)) {
            std::string a = expect_ident();
            alias_map[a] = table;
        }
    }

    // JOIN / LEFT JOIN / RIGHT JOIN / CROSS JOIN / NATURAL JOIN (다중 반복)
    std::vector<Join> joins;
    for (;;) {
        std::optional<JoinType> jt;
        if (peek_is(TokenKind::Join)) { advance(); jt = JoinType::Inner; }
        else if (peek_is(TokenKind::Inner)) {
            advance();
            if (!peek_is(TokenKind::Join)) throw ParseError("Expected JOIN after INNER");
            advance();
            jt = JoinType::Inner;
        } else if (peek_is(TokenKind::Left)) {
            advance();
            if (peek_is(TokenKind::Outer)) advance();
            if (!peek_is(TokenKind::Join)) throw ParseError("Expected JOIN after LEFT");
            advance();
            jt = JoinType::Left;
        } else if (peek_is(TokenKind::Right)) {
            advance();
            if (peek_is(TokenKind::Outer)) advance();
            if (!peek_is(TokenKind::Join)) throw ParseError("Expected JOIN after RIGHT");
            advance();
            jt = JoinType::Right;
        } else if (peek_is(TokenKind::Cross)) {
            advance();
            if (!peek_is(TokenKind::Join)) throw ParseError("Expected JOIN after CROSS");
            advance();
            jt = JoinType::Cross;
        } else if (peek_is(TokenKind::Natural)) {
            advance();
            if (!peek_is(TokenKind::Join)) throw ParseError("Expected JOIN after NATURAL");
            advance();
            jt = JoinType::Natural;
        } else if (peek_is(TokenKind::Full)) {
            advance();
            if (peek_is(TokenKind::Outer)) advance();
            if (!peek_is(TokenKind::Join)) throw ParseError("Expected JOIN after FULL");
            advance();
            jt = JoinType::FullOuter;
        } else {
            break;
        }

        std::string join_table = expect_ident();
        if (peek_is(TokenKind::Ident)) {
            std::string a = expect_ident();
            alias_map[a] = join_table;
        }
        auto dummy_true = []() {
            return CondExpr(CondExpr::Leaf{
                Condition{ArithExpr(ArithExpr::Num{"1"}), Operator::Eq, ConditionValue(ConditionValue::Literal{"1"})}});
        };

        std::vector<std::string> using_cols;
        CondExpr on_expr = [&]() -> CondExpr {
            if (*jt == JoinType::Cross || *jt == JoinType::Natural) {
                return dummy_true();
            }
            if (peek_is(TokenKind::Using)) {
                advance();
                if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after USING");
                advance();
                for (;;) {
                    using_cols.push_back(expect_ident());
                    if (peek_is(TokenKind::Comma)) { advance(); }
                    else if (peek_is(TokenKind::RParen)) { advance(); break; }
                    else throw ParseError("Expected ',' or ')' in USING");
                }
                return dummy_true();
            }
            if (!peek_is(TokenKind::On)) throw ParseError("Expected ON or USING");
            advance();
            return parse_condexpr();
        }();
        joins.push_back(Join{join_table, std::move(on_expr), *jt, using_cols});
    }

    // WHERE
    std::optional<CondExpr> condition;
    if (peek_is(TokenKind::Where)) { advance(); condition = parse_condexpr(); }

    // GROUP BY
    std::optional<std::vector<std::string>> group_by;
    if (peek_is(TokenKind::Group)) {
        advance();
        if (!peek_is(TokenKind::By)) throw ParseError("Expected BY");
        advance();
        std::vector<std::string> cols;
        cols.push_back(expect_col_ref());
        while (peek_is(TokenKind::Comma)) { advance(); cols.push_back(expect_col_ref()); }
        group_by = cols;
    }

    // HAVING
    std::optional<CondExpr> having;
    if (peek_is(TokenKind::Having)) { advance(); having = parse_condexpr(); }

    // ORDER BY
    std::vector<OrderBy> order_by;
    if (peek_is(TokenKind::Order)) {
        advance();
        if (!peek_is(TokenKind::By)) throw ParseError("Expected BY");
        advance();
        for (;;) {
            std::string col = expect_col_ref();
            bool asc = true;
            if (peek_is(TokenKind::Desc)) { advance(); asc = false; }
            else if (peek_is(TokenKind::Asc)) { advance(); asc = true; }
            order_by.push_back(OrderBy{col, asc});
            if (peek_is(TokenKind::Comma)) advance(); else break;
        }
    }

    // LIMIT [OFFSET] / FETCH (FIRST|NEXT) n ROWS ONLY
    std::optional<std::size_t> limit, offset;
    if (peek_is(TokenKind::Limit)) {
        advance();
        const Token* n = advance();
        if (!n || n->kind != TokenKind::NumberLit) throw ParseError("Expected number after LIMIT");
        std::size_t first = 0;
        try { first = static_cast<std::size_t>(std::stoull(n->text)); } catch (...) {}
        if (peek_is(TokenKind::Comma)) {
            advance();
            const Token* cnt = advance();
            if (!cnt || cnt->kind != TokenKind::NumberLit) throw ParseError("Expected count after LIMIT offset,");
            std::size_t count = 0;
            try { count = static_cast<std::size_t>(std::stoull(cnt->text)); } catch (...) {}
            limit = count;
            offset = first;
        } else {
            if (peek_is(TokenKind::Offset)) {
                advance();
                const Token* o = advance();
                if (!o || o->kind != TokenKind::NumberLit) throw ParseError("Expected number after OFFSET");
                std::size_t ov = 0;
                try { ov = static_cast<std::size_t>(std::stoull(o->text)); } catch (...) {}
                offset = ov;
            }
            limit = first;
        }
    } else if (peek_is(TokenKind::Fetch)) {
        advance();
        if (peek_is(TokenKind::Next) || peek_is(TokenKind::Ident)) advance(); // FIRST or NEXT
        const Token* n = advance();
        if (!n || n->kind != TokenKind::NumberLit) throw ParseError("Expected number after FETCH FIRST/NEXT");
        std::size_t lim = 0;
        try { lim = static_cast<std::size_t>(std::stoull(n->text)); } catch (...) {}
        if (peek_is(TokenKind::Rows)) advance();
        if (peek_is(TokenKind::Only)) advance();
        limit = lim;
    }

    // FOR UPDATE / FOR SHARE
    bool for_update = false, for_share = false;
    if (peek_is(TokenKind::For)) {
        advance();
        if (peek_is(TokenKind::Update)) { advance(); for_update = true; }
        else if (peek_is(TokenKind::Share)) { advance(); for_share = true; }
        else throw ParseError("Expected UPDATE or SHARE after FOR");
    }

    // 별칭 확장 적용
    for (auto& c : columns) c = detail::expand_select_column(c, alias_map);
    for (auto& j : joins) j.on_expr = detail::expand_condexpr(j.on_expr, alias_map);
    if (condition) condition = detail::expand_condexpr(*condition, alias_map);
    for (auto& o : order_by) o.column = detail::expand_alias_str(o.column, alias_map);
    if (group_by) {
        for (auto& c : *group_by) c = detail::expand_alias_str(c, alias_map);
    }
    if (having) having = detail::expand_condexpr(*having, alias_map);

    Statement select_stmt = Statement(Statement::Select{
        table, std::move(subquery), columns, distinct, condition, joins, order_by, group_by,
        having, limit, offset, for_update, for_share});

    // UNION / INTERSECT / EXCEPT [ALL]
    int set_op = 0;
    if (peek_is(TokenKind::Union)) { advance(); set_op = 1; }
    else if (peek_is(TokenKind::Intersect)) { advance(); set_op = 2; }
    else if (peek_is(TokenKind::Except)) { advance(); set_op = 3; }

    if (set_op > 0) {
        bool all = false;
        if (peek_is(TokenKind::All)) { advance(); all = true; }
        if (!peek_is(TokenKind::Select)) throw ParseError("Expected SELECT after set operator");
        advance();
        Statement right = parse_select();

        Statement right_clean = [&]() -> Statement {
            if (std::holds_alternative<Statement::Select>(right.data)) {
                auto& s = std::get<Statement::Select>(right.data);
                return Statement(Statement::Select{
                    s.table, std::move(s.subquery), s.columns, s.distinct, s.condition, s.joins,
                    {}, s.group_by, s.having, std::nullopt, std::nullopt, s.for_update, s.for_share});
            }
            return right;
        }();
        std::vector<OrderBy> op_order_by;
        std::optional<std::size_t> op_limit, op_offset;
        if (std::holds_alternative<Statement::Select>(right.data)) {
            auto& s = std::get<Statement::Select>(right.data);
            op_order_by = s.order_by;
            op_limit = s.limit;
            op_offset = s.offset;
        }

        switch (set_op) {
            case 1:
                return Statement(Statement::Union{std::make_unique<Statement>(std::move(select_stmt)),
                                                   std::make_unique<Statement>(std::move(right_clean)),
                                                   all, op_order_by, op_limit, op_offset});
            case 2:
                return Statement(Statement::Intersect{std::make_unique<Statement>(std::move(select_stmt)),
                                                        std::make_unique<Statement>(std::move(right_clean)),
                                                        all, op_order_by, op_limit, op_offset});
            default:
                return Statement(Statement::Except{std::make_unique<Statement>(std::move(select_stmt)),
                                                     std::make_unique<Statement>(std::move(right_clean)),
                                                     all, op_order_by, op_limit, op_offset});
        }
    }

    return select_stmt;
}

std::optional<std::vector<SelectColumn>> Parser::parse_returning() {
    if (!peek_is(TokenKind::Returning)) return std::nullopt;
    advance(); // consume RETURNING
    std::vector<SelectColumn> cols;
    for (;;) {
        if (peek_is(TokenKind::Asterisk)) {
            advance();
            cols.push_back(SelectColumn(SelectColumn::All{}));
        } else {
            std::string name = expect_ident();
            if (peek_is(TokenKind::As)) {
                advance();
                std::string alias = expect_alias_ident();
                cols.push_back(SelectColumn(SelectColumn::ColumnAlias{name, alias}));
            } else {
                cols.push_back(SelectColumn(SelectColumn::Column{name}));
            }
        }
        if (peek_is(TokenKind::Comma)) advance(); else break;
    }
    return cols;
}

} // namespace engine
