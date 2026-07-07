#include <vector>

#include "catch.hpp"
#include "engine/parser/lexer.hpp"

using namespace engine;

namespace {
std::vector<Token> lex(const std::string& sql) {
    Lexer lexer(sql);
    return lexer.tokenize();
}
} // namespace

TEST_CASE("basic SELECT tokenizes to expected keyword/ident/operator sequence", "[lexer]") {
    auto toks = lex("SELECT * FROM t WHERE id = 1;");
    std::vector<TokenKind> kinds;
    for (auto& t : toks) kinds.push_back(t.kind);

    std::vector<TokenKind> expected = {
        TokenKind::Select, TokenKind::Asterisk, TokenKind::From, TokenKind::Ident,
        TokenKind::Where, TokenKind::Ident, TokenKind::Eq, TokenKind::NumberLit, TokenKind::Semicolon,
    };
    REQUIRE(kinds == expected);
    REQUIRE(toks[3].text == "t");
    REQUIRE(toks[5].text == "id");
    REQUIRE(toks[7].text == "1");
}

TEST_CASE("keywords are case-insensitive", "[lexer]") {
    auto toks = lex("select * from t");
    REQUIRE(toks[0].kind == TokenKind::Select);
    REQUIRE(toks[2].kind == TokenKind::From);
}

TEST_CASE("negative number literal only when not following a value/ident/string/RParen", "[lexer]") {
    // Leading negative literal.
    auto a = lex("SELECT -5");
    REQUIRE(a.back().kind == TokenKind::NumberLit);
    REQUIRE(a.back().text == "-5");

    // After an identifier: binary minus, not a negative literal.
    auto b = lex("col -5");
    REQUIRE(b[0].kind == TokenKind::Ident);
    REQUIRE(b[1].kind == TokenKind::Minus);
    REQUIRE(b[2].kind == TokenKind::NumberLit);
    REQUIRE(b[2].text == "5");

    // After ')': binary minus.
    auto c = lex(") -5");
    REQUIRE(c[0].kind == TokenKind::RParen);
    REQUIRE(c[1].kind == TokenKind::Minus);

    // After a comma: negative literal again.
    auto d = lex("(1, -2)");
    REQUIRE(d[2].kind == TokenKind::Comma);
    REQUIRE(d[3].kind == TokenKind::NumberLit);
    REQUIRE(d[3].text == "-2");
}

TEST_CASE("string literals handle escapes and doubled single-quotes", "[lexer]") {
    auto toks = lex(R"(SELECT 'it''s' , 'line\nbreak')");
    REQUIRE(toks[1].kind == TokenKind::StringLit);
    REQUIRE(toks[1].text == "it's");
    REQUIRE(toks[3].kind == TokenKind::StringLit);
    REQUIRE(toks[3].text == "line\nbreak");
}

TEST_CASE("quoted identifiers are always Ident, never keywords", "[lexer]") {
    auto toks = lex("SELECT `order`, \"user\" FROM t");
    REQUIRE(toks[1].kind == TokenKind::Ident);
    REQUIRE(toks[1].text == "order");
    REQUIRE(toks[3].kind == TokenKind::Ident);
    REQUIRE(toks[3].text == "user");
}

TEST_CASE("comments are stripped: --, #, /* */", "[lexer]") {
    auto toks = lex("SELECT 1 -- trailing comment\n, 2 # hash comment\n, /* block\ncomment */ 3");
    std::vector<TokenKind> kinds;
    for (auto& t : toks) kinds.push_back(t.kind);
    std::vector<TokenKind> expected = {
        TokenKind::Select, TokenKind::NumberLit, TokenKind::Comma, TokenKind::NumberLit,
        TokenKind::Comma, TokenKind::NumberLit,
    };
    REQUIRE(kinds == expected);
}

TEST_CASE("hex literal 0x.. becomes a StringLit of hex digits", "[lexer]") {
    auto toks = lex("SELECT 0x1A");
    REQUIRE(toks[1].kind == TokenKind::StringLit);
    REQUIRE(toks[1].text == "1A");
}

TEST_CASE("multi-char operators: <> != >= <= -> ->> || %", "[lexer]") {
    auto toks = lex("a <> b != c >= d <= e -> f ->> g || h % i");
    std::vector<TokenKind> kinds;
    for (auto& t : toks) kinds.push_back(t.kind);
    std::vector<TokenKind> expected = {
        TokenKind::Ident, TokenKind::Ne, TokenKind::Ident,
        TokenKind::Ne, TokenKind::Ident,
        TokenKind::Gte, TokenKind::Ident,
        TokenKind::Lte, TokenKind::Ident,
        TokenKind::Arrow, TokenKind::Ident,
        TokenKind::LongArrow, TokenKind::Ident,
        TokenKind::PipePipe, TokenKind::Ident,
        TokenKind::Percent, TokenKind::Ident,
    };
    REQUIRE(kinds == expected);
}

TEST_CASE("TRUE/FALSE normalize to lowercase Ident tokens", "[lexer]") {
    // WHERE(0) a(1) =(2) TRUE(3) AND(4) b(5) =(6) false(7)
    auto toks = lex("WHERE a = TRUE AND b = false");
    REQUIRE(toks[3].kind == TokenKind::Ident);
    REQUIRE(toks[3].text == "true");
    REQUIRE(toks[7].kind == TokenKind::Ident);
    REQUIRE(toks[7].text == "false");
}

TEST_CASE("type alias keywords normalize to the canonical TokenKind", "[lexer]") {
    auto toks = lex("INTEGER INT4 INT8 CHAR NUMERIC REAL BOOL");
    std::vector<TokenKind> kinds;
    for (auto& t : toks) kinds.push_back(t.kind);
    std::vector<TokenKind> expected = {
        TokenKind::Int, TokenKind::Int, TokenKind::BigInt, TokenKind::Varchar,
        TokenKind::Decimal, TokenKind::Float, TokenKind::Boolean,
    };
    REQUIRE(kinds == expected);
}
