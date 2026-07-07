#include <algorithm>
#include <cctype>

#include "engine/parser/ast_json.hpp"
#include "engine/parser/parser.hpp"

namespace engine {

namespace {
std::string to_upper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}
std::string to_lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
} // namespace

// ── CALL ────────────────────────────────────────────────────
Statement Parser::parse_call() {
    std::string name = expect_ident();
    std::vector<std::string> args;
    if (peek_is(TokenKind::LParen)) {
        advance();
        if (!peek_is(TokenKind::RParen)) {
            for (;;) {
                args.push_back(parse_single_value());
                if (peek_is(TokenKind::Comma)) advance(); else break;
            }
        }
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after CALL args");
        advance();
    }
    return Statement(Statement::CallProcedure{name, args});
}

// ── CREATE PROCEDURE ────────────────────────────────────────
Statement Parser::parse_create_procedure() {
    std::string name = expect_ident();
    std::vector<std::tuple<std::string, std::string, std::string>> params;
    if (peek_is(TokenKind::LParen)) {
        advance();
        while (!peek_is(TokenKind::RParen)) {
            std::string dir;
            if (peek_is(TokenKind::In)) { advance(); dir = "IN"; }
            else if (const Token* p = peek(); p && p->kind == TokenKind::Ident) {
                std::string upper = to_upper(p->text);
                if (upper == "OUT" || upper == "INOUT") { advance(); dir = upper; }
                else dir = "IN";
            } else {
                dir = "IN";
            }
            std::string pname = expect_ident();
            std::string ptype;
            {
                const Token* t = advance();
                if (!t) throw ParseError("Expected type in procedure param");
                switch (t->kind) {
                    case TokenKind::Int: ptype = "INT"; break;
                    case TokenKind::Varchar:
                        if (peek_is(TokenKind::LParen)) {
                            advance();
                            while (!peek_is(TokenKind::RParen) && peek() != nullptr) advance();
                            advance();
                        }
                        ptype = "VARCHAR";
                        break;
                    case TokenKind::Text: ptype = "TEXT"; break;
                    case TokenKind::Float: ptype = "FLOAT"; break;
                    case TokenKind::Date: ptype = "DATE"; break;
                    case TokenKind::Datetime: ptype = "DATETIME"; break;
                    case TokenKind::Boolean: ptype = "BOOLEAN"; break;
                    case TokenKind::Ident: ptype = t->text; break;
                    default: throw ParseError("Expected type in procedure param");
                }
            }
            params.emplace_back(dir, pname, ptype);
            if (peek_is(TokenKind::Comma)) advance(); else break;
        }
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after params");
        advance();
    }
    std::vector<Statement> body = parse_proc_body();
    return Statement(Statement::CreateProcedure{name, params, std::move(body)});
}

// ── CREATE FUNCTION ─────────────────────────────────────────
Statement Parser::parse_create_function() {
    // CREATE FUNCTION name(p1, p2, ...) RETURNS type RETURN <expr>
    std::string name = to_lower(expect_ident());
    std::vector<std::string> params;
    if (peek_is(TokenKind::LParen)) {
        advance();
        while (!peek_is(TokenKind::RParen) && peek() != nullptr) {
            params.push_back(expect_ident());
            if (peek_is(TokenKind::Comma)) advance(); else break;
        }
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after params");
        advance();
    }
    // skip optional RETURNS type (e.g. RETURNS VARCHAR(50))
    if (const Token* p = peek(); p && p->kind == TokenKind::Ident && to_upper(p->text) == "RETURNS") {
        advance(); // RETURNS
        advance(); // type token
        if (peek_is(TokenKind::LParen)) {
            advance();
            while (!peek_is(TokenKind::RParen) && peek() != nullptr) advance();
            if (peek_is(TokenKind::RParen)) advance();
        }
    }
    // RETURN <expr>
    if (const Token* p = peek(); p && p->kind == TokenKind::Ident && to_upper(p->text) == "RETURN") {
        advance();
    }
    ArithExpr expr = parse_arith_expr();
    nlohmann::json j = expr;
    std::string body = j.dump();
    return Statement(Statement::CreateFunction{name, params, body});
}

Statement Parser::parse_drop_function() {
    bool if_exists = false;
    if (peek_is(TokenKind::If)) {
        advance();
        if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS");
        advance();
        if_exists = true;
    }
    std::string name = to_lower(expect_ident());
    return Statement(Statement::DropFunction{name, if_exists});
}

// ── CREATE TRIGGER ──────────────────────────────────────────
Statement Parser::parse_create_trigger() {
    std::string name = expect_ident();
    TriggerTiming timing;
    {
        const Token* t = advance();
        if (t && t->kind == TokenKind::Before) timing = TriggerTiming::Before;
        else if (t && t->kind == TokenKind::After) timing = TriggerTiming::After;
        else throw ParseError("Expected BEFORE/AFTER");
    }
    TriggerEvent event;
    {
        const Token* t = advance();
        if (t && t->kind == TokenKind::Insert) event = TriggerEvent::Insert;
        else if (t && t->kind == TokenKind::Update) event = TriggerEvent::Update;
        else if (t && t->kind == TokenKind::Delete) event = TriggerEvent::Delete;
        else throw ParseError("Expected INSERT/UPDATE/DELETE");
    }
    if (!peek_is(TokenKind::On)) throw ParseError("Expected ON");
    advance();
    std::string table = expect_ident();
    if (peek_is(TokenKind::For)) advance();
    if (peek_is(TokenKind::Each)) advance();
    if (peek_is(TokenKind::Row)) advance();
    std::vector<Statement> body = parse_proc_body();
    return Statement(Statement::CreateTrigger{name, timing, event, table, std::move(body)});
}

/// Parse BEGIN ... END block or a single statement as a procedure/trigger body
std::vector<Statement> Parser::parse_proc_body() {
    if (const Token* p = peek(); p && p->kind == TokenKind::Ident && to_upper(p->text) == "BEGIN") {
        advance();
        return parse_proc_stmts_until_end();
    }
    std::vector<Statement> v;
    v.push_back(parse_proc_stmt());
    return v;
}

/// Parse statements until END (consumes END token)
std::vector<Statement> Parser::parse_proc_stmts_until_end() {
    std::vector<Statement> stmts;
    for (;;) {
        while (peek_is(TokenKind::Semicolon)) advance();
        if (peek_is(TokenKind::End)) { advance(); break; }
        if (!peek()) break;
        stmts.push_back(parse_proc_stmt());
        if (peek_is(TokenKind::Semicolon)) advance();
    }
    return stmts;
}

/// Parse one statement inside a procedure/trigger body (handles control flow)
Statement Parser::parse_proc_stmt() {
    std::optional<std::string> label = try_parse_label();

    const Token* p = peek();
    if (p && p->kind == TokenKind::Declare) { advance(); return parse_proc_declare(); }
    if (p && p->kind == TokenKind::If) { advance(); return parse_proc_if(); }
    if (p && p->kind == TokenKind::While) { advance(); return parse_proc_while(label); }
    if (p && p->kind == TokenKind::Loop) { advance(); return parse_proc_loop(label); }
    if (p && p->kind == TokenKind::Repeat) { advance(); return parse_proc_repeat(label); }
    if (p && p->kind == TokenKind::Leave) {
        advance();
        auto lbl = try_expect_ident();
        return Statement(Statement::ProcLeave{lbl});
    }
    if (p && p->kind == TokenKind::Iterate) {
        advance();
        auto lbl = try_expect_ident();
        return Statement(Statement::ProcIterate{lbl});
    }
    if (p && p->kind == TokenKind::Set) {
        advance();
        if (peek_is(TokenKind::Isolation) || peek_is(TokenKind::At)) return parse_set();
        return parse_proc_set_var();
    }
    return parse_stmt();
}

/// If next tokens are `Ident` followed by WHILE/LOOP/REPEAT, consume the ident as a label.
std::optional<std::string> Parser::try_parse_label() {
    if (const Token* t = peek(); t && t->kind == TokenKind::Ident) {
        const Token* next = peek_at(1);
        if (next && (next->kind == TokenKind::While || next->kind == TokenKind::Loop || next->kind == TokenKind::Repeat)) {
            const Token* consumed = advance();
            return consumed->text;
        }
    }
    return std::nullopt;
}

/// Returns Some(ident) if next token is an identifier, None otherwise (no consume on None)
std::optional<std::string> Parser::try_expect_ident() {
    if (const Token* t = peek(); t && t->kind == TokenKind::Ident) {
        const Token* consumed = advance();
        return consumed->text;
    }
    return std::nullopt;
}

Statement Parser::parse_proc_declare() {
    std::string name = expect_ident();
    std::string typ;
    {
        const Token* t = advance();
        if (!t) throw ParseError("Expected type in DECLARE");
        switch (t->kind) {
            case TokenKind::Int: typ = "INT"; break;
            case TokenKind::BigInt: typ = "BIGINT"; break;
            case TokenKind::TinyInt: typ = "TINYINT"; break;
            case TokenKind::SmallInt: typ = "SMALLINT"; break;
            case TokenKind::Varchar:
                if (peek_is(TokenKind::LParen)) {
                    advance();
                    while (!peek_is(TokenKind::RParen) && peek() != nullptr) advance();
                    if (peek_is(TokenKind::RParen)) advance();
                }
                typ = "VARCHAR";
                break;
            case TokenKind::Text: typ = "TEXT"; break;
            case TokenKind::Float: typ = "FLOAT"; break;
            case TokenKind::Double: typ = "DOUBLE"; break;
            case TokenKind::Decimal:
                if (peek_is(TokenKind::LParen)) {
                    advance();
                    while (!peek_is(TokenKind::RParen) && peek() != nullptr) advance();
                    if (peek_is(TokenKind::RParen)) advance();
                }
                typ = "DECIMAL";
                break;
            case TokenKind::Boolean: typ = "BOOLEAN"; break;
            case TokenKind::Date: typ = "DATE"; break;
            case TokenKind::Datetime: typ = "DATETIME"; break;
            case TokenKind::Ident: typ = t->text; break;
            default: throw ParseError("Expected type in DECLARE");
        }
    }
    std::optional<std::string> default_value;
    if (peek_is(TokenKind::Default)) {
        advance();
        const Token* t = advance();
        if (!t) throw ParseError("Expected default value in DECLARE");
        switch (t->kind) {
            case TokenKind::StringLit: case TokenKind::NumberLit: case TokenKind::Ident: default_value = t->text; break;
            case TokenKind::Null: default_value = "NULL"; break;
            default: throw ParseError("Expected default value in DECLARE");
        }
    }
    return Statement(Statement::ProcDeclare{name, typ, default_value});
}

Statement Parser::parse_proc_set_var() {
    std::string name = expect_ident();
    if (!peek_is(TokenKind::Eq)) throw ParseError("Expected '=' in SET");
    advance();
    ArithExpr expr = parse_arith_expr();
    return Statement(Statement::ProcSet{name, std::move(expr)});
}

Statement Parser::parse_proc_if() {
    // IF <cond> THEN <body> [ELSEIF <cond> THEN <body>]* [ELSE <body>] END IF
    CondExpr condition = parse_condexpr();
    if (!peek_is(TokenKind::Then)) throw ParseError("Expected THEN after IF condition");
    advance();
    std::vector<Statement> then_body = parse_proc_stmts_until_elseif_or_else_or_end();

    std::vector<std::pair<CondExpr, std::vector<Statement>>> elseif_branches;
    std::optional<std::vector<Statement>> else_body;

    for (;;) {
        if (peek_is(TokenKind::ElseIfKw)) {
            advance();
            CondExpr cond = parse_condexpr();
            if (!peek_is(TokenKind::Then)) throw ParseError("Expected THEN after ELSEIF");
            advance();
            std::vector<Statement> body = parse_proc_stmts_until_elseif_or_else_or_end();
            elseif_branches.emplace_back(std::move(cond), std::move(body));
        } else if (peek_is(TokenKind::Else)) {
            advance();
            std::vector<Statement> body = parse_proc_stmts_until_elseif_or_else_or_end();
            else_body = std::move(body);
            if (peek_is(TokenKind::End)) advance();
            if (peek_is(TokenKind::If)) advance();
            break;
        } else if (peek_is(TokenKind::End)) {
            advance();
            if (peek_is(TokenKind::If)) advance();
            break;
        } else {
            break;
        }
    }

    return Statement(Statement::ProcIf{std::move(condition), std::move(then_body), std::move(elseif_branches), else_body});
}

/// Parse statements until ELSEIF / ELSE / END (does NOT consume that token)
std::vector<Statement> Parser::parse_proc_stmts_until_elseif_or_else_or_end() {
    std::vector<Statement> stmts;
    for (;;) {
        while (peek_is(TokenKind::Semicolon)) advance();
        if (peek_is(TokenKind::ElseIfKw) || peek_is(TokenKind::Else) || peek_is(TokenKind::End) || !peek()) break;
        stmts.push_back(parse_proc_stmt());
        if (peek_is(TokenKind::Semicolon)) advance();
    }
    return stmts;
}

Statement Parser::parse_proc_while(std::optional<std::string> label) {
    // WHILE <cond> DO <body> END WHILE
    CondExpr condition = parse_condexpr();
    if (!peek_is(TokenKind::Do)) throw ParseError("Expected DO after WHILE condition");
    advance();
    std::vector<Statement> body = parse_proc_stmts_until_end_while();
    return Statement(Statement::ProcWhile{label, std::move(condition), std::move(body)});
}

/// Parse statements until END WHILE (consumes both tokens)
std::vector<Statement> Parser::parse_proc_stmts_until_end_while() {
    std::vector<Statement> stmts;
    for (;;) {
        while (peek_is(TokenKind::Semicolon)) advance();
        if (peek_is(TokenKind::End)) {
            advance();
            if (peek_is(TokenKind::While)) advance();
            break;
        }
        if (!peek()) break;
        stmts.push_back(parse_proc_stmt());
        if (peek_is(TokenKind::Semicolon)) advance();
    }
    return stmts;
}

Statement Parser::parse_proc_loop(std::optional<std::string> label) {
    // LOOP <body> END LOOP
    std::vector<Statement> body = parse_proc_stmts_until_end_loop();
    return Statement(Statement::ProcLoop{label, std::move(body)});
}

std::vector<Statement> Parser::parse_proc_stmts_until_end_loop() {
    std::vector<Statement> stmts;
    for (;;) {
        while (peek_is(TokenKind::Semicolon)) advance();
        if (peek_is(TokenKind::End)) {
            advance();
            if (peek_is(TokenKind::Loop)) advance();
            break;
        }
        if (!peek()) break;
        stmts.push_back(parse_proc_stmt());
        if (peek_is(TokenKind::Semicolon)) advance();
    }
    return stmts;
}

Statement Parser::parse_proc_repeat(std::optional<std::string> label) {
    // REPEAT <body> UNTIL <cond> END REPEAT
    std::vector<Statement> body = parse_proc_stmts_until_until();
    CondExpr until = parse_condexpr();
    if (peek_is(TokenKind::End)) advance();
    if (peek_is(TokenKind::Repeat)) advance();
    return Statement(Statement::ProcRepeat{label, std::move(body), std::move(until)});
}

std::vector<Statement> Parser::parse_proc_stmts_until_until() {
    std::vector<Statement> stmts;
    for (;;) {
        while (peek_is(TokenKind::Semicolon)) advance();
        if (peek_is(TokenKind::Until)) { advance(); break; }
        if (!peek()) break;
        stmts.push_back(parse_proc_stmt());
        if (peek_is(TokenKind::Semicolon)) advance();
    }
    return stmts;
}

// ── DROP TRIGGER / PROCEDURE ────────────────────────────────
Statement Parser::parse_drop_trigger() {
    bool if_exists = false;
    if (peek_is(TokenKind::If)) {
        advance();
        if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS after IF");
        advance();
        if_exists = true;
    }
    std::string name = expect_ident();
    return Statement(Statement::DropTrigger{name, if_exists});
}

Statement Parser::parse_drop_procedure() {
    bool if_exists = false;
    if (peek_is(TokenKind::If)) {
        advance();
        if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS");
        advance();
        if_exists = true;
    }
    std::string name = expect_ident();
    return Statement(Statement::DropProcedure{name, if_exists});
}

} // namespace engine
