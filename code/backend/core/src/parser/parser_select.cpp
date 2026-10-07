#include <unordered_map>
#include <unordered_set>

#include "engine/parser/parser.hpp"
#include "parser_detail.hpp"

namespace engine {

// Whether the select item that starts at the current token -- a function call, an aggregate, a CAST -- goes on as an expression: an operator outside
// every parenthesis before the item ends (`ROUND(x) * 100`, `SUM(a) / COUNT(*)`, `UPPER(s) || '!'`, `LENGTH(s) > 3`).
bool Parser::select_item_continues() const {
    int depth = 0; // parentheses and CASE ... END
    for (std::size_t i = pos_; i < tokens_.size(); i++) {
        switch (tokens_[i].kind) {
            case TokenKind::LParen: case TokenKind::Case: depth++; break;
            case TokenKind::End: if (depth > 0) depth--; break;
            case TokenKind::RParen:
                if (depth == 0) return false; // the end of the select that holds this one
                depth--;
                break;
            case TokenKind::Comma: case TokenKind::From: case TokenKind::As: case TokenKind::Semicolon: case TokenKind::Into:
            case TokenKind::Where: case TokenKind::Group: case TokenKind::Having: case TokenKind::Order: case TokenKind::Limit:
            case TokenKind::Union: case TokenKind::Intersect: case TokenKind::Except: case TokenKind::Offset: case TokenKind::For:
            case TokenKind::Fetch: case TokenKind::Over: case TokenKind::Filter:
                if (depth == 0) return false;
                break;
            case TokenKind::Plus: case TokenKind::Minus: case TokenKind::Asterisk: case TokenKind::Slash: case TokenKind::Percent:
            case TokenKind::PipePipe: case TokenKind::Arrow: case TokenKind::LongArrow: case TokenKind::Eq: case TokenKind::Ne:
            case TokenKind::Gt: case TokenKind::Lt: case TokenKind::Gte: case TokenKind::Lte: case TokenKind::Is: case TokenKind::Between:
            case TokenKind::In: case TokenKind::Like: case TokenKind::Regexp: case TokenKind::And: case TokenKind::Or: case TokenKind::Not:
                if (depth == 0) return true;
                break;
            default:
                break;
        }
    }
    return false;
}

std::string Parser::parse_sort_item() {
    // a column the way it has always been read (a word that is also a keyword -- `date`, `year`, `count` -- is a column here), unless
    // something goes on after it: then the whole item is an expression
    const std::size_t start = pos_;
    try {
        std::string name = expect_col_ref();
        if (!peek_is(TokenKind::LParen) && !at_value_continuation()) return name;
    } catch (const ParseError&) {
    }
    pos_ = start;
    ArithExpr e = parse_value_expr();
    if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) return col->name;
    if (auto* num = std::get_if<ArithExpr::Num>(&e.data); num && !num->value.empty() &&
                                                           num->value.find_first_not_of("0123456789") == std::string::npos) {
        return num->value; // a position in the select list
    }
    return aggregate_argument_text(e);
}

std::optional<CondExpr> Parser::parse_optional_filter_clause() {
    if (!peek_is(TokenKind::Filter)) return std::nullopt;
    advance();
    if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after FILTER");
    advance();
    if (!peek_is(TokenKind::Where)) throw ParseError("Expected WHERE after FILTER(");
    advance();
    CondExpr cond = parse_condexpr();
    if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after FILTER (WHERE ...)");
    advance();
    return cond;
}

std::unordered_set<std::string> Parser::scan_from_names() const {
    std::unordered_set<std::string> names;
    int depth = 0;
    std::size_t i = pos_;
    for (; i < tokens_.size(); i++) { // (the FROM of this query: not one of a subquery in the select list)
        const TokenKind k = tokens_[i].kind;
        if (k == TokenKind::LParen) depth++;
        else if (k == TokenKind::RParen) {
            if (depth == 0) return names;
            depth--;
        } else if (depth == 0 && k == TokenKind::From) break;
        else if (depth == 0 && k == TokenKind::Semicolon) return names;
    }
    bool table_next = true; // the next identifier names a table
    for (i++; i < tokens_.size(); i++) {
        const TokenKind k = tokens_[i].kind;
        if (k == TokenKind::LParen) { depth++; table_next = false; continue; }
        if (k == TokenKind::RParen) {
            if (depth == 0) break;
            depth--;
            continue;
        }
        if (depth > 0) continue;
        if (k == TokenKind::Where || k == TokenKind::Group || k == TokenKind::Order || k == TokenKind::Having || k == TokenKind::Limit ||
            k == TokenKind::Union || k == TokenKind::Intersect || k == TokenKind::Except || k == TokenKind::Semicolon ||
            k == TokenKind::Offset || k == TokenKind::Fetch || k == TokenKind::For) {
            break;
        }
        if (k == TokenKind::Join || k == TokenKind::Comma) { table_next = true; continue; }
        if (k == TokenKind::On || k == TokenKind::Using) { table_next = false; continue; }
        if (k != TokenKind::Ident || !table_next) continue;
        std::size_t last = i; // (`db.table`: the table is the last part)
        while (last + 2 < tokens_.size() && tokens_[last + 1].kind == TokenKind::Dot && tokens_[last + 2].kind == TokenKind::Ident) last += 2;
        names.insert(tokens_[last].text);
        std::size_t alias = last + 1;
        if (alias < tokens_.size() && tokens_[alias].kind == TokenKind::As) alias++;
        if (alias < tokens_.size() && tokens_[alias].kind == TokenKind::Ident) {
            names.insert(tokens_[alias].text);
            last = alias;
        }
        i = last;
        table_next = false;
    }
    return names;
}

Statement Parser::parse_select() {
    // The FROM list of this query is the enclosing one for the subqueries in it (until the set operator's other side is parsed).
    struct Enclosing {
        std::vector<std::unordered_set<std::string>>* stack;
        explicit Enclosing(std::vector<std::unordered_set<std::string>>& s, std::unordered_set<std::string> names) : stack(&s) { s.push_back(std::move(names)); }
        void release() {
            if (stack) stack->pop_back();
            stack = nullptr;
        }
        ~Enclosing() { release(); }
    } enclosing(enclosing_from_, scan_from_names());
    // a table that a query around this one has in its FROM list
    auto shadowed = [this](const std::string& table) {
        for (std::size_t i = 0; i + 1 < enclosing_from_.size(); i++) {
            if (enclosing_from_[i].count(table)) return true;
        }
        return false;
    };

    // DISTINCT
    bool distinct = false;
    if (peek_is(TokenKind::Distinct)) { advance(); distinct = true; }

    // 컬럼 목록 (AS 별칭 포함)
    std::vector<SelectColumn> columns;
    for (;;) {
        SelectColumn col = [&]() -> SelectColumn {
            const Token* p = peek();

            if (p && p->kind == TokenKind::Asterisk) { advance(); return SelectColumn(SelectColumn::All{}); }

            // `table.*`
            if (p && p->kind == TokenKind::Ident && peek_at_is(1, TokenKind::Dot) && peek_at_is(2, TokenKind::Asterisk)) {
                std::string qualifier = p->text;
                advance();
                advance();
                advance();
                return SelectColumn(SelectColumn::All{std::move(qualifier)});
            }

            // a scalar function, CASE, IF and CAST are expressions (with whatever goes on after them); an aggregate is one when it goes on
            if (p) {
                const bool call = peek_at_is(1, TokenKind::LParen); // (a word that names a function is a column without its parentheses)
                const bool aggregate = call && (p->kind == TokenKind::Count || p->kind == TokenKind::Sum || p->kind == TokenKind::Avg ||
                                       p->kind == TokenKind::Min || p->kind == TokenKind::Max || p->kind == TokenKind::Stddev ||
                                       p->kind == TokenKind::Variance || p->kind == TokenKind::BitAnd || p->kind == TokenKind::BitOr ||
                                       p->kind == TokenKind::JsonAgg || p->kind == TokenKind::ArrayAgg || p->kind == TokenKind::Median ||
                                       p->kind == TokenKind::GroupConcat);
                const bool function = (call || is_bare_function(*p)) && (p->kind == TokenKind::DateAdd || p->kind == TokenKind::DateSub || p->kind == TokenKind::Upper ||
                                      p->kind == TokenKind::Lower || p->kind == TokenKind::Length || p->kind == TokenKind::Trim ||
                                      p->kind == TokenKind::Concat || p->kind == TokenKind::Substr || p->kind == TokenKind::Substring ||
                                      p->kind == TokenKind::Now || p->kind == TokenKind::Curdate || p->kind == TokenKind::DateFormat ||
                                      p->kind == TokenKind::Coalesce || p->kind == TokenKind::Ifnull || p->kind == TokenKind::Replace ||
                                      p->kind == TokenKind::Round || p->kind == TokenKind::Abs || p->kind == TokenKind::Ceil ||
                                      p->kind == TokenKind::Floor || p->kind == TokenKind::Mod || p->kind == TokenKind::Nullif ||
                                      p->kind == TokenKind::Lpad || p->kind == TokenKind::Rpad || p->kind == TokenKind::DateDiff ||
                                      ((p->kind == TokenKind::Database || p->kind == TokenKind::User) && call));
                const bool always = p->kind == TokenKind::Case || (p->kind == TokenKind::If && peek_at_is(1, TokenKind::LParen)) ||
                                    (p->kind == TokenKind::Cast && peek_at_is(1, TokenKind::LParen));
                if (always || function || (aggregate && select_item_continues())) {
                    ArithExpr expr = parse_value_expr();
                    std::optional<std::string> alias;
                    if (peek_is(TokenKind::As)) { advance(); alias = expect_alias_ident(); }
                    return SelectColumn(SelectColumn::Expr{std::move(expr), alias});
                }
            }

            if (p && peek_at_is(1, TokenKind::LParen) &&
                (p->kind == TokenKind::Count || p->kind == TokenKind::Sum || p->kind == TokenKind::Avg ||
                 p->kind == TokenKind::Min || p->kind == TokenKind::Max ||
                 p->kind == TokenKind::Stddev || p->kind == TokenKind::Variance ||
                 p->kind == TokenKind::BitAnd || p->kind == TokenKind::BitOr ||
                 p->kind == TokenKind::JsonAgg || p->kind == TokenKind::ArrayAgg ||
                 p->kind == TokenKind::Median)) {
                const Token* ft = advance();
                AggFunc func = [&]() -> AggFunc {
                    switch (ft->kind) {
                        case TokenKind::Count: return AggFunc(AggFunc::Count{});
                        case TokenKind::Sum: return AggFunc(AggFunc::Sum{});
                        case TokenKind::Avg: return AggFunc(AggFunc::Avg{});
                        case TokenKind::Min: return AggFunc(AggFunc::Min{});
                        case TokenKind::Max: return AggFunc(AggFunc::Max{});
                        case TokenKind::Stddev: return AggFunc(AggFunc::Stddev{});
                        case TokenKind::Variance: return AggFunc(AggFunc::Variance{});
                        case TokenKind::BitAnd: return AggFunc(AggFunc::BitAnd{});
                        case TokenKind::BitOr: return AggFunc(AggFunc::BitOr{});
                        case TokenKind::ArrayAgg: return AggFunc(AggFunc::ArrayAgg{});
                        case TokenKind::Median: return AggFunc(AggFunc::Median{});
                        default: return AggFunc(AggFunc::JsonAgg{});
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
                std::string agg_col;
                if (peek_is(TokenKind::Asterisk)) { advance(); agg_col = "*"; }
                else {
                    // a column (its qualifier stays: with `a.id` and `b.id` the bare name cannot say which table's column is meant) or any
                    // expression -- `price * qty`, `COALESCE(x, 0)`, `1`, `CASE WHEN .. END`, `v > 5` --, kept as the text of the expression:
                    // the executor computes it for every row under that text
                    agg_col = aggregate_argument_text(parse_value_expr());
                }
                if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')'");
                advance();
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
                // FILTER (WHERE ...) -- only meaningful for the plain aggregate case
                // reached here (OVER and the expression case above both already
                // returned); deliberately not supported combined with OVER (aggregate
                // window functions), out of scope for this addition.
                std::optional<CondExpr> filter_cond = parse_optional_filter_clause();
                // AS 별칭
                if (peek_is(TokenKind::As)) {
                    advance();
                    std::string alias = expect_alias_ident();
                    return SelectColumn(SelectColumn::AggAlias{func, agg_col, alias, filter_cond});
                }
                return SelectColumn(SelectColumn::Agg{func, agg_col, filter_cond});
            }

            // GROUP_CONCAT(col [SEPARATOR 'sep'])
            if (p && p->kind == TokenKind::GroupConcat && peek_at_is(1, TokenKind::LParen)) {
                advance();
                if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after GROUP_CONCAT");
                advance();
                const std::string agg_col = aggregate_argument_text(parse_arith_expr());
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
                std::optional<CondExpr> filter_cond = parse_optional_filter_clause();
                if (peek_is(TokenKind::As)) {
                    advance();
                    std::string alias = expect_alias_ident();
                    return SelectColumn(SelectColumn::AggAlias{func, agg_col, alias, filter_cond});
                }
                return SelectColumn(SelectColumn::Agg{func, agg_col, filter_cond});
            }

            // 윈도우 함수
            if (p && peek_at_is(1, TokenKind::LParen) && (p->kind == TokenKind::RowNumber || p->kind == TokenKind::Rank || p->kind == TokenKind::DenseRank ||
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

            // 스칼라 서브쿼리: (SELECT ...) [AS alias]
            if (p && p->kind == TokenKind::LParen && peek_at_is(1, TokenKind::Select)) {
                const std::size_t start = pos_;
                advance(); // consume (
                advance(); // consume SELECT
                Statement inner = parse_select();
                if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after scalar subquery");
                advance();
                if (!at_value_continuation()) {
                    std::optional<std::string> alias;
                    if (peek_is(TokenKind::As)) { advance(); alias = expect_alias_ident(); }
                    return SelectColumn(SelectColumn::Subquery{std::make_unique<Statement>(std::move(inner)), alias});
                }
                pos_ = start; // `(SELECT ...) + 1`, `(SELECT ...) > 3`: an expression (read below)
            }

            // default: arithmetic expression, possibly Column/ColumnAlias/Expr/Cmp
            {
                ArithExpr expr = parse_value_expr();
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

    // SELECT ... INTO var [, var] (a procedure's variables, or @user variables)
    std::vector<std::string> into_vars;
    if (peek_is(TokenKind::Into)) {
        advance();
        for (;;) {
            if (peek_is(TokenKind::At)) {
                advance();
                into_vars.push_back("@" + expect_ident());
            } else {
                into_vars.push_back(expect_ident());
            }
            if (peek_is(TokenKind::Comma)) advance(); else break;
        }
    }
    auto with_into = [&into_vars](Statement select) {
        if (into_vars.empty()) return select;
        return Statement(Statement::SelectInto{std::make_unique<Statement>(std::move(select)), into_vars});
    };

    // FROM is optional: scalar SELECT (no FROM) is supported
    if (!peek_is(TokenKind::From)) {
        return with_into(Statement(Statement::Select{
            "_dual_", std::nullopt, std::move(columns), distinct, std::nullopt, {}, {}, std::nullopt,
            std::nullopt, std::nullopt, std::nullopt, false, false}));
    }
    advance(); // consume FROM

    std::unordered_map<std::string, std::string> alias_map;
    // The name each table of the FROM list is known by (its alias, else its own name) is unique within the list, and a table used
    // a second time (a self-join, a lookup table joined twice) needs an alias to keep the uses apart.
    std::unordered_set<std::string> used_names;
    auto note_name = [&used_names](const std::string& name) {
        if (!used_names.insert(name).second) throw ParseError("Not unique table/alias: '" + name + "'");
    };
    auto bare_table = [](const std::string& t) { return t.substr(t.rfind('.') == std::string::npos ? 0 : t.rfind('.') + 1); };
    std::unordered_set<std::string> used_tables; // by their own (bare) names

    std::string table, table_alias;
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
        note_name(alias);
        table = "";
        subquery = std::make_pair(std::make_unique<Statement>(std::move(inner)), alias);
    } else {
        table = expect_col_ref();
        std::optional<std::string> a = parse_table_alias();
        note_name(a ? *a : bare_table(table));
        used_tables.insert(bare_table(table));
        if (a && shadowed(bare_table(table))) {
            table_alias = *a;
            alias_map[*a] = *a;
        } else if (a) {
            alias_map[*a] = table;
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
        } else if (peek_is(TokenKind::Comma)) {
            advance(); // `FROM a, b WHERE ...` is a cross join
            jt = JoinType::Cross;
        } else {
            break;
        }

        // LATERAL JOIN -- Rust 원본에 없음. 서브쿼리가 앞쪽 FROM/JOIN 테이블의 컬럼을 참조할 수
        // 있고, 바깥 행마다 재평가된다(일반 FROM 서브쿼리는 한 번만 평가되는 고정 임시 테이블).
        bool lateral = false;
        if (peek_is(TokenKind::Lateral)) { advance(); lateral = true; }

        std::string join_table;
        std::string join_alias; // only for a table that is used again
        std::optional<std::pair<StatementPtr, std::string>> join_subquery;
        if (lateral && *jt != JoinType::Inner && *jt != JoinType::Left && *jt != JoinType::Cross)
            throw ParseError("LATERAL is only supported with INNER/LEFT/CROSS JOIN");
        if (lateral || peek_is(TokenKind::LParen)) {
            // a derived table: `(SELECT ...) [AS] alias` is evaluated once, and LATERAL once for each row of the tables on its left
            const char* what = lateral ? " LATERAL" : "";
            if (!peek_is(TokenKind::LParen)) throw ParseError(std::string("Expected '(' after") + what);
            advance();
            if (!peek_is(TokenKind::Select)) throw ParseError(lateral ? "Expected SELECT in LATERAL subquery" : "Expected SELECT in subquery");
            advance();
            Statement inner = parse_select();
            if (!peek_is(TokenKind::RParen)) throw ParseError(lateral ? "Expected ')' after LATERAL subquery" : "Expected ')' after subquery");
            advance();
            if (peek_is(TokenKind::As)) advance();
            join_table = expect_ident(); // 별칭 필수 (파생 테이블은 반드시 별칭 필요)
            note_name(join_table);
            join_subquery = std::make_pair(std::make_unique<Statement>(std::move(inner)), join_table);
        } else {
            join_table = expect_ident();
            std::optional<std::string> a = parse_table_alias();
            if (used_tables.count(join_table)) {
                // a second use of the table: the alias is what its columns are called in the joined rows (`m.name`), so it is
                // kept as it is instead of being replaced by the table name, which the first use is known by
                if (!a) throw ParseError("Not unique table/alias: '" + join_table + "'");
                join_alias = *a;
                alias_map[*a] = *a;
            } else if (a && shadowed(join_table)) {
                join_alias = *a;
                alias_map[*a] = *a;
            } else if (a) {
                alias_map[*a] = join_table;
            }
            note_name(a ? *a : join_table);
            used_tables.insert(join_table);
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
        joins.push_back(Join{join_table, std::move(on_expr), *jt, using_cols, std::move(join_subquery), lateral, std::move(join_alias)});
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
        cols.push_back(parse_sort_item());
        while (peek_is(TokenKind::Comma)) { advance(); cols.push_back(parse_sort_item()); }
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
            std::string col = parse_sort_item();
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

    Statement select_stmt = with_into(Statement(Statement::Select{
        table, std::move(subquery), columns, distinct, condition, joins, order_by, group_by,
        having, limit, offset, for_update, for_share, false, table_alias, {alias_map.begin(), alias_map.end()}}));

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
        enclosing.release(); // (the other side of a set operator is not inside this query)
        Statement right = parse_select();

        Statement right_clean = [&]() -> Statement {
            if (std::holds_alternative<Statement::Select>(right.data)) {
                auto& s = std::get<Statement::Select>(right.data);
                return Statement(Statement::Select{
                    s.table, std::move(s.subquery), s.columns, s.distinct, s.condition, s.joins,
                    {}, s.group_by, s.having, std::nullopt, std::nullopt, s.for_update, s.for_share, false, s.table_alias, s.table_aliases});
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
