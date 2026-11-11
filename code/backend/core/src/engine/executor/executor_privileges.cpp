// Accounts, privileges and roles as MySQL 8.4 has them (R78): CREATE / ALTER / RENAME / DROP USER, GRANT and REVOKE (global, database, table and column level, WITH GRANT OPTION),
// SHOW GRANTS, roles (CREATE ROLE, GRANT role, SET ROLE, SET DEFAULT ROLE, CURRENT_ROLE()), and the check that a statement a user sends is allowed to run
// (check_privileges, called by execute_with_s_body for the statements of a client; the errors carry MySQL's own numbers: 1142, 1143, 1044, 1045, 1227, 1370, ...).
//
// Who is held to the tables: an account of the user table that is not root. root (and an embedding program that sets no account) is never refused. Statements that run inside
// other statements (a procedure's body, a trigger, the DELETE of a REPLACE, a view's body) run with the rights of their definer and are not looked at again.

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

#include "engine/executor/executor.hpp"
#include "engine/parser/parser.hpp"
#include "engine/result_text.hpp"

namespace engine {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string quoted_name(const std::string& s) { return "`" + s + "`"; }

std::string account_text(const std::string& user, const std::string& host) { return "'" + user + "'@'" + host + "'"; }

std::string role_text(const std::string& name) { return quoted_name(name) + "@" + quoted_name("%"); }

std::string join(const std::vector<std::string>& items, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < items.size(); i++) out += (i ? sep : "") + items[i];
    return out;
}

bool has(const std::vector<std::string>& list, const std::string& item) { return std::find(list.begin(), list.end(), item) != list.end(); }

// the privileges, in the order MySQL lists them
const std::vector<std::string>& static_privileges() {
    static const std::vector<std::string> list = {
        "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP", "RELOAD", "SHUTDOWN", "PROCESS", "FILE", "REFERENCES", "INDEX", "ALTER", "SHOW DATABASES", "SUPER",
        "CREATE TEMPORARY TABLES", "LOCK TABLES", "EXECUTE", "REPLICATION SLAVE", "REPLICATION CLIENT", "CREATE VIEW", "SHOW VIEW", "CREATE ROUTINE", "ALTER ROUTINE",
        "CREATE USER", "EVENT", "TRIGGER", "CREATE TABLESPACE", "CREATE ROLE", "DROP ROLE"};
    return list;
}

// the privileges only the whole server can have (a database or a table can not be given them)
bool global_only(const std::string& p) {
    static const std::set<std::string> list = {"RELOAD", "SHUTDOWN", "PROCESS", "FILE", "SHOW DATABASES", "SUPER", "REPLICATION SLAVE", "REPLICATION CLIENT", "CREATE USER",
                                               "CREATE TABLESPACE", "CREATE ROLE", "DROP ROLE"};
    return list.count(p) != 0;
}

bool table_privilege(const std::string& p) {
    static const std::set<std::string> list = {"SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP", "REFERENCES", "INDEX", "ALTER", "CREATE VIEW", "SHOW VIEW", "TRIGGER"};
    return list.count(p) != 0;
}

bool column_privilege(const std::string& p) { return p == "SELECT" || p == "INSERT" || p == "UPDATE" || p == "REFERENCES"; }

const std::vector<std::string>& dynamic_privileges() {
    static const std::vector<std::string> list = {
        "ALLOW_NONEXISTENT_DEFINER", "APPLICATION_PASSWORD_ADMIN", "AUDIT_ABORT_EXEMPT", "AUDIT_ADMIN", "AUTHENTICATION_POLICY_ADMIN", "BACKUP_ADMIN", "BINLOG_ADMIN",
        "BINLOG_ENCRYPTION_ADMIN", "CLONE_ADMIN", "CONNECTION_ADMIN", "ENCRYPTION_KEY_ADMIN", "FIREWALL_EXEMPT", "FLUSH_OPTIMIZER_COSTS", "FLUSH_PRIVILEGES", "FLUSH_STATUS",
        "FLUSH_TABLES", "FLUSH_USER_RESOURCES", "GROUP_REPLICATION_ADMIN", "GROUP_REPLICATION_STREAM", "INNODB_REDO_LOG_ARCHIVE", "INNODB_REDO_LOG_ENABLE",
        "OPTIMIZE_LOCAL_TABLE", "PASSWORDLESS_USER_ADMIN", "PERSIST_RO_VARIABLES_ADMIN", "REPLICATION_APPLIER", "REPLICATION_SLAVE_ADMIN", "RESOURCE_GROUP_ADMIN",
        "RESOURCE_GROUP_USER", "ROLE_ADMIN", "SENSITIVE_VARIABLES_OBSERVER", "SERVICE_CONNECTION_ADMIN", "SESSION_VARIABLES_ADMIN", "SET_ANY_DEFINER", "SHOW_ROUTINE",
        "SYSTEM_USER", "SYSTEM_VARIABLES_ADMIN", "TABLE_ENCRYPTION_ADMIN", "TELEMETRY_LOG_ADMIN", "TRANSACTION_GTID_TAG", "XA_RECOVER_ADMIN"};
    return list;
}

enum Level { kGlobal = 0, kDatabase = 1, kTable = 2 };

// every privilege ALL stands for at a level
std::vector<std::string> all_for(int level) {
    std::vector<std::string> out;
    for (const std::string& p : static_privileges()) {
        if (level == kGlobal) out.push_back(p);
        else if (level == kTable ? table_privilege(p) : !global_only(p)) out.push_back(p);
    }
    if (level == kGlobal) out.insert(out.end(), dynamic_privileges().begin(), dynamic_privileges().end());
    return out;
}

// where a grant record applies: level, database, table (lower case)
struct Object {
    int level = kGlobal;
    std::string db, table;
};

Object object_of(const GrantRecord& g, const std::string& current_db) {
    const std::string& o = g.object;
    if (o == "*.*" || o == "*") return {kGlobal, "", ""};
    const std::size_t dot = o.find('.');
    if (dot == std::string::npos) return {kTable, lower(current_db), lower(o)};
    const std::string db = lower(o.substr(0, dot)), table = lower(o.substr(dot + 1));
    if (table == "*") return {kDatabase, db, ""};
    return {kTable, db, table};
}

bool routine_grant(const GrantRecord& g) { return g.object_type == "PROCEDURE" || g.object_type == "FUNCTION"; }

// the privileges a record gives (ALL expanded)
std::set<std::string> privileges_of(const GrantRecord& g, int level) {
    std::set<std::string> out;
    for (const std::string& p : g.privileges) {
        if (p == "ALL PRIVILEGES" || p == "ALL") {
            for (const std::string& a : all_for(level)) out.insert(a);
        } else {
            out.insert(p);
        }
    }
    return out;
}

const UserRecord* find_user(const SharedDatabase& s, const std::string& user, const std::string& host) {
    for (const UserRecord& u : s.users) {
        if (u.user == user && u.host == host) return &u;
    }
    return nullptr;
}

bool role_exists(const SharedDatabase& s, const std::string& name) {
    return std::any_of(s.roles.begin(), s.roles.end(), [&](const RoleRecord& r) { return r.name == name; });
}

// the account a login of `user` is (the row the server checked the password of)
const UserRecord* login_account(const SharedDatabase& s, const std::string& user) {
    for (const UserRecord& u : s.users) {
        if (u.user == user && (u.host == "%" || u.host == "localhost" || u.host == "127.0.0.1" || u.host == "::1")) return &u;
    }
    return nullptr;
}

// the roles `names` hold, and the roles those hold, and so on (a role is granted to a role as to an account: the role is the user of the grant)
std::vector<std::string> role_closure(const SharedDatabase& s, const std::vector<std::string>& names) {
    std::vector<std::string> out;
    std::vector<std::string> queue = names;
    while (!queue.empty()) {
        const std::string name = queue.back();
        queue.pop_back();
        if (has(out, name)) continue;
        out.push_back(name);
        for (const RoleGrant& rg : s.role_grants) {
            if (rg.user == name && rg.host == "%" && role_exists(s, name)) queue.push_back(rg.role);
        }
    }
    return out;
}

// What an account (and the roles it has switched on) holds
struct Who {
    const SharedDatabase& s;
    std::string user, host, current_db;
    std::vector<std::string> roles; // the active roles and every role they hold
    std::vector<const GrantRecord*> held;
    bool has_password = false;

    Who(const SharedDatabase& db, std::string u, std::string h, std::string cur, const std::vector<std::string>& active) : s(db), user(std::move(u)), host(std::move(h)), current_db(lower(cur)) {
        if (const UserRecord* rec = find_user(s, user, host)) has_password = rec->password_hash.has_value() || rec->mysql_native_hash.has_value();
        std::vector<std::string> direct;
        for (const std::string& r : active) {
            if (std::any_of(s.role_grants.begin(), s.role_grants.end(), [&](const RoleGrant& rg) { return rg.role == r && rg.user == user && rg.host == host; })) direct.push_back(r);
        }
        roles = role_closure(s, direct);
        for (const GrantRecord& g : s.grants) {
            const bool mine = g.user == user && g.host == host;
            const bool of_role = g.host == "%" && has(roles, g.user) && role_exists(s, g.user);
            if (mine || of_role) held.push_back(&g);
        }
    }

    bool covers(const Object& o, const std::string& db, const std::string& table) const {
        if (o.level == kGlobal) return true;
        if (o.level == kDatabase) return o.db == db;
        return !table.empty() && o.db == db && o.table == table;
    }

    // the privilege at the table (table empty: at the database)
    bool allowed(const std::string& priv, const std::string& db, const std::string& table) const {
        for (const GrantRecord* g : held) {
            if (routine_grant(*g)) continue;
            const Object o = object_of(*g, current_db);
            if (covers(o, db, table) && privileges_of(*g, o.level).count(priv)) return true;
        }
        return false;
    }

    bool grant_option(const std::string& db, const std::string& table) const {
        for (const GrantRecord* g : held) {
            if (routine_grant(*g)) continue;
            if (g->with_grant_option && covers(object_of(*g, current_db), db, table)) return true;
        }
        return false;
    }

    bool column_allowed(const std::string& priv, const std::string& db, const std::string& table, const std::string& column) const {
        if (allowed(priv, db, table)) return true;
        for (const GrantRecord* g : held) {
            if (routine_grant(*g)) continue;
            const Object o = object_of(*g, current_db);
            if (o.level != kTable || o.db != db || o.table != table) continue;
            for (const auto& [p, c] : g->column_privileges) {
                if (p == priv && lower(c) == column) return true;
            }
        }
        return false;
    }

    bool any_column_privilege(const std::string& priv, const std::string& db, const std::string& table) const {
        for (const GrantRecord* g : held) {
            if (routine_grant(*g)) continue;
            const Object o = object_of(*g, current_db);
            if (o.level != kTable || o.db != db || o.table != table) continue;
            for (const auto& entry : g->column_privileges) {
                if (entry.first == priv) return true;
            }
        }
        return false;
    }

    // any privilege that lets the account into the database (table empty) or the table
    bool any_access(const std::string& db, const std::string& table) const {
        for (const GrantRecord* g : held) {
            if (routine_grant(*g)) continue;
            const Object o = object_of(*g, current_db);
            if (o.level == kGlobal) {
                for (const std::string& p : privileges_of(*g, kGlobal)) {
                    if (!global_only(p) && has(static_privileges(), p)) return true;
                }
                continue;
            }
            if (o.db != db) continue;
            if (o.level == kDatabase) {
                if (!privileges_of(*g, kDatabase).empty()) return true;
            } else if (table.empty() || o.table == table) {
                if (!privileges_of(*g, kTable).empty() || !g->column_privileges.empty()) return true;
            }
        }
        return false;
    }

    bool routine_allowed(const std::string& priv, const std::string& db, const std::string& name) const {
        if (allowed(priv, db, "")) return true;
        for (const GrantRecord* g : held) {
            if (!routine_grant(*g)) continue;
            if (lower(g->object) == db + "." + name && has(g->privileges, priv)) return true;
        }
        return false;
    }

    bool global(const std::string& priv) const { return allowed(priv, "", ""); }
};

// the errors a client gets; the text is MySQL's
std::string error_text(int code, const char* state, const std::string& text) { return "ERROR " + std::to_string(code) + " (" + state + "): " + text; }

std::string table_denied(const Who& w, const std::string& priv, const std::string& table) {
    return error_text(1142, "42000", priv + " command denied to user '" + w.user + "'@'localhost' for table '" + table + "'");
}
std::string column_denied(const Who& w, const std::string& priv, const std::string& column, const std::string& table) {
    return error_text(1143, "42000", priv + " command denied to user '" + w.user + "'@'localhost' for column '" + column + "' in table '" + table + "'");
}
std::string database_denied(const Who& w, const std::string& db) { return error_text(1044, "42000", "Access denied for user '" + w.user + "'@'" + w.host + "' to database '" + db + "'"); }
std::string access_denied(const Who& w) {
    return error_text(1045, "28000", "Access denied for user '" + w.user + "'@'" + w.host + "' (using password: " + (w.has_password ? "YES" : "NO") + ")");
}
std::string need_privileges(const std::string& list) { return error_text(1227, "42000", "Access denied; you need (at least one of) the " + list + " privilege(s) for this operation"); }
std::string routine_denied(const Who& w, const std::string& priv, const std::string& db, const std::string& name) {
    return error_text(1370, "42000", priv + " command denied to user '" + w.user + "'@'" + w.host + "' for routine '" + db + "." + name + "'");
}

struct Denied {
    std::string text;
};

// the words (lower case) of a piece of SQL text that may name a column
void add_words(const std::string& text, std::set<std::string>& out) {
    std::size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (std::isalpha(c) || c == '_') {
            std::size_t j = i;
            while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_' || text[j] == '$')) j++;
            out.insert(lower(text.substr(i, j - i)));
            i = j;
        } else {
            i++;
        }
    }
}

// What a query reads: the words that may be column names, and whether it takes every column (*)
struct Reads {
    std::set<std::string> words;
    bool star = false;
};

class Checker {
public:
    Checker(const Who& who, const SharedDatabase& s) : w_(who), s_(s) {}

    std::optional<std::string> run(const Statement& stmt) {
        try {
            visit(stmt);
        } catch (const Denied& d) {
            return d.text;
        }
        return std::nullopt;
    }

    // CHECK TABLE / OPTIMIZE TABLE (statements the engine reads as text): SELECT, and for OPTIMIZE also INSERT, on each table
    std::optional<std::string> table_access(const std::string& name, bool write) {
        try {
            need_select(name, Reads{{}, true}, true);
            if (write) need_table("INSERT", name);
        } catch (const Denied& d) {
            return d.text;
        }
        return std::nullopt;
    }

private:
    const Who& w_;
    const SharedDatabase& s_;
    std::set<std::string> locals_; // names of the CTEs in scope

    std::pair<std::string, std::string> split(const std::string& name) const {
        const std::size_t dot = name.find('.');
        if (dot == std::string::npos) return {w_.current_db, lower(name)};
        return {lower(name.substr(0, dot)), lower(name.substr(dot + 1))};
    }

    static bool open_schema(const std::string& db) { return db == "information_schema"; }

    [[noreturn]] void deny(std::string text) const { throw Denied{std::move(text)}; }

    void need_table(const std::string& priv, const std::string& name) const {
        const auto [db, table] = split(name);
        if (open_schema(db) || table.empty() || table == "_dual_") return;
        if (!w_.allowed(priv, db, table)) deny(table_denied(w_, priv, table));
    }

    void need_database(const std::string& priv, const std::string& db) const {
        if (!w_.allowed(priv, lower(db), "")) deny(database_denied(w_, lower(db)));
    }

    // the table's columns, lower case, in order (none for a view, or a table that is not there)
    std::vector<std::string> columns_of(const std::string& db, const std::string& table) const {
        std::vector<std::string> out;
        if (const TableSchema* schema = s_.catalog.get_table(db + "." + table)) {
            for (const ColumnDef& c : schema->columns) out.push_back(lower(c.name));
        }
        return out;
    }

    // SELECT on a table for the columns a statement reads. `reading`: a query (a table needs some SELECT privilege even when no column is read); an UPDATE or DELETE needs none then.
    void need_select(const std::string& name, const Reads& reads, bool reading) const {
        const auto [db, table] = split(name);
        if (open_schema(db) || table.empty() || table == "_dual_") return;
        if (w_.allowed("SELECT", db, table)) return;
        const bool by_column = w_.any_column_privilege("SELECT", db, table);
        if (reading && !by_column) deny(table_denied(w_, "SELECT", table));
        const std::vector<std::string> columns = columns_of(db, table);
        if (columns.empty()) {
            if (reading || !reads.words.empty()) deny(table_denied(w_, "SELECT", table));
            return;
        }
        for (const std::string& c : columns) {
            if ((reads.star && reading) || reads.words.count(c)) {
                if (!w_.column_allowed("SELECT", db, table, c)) deny(column_denied(w_, "SELECT", c, table));
            }
        }
    }

    // INSERT / UPDATE (REFERENCES) on the named columns of a table; empty: every column
    void need_columns(const std::string& priv, const std::string& name, const std::vector<std::string>& named) const {
        const auto [db, table] = split(name);
        if (open_schema(db) || table.empty()) return;
        if (w_.allowed(priv, db, table)) return;
        if (!w_.any_column_privilege(priv, db, table)) deny(table_denied(w_, priv, table));
        std::vector<std::string> wanted;
        for (const std::string& c : named) wanted.push_back(lower(c.substr(c.rfind('.') == std::string::npos ? 0 : c.rfind('.') + 1)));
        if (wanted.empty()) wanted = columns_of(db, table);
        for (const std::string& c : wanted) {
            if (!w_.column_allowed(priv, db, table, c)) deny(column_denied(w_, priv, c, table));
        }
    }

    // some privilege on the table or one of its columns (DESCRIBE, SHOW INDEX, SHOW CREATE TABLE)
    void need_any(const std::string& name, const std::string& named_priv) const {
        const auto [db, table] = split(name);
        if (open_schema(db) || table.empty()) return;
        if (!w_.any_access(db, table)) deny(table_denied(w_, named_priv, table));
    }

    // ---- what the expressions of a query read; a subquery inside is checked as a query of its own ----
    void add_text(const std::string& text, Reads& r) const { add_words(text, r.words); }

    void arith(const ArithExpr& e, Reads& r) {
        if (auto* c = std::get_if<ArithExpr::Col>(&e.data)) { add_text(c->name, r); return; }
        if (auto* a = std::get_if<ArithExpr::Add>(&e.data)) { arith(*a->lhs, r); arith(*a->rhs, r); return; }
        if (auto* a = std::get_if<ArithExpr::Sub>(&e.data)) { arith(*a->lhs, r); arith(*a->rhs, r); return; }
        if (auto* a = std::get_if<ArithExpr::Mul>(&e.data)) { arith(*a->lhs, r); arith(*a->rhs, r); return; }
        if (auto* a = std::get_if<ArithExpr::Div>(&e.data)) { arith(*a->lhs, r); arith(*a->rhs, r); return; }
        if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) { for (const ArithExpr& x : f->args) arith(x, r); return; }
        if (auto* c = std::get_if<ArithExpr::Cmp>(&e.data)) { arith(*c->lhs, r); arith(*c->rhs, r); return; }
        if (auto* p = std::get_if<ArithExpr::Pred>(&e.data)) { if (p->cond) cond(*p->cond, r); return; }
        if (auto* q = std::get_if<ArithExpr::Subquery>(&e.data)) { if (q->query) query(*q->query); }
    }

    void cond(const CondExpr& e, Reads& r) {
        if (auto* a = std::get_if<CondExpr::And>(&e.data)) { cond(*a->lhs, r); cond(*a->rhs, r); return; }
        if (auto* o = std::get_if<CondExpr::Or>(&e.data)) { cond(*o->lhs, r); cond(*o->rhs, r); return; }
        if (auto* n = std::get_if<CondExpr::Not>(&e.data)) { cond(*n->inner, r); return; }
        const Condition& c = std::get<CondExpr::Leaf>(e.data).condition;
        arith(c.left, r);
        if (auto* lit = std::get_if<ConditionValue::Literal>(&c.value.data)) { if (!lit->quoted) add_text(lit->value, r); return; }
        if (auto* sub = std::get_if<ConditionValue::Subquery>(&c.value.data)) { if (sub->query) query(*sub->query); return; }
        if (auto* between = std::get_if<ConditionValue::Between>(&c.value.data)) {
            if (!between->lo_quoted) add_text(between->lo, r);
            if (!between->hi_quoted) add_text(between->hi, r);
            return;
        }
        if (auto* list = std::get_if<ConditionValue::LiteralList>(&c.value.data)) {
            for (std::size_t i = 0; i < list->values.size(); i++) {
                if (i >= list->quoted.size() || !list->quoted[i]) add_text(list->values[i], r);
            }
            return;
        }
        if (auto* a = std::get_if<ConditionValue::Arith>(&c.value.data)) arith(a->expr, r);
    }

    void select_column(const SelectColumn& c, Reads& r) {
        if (std::get_if<SelectColumn::All>(&c.data)) { r.star = true; return; }
        if (auto* x = std::get_if<SelectColumn::Column>(&c.data)) { add_text(x->name, r); return; }
        if (auto* x = std::get_if<SelectColumn::ColumnAlias>(&c.data)) { add_text(x->name, r); return; }
        if (auto* x = std::get_if<SelectColumn::Agg>(&c.data)) { add_text(x->col, r); if (x->filter) cond(*x->filter, r); return; }
        if (auto* x = std::get_if<SelectColumn::AggAlias>(&c.data)) { add_text(x->col, r); if (x->filter) cond(*x->filter, r); return; }
        if (auto* x = std::get_if<SelectColumn::Func>(&c.data)) { for (const std::string& a : x->args) add_text(a, r); return; }
        if (auto* x = std::get_if<SelectColumn::Expr>(&c.data)) { arith(x->expr, r); return; }
        if (auto* x = std::get_if<SelectColumn::CaseWhen>(&c.data)) {
            for (const CaseWhenBranch& b : x->branches) { cond(b.condition, r); add_text(b.result, r); }
            if (x->else_val) add_text(*x->else_val, r);
            return;
        }
        if (auto* x = std::get_if<SelectColumn::WinFunc>(&c.data)) {
            if (x->col) add_text(*x->col, r);
            for (const std::string& p : x->partition_by) add_text(p, r);
            for (const OrderBy& o : x->order_by) add_text(o.column, r);
            if (x->default_value) add_text(*x->default_value, r);
            return;
        }
        if (auto* x = std::get_if<SelectColumn::Subquery>(&c.data)) { if (x->query) query(*x->query); }
    }

    void select_columns(const std::vector<SelectColumn>& columns, Reads& r) {
        for (const SelectColumn& c : columns) select_column(c, r);
    }

    // ---- queries ----
    void select(const Statement::Select& sel) {
        Reads reads;
        select_columns(sel.columns, reads);
        if (sel.condition) cond(*sel.condition, reads);
        if (sel.having) cond(*sel.having, reads);
        if (sel.group_by) { for (const std::string& g : *sel.group_by) add_text(g, reads); }
        for (const OrderBy& o : sel.order_by) add_text(o.column, reads);
        std::vector<std::string> tables;
        if (sel.subquery) {
            if (sel.subquery->first) query(*sel.subquery->first);
        } else if (sel.table != "_dual_" && !locals_.count(lower(sel.table))) {
            tables.push_back(sel.table);
        }
        for (const Join& j : sel.joins) {
            cond(j.on_expr, reads);
            for (const std::string& u : j.using_cols) add_text(u, reads);
            if (j.subquery) {
                if (j.subquery->first) query(*j.subquery->first);
            } else if (!locals_.count(lower(j.table))) {
                tables.push_back(j.table);
            }
        }
        for (const std::string& t : tables) need_select(t, reads, true);
        if (sel.for_update || sel.for_share) {
            for (const std::string& t : tables) {
                const auto [db, table] = split(t);
                if (!w_.allowed("UPDATE", db, table) && !w_.allowed("DELETE", db, table) && !w_.allowed("LOCK TABLES", db, table)) {
                    deny(table_denied(w_, "SELECT with locking clause", table));
                }
            }
        }
    }

    void query(const Statement& st) {
        if (auto* sel = std::get_if<Statement::Select>(&st.data)) { select(*sel); return; }
        if (auto* u = std::get_if<Statement::Union>(&st.data)) { query(*u->left); query(*u->right); return; }
        if (auto* u = std::get_if<Statement::Intersect>(&st.data)) { query(*u->left); query(*u->right); return; }
        if (auto* u = std::get_if<Statement::Except>(&st.data)) { query(*u->left); query(*u->right); return; }
        if (auto* with = std::get_if<Statement::With>(&st.data)) { on(*with); return; }
        visit(st);
    }

    // ---- statements ----
    void visit(const Statement& st) {
        std::visit([&](const auto& v) { on(v); }, st.data);
    }

    template <class T> void on(const T&) {}

    void on(const Statement::Select& v) { select(v); }
    void on(const Statement::Union& v) { query(*v.left); query(*v.right); }
    void on(const Statement::Intersect& v) { query(*v.left); query(*v.right); }
    void on(const Statement::Except& v) { query(*v.left); query(*v.right); }
    void on(const Statement::With& v) {
        for (const auto& cte : v.ctes) {
            locals_.insert(lower(cte.first));
            if (cte.second) query(*cte.second);
        }
        if (v.query) query(*v.query);
    }
    void on(const Statement::SelectInto& v) { if (v.query) query(*v.query); }
    void on(const Statement::Explain& v) { if (v.inner) visit(*v.inner); }
    void on(const Statement::ExplainAnalyze& v) { if (v.inner) visit(*v.inner); }

    void on_conflict(const InsertConflict& c, const std::string& table, Reads& reads) {
        if (auto* up = std::get_if<InsertConflict::Update>(&c.data)) {
            std::vector<std::string> assigned;
            for (const auto& a : up->assignments) {
                assigned.push_back(a.first);
                arith(a.second, reads);
            }
            need_columns("UPDATE", table, assigned);
            need_select(table, reads, false);
        } else if (std::holds_alternative<InsertConflict::Replace>(c.data)) {
            need_table("DELETE", table);
        }
    }

    void on(const Statement::Insert& v) {
        need_columns("INSERT", v.table, v.columns ? *v.columns : std::vector<std::string>{});
        Reads reads;
        on_conflict(v.on_conflict, v.table, reads);
    }
    void on(const Statement::InsertSelect& v) {
        need_columns("INSERT", v.table, v.columns ? *v.columns : std::vector<std::string>{});
        Reads reads;
        on_conflict(v.on_conflict, v.table, reads);
        if (v.query) query(*v.query);
    }
    void on(const Statement::Update& v) {
        std::vector<std::string> assigned;
        Reads reads;
        for (const auto& a : v.assignments) {
            assigned.push_back(a.first);
            arith(a.second, reads);
        }
        if (v.condition) cond(*v.condition, reads);
        need_columns("UPDATE", v.table, assigned);
        need_select(v.table, reads, false);
    }
    void on(const Statement::Delete& v) {
        need_table("DELETE", v.table);
        Reads reads;
        if (v.condition) cond(*v.condition, reads);
        need_select(v.table, reads, false);
    }

    // the table an assignment target (`t.c`, `db.t.c`, `alias.c`, `c`) belongs to, among the tables of the statement
    std::string target_table(const std::string& target, const std::vector<std::string>& tables, const std::vector<std::pair<std::string, std::string>>& aliases) const {
        const std::size_t dot = target.rfind('.');
        if (dot == std::string::npos) {
            for (const std::string& t : tables) {
                const auto [db, table] = split(t);
                const auto columns = columns_of(db, table);
                if (has(columns, lower(target))) return t;
            }
            return tables.empty() ? std::string() : tables.front();
        }
        const std::string qualifier = lower(target.substr(0, dot));
        for (const auto& [alias, table] : aliases) {
            if (lower(alias) == qualifier) return table;
        }
        for (const std::string& t : tables) {
            const std::string name = lower(t);
            if (name == qualifier || name.substr(name.rfind('.') == std::string::npos ? 0 : name.rfind('.') + 1) == qualifier.substr(qualifier.rfind('.') == std::string::npos ? 0 : qualifier.rfind('.') + 1)) return t;
        }
        return tables.empty() ? std::string() : tables.front();
    }

    void on(const Statement::MultiUpdate& v) {
        std::vector<std::string> tables = v.tables;
        for (const Join& j : v.joins) tables.push_back(j.table);
        std::map<std::string, std::vector<std::string>> assigned;
        Reads reads;
        for (const auto& a : v.assignments) {
            assigned[target_table(a.first, tables, v.table_aliases)].push_back(a.first);
            arith(a.second, reads);
        }
        if (v.condition) cond(*v.condition, reads);
        for (const Join& j : v.joins) cond(j.on_expr, reads);
        // (the tables that are only read come first: MySQL names the table it may not read before a column of the table it updates)
        for (const std::string& t : tables) {
            if (!assigned.count(t)) need_select(t, reads, true);
        }
        for (const auto& [table, columns] : assigned) need_columns("UPDATE", table, columns);
        for (const std::string& t : tables) {
            if (assigned.count(t)) need_select(t, reads, false);
        }
    }
    void on(const Statement::MultiDelete& v) {
        std::vector<std::string> tables = {v.from_table};
        for (const Join& j : v.joins) tables.push_back(j.table);
        Reads reads;
        if (v.condition) cond(*v.condition, reads);
        for (const Join& j : v.joins) cond(j.on_expr, reads);
        for (const std::string& t : v.delete_tables) need_table("DELETE", t);
        for (const std::string& t : tables) need_select(t, reads, !has(v.delete_tables, t));
    }
    void on(const Statement::Merge& v) {
        Reads reads;
        cond(v.on, reads);
        need_select(v.target, reads, true);
        need_select(v.source, reads, true);
        if (v.when_matched_update) need_table("UPDATE", v.target);
        if (v.when_matched_delete) need_table("DELETE", v.target);
        if (v.when_not_matched_columns || !v.when_not_matched_values.empty()) need_table("INSERT", v.target);
    }

    void on(const Statement::CreateTable& v) { need_table("CREATE", v.name); }
    void on(const Statement::CreateTableLike& v) { need_table("CREATE", v.name); need_select(v.source, Reads{{}, true}, true); }
    void on(const Statement::CreateTableAs& v) {
        need_table("CREATE", v.name);
        need_table("INSERT", v.name);
        if (v.query) query(*v.query);
    }
    void on(const Statement::DropTable& v) {
        need_table("DROP", v.name);
        for (const std::string& t : v.more) need_table("DROP", t);
    }
    void on(const Statement::TruncateTable& v) { need_table("DROP", v.name); }
    void on(const Statement::RenameTable& v) {
        for (const auto& [from, to] : v.pairs) {
            need_table("ALTER", from);
            need_table("DROP", from);
            need_table("CREATE", to);
            need_table("INSERT", to);
        }
    }
    void on(const Statement::AlterTable& v) {
        need_table("ALTER", v.table);
        auto rename = [&](const AlterAction& a) {
            if (auto* r = std::get_if<AlterAction::RenameTable>(&a.data)) {
                need_table("DROP", v.table);
                need_table("CREATE", r->to);
                need_table("INSERT", r->to);
            }
        };
        rename(v.action);
        for (const AlterAction& a : v.more) rename(a);
    }
    void on(const Statement::CreateIndex& v) { need_table("INDEX", v.table); }
    void on(const Statement::DropIndex& v) { if (v.table) need_table("INDEX", *v.table); }
    void on(const Statement::CreateView& v) {
        need_table("CREATE VIEW", v.name);
        if (v.query) query(*v.query);
    }
    void on(const Statement::DropView& v) { need_table("DROP", v.name); }
    void on(const Statement::AnalyzeTable& v) { need_table("SELECT", v.table); need_table("INSERT", v.table); }

    void on(const Statement::ShowTables& v) {
        const std::string db = v.database ? lower(*v.database) : w_.current_db;
        if (db.empty() || open_schema(db)) return;
        if (!w_.any_access(db, "")) deny(database_denied(w_, db));
    }
    void on(const Statement::Describe& v) { need_any(v.table, "SELECT"); }
    void on(const Statement::ShowIndex& v) { need_any(v.table, "SELECT"); }
    void on(const Statement::ShowCreateTable& v) { need_any(v.table, "SHOW"); }
    void on(const Statement::ShowCreateView& v) { need_table("SELECT", v.view); need_table("SHOW VIEW", v.view); }
    void on(const Statement::Use& v) {
        const std::string db = lower(v.database);
        if (!open_schema(db) && !w_.any_access(db, "")) deny(database_denied(w_, db));
    }
    void on(const Statement::CreateDatabase& v) { need_database("CREATE", v.name); }
    void on(const Statement::DropDatabase& v) { need_database("DROP", v.name); }
    void on(const Statement::LockTables& v) {
        for (const auto& entry : v.tables) {
            const auto [db, table] = split(entry.first);
            if (!w_.allowed("LOCK TABLES", db, "")) deny(database_denied(w_, db));
            need_any(entry.first, "SELECT");
        }
    }
    void on(const Statement::PrepareStmt& v) {
        Parser parser(v.query, false);
        if (!parser.has_tokens()) return;
        auto parsed = parser.parse();
        if (parsed.is_ok()) {
            Statement inner = std::move(parsed).value();
            visit(inner);
        }
    }

    // routines
    std::pair<std::string, std::string> routine_name(const std::string& name) const { return split(name); }
    void on(const Statement::CallProcedure& v) {
        const auto [db, name] = routine_name(v.name);
        if (!w_.routine_allowed("EXECUTE", db, name)) deny(routine_denied(w_, "execute", db, name));
    }
    void on(const Statement::CreateProcedure& v) { need_database("CREATE ROUTINE", split(v.name).first); }
    void on(const Statement::CreateFunction& v) { need_database("CREATE ROUTINE", split(v.name).first); }
    void on(const Statement::DropProcedure& v) {
        const auto [db, name] = routine_name(v.name);
        if (!w_.routine_allowed("ALTER ROUTINE", db, name)) deny(routine_denied(w_, "alter routine", db, name));
    }
    void on(const Statement::DropFunction& v) {
        const auto [db, name] = routine_name(v.name);
        if (!w_.routine_allowed("ALTER ROUTINE", db, name)) deny(routine_denied(w_, "alter routine", db, name));
    }
    void on(const Statement::CreateTrigger& v) { need_table("TRIGGER", v.table); }
    void on(const Statement::DropTrigger& v) {
        std::string key = v.name.find('.') == std::string::npos ? w_.current_db + "." + v.name : v.name;
        auto it = s_.triggers.find(key);
        if (it != s_.triggers.end()) need_table("TRIGGER", std::get<0>(it->second));
    }
    void on(const Statement::Backup&) { if (!w_.global("FILE")) deny(need_privileges("FILE")); }
    void on(const Statement::Restore&) { if (!w_.global("FILE")) deny(need_privileges("FILE")); }

    // accounts and roles
    bool is_me(const std::string& user, const std::string& host) const { return user == w_.user && host == w_.host; }
    void need_create_user() const { if (!w_.global("CREATE USER")) deny(need_privileges("CREATE USER")); }

    void on(const Statement::CreateUser&) { need_create_user(); }
    void on(const Statement::DropUser&) { need_create_user(); }
    void on(const Statement::RenameUser&) { need_create_user(); }
    void on(const Statement::AlterUser& v) { if (!v.current && !is_me(v.user, v.host)) need_create_user(); }
    void on(const Statement::SetPassword& v) {
        if (!v.current && !is_me(v.user, v.host) && !w_.allowed("UPDATE", "mysql", "user")) deny(database_denied(w_, "mysql"));
    }
    void on(const Statement::CreateRole&) {
        if (!w_.global("CREATE USER") && !w_.global("CREATE ROLE")) deny(need_privileges("CREATE USER, CREATE ROLE"));
    }
    void on(const Statement::DropRole&) {
        if (!w_.global("CREATE USER") && !w_.global("DROP ROLE")) deny(need_privileges("CREATE USER, DROP ROLE"));
    }
    bool may_administer(const std::string& role) const {
        if (w_.global("ROLE_ADMIN") || w_.global("SUPER")) return true;
        return std::any_of(s_.role_grants.begin(), s_.role_grants.end(), [&](const RoleGrant& rg) {
            return rg.role == role && rg.user == w_.user && rg.host == w_.host && rg.with_admin_option;
        });
    }
    void on(const Statement::GrantRole& v) {
        std::vector<std::string> roles = {v.role};
        roles.insert(roles.end(), v.more_roles.begin(), v.more_roles.end());
        for (const std::string& r : roles) {
            if (!may_administer(r)) deny(need_privileges("WITH ADMIN, ROLE_ADMIN, SUPER"));
        }
    }
    void on(const Statement::RevokeRole& v) {
        std::vector<std::string> roles = {v.role};
        roles.insert(roles.end(), v.more_roles.begin(), v.more_roles.end());
        for (const std::string& r : roles) {
            if (!may_administer(r)) deny(need_privileges("WITH ADMIN, ROLE_ADMIN, SUPER"));
        }
    }
    void on(const Statement::ShowGrants& v) {
        if (v.user && !is_me(*v.user, v.host.value_or("%"))) deny(table_denied(w_, "SELECT", "user"));
    }

    // GRANT / REVOKE: the account needs the GRANT OPTION where it gives privileges, and has to hold them itself
    void grant_gate(const std::string& object, const std::string& object_type, const std::vector<std::string>& privileges) const {
        std::string db, table;
        int level = kGlobal;
        if (object != "*.*") {
            const std::size_t dot = object.find('.');
            if (object == "*") {
                db = w_.current_db;
                level = kDatabase;
            } else if (dot == std::string::npos) {
                if (object_type == "DATABASE") { db = lower(object); level = kDatabase; }
                else { db = w_.current_db; table = lower(object); level = kTable; }
            } else {
                db = lower(object.substr(0, dot));
                table = lower(object.substr(dot + 1));
                if (table == "*") { table.clear(); level = kDatabase; } else level = kTable;
            }
        }
        auto refuse = [&]() {
            if (level == kGlobal) deny(access_denied(w_));
            if (level == kDatabase) deny(database_denied(w_, db));
            deny(table_denied(w_, "GRANT", table));
        };
        if (object_type == "PROCEDURE" || object_type == "FUNCTION") return;
        if (!w_.grant_option(db, table)) refuse();
        for (const std::string& p : privileges) {
            if (p == "GRANT OPTION" || p == "USAGE") continue;
            if (p == "ALL PRIVILEGES" || p == "ALL") {
                for (const std::string& a : all_for(level)) {
                    if (!w_.allowed(a, db, table)) refuse();
                }
            } else if (!w_.allowed(p, db, table)) {
                refuse();
            }
        }
    }
    void on(const Statement::Grant& v) { grant_gate(v.object, v.object_type, v.privileges); }
    void on(const Statement::Revoke& v) {
        if (v.all_with_grant_option) { // REVOKE ALL PRIVILEGES, GRANT OPTION FROM account
            if (!w_.global("CREATE USER") && !w_.grant_option("", "")) deny(access_denied(w_));
            return;
        }
        grant_gate(v.object, v.object_type, v.privileges);
    }
};

// rows to the text table the clients read
std::string box(const std::vector<std::string>& headers, const std::vector<std::vector<std::string>>& rows) {
    std::vector<std::string> head;
    std::vector<std::size_t> width;
    for (const std::string& h : headers) {
        head.push_back(result_text::escape_cell(h));
        width.push_back(head.back().size());
    }
    std::vector<std::vector<std::string>> cells;
    for (const auto& row : rows) {
        cells.emplace_back();
        for (std::size_t c = 0; c < row.size(); c++) {
            cells.back().push_back(result_text::escape_cell(row[c]));
            width[c] = std::max(width[c], cells.back().back().size());
        }
    }
    std::string bar = "+";
    for (std::size_t w : width) bar += std::string(w + 2, '-') + "+";
    auto line = [&](const std::vector<std::string>& r) {
        std::string out = "|";
        for (std::size_t c = 0; c < r.size(); c++) out += " " + r[c] + std::string(width[c] - r[c].size(), ' ') + " |";
        return out + "\n";
    };
    std::string out = bar + "\n" + line(head) + bar + "\n";
    for (const auto& r : cells) out += line(r);
    return out + bar;
}

StringResult ok_query() { return StringResult::Ok("Query OK"); }

StringResult fail(int code, const char* state, const std::string& text) { return StringResult::Err(error_text(code, state, text)); }

// the privilege names of a GRANT / REVOKE against the level they are given at; the error text of the first that does not fit
std::optional<std::string> check_privilege_names(const std::vector<std::string>& privileges, const std::vector<std::vector<std::string>>& columns, int level, const std::string& object_name) {
    for (std::size_t i = 0; i < privileges.size(); i++) {
        const std::string& p = privileges[i];
        const bool has_columns = i < columns.size() && !columns[i].empty();
        if (p == "ALL PRIVILEGES" || p == "USAGE" || p == "GRANT OPTION") {
            if (has_columns) return error_text(1064, "42000", "You have an error in your SQL syntax; check the manual that corresponds to your MySQL server version for the right syntax to use");
            continue;
        }
        const bool is_static = has(static_privileges(), p);
        const bool is_dynamic = has(dynamic_privileges(), p);
        if (!is_static && !is_dynamic) {
            if (level == kGlobal) return error_text(1149, "42000", "You have an error in your SQL syntax; check the manual that corresponds to your MySQL server version for the right syntax to use");
            return error_text(3619, "HY000", "Illegal privilege level specified for " + (level == kDatabase ? p : object_name));
        }
        if (has_columns) {
            if (level != kTable) return error_text(1144, "42000", "Illegal GRANT/REVOKE command; please consult the manual to see which privileges can be used");
            if (!column_privilege(p)) return error_text(1064, "42000", "You have an error in your SQL syntax; check the manual that corresponds to your MySQL server version for the right syntax to use");
            continue;
        }
        if (level == kGlobal) continue;
        if (is_dynamic) return error_text(3619, "HY000", "Illegal privilege level specified for " + (level == kDatabase ? p : object_name));
        if (global_only(p)) {
            if (level == kDatabase) return error_text(1221, "HY000", "Incorrect usage of DB GRANT and GLOBAL PRIVILEGES");
            return error_text(1144, "42000", "Illegal GRANT/REVOKE command; please consult the manual to see which privileges can be used");
        }
        if (level == kTable && !table_privilege(p)) return error_text(1144, "42000", "Illegal GRANT/REVOKE command; please consult the manual to see which privileges can be used");
    }
    return std::nullopt;
}

struct Target {
    int level = kGlobal;
    std::string db, table, key; // key: *.*, db.*, db.table (routines: db.name)
};

// the object of a GRANT / REVOKE; an error text when it names no database / table
std::variant<Target, std::string> target_of(const SharedDatabase& s, const std::string& object, const std::string& object_type, const std::string& current_db) {
    Target t;
    if (object == "*.*") {
        t.key = "*.*";
        return t;
    }
    const std::size_t dot = object.find('.');
    if (object == "*" || (dot == std::string::npos && object_type == "DATABASE")) {
        t.level = kDatabase;
        t.db = object == "*" ? lower(current_db) : lower(object);
        if (t.db.empty()) return error_text(1046, "3D000", "No database selected");
        t.key = t.db + ".*";
        return t;
    }
    if (dot == std::string::npos) {
        t.db = lower(current_db);
        if (t.db.empty()) return error_text(1046, "3D000", "No database selected");
        t.table = lower(object);
    } else {
        t.db = lower(object.substr(0, dot));
        t.table = lower(object.substr(dot + 1));
    }
    if (t.table == "*") {
        t.level = kDatabase;
        t.table.clear();
        t.key = t.db + ".*";
        return t;
    }
    t.level = kTable;
    t.key = t.db + "." + t.table;
    (void)s;
    return t;
}

GrantRecord* find_record(SharedDatabase& s, const std::string& user, const std::string& host, const std::string& key, const std::string& object_type, const std::string& current_db) {
    for (GrantRecord& g : s.grants) {
        if (g.user != user || g.host != host) continue;
        const bool routine = routine_grant(g);
        if (routine != (object_type == "PROCEDURE" || object_type == "FUNCTION")) continue;
        if (routine) {
            if (lower(g.object) == key && g.object_type == object_type) return &g;
            continue;
        }
        const Object o = object_of(g, current_db);
        const std::string k = o.level == kGlobal ? "*.*" : o.level == kDatabase ? o.db + ".*" : o.db + "." + o.table;
        if (k == key) return &g;
    }
    return nullptr;
}

// the grants an account holds, for SHOW GRANTS: the account's own and those of `with_roles` (and the roles they hold)
struct Merged {
    std::set<std::string> privileges;
    std::map<std::string, std::set<std::string>> columns; // privilege -> columns
    bool grant_option = false;
};

std::vector<std::string> grants_of(const SharedDatabase& s, const std::string& user, const std::string& host, const std::vector<std::string>& with_roles) {
    std::vector<std::string> principals_roles = role_closure(s, with_roles);
    Merged global;
    std::map<std::string, Merged> databases, tables, routines;
    auto fold = [&](const GrantRecord& g) {
        if (routine_grant(g)) {
            Merged& m = routines[g.object_type + " " + lower(g.object)];
            for (const std::string& p : g.privileges) m.privileges.insert(p);
            m.grant_option = m.grant_option || g.with_grant_option;
            return;
        }
        const Object o = object_of(g, "");
        Merged& m = o.level == kGlobal ? global : o.level == kDatabase ? databases[o.db] : tables[o.db + "." + o.table];
        for (const std::string& p : privileges_of(g, o.level)) m.privileges.insert(p);
        for (const auto& [p, c] : g.column_privileges) m.columns[p].insert(lower(c));
        m.grant_option = m.grant_option || g.with_grant_option;
    };
    const bool is_root = user == "root" && find_user(s, user, host) != nullptr;
    for (const GrantRecord& g : s.grants) {
        if ((g.user == user && g.host == host) || (g.host == "%" && has(principals_roles, g.user) && role_exists(s, g.user))) fold(g);
    }
    if (is_root) {
        for (const std::string& p : all_for(kGlobal)) global.privileges.insert(p);
        global.grant_option = true;
    }
    const std::string to = " TO " + quoted_name(user) + "@" + quoted_name(host);
    auto list = [&](const std::set<std::string>& privileges, int level) {
        std::vector<std::string> names;
        if (level != kGlobal && privileges.size() == all_for(level).size()) return std::string("ALL PRIVILEGES");
        for (const std::string& p : static_privileges()) {
            if (privileges.count(p)) names.push_back(p);
        }
        return names.empty() ? std::string("USAGE") : join(names, ", ");
    };
    std::vector<std::string> lines;
    lines.push_back("GRANT " + list(global.privileges, kGlobal) + " ON *.*" + to + (global.grant_option ? " WITH GRANT OPTION" : ""));
    std::vector<std::string> dynamic;
    for (const std::string& p : dynamic_privileges()) {
        if (global.privileges.count(p)) dynamic.push_back(p);
    }
    if (!dynamic.empty()) lines.push_back("GRANT " + join(dynamic, ",") + " ON *.*" + to + (global.grant_option ? " WITH GRANT OPTION" : ""));
    for (const auto& [db, m] : databases) {
        if (m.privileges.empty() && !m.grant_option) continue;
        lines.push_back("GRANT " + list(m.privileges, kDatabase) + " ON " + quoted_name(db) + ".*" + to + (m.grant_option ? " WITH GRANT OPTION" : ""));
    }
    for (const auto& [key, m] : tables) {
        const std::size_t dot = key.find('.');
        std::vector<std::string> items;
        if (m.privileges.size() == all_for(kTable).size() && m.columns.empty()) {
            items.push_back("ALL PRIVILEGES");
        } else {
            for (const std::string& p : static_privileges()) {
                if (m.privileges.count(p)) items.push_back(p);
                if (auto it = m.columns.find(p); it != m.columns.end() && !it->second.empty()) { // (a privilege of the table and of some columns shows twice, as in MySQL)
                    std::vector<std::string> columns;
                    for (const std::string& c : it->second) columns.push_back(quoted_name(c));
                    items.push_back(p + " (" + join(columns, ", ") + ")");
                }
            }
        }
        lines.push_back("GRANT " + (items.empty() ? std::string("USAGE") : join(items, ", ")) + " ON " + quoted_name(key.substr(0, dot)) + "." + quoted_name(key.substr(dot + 1)) + to +
                        (m.grant_option ? " WITH GRANT OPTION" : ""));
    }
    for (const auto& [key, m] : routines) {
        const std::size_t space = key.find(' ');
        const std::string name = key.substr(space + 1);
        const std::size_t dot = name.find('.');
        std::vector<std::string> names(m.privileges.begin(), m.privileges.end());
        lines.push_back("GRANT " + join(names, ", ") + " ON " + key.substr(0, space) + " " + quoted_name(name.substr(0, dot)) + "." + quoted_name(name.substr(dot + 1)) + to +
                        (m.grant_option ? " WITH GRANT OPTION" : ""));
    }
    std::set<std::string> plain, with_admin;
    for (const RoleGrant& rg : s.role_grants) {
        if (rg.user != user || rg.host != host) continue;
        (rg.with_admin_option ? with_admin : plain).insert(role_text(rg.role));
    }
    if (!plain.empty()) lines.push_back("GRANT " + join(std::vector<std::string>(plain.begin(), plain.end()), ",") + to);
    if (!with_admin.empty()) lines.push_back("GRANT " + join(std::vector<std::string>(with_admin.begin(), with_admin.end()), ",") + to + " WITH ADMIN OPTION");
    return lines;
}

// the accounts a statement names, as 'u'@'h' texts for an error
std::string accounts_text(const std::vector<std::pair<std::string, std::string>>& accounts) {
    std::vector<std::string> out;
    for (const auto& [u, h] : accounts) out.push_back(account_text(u, h));
    return join(out, ",");
}

void forget_account(SharedDatabase& s, const std::string& user, const std::string& host) {
    s.grants.erase(std::remove_if(s.grants.begin(), s.grants.end(), [&](const GrantRecord& g) { return g.user == user && g.host == host; }), s.grants.end());
    s.role_grants.erase(std::remove_if(s.role_grants.begin(), s.role_grants.end(), [&](const RoleGrant& rg) { return rg.user == user && rg.host == host; }), s.role_grants.end());
}

void save_privileges(SharedDatabase& s) {
    s.disk.save_users(s.users);
    s.disk.save_grants(s.grants);
    s.disk.save_roles(s.roles);
    s.disk.save_role_grants(s.role_grants);
}

void set_password(UserRecord& u, const std::string& password) {
    u.password_hash = hash_password(password);
    u.mysql_native_hash = mysql_native_hash_compute(password);
}

} // namespace

// ---------------------------------------------------------------------------
// Who the session is, and what it may do
// ---------------------------------------------------------------------------

bool Executor::privileges_apply(const SharedDatabase& s) const { return auth_user != "root" && login_account(s, auth_user) != nullptr; }

std::string Executor::account_host(const SharedDatabase& s) const {
    if (auth_user == "root") return "localhost";
    const UserRecord* account = login_account(s, auth_user);
    return account ? account->host : "localhost";
}

std::string Executor::current_role_text(const SharedDatabase& s) const {
    const std::string host = account_host(s);
    std::vector<std::string> names;
    for (const std::string& r : active_roles_) {
        if (std::any_of(s.role_grants.begin(), s.role_grants.end(), [&](const RoleGrant& rg) { return rg.role == r && rg.user == auth_user && rg.host == host; })) names.push_back(r);
    }
    if (names.empty()) return "NONE";
    std::sort(names.begin(), names.end());
    std::vector<std::string> texts;
    for (const std::string& n : names) texts.push_back(role_text(n));
    return join(texts, ",");
}

void Executor::activate_default_roles() {
    active_roles_.clear();
    auto s = shared->read();
    const std::string host = account_host(*s);
    for (const RoleGrant& rg : s->role_grants) {
        if (rg.is_default && rg.user == auth_user && rg.host == host) active_roles_.push_back(rg.role);
    }
}

std::optional<std::string> Executor::check_privileges(const SharedDatabase& s, const Statement& stmt) {
    if (!privileges_apply(s)) return std::nullopt;
    Who who(s, auth_user, account_host(s), current_db, active_roles_);
    return Checker(who, s).run(stmt);
}

std::optional<std::string> Executor::check_table_maintenance(const SharedDatabase& s, const std::string& table, bool write) {
    if (!privileges_apply(s)) return std::nullopt;
    Who who(s, auth_user, account_host(s), current_db, active_roles_);
    return Checker(who, s).table_access(table, write);
}

std::optional<std::string> Executor::check_set_global(const SharedDatabase& s, const std::string& sql) {
    if (!privileges_apply(s)) return std::nullopt;
    std::string plain; // the statement without what is quoted, upper case
    char quote = 0;
    for (char c : sql) {
        if (quote) {
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '\'' || c == '"' || c == '`') {
            quote = c;
            plain += ' ';
            continue;
        }
        plain += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    auto is_word_char = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    bool global_scope = false;
    for (const char* keyword : {"GLOBAL", "PERSIST_ONLY", "PERSIST"}) {
        const std::string word = keyword;
        for (std::size_t at = plain.find(word); at != std::string::npos; at = plain.find(word, at + 1)) {
            const std::size_t end = at + word.size();
            if (at > 0 && is_word_char(plain[at - 1])) continue;
            if (end < plain.size() && is_word_char(plain[end])) continue;
            if (at > 0 && plain[at - 1] == '@' && !(at > 1 && plain[at - 2] == '@')) continue; // (@global is a user variable; @@global. is the scope)
            global_scope = true;
        }
    }
    if (!global_scope) return std::nullopt;
    Who who(s, auth_user, account_host(s), current_db, active_roles_);
    if (who.global("SUPER") || who.global("SYSTEM_VARIABLES_ADMIN")) return std::nullopt;
    return need_privileges("SUPER or SYSTEM_VARIABLES_ADMIN");
}

bool Executor::holds_global(const SharedDatabase& s, const std::string& priv) const {
    if (!privileges_apply(s)) return true;
    return Who(s, auth_user, account_host(s), current_db, active_roles_).global(priv);
}

bool Executor::may_see(const SharedDatabase& s, const std::string& db, const std::string& table) const {
    if (!privileges_apply(s)) return true;
    Who who(s, auth_user, account_host(s), current_db, active_roles_);
    return who.any_access(lower(db), lower(table));
}

// ---------------------------------------------------------------------------
// Users
// ---------------------------------------------------------------------------

StringResult Executor::exec_create_user(SharedDatabase& s, const Statement::CreateUser& st) {
    std::vector<const Statement::CreateUser*> accounts = {&st};
    for (const auto& more : st.more_users) accounts.push_back(&more);
    std::vector<std::pair<std::string, std::string>> failing;
    std::vector<const Statement::CreateUser*> to_create;
    for (const auto* a : accounts) {
        const bool exists = find_user(s, a->user, a->host) != nullptr || (a->host == "%" && role_exists(s, a->user));
        if (!exists) to_create.push_back(a);
        else if (!st.if_not_exists) failing.emplace_back(a->user, a->host);
    }
    if (!failing.empty()) return fail(1396, "HY000", "Operation CREATE USER failed for " + accounts_text(failing));
    for (const auto* a : to_create) {
        UserRecord rec;
        rec.user = a->user;
        rec.host = a->host;
        if (a->password) set_password(rec, *a->password);
        s.users.push_back(std::move(rec));
    }
    save_privileges(s);
    return ok_query();
}

StringResult Executor::exec_drop_user(SharedDatabase& s, const Statement::DropUser& st) {
    std::vector<std::pair<std::string, std::string>> accounts = {{st.user, st.host}};
    accounts.insert(accounts.end(), st.more_users.begin(), st.more_users.end());
    std::vector<std::pair<std::string, std::string>> missing, present;
    for (const auto& a : accounts) {
        if (find_user(s, a.first, a.second) || (a.second == "%" && role_exists(s, a.first))) present.push_back(a);
        else missing.push_back(a);
    }
    if (!missing.empty() && !st.if_exists) return fail(1396, "HY000", "Operation DROP USER failed for " + accounts_text(missing));
    for (const auto& [user, host] : present) {
        s.users.erase(std::remove_if(s.users.begin(), s.users.end(), [&](const UserRecord& u) { return u.user == user && u.host == host; }), s.users.end());
        forget_account(s, user, host);
        if (host == "%" && role_exists(s, user)) {
            s.roles.erase(std::remove_if(s.roles.begin(), s.roles.end(), [&](const RoleRecord& r) { return r.name == user; }), s.roles.end());
            s.role_grants.erase(std::remove_if(s.role_grants.begin(), s.role_grants.end(), [&](const RoleGrant& rg) { return rg.role == user; }), s.role_grants.end());
        }
    }
    save_privileges(s);
    return ok_query();
}

StringResult Executor::exec_alter_user(SharedDatabase& s, const Statement::AlterUser& st) {
    const std::string user = st.current ? auth_user : st.user;
    const std::string host = st.current ? account_host(s) : st.host;
    for (UserRecord& u : s.users) {
        if (u.user == user && (u.host == host || (st.current && auth_user == "root"))) {
            if (st.password) set_password(u, *st.password);
            save_privileges(s);
            return ok_query();
        }
    }
    if (st.if_exists) return ok_query();
    return fail(1396, "HY000", "Operation ALTER USER failed for " + account_text(user, host));
}

StringResult Executor::exec_set_password(SharedDatabase& s, const Statement::SetPassword& st) {
    const std::string user = st.current ? auth_user : st.user;
    const std::string host = st.current ? account_host(s) : st.host;
    for (UserRecord& u : s.users) {
        if (u.user == user && (u.host == host || (st.current && auth_user == "root"))) {
            set_password(u, st.password);
            save_privileges(s);
            return ok_query();
        }
    }
    return fail(1133, "42000", "Can't find any matching row in the user table");
}

StringResult Executor::exec_rename_user(SharedDatabase& s, const Statement::RenameUser& st) {
    for (const auto& [from, to] : st.pairs) {
        if (!find_user(s, from.first, from.second) || find_user(s, to.first, to.second)) return fail(1396, "HY000", "Operation RENAME USER failed for " + account_text(from.first, from.second));
    }
    for (const auto& [from, to] : st.pairs) {
        for (UserRecord& u : s.users) {
            if (u.user == from.first && u.host == from.second) { u.user = to.first; u.host = to.second; }
        }
        for (GrantRecord& g : s.grants) {
            if (g.user == from.first && g.host == from.second) { g.user = to.first; g.host = to.second; }
        }
        for (RoleGrant& rg : s.role_grants) {
            if (rg.user == from.first && rg.host == from.second) { rg.user = to.first; rg.host = to.second; }
        }
    }
    save_privileges(s);
    return ok_query();
}

// ---------------------------------------------------------------------------
// GRANT / REVOKE / SHOW GRANTS
// ---------------------------------------------------------------------------

StringResult Executor::exec_grant(SharedDatabase& s, const Statement::Grant& st) {
    std::vector<std::pair<std::string, std::string>> accounts = {{st.user, st.host}};
    accounts.insert(accounts.end(), st.more_users.begin(), st.more_users.end());
    const auto where = target_of(s, st.object, st.object_type, current_db);
    if (auto* text = std::get_if<std::string>(&where)) return StringResult::Err(*text);
    const Target& target = std::get<Target>(where);
    const bool routine = st.object_type == "PROCEDURE" || st.object_type == "FUNCTION";
    if (!routine) {
        if (auto bad = check_privilege_names(st.privileges, st.privilege_columns, target.level, target.table.empty() ? target.db : target.table)) return StringResult::Err(*bad);
    }
    if (target.level == kTable && !routine) {
        const TableSchema* schema = s.catalog.get_table(target.key);
        if (!schema && !s.views.count(target.key)) return fail(1146, "42S02", "Table '" + target.key + "' doesn't exist");
        if (schema) {
            for (const auto& columns : st.privilege_columns) {
                for (const std::string& c : columns) {
                    const bool known = std::any_of(schema->columns.begin(), schema->columns.end(), [&](const ColumnDef& d) { return lower(d.name) == lower(c); });
                    if (!known) return fail(1054, "42S22", "Unknown column '" + c + "' in '" + target.table + "'");
                }
            }
        }
    }
    for (const auto& [user, host] : accounts) {
        if (!find_user(s, user, host) && !(host == "%" && role_exists(s, user))) return fail(1410, "42000", "You are not allowed to create a user with GRANT");
    }
    for (const auto& [user, host] : accounts) {
        GrantRecord* rec = find_record(s, user, host, routine ? lower(st.object) : target.key, st.object_type, current_db);
        if (!rec) {
            GrantRecord fresh;
            fresh.user = user;
            fresh.host = host;
            fresh.object_type = routine ? st.object_type : "TABLE";
            fresh.object = routine ? lower(st.object) : target.key;
            s.grants.push_back(std::move(fresh));
            rec = &s.grants.back();
        }
        // an older record may still say ALL PRIVILEGES: it is kept as the privileges it stands for
        std::set<std::string> held = privileges_of(*rec, target.level);
        for (std::size_t i = 0; i < st.privileges.size(); i++) {
            const std::string& p = st.privileges[i];
            const bool by_column = i < st.privilege_columns.size() && !st.privilege_columns[i].empty();
            if (p == "GRANT OPTION") { rec->with_grant_option = true; continue; }
            if (p == "USAGE") continue;
            if (by_column) {
                for (const std::string& c : st.privilege_columns[i]) {
                    const auto entry = std::make_pair(p, lower(c));
                    if (std::find(rec->column_privileges.begin(), rec->column_privileges.end(), entry) == rec->column_privileges.end()) rec->column_privileges.push_back(entry);
                }
            } else if (p == "ALL PRIVILEGES") {
                if (routine) { held.insert("EXECUTE"); held.insert("ALTER ROUTINE"); }
                else for (const std::string& a : all_for(target.level)) held.insert(a);
            } else {
                held.insert(p);
            }
        }
        rec->privileges.assign(held.begin(), held.end());
        if (st.with_grant_option) rec->with_grant_option = true;
    }
    s.disk.save_grants(s.grants);
    return ok_query();
}

StringResult Executor::exec_revoke(SharedDatabase& s, const Statement::Revoke& st) {
    std::vector<std::pair<std::string, std::string>> accounts = {{st.user, st.host}};
    accounts.insert(accounts.end(), st.more_users.begin(), st.more_users.end());
    if (st.all_with_grant_option) { // REVOKE ALL PRIVILEGES, GRANT OPTION FROM account: everything the account holds, at every level
        for (const auto& [user, host] : accounts) {
            if (!find_user(s, user, host) && !(host == "%" && role_exists(s, user))) return fail(1269, "HY000", "Can't revoke all privileges for one or more of the requested users");
        }
        for (const auto& [user, host] : accounts) {
            s.grants.erase(std::remove_if(s.grants.begin(), s.grants.end(), [&](const GrantRecord& g) { return g.user == user && g.host == host; }), s.grants.end());
        }
        s.disk.save_grants(s.grants);
        return ok_query();
    }
    const auto where = target_of(s, st.object, st.object_type, current_db);
    if (auto* text = std::get_if<std::string>(&where)) return StringResult::Err(*text);
    const Target& target = std::get<Target>(where);
    const bool routine = st.object_type == "PROCEDURE" || st.object_type == "FUNCTION";
    if (!routine) {
        if (auto bad = check_privilege_names(st.privileges, st.privilege_columns, target.level, target.table.empty() ? target.db : target.table)) return StringResult::Err(*bad);
    }
    for (const auto& [user, host] : accounts) {
        const bool exists = find_user(s, user, host) || (host == "%" && role_exists(s, user));
        GrantRecord* rec = exists ? find_record(s, user, host, routine ? lower(st.object) : target.key, st.object_type, current_db) : nullptr;
        if (!rec && target.level == kGlobal && exists) continue; // (everyone has a global record, empty)
        if (!rec) {
            if (target.level == kTable || routine) return fail(1147, "42000", "There is no such grant defined for user '" + user + "' on host '" + host + "' on table '" + (routine ? lower(st.object) : target.table) + "'");
            return fail(1141, "42000", "There is no such grant defined for user '" + user + "' on host '" + host + "'");
        }
    }
    for (const auto& [user, host] : accounts) {
        GrantRecord* rec = find_record(s, user, host, routine ? lower(st.object) : target.key, st.object_type, current_db);
        if (!rec) continue;
        std::set<std::string> held = privileges_of(*rec, target.level);
        for (std::size_t i = 0; i < st.privileges.size(); i++) {
            const std::string& p = st.privileges[i];
            const bool by_column = i < st.privilege_columns.size() && !st.privilege_columns[i].empty();
            if (p == "GRANT OPTION") { rec->with_grant_option = false; continue; }
            if (by_column) {
                for (const std::string& c : st.privilege_columns[i]) {
                    rec->column_privileges.erase(std::remove(rec->column_privileges.begin(), rec->column_privileges.end(), std::make_pair(p, lower(c))), rec->column_privileges.end());
                }
            } else if (p == "ALL PRIVILEGES") {
                held.clear();
                rec->column_privileges.clear();
                rec->with_grant_option = false;
            } else {
                held.erase(p);
                rec->column_privileges.erase(std::remove_if(rec->column_privileges.begin(), rec->column_privileges.end(), [&](const auto& e) { return e.first == p; }), rec->column_privileges.end());
            }
        }
        rec->privileges.assign(held.begin(), held.end());
    }
    s.grants.erase(std::remove_if(s.grants.begin(), s.grants.end(),
                                  [](const GrantRecord& g) { return g.privileges.empty() && g.column_privileges.empty() && !g.with_grant_option; }),
                   s.grants.end());
    s.disk.save_grants(s.grants);
    return ok_query();
}

StringResult Executor::exec_show_grants(const SharedDatabase& s, const Statement::ShowGrants& st) const {
    std::string user = auth_user, host = account_host(s);
    if (st.user) {
        user = *st.user;
        host = st.host.value_or("%");
    } else if (auth_user != "root") {
        if (const UserRecord* account = login_account(s, auth_user)) host = account->host;
    } else if (const UserRecord* account = login_account(s, "root")) {
        host = account->host;
    }
    const bool exists = find_user(s, user, host) != nullptr || (host == "%" && role_exists(s, user));
    if (!exists) return fail(1141, "42000", "There is no such grant defined for user '" + user + "' on host '" + host + "'");
    std::vector<std::string> roles = st.using_roles;
    for (const std::string& r : roles) {
        if (!std::any_of(s.role_grants.begin(), s.role_grants.end(), [&](const RoleGrant& rg) { return rg.role == r && rg.user == user && rg.host == host; })) {
            return fail(3530, "HY000", role_text(r) + " is not granted to " + quoted_name(user) + "@" + quoted_name(host));
        }
    }
    std::vector<std::vector<std::string>> rows;
    for (const std::string& line : grants_of(s, user, host, roles)) rows.push_back({line});
    // (the shown account of the current user reads as root@localhost, as MySQL's own root)
    return StringResult::Ok(box({"Grants for " + user + "@" + host}, rows));
}

StringResult Executor::exec_show_databases(const SharedDatabase& s) const {
    std::vector<std::string> dbs;
    const bool all = !privileges_apply(s) || Who(s, auth_user, account_host(s), current_db, active_roles_).global("SHOW DATABASES");
    for (const std::string& d : s.databases) {
        if (all || may_see(s, d, "")) dbs.push_back(d);
    }
    std::sort(dbs.begin(), dbs.end());
    if (dbs.empty()) return StringResult::Ok("No databases.");
    std::vector<std::vector<std::string>> rows;
    for (const std::string& d : dbs) rows.push_back({d});
    return StringResult::Ok(box({"Database"}, rows));
}

// ---------------------------------------------------------------------------
// Roles
// ---------------------------------------------------------------------------

StringResult Executor::exec_create_role(SharedDatabase& s, const Statement::CreateRole& st) {
    std::vector<std::string> names = {st.name};
    names.insert(names.end(), st.more.begin(), st.more.end());
    std::vector<std::pair<std::string, std::string>> failing;
    std::vector<std::string> fresh;
    for (const std::string& n : names) {
        const bool exists = role_exists(s, n) || find_user(s, n, "%") != nullptr;
        if (!exists) fresh.push_back(n);
        else if (!st.if_not_exists) failing.emplace_back(n, "%");
    }
    if (!failing.empty()) return fail(1396, "HY000", "Operation CREATE ROLE failed for " + accounts_text(failing));
    for (const std::string& n : fresh) s.roles.push_back(RoleRecord{n});
    s.disk.save_roles(s.roles);
    return ok_query();
}

StringResult Executor::exec_drop_role(SharedDatabase& s, const Statement::DropRole& st) {
    std::vector<std::string> names = {st.name};
    names.insert(names.end(), st.more.begin(), st.more.end());
    std::vector<std::pair<std::string, std::string>> missing;
    for (const std::string& n : names) {
        if (!role_exists(s, n)) missing.emplace_back(n, "%");
    }
    if (!missing.empty() && !st.if_exists) return fail(1396, "HY000", "Operation DROP ROLE failed for " + accounts_text(missing));
    for (const std::string& n : names) {
        if (!role_exists(s, n)) continue;
        s.roles.erase(std::remove_if(s.roles.begin(), s.roles.end(), [&](const RoleRecord& r) { return r.name == n; }), s.roles.end());
        s.role_grants.erase(std::remove_if(s.role_grants.begin(), s.role_grants.end(), [&](const RoleGrant& rg) { return rg.role == n || (rg.user == n && rg.host == "%"); }),
                            s.role_grants.end());
        s.grants.erase(std::remove_if(s.grants.begin(), s.grants.end(), [&](const GrantRecord& g) { return g.user == n && g.host == "%"; }), s.grants.end());
    }
    save_privileges(s);
    return ok_query();
}

StringResult Executor::exec_grant_role(SharedDatabase& s, const Statement::GrantRole& st) {
    std::vector<std::string> roles = {st.role};
    roles.insert(roles.end(), st.more_roles.begin(), st.more_roles.end());
    std::vector<std::pair<std::string, std::string>> accounts = {{st.user, st.host}};
    accounts.insert(accounts.end(), st.more_users.begin(), st.more_users.end());
    for (const std::string& r : roles) {
        if (!role_exists(s, r)) return fail(3523, "HY000", "Unknown authorization ID " + role_text(r));
    }
    for (const auto& [user, host] : accounts) {
        if (!find_user(s, user, host) && !(host == "%" && role_exists(s, user))) return fail(3523, "HY000", "Unknown authorization ID " + quoted_name(user) + "@" + quoted_name(host));
    }
    for (const auto& [user, host] : accounts) {
        for (const std::string& r : roles) {
            if (host == "%" && role_exists(s, user) && has(role_closure(s, {r}), user)) {
                return fail(4027, "HY000", "User account " + role_text(user) + " is directly or indirectly granted to the role " + role_text(r) + ". The GRANT would create a loop");
            }
        }
    }
    for (const auto& [user, host] : accounts) {
        for (const std::string& r : roles) {
            bool was_default = false;
            for (const RoleGrant& rg : s.role_grants) {
                if (rg.role == r && rg.user == user && rg.host == host) was_default = rg.is_default;
            }
            s.role_grants.erase(std::remove_if(s.role_grants.begin(), s.role_grants.end(), [&](const RoleGrant& rg) { return rg.role == r && rg.user == user && rg.host == host; }),
                                s.role_grants.end());
            RoleGrant added;
            added.role = r;
            added.user = user;
            added.host = host;
            added.with_admin_option = st.with_admin_option;
            added.is_default = was_default;
            s.role_grants.push_back(std::move(added));
        }
    }
    s.disk.save_role_grants(s.role_grants);
    return ok_query();
}

StringResult Executor::exec_revoke_role(SharedDatabase& s, const Statement::RevokeRole& st) {
    std::vector<std::string> roles = {st.role};
    roles.insert(roles.end(), st.more_roles.begin(), st.more_roles.end());
    std::vector<std::pair<std::string, std::string>> accounts = {{st.user, st.host}};
    accounts.insert(accounts.end(), st.more_users.begin(), st.more_users.end());
    for (const std::string& r : roles) {
        if (!role_exists(s, r)) return fail(3523, "HY000", "Unknown authorization ID " + role_text(r));
    }
    for (const auto& [user, host] : accounts) {
        if (!find_user(s, user, host) && !(host == "%" && role_exists(s, user))) return fail(3523, "HY000", "Unknown authorization ID " + quoted_name(user) + "@" + quoted_name(host));
    }
    for (const auto& [user, host] : accounts) {
        for (const std::string& r : roles) {
            if (!std::any_of(s.role_grants.begin(), s.role_grants.end(), [&](const RoleGrant& rg) { return rg.role == r && rg.user == user && rg.host == host; })) {
                return fail(3530, "HY000", role_text(r) + " is not granted to " + quoted_name(user) + "@" + quoted_name(host));
            }
        }
    }
    for (const auto& [user, host] : accounts) {
        for (const std::string& r : roles) {
            s.role_grants.erase(std::remove_if(s.role_grants.begin(), s.role_grants.end(), [&](const RoleGrant& rg) { return rg.role == r && rg.user == user && rg.host == host; }),
                                s.role_grants.end());
        }
    }
    s.disk.save_role_grants(s.role_grants);
    return ok_query();
}

StringResult Executor::exec_set_role(SharedDatabase& s, const Statement::SetRole& st) {
    const std::string host = account_host(s);
    std::vector<std::string> granted;
    for (const RoleGrant& rg : s.role_grants) {
        if (rg.user == auth_user && rg.host == host && !has(granted, rg.role)) granted.push_back(rg.role);
    }
    auto not_granted = [&](const std::string& r) { return fail(3530, "HY000", role_text(r) + " is not granted to " + quoted_name(auth_user) + "@" + quoted_name(host)); };
    if (st.mode == "NONE") {
        active_roles_.clear();
    } else if (st.mode == "ALL") {
        active_roles_ = granted;
    } else if (st.mode == "DEFAULT") {
        active_roles_.clear();
        for (const RoleGrant& rg : s.role_grants) {
            if (rg.is_default && rg.user == auth_user && rg.host == host) active_roles_.push_back(rg.role);
        }
    } else {
        for (const std::string& r : st.roles) {
            if (!has(granted, r)) return not_granted(r);
        }
        if (st.mode == "ALL EXCEPT") {
            active_roles_.clear();
            for (const std::string& r : granted) {
                if (!has(st.roles, r)) active_roles_.push_back(r);
            }
        } else {
            active_roles_ = st.roles;
        }
    }
    return ok_query();
}

StringResult Executor::exec_set_default_role(SharedDatabase& s, const Statement::SetDefaultRole& st) {
    for (const auto& [user, host] : st.users) {
        if (!find_user(s, user, host)) return fail(1396, "HY000", "Operation SET DEFAULT ROLE failed for " + account_text(user, host)); // (MySQL words it the same)
        for (const std::string& r : st.roles) {
            if (!std::any_of(s.role_grants.begin(), s.role_grants.end(), [&](const RoleGrant& rg) { return rg.role == r && rg.user == user && rg.host == host; })) {
                return fail(3530, "HY000", role_text(r) + " is not granted to " + quoted_name(user) + "@" + quoted_name(host));
            }
        }
    }
    for (const auto& [user, host] : st.users) {
        for (RoleGrant& rg : s.role_grants) {
            if (rg.user != user || rg.host != host) continue;
            rg.is_default = st.mode == "ALL" || (st.mode == "LIST" && has(st.roles, rg.role));
        }
    }
    s.disk.save_role_grants(s.role_grants);
    return ok_query();
}

StringResult Executor::exec_show_roles(const SharedDatabase& s) const {
    if (s.roles.empty()) return StringResult::Ok("No roles defined.");
    std::vector<std::vector<std::string>> rows;
    for (const RoleRecord& r : s.roles) rows.push_back({r.name});
    return StringResult::Ok(box({"Role"}, rows));
}

} // namespace engine
