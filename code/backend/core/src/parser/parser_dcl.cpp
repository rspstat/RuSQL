#include <cctype>

#include "engine/parser/parser.hpp"

namespace engine {

namespace {
std::string to_upper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}
} // namespace

Statement Parser::parse_backup() {
    // BACKUP [DATABASE db] [INTO 'file']
    std::optional<std::string> database;
    std::optional<std::string> output_file;

    if (peek_is(TokenKind::Database)) {
        advance();
        database = expect_ident();
    }
    bool is_into = peek_is(TokenKind::Into) ||
                   (peek() && peek()->kind == TokenKind::Ident && to_upper(peek()->text) == "INTO");
    if (is_into) {
        advance();
        const Token* f = advance();
        if (!f || f->kind != TokenKind::StringLit) throw ParseError("Expected filename string");
        output_file = f->text;
    }
    return Statement(Statement::Backup{database, output_file});
}

Statement Parser::parse_restore() {
    // RESTORE FROM 'file' [DATABASE db]
    std::optional<std::string> database;
    if (peek_is(TokenKind::Database)) {
        advance();
        database = expect_ident();
    }
    {
        const Token* t = advance();
        bool ok = t && (t->kind == TokenKind::From || (t->kind == TokenKind::Ident && to_upper(t->text) == "FROM"));
        if (!ok) throw ParseError("Expected FROM in RESTORE");
    }
    const Token* f = advance();
    if (!f || f->kind != TokenKind::StringLit) throw ParseError("Expected filename string after FROM");
    std::string source_file = f->text;
    if (!database) {
        if (peek_is(TokenKind::Database)) {
            advance();
            database = expect_ident();
        }
    }
    return Statement(Statement::Restore{source_file, database});
}

Statement Parser::parse_show() {
    const Token* t = advance();
    if (!t) throw ParseError("Expected TABLES, BUFFER, WAL, ISOLATION, LOCKS, DATABASES, GRANTS, CREATE, INDEX, or PROCESSLIST");

    if (t->kind == TokenKind::Tables) { // SHOW TABLES [FROM db] [LIKE 'pattern']
        Statement::ShowTables show;
        if (peek_is(TokenKind::From) || (peek_is(TokenKind::Ident) && to_upper(peek()->text) == "IN")) {
            advance();
            show.database = expect_ident();
        }
        if (peek_is(TokenKind::Like)) {
            advance();
            const Token* pattern = advance();
            if (!pattern || pattern->kind != TokenKind::StringLit) throw ParseError("Expected a quoted pattern after LIKE");
            show.like = pattern->text;
        }
        return Statement(std::move(show));
    }
    if (t->kind == TokenKind::Ident && t->text == "BUFFER") {
        const Token* n = advance();
        if (n && n->kind == TokenKind::Ident && n->text == "POOL") return Statement(Statement::ShowBufferPool{});
        throw ParseError("Expected POOL");
    }
    if (t->kind == TokenKind::Ident && t->text == "WAL") return Statement(Statement::ShowWal{});
    if (t->kind == TokenKind::Isolation) {
        if (!peek_is(TokenKind::Level)) throw ParseError("Expected LEVEL");
        advance();
        return Statement(Statement::ShowIsolationLevel{});
    }
    if (t->kind == TokenKind::Locks) return Statement(Statement::ShowLocks{});
    if (t->kind == TokenKind::Databases) return Statement(Statement::ShowDatabases{});
    if (t->kind == TokenKind::Grants) {
        std::optional<std::string> user, host;
        if (peek_is(TokenKind::For)) {
            advance();
            auto [u, h] = parse_user_spec();
            user = u;
            host = h;
        }
        return Statement(Statement::ShowGrants{user, host});
    }
    if (t->kind == TokenKind::Create) {
        const Token* n = advance();
        if (n && n->kind == TokenKind::Table) {
            std::string table = expect_table_name();
            return Statement(Statement::ShowCreateTable{table});
        }
        if (n && n->kind == TokenKind::View) {
            std::string view = expect_table_name();
            return Statement(Statement::ShowCreateView{view});
        }
        throw ParseError("Expected TABLE or VIEW after SHOW CREATE");
    }
    if (t->kind == TokenKind::Index) {
        const Token* n = advance();
        bool ok = n && (n->kind == TokenKind::From || (n->kind == TokenKind::Ident && to_upper(n->text) == "IN"));
        if (!ok) throw ParseError("Expected FROM after SHOW INDEX");
        std::string table = expect_table_name();
        return Statement(Statement::ShowIndex{table});
    }
    if (t->kind == TokenKind::Ident && to_upper(t->text) == "PROCESSLIST") return Statement(Statement::ShowProcessList{});
    if (t->kind == TokenKind::Role) {
        if (const Token* n = peek(); n && n->kind == TokenKind::Ident && to_upper(n->text) == "S") advance();
        return Statement(Statement::ShowRoles{});
    }
    if (t->kind == TokenKind::Ident && to_upper(t->text) == "ROLES") return Statement(Statement::ShowRoles{});
    if (t->kind == TokenKind::Synonym) return Statement(Statement::ShowSynonyms{});
    if (t->kind == TokenKind::Ident && to_upper(t->text) == "SYNONYMS") return Statement(Statement::ShowSynonyms{});
    throw ParseError("Expected TABLES, BUFFER, WAL, ISOLATION, LOCKS, DATABASES, GRANTS, CREATE, INDEX, or PROCESSLIST");
}

Statement Parser::parse_lock_tables() {
    // LOCK TABLES tbl_name [READ | WRITE] [, tbl_name2 [READ | WRITE] ...]
    // No Rust/MySQL-parity original -- new C++-native addition (V1 scope: see
    // executor_dcl.cpp's exec_lock_tables design note). READ/WRITE are recognized as
    // plain identifier text (not reserved keywords), matching this file's own SHOW-clause
    // convention just above and parser_core.cpp's BACKUP/RESTORE handling -- reserving the
    // bare words "read"/"write" globally would risk breaking a real column/table named
    // that.
    if (!peek_is(TokenKind::Tables)) throw ParseError("Expected TABLES after LOCK");
    advance();
    std::vector<std::pair<std::string, bool>> tables; // (name, exclusive)
    for (;;) {
        std::string name = expect_ident();
        const Token* p = peek();
        if (!p || p->kind != TokenKind::Ident) throw ParseError("Expected READ or WRITE after '" + name + "' in LOCK TABLES");
        std::string mode = to_upper(p->text);
        bool exclusive;
        if (mode == "READ") exclusive = false;
        else if (mode == "WRITE") exclusive = true;
        else throw ParseError("Expected READ or WRITE after '" + name + "' in LOCK TABLES");
        advance();
        tables.emplace_back(std::move(name), exclusive);
        if (peek_is(TokenKind::Comma)) { advance(); continue; }
        break;
    }
    return Statement(Statement::LockTables{std::move(tables)});
}

Statement Parser::parse_set() {
    // SET @user = expr | [SESSION|GLOBAL] variable = expr | @@variable = expr, ... (several, separated by commas) | NAMES x | CHARACTER SET x |
    // [SESSION] TRANSACTION ISOLATION LEVEL x | ISOLATION LEVEL x
    const Token* t = peek();
    if (!t) throw ParseError("Expected ISOLATION");
    auto is_word = [&](const Token* tok, const char* w) { return tok && tok->kind == TokenKind::Ident && to_upper(tok->text) == w; };
    auto is_scope = [&](const Token* tok) {
        return tok && tok->kind == TokenKind::Ident &&
               (to_upper(tok->text) == "SESSION" || to_upper(tok->text) == "GLOBAL" || to_upper(tok->text) == "LOCAL" || to_upper(tok->text) == "PERSIST" ||
                to_upper(tok->text) == "PERSIST_ONLY");
    };
    if (is_scope(t) && is_word(peek_at(1), "TRANSACTION") && peek_at_is(2, TokenKind::Isolation)) { advance(); advance(); }
    else if (is_word(t, "TRANSACTION") && peek_at_is(1, TokenKind::Isolation)) advance();

    const bool character_set = t->kind == TokenKind::Varchar && to_upper(t->word) == "CHARACTER" && peek_at_is(1, TokenKind::Set); // (CHARACTER is lexed as a type)
    if (is_word(t, "NAMES") || character_set) {
        // the character set of the connection: everything is utf8mb4 here
        advance();
        if (peek_is(TokenKind::Set)) advance();
        if (peek()) advance(); // the name (or DEFAULT)
        if (const Token* c = peek(); is_word(c, "COLLATE")) {
            advance();
            if (peek()) advance();
        }
        return Statement(Statement::SetVars{});
    }

    if (peek_is(TokenKind::Isolation)) {
        advance();
        if (!peek_is(TokenKind::Level)) throw ParseError("Expected LEVEL");
        advance();
        const Token* lt = advance();
        if (!lt) throw ParseError("Expected isolation level name");
        IsolationLevel level;
        if (lt->kind == TokenKind::Ident && to_upper(lt->text) == "READ") {
            const Token* n = advance();
            if (n && n->kind == TokenKind::Uncommitted) level = IsolationLevel::ReadUncommitted;
            else if (n && n->kind == TokenKind::Committed) level = IsolationLevel::ReadCommitted;
            else throw ParseError("Expected UNCOMMITTED or COMMITTED after READ");
        } else if (lt->kind == TokenKind::Repeatable) {
            advance(); // consume trailing "READ"
            level = IsolationLevel::RepeatableRead;
        } else if (lt->kind == TokenKind::Serializable) {
            level = IsolationLevel::Serializable;
        } else {
            throw ParseError("Expected isolation level name");
        }
        return Statement(Statement::SetIsolationLevel{level});
    }

    std::vector<Statement::SetUserVar> items;
    for (;;) {
        std::string name;
        bool server_variable = false;
        if (peek_is(TokenKind::At)) {
            advance();
            name = expect_ident(); // (`@x`: x; `@@x`: @x, as the lexer reads it)
            server_variable = !name.empty() && name[0] == '@';
        } else {
            if (is_scope(peek()) && peek_at(1) && peek_at(1)->kind != TokenKind::Eq) advance(); // SESSION x = ...
            const Token* v = advance();
            if (!v) throw ParseError("Expected ISOLATION");
            std::string word = !v->text.empty() ? v->text : v->word;
            if (word.empty()) throw ParseError("Expected a variable name after SET");
            for (char& c : word) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            name = "@" + word;
            server_variable = true;
        }
        if (!peek_is_assign()) throw ParseError("Expected '=' after " + (server_variable ? name.substr(1) : "@" + name));
        advance();
        ArithExpr expr;
        const Token* val = peek();
        const bool bare_word = server_variable && val && val->kind != TokenKind::Null && val->kind != TokenKind::StringLit && val->kind != TokenKind::NumberLit &&
                               val->kind != TokenKind::LParen && val->kind != TokenKind::At && val->kind != TokenKind::Minus &&
                               (!val->text.empty() || !val->word.empty()) &&
                               (!peek_at(1) || peek_at(1)->kind == TokenKind::Comma || peek_at(1)->kind == TokenKind::Semicolon);
        if (bare_word) { // SET sql_mode = ANSI, autocommit = ON, time_zone = SYSTEM: the word is the value
            advance();
            expr = ArithExpr(ArithExpr::Str{!val->text.empty() ? val->text : val->word});
        } else {
            expr = parse_value_expr(); // (a `(SELECT ...)` is a scalar subquery: no row is NULL, several rows an error)
        }
        items.push_back(Statement::SetUserVar{std::move(name), std::move(expr)});
        if (peek_is(TokenKind::Comma)) { advance(); continue; }
        break;
    }
    if (items.size() == 1) return Statement(std::move(items.front()));
    return Statement(Statement::SetVars{std::move(items)});
}

Statement Parser::parse_prepare() {
    // PREPARE stmt_name FROM 'query'
    std::string name = expect_ident();
    {
        const Token* t = advance();
        bool ok = t && (t->kind == TokenKind::From || t->kind == TokenKind::Ident);
        if (!ok) throw ParseError("Expected FROM in PREPARE");
    }
    const Token* q = advance();
    if (!q || q->kind != TokenKind::StringLit) throw ParseError("Expected query string in PREPARE");
    return Statement(Statement::PrepareStmt{name, q->text});
}

Statement Parser::parse_execute() {
    // EXECUTE stmt_name [USING @var1, @var2, ...]
    std::string name = expect_ident();
    std::vector<std::string> using_vars;
    if (peek_is(TokenKind::Using)) {
        advance();
        for (;;) {
            if (!peek_is(TokenKind::At)) throw ParseError("Expected @var in EXECUTE USING");
            advance();
            using_vars.push_back(expect_ident());
            if (peek_is(TokenKind::Comma)) advance(); else break;
        }
    }
    return Statement(Statement::ExecuteStmt{name, using_vars});
}

Statement Parser::parse_deallocate() {
    if (!peek_is(TokenKind::Prepare)) throw ParseError("Expected PREPARE after DEALLOCATE");
    advance();
    std::string name = expect_ident();
    return Statement(Statement::DeallocatePrepare{name});
}

Statement Parser::parse_describe() {
    std::string table = expect_table_name();
    return Statement(Statement::Describe{table});
}

Statement Parser::parse_truncate() {
    if (peek_is(TokenKind::Table)) advance(); // (TRUNCATE t is TRUNCATE TABLE t)
    std::string name = expect_table_name();
    return Statement(Statement::TruncateTable{name});
}

Statement Parser::parse_vacuum() {
    std::optional<std::string> table;
    if (peek_is(TokenKind::Ident)) table = expect_ident();
    return Statement(Statement::Vacuum{table});
}

// ── 사용자 스펙 파싱: 'user'@'host' 또는 user@host 또는 user ──────────
std::pair<std::string, std::string> Parser::parse_user_spec() {
    std::string user;
    {
        const Token* t = advance();
        if (!t) throw ParseError("Expected username");
        if (t->kind == TokenKind::StringLit || t->kind == TokenKind::Ident) user = t->text;
        else throw ParseError("Expected username");
    }
    std::string host = "%";
    if (peek_is(TokenKind::At)) {
        advance();
        const Token* t = advance();
        if (!t) throw ParseError("Expected hostname after @");
        if (t->kind == TokenKind::StringLit || t->kind == TokenKind::Ident || t->kind == TokenKind::NumberLit) host = t->text;
        else if (t->kind == TokenKind::Mod) host = "mod";
        else throw ParseError("Expected hostname after @");
    }
    return {user, host};
}

// CREATE USER ['user'@'host'] [IDENTIFIED BY 'password']
Statement Parser::parse_create_user() {
    bool if_not_exists = false;
    if (peek_is(TokenKind::If)) {
        advance();
        if (!peek_is(TokenKind::Not)) throw ParseError("Expected NOT");
        advance();
        if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS");
        advance();
        if_not_exists = true;
    }
    auto [user, host] = parse_user_spec();
    std::optional<std::string> password;
    if (peek_is(TokenKind::Identified)) {
        advance(); // IDENTIFIED
        const Token* by = advance();
        bool ok = by && (by->kind == TokenKind::By || by->kind == TokenKind::Ident);
        if (!ok) throw ParseError("Expected BY");
        const Token* p = advance();
        if (!p || p->kind != TokenKind::StringLit) throw ParseError("Expected password string");
        password = p->text;
    }
    return Statement(Statement::CreateUser{user, host, password, if_not_exists});
}

// DROP USER [IF EXISTS] 'user'@'host'
Statement Parser::parse_drop_user() {
    bool if_exists = false;
    if (peek_is(TokenKind::If)) {
        advance();
        if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS");
        advance();
        if_exists = true;
    }
    auto [user, host] = parse_user_spec();
    return Statement(Statement::DropUser{user, host, if_exists});
}

std::string Parser::parse_grant_object() {
    std::string first;
    {
        const Token* t = advance();
        if (!t) throw ParseError("Expected grant object");
        if (t->kind == TokenKind::Asterisk) first = "*";
        else if (t->kind == TokenKind::Ident || t->kind == TokenKind::StringLit) first = t->text;
        else throw ParseError("Expected grant object");
    }
    if (peek_is(TokenKind::Dot)) {
        advance();
        const Token* t = advance();
        std::string second;
        if (t && t->kind == TokenKind::Asterisk) second = "*";
        else if (t && t->kind == TokenKind::Ident) second = t->text;
        else throw ParseError("Expected identifier or * after dot");
        return first + "." + second;
    }
    return first;
}

Statement Parser::parse_grant() {
    // GRANT ROLE roleName TO 'user'@'host' [WITH ADMIN OPTION]
    if (peek_is(TokenKind::Role)) {
        advance();
        std::string role = expect_ident();
        if (!peek_is(TokenKind::To)) throw ParseError("Expected TO after GRANT ROLE name");
        advance();
        auto [user, host] = parse_user_spec();
        bool with_admin_option = false;
        if (peek_is(TokenKind::With)) {
            advance();
            const Token* t = advance();
            if (!(t && t->kind == TokenKind::Ident && to_upper(t->text) == "ADMIN")) throw ParseError("Expected ADMIN after WITH");
            if (peek_is(TokenKind::OptionKw)) advance();
            with_admin_option = true;
        }
        return Statement(Statement::GrantRole{role, user, host, with_admin_option});
    }

    std::vector<std::string> privileges;
    for (;;) {
        const Token* t = advance();
        std::string priv_name;
        if (!t) throw ParseError("Expected privilege name");
        switch (t->kind) {
            case TokenKind::Privileges: priv_name = "ALL PRIVILEGES"; break;
            case TokenKind::All:
                if (peek_is(TokenKind::Privileges)) advance();
                priv_name = "ALL PRIVILEGES";
                break;
            case TokenKind::Select: priv_name = "SELECT"; break;
            case TokenKind::Insert: priv_name = "INSERT"; break;
            case TokenKind::Update: priv_name = "UPDATE"; break;
            case TokenKind::Delete: priv_name = "DELETE"; break;
            case TokenKind::Create: priv_name = "CREATE"; break;
            case TokenKind::Drop: priv_name = "DROP"; break;
            case TokenKind::Alter: priv_name = "ALTER"; break;
            case TokenKind::Index: priv_name = "INDEX"; break;
            case TokenKind::Grant: priv_name = "GRANT OPTION"; break;
            case TokenKind::Ident:
                if (to_upper(t->text) == "ALL") {
                    if (peek_is(TokenKind::Privileges)) advance();
                    priv_name = "ALL PRIVILEGES";
                } else {
                    priv_name = to_upper(t->text);
                }
                break;
            default: throw ParseError("Expected privilege name");
        }
        privileges.push_back(priv_name);
        if (peek_is(TokenKind::Comma)) advance(); else break;
    }
    if (!peek_is(TokenKind::On)) throw ParseError("Expected ON");
    advance();
    std::string object_type = "TABLE";
    if (peek_is(TokenKind::Table)) { advance(); object_type = "TABLE"; }
    else if (peek_is(TokenKind::Database)) { advance(); object_type = "DATABASE"; }
    else if (peek_is(TokenKind::Databases)) { advance(); object_type = "DATABASES"; }
    std::string object = parse_grant_object();
    if (!peek_is(TokenKind::To)) throw ParseError("Expected TO");
    advance();
    auto [user, host] = parse_user_spec();
    bool with_grant_option = false;
    if (peek_is(TokenKind::With)) {
        advance();
        if (peek_is(TokenKind::Grant)) advance();
        if (peek_is(TokenKind::OptionKw)) advance();
        with_grant_option = true;
    }
    return Statement(Statement::Grant{privileges, object_type, object, user, host, with_grant_option});
}

Statement Parser::parse_revoke() {
    if (peek_is(TokenKind::Role)) {
        advance();
        std::string role = expect_ident();
        if (!peek_is(TokenKind::From)) throw ParseError("Expected FROM after REVOKE ROLE name");
        advance();
        auto [user, host] = parse_user_spec();
        return Statement(Statement::RevokeRole{role, user, host});
    }

    std::vector<std::string> privileges;
    for (;;) {
        const Token* t = advance();
        std::string priv_name;
        if (!t) throw ParseError("Expected privilege name");
        switch (t->kind) {
            case TokenKind::Privileges: priv_name = "ALL PRIVILEGES"; break;
            case TokenKind::All:
                if (peek_is(TokenKind::Privileges)) advance();
                priv_name = "ALL PRIVILEGES";
                break;
            case TokenKind::Select: priv_name = "SELECT"; break;
            case TokenKind::Insert: priv_name = "INSERT"; break;
            case TokenKind::Update: priv_name = "UPDATE"; break;
            case TokenKind::Delete: priv_name = "DELETE"; break;
            case TokenKind::Create: priv_name = "CREATE"; break;
            case TokenKind::Drop: priv_name = "DROP"; break;
            case TokenKind::Alter: priv_name = "ALTER"; break;
            case TokenKind::Index: priv_name = "INDEX"; break;
            case TokenKind::Grant: priv_name = "GRANT OPTION"; break;
            case TokenKind::Ident:
                if (to_upper(t->text) == "ALL") {
                    if (peek_is(TokenKind::Privileges)) advance();
                    priv_name = "ALL PRIVILEGES";
                } else {
                    priv_name = to_upper(t->text);
                }
                break;
            default: throw ParseError("Expected privilege name");
        }
        privileges.push_back(priv_name);
        if (peek_is(TokenKind::Comma)) advance(); else break;
    }
    if (!peek_is(TokenKind::On)) throw ParseError("Expected ON");
    advance();
    std::string object_type = "TABLE";
    if (peek_is(TokenKind::Table)) { advance(); object_type = "TABLE"; }
    else if (peek_is(TokenKind::Database)) { advance(); object_type = "DATABASE"; }
    else if (peek_is(TokenKind::Databases)) { advance(); object_type = "DATABASES"; }
    std::string object = parse_grant_object();
    if (!peek_is(TokenKind::From)) throw ParseError("Expected FROM");
    advance();
    auto [user, host] = parse_user_spec();
    return Statement(Statement::Revoke{privileges, object_type, object, user, host});
}

} // namespace engine
