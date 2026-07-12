#include <cctype>
#include <charconv>
#include <cstdlib>

#include "engine/parser/parser.hpp"

namespace engine {

namespace {
std::string to_upper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// Uses from_chars (not strtod/c_str()) to validate the whole [data, data+size)
// range, matching Rust's slice-based `s.parse::<f64>()` rather than stopping at an
// embedded '\0' the way a null-terminated C-string parse would.
bool parses_as_number(const std::string& s) {
    if (s.empty()) return false;
    double out = 0.0;
    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

// PLAN.md P0 fix: a lone `-` followed directly (no whitespace) by a digit, where the
// preceding token isn't itself a value, gets folded by the lexer into a single
// negative NumberLit token (see lexer.cpp's '-' case) -- which is exactly what
// happens to the second `-` in a double-unary chain like `- -5` (the first bare
// Minus operator token isn't a "value", so lexing the second "-5" folds it into
// NumberLit("-5")). Every call site below used to blindly prepend another '-' onto
// that already-negative text, turning `- -5` into the literal string "--5" instead
// of the correctly re-negated "5". Toggling the existing sign instead of always
// prepending handles both the already-negative and the plain-positive case.
std::string negate_number_text(const std::string& text) {
    if (!text.empty() && text.front() == '-') return text.substr(1);
    return "-" + text;
}
} // namespace

/// Top-level condition expression parser (entry point for WHERE/HAVING/ON)
CondExpr Parser::parse_condexpr() { return parse_or_expr(); }

/// OR has lower precedence than AND
CondExpr Parser::parse_or_expr() {
    CondExpr left = parse_and_expr();
    while (peek_is(TokenKind::Or)) {
        advance();
        CondExpr right = parse_and_expr();
        left = CondExpr(CondExpr::Or{std::make_unique<CondExpr>(std::move(left)), std::make_unique<CondExpr>(std::move(right))});
    }
    return left;
}

/// AND has higher precedence than OR
CondExpr Parser::parse_and_expr() {
    CondExpr left = parse_not_expr();
    while (peek_is(TokenKind::And)) {
        advance();
        CondExpr right = parse_not_expr();
        left = CondExpr(CondExpr::And{std::make_unique<CondExpr>(std::move(left)), std::make_unique<CondExpr>(std::move(right))});
    }
    return left;
}

/// NOT has higher precedence than AND
CondExpr Parser::parse_not_expr() {
    if (peek_is(TokenKind::Not)) {
        const Token* next = peek_at(1);
        bool is_not_in_or_exists = next && (next->kind == TokenKind::In || next->kind == TokenKind::Exists);
        if (!is_not_in_or_exists) {
            advance(); // consume NOT
            CondExpr inner = parse_not_expr();
            return CondExpr(CondExpr::Not{std::make_unique<CondExpr>(std::move(inner))});
        }
    }
    return parse_primary_cond();
}

/// Handles parenthesized sub-expressions or single predicates
CondExpr Parser::parse_primary_cond() {
    if (peek_is(TokenKind::LParen)) {
        bool is_subquery = peek_at_is(1, TokenKind::Select);
        if (!is_subquery) {
            advance(); // consume '('
            CondExpr inner = parse_or_expr();
            if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')'");
            advance();
            return inner;
        }
    }
    Condition cond = parse_single_pred();
    return CondExpr(CondExpr::Leaf{std::move(cond)});
}

/// Parses a single predicate (leaf node): col OP val, IS NULL, BETWEEN, LIKE, IN, EXISTS, etc.
Condition Parser::parse_single_pred() {
    // EXISTS (SELECT ...)
    if (peek_is(TokenKind::Exists)) {
        advance();
        Statement sub = parse_exists_subquery();
        return Condition{ArithExpr(ArithExpr::Col{""}), Operator::Exists,
                          ConditionValue(ConditionValue::Subquery{std::make_unique<Statement>(std::move(sub))})};
    }

    // NOT EXISTS (SELECT ...)
    if (peek_is(TokenKind::Not) && peek_at_is(1, TokenKind::Exists)) {
        advance(); // NOT
        advance(); // EXISTS
        Statement sub = parse_exists_subquery();
        return Condition{ArithExpr(ArithExpr::Col{""}), Operator::NotExists,
                          ConditionValue(ConditionValue::Subquery{std::make_unique<Statement>(std::move(sub))})};
    }

    // Left side: arithmetic expression (handles columns, aggregates, arithmetic)
    ArithExpr left = parse_arith_expr();
    return parse_pred_tail(std::move(left));
}

/// Parses the operator + RHS following an already-parsed LHS: OP val, IS NULL, BETWEEN,
/// LIKE, IN, REGEXP, etc. Factored out of parse_single_pred() so callers that already
/// have an ArithExpr in hand (e.g. an aggregate's bare column argument, for `SUM(col > x)`)
/// can reuse the same predicate grammar instead of duplicating a narrower one.
Condition Parser::parse_pred_tail(ArithExpr left) {
    auto read_in_value = [this]() -> std::string {
        const Token* t = advance();
        if (!t) throw ParseError("Expected value in IN list");
        switch (t->kind) {
            case TokenKind::StringLit:
            case TokenKind::NumberLit:
            case TokenKind::Ident:
                return t->text;
            case TokenKind::Null:
                return "NULL";
            case TokenKind::Minus: {
                const Token* n = advance();
                if (!n || n->kind != TokenKind::NumberLit) throw ParseError("Expected number after '-' in IN list");
                return negate_number_text(n->text);
            }
            default:
                throw ParseError("Expected value in IN list");
        }
    };

    // IN (subquery or literal list)
    if (peek_is(TokenKind::In)) {
        advance();
        if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after IN");
        advance();
        if (peek_is(TokenKind::Select)) {
            advance();
            Statement sub_stmt = parse_select();
            if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')'");
            advance();
            return Condition{std::move(left), Operator::In,
                              ConditionValue(ConditionValue::Subquery{std::make_unique<Statement>(std::move(sub_stmt))})};
        }
        std::vector<std::string> values;
        for (;;) {
            values.push_back(read_in_value());
            if (peek_is(TokenKind::Comma)) { advance(); }
            else if (peek_is(TokenKind::RParen)) { break; }
            else throw ParseError("Expected ',' or ')' in IN list");
        }
        advance(); // consume ')'
        return Condition{std::move(left), Operator::In, ConditionValue(ConditionValue::LiteralList{std::move(values)})};
    }

    // NOT IN (subquery or literal list)
    if (peek_is(TokenKind::Not) && peek_at_is(1, TokenKind::In)) {
        advance(); // NOT
        advance(); // IN
        if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after NOT IN");
        advance();
        if (peek_is(TokenKind::Select)) {
            advance();
            Statement sub_stmt = parse_select();
            if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')'");
            advance();
            return Condition{std::move(left), Operator::NotIn,
                              ConditionValue(ConditionValue::Subquery{std::make_unique<Statement>(std::move(sub_stmt))})};
        }
        std::vector<std::string> values;
        for (;;) {
            values.push_back(read_in_value());
            if (peek_is(TokenKind::Comma)) { advance(); }
            else if (peek_is(TokenKind::RParen)) { break; }
            else throw ParseError("Expected ',' or ')' in NOT IN list");
        }
        advance(); // consume ')'
        return Condition{std::move(left), Operator::NotIn, ConditionValue(ConditionValue::LiteralList{std::move(values)})};
    }

    auto read_between_value = [this](const char* ctx) -> std::string {
        const Token* t = advance();
        if (!t) throw ParseError(std::string("Expected value after ") + ctx);
        switch (t->kind) {
            case TokenKind::NumberLit:
            case TokenKind::StringLit:
            case TokenKind::Ident:
                return t->text;
            case TokenKind::Minus: {
                const Token* n = advance();
                if (!n || n->kind != TokenKind::NumberLit) throw ParseError(std::string("Expected number after '-' in ") + ctx);
                return negate_number_text(n->text);
            }
            default:
                throw ParseError(std::string("Expected value after ") + ctx);
        }
    };

    // NOT BETWEEN val AND val
    if (peek_is(TokenKind::Not) && peek_at_is(1, TokenKind::Between)) {
        advance(); // NOT
        advance(); // BETWEEN
        std::string start = read_between_value("NOT BETWEEN");
        if (!peek_is(TokenKind::And)) throw ParseError("Expected AND in NOT BETWEEN");
        advance();
        std::string end = read_between_value("NOT BETWEEN ... AND");
        return Condition{std::move(left), Operator::NotBetween, ConditionValue(ConditionValue::Between{start, end})};
    }

    // NOT LIKE pattern
    if (peek_is(TokenKind::Not) && peek_at_is(1, TokenKind::Like)) {
        advance(); // NOT
        advance(); // LIKE
        const Token* t = advance();
        if (!t || (t->kind != TokenKind::StringLit && t->kind != TokenKind::Ident))
            throw ParseError("Expected pattern after NOT LIKE");
        return Condition{std::move(left), Operator::NotLike, ConditionValue(ConditionValue::Literal{t->text})};
    }

    // BETWEEN val AND val
    if (peek_is(TokenKind::Between)) {
        advance();
        std::string start = read_between_value("BETWEEN");
        if (!peek_is(TokenKind::And)) throw ParseError("Expected AND in BETWEEN");
        advance();
        std::string end = read_between_value("BETWEEN ... AND");
        return Condition{std::move(left), Operator::Between, ConditionValue(ConditionValue::Between{start, end})};
    }

    // LIKE pattern
    if (peek_is(TokenKind::Like)) {
        advance();
        const Token* t = advance();
        if (!t || (t->kind != TokenKind::StringLit && t->kind != TokenKind::Ident))
            throw ParseError("Expected pattern after LIKE");
        return Condition{std::move(left), Operator::Like, ConditionValue(ConditionValue::Literal{t->text})};
    }

    // NOT REGEXP / NOT RLIKE pattern
    if (peek_is(TokenKind::Not) && peek_at_is(1, TokenKind::Regexp)) {
        advance(); // NOT
        advance(); // REGEXP
        const Token* t = advance();
        if (!t || (t->kind != TokenKind::StringLit && t->kind != TokenKind::Ident))
            throw ParseError("Expected pattern after NOT REGEXP");
        return Condition{std::move(left), Operator::NotRegexp, ConditionValue(ConditionValue::Literal{t->text})};
    }

    // REGEXP / RLIKE pattern
    if (peek_is(TokenKind::Regexp)) {
        advance();
        const Token* t = advance();
        if (!t || (t->kind != TokenKind::StringLit && t->kind != TokenKind::Ident))
            throw ParseError("Expected pattern after REGEXP");
        return Condition{std::move(left), Operator::Regexp, ConditionValue(ConditionValue::Literal{t->text})};
    }

    // IS NULL / IS NOT NULL
    if (peek_is(TokenKind::Is)) {
        advance();
        if (peek_is(TokenKind::Not)) {
            advance();
            if (!peek_is(TokenKind::Null)) throw ParseError("Expected NULL after IS NOT");
            advance();
            return Condition{std::move(left), Operator::IsNotNull, ConditionValue(ConditionValue::Literal{""})};
        }
        if (peek_is(TokenKind::Null)) {
            advance();
            return Condition{std::move(left), Operator::IsNull, ConditionValue(ConditionValue::Literal{""})};
        }
        throw ParseError("Expected NULL or NOT after IS");
    }

    Operator op;
    {
        const Token* t = advance();
        if (!t) throw ParseError("Expected comparison operator");
        switch (t->kind) {
            case TokenKind::Eq:  op = Operator::Eq; break;
            case TokenKind::Ne:  op = Operator::Ne; break;
            case TokenKind::Gt:  op = Operator::Gt; break;
            case TokenKind::Lt:  op = Operator::Lt; break;
            case TokenKind::Gte: op = Operator::Gte; break;
            case TokenKind::Lte: op = Operator::Lte; break;
            default: throw ParseError("Expected comparison operator");
        }
    }

    ConditionValue value = [&]() -> ConditionValue {
        if (peek_is(TokenKind::LParen) && peek_at_is(1, TokenKind::Select)) {
            advance(); // (
            advance(); // SELECT
            Statement sub_stmt = parse_select();
            if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after subquery");
            advance();
            return ConditionValue(ConditionValue::Subquery{std::make_unique<Statement>(std::move(sub_stmt))});
        }
        // NULL is special-cased ahead of the general expression parse: it needs the
        // "__NULL__" sentinel (not the literal 4-char string "NULL", which is also how a
        // real NULL *value* happens to be represented elsewhere in this string-based
        // engine) so `WHERE x = NULL` reliably evaluates to false rather than matching
        // NULL-valued rows.
        if (peek_is(TokenKind::Null)) {
            advance();
            return ConditionValue(ConditionValue::Literal{"__NULL__"});
        }
        // PLAN.md P0 fix: the RHS used to consume a single token (column/number/string/
        // keyword-as-column), so `WHERE v > id + 100` silently dropped `+ 100` and
        // evaluated as `v > id`. Parsing a full arithmetic expression here — the same
        // parser already used for the LHS just above — makes the RHS support the same
        // columns/numbers/strings/functions plus +,-,*,/,||,->,->> that the LHS does.
        //
        // Simple terminals (bare column, number, or string — everything the old
        // single-token parse already handled) are reduced back to ConditionValue::Literal
        // rather than wrapped as Arith, so existing Literal-based logic (Planner's
        // index-access-path selection, has_outer_ref's correlation heuristic, equi-join
        // column extraction, etc.) keeps matching exactly as before. Only a genuinely
        // compound expression (+,-,*,/, a function call, ...) becomes an Arith.
        ArithExpr expr = parse_arith_expr();
        if (auto* col = std::get_if<ArithExpr::Col>(&expr.data)) return ConditionValue(ConditionValue::Literal{col->name});
        if (auto* num = std::get_if<ArithExpr::Num>(&expr.data)) return ConditionValue(ConditionValue::Literal{num->value});
        if (auto* str = std::get_if<ArithExpr::Str>(&expr.data)) return ConditionValue(ConditionValue::Literal{str->value});
        return ConditionValue(ConditionValue::Arith{std::move(expr)});
    }();

    return Condition{std::move(left), op, std::move(value)};
}

/// EXISTS / NOT EXISTS 뒤의 (SELECT ...) 파싱
Statement Parser::parse_exists_subquery() {
    if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after EXISTS");
    advance();
    if (!peek_is(TokenKind::Select)) throw ParseError("Expected SELECT inside EXISTS");
    advance();
    Statement sub = parse_select();
    if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after EXISTS subquery");
    advance();
    return sub;
}

namespace {
// Scalar-function tokens usable in arithmetic / UPDATE SET context (parse_arith_factor).
bool is_scalar_func_token(TokenKind k) {
    switch (k) {
        case TokenKind::Concat: case TokenKind::Upper: case TokenKind::Lower:
        case TokenKind::Length: case TokenKind::Trim: case TokenKind::Substr:
        case TokenKind::Substring: case TokenKind::Replace:
        case TokenKind::Round: case TokenKind::Abs: case TokenKind::Ceil:
        case TokenKind::Floor: case TokenKind::Mod:
        case TokenKind::Coalesce: case TokenKind::Ifnull: case TokenKind::Nullif:
        case TokenKind::Lpad: case TokenKind::Rpad: case TokenKind::If:
        case TokenKind::DateAdd: case TokenKind::DateDiff:
        case TokenKind::Left: case TokenKind::Right:
        case TokenKind::Truncate: case TokenKind::Repeat:
        case TokenKind::Now: case TokenKind::Curdate:
        case TokenKind::JsonExtract: case TokenKind::JsonUnquote: case TokenKind::JsonValue:
            return true;
        default:
            return false;
    }
}

const char* scalar_func_name(TokenKind k) {
    switch (k) {
        case TokenKind::Concat: return "CONCAT";
        case TokenKind::Upper: return "UPPER";
        case TokenKind::Lower: return "LOWER";
        case TokenKind::Length: return "LENGTH";
        case TokenKind::Trim: return "TRIM";
        case TokenKind::Substr: case TokenKind::Substring: return "SUBSTR";
        case TokenKind::Replace: return "REPLACE";
        case TokenKind::Round: return "ROUND";
        case TokenKind::Abs: return "ABS";
        case TokenKind::Ceil: return "CEIL";
        case TokenKind::Floor: return "FLOOR";
        case TokenKind::Mod: return "MOD";
        case TokenKind::Coalesce: return "COALESCE";
        case TokenKind::Ifnull: return "IFNULL";
        case TokenKind::Nullif: return "NULLIF";
        case TokenKind::Lpad: return "LPAD";
        case TokenKind::Rpad: return "RPAD";
        case TokenKind::If: return "IF";
        case TokenKind::DateAdd: return "DATE_ADD";
        case TokenKind::DateDiff: return "DATEDIFF";
        case TokenKind::Left: return "LEFT";
        case TokenKind::Right: return "RIGHT";
        case TokenKind::Truncate: return "TRUNCATE";
        case TokenKind::Repeat: return "REPEAT";
        case TokenKind::Now: return "NOW";
        case TokenKind::Curdate: return "CURDATE";
        case TokenKind::JsonExtract: return "JSON_EXTRACT";
        case TokenKind::JsonUnquote: return "JSON_UNQUOTE";
        case TokenKind::JsonValue: return "JSON_VALUE";
        default: return "";
    }
}
} // namespace

/// Arithmetic factor: number | string | column | agg_func | '(' expr ')'
ArithExpr Parser::parse_arith_factor() {
    const Token* p = peek();
    if (!p) throw ParseError("Expected expression term");

    // Aggregate functions → stored as Col("COUNT(*)")
    if (p->kind == TokenKind::Count || p->kind == TokenKind::Sum || p->kind == TokenKind::Avg ||
        p->kind == TokenKind::Min || p->kind == TokenKind::Max) {
        const Token* t = advance();
        const char* label = t->kind == TokenKind::Count ? "COUNT" : t->kind == TokenKind::Sum ? "SUM" :
                            t->kind == TokenKind::Avg ? "AVG" : t->kind == TokenKind::Min ? "MIN" : "MAX";
        if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after aggregate");
        advance();
        std::string inner;
        if (peek_is(TokenKind::Asterisk)) { advance(); inner = "*"; }
        else {
            const Token* it = advance();
            if (!it || it->kind != TokenKind::Ident) throw ParseError("Expected column in aggregate");
            inner = it->text;
        }
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after aggregate");
        advance();
        return ArithExpr(ArithExpr::Col{std::string(label) + "(" + inner + ")"});
    }

    if (p->kind == TokenKind::NumberLit) {
        const Token* t = advance();
        return ArithExpr(ArithExpr::Num{t->text});
    }

    if (p->kind == TokenKind::Minus) {
        advance();
        if (peek_is(TokenKind::NumberLit)) {
            const Token* n = advance();
            return ArithExpr(ArithExpr::Num{negate_number_text(n->text)});
        }
        ArithExpr inner = parse_arith_factor();
        return ArithExpr(ArithExpr::Sub{std::make_unique<ArithExpr>(ArithExpr::Num{"0"}), std::make_unique<ArithExpr>(std::move(inner))});
    }

    if (p->kind == TokenKind::StringLit) {
        const Token* t = advance();
        return ArithExpr(ArithExpr::Str{t->text});
    }

    if (p->kind == TokenKind::Null) {
        advance();
        return ArithExpr(ArithExpr::Str{"NULL"});
    }

    if (p->kind == TokenKind::LParen) {
        advance();
        ArithExpr inner = parse_arith_expr();
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' in expression");
        advance();
        return inner;
    }

    if (is_scalar_func_token(p->kind)) {
        const Token* t = advance();
        std::string fname = scalar_func_name(t->kind);
        if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after " + fname);
        advance();
        std::vector<ArithExpr> args;
        while (!peek_is(TokenKind::RParen)) {
            if (!args.empty()) {
                if (!peek_is(TokenKind::Comma)) throw ParseError("Expected ',' in " + fname + " args");
                advance();
            }
            if (peek_is(TokenKind::RParen)) break;
            args.push_back(parse_arith_expr());
        }
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after " + fname + " args");
        advance();
        return ArithExpr(ArithExpr::Func{fname, std::move(args)});
    }

    // CONVERT(expr, type) — MySQL type-conversion syntax
    if (p->kind == TokenKind::Ident && to_upper(p->text) == "CONVERT") {
        advance();
        if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after CONVERT");
        advance();
        ArithExpr val_expr = parse_arith_expr();
        if (!peek_is(TokenKind::Comma)) throw ParseError("Expected ',' in CONVERT");
        advance();
        std::string type_str;
        {
            const Token* tt = advance();
            if (!tt) throw ParseError("Expected type in CONVERT");
            switch (tt->kind) {
                case TokenKind::Ident: type_str = to_upper(tt->text); break;
                case TokenKind::Int: type_str = "INT"; break;
                case TokenKind::BigInt: type_str = "BIGINT"; break;
                case TokenKind::Float: type_str = "FLOAT"; break;
                case TokenKind::Double: type_str = "DOUBLE"; break;
                case TokenKind::Text: type_str = "TEXT"; break;
                case TokenKind::Varchar: type_str = "CHAR"; break;
                case TokenKind::Date: type_str = "DATE"; break;
                case TokenKind::Datetime: type_str = "DATETIME"; break;
                case TokenKind::Decimal: type_str = "DECIMAL"; break;
                case TokenKind::Boolean: type_str = "BOOLEAN"; break;
                default: throw ParseError("Expected type in CONVERT");
            }
        }
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after CONVERT");
        advance();
        std::vector<ArithExpr> args;
        args.push_back(std::move(val_expr));
        args.push_back(ArithExpr(ArithExpr::Str{type_str}));
        return ArithExpr(ArithExpr::Func{"CONVERT", std::move(args)});
    }

    if (p->kind == TokenKind::Ident) {
        // Check for generic function call: IDENT(...)
        if (peek_at_is(1, TokenKind::LParen)) {
            const Token* nt = advance();
            std::string fname = nt->text;
            advance(); // consume (
            std::vector<ArithExpr> args;
            while (!peek_is(TokenKind::RParen) && peek() != nullptr) {
                if (!args.empty()) {
                    if (peek_is(TokenKind::Comma)) advance(); else break;
                }
                if (peek_is(TokenKind::RParen)) break;
                args.push_back(parse_arith_expr());
            }
            if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after " + fname + " args");
            advance();
            return ArithExpr(ArithExpr::Func{fname, std::move(args)});
        }
        std::string s = expect_col_ref();
        return ArithExpr(ArithExpr::Col{s});
    }

    // YEAR: function call if followed by '(', else unit string literal
    if (p->kind == TokenKind::Year) {
        if (peek_at_is(1, TokenKind::LParen)) {
            advance(); // consume YEAR
            advance(); // consume (
            std::vector<ArithExpr> args;
            while (!peek_is(TokenKind::RParen) && peek() != nullptr) {
                if (!args.empty()) {
                    if (peek_is(TokenKind::Comma)) advance(); else break;
                }
                if (peek_is(TokenKind::RParen)) break;
                args.push_back(parse_arith_expr());
            }
            if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after YEAR args");
            advance();
            return ArithExpr(ArithExpr::Func{"YEAR", std::move(args)});
        }
        advance();
        return ArithExpr(ArithExpr::Str{"YEAR"});
    }

    // DATE_SUB in expression context: parse INTERVAL-aware args
    if (p->kind == TokenKind::DateSub) {
        advance(); // consume DATE_SUB
        std::vector<std::string> str_args = parse_date_add_args();
        std::vector<ArithExpr> arith_args;
        for (auto& s : str_args) {
            if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
                arith_args.push_back(ArithExpr(ArithExpr::Str{s.substr(1, s.size() - 2)}));
            } else if (parses_as_number(s)) {
                arith_args.push_back(ArithExpr(ArithExpr::Num{s}));
            } else {
                arith_args.push_back(ArithExpr(ArithExpr::Col{s}));
            }
        }
        return ArithExpr(ArithExpr::Func{"DATE_SUB", std::move(arith_args)});
    }

    if (p->kind == TokenKind::At) {
        advance();
        std::string name = expect_ident();
        return ArithExpr(ArithExpr::Col{"@" + name});
    }

    // Keywords commonly used as column/table names — treat as column reference
    {
        const char* col_name = nullptr;
        switch (p->kind) {
            case TokenKind::User: col_name = "user"; break;
            case TokenKind::Row: col_name = "row"; break;
            case TokenKind::Order: col_name = "order"; break;
            case TokenKind::Group: col_name = "group"; break;
            case TokenKind::Key: col_name = "key"; break;
            case TokenKind::Role: col_name = "role"; break;
            case TokenKind::Check: col_name = "check"; break;
            case TokenKind::Rank: col_name = "rank"; break;
            case TokenKind::Interval: col_name = "interval"; break;
            case TokenKind::Database: col_name = "database"; break;
            case TokenKind::Index: col_name = "index"; break;
            case TokenKind::View: col_name = "view"; break;
            case TokenKind::Column: col_name = "column"; break;
            case TokenKind::Tables: col_name = "tables"; break;
            default: break;
        }
        if (col_name) {
            advance();
            if (peek_is(TokenKind::Dot)) {
                advance();
                std::string right = expect_any_name();
                return ArithExpr(ArithExpr::Col{std::string(col_name) + "." + right});
            }
            return ArithExpr(ArithExpr::Col{col_name});
        }
    }

    throw ParseError("Expected expression term");
}

/// Arithmetic term: factor ('*' | '/' factor)*
ArithExpr Parser::parse_arith_term() {
    ArithExpr left = parse_arith_factor();
    for (;;) {
        if (peek_is(TokenKind::Asterisk)) {
            advance();
            ArithExpr right = parse_arith_factor();
            left = ArithExpr(ArithExpr::Mul{std::make_unique<ArithExpr>(std::move(left)), std::make_unique<ArithExpr>(std::move(right))});
        } else if (peek_is(TokenKind::Slash)) {
            advance();
            ArithExpr right = parse_arith_factor();
            left = ArithExpr(ArithExpr::Div{std::make_unique<ArithExpr>(std::move(left)), std::make_unique<ArithExpr>(std::move(right))});
        } else if (peek_is(TokenKind::Percent)) {
            advance();
            ArithExpr right = parse_arith_factor();
            std::vector<ArithExpr> args;
            args.push_back(std::move(left));
            args.push_back(std::move(right));
            left = ArithExpr(ArithExpr::Func{"MOD", std::move(args)});
        } else {
            break;
        }
    }
    return left;
}

/// Arithmetic expression: term (('+' | '-') term)*
ArithExpr Parser::parse_arith_expr() {
    ArithExpr left = parse_arith_term();
    for (;;) {
        if (peek_is(TokenKind::Plus)) {
            advance();
            ArithExpr right = parse_arith_term();
            left = ArithExpr(ArithExpr::Add{std::make_unique<ArithExpr>(std::move(left)), std::make_unique<ArithExpr>(std::move(right))});
        } else if (peek_is(TokenKind::Minus)) {
            advance();
            ArithExpr right = parse_arith_term();
            left = ArithExpr(ArithExpr::Sub{std::make_unique<ArithExpr>(std::move(left)), std::make_unique<ArithExpr>(std::move(right))});
        } else if (peek_is(TokenKind::PipePipe)) {
            // a || b  →  CONCAT(a, b)
            advance();
            ArithExpr right = parse_arith_term();
            std::vector<ArithExpr> args;
            args.push_back(std::move(left));
            args.push_back(std::move(right));
            left = ArithExpr(ArithExpr::Func{"CONCAT", std::move(args)});
        } else if (peek_is(TokenKind::Arrow)) {
            // col->'$.key'  →  JSON_EXTRACT(col, '$.key')
            advance();
            const Token* t = advance();
            if (!t || t->kind != TokenKind::StringLit) throw ParseError("Expected path string after ->");
            std::vector<ArithExpr> args;
            args.push_back(std::move(left));
            args.push_back(ArithExpr(ArithExpr::Str{t->text}));
            left = ArithExpr(ArithExpr::Func{"JSON_EXTRACT", std::move(args)});
        } else if (peek_is(TokenKind::LongArrow)) {
            // col->>'$.key'  →  JSON_UNQUOTE(JSON_EXTRACT(col, '$.key'))
            advance();
            const Token* t = advance();
            if (!t || t->kind != TokenKind::StringLit) throw ParseError("Expected path string after ->>");
            std::vector<ArithExpr> extract_args;
            extract_args.push_back(std::move(left));
            extract_args.push_back(ArithExpr(ArithExpr::Str{t->text}));
            ArithExpr extract = ArithExpr(ArithExpr::Func{"JSON_EXTRACT", std::move(extract_args)});
            std::vector<ArithExpr> unquote_args;
            unquote_args.push_back(std::move(extract));
            left = ArithExpr(ArithExpr::Func{"JSON_UNQUOTE", std::move(unquote_args)});
        } else {
            break;
        }
    }
    return left;
}

std::string Parser::arith_to_string(const ArithExpr& expr) {
    return std::visit(
        [](const auto& alt) -> std::string {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, ArithExpr::Col> || std::is_same_v<T, ArithExpr::Num>) {
                if constexpr (std::is_same_v<T, ArithExpr::Col>) return alt.name; else return alt.value;
            } else if constexpr (std::is_same_v<T, ArithExpr::Str>) {
                return "'" + alt.value + "'";
            } else if constexpr (std::is_same_v<T, ArithExpr::Add>) {
                return arith_to_string(*alt.lhs) + " + " + arith_to_string(*alt.rhs);
            } else if constexpr (std::is_same_v<T, ArithExpr::Sub>) {
                return arith_to_string(*alt.lhs) + " - " + arith_to_string(*alt.rhs);
            } else if constexpr (std::is_same_v<T, ArithExpr::Mul>) {
                return arith_to_string(*alt.lhs) + " * " + arith_to_string(*alt.rhs);
            } else if constexpr (std::is_same_v<T, ArithExpr::Div>) {
                return arith_to_string(*alt.lhs) + " / " + arith_to_string(*alt.rhs);
            } else if constexpr (std::is_same_v<T, ArithExpr::Func>) {
                std::string out = alt.name + "(";
                for (std::size_t i = 0; i < alt.args.size(); i++) {
                    if (i) out += ", ";
                    out += arith_to_string(alt.args[i]);
                }
                out += ")";
                return out;
            } else if constexpr (std::is_same_v<T, ArithExpr::Cmp>) {
                return arith_to_string(*alt.lhs) + " " + alt.op + " " + arith_to_string(*alt.rhs);
            }
        },
        expr.data);
}

ArithExpr Parser::str_to_arith(const std::string& s) {
    Parser p(s);
    try {
        return p.parse_arith_expr();
    } catch (const ParseError&) {
        return ArithExpr(ArithExpr::Col{s});
    }
}

std::optional<WindowFrame> Parser::parse_window_frame() {
    FrameUnit unit;
    if (peek_is(TokenKind::Rows)) { advance(); unit = FrameUnit::Rows; }
    else if (peek_is(TokenKind::Range)) { advance(); unit = FrameUnit::Range; }
    else return std::nullopt;

    if (!peek_is(TokenKind::Between)) throw ParseError("Expected BETWEEN after ROWS/RANGE");
    advance();

    auto parse_bound = [this]() -> FrameBound {
        const Token* p = peek();
        if (!p) throw ParseError("Expected frame bound");
        if (p->kind == TokenKind::Unbounded) {
            advance();
            const Token* t = advance();
            if (t && t->kind == TokenKind::Preceding) return FrameBound(FrameBound::UnboundedPreceding{});
            if (t && t->kind == TokenKind::Following) return FrameBound(FrameBound::UnboundedFollowing{});
            throw ParseError("Expected PRECEDING/FOLLOWING after UNBOUNDED");
        }
        if (p->kind == TokenKind::Current) {
            advance();
            const Token* t = advance();
            if (t && (t->kind == TokenKind::Row || (t->kind == TokenKind::Ident && to_upper(t->text) == "ROW")))
                return FrameBound(FrameBound::CurrentRow{});
            throw ParseError("Expected ROW after CURRENT");
        }
        if (p->kind == TokenKind::NumberLit) {
            const Token* n = advance();
            std::size_t val = 0;
            try { val = static_cast<std::size_t>(std::stoull(n->text)); } catch (...) { val = 0; }
            const Token* t = advance();
            if (t && t->kind == TokenKind::Preceding) return FrameBound(FrameBound::Preceding{val});
            if (t && t->kind == TokenKind::Following) return FrameBound(FrameBound::Following{val});
            throw ParseError("Expected PRECEDING/FOLLOWING after N");
        }
        throw ParseError("Expected frame bound");
    };

    FrameBound start = parse_bound();
    if (!peek_is(TokenKind::And)) throw ParseError("Expected AND in frame");
    advance();
    FrameBound end = parse_bound();
    return WindowFrame{unit, std::move(start), std::move(end)};
}

std::pair<std::vector<CaseWhenBranch>, std::optional<std::string>> Parser::parse_case_when_inner() {
    std::vector<CaseWhenBranch> branches;
    for (;;) {
        if (!peek_is(TokenKind::When)) break;
        advance();
        CondExpr cond = parse_condexpr();
        if (!peek_is(TokenKind::Then)) throw ParseError("Expected THEN");
        advance();
        const Token* t = advance();
        std::string result;
        if (t) {
            switch (t->kind) {
                case TokenKind::StringLit: case TokenKind::NumberLit: case TokenKind::Ident: result = t->text; break;
                case TokenKind::Null: result = "NULL"; break;
                default: throw ParseError("Expected THEN value");
            }
        } else {
            throw ParseError("Expected THEN value");
        }
        branches.push_back(CaseWhenBranch{std::move(cond), result});
    }
    std::optional<std::string> else_val;
    if (peek_is(TokenKind::Else)) {
        advance();
        const Token* t = advance();
        if (!t) throw ParseError("Expected ELSE value");
        switch (t->kind) {
            case TokenKind::StringLit: case TokenKind::NumberLit: case TokenKind::Ident: else_val = t->text; break;
            case TokenKind::Null: else_val = "NULL"; break;
            default: throw ParseError("Expected ELSE value");
        }
    }
    if (!peek_is(TokenKind::End)) throw ParseError("Expected END after CASE");
    advance();
    return {branches, else_val};
}

SelectColumn Parser::parse_case_when() {
    auto [branches, else_val] = parse_case_when_inner();
    std::optional<std::string> alias;
    if (peek_is(TokenKind::As)) { advance(); alias = expect_alias_ident(); }
    return SelectColumn(SelectColumn::CaseWhen{branches, else_val, alias});
}

/// 함수 호출 인수 파싱: (arg1, arg2, ...) → Vec<String>
std::vector<std::string> Parser::parse_func_args() {
    if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after function name");
    advance();
    std::vector<std::string> args;
    while (!peek_is(TokenKind::RParen)) {
        if (!args.empty()) {
            if (!peek_is(TokenKind::Comma)) throw ParseError("Expected ',' in function args");
            advance();
        }
        if (peek_is(TokenKind::RParen)) break;
        ArithExpr expr = parse_arith_expr();
        args.push_back(arith_to_string(expr));
    }
    if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after function args");
    advance();
    return args;
}

/// CAST(expr AS type) → ["expr", "TYPE"]
std::vector<std::string> Parser::parse_cast_args() {
    if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after CAST");
    advance();
    std::string expr;
    {
        const Token* t = advance();
        if (!t) throw ParseError("Expected expression in CAST");
        switch (t->kind) {
            case TokenKind::StringLit: expr = "'" + t->text + "'"; break;
            case TokenKind::NumberLit: expr = t->text; break;
            case TokenKind::Null: expr = "NULL"; break;
            case TokenKind::Ident: {
                std::string s = t->text;
                if (peek_is(TokenKind::Dot)) {
                    advance();
                    std::string col = expect_ident();
                    expr = s + "." + col;
                } else {
                    expr = s;
                }
                break;
            }
            default: throw ParseError("Expected expression in CAST");
        }
    }
    if (!peek_is(TokenKind::As)) throw ParseError("Expected AS in CAST");
    advance();
    std::string type_str;
    {
        const Token* t = advance();
        if (!t) throw ParseError("Expected type in CAST");
        switch (t->kind) {
            case TokenKind::Ident: type_str = to_upper(t->text); break;
            case TokenKind::Int: type_str = "INT"; break;
            case TokenKind::BigInt: type_str = "BIGINT"; break;
            case TokenKind::Float: type_str = "FLOAT"; break;
            case TokenKind::Double: type_str = "DOUBLE"; break;
            case TokenKind::Text: type_str = "TEXT"; break;
            case TokenKind::Varchar: type_str = "CHAR"; break;
            case TokenKind::Date: type_str = "DATE"; break;
            case TokenKind::Datetime: type_str = "DATETIME"; break;
            case TokenKind::Decimal: type_str = "DECIMAL"; break;
            case TokenKind::Boolean: type_str = "BOOLEAN"; break;
            default: throw ParseError("Expected type in CAST");
        }
    }
    // CAST(x AS SIGNED INT) / CAST(x AS UNSIGNED INT) — skip optional INT keyword
    if (type_str == "SIGNED" || type_str == "UNSIGNED") {
        if (peek_is(TokenKind::Int) || peek_is(TokenKind::BigInt)) advance();
    }
    // optional (n) for VARCHAR(n)
    if (peek_is(TokenKind::LParen)) {
        advance();
        while (!peek_is(TokenKind::RParen) && peek() != nullptr) advance();
        advance(); // consume ')'
    }
    if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after CAST");
    advance();
    return {expr, type_str};
}

/// DATE_ADD(date, INTERVAL n unit) → ["date_expr", "n", "UNIT"]
std::vector<std::string> Parser::parse_date_add_args() {
    if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after DATE_ADD");
    advance();
    std::string date_expr;
    {
        const Token* t = advance();
        if (!t) throw ParseError("Expected date expr in DATE_ADD");
        if (t->kind == TokenKind::StringLit) {
            date_expr = "'" + t->text + "'";
        } else if (t->kind == TokenKind::Ident) {
            std::string s = t->text;
            if (peek_is(TokenKind::Dot)) {
                advance();
                std::string col = expect_ident();
                date_expr = s + "." + col;
            } else {
                date_expr = s;
            }
        } else {
            throw ParseError("Expected date expr in DATE_ADD");
        }
    }
    if (!peek_is(TokenKind::Comma)) throw ParseError("Expected ',' in DATE_ADD");
    advance();
    if (!peek_is(TokenKind::Interval)) throw ParseError("Expected INTERVAL in DATE_ADD");
    advance();
    std::string amount;
    {
        const Token* t = advance();
        if (!t) throw ParseError("Expected number in INTERVAL");
        if (t->kind == TokenKind::NumberLit) {
            amount = t->text;
        } else if (t->kind == TokenKind::Minus) {
            const Token* n = advance();
            if (!n || n->kind != TokenKind::NumberLit) throw ParseError("Expected number after - in INTERVAL");
            amount = negate_number_text(n->text);
        } else {
            throw ParseError("Expected number in INTERVAL");
        }
    }
    std::string unit;
    {
        const Token* t = advance();
        if (!t) throw ParseError("Expected INTERVAL unit in DATE_ADD");
        if (t->kind == TokenKind::Ident) unit = to_upper(t->text);
        else if (t->kind == TokenKind::Year) unit = "YEAR";
        else throw ParseError("Expected INTERVAL unit in DATE_ADD");
    }
    if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after DATE_ADD");
    advance();
    return {date_expr, amount, unit};
}

} // namespace engine
