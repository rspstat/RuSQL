#include <algorithm>
#include <functional>
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
            case TokenKind::Fetch: case TokenKind::Filter:
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

bool Parser::is_window_function_token(TokenKind kind) {
    return kind == TokenKind::RowNumber || kind == TokenKind::Rank || kind == TokenKind::DenseRank || kind == TokenKind::Lag || kind == TokenKind::Lead ||
           kind == TokenKind::FirstValue || kind == TokenKind::LastValue || kind == TokenKind::NthValue || kind == TokenKind::Ntile ||
           kind == TokenKind::PercentRank || kind == TokenKind::CumeDist;
}

/// OVER (PARTITION BY ... ORDER BY ... frame) | OVER w, right after a window function's call; the items are columns, or expressions and aggregates of a query that groups.
void Parser::parse_over_clause(SelectColumn::WinFunc& wf) {
    if (!peek_is(TokenKind::Over)) throw ParseError("Expected OVER");
    advance();
    expand_window_name();
    if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after OVER");
    advance();
    if (peek_is(TokenKind::Partition)) {
        advance();
        if (!peek_is(TokenKind::By)) throw ParseError("Expected BY after PARTITION");
        advance();
        wf.partition_by.push_back(parse_sort_item());
        while (peek_is(TokenKind::Comma)) { advance(); wf.partition_by.push_back(parse_sort_item()); }
    }
    if (peek_is(TokenKind::Order)) {
        advance();
        if (!peek_is(TokenKind::By)) throw ParseError("Expected BY after ORDER");
        advance();
        for (;;) {
            std::string c = parse_sort_item();
            bool asc = true;
            if (peek_is(TokenKind::Desc)) { advance(); asc = false; }
            else if (peek_is(TokenKind::Asc)) { advance(); asc = true; }
            wf.order_by.push_back(OrderBy{c, asc});
            if (peek_is(TokenKind::Comma)) advance(); else break;
        }
    }
    wf.frame = parse_window_frame();
    if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after OVER clause");
    advance();
}

/// ROW_NUMBER() / RANK() / DENSE_RANK() / LAG(c [, n [, d]]) / LEAD(...) / FIRST_VALUE(c) / LAST_VALUE(c) / NTH_VALUE(c, n) / NTILE(n) / PERCENT_RANK() / CUME_DIST() OVER (...)
SelectColumn::WinFunc Parser::parse_ranking_window() {
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
    std::optional<std::string> wf_default;
    std::int64_t wf_offset = 0;
    if (func == WindowFunc::Lag || func == WindowFunc::Lead) {
        wf_col = parse_sort_item(); // (a column, or an expression / aggregate: LAG(SUM(v)))
        if (peek_is(TokenKind::Comma)) {
            advance();
            const Token* n = advance();
            if (!n || n->kind != TokenKind::NumberLit) throw ParseError("Expected offset number in LAG/LEAD");
            try { wf_offset = std::stoll(n->text); } catch (...) { wf_offset = 1; }
            if (peek_is(TokenKind::Comma)) { // LAG(c, n, default)
                advance();
                const Token* d = advance();
                if (!d) throw ParseError("Expected the default value in LAG/LEAD");
                if (d->kind == TokenKind::NumberLit || d->kind == TokenKind::StringLit) wf_default = d->text;
                else if (d->kind == TokenKind::Null) wf_default = "NULL";
                else throw ParseError("Expected a number, a string or NULL as the default value in LAG/LEAD");
            }
        } else {
            wf_offset = 1;
        }
    } else if (func == WindowFunc::FirstValue || func == WindowFunc::LastValue) {
        wf_col = parse_sort_item();
        wf_offset = 0;
    } else if (func == WindowFunc::NthValue) {
        wf_col = parse_sort_item();
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
    SelectColumn::WinFunc wf;
    wf.func = func;
    wf.col = wf_col;
    wf.offset = wf_offset;
    wf.default_value = wf_default;
    parse_over_clause(wf);
    return wf;
}

/// The call becomes a hidden window-function column of the query and its value is read as a column (see hidden_window_name).
ArithExpr Parser::hide_window(SelectColumn::WinFunc wf) {
    const std::string name = "__win" + std::to_string(next_hidden_window_++);
    wf.alias = name;
    hidden_windows_.push_back(SelectColumn(std::move(wf)));
    return ArithExpr(ArithExpr::Col{name});
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

namespace {
std::string lowered(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

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

namespace {
std::string upper_word(std::string w) {
    for (char& c : w) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return w;
}
} // namespace

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
    // the named windows of this query (WINDOW w AS (...)) are known from here on; the clause itself is out of the tokens
    window_defs_.push_back(take_window_clause());
    struct WindowScope {
        std::vector<std::map<std::string, std::vector<Token>>>* defs;
        ~WindowScope() { defs->pop_back(); }
    } window_scope{&window_defs_};
    // a table that a query around this one has in its FROM list
    auto shadowed = [this](const std::string& table) {
        for (std::size_t i = 0; i + 1 < enclosing_from_.size(); i++) {
            if (enclosing_from_[i].count(table)) return true;
            for (const std::string& name : enclosing_from_[i]) {
                if (lowered(name) == lowered(table)) return true;
            }
        }
        return false;
    };

    // DISTINCT
    bool distinct = false;
    if (peek_is(TokenKind::Distinct)) { advance(); distinct = true; }

    // the hidden window-function columns of this query (a window function inside an expression); a query inside it has its own
    struct HiddenWindows {
        Parser& p;
        std::vector<SelectColumn> saved;
        bool allowed;
        explicit HiddenWindows(Parser& parser) : p(parser), saved(std::move(parser.hidden_windows_)), allowed(parser.allow_window_expr_) {
            p.hidden_windows_.clear();
            p.allow_window_expr_ = false;
        }
        ~HiddenWindows() {
            p.hidden_windows_ = std::move(saved);
            p.allow_window_expr_ = allowed;
        }
    } hidden_windows_guard(*this);

    // 컬럼 목록 (AS 별칭 포함)
    std::vector<SelectColumn> columns;
    allow_window_expr_ = true;
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
                const bool window_call = call && is_window_function_token(p->kind);
                const bool always = p->kind == TokenKind::Case || (p->kind == TokenKind::If && peek_at_is(1, TokenKind::LParen)) ||
                                    (p->kind == TokenKind::Cast && peek_at_is(1, TokenKind::LParen));
                if (always || function || ((aggregate || window_call) && select_item_continues())) {
                    const std::size_t item_start = pos_;
                    const std::size_t hidden_before = next_hidden_window_;
                    ArithExpr expr = parse_value_expr();
                    // (an item with a window function in it is called by its text, as MySQL does)
                    const std::string typed = next_hidden_window_ > hidden_before ? source_between(item_start, pos_) : std::string();
                    std::optional<std::string> alias = parse_column_alias();
                    if (!alias && !typed.empty()) alias = typed;
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
                const std::size_t item_start = pos_;
                const Token* ft = advance();
                AggFunc func = [&]() -> AggFunc {
                    switch (ft->kind) {
                        case TokenKind::Count: return AggFunc(AggFunc::Count{});
                        case TokenKind::Sum: return AggFunc(AggFunc::Sum{});
                        case TokenKind::Avg: return AggFunc(AggFunc::Avg{});
                        case TokenKind::Min: return AggFunc(AggFunc::Min{});
                        case TokenKind::Max: return AggFunc(AggFunc::Max{});
                        case TokenKind::Stddev: return AggFunc(AggFunc::Stddev{upper_word(ft->word).find("SAMP") != std::string::npos});
                        case TokenKind::Variance: return AggFunc(AggFunc::Variance{upper_word(ft->word).find("SAMP") != std::string::npos});
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
                    // COUNT(DISTINCT a, b): the rows with a different pair of values (the executor reads the text back as one value per row)
                    if (std::holds_alternative<AggFunc::CountDistinct>(func.data)) {
                        while (peek_is(TokenKind::Comma)) {
                            advance();
                            agg_col += ", " + aggregate_argument_text(parse_value_expr());
                        }
                    }
                }
                if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')'");
                advance();
                // 집계함수 + OVER → aggregate window function
                if (peek_is(TokenKind::Over)) {
                    SelectColumn::WinFunc wf;
                    wf.col = agg_col;
                    parse_over_clause(wf);
                    wf.func = std::visit([](const auto& alt) -> WindowFunc {
                        using T = std::decay_t<decltype(alt)>;
                        if constexpr (std::is_same_v<T, AggFunc::Sum> || std::is_same_v<T, AggFunc::SumDistinct>) return WindowFunc::Sum;
                        else if constexpr (std::is_same_v<T, AggFunc::Avg> || std::is_same_v<T, AggFunc::AvgDistinct>) return WindowFunc::Avg;
                        else if constexpr (std::is_same_v<T, AggFunc::Count> || std::is_same_v<T, AggFunc::CountDistinct>) return WindowFunc::Count;
                        else if constexpr (std::is_same_v<T, AggFunc::Min>) return WindowFunc::Min;
                        else if constexpr (std::is_same_v<T, AggFunc::Max>) return WindowFunc::Max;
                        else return WindowFunc::Sum;
                    }, func.data);
                    const std::string typed = source_between(item_start, pos_); // (a window function with no alias is called by its text, as MySQL does)
                    std::optional<std::string> alias = parse_column_alias();
                    if (!alias && !typed.empty()) alias = typed;
                    wf.alias = alias;
                    return SelectColumn(std::move(wf));
                }
                // FILTER (WHERE ...) -- only meaningful for the plain aggregate case
                // reached here (OVER and the expression case above both already
                // returned); deliberately not supported combined with OVER (aggregate
                // window functions), out of scope for this addition.
                std::optional<CondExpr> filter_cond = parse_optional_filter_clause();
                // [AS] 별칭
                if (auto alias = parse_column_alias()) {
                    return SelectColumn(SelectColumn::AggAlias{func, agg_col, *alias, filter_cond});
                }
                return SelectColumn(SelectColumn::Agg{func, agg_col, filter_cond});
            }

            // GROUP_CONCAT([DISTINCT] col [ORDER BY col [ASC | DESC], ...] [SEPARATOR 'sep'])
            if (p && p->kind == TokenKind::GroupConcat && peek_at_is(1, TokenKind::LParen)) {
                const std::size_t group_concat_start = pos_;
                advance();
                if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after GROUP_CONCAT");
                advance();
                bool concat_distinct = false;
                if (peek_is(TokenKind::Distinct)) {
                    advance();
                    concat_distinct = true;
                }
                const std::string agg_col = aggregate_argument_text(parse_arith_expr());
                std::vector<std::pair<std::string, bool>> concat_order;
                if (peek_is(TokenKind::Order)) {
                    advance();
                    if (!peek_is(TokenKind::By)) throw ParseError("Expected BY after ORDER in GROUP_CONCAT");
                    advance();
                    for (;;) {
                        std::string key = aggregate_argument_text(parse_arith_expr());
                        bool ascending = true;
                        if (peek_is(TokenKind::Desc)) { advance(); ascending = false; }
                        else if (peek_is(TokenKind::Asc)) advance();
                        concat_order.emplace_back(std::move(key), ascending);
                        if (peek_is(TokenKind::Comma)) { advance(); continue; }
                        break;
                    }
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
                AggFunc func = AggFunc(AggFunc::GroupConcat{separator, concat_distinct, std::move(concat_order)});
                std::optional<CondExpr> filter_cond = parse_optional_filter_clause();
                const std::string typed = source_between(group_concat_start, pos_); // (called by its text, as MySQL does: two GROUP_CONCATs of a column differ)
                if (auto alias = parse_column_alias()) {
                    return SelectColumn(SelectColumn::AggAlias{func, agg_col, *alias, filter_cond});
                }
                if (!typed.empty() && (!std::get<AggFunc::GroupConcat>(func.data).order_by.empty() || std::get<AggFunc::GroupConcat>(func.data).separator != ",")) {
                    return SelectColumn(SelectColumn::AggAlias{func, agg_col, typed, filter_cond});
                }
                return SelectColumn(SelectColumn::Agg{func, agg_col, filter_cond});
            }

            // 윈도우 함수
            if (p && peek_at_is(1, TokenKind::LParen) && is_window_function_token(p->kind) && !select_item_continues()) {
                const std::size_t item_start = pos_;
                SelectColumn::WinFunc wf = parse_ranking_window();
                const std::string typed = source_between(item_start, pos_); // (a window function with no alias is called by its text, as MySQL does)
                std::optional<std::string> alias = parse_column_alias();
                if (!alias && !typed.empty()) alias = typed;
                wf.alias = alias;
                return SelectColumn(std::move(wf));
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
                    std::optional<std::string> alias = parse_column_alias();
                    return SelectColumn(SelectColumn::Subquery{std::make_unique<Statement>(std::move(inner)), alias});
                }
                pos_ = start; // `(SELECT ...) + 1`, `(SELECT ...) > 3`: an expression (read below)
            }

            // default: arithmetic expression, possibly Column/ColumnAlias/Expr/Cmp
            {
                const std::size_t item_start = pos_;
                const std::size_t hidden_before = next_hidden_window_;
                ArithExpr expr = parse_value_expr();
                const std::string typed = next_hidden_window_ > hidden_before ? source_between(item_start, pos_) : std::string();
                std::optional<std::string> alias = parse_column_alias();
                if (!alias && !typed.empty()) alias = typed;
                // a string constant on its own names its column by what it says (`SELECT 'a'` has the column a, not 'a'), as MySQL does
                if (!alias) {
                    if (auto* constant = std::get_if<ArithExpr::Str>(&expr.data); constant && !constant->value.empty()) alias = constant->value;
                }
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
    allow_window_expr_ = false;
    for (auto& hidden : hidden_windows_) columns.push_back(std::move(hidden));
    hidden_windows_.clear();

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

    // UNION / INTERSECT / EXCEPT [ALL] after a select (with or without FROM)
    auto set_operation = [&](Statement select_stmt) -> Statement {
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
    };

    // FROM is optional: scalar SELECT (no FROM) is supported
    if (!peek_is(TokenKind::From)) {
        // `SELECT 1 WHERE 1 = 0`: the one row a select without FROM has is there, or it is not
        std::optional<CondExpr> dual_condition;
        if (peek_is(TokenKind::Where)) { advance(); dual_condition = parse_condexpr(); }
        return set_operation(with_into(Statement(Statement::Select{
            "_dual_", std::nullopt, std::move(columns), distinct, std::move(dual_condition), {}, {}, std::nullopt,
            std::nullopt, std::nullopt, std::nullopt, false, false})));
    }
    advance(); // consume FROM

    // `FROM ((a x JOIN b y ON (...)) JOIN c z ON (...))`: the parentheses MySQL writes around the left-deep join groups of a view's query (and a
    // dump holds that text) change nothing; the '(' that begin the FROM list and their ')' are dropped. (`FROM (SELECT ...) d` is a derived table.)
    {
        std::size_t lead = 0;
        while (peek_at_is(lead, TokenKind::LParen)) lead++;
        if (lead > 0 && !peek_at_is(lead, TokenKind::Select)) {
            std::vector<std::size_t> closers;
            std::vector<std::size_t> open;
            for (std::size_t i = pos_; i < tokens_.size() && closers.size() < lead; i++) {
                if (tokens_[i].kind == TokenKind::LParen) open.push_back(i);
                else if (tokens_[i].kind == TokenKind::RParen) {
                    if (open.empty()) break;
                    if (open.back() < pos_ + lead) closers.push_back(i);
                    open.pop_back();
                }
            }
            if (closers.size() == lead) {
                std::sort(closers.begin(), closers.end(), std::greater<std::size_t>());
                for (std::size_t c : closers) tokens_.erase(tokens_.begin() + static_cast<std::ptrdiff_t>(c));
                tokens_.erase(tokens_.begin() + static_cast<std::ptrdiff_t>(pos_), tokens_.begin() + static_cast<std::ptrdiff_t>(pos_ + lead));
            }
        }
    }

    std::unordered_map<std::string, std::string> alias_map;
    // The name each table of the FROM list is known by (its alias, else its own name) is unique within the list, and a table used
    // a second time (a self-join, a lookup table joined twice) needs an alias to keep the uses apart.
    std::unordered_set<std::string> used_names;
    auto note_name = [&used_names](const std::string& name) {
        if (!used_names.insert(lowered(name)).second) throw ParseError("Not unique table/alias: '" + name + "'");
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
        used_tables.insert(lowered(bare_table(table)));
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
            join_table = expect_col_ref(); // (db.table too)
            std::optional<std::string> a = parse_table_alias();
            if (used_tables.count(lowered(bare_table(join_table)))) {
                // a second use of the table: the alias is what its columns are called in the joined rows (`m.name`), so it is
                // kept as it is instead of being replaced by the table name, which the first use is known by
                if (!a) throw ParseError("Not unique table/alias: '" + join_table + "'");
                join_alias = *a;
                alias_map[*a] = *a;
            } else if (a && shadowed(bare_table(join_table))) {
                join_alias = *a;
                alias_map[*a] = *a;
            } else if (a) {
                alias_map[*a] = join_table;
            }
            note_name(a ? *a : bare_table(join_table));
            used_tables.insert(lowered(bare_table(join_table)));
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
        allow_window_expr_ = true; // (`ORDER BY RANK() OVER (...)`: the window function becomes a hidden column the sort reads)
        for (;;) {
            std::string col = parse_sort_item();
            bool asc = true;
            if (peek_is(TokenKind::Desc)) { advance(); asc = false; }
            else if (peek_is(TokenKind::Asc)) { advance(); asc = true; }
            order_by.push_back(OrderBy{col, asc});
            if (peek_is(TokenKind::Comma)) advance(); else break;
        }
        allow_window_expr_ = false;
        for (auto& hidden : hidden_windows_) columns.push_back(std::move(hidden));
        hidden_windows_.clear();
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
        // FOR UPDATE NOWAIT / SKIP LOCKED: the locks are taken as without them
        if (peek_is(TokenKind::Ident) && (upper_word(peek()->text) == "NOWAIT")) advance();
        else if (peek_is(TokenKind::Ident) && upper_word(peek()->text) == "SKIP" && peek_at_is(1, TokenKind::Ident) && upper_word(peek_at(1)->text) == "LOCKED") { advance(); advance(); }
    } else if (peek_is(TokenKind::Ident) && upper_word(peek()->text) == "LOCK" && peek_at_is(1, TokenKind::In) && peek_at_is(2, TokenKind::Share) &&
               peek_at(3) && peek_at(3)->kind == TokenKind::Ident && upper_word(peek_at(3)->text) == "MODE") { // LOCK IN SHARE MODE
        advance(); advance(); advance(); advance();
        for_share = true;
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

    return set_operation(std::move(select_stmt));
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
