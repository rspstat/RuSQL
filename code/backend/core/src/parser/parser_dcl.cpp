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

    if (t->kind == TokenKind::Tables) return Statement(Statement::ShowTables{});
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
            std::string table = expect_ident();
            return Statement(Statement::ShowCreateTable{table});
        }
        if (n && n->kind == TokenKind::View) {
            std::string view = expect_ident();
            return Statement(Statement::ShowCreateView{view});
        }
        throw ParseError("Expected TABLE or VIEW after SHOW CREATE");
    }
    if (t->kind == TokenKind::Index) {
        const Token* n = advance();
        bool ok = n && (n->kind == TokenKind::From || (n->kind == TokenKind::Ident && to_upper(n->text) == "IN"));
        if (!ok) throw ParseError("Expected FROM after SHOW INDEX");
        std::string table = expect_ident();
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

Statement Parser::parse_set() {
    const Token* t = advance();
    if (!t) throw ParseError("Expected ISOLATION");
    if (t->kind == TokenKind::At) {
        std::string name = expect_ident();
        if (!peek_is(TokenKind::Eq)) throw ParseError("Expected '=' after @" + name);
        advance();
        ArithExpr expr = parse_arith_expr();
        return Statement(Statement::SetUserVar{name, std::move(expr)});
    }
    if (t->kind == TokenKind::Isolation) {
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
    throw ParseError("Expected ISOLATION");
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
    std::string table = expect_ident();
    return Statement(Statement::Describe{table});
}

Statement Parser::parse_truncate() {
    if (!peek_is(TokenKind::Table)) throw ParseError("Expected TABLE");
    advance();
    std::string name = expect_ident();
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
