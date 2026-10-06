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
    if (text.find_first_of("123456789") == std::string::npos) return text; // (-0 is 0)
    return "-" + text;
}

// What the right side of a comparison holds when it is a plain name, number or string; anything else stays an expression. (Planner,
// correlation and join code recognise these plain forms.)
ConditionValue condition_value_of(ArithExpr expr) {
    if (auto* col = std::get_if<ArithExpr::Col>(&expr.data)) return ConditionValue(ConditionValue::Literal{col->name});
    if (auto* num = std::get_if<ArithExpr::Num>(&expr.data)) return ConditionValue(ConditionValue::Literal{num->value});
    if (auto* str = std::get_if<ArithExpr::Str>(&expr.data)) return ConditionValue(ConditionValue::Literal{str->value, true});
    return ConditionValue(ConditionValue::Arith{std::move(expr)});
}

CondExpr both(CondExpr lhs, CondExpr rhs) {
    return CondExpr(CondExpr::And{std::make_unique<CondExpr>(std::move(lhs)), std::make_unique<CondExpr>(std::move(rhs))});
}

CondExpr either(CondExpr lhs, CondExpr rhs) {
    return CondExpr(CondExpr::Or{std::make_unique<CondExpr>(std::move(lhs)), std::make_unique<CondExpr>(std::move(rhs))});
}

CondExpr negation(CondExpr inner) { return CondExpr(CondExpr::Not{std::make_unique<CondExpr>(std::move(inner))}); }

CondExpr leaf(ArithExpr left, Operator op, ConditionValue value) {
    return CondExpr(CondExpr::Leaf{Condition{std::move(left), op, std::move(value)}});
}

// A value as a condition: true when it is not NULL and not 0 (a number, or the number a text starts with), as in MySQL -- `a AND b`,
// `WHERE flag`, `IF(v, 1, 2)`. A condition that was written as one stays what it is.
CondExpr cond_of_value(ArithExpr value) {
    if (auto* pred = std::get_if<ArithExpr::Pred>(&value.data)) return std::move(*pred->cond);
    return leaf(std::move(value), Operator::Ne, ConditionValue(ConditionValue::Literal{"0"}));
}

// A condition as a value: 1, 0 or NULL
ArithExpr value_of_cond(CondExpr cond) { return ArithExpr(ArithExpr::Pred{std::make_unique<CondExpr>(std::move(cond))}); }
} // namespace

bool Parser::at_pred_operator() const {
    const Token* t = peek();
    if (!t) return false;
    switch (t->kind) {
        case TokenKind::Eq: case TokenKind::Ne: case TokenKind::Gt: case TokenKind::Lt: case TokenKind::Gte: case TokenKind::Lte:
        case TokenKind::In: case TokenKind::Between: case TokenKind::Like: case TokenKind::Regexp: case TokenKind::Is:
            return true;
        case TokenKind::Not: {
            const Token* n = peek_at(1);
            return n && (n->kind == TokenKind::In || n->kind == TokenKind::Between || n->kind == TokenKind::Like || n->kind == TokenKind::Regexp);
        }
        default:
            return false;
    }
}

bool Parser::at_value_continuation() const {
    if (at_pred_operator()) return true;
    const Token* t = peek();
    if (!t) return false;
    switch (t->kind) {
        case TokenKind::Plus: case TokenKind::Minus: case TokenKind::Asterisk: case TokenKind::Slash: case TokenKind::Percent:
        case TokenKind::PipePipe: case TokenKind::Arrow: case TokenKind::LongArrow:
            return true;
        default:
            return false;
    }
}

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
    if (peek_is(TokenKind::LParen) && !peek_at_is(1, TokenKind::Select)) {
        // `(a = 1 OR b = 2)` groups conditions, but `(a + b) * 2 > 10` starts with a parenthesised VALUE: the group is tried first and, when
        // what follows it goes on with an expression (or it is not a group of conditions at all), the whole is read as one predicate
        const std::size_t start = pos_;
        try {
            advance(); // consume '('
            CondExpr inner = parse_or_expr();
            if (peek_is(TokenKind::RParen)) {
                advance();
                if (!at_value_continuation()) return inner;
            }
        } catch (const ParseError&) {
        }
        pos_ = start;
    }
    return parse_pred_expr();
}

/// Parses a single predicate: col OP val, IS NULL, BETWEEN, LIKE, IN, EXISTS, etc.; an expression with no operator is true when it is not
/// NULL and not 0 (`WHERE flag`, `WHERE TRUE`)
CondExpr Parser::parse_pred_expr() {
    // EXISTS (SELECT ...)
    if (peek_is(TokenKind::Exists)) {
        advance();
        Statement sub = parse_exists_subquery();
        return leaf(ArithExpr(ArithExpr::Col{""}), Operator::Exists, ConditionValue(ConditionValue::Subquery{std::make_unique<Statement>(std::move(sub))}));
    }

    // NOT EXISTS (SELECT ...)
    if (peek_is(TokenKind::Not) && peek_at_is(1, TokenKind::Exists)) {
        advance(); // NOT
        advance(); // EXISTS
        Statement sub = parse_exists_subquery();
        return leaf(ArithExpr(ArithExpr::Col{""}), Operator::NotExists, ConditionValue(ConditionValue::Subquery{std::make_unique<Statement>(std::move(sub))}));
    }

    // Left side: arithmetic expression (handles columns, aggregates, arithmetic)
    ArithExpr left = parse_arith_expr();
    if (at_pred_operator()) return parse_pred_cond(std::move(left));
    return cond_of_value(std::move(left));
}

/// `left IS [NOT] TRUE|FALSE` is a test that never answers NULL; every other operator is one comparison (parse_pred_tail)
CondExpr Parser::parse_pred_cond(ArithExpr left) {
    // [NOT] BETWEEN lo AND hi: bounds that are a number, a string or an @variable make one Between condition; any other bound (a column, an
    // expression: `BETWEEN a AND a + 10`) makes the two comparisons it stands for
    const bool not_between = peek_is(TokenKind::Not) && peek_at_is(1, TokenKind::Between);
    if (not_between || peek_is(TokenKind::Between)) {
        if (not_between) advance();
        advance(); // BETWEEN
        ArithExpr lo = parse_arith_expr();
        if (!peek_is(TokenKind::And)) throw ParseError(not_between ? "Expected AND in NOT BETWEEN" : "Expected AND in BETWEEN");
        advance();
        ArithExpr hi = parse_arith_expr();
        auto plain = [](const ArithExpr& bound, std::string& text, bool& quoted) {
            if (auto* num = std::get_if<ArithExpr::Num>(&bound.data)) { text = num->value; quoted = false; return true; }
            if (auto* str = std::get_if<ArithExpr::Str>(&bound.data)) { text = str->value; quoted = true; return true; }
            if (auto* col = std::get_if<ArithExpr::Col>(&bound.data); col && !col->name.empty() && col->name[0] == '@') { text = col->name; quoted = false; return true; }
            return false;
        };
        std::string lo_text, hi_text;
        bool lo_quoted = false, hi_quoted = false;
        if (plain(lo, lo_text, lo_quoted) && plain(hi, hi_text, hi_quoted)) {
            return leaf(std::move(left), not_between ? Operator::NotBetween : Operator::Between,
                        ConditionValue(ConditionValue::Between{lo_text, hi_text, lo_quoted, hi_quoted}));
        }
        CondExpr inside = both(leaf(left, Operator::Gte, condition_value_of(std::move(lo))), leaf(left, Operator::Lte, condition_value_of(std::move(hi))));
        return not_between ? negation(std::move(inside)) : inside;
    }
    if (peek_is(TokenKind::Is)) {
        const bool negated = peek_at_is(1, TokenKind::Not);
        const Token* word = peek_at(negated ? 2 : 1);
        if (word && word->kind == TokenKind::Ident && (word->text == "true" || word->text == "false")) {
            const bool want_true = word->text == "true";
            advance(); // IS
            if (negated) advance();
            advance(); // TRUE / FALSE
            CondExpr is_it = both(leaf(left, Operator::IsNotNull, ConditionValue(ConditionValue::Literal{""})),
                                  leaf(left, want_true ? Operator::Ne : Operator::Eq, ConditionValue(ConditionValue::Literal{"0"})));
            return negated ? negation(std::move(is_it)) : is_it;
        }
    }
    return CondExpr(CondExpr::Leaf{parse_pred_tail(std::move(left))});
}

// ---------------------------------------------------------------------------
// Value expressions: arithmetic with conditions as values (`v > 5`, `a AND b`, `NOT x`), CASE, IF
// ---------------------------------------------------------------------------
ArithExpr Parser::parse_value_expr() {
    ArithExpr left = parse_value_and();
    if (!peek_is(TokenKind::Or)) return left;
    CondExpr cond = cond_of_value(std::move(left));
    while (peek_is(TokenKind::Or)) {
        advance();
        cond = either(std::move(cond), cond_of_value(parse_value_and()));
    }
    return value_of_cond(std::move(cond));
}

ArithExpr Parser::parse_value_and() {
    ArithExpr left = parse_value_not();
    if (!peek_is(TokenKind::And)) return left;
    CondExpr cond = cond_of_value(std::move(left));
    while (peek_is(TokenKind::And)) {
        advance();
        cond = both(std::move(cond), cond_of_value(parse_value_not()));
    }
    return value_of_cond(std::move(cond));
}

ArithExpr Parser::parse_value_not() {
    if (peek_is(TokenKind::Not) && !peek_at_is(1, TokenKind::Exists)) {
        advance();
        return value_of_cond(negation(cond_of_value(parse_value_not())));
    }
    return parse_value_pred();
}

ArithExpr Parser::parse_value_pred() {
    if (peek_is(TokenKind::Exists) || (peek_is(TokenKind::Not) && peek_at_is(1, TokenKind::Exists))) return value_of_cond(parse_pred_expr());
    ArithExpr left = parse_arith_expr();
    if (!at_pred_operator()) return left;
    return value_of_cond(parse_pred_cond(std::move(left)));
}

/// CASE [operand] WHEN .. THEN .. [ELSE ..] END, after the CASE: the function CASE(cond1, result1, cond2, result2, .. [, else]) where each
/// cond is a Pred; `CASE x WHEN a` is `CASE WHEN x = a`
ArithExpr Parser::parse_case_expr() {
    std::optional<ArithExpr> operand;
    if (peek_is(TokenKind::End)) throw ParseError("Expected WHEN after CASE");
    if (!peek_is(TokenKind::When)) operand = parse_arith_expr();
    if (!peek_is(TokenKind::When)) throw ParseError("Expected WHEN after CASE");
    std::vector<ArithExpr> args;
    while (peek_is(TokenKind::When)) {
        advance();
        if (operand) {
            ConditionValue value;
            if (peek_is(TokenKind::Null)) {
                advance();
                value = ConditionValue(ConditionValue::Literal{"__NULL__"}); // (equal to nothing, as in the comparison `x = NULL`)
            } else {
                value = condition_value_of(parse_arith_expr());
            }
            args.push_back(value_of_cond(leaf(*operand, Operator::Eq, std::move(value))));
        } else {
            args.push_back(value_of_cond(parse_condexpr()));
        }
        if (!peek_is(TokenKind::Then)) throw ParseError("Expected THEN");
        advance();
        args.push_back(parse_value_expr());
    }
    if (peek_is(TokenKind::Else)) {
        advance();
        args.push_back(parse_value_expr());
    }
    if (!peek_is(TokenKind::End)) throw ParseError("Expected END after CASE");
    advance();
    return ArithExpr(ArithExpr::Func{"CASE", std::move(args)});
}

/// IF(cond, a, b), after the IF: a CASE
ArithExpr Parser::parse_if_expr() {
    if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after IF");
    advance();
    std::vector<ArithExpr> args;
    args.push_back(value_of_cond(cond_of_value(parse_value_expr())));
    for (int i = 0; i < 2; i++) {
        if (!peek_is(TokenKind::Comma)) throw ParseError("Expected ',' in IF()");
        advance();
        args.push_back(parse_value_expr());
    }
    if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after IF()");
    advance();
    return ArithExpr(ArithExpr::Func{"CASE", std::move(args)});
}

/// The type after AS in CAST(x AS type): SIGNED, UNSIGNED, CHAR, DECIMAL(10, 2) (the length and scale are skipped), ...
std::string Parser::parse_cast_type() {
    std::string type_str;
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
    // CAST(x AS SIGNED INT) / CAST(x AS UNSIGNED INTEGER) -- the optional INT / INTEGER keyword is skipped
    if (type_str == "SIGNED" || type_str == "UNSIGNED") {
        if (peek_is(TokenKind::Int) || peek_is(TokenKind::BigInt)) advance();
    }
    // optional (n) for VARCHAR(n)
    if (peek_is(TokenKind::LParen)) {
        advance();
        while (!peek_is(TokenKind::RParen) && peek() != nullptr) advance();
        advance(); // consume ')'
    }
    return type_str;
}

/// CAST(expr AS type), after the CAST: the function CAST(expr, 'TYPE')
ArithExpr Parser::parse_cast_expr() {
    if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after CAST");
    advance();
    ArithExpr value = parse_value_expr();
    if (!peek_is(TokenKind::As)) throw ParseError("Expected AS in CAST");
    advance();
    std::string type_str = parse_cast_type();
    if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after CAST");
    advance();
    std::vector<ArithExpr> args;
    args.push_back(std::move(value));
    args.push_back(ArithExpr(ArithExpr::Str{type_str}));
    return ArithExpr(ArithExpr::Func{"CAST", std::move(args)});
}

/// Parses the operator + RHS following an already-parsed LHS: OP val, IS NULL, BETWEEN,
/// LIKE, IN, REGEXP, etc. Factored out of parse_single_pred() so callers that already
/// have an ArithExpr in hand (e.g. an aggregate's bare column argument, for `SUM(col > x)`)
/// can reuse the same predicate grammar instead of duplicating a narrower one.
Condition Parser::parse_pred_tail(ArithExpr left) {
    auto read_in_value = [this](bool& quoted) -> std::string {
        const Token* t = advance();
        if (!t) throw ParseError("Expected value in IN list");
        quoted = t->kind == TokenKind::StringLit;
        switch (t->kind) {
            case TokenKind::StringLit:
            case TokenKind::NumberLit:
            case TokenKind::Ident:
                return t->text;
            case TokenKind::At:
                return "@" + expect_ident(); // a user variable
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
        std::vector<bool> quoted;
        for (;;) {
            bool is_string = false;
            values.push_back(read_in_value(is_string));
            quoted.push_back(is_string);
            if (peek_is(TokenKind::Comma)) { advance(); }
            else if (peek_is(TokenKind::RParen)) { break; }
            else throw ParseError("Expected ',' or ')' in IN list");
        }
        advance(); // consume ')'
        return Condition{std::move(left), Operator::In, ConditionValue(ConditionValue::LiteralList{std::move(values), std::move(quoted)})};
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
        std::vector<bool> quoted;
        for (;;) {
            bool is_string = false;
            values.push_back(read_in_value(is_string));
            quoted.push_back(is_string);
            if (peek_is(TokenKind::Comma)) { advance(); }
            else if (peek_is(TokenKind::RParen)) { break; }
            else throw ParseError("Expected ',' or ')' in NOT IN list");
        }
        advance(); // consume ')'
        return Condition{std::move(left), Operator::NotIn, ConditionValue(ConditionValue::LiteralList{std::move(values), std::move(quoted)})};
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
        // (a NULL that something is done to -- `x < NULL + 3` -- is an expression like any other)
        if (peek_is(TokenKind::Null) && !(peek_at(1) && (peek_at(1)->kind == TokenKind::Plus || peek_at(1)->kind == TokenKind::Minus ||
                                                         peek_at(1)->kind == TokenKind::Asterisk || peek_at(1)->kind == TokenKind::Slash ||
                                                         peek_at(1)->kind == TokenKind::Percent || peek_at(1)->kind == TokenKind::PipePipe))) {
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
        // index-access-path selection, equi-join
        // column extraction, etc.) keeps matching exactly as before. Only a genuinely
        // compound expression (+,-,*,/, a function call, ...) becomes an Arith.
        return condition_value_of(parse_arith_expr());
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
        case TokenKind::DateDiff: case TokenKind::DateFormat:
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
        case TokenKind::DateDiff: return "DATEDIFF";
        case TokenKind::DateFormat: return "DATE_FORMAT";
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

/// The argument of an aggregate as text: a column as its name, any other expression (`price * qty`, `COALESCE(x, 0)`, `1`) as the text of the expression.
/// The executor reads that text back (it computes the expression for every row under it), so it has to keep the grouping of the expression.
std::string Parser::aggregate_argument_text(const ArithExpr& arg) {
    if (auto* col = std::get_if<ArithExpr::Col>(&arg.data)) return col->name;
    return arith_to_string(arg);
}

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
        if (peek_is(TokenKind::Distinct)) { advance(); inner = "DISTINCT "; }
        if (peek_is(TokenKind::Asterisk)) { advance(); inner += "*"; }
        else inner += aggregate_argument_text(parse_value_expr());
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
        ArithExpr inner = parse_value_expr();
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' in expression");
        advance();
        return inner;
    }

    if (p->kind == TokenKind::Case) {
        advance();
        return parse_case_expr();
    }
    if (p->kind == TokenKind::If && peek_at_is(1, TokenKind::LParen)) {
        advance();
        return parse_if_expr();
    }
    if (p->kind == TokenKind::Cast && peek_at_is(1, TokenKind::LParen)) {
        advance();
        return parse_cast_expr();
    }

    // DATE_ADD(date, INTERVAL amount unit) / DATE_SUB: the function with its arguments (date, amount, unit); the unit is a word
    if (p->kind == TokenKind::DateAdd || p->kind == TokenKind::DateSub) {
        const std::string fname = p->kind == TokenKind::DateAdd ? "DATE_ADD" : "DATE_SUB";
        advance();
        if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after " + fname);
        advance();
        std::vector<ArithExpr> args;
        args.push_back(parse_value_expr());
        if (!peek_is(TokenKind::Comma)) throw ParseError("Expected ',' in " + fname);
        advance();
        if (!peek_is(TokenKind::Interval)) throw ParseError("Expected INTERVAL in " + fname);
        advance();
        args.push_back(parse_arith_expr());
        const Token* unit = advance();
        if (!unit || (unit->kind != TokenKind::Ident && unit->kind != TokenKind::Year)) throw ParseError("Expected INTERVAL unit in " + fname);
        args.push_back(ArithExpr(ArithExpr::Col{unit->kind == TokenKind::Year ? "YEAR" : to_upper(unit->text)}));
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after " + fname);
        advance();
        return ArithExpr(ArithExpr::Func{fname, std::move(args)});
    }

    // DATABASE() and USER() (the words are also the names of columns)
    if ((p->kind == TokenKind::Database || p->kind == TokenKind::User) && peek_at_is(1, TokenKind::LParen)) {
        const std::string fname = p->kind == TokenKind::Database ? "DATABASE" : "USER";
        advance();
        advance();
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after " + fname);
        advance();
        return ArithExpr(ArithExpr::Func{fname, {}});
    }

    if (is_scalar_func_token(p->kind)) {
        const Token* t = advance();
        std::string fname = scalar_func_name(t->kind);
        if (!peek_is(TokenKind::LParen)) {
            // NOW, CURDATE and their spellings CURRENT_DATE / CURRENT_TIMESTAMP need no parentheses
            if (t->kind == TokenKind::Now || t->kind == TokenKind::Curdate) return ArithExpr(ArithExpr::Func{fname, {}});
            throw ParseError("Expected '(' after " + fname);
        }
        advance();
        std::vector<ArithExpr> args;
        while (!peek_is(TokenKind::RParen)) {
            if (!args.empty()) {
                if (!peek_is(TokenKind::Comma)) throw ParseError("Expected ',' in " + fname + " args");
                advance();
            }
            if (peek_is(TokenKind::RParen)) break;
            args.push_back(parse_value_expr());
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
        ArithExpr val_expr = parse_value_expr();
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

    // VALUES(col): the value an INSERT ... ON DUPLICATE KEY UPDATE was going to insert into col
    if (p->kind == TokenKind::Values && peek_at_is(1, TokenKind::LParen)) {
        advance();
        advance();
        std::vector<ArithExpr> args;
        args.push_back(parse_arith_expr());
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after VALUES");
        advance();
        return ArithExpr(ArithExpr::Func{"VALUES", std::move(args)});
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
                args.push_back(parse_value_expr());
            }
            if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after " + fname + " args");
            advance();
            return ArithExpr(ArithExpr::Func{fname, std::move(args)});
        }
        std::string s = expect_col_ref();
        // TRUE and FALSE (the lexer hands them over as the identifiers "true" / "false") are the numbers 1 and 0, as in MySQL: a BOOLEAN
        // column holds 1 / 0, so `flag = TRUE` has to compare with 1
        if (s == "true") return ArithExpr(ArithExpr::Num{"1"});
        if (s == "false") return ArithExpr(ArithExpr::Num{"0"});
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
            case TokenKind::NewKw: col_name = "NEW"; break; // a trigger's NEW.x / OLD.x
            case TokenKind::OldKw: col_name = "OLD"; break;
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

namespace {
// How tightly the outermost operator of an expression binds: a comparison or a condition 0, + and - 1, * and / 2, a column, a number, a string or a call 3.
int arith_strength(const ArithExpr& e) {
    if (std::holds_alternative<ArithExpr::Add>(e.data) || std::holds_alternative<ArithExpr::Sub>(e.data)) return 1;
    if (std::holds_alternative<ArithExpr::Mul>(e.data) || std::holds_alternative<ArithExpr::Div>(e.data)) return 2;
    if (std::holds_alternative<ArithExpr::Cmp>(e.data) || std::holds_alternative<ArithExpr::Pred>(e.data)) return 0;
    return 3;
}

std::string quoted_text(const std::string& value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') quoted += '\'';
        quoted += c;
    }
    return quoted + "'";
}

// a name, number or variable is written as it is, text (and the NULL of a comparison) the way the parser reads it back
std::string literal_text(const std::string& value, bool quoted) {
    if (value == "__NULL__") return "NULL";
    return quoted ? quoted_text(value) : value;
}

const char* operator_text(Operator op) {
    switch (op) {
        case Operator::Eq: return "=";
        case Operator::Ne: return "<>";
        case Operator::Gt: return ">";
        case Operator::Lt: return "<";
        case Operator::Gte: return ">=";
        case Operator::Lte: return "<=";
        case Operator::In: return "IN";
        case Operator::NotIn: return "NOT IN";
        case Operator::Like: return "LIKE";
        case Operator::NotLike: return "NOT LIKE";
        case Operator::Between: return "BETWEEN";
        case Operator::NotBetween: return "NOT BETWEEN";
        case Operator::IsNull: return "IS NULL";
        case Operator::IsNotNull: return "IS NOT NULL";
        case Operator::Exists: return "EXISTS";
        case Operator::NotExists: return "NOT EXISTS";
        case Operator::Regexp: return "REGEXP";
        case Operator::NotRegexp: return "NOT REGEXP";
    }
    return "";
}
// an operand is put in parentheses when it binds less tightly than the operator it belongs to needs: `(a + b) * c`, `a - (b - c)`
std::string arith_operand(const ArithExpr& e, int needs) {
    const std::string text = Parser::arith_to_string(e);
    return arith_strength(e) < needs ? "(" + text + ")" : text;
}
} // namespace

std::string Parser::arith_to_string(const ArithExpr& expr) {
    return std::visit(
        [](const auto& alt) -> std::string {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, ArithExpr::Col> || std::is_same_v<T, ArithExpr::Num>) {
                if constexpr (std::is_same_v<T, ArithExpr::Col>) return alt.name; else return alt.value;
            } else if constexpr (std::is_same_v<T, ArithExpr::Str>) {
                return quoted_text(alt.value);
            } else if constexpr (std::is_same_v<T, ArithExpr::Add>) {
                return arith_operand(*alt.lhs, 1) + " + " + arith_operand(*alt.rhs, 2);
            } else if constexpr (std::is_same_v<T, ArithExpr::Sub>) {
                return arith_operand(*alt.lhs, 1) + " - " + arith_operand(*alt.rhs, 2);
            } else if constexpr (std::is_same_v<T, ArithExpr::Mul>) {
                return arith_operand(*alt.lhs, 2) + " * " + arith_operand(*alt.rhs, 3);
            } else if constexpr (std::is_same_v<T, ArithExpr::Div>) {
                return arith_operand(*alt.lhs, 2) + " / " + arith_operand(*alt.rhs, 3);
            } else if constexpr (std::is_same_v<T, ArithExpr::Func>) {
                if (alt.name == "CASE") {
                    std::string out = "CASE";
                    for (std::size_t i = 0; i + 1 < alt.args.size(); i += 2) {
                        out += " WHEN " + condition_text(alt.args[i]) + " THEN " + arith_to_string(alt.args[i + 1]);
                    }
                    if (alt.args.size() % 2 == 1) out += " ELSE " + arith_to_string(alt.args.back());
                    return out + " END";
                }
                if ((alt.name == "DATE_ADD" || alt.name == "DATE_SUB") && alt.args.size() == 3 && std::holds_alternative<ArithExpr::Col>(alt.args[2].data)) {
                    return alt.name + "(" + arith_to_string(alt.args[0]) + ", INTERVAL " + arith_to_string(alt.args[1]) + " " +
                           std::get<ArithExpr::Col>(alt.args[2].data).name + ")";
                }
                // (the type of a CAST / CONVERT is a keyword, not a string)
                if ((alt.name == "CAST" || alt.name == "CONVERT") && alt.args.size() == 2 && std::holds_alternative<ArithExpr::Str>(alt.args[1].data)) {
                    const std::string& type = std::get<ArithExpr::Str>(alt.args[1].data).value;
                    return alt.name == "CAST" ? "CAST(" + arith_to_string(alt.args[0]) + " AS " + type + ")"
                                              : "CONVERT(" + arith_to_string(alt.args[0]) + ", " + type + ")";
                }
                std::string out = alt.name + "(";
                for (std::size_t i = 0; i < alt.args.size(); i++) {
                    if (i) out += ", ";
                    out += arith_to_string(alt.args[i]);
                }
                out += ")";
                return out;
            } else if constexpr (std::is_same_v<T, ArithExpr::Cmp>) {
                return arith_operand(*alt.lhs, 1) + " " + alt.op + " " + arith_operand(*alt.rhs, 1);
            } else if constexpr (std::is_same_v<T, ArithExpr::Pred>) {
                return cond_to_string(*alt.cond);
            }
        },
        expr.data);
}

// the condition of a CASE branch (a Pred), as written after WHEN
std::string Parser::condition_text(const ArithExpr& when) {
    if (auto* pred = std::get_if<ArithExpr::Pred>(&when.data)) return cond_to_string(*pred->cond);
    return arith_to_string(when);
}

std::string Parser::cond_to_string(const CondExpr& cond) {
    if (auto* a = std::get_if<CondExpr::And>(&cond.data)) return "(" + cond_to_string(*a->lhs) + " AND " + cond_to_string(*a->rhs) + ")";
    if (auto* o = std::get_if<CondExpr::Or>(&cond.data)) return "(" + cond_to_string(*o->lhs) + " OR " + cond_to_string(*o->rhs) + ")";
    if (auto* n = std::get_if<CondExpr::Not>(&cond.data)) return "NOT (" + cond_to_string(*n->inner) + ")";
    const Condition& c = std::get<CondExpr::Leaf>(cond.data).condition;
    const std::string left = arith_operand(c.left, 1);
    if (c.op == Operator::IsNull || c.op == Operator::IsNotNull) return left + " " + operator_text(c.op);
    if (c.op == Operator::Exists || c.op == Operator::NotExists) throw ParseError("A subquery inside this expression is not supported");
    std::string value;
    if (auto* lit = std::get_if<ConditionValue::Literal>(&c.value.data)) {
        // (the pattern of LIKE / REGEXP is a string even where the parser kept it without quotes)
        const bool pattern = c.op == Operator::Like || c.op == Operator::NotLike || c.op == Operator::Regexp || c.op == Operator::NotRegexp;
        value = literal_text(lit->value, lit->quoted || pattern);
    } else if (auto* list = std::get_if<ConditionValue::LiteralList>(&c.value.data)) {
        value = "(";
        for (std::size_t i = 0; i < list->values.size(); i++) {
            if (i) value += ", ";
            value += literal_text(list->values[i], i < list->quoted.size() && list->quoted[i]);
        }
        value += ")";
    } else if (auto* between = std::get_if<ConditionValue::Between>(&c.value.data)) {
        value = literal_text(between->lo, between->lo_quoted) + " AND " + literal_text(between->hi, between->hi_quoted);
    } else if (auto* arith = std::get_if<ConditionValue::Arith>(&c.value.data)) {
        value = arith_operand(arith->expr, 1);
    } else {
        throw ParseError("A subquery inside this expression is not supported");
    }
    return left + " " + operator_text(c.op) + " " + value;
}

ArithExpr Parser::str_to_arith(const std::string& s) {
    Parser p(s);
    try {
        ArithExpr expr = p.parse_value_expr();
        if (p.peek() != nullptr) return ArithExpr(ArithExpr::Col{s}); // (something is left over: not an expression)
        return expr;
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

} // namespace engine
