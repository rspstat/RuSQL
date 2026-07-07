#include "engine/parser/parser.hpp"

#include <cctype>
#include <unordered_map>

namespace engine {

const std::string NULL_DEFAULT = "__NULL_DEFAULT__";

namespace {
std::string to_upper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}
} // namespace

Parser::Parser(const std::string& input) {
    Lexer lexer(input);
    tokens_ = lexer.tokenize();
}

const Token* Parser::peek() const {
    return pos_ < tokens_.size() ? &tokens_[pos_] : nullptr;
}

const Token* Parser::peek_at(std::size_t offset) const {
    std::size_t i = pos_ + offset;
    return i < tokens_.size() ? &tokens_[i] : nullptr;
}

const Token* Parser::advance() {
    const Token* t = peek();
    pos_++;
    return t;
}

bool Parser::peek_is(TokenKind k) const {
    const Token* t = peek();
    return t != nullptr && t->kind == k;
}

bool Parser::peek_at_is(std::size_t offset, TokenKind k) const {
    const Token* t = peek_at(offset);
    return t != nullptr && t->kind == k;
}

Result<Statement, std::string> Parser::parse() {
    try {
        return Result<Statement, std::string>::Ok(parse_stmt());
    } catch (const ParseError& e) {
        return Result<Statement, std::string>::Err(e.what());
    }
}

std::string Parser::expect_ident() {
    const Token* t = advance();
    if (t && t->kind == TokenKind::Ident) return t->text;
    throw ParseError("Expected identifier");
}

/// 키워드도 식별자로 허용 (AS alias 위치에서 사용)
std::string Parser::expect_alias_ident() {
    const Token* t = advance();
    if (!t) throw ParseError("Expected identifier (alias)");
    switch (t->kind) {
        case TokenKind::Ident:    return t->text;
        case TokenKind::Now:      return "now";
        case TokenKind::Date:     return "date";
        case TokenKind::Count:    return "count";
        case TokenKind::Sum:      return "sum";
        case TokenKind::Avg:      return "avg";
        case TokenKind::Min:      return "min";
        case TokenKind::Max:      return "max";
        case TokenKind::Key:      return "key";
        case TokenKind::Set:      return "set";
        case TokenKind::Select:   return "select";
        case TokenKind::From:     return "from";
        case TokenKind::Where:    return "where";
        case TokenKind::Table:    return "table";
        case TokenKind::Order:    return "order";
        case TokenKind::Group:    return "group";
        case TokenKind::Index:    return "index";
        case TokenKind::View:     return "view";
        case TokenKind::User:     return "user";
        case TokenKind::Role:     return "role";
        case TokenKind::Row:      return "row";
        case TokenKind::Left:     return "left";
        case TokenKind::Right:    return "right";
        case TokenKind::Time:     return "time";
        case TokenKind::Year:     return "year";
        case TokenKind::Rank:     return "rank";
        case TokenKind::Check:    return "check";
        case TokenKind::Replace:  return "replace";
        case TokenKind::Repeat:   return "repeat";
        case TokenKind::Truncate: return "truncate";
        case TokenKind::Interval: return "interval";
        case TokenKind::Database: return "database";
        case TokenKind::Tables:   return "tables";
        case TokenKind::Column:   return "column";
        case TokenKind::Null:     return "null";
        default: throw ParseError("Expected identifier (alias)");
    }
}

// table.column 형태를 허용하며, 테이블 접두사는 무시하고 컬럼명만 반환
std::string Parser::expect_col_ref() {
    std::string first = expect_any_ident();
    if (peek_is(TokenKind::Dot)) {
        advance();
        std::string col = expect_any_name();
        return first + "." + col; // table.column 전체 보존
    }
    return first;
}

/// Any token that can serve as an identifier in dotted names (schema.table, table.column).
std::string Parser::expect_any_name() {
    const Token* t = advance();
    if (!t) throw ParseError("Expected identifier");
    switch (t->kind) {
        case TokenKind::Ident:    return t->text;
        case TokenKind::Tables:   return "tables";
        case TokenKind::Column:   return "column";
        case TokenKind::Database: return "schema";
        case TokenKind::Index:    return "index";
        case TokenKind::View:     return "view";
        case TokenKind::Key:      return "key";
        case TokenKind::Set:      return "set";
        case TokenKind::Count:    return "count";
        case TokenKind::Sum:      return "sum";
        case TokenKind::Avg:      return "avg";
        case TokenKind::Min:      return "min";
        case TokenKind::Max:      return "max";
        case TokenKind::User:     return "user";
        case TokenKind::Role:     return "role";
        case TokenKind::Row:      return "row";
        case TokenKind::Order:    return "order";
        case TokenKind::Group:    return "group";
        case TokenKind::Left:     return "left";
        case TokenKind::Right:    return "right";
        case TokenKind::Date:     return "date";
        case TokenKind::Time:     return "time";
        case TokenKind::Year:     return "year";
        case TokenKind::Repeat:   return "repeat";
        case TokenKind::Replace:  return "replace";
        case TokenKind::Truncate: return "truncate";
        case TokenKind::Now:      return "now";
        case TokenKind::Rank:     return "rank";
        case TokenKind::Check:    return "check";
        case TokenKind::Interval: return "interval";
        default: throw ParseError("Expected identifier");
    }
}

/// 식별자 또는 alias로 쓰이는 키워드 모두 허용 (ORDER BY / GROUP BY 컬럼명 파싱용)
std::string Parser::expect_any_ident() {
    const Token* t = advance();
    if (!t) throw ParseError("Expected identifier");
    switch (t->kind) {
        case TokenKind::Ident:    return t->text;
        case TokenKind::Count:    return "count";
        case TokenKind::Sum:      return "sum";
        case TokenKind::Avg:      return "avg";
        case TokenKind::Min:      return "min";
        case TokenKind::Max:      return "max";
        case TokenKind::Now:      return "now";
        case TokenKind::Date:     return "date";
        case TokenKind::Key:      return "key";
        case TokenKind::Set:      return "set";
        case TokenKind::Index:    return "index";
        case TokenKind::View:     return "view";
        case TokenKind::User:     return "user";
        case TokenKind::Role:     return "role";
        case TokenKind::Row:      return "row";
        case TokenKind::Order:    return "order";
        case TokenKind::Group:    return "group";
        case TokenKind::Left:     return "left";
        case TokenKind::Right:    return "right";
        case TokenKind::Time:     return "time";
        case TokenKind::Year:     return "year";
        case TokenKind::Repeat:   return "repeat";
        case TokenKind::Replace:  return "replace";
        case TokenKind::Truncate: return "truncate";
        case TokenKind::Rank:     return "rank";
        case TokenKind::Check:    return "check";
        case TokenKind::Interval: return "interval";
        case TokenKind::Tables:   return "tables";
        case TokenKind::Column:   return "column";
        case TokenKind::Database: return "schema";
        default: throw ParseError("Expected identifier");
    }
}

Statement Parser::parse_stmt() {
    const Token* t = advance();
    if (!t) throw ParseError("Unknown statement: end of input");

    switch (t->kind) {
        case TokenKind::Select: return parse_select();
        case TokenKind::Insert: return parse_insert();
        case TokenKind::Update: return parse_update();
        case TokenKind::Delete: return parse_delete();
        case TokenKind::Create: {
            if (peek_is(TokenKind::Index))    { advance(); return parse_create_index(); }
            if (peek_is(TokenKind::View))     { advance(); return parse_create_view(); }
            if (peek_is(TokenKind::Database)) { advance(); return parse_create_database(); }
            if (peek_is(TokenKind::User))     { advance(); return parse_create_user(); }
            if (peek_is(TokenKind::Procedure)){ advance(); return parse_create_procedure(); }
            if (peek_is(TokenKind::Trigger))  { advance(); return parse_create_trigger(); }
            if (const Token* p = peek(); p && p->kind == TokenKind::Ident && to_upper(p->text) == "FUNCTION") {
                advance();
                return parse_create_function();
            }
            if (peek_is(TokenKind::Role)) {
                advance();
                std::string name = expect_ident();
                return Statement(Statement::CreateRole{name});
            }
            if (peek_is(TokenKind::Synonym)) {
                advance();
                std::string name = expect_ident();
                if (!peek_is(TokenKind::For)) throw ParseError("Expected FOR after SYNONYM name");
                advance();
                std::string target = expect_ident();
                return Statement(Statement::CreateSynonym{name, target, false});
            }
            if (peek_is(TokenKind::Or)) {
                // CREATE OR REPLACE SYNONYM name FOR target
                advance(); // OR
                const Token* rep = advance();
                bool ok_replace = rep && (rep->kind == TokenKind::Replace ||
                                          (rep->kind == TokenKind::Ident && to_upper(rep->text) == "REPLACE"));
                if (!ok_replace) throw ParseError("Expected REPLACE");
                if (!peek_is(TokenKind::Synonym)) throw ParseError("Expected SYNONYM after CREATE OR REPLACE");
                advance();
                std::string name = expect_ident();
                if (!peek_is(TokenKind::For)) throw ParseError("Expected FOR");
                advance();
                std::string target = expect_ident();
                return Statement(Statement::CreateSynonym{name, target, true});
            }
            return parse_create();
        }
        case TokenKind::Drop: {
            if (peek_is(TokenKind::Index))    { advance(); return parse_drop_index(); }
            if (peek_is(TokenKind::View))     { advance(); return parse_drop_view(); }
            if (peek_is(TokenKind::Database)) { advance(); return parse_drop_database(); }
            if (peek_is(TokenKind::User))     { advance(); return parse_drop_user(); }
            if (peek_is(TokenKind::Trigger))  { advance(); return parse_drop_trigger(); }
            if (peek_is(TokenKind::Procedure)){ advance(); return parse_drop_procedure(); }
            if (const Token* p = peek(); p && p->kind == TokenKind::Ident && to_upper(p->text) == "FUNCTION") {
                advance();
                return parse_drop_function();
            }
            if (peek_is(TokenKind::Role)) {
                advance();
                bool if_exists = false;
                if (peek_is(TokenKind::If)) {
                    advance();
                    if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS after IF");
                    advance();
                    if_exists = true;
                }
                std::string name = expect_ident();
                return Statement(Statement::DropRole{name, if_exists});
            }
            if (peek_is(TokenKind::Synonym)) {
                advance();
                bool if_exists = false;
                if (peek_is(TokenKind::If)) {
                    advance();
                    if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS after IF");
                    advance();
                    if_exists = true;
                }
                std::string name = expect_ident();
                return Statement(Statement::DropSynonym{name, if_exists});
            }
            return parse_drop();
        }
        case TokenKind::Grant:  return parse_grant();
        case TokenKind::Revoke: return parse_revoke();
        case TokenKind::Ident: {
            if (t->text == "BEGIN") return Statement(Statement::Begin{});
            if (t->text == "COMMIT") return Statement(Statement::Commit{});
            if (t->text == "ROLLBACK") {
                // ROLLBACK TO [SAVEPOINT] name
                if (peek_is(TokenKind::To)) {
                    advance();
                    if (peek_is(TokenKind::Savepoint)) advance();
                    std::string name = expect_ident();
                    return Statement(Statement::RollbackTo{name});
                }
                return Statement(Statement::Rollback{});
            }
            if (to_upper(t->text) == "BACKUP") return parse_backup();
            if (to_upper(t->text) == "RESTORE") return parse_restore();
            throw ParseError("Unknown statement: " + t->text);
        }
        case TokenKind::Savepoint: {
            std::string name = expect_ident();
            return Statement(Statement::Savepoint{name});
        }
        case TokenKind::Release: {
            if (peek_is(TokenKind::Savepoint)) advance();
            std::string name = expect_ident();
            return Statement(Statement::ReleaseSavepoint{name});
        }
        case TokenKind::Analyze: {
            if (!peek_is(TokenKind::Table)) throw ParseError("Expected TABLE after ANALYZE");
            advance();
            std::string table = expect_ident();
            return Statement(Statement::AnalyzeTable{table});
        }
        case TokenKind::Explain: {
            if (peek_is(TokenKind::Analyze)) {
                advance();
                Statement inner = parse_stmt();
                return Statement(Statement::ExplainAnalyze{std::make_unique<Statement>(std::move(inner))});
            }
            Statement inner = parse_stmt();
            return Statement(Statement::Explain{std::make_unique<Statement>(std::move(inner))});
        }
        case TokenKind::Alter:    return parse_alter();
        case TokenKind::Show:     return parse_show();
        case TokenKind::Describe: return parse_describe();
        case TokenKind::Truncate: return parse_truncate();
        case TokenKind::Checkpoint: return Statement(Statement::Checkpoint{});
        case TokenKind::Set:      return parse_set();
        case TokenKind::Vacuum:   return parse_vacuum();
        case TokenKind::With:     return parse_with();
        case TokenKind::Use:      return parse_use();
        case TokenKind::Merge:    return parse_merge();
        case TokenKind::Call:     return parse_call();
        case TokenKind::Prepare:  return parse_prepare();
        case TokenKind::Execute:  return parse_execute();
        case TokenKind::Deallocate: return parse_deallocate();
        default: throw ParseError("Unknown statement");
    }
}

Statement Parser::parse_use() {
    // USE [DATABASE] name
    if (peek_is(TokenKind::Database)) advance();
    std::string database = expect_ident();
    return Statement(Statement::Use{database});
}

Statement Parser::parse_with() {
    // WITH [RECURSIVE] cte_name AS (query) [, cte_name AS (query)] ... SELECT ...
    bool recursive = false;
    if (peek_is(TokenKind::Recursive)) { advance(); recursive = true; }

    std::vector<std::pair<std::string, StatementPtr>> ctes;
    for (;;) {
        std::string name = expect_ident();
        if (!peek_is(TokenKind::As)) throw ParseError("Expected AS in CTE");
        advance();
        if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' in CTE");
        advance();
        if (!peek_is(TokenKind::Select)) throw ParseError("Expected SELECT in CTE body");
        advance();
        Statement base = parse_select();
        // 재귀 CTE: body 내 UNION [ALL] 처리
        Statement body = [&]() -> Statement {
            if (peek_is(TokenKind::Union)) {
                advance(); // consume UNION
                bool all = false;
                if (peek_is(TokenKind::All)) { advance(); all = true; }
                if (!peek_is(TokenKind::Select)) throw ParseError("Expected SELECT after UNION in CTE");
                advance();
                Statement recursive_part = parse_select();
                return Statement(Statement::Union{
                    std::make_unique<Statement>(std::move(base)),
                    std::make_unique<Statement>(std::move(recursive_part)),
                    all, {}, std::nullopt, std::nullopt});
            }
            return base;
        }();
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after CTE body");
        advance();
        ctes.emplace_back(name, std::make_unique<Statement>(std::move(body)));
        if (peek_is(TokenKind::Comma)) advance(); else break;
    }

    // Main query (any DML: SELECT, INSERT ... SELECT, etc.)
    Statement query = parse_stmt();
    return Statement(Statement::With{std::move(ctes), std::make_unique<Statement>(std::move(query)), recursive});
}

} // namespace engine
