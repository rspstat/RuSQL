// Accounts, privileges and roles (R78): GRANT / REVOKE / SHOW GRANTS, what a session of an account may do (the errors carry MySQL's numbers), roles, CURRENT_USER() and
// CURRENT_ROLE(). The answers were measured on MySQL 8.4 (code/test/diff/corpus/privileges.sql repeats them against the server).

#include <filesystem>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

// a database with a root session and the tables v.t and v.u
struct World {
    TempDataDir dir;
    Executor root;
    std::shared_ptr<RwLock<SharedDatabase>> shared;
    explicit World(const std::string& name) : dir(name), root(dir.path) {
        run(root, "CREATE DATABASE v");
        run(root, "USE v");
        run(root, "CREATE TABLE t (id INT PRIMARY KEY, i INT)");
        run(root, "CREATE TABLE u (id INT PRIMARY KEY, j INT)");
        run(root, "INSERT INTO t VALUES (1, 10), (2, 20)");
        run(root, "INSERT INTO u VALUES (1, 5)");
        shared = root.get_shared();
    }
    static void run(Executor& ex, const std::string& sql) {
        auto r = ex.execute_sql(sql);
        INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
        REQUIRE(r.is_ok());
    }
    // a session of the account, in the database v
    Executor session(const std::string& user, const std::string& password = "pw") {
        run(root, "CREATE USER IF NOT EXISTS '" + user + "'@'%' IDENTIFIED BY '" + password + "'");
        Executor ex = Executor::new_session(shared);
        ex.auth_user = user;
        ex.activate_default_roles();
        ex.current_db = "v"; // (a server runs USE for the database a client names at login, and lets a refusal stand)
        return ex;
    }
};

// the number of an error "ERROR 1142 (42000): ...", 0 for anything else or no error
int errno_of(const StringResult& r) {
    if (r.is_ok()) return 0;
    const std::string& e = r.error();
    if (e.compare(0, 6, "ERROR ") != 0) return -1;
    return std::stoi(e.substr(6, 4));
}
} // namespace

TEST_CASE("a user with SELECT on one table may read it and nothing else", "[privileges]") {
    World w("priv_data_1");
    World::run(w.root, "CREATE USER 'alice'@'%' IDENTIFIED BY 'pw'");
    World::run(w.root, "GRANT SELECT ON v.t TO 'alice'@'%'");
    Executor alice = w.session("alice");

    SECTION("reads") {
        REQUIRE(alice.execute_sql("SELECT id FROM t ORDER BY id").is_ok());
        REQUIRE(alice.execute_sql("SELECT COUNT(*) FROM v.t").is_ok());
        REQUIRE(alice.execute_sql("SELECT 1").is_ok());
        REQUIRE(errno_of(alice.execute_sql("SELECT * FROM u")) == 1142);
        REQUIRE(errno_of(alice.execute_sql("SELECT * FROM t, u")) == 1142);
        REQUIRE(errno_of(alice.execute_sql("SELECT * FROM t WHERE id IN (SELECT id FROM u)")) == 1142);
        REQUIRE(errno_of(alice.execute_sql("SELECT (SELECT COUNT(*) FROM u) FROM t")) == 1142);
        REQUIRE(errno_of(alice.execute_sql("SELECT id FROM t UNION SELECT id FROM u")) == 1142);
        REQUIRE(errno_of(alice.execute_sql("EXPLAIN SELECT * FROM u")) == 1142);
    }
    SECTION("writes and definitions") {
        REQUIRE(errno_of(alice.execute_sql("INSERT INTO t (id, i) VALUES (50, 1)")) == 1142);
        REQUIRE(errno_of(alice.execute_sql("UPDATE t SET i = 0")) == 1142);
        REQUIRE(errno_of(alice.execute_sql("DELETE FROM t")) == 1142);
        REQUIRE(errno_of(alice.execute_sql("CREATE TABLE z (a INT)")) == 1142);
        REQUIRE(errno_of(alice.execute_sql("DROP TABLE t")) == 1142);
        REQUIRE(errno_of(alice.execute_sql("TRUNCATE TABLE t")) == 1142);
        REQUIRE(errno_of(alice.execute_sql("ALTER TABLE t ADD COLUMN k INT")) == 1142);
        REQUIRE(errno_of(alice.execute_sql("CREATE INDEX ix ON t (i)")) == 1142);
        REQUIRE(errno_of(alice.execute_sql("CREATE DATABASE z")) == 1044);
        // nothing was changed
        auto rows = w.root.execute_sql("SELECT COUNT(*) FROM t");
        REQUIRE(rows.is_ok());
        REQUIRE(rows.value().find("| 2 ") != std::string::npos);
    }
    SECTION("the table list, the other databases, and administration") {
        auto tables = alice.execute_sql("SHOW TABLES");
        REQUIRE(tables.is_ok());
        REQUIRE(tables.value().find("| t ") != std::string::npos);
        REQUIRE(tables.value().find("| u ") == std::string::npos);
        REQUIRE(errno_of(alice.execute_sql("DESCRIBE u")) == 1142);
        REQUIRE(errno_of(alice.execute_sql("SHOW CREATE TABLE u")) == 1142);
        REQUIRE(alice.execute_sql("DESCRIBE t").is_ok());
        REQUIRE(errno_of(alice.execute_sql("USE mysql")) == 1044);
        REQUIRE(errno_of(alice.execute_sql("CREATE USER 'bob'@'%'")) == 1227);
        REQUIRE(errno_of(alice.execute_sql("GRANT SELECT ON v.t TO 'bob'@'%'")) == 1142);
        REQUIRE(errno_of(alice.execute_sql("GRANT SELECT ON v.* TO 'bob'@'%'")) == 1044);
        REQUIRE(errno_of(alice.execute_sql("GRANT SELECT ON *.* TO 'bob'@'%'")) == 1045);
        REQUIRE(errno_of(alice.execute_sql("SHOW GRANTS FOR 'root'@'%'")) == 1142);
        REQUIRE(alice.execute_sql("SHOW GRANTS").is_ok());
    }
    SECTION("an answer root has made is not handed to a user who may not read it") {
        REQUIRE(w.root.execute_sql("SELECT * FROM u").is_ok()); // (the result cache keeps the answer of this text)
        REQUIRE(errno_of(alice.execute_sql("SELECT * FROM u")) == 1142);
        REQUIRE(w.root.execute_sql("SELECT id FROM t ORDER BY id").is_ok());
        REQUIRE(alice.execute_sql("SELECT id FROM t ORDER BY id").is_ok());
    }
    SECTION("information_schema shows the account only what it holds a privilege on") {
        auto tables = alice.execute_sql("SELECT table_name FROM information_schema.tables WHERE table_schema = 'v'");
        REQUIRE(tables.is_ok());
        REQUIRE(tables.value().find("| t ") != std::string::npos);
        REQUIRE(tables.value().find("| u ") == std::string::npos);
        auto columns = alice.execute_sql("SELECT DISTINCT table_name FROM information_schema.columns WHERE table_schema = 'v'");
        REQUIRE(columns.is_ok());
        REQUIRE(columns.value().find("| u ") == std::string::npos);
        auto all = w.root.execute_sql("SELECT table_name FROM information_schema.tables WHERE table_schema = 'v'");
        REQUIRE(all.is_ok());
        REQUIRE(all.value().find("| u ") != std::string::npos);
    }
    SECTION("SET GLOBAL needs SUPER or SYSTEM_VARIABLES_ADMIN") {
        REQUIRE(errno_of(alice.execute_sql("SET GLOBAL slow_query_log = 'OFF'")) == 1227);
        REQUIRE(errno_of(alice.execute_sql("SET @@global.long_query_time = 5")) == 1227);
        REQUIRE(alice.execute_sql("SET SESSION sql_mode = ''").is_ok());
        REQUIRE(alice.execute_sql("SET @x = 1").is_ok());
        REQUIRE(alice.execute_sql("SET @global = 'a global thing'").is_ok()); // (a user variable and a text are no scope)
        World::run(w.root, "GRANT SYSTEM_VARIABLES_ADMIN ON *.* TO 'alice'@'%'");
        REQUIRE(alice.execute_sql("SET GLOBAL slow_query_log = 'OFF'").is_ok());
    }
    SECTION("an account without PROCESS sees only its own sessions") {
        w.root.register_process("root", "localhost:1", [] {});
        alice.register_process("alice", "localhost:2", [] {});
        auto mine = alice.execute_sql("SHOW PROCESSLIST");
        REQUIRE(mine.is_ok());
        REQUIRE(mine.value().find("alice") != std::string::npos);
        REQUIRE(mine.value().find("root") == std::string::npos);
        auto listed = alice.execute_sql("SELECT user FROM information_schema.processlist");
        REQUIRE(listed.is_ok());
        REQUIRE(listed.value().find("root") == std::string::npos);
        auto all = w.root.execute_sql("SHOW PROCESSLIST");
        REQUIRE(all.is_ok());
        REQUIRE(all.value().find("alice") != std::string::npos);
        World::run(w.root, "GRANT PROCESS ON *.* TO 'alice'@'%'");
        auto now = alice.execute_sql("SHOW PROCESSLIST");
        REQUIRE(now.is_ok());
        REQUIRE(now.value().find("root") != std::string::npos);
    }
    SECTION("CHECK TABLE and OPTIMIZE TABLE") {
        REQUIRE(alice.execute_sql("CHECK TABLE t").is_ok());
        REQUIRE(errno_of(alice.execute_sql("CHECK TABLE u")) == 1142);
        REQUIRE(errno_of(alice.execute_sql("OPTIMIZE TABLE t")) == 1142); // INSERT
    }
    SECTION("root is never refused, and neither is a session that names no account of the table") {
        REQUIRE(w.root.execute_sql("SELECT * FROM u").is_ok());
        Executor embedded = Executor::new_session(w.shared);
        embedded.auth_user = "nobody";
        REQUIRE(embedded.execute_sql("USE v").is_ok());
        REQUIRE(embedded.execute_sql("SELECT * FROM u").is_ok());
    }
}

TEST_CASE("INSERT, UPDATE and DELETE need their own privileges and the SELECT of what they read", "[privileges]") {
    World w("priv_data_2");
    World::run(w.root, "CREATE USER 'a'@'%' IDENTIFIED BY 'pw'");

    SECTION("INSERT") {
        World::run(w.root, "GRANT INSERT ON v.t TO 'a'@'%'");
        Executor a = w.session("a");
        REQUIRE(a.execute_sql("INSERT INTO t VALUES (30, 1)").is_ok());
        REQUIRE(errno_of(a.execute_sql("INSERT INTO t VALUES (1, 1) ON DUPLICATE KEY UPDATE i = 2")) == 1142); // UPDATE
        REQUIRE(errno_of(a.execute_sql("REPLACE INTO t VALUES (1, 1)")) == 1142);                           // DELETE
        REQUIRE(errno_of(a.execute_sql("INSERT INTO t SELECT id + 100, i FROM t")) == 1142);                 // SELECT
        REQUIRE(errno_of(a.execute_sql("SELECT * FROM t")) == 1142);
    }
    SECTION("UPDATE") {
        World::run(w.root, "GRANT UPDATE ON v.t TO 'a'@'%'");
        Executor a = w.session("a");
        REQUIRE(a.execute_sql("UPDATE t SET i = 5").is_ok());
        REQUIRE(errno_of(a.execute_sql("UPDATE t SET i = 5 WHERE id = 1")) == 1143); // reads id
        REQUIRE(errno_of(a.execute_sql("UPDATE t SET i = i + 1")) == 1143);
        REQUIRE(errno_of(a.execute_sql("UPDATE t, u SET t.i = 1 WHERE t.id = u.id")) == 1142);
    }
    SECTION("DELETE") {
        World::run(w.root, "GRANT DELETE ON v.t TO 'a'@'%'");
        Executor a = w.session("a");
        REQUIRE(a.execute_sql("DELETE FROM t WHERE 1 = 0").is_ok());
        REQUIRE(errno_of(a.execute_sql("DELETE FROM t WHERE id = 999")) == 1143);
        REQUIRE(errno_of(a.execute_sql("TRUNCATE TABLE t")) == 1142); // DROP
    }
}

TEST_CASE("privileges of a database, GRANT OPTION, and REVOKE", "[privileges]") {
    World w("priv_data_3");
    World::run(w.root, "CREATE USER 'a'@'%' IDENTIFIED BY 'pw'");

    SECTION("ALL on a database") {
        World::run(w.root, "GRANT ALL ON v.* TO 'a'@'%'");
        Executor a = w.session("a");
        REQUIRE(a.execute_sql("CREATE TABLE n (x INT)").is_ok());
        REQUIRE(a.execute_sql("DROP TABLE n").is_ok());
        REQUIRE(a.execute_sql("CREATE VIEW vw AS SELECT * FROM t").is_ok());
        REQUIRE(a.execute_sql("SELECT * FROM vw").is_ok());
        REQUIRE(a.execute_sql("DROP VIEW vw").is_ok());
        REQUIRE(errno_of(a.execute_sql("CREATE DATABASE z")) == 1044);
        REQUIRE(errno_of(a.execute_sql("CREATE USER 'b'@'%'")) == 1227);
        REQUIRE(errno_of(a.execute_sql("GRANT SELECT ON v.t TO 'b'@'%'")) == 1142); // no GRANT OPTION
        auto shown = w.root.execute_sql("SHOW GRANTS FOR 'a'@'%'");
        REQUIRE(shown.is_ok());
        REQUIRE(shown.value().find("GRANT USAGE ON *.* TO `a`@`%`") != std::string::npos);
        REQUIRE(shown.value().find("GRANT ALL PRIVILEGES ON `v`.* TO `a`@`%`") != std::string::npos);
    }
    SECTION("WITH GRANT OPTION lets the user pass on what it holds, and only that") {
        World::run(w.root, "GRANT SELECT, INSERT ON v.t TO 'a'@'%' WITH GRANT OPTION");
        World::run(w.root, "CREATE USER 'b'@'%'");
        Executor a = w.session("a");
        REQUIRE(a.execute_sql("GRANT SELECT ON v.t TO 'b'@'%'").is_ok());
        REQUIRE(errno_of(a.execute_sql("GRANT DELETE ON v.t TO 'b'@'%'")) == 1142); // not held
        REQUIRE(errno_of(a.execute_sql("GRANT SELECT ON v.u TO 'b'@'%'")) == 1142);
        Executor b = w.session("b");
        REQUIRE(b.execute_sql("SELECT COUNT(*) FROM t").is_ok());
    }
    SECTION("REVOKE takes it away at once, and a revoke of nothing is an error") {
        World::run(w.root, "GRANT SELECT, UPDATE ON v.t TO 'a'@'%'");
        Executor a = w.session("a");
        REQUIRE(a.execute_sql("SELECT * FROM t").is_ok());
        World::run(w.root, "REVOKE SELECT ON v.t FROM 'a'@'%'");
        REQUIRE(errno_of(a.execute_sql("SELECT * FROM t")) == 1142);
        World::run(w.root, "REVOKE UPDATE ON v.t FROM 'a'@'%'");
        REQUIRE(errno_of(w.root.execute_sql("REVOKE UPDATE ON v.t FROM 'a'@'%'")) == 1147);
        REQUIRE(errno_of(w.root.execute_sql("REVOKE SELECT ON v.* FROM 'a'@'%'")) == 1141);
        REQUIRE(errno_of(w.root.execute_sql("REVOKE ALL PRIVILEGES, GRANT OPTION FROM 'nobody'@'%'")) == 1269);
        World::run(w.root, "GRANT ALL ON *.* TO 'a'@'%' WITH GRANT OPTION");
        World::run(w.root, "REVOKE ALL PRIVILEGES, GRANT OPTION FROM 'a'@'%'");
        auto shown = w.root.execute_sql("SHOW GRANTS FOR 'a'@'%'");
        REQUIRE(shown.is_ok());
        REQUIRE(shown.value().find("GRANT USAGE ON *.* TO `a`@`%`") != std::string::npos);
        REQUIRE(shown.value().find("ON `v`") == std::string::npos);
    }
    SECTION("the errors of GRANT") {
        REQUIRE(errno_of(w.root.execute_sql("GRANT SELECT ON v.t TO 'nobody'@'%'")) == 1410);
        REQUIRE(errno_of(w.root.execute_sql("GRANT SELECT ON v.nosuch TO 'a'@'%'")) == 1146);
        REQUIRE(errno_of(w.root.execute_sql("GRANT FOO ON *.* TO 'a'@'%'")) == 1149);
        REQUIRE(errno_of(w.root.execute_sql("GRANT FOO ON v.t TO 'a'@'%'")) == 3619);
        REQUIRE(errno_of(w.root.execute_sql("GRANT RELOAD ON v.* TO 'a'@'%'")) == 1221);
        REQUIRE(errno_of(w.root.execute_sql("GRANT RELOAD ON v.t TO 'a'@'%'")) == 1144);
        REQUIRE(errno_of(w.root.execute_sql("GRANT SELECT (nosuch) ON v.t TO 'a'@'%'")) == 1054);
        REQUIRE(errno_of(w.root.execute_sql("CREATE USER 'a'@'%'")) == 1396);
        REQUIRE(errno_of(w.root.execute_sql("DROP USER 'nobody'@'%'")) == 1396);
        REQUIRE(w.root.execute_sql("DROP USER IF EXISTS 'nobody'@'%'").is_ok());
        REQUIRE(errno_of(w.root.execute_sql("ALTER USER 'nobody'@'%' IDENTIFIED BY 'x'")) == 1396);
        REQUIRE(errno_of(w.root.execute_sql("SHOW GRANTS FOR 'nobody'@'%'")) == 1141);
    }
}

TEST_CASE("SHOW GRANTS words the grants as MySQL does", "[privileges]") {
    World w("priv_data_4");
    World::run(w.root, "CREATE USER 'a'@'%'");
    World::run(w.root, "GRANT SELECT, INSERT, UPDATE ON v.t TO 'a'@'%'");
    World::run(w.root, "GRANT INSERT (j), SELECT (j, id), UPDATE (j) ON v.u TO 'a'@'%'");
    World::run(w.root, "GRANT SELECT ON *.* TO 'a'@'%'");
    auto shown = w.root.execute_sql("SHOW GRANTS FOR 'a'@'%'");
    REQUIRE(shown.is_ok());
    const std::string& text = shown.value();
    REQUIRE(text.find("| Grants for a@% ") != std::string::npos);
    REQUIRE(text.find("GRANT SELECT ON *.* TO `a`@`%`") != std::string::npos);
    REQUIRE(text.find("GRANT SELECT, INSERT, UPDATE ON `v`.`t` TO `a`@`%`") != std::string::npos);
    REQUIRE(text.find("GRANT SELECT (`id`, `j`), INSERT (`j`), UPDATE (`j`) ON `v`.`u` TO `a`@`%`") != std::string::npos);
    // the global line comes first
    REQUIRE(text.find("ON *.*") < text.find("ON `v`.`t`"));
}

TEST_CASE("column privileges", "[privileges]") {
    World w("priv_data_5");
    REQUIRE(errno_of(w.root.execute_sql("GRANT SELECT (i) ON v.t TO 'a'@'%'")) == 1410); // (GRANT does not create the user)
    World::run(w.root, "CREATE USER 'a'@'%' IDENTIFIED BY 'pw'");
    World::run(w.root, "GRANT SELECT (i) ON v.t TO 'a'@'%'");
    World::run(w.root, "GRANT UPDATE (j) ON v.u TO 'a'@'%'");
    Executor a = w.session("a");
    REQUIRE(a.execute_sql("SELECT i FROM t").is_ok());
    REQUIRE(a.execute_sql("SELECT i FROM t ORDER BY i").is_ok());
    REQUIRE(a.execute_sql("SELECT COUNT(*) FROM t").is_ok());
    REQUIRE(a.execute_sql("SELECT 1 FROM t").is_ok());
    REQUIRE(errno_of(a.execute_sql("SELECT id FROM t")) == 1143);
    REQUIRE(errno_of(a.execute_sql("SELECT i FROM t WHERE id = 1")) == 1143);
    REQUIRE(errno_of(a.execute_sql("SELECT * FROM t")) == 1143);
    REQUIRE(errno_of(a.execute_sql("SELECT i FROM t, u")) == 1142);
    REQUIRE(a.execute_sql("UPDATE u SET j = 1").is_ok());
    REQUIRE(errno_of(a.execute_sql("UPDATE u SET j = 1 WHERE id = 1")) == 1143);
    REQUIRE(errno_of(a.execute_sql("UPDATE u SET id = 1")) == 1143);
}

TEST_CASE("roles: privileges come with the roles that are switched on", "[privileges][roles]") {
    World w("priv_data_6");
    World::run(w.root, "CREATE ROLE 'reader', 'writer'");
    World::run(w.root, "GRANT SELECT ON v.* TO 'reader'");
    World::run(w.root, "GRANT INSERT ON v.* TO 'writer'");
    World::run(w.root, "CREATE USER 'a'@'%' IDENTIFIED BY 'pw'");
    World::run(w.root, "GRANT 'reader' TO 'a'@'%'");
    Executor a = w.session("a");

    auto current = [&]() {
        auto r = a.execute_sql("SELECT CURRENT_ROLE()");
        REQUIRE(r.is_ok());
        return r.value();
    };
    REQUIRE(current().find("NONE") != std::string::npos);
    REQUIRE(errno_of(a.execute_sql("SELECT * FROM t")) == 1142); // a role granted is not a role switched on
    REQUIRE(a.execute_sql("SET ROLE 'reader'").is_ok());
    REQUIRE(current().find("`reader`@`%`") != std::string::npos);
    REQUIRE(a.execute_sql("SELECT * FROM t").is_ok());
    REQUIRE(a.execute_sql("SET ROLE NONE").is_ok());
    REQUIRE(errno_of(a.execute_sql("SELECT * FROM t")) == 1142);
    REQUIRE(a.execute_sql("SET ROLE ALL").is_ok());
    REQUIRE(a.execute_sql("SELECT * FROM t").is_ok());
    REQUIRE(errno_of(a.execute_sql("SET ROLE 'writer'")) == 3530);

    World::run(w.root, "GRANT 'writer' TO 'a'@'%'");
    REQUIRE(a.execute_sql("SET ROLE 'writer'").is_ok());
    REQUIRE(a.execute_sql("INSERT INTO t VALUES (60, 1)").is_ok());
    REQUIRE(errno_of(a.execute_sql("SELECT * FROM t")) == 1142);
    REQUIRE(a.execute_sql("SET ROLE ALL EXCEPT 'writer'").is_ok());
    REQUIRE(current().find("`reader`@`%`") != std::string::npos);
    REQUIRE(current().find("writer") == std::string::npos);

    // a revoked role is no longer active
    World::run(w.root, "REVOKE 'reader' FROM 'a'@'%'");
    REQUIRE(current().find("NONE") != std::string::npos);
    REQUIRE(errno_of(a.execute_sql("SELECT * FROM t")) == 1142);
}

TEST_CASE("roles that hold roles, default roles, and the errors of role statements", "[privileges][roles]") {
    World w("priv_data_7");
    World::run(w.root, "CREATE ROLE 'outer', 'inner'");
    World::run(w.root, "GRANT SELECT ON v.t TO 'outer'");
    World::run(w.root, "GRANT SELECT ON v.u TO 'inner'");
    World::run(w.root, "GRANT 'inner' TO 'outer'");
    World::run(w.root, "CREATE USER 'a'@'%' IDENTIFIED BY 'pw'");
    World::run(w.root, "GRANT 'outer' TO 'a'@'%' WITH ADMIN OPTION");

    SECTION("the privileges of the inner role come with the outer one") {
        Executor a = w.session("a");
        REQUIRE(a.execute_sql("SET ROLE 'outer'").is_ok());
        REQUIRE(a.execute_sql("SELECT * FROM t").is_ok());
        REQUIRE(a.execute_sql("SELECT * FROM u").is_ok());
        auto shown = w.root.execute_sql("SHOW GRANTS FOR 'a'@'%' USING 'outer'");
        REQUIRE(shown.is_ok());
        REQUIRE(shown.value().find("GRANT SELECT ON `v`.`t` TO `a`@`%`") != std::string::npos);
        REQUIRE(shown.value().find("GRANT SELECT ON `v`.`u` TO `a`@`%`") != std::string::npos);
        REQUIRE(shown.value().find("GRANT `outer`@`%` TO `a`@`%` WITH ADMIN OPTION") != std::string::npos);
    }
    SECTION("WITH ADMIN OPTION lets the user pass the role on") {
        World::run(w.root, "CREATE USER 'b'@'%'");
        Executor a = w.session("a");
        REQUIRE(a.execute_sql("GRANT 'outer' TO 'b'@'%'").is_ok());
        REQUIRE(errno_of(a.execute_sql("GRANT 'inner' TO 'b'@'%'")) == 1227);
        REQUIRE(a.execute_sql("REVOKE 'outer' FROM 'b'@'%'").is_ok());
        REQUIRE(errno_of(a.execute_sql("CREATE ROLE 'r9'")) == 1227);
    }
    SECTION("a default role is on from the login") {
        World::run(w.root, "SET DEFAULT ROLE 'outer' TO 'a'@'%'");
        Executor a = w.session("a");
        auto current = a.execute_sql("SELECT CURRENT_ROLE()");
        REQUIRE(current.is_ok());
        REQUIRE(current.value().find("`outer`@`%`") != std::string::npos);
        REQUIRE(a.execute_sql("SELECT * FROM t").is_ok());
        REQUIRE(a.execute_sql("SET ROLE NONE").is_ok());
        REQUIRE(a.execute_sql("SET ROLE DEFAULT").is_ok());
        REQUIRE(a.execute_sql("SELECT * FROM t").is_ok());
    }
    SECTION("errors") {
        REQUIRE(errno_of(w.root.execute_sql("CREATE ROLE 'outer'")) == 1396);
        REQUIRE(w.root.execute_sql("CREATE ROLE IF NOT EXISTS 'outer'").is_ok());
        REQUIRE(errno_of(w.root.execute_sql("CREATE ROLE 'a'")) == 1396);   // a user of that name
        REQUIRE(errno_of(w.root.execute_sql("CREATE USER 'inner'@'%'")) == 1396); // a role of that name
        REQUIRE(errno_of(w.root.execute_sql("GRANT 'nosuchrole' TO 'a'@'%'")) == 3523);
        REQUIRE(errno_of(w.root.execute_sql("GRANT 'outer' TO 'nobody'@'%'")) == 3523);
        REQUIRE(errno_of(w.root.execute_sql("REVOKE 'inner' FROM 'a'@'%'")) == 3530);
        REQUIRE(errno_of(w.root.execute_sql("GRANT 'outer' TO 'outer'")) == 4027);
        REQUIRE(errno_of(w.root.execute_sql("GRANT 'outer' TO 'inner'")) == 4027);
        REQUIRE(errno_of(w.root.execute_sql("DROP ROLE 'outer', 'nosuch'")) == 1396);
        REQUIRE(w.root.execute_sql("DROP ROLE 'outer', 'inner'").is_ok());
        REQUIRE(errno_of(w.root.execute_sql("DROP ROLE 'inner'")) == 1396);
        REQUIRE(w.root.execute_sql("DROP ROLE IF EXISTS 'inner'").is_ok());
        // dropping the roles took the grants of the roles and the role grants of the user with them
        auto shown = w.root.execute_sql("SHOW GRANTS FOR 'a'@'%'");
        REQUIRE(shown.is_ok());
        REQUIRE(shown.value().find("outer") == std::string::npos);
    }
}

TEST_CASE("CURRENT_USER() is the account, USER() the client", "[privileges]") {
    World w("priv_data_8");
    Executor a = w.session("a");
    auto account = a.execute_sql("SELECT CURRENT_USER()");
    REQUIRE(account.is_ok());
    REQUIRE(account.value().find("a@%") != std::string::npos);
    auto client = a.execute_sql("SELECT USER()");
    REQUIRE(client.is_ok());
    REQUIRE(client.value().find("a@localhost") != std::string::npos);
    auto root = w.root.execute_sql("SELECT CURRENT_USER()");
    REQUIRE(root.is_ok());
    REQUIRE(root.value().find("root@localhost") != std::string::npos);
}

TEST_CASE("users: ALTER, RENAME, DROP, SET PASSWORD", "[privileges]") {
    World w("priv_data_9");
    World::run(w.root, "CREATE USER 'a'@'%' IDENTIFIED BY 'one'");
    World::run(w.root, "GRANT SELECT ON v.t TO 'a'@'%'");
    auto hash_of = [&](const std::string& user) {
        auto s = w.shared->read();
        for (const UserRecord& u : s->users) {
            if (u.user == user) return u.mysql_native_hash.value_or("");
        }
        return std::string("none");
    };
    const std::string first = hash_of("a");
    REQUIRE(first != "none");
    World::run(w.root, "ALTER USER 'a'@'%' IDENTIFIED BY 'two'");
    const std::string second = hash_of("a");
    REQUIRE(second != first);
    World::run(w.root, "SET PASSWORD FOR 'a'@'%' = 'three'");
    const std::string third = hash_of("a");
    REQUIRE(third != second);
    Executor a = w.session("a");
    World::run(a, "SET PASSWORD = 'four'");
    REQUIRE(hash_of("a") != third);
    REQUIRE(errno_of(w.root.execute_sql("SET PASSWORD FOR 'nobody'@'%' = 'x'")) == 1133);

    SECTION("RENAME USER takes the grants along") {
        World::run(w.root, "RENAME USER 'a'@'%' TO 'b'@'%'");
        auto shown = w.root.execute_sql("SHOW GRANTS FOR 'b'@'%'");
        REQUIRE(shown.is_ok());
        REQUIRE(shown.value().find("GRANT SELECT ON `v`.`t` TO `b`@`%`") != std::string::npos);
        REQUIRE(errno_of(w.root.execute_sql("SHOW GRANTS FOR 'a'@'%'")) == 1141);
        REQUIRE(errno_of(w.root.execute_sql("RENAME USER 'a'@'%' TO 'c'@'%'")) == 1396);
    }
    SECTION("DROP USER takes them away, and a user made again starts with none") {
        World::run(w.root, "DROP USER 'a'@'%'");
        World::run(w.root, "CREATE USER 'a'@'%'");
        auto shown = w.root.execute_sql("SHOW GRANTS FOR 'a'@'%'");
        REQUIRE(shown.is_ok());
        REQUIRE(shown.value().find("`v`") == std::string::npos);
    }
    SECTION("a list of accounts is all or nothing") {
        REQUIRE(errno_of(w.root.execute_sql("DROP USER 'a'@'%', 'nobody'@'%'")) == 1396);
        REQUIRE(w.root.execute_sql("SHOW GRANTS FOR 'a'@'%'").is_ok());
        REQUIRE(errno_of(w.root.execute_sql("CREATE USER 'x'@'%', 'a'@'%'")) == 1396);
        REQUIRE(errno_of(w.root.execute_sql("SHOW GRANTS FOR 'x'@'%'")) == 1141);
    }
}
