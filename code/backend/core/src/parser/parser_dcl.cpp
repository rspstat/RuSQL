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
        Statement::ShowGrants show;
        if (peek_is(TokenKind::For)) {
            advance();
            bool current = false;
            auto [u, h] = parse_account_or_current(current);
            if (!current) {
                show.user = u;
                show.host = h;
            }
            if (peek_is(TokenKind::Using)) { // SHOW GRANTS FOR user USING role [, role ...]: the privileges of those roles count as the user's
                advance();
                for (;;) {
                    show.using_roles.push_back(parse_role_name());
                    if (!peek_is(TokenKind::Comma)) break;
                    advance();
                }
            }
        }
        return Statement(std::move(show));
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
        // `READ [LOCAL]` (mysqldump writes it): the extra word changes nothing here (MySQL 8.4 no longer takes LOW_PRIORITY WRITE)
        bool exclusive;
        if (mode == "READ") exclusive = false;
        else if (mode == "WRITE") exclusive = true;
        else throw ParseError("Expected READ or WRITE after '" + name + "' in LOCK TABLES");
        advance();
        if (!exclusive && peek_is(TokenKind::Ident) && to_upper(peek()->text) == "LOCAL") advance();
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
    if (t->kind == TokenKind::Password && (peek_at_is(1, TokenKind::For) || peek_at_is(1, TokenKind::Eq))) { advance(); return parse_set_password(); }
    if (t->kind == TokenKind::Role) { advance(); return parse_set_role(); }
    if (t->kind == TokenKind::Default && peek_at_is(1, TokenKind::Role)) { advance(); advance(); return parse_set_default_role(); }
    auto is_word = [&](const Token* tok, const char* w) { return tok && tok->kind == TokenKind::Ident && to_upper(tok->text) == w; };
    auto is_scope = [&](const Token* tok) {
        return tok && tok->kind == TokenKind::Ident &&
               (to_upper(tok->text) == "SESSION" || to_upper(tok->text) == "GLOBAL" || to_upper(tok->text) == "LOCAL" || to_upper(tok->text) == "PERSIST" ||
                to_upper(tok->text) == "PERSIST_ONLY");
    };
    // [SESSION | GLOBAL] TRANSACTION ISOLATION LEVEL x | READ ONLY | READ WRITE [, ...]; without SESSION it is for the next transaction only
    auto is_characteristic = [&](std::size_t at) { return peek_at_is(at, TokenKind::Isolation) || (is_word(peek_at(at), "READ") && !peek_at_is(at + 1, TokenKind::Eq)); };
    bool transaction_form = false, next_only = false;
    if (is_scope(t) && is_word(peek_at(1), "TRANSACTION") && is_characteristic(2)) { advance(); advance(); transaction_form = true; }
    else if (is_word(t, "TRANSACTION") && is_characteristic(1)) { advance(); transaction_form = true; next_only = true; }
    else if (peek_is(TokenKind::Isolation)) transaction_form = true;

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

    if (transaction_form) {
        Statement::SetIsolationLevel set{IsolationLevel::ReadCommitted, next_only, false, std::nullopt};
        for (;;) {
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
                set.level = level;
                set.has_level = true;
            } else if (is_word(peek(), "READ")) {
                advance();
                const Token* mode = advance();
                const std::string word = mode ? to_upper(!mode->text.empty() ? mode->text : mode->word) : "";
                if (word == "ONLY") set.read_only = true;
                else if (word == "WRITE") set.read_only = false;
                else throw ParseError("Expected ONLY or WRITE after READ");
            } else {
                throw ParseError("Expected ISOLATION LEVEL, READ ONLY or READ WRITE");
            }
            if (!peek_is(TokenKind::Comma)) break;
            advance();
        }
        return Statement(set);
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

namespace {
// the word a token stands for: an identifier / string as typed, a keyword as it was written
std::string token_text(const Token* t) { return !t->text.empty() ? t->text : t->word; }
} // namespace

// 'user'@'host' | user | USER() | CURRENT_USER() (then `current`)
std::pair<std::string, std::string> Parser::parse_account_or_current(bool& current) {
    current = false;
    const Token* t = peek();
    if (t && (t->kind == TokenKind::User || (t->kind == TokenKind::Ident && to_upper(t->text) == "CURRENT_USER")) && peek_at_is(1, TokenKind::LParen)) {
        advance();
        advance();
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')'");
        advance();
        current = true;
        return {"", ""};
    }
    if (t && t->kind == TokenKind::Ident && to_upper(t->text) == "CURRENT_USER") { // CURRENT_USER without the parentheses
        advance();
        current = true;
        return {"", ""};
    }
    return parse_user_spec();
}

std::string Parser::parse_role_name() {
    const Token* t = advance();
    if (!t || (t->text.empty() && t->word.empty())) throw ParseError("Expected role name");
    std::string name = token_text(t);
    if (peek_is(TokenKind::At)) { // 'r'@'host': a role has no host here
        advance();
        if (!advance()) throw ParseError("Expected hostname after @");
    }
    return name;
}

// CREATE USER [IF NOT EXISTS] 'user'@'host' [IDENTIFIED BY 'password'] [, 'user2'@'host2' [IDENTIFIED BY 'password2']] ...
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
    auto one = [&]() {
        Statement::CreateUser u;
        auto [user, host] = parse_user_spec();
        u.user = user;
        u.host = host;
        u.if_not_exists = if_not_exists;
        if (peek_is(TokenKind::Identified)) {
            advance(); // IDENTIFIED
            const Token* by = advance();
            bool ok = by && (by->kind == TokenKind::By || by->kind == TokenKind::Ident);
            if (!ok) throw ParseError("Expected BY");
            const Token* p = advance();
            if (!p || p->kind != TokenKind::StringLit) throw ParseError("Expected password string");
            u.password = p->text;
        }
        return u;
    };
    Statement::CreateUser first = one();
    while (peek_is(TokenKind::Comma)) {
        advance();
        first.more_users.push_back(one());
    }
    return Statement(std::move(first));
}

// DROP USER [IF EXISTS] 'user'@'host' [, ...]
Statement Parser::parse_drop_user() {
    bool if_exists = false;
    if (peek_is(TokenKind::If)) {
        advance();
        if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS");
        advance();
        if_exists = true;
    }
    auto [user, host] = parse_user_spec();
    Statement::DropUser drop{user, host, if_exists, {}};
    while (peek_is(TokenKind::Comma)) {
        advance();
        drop.more_users.push_back(parse_user_spec());
    }
    return Statement(std::move(drop));
}

// ALTER USER [IF EXISTS] account IDENTIFIED BY 'password'    (account: 'u'@'h' | USER() | CURRENT_USER())
Statement Parser::parse_alter_user() {
    Statement::AlterUser alter;
    if (peek_is(TokenKind::If)) {
        advance();
        if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS");
        advance();
        alter.if_exists = true;
    }
    auto [user, host] = parse_account_or_current(alter.current);
    alter.user = user;
    alter.host = host;
    if (!peek_is(TokenKind::Identified)) throw ParseError("Expected IDENTIFIED BY after ALTER USER account");
    advance();
    const Token* by = advance();
    if (!(by && (by->kind == TokenKind::By || by->kind == TokenKind::Ident))) throw ParseError("Expected BY");
    const Token* p = advance();
    if (!p || p->kind != TokenKind::StringLit) throw ParseError("Expected password string");
    alter.password = p->text;
    return Statement(std::move(alter));
}

// RENAME USER a TO b [, c TO d ...]
Statement Parser::parse_rename_user() {
    Statement::RenameUser rename;
    for (;;) {
        auto from = parse_user_spec();
        if (!peek_is(TokenKind::To)) throw ParseError("Expected TO in RENAME USER");
        advance();
        auto to = parse_user_spec();
        rename.pairs.emplace_back(from, to);
        if (!peek_is(TokenKind::Comma)) break;
        advance();
    }
    return Statement(std::move(rename));
}

// SET PASSWORD [FOR account] = 'password'    (the word PASSWORD is read)
Statement Parser::parse_set_password() {
    Statement::SetPassword set;
    set.current = true;
    if (peek_is(TokenKind::For)) {
        advance();
        auto [user, host] = parse_account_or_current(set.current);
        set.user = user;
        set.host = host;
    }
    if (!peek_is_assign()) throw ParseError("Expected '=' after SET PASSWORD");
    advance();
    const Token* p = advance();
    if (!p || p->kind != TokenKind::StringLit) throw ParseError("Expected password string");
    set.password = p->text;
    return Statement(std::move(set));
}

// SET ROLE NONE | ALL | ALL EXCEPT r [, ...] | DEFAULT | r [, ...]    (the word ROLE is read)
Statement Parser::parse_set_role() {
    Statement::SetRole set;
    const Token* t = peek();
    if (!t) throw ParseError("Expected role name after SET ROLE");
    const std::string word = to_upper(token_text(t));
    if (t->kind != TokenKind::StringLit && (word == "NONE" || word == "DEFAULT")) {
        advance();
        set.mode = word;
        return Statement(std::move(set));
    }
    if (t->kind != TokenKind::StringLit && (t->kind == TokenKind::All || word == "ALL")) {
        advance();
        set.mode = "ALL";
        if (const Token* e = peek(); e && e->kind != TokenKind::StringLit && to_upper(token_text(e)) == "EXCEPT") {
            advance();
            set.mode = "ALL EXCEPT";
            for (;;) {
                set.roles.push_back(parse_role_name());
                if (!peek_is(TokenKind::Comma)) break;
                advance();
            }
        }
        return Statement(std::move(set));
    }
    set.mode = "LIST";
    for (;;) {
        set.roles.push_back(parse_role_name());
        if (!peek_is(TokenKind::Comma)) break;
        advance();
    }
    return Statement(std::move(set));
}

// SET DEFAULT ROLE NONE | ALL | r [, ...] TO account [, ...]    (DEFAULT ROLE is read)
Statement Parser::parse_set_default_role() {
    Statement::SetDefaultRole set;
    const Token* t = peek();
    if (!t) throw ParseError("Expected role name after SET DEFAULT ROLE");
    const std::string word = to_upper(token_text(t));
    if (t->kind != TokenKind::StringLit && (word == "NONE" || t->kind == TokenKind::All || word == "ALL")) {
        advance();
        set.mode = word == "NONE" ? "NONE" : "ALL";
    } else {
        set.mode = "LIST";
        for (;;) {
            set.roles.push_back(parse_role_name());
            if (!peek_is(TokenKind::Comma)) break;
            advance();
        }
    }
    if (!peek_is(TokenKind::To)) throw ParseError("Expected TO in SET DEFAULT ROLE");
    advance();
    for (;;) {
        set.users.push_back(parse_user_spec());
        if (!peek_is(TokenKind::Comma)) break;
        advance();
    }
    return Statement(std::move(set));
}

// CREATE ROLE [IF NOT EXISTS] role [, role ...]    (ROLE is read)
Statement Parser::parse_create_role() {
    Statement::CreateRole create;
    if (peek_is(TokenKind::If)) {
        advance();
        if (!peek_is(TokenKind::Not)) throw ParseError("Expected NOT");
        advance();
        if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS");
        advance();
        create.if_not_exists = true;
    }
    create.name = parse_role_name();
    while (peek_is(TokenKind::Comma)) {
        advance();
        create.more.push_back(parse_role_name());
    }
    return Statement(std::move(create));
}

// DROP ROLE [IF EXISTS] role [, role ...]    (ROLE is read)
Statement Parser::parse_drop_role() {
    Statement::DropRole drop;
    if (peek_is(TokenKind::If)) {
        advance();
        if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS after IF");
        advance();
        drop.if_exists = true;
    }
    drop.name = parse_role_name();
    while (peek_is(TokenKind::Comma)) {
        advance();
        drop.more.push_back(parse_role_name());
    }
    return Statement(std::move(drop));
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

namespace {
// One item of the list after GRANT / REVOKE: a privilege (words, e.g. CREATE TEMPORARY TABLES, with an optional column list) or a role name.
struct GrantItem {
    std::vector<const Token*> words;
    std::vector<std::string> columns;
};
} // namespace

// GRANT priv [(col, ...)] [, ...] ON [TABLE | DATABASE | PROCEDURE | FUNCTION] object TO account [, ...] [WITH GRANT OPTION]
// GRANT role [, role ...] TO account [, ...] [WITH ADMIN OPTION]            (also the older GRANT ROLE r TO account)
Statement Parser::parse_grant() {
    if (peek_is(TokenKind::Role) && peek_at(1) && (peek_at(1)->kind == TokenKind::Ident || peek_at(1)->kind == TokenKind::StringLit) &&
        (peek_at_is(2, TokenKind::To) || peek_at_is(2, TokenKind::Comma))) {
        advance(); // (GRANT ROLE r TO ...)
    }
    std::vector<GrantItem> items;
    for (;;) {
        GrantItem item;
        for (;;) {
            const Token* t = peek();
            if (!t || t->kind == TokenKind::Comma || t->kind == TokenKind::LParen || t->kind == TokenKind::On || t->kind == TokenKind::To || t->kind == TokenKind::From) break;
            item.words.push_back(advance());
        }
        if (item.words.empty()) throw ParseError("Expected privilege name");
        if (peek_is(TokenKind::LParen)) {
            advance();
            for (;;) {
                item.columns.push_back(expect_ident());
                if (!peek_is(TokenKind::Comma)) break;
                advance();
            }
            if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after the column list");
            advance();
        }
        items.push_back(std::move(item));
        if (peek_is(TokenKind::Comma)) { advance(); continue; }
        break;
    }
    auto accounts = [&](Statement::Grant* grant, Statement::GrantRole* role) {
        auto first = parse_user_spec();
        std::vector<std::pair<std::string, std::string>> more;
        while (peek_is(TokenKind::Comma)) { advance(); more.push_back(parse_user_spec()); }
        if (grant) { grant->user = first.first; grant->host = first.second; grant->more_users = std::move(more); }
        if (role) { role->user = first.first; role->host = first.second; role->more_users = std::move(more); }
    };
    if (peek_is(TokenKind::To)) { // roles
        advance();
        Statement::GrantRole grant;
        for (std::size_t i = 0; i < items.size(); i++) {
            if (items[i].words.size() != 1 || !items[i].columns.empty()) throw ParseError("Expected privilege name");
            std::string name = token_text(items[i].words[0]);
            if (i == 0) grant.role = std::move(name); else grant.more_roles.push_back(std::move(name));
        }
        accounts(nullptr, &grant);
        if (peek_is(TokenKind::With)) {
            advance();
            const Token* t = advance();
            if (!(t && to_upper(token_text(t)) == "ADMIN")) throw ParseError("Expected ADMIN after WITH");
            if (peek_is(TokenKind::OptionKw)) advance();
            grant.with_admin_option = true;
        }
        return Statement(std::move(grant));
    }
    Statement::Grant grant;
    for (auto& item : items) {
        std::string name;
        for (std::size_t i = 0; i < item.words.size(); i++) {
            if (i) name += " ";
            name += to_upper(token_text(item.words[i]));
        }
        if (name == "ALL") name = "ALL PRIVILEGES";
        grant.privileges.push_back(std::move(name));
        grant.privilege_columns.push_back(item.columns);
    }
    if (!peek_is(TokenKind::On)) throw ParseError("Expected ON");
    advance();
    grant.object_type = "TABLE";
    if (peek_is(TokenKind::Table)) { advance(); grant.object_type = "TABLE"; }
    else if (peek_is(TokenKind::Database)) { advance(); grant.object_type = "DATABASE"; }
    else if (peek_is(TokenKind::Databases)) { advance(); grant.object_type = "DATABASES"; }
    else if (peek_is(TokenKind::Procedure)) { advance(); grant.object_type = "PROCEDURE"; }
    else if (const Token* f = peek(); f && f->kind == TokenKind::Ident && to_upper(f->text) == "FUNCTION") { advance(); grant.object_type = "FUNCTION"; }
    grant.object = parse_grant_object();
    if (!peek_is(TokenKind::To)) throw ParseError("Expected TO");
    advance();
    accounts(&grant, nullptr);
    if (peek_is(TokenKind::With)) {
        advance();
        if (peek_is(TokenKind::Grant)) advance();
        if (peek_is(TokenKind::OptionKw)) advance();
        grant.with_grant_option = true;
    }
    return Statement(std::move(grant));
}

// REVOKE priv [(col, ...)] [, ...] ON object FROM account [, ...]   |   REVOKE ALL PRIVILEGES, GRANT OPTION FROM account [, ...]
// REVOKE role [, role ...] FROM account [, ...]                       (also the older REVOKE ROLE r FROM account)
Statement Parser::parse_revoke() {
    if (peek_is(TokenKind::Role) && peek_at(1) && (peek_at(1)->kind == TokenKind::Ident || peek_at(1)->kind == TokenKind::StringLit) &&
        (peek_at_is(2, TokenKind::From) || peek_at_is(2, TokenKind::Comma))) {
        advance();
    }
    std::vector<GrantItem> items;
    for (;;) {
        GrantItem item;
        for (;;) {
            const Token* t = peek();
            if (!t || t->kind == TokenKind::Comma || t->kind == TokenKind::LParen || t->kind == TokenKind::On || t->kind == TokenKind::To || t->kind == TokenKind::From) break;
            item.words.push_back(advance());
        }
        if (item.words.empty()) throw ParseError("Expected privilege name");
        if (peek_is(TokenKind::LParen)) {
            advance();
            for (;;) {
                item.columns.push_back(expect_ident());
                if (!peek_is(TokenKind::Comma)) break;
                advance();
            }
            if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after the column list");
            advance();
        }
        items.push_back(std::move(item));
        if (peek_is(TokenKind::Comma)) { advance(); continue; }
        break;
    }
    auto read_accounts = [&](std::string& user, std::string& host, std::vector<std::pair<std::string, std::string>>& more) {
        auto first = parse_user_spec();
        user = first.first;
        host = first.second;
        while (peek_is(TokenKind::Comma)) { advance(); more.push_back(parse_user_spec()); }
    };
    if (peek_is(TokenKind::From)) { // roles, or REVOKE ALL PRIVILEGES, GRANT OPTION
        advance();
        std::vector<std::string> names;
        for (auto& item : items) {
            std::string name;
            for (std::size_t i = 0; i < item.words.size(); i++) name += (i ? " " : "") + to_upper(token_text(item.words[i]));
            names.push_back(std::move(name));
        }
        const bool is_all = items.size() == 2 && (names[0] == "ALL PRIVILEGES" || names[0] == "ALL") && names[1] == "GRANT OPTION";
        if (is_all) {
            Statement::Revoke revoke;
            revoke.privileges = {"ALL PRIVILEGES", "GRANT OPTION"};
            revoke.all_with_grant_option = true;
            read_accounts(revoke.user, revoke.host, revoke.more_users);
            return Statement(std::move(revoke));
        }
        Statement::RevokeRole revoke;
        for (std::size_t i = 0; i < items.size(); i++) {
            if (items[i].words.size() != 1 || !items[i].columns.empty()) throw ParseError("Expected privilege name");
            std::string name = token_text(items[i].words[0]);
            if (i == 0) revoke.role = std::move(name); else revoke.more_roles.push_back(std::move(name));
        }
        read_accounts(revoke.user, revoke.host, revoke.more_users);
        return Statement(std::move(revoke));
    }
    Statement::Revoke revoke;
    for (auto& item : items) {
        std::string name;
        for (std::size_t i = 0; i < item.words.size(); i++) {
            if (i) name += " ";
            name += to_upper(token_text(item.words[i]));
        }
        if (name == "ALL") name = "ALL PRIVILEGES";
        revoke.privileges.push_back(std::move(name));
        revoke.privilege_columns.push_back(item.columns);
    }
    if (!peek_is(TokenKind::On)) throw ParseError("Expected ON");
    advance();
    revoke.object_type = "TABLE";
    if (peek_is(TokenKind::Table)) { advance(); revoke.object_type = "TABLE"; }
    else if (peek_is(TokenKind::Database)) { advance(); revoke.object_type = "DATABASE"; }
    else if (peek_is(TokenKind::Databases)) { advance(); revoke.object_type = "DATABASES"; }
    else if (peek_is(TokenKind::Procedure)) { advance(); revoke.object_type = "PROCEDURE"; }
    else if (const Token* f = peek(); f && f->kind == TokenKind::Ident && to_upper(f->text) == "FUNCTION") { advance(); revoke.object_type = "FUNCTION"; }
    revoke.object = parse_grant_object();
    if (!peek_is(TokenKind::From)) throw ParseError("Expected FROM");
    advance();
    read_accounts(revoke.user, revoke.host, revoke.more_users);
    return Statement(std::move(revoke));
}

} // namespace engine
