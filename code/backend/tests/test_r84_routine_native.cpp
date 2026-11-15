// R84: what the R83 leftovers of RECOMMAND.md asked for -- SHOW CREATE DATABASE / PROCEDURE / FUNCTION / TRIGGER, SHOW TRIGGERS and SHOW PROCEDURE | FUNCTION STATUS in the
// native protocol (the app, the CLI, MCP) and not only on the MySQL wire, the account and the time a routine was made by (DEFINER, CREATED, LAST_ALTERED), and who may see the
// text of a routine. The answers were measured on MySQL 8.4; code/test/diff/corpus/routines_r84.sql repeats them against the server.

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>

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

void ok(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
}

std::string text_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    return r.value();
}

std::string error_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_err());
    return r.error();
}

// the rows of an answer table as "a,b;c,d" (every cell trimmed)
std::string flat(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    std::istringstream in(r.value());
    std::string line, out;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars == 1) continue; // (the header)
        std::string row;
        std::size_t from = 1;
        while (from < line.size()) {
            const std::size_t end = line.find('|', from);
            if (end == std::string::npos) break;
            std::string cell = line.substr(from, end - from);
            const std::size_t a = cell.find_first_not_of(' '), b = cell.find_last_not_of(' ');
            row += (row.empty() ? "" : ",") + (a == std::string::npos ? std::string() : cell.substr(a, b - a + 1));
            from = end + 1;
        }
        out += (out.empty() ? "" : ";") + row;
    }
    return out;
}

int errno_of(const StringResult& r) {
    if (r.is_ok()) return 0;
    const std::string& e = r.error();
    return e.compare(0, 6, "ERROR ") == 0 ? std::stoi(e.substr(6, 4)) : -1;
}

struct World {
    TempDataDir dir;
    Executor root;
    explicit World(const std::string& name) : dir(name), root(dir.path) {
        ok(root, "CREATE DATABASE v");
        ok(root, "USE v");
        ok(root, "CREATE TABLE t (id INT PRIMARY KEY, n INT)");
        ok(root, "CREATE PROCEDURE p() SELECT 1");
        ok(root, "CREATE FUNCTION f() RETURNS INT DETERMINISTIC RETURN 1");
        ok(root, "CREATE TRIGGER tr BEFORE INSERT ON t FOR EACH ROW SET NEW.n = 1");
    }
    Executor session(const std::string& user) {
        ok(root, "CREATE USER IF NOT EXISTS '" + user + "'@'%' IDENTIFIED BY 'pw'");
        Executor ex = Executor::new_session(root.get_shared());
        ex.auth_user = user;
        ex.activate_default_roles();
        ex.current_db = "v";
        return ex;
    }
};
} // namespace

TEST_CASE("R84: SHOW CREATE PROCEDURE / FUNCTION / TRIGGER / DATABASE in the native protocol give the CREATE text itself", "[r84]") {
    World w("test_r84_native_data");
    Executor& ex = w.root;
    const std::string proc = "CREATE DEFINER=`root`@`localhost` PROCEDURE `p`()\nSELECT 1";
    CHECK(text_of(ex, "SHOW CREATE PROCEDURE p") == proc);
    CHECK(text_of(ex, "SHOW CREATE PROCEDURE v.p") == proc);
    CHECK(text_of(ex, "SHOW CREATE PROCEDURE `v`.`p`;") == proc);
    CHECK(text_of(ex, "show   create\n procedure  P") == proc); // (white space and case)
    CHECK(text_of(ex, "SHOW CREATE FUNCTION f") == "CREATE DEFINER=`root`@`localhost` FUNCTION `f`() RETURNS int\n    DETERMINISTIC\nRETURN 1");
    CHECK(text_of(ex, "SHOW CREATE TRIGGER tr") == "CREATE DEFINER=`root`@`localhost` TRIGGER `tr` BEFORE INSERT ON `t` FOR EACH ROW SET NEW.n = 1");
    const std::string database = "CREATE DATABASE `v` /*!40100 DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci */ /*!80016 DEFAULT ENCRYPTION='N' */";
    CHECK(text_of(ex, "SHOW CREATE DATABASE v") == database);
    CHECK(text_of(ex, "SHOW CREATE SCHEMA `v`") == database);
    CHECK(text_of(ex, "SHOW CREATE DATABASE IF NOT EXISTS v").find("CREATE DATABASE /*!32312 IF NOT EXISTS*/ `v`") == 0);
    // the same text as the C++ function the MySQL protocol layer builds its row from
    CHECK(text_of(ex, "SHOW CREATE PROCEDURE p") == ex.show_create_routine_mysql("procedure", "p").value());

    // what is not there
    CHECK(error_of(ex, "SHOW CREATE PROCEDURE nope") == "ERROR 1305 (42000): PROCEDURE nope does not exist");
    CHECK(error_of(ex, "SHOW CREATE FUNCTION nope") == "ERROR 1305 (42000): FUNCTION nope does not exist");
    CHECK(error_of(ex, "SHOW CREATE TRIGGER nope") == "ERROR 1360 (HY000): Trigger does not exist");
    CHECK(error_of(ex, "SHOW CREATE DATABASE nope") == "ERROR 1049 (42000): Unknown database 'nope'");
    // TABLE and VIEW are still the parser's, and a name that is missing or followed by more is a syntax error
    CHECK(text_of(ex, "SHOW CREATE TABLE t").find("CREATE TABLE `t`") != std::string::npos);
    CHECK(errno_of(ex.execute_sql("SHOW CREATE PROCEDURE")) != 0);
    CHECK(errno_of(ex.execute_sql("SHOW CREATE PROCEDURE p extra")) != 0);
    CHECK(errno_of(ex.execute_sql("SHOW CREATE DATABASE")) != 0);
    CHECK(errno_of(ex.execute_sql("SHOW CREATE DATABASE nothere.mysql")) != 0); // (a database has no database in front of its name)
    CHECK(errno_of(ex.execute_sql("SHOW CREATE DATABASE v extra")) != 0);
}

TEST_CASE("R84: SHOW TRIGGERS and SHOW PROCEDURE | FUNCTION STATUS in the native protocol", "[r84]") {
    World w("test_r84_status_data");
    Executor& ex = w.root;
    ok(ex, "CREATE DATABASE w");
    ok(ex, "USE w");
    ok(ex, "CREATE PROCEDURE pw() SELECT 2");
    ok(ex, "CREATE FUNCTION fw() RETURNS INT DETERMINISTIC RETURN 2");
    ok(ex, "USE v");
    // (the column names are MySQL's)
    const std::string trigger_header = text_of(ex, "SHOW TRIGGERS");
    for (const char* column : {"Trigger", "Event", "Table", "Statement", "Timing", "Created", "sql_mode", "Definer", "character_set_client", "collation_connection", "Database Collation"}) {
        CHECK(trigger_header.find(std::string("| ") + column) != std::string::npos);
    }
    CHECK(text_of(ex, "SHOW TRIGGERS").find("| tr ") != std::string::npos);
    CHECK(text_of(ex, "SHOW TRIGGERS FROM v").find("| tr ") != std::string::npos);
    CHECK(text_of(ex, "SHOW TRIGGERS IN w").find("| tr ") == std::string::npos);
    CHECK(text_of(ex, "SHOW TRIGGERS LIKE 't'").find("| tr ") != std::string::npos);
    CHECK(text_of(ex, "SHOW TRIGGERS LIKE 'zzz'").find("| tr ") == std::string::npos);
    CHECK(text_of(ex, "SHOW TRIGGERS WHERE Timing = 'AFTER'").find("| tr ") == std::string::npos);
    CHECK(text_of(ex, "SHOW TRIGGERS WHERE Timing = 'BEFORE' AND Event = 'INSERT'").find("| tr ") != std::string::npos);

    const std::string procedures = text_of(ex, "SHOW PROCEDURE STATUS");
    CHECK(procedures.find("| p ") != std::string::npos);
    CHECK(procedures.find("| pw ") != std::string::npos);
    CHECK(procedures.find("| f ") == std::string::npos); // (a function is not a procedure)
    for (const char* column : {"Db", "Name", "Type", "Definer", "Modified", "Created", "Security_type", "Comment"}) CHECK(procedures.find(std::string("| ") + column) != std::string::npos);
    CHECK(flat(ex, "SHOW PROCEDURE STATUS WHERE Db = 'w'").find("pw") != std::string::npos);
    CHECK(flat(ex, "SHOW PROCEDURE STATUS WHERE Db = 'w'").find(",p,") == std::string::npos);
    CHECK(text_of(ex, "SHOW PROCEDURE STATUS LIKE 'p%'").find("| pw ") != std::string::npos);
    CHECK(text_of(ex, "SHOW PROCEDURE STATUS LIKE 'pw'").find("| p ") == std::string::npos);
    CHECK(text_of(ex, "SHOW FUNCTION STATUS").find("| f ") != std::string::npos);
    CHECK(text_of(ex, "SHOW FUNCTION STATUS").find("| p ") == std::string::npos);
    CHECK(text_of(ex, "SHOW FUNCTION STATUS LIKE 'fw'").find("| fw ") != std::string::npos);
    CHECK(text_of(ex, "SHOW FUNCTION STATUS LIKE 'fw';").find("| f ") == std::string::npos);
}

TEST_CASE("R84: a routine keeps the account that made it and when", "[r84]") {
    TempDataDir dir("test_r84_account_data");
    const std::regex when_routine(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})");
    const std::regex when_trigger(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}\.\d{2})");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE v");
        ok(ex, "USE v");
        ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, n INT)");
        ok(ex, "CREATE USER 'alice'@'%' IDENTIFIED BY 'pw'");
        ok(ex, "GRANT ALL ON v.* TO 'alice'@'%'");
        ok(ex, "CREATE PROCEDURE p() SELECT 1");
        ok(ex, "CREATE DEFINER=`bob`@`%` PROCEDURE pb() SELECT 2");
        ok(ex, "CREATE DEFINER='carol'@'host1' FUNCTION fc() RETURNS INT DETERMINISTIC RETURN 3");
        ok(ex, "CREATE DEFINER=CURRENT_USER PROCEDURE pcu() SELECT 4");
        ok(ex, "CREATE DEFINER=CURRENT_USER() PROCEDURE pcu2() SELECT 5");
        ok(ex, "CREATE DEFINER=`dave`@`%` TRIGGER tr AFTER INSERT ON t FOR EACH ROW SET @x = 1");
        { // an account that makes a routine is its definer
            Executor alice = Executor::new_session(ex.get_shared());
            alice.auth_user = "alice";
            alice.current_db = "v";
            ok(alice, "CREATE PROCEDURE pa() SELECT 6");
            ok(alice, "CREATE TRIGGER ta BEFORE INSERT ON t FOR EACH ROW SET NEW.n = 7");
        }
        CHECK(text_of(ex, "SHOW CREATE PROCEDURE p").find("DEFINER=`root`@`localhost` PROCEDURE") != std::string::npos);
        CHECK(text_of(ex, "SHOW CREATE PROCEDURE pb").find("DEFINER=`bob`@`%` PROCEDURE `pb`") != std::string::npos);
        CHECK(text_of(ex, "SHOW CREATE FUNCTION fc").find("DEFINER=`carol`@`host1` FUNCTION `fc`") != std::string::npos);
        CHECK(text_of(ex, "SHOW CREATE PROCEDURE pcu").find("DEFINER=`root`@`localhost` PROCEDURE") != std::string::npos);
        CHECK(text_of(ex, "SHOW CREATE PROCEDURE pcu2").find("DEFINER=`root`@`localhost` PROCEDURE") != std::string::npos);
        CHECK(text_of(ex, "SHOW CREATE TRIGGER tr").find("DEFINER=`dave`@`%` TRIGGER `tr`") != std::string::npos);
        CHECK(text_of(ex, "SHOW CREATE PROCEDURE pa").find("DEFINER=`alice`@`%` PROCEDURE `pa`") != std::string::npos);
        CHECK(text_of(ex, "SHOW CREATE TRIGGER ta").find("DEFINER=`alice`@`%` TRIGGER `ta`") != std::string::npos);
        // information_schema and the SHOW lists say the same
        CHECK(flat(ex, "SELECT routine_name, definer FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name") ==
              "fc,carol@host1;p,root@localhost;pa,alice@%;pb,bob@%;pcu,root@localhost;pcu2,root@localhost");
        CHECK(flat(ex, "SELECT trigger_name, definer FROM information_schema.triggers WHERE trigger_schema = 'v' ORDER BY trigger_name") == "ta,alice@%;tr,dave@%");
        CHECK(text_of(ex, "SHOW PROCEDURE STATUS WHERE Name = 'pb'").find("| bob@% ") != std::string::npos);
        CHECK(text_of(ex, "SHOW TRIGGERS WHERE `Trigger` = 'tr'").find("| dave@% ") != std::string::npos);

        // the time: to the second for a routine, to a hundredth for a trigger (MySQL's TIMESTAMP(2)); a routine is not altered, so the two times are one
        const std::string made = flat(ex, "SELECT created FROM information_schema.routines WHERE routine_name = 'p'");
        CHECK(std::regex_match(made, when_routine));
        CHECK(flat(ex, "SELECT created = last_altered FROM information_schema.routines WHERE routine_name = 'p'") == "1");
        const std::string trigger_made = flat(ex, "SELECT created FROM information_schema.triggers WHERE trigger_name = 'tr'");
        CHECK(std::regex_match(trigger_made, when_trigger));
        CHECK(text_of(ex, "SHOW PROCEDURE STATUS WHERE Name = 'p'").find(made) != std::string::npos);
        CHECK(text_of(ex, "SHOW TRIGGERS WHERE `Trigger` = 'tr'").find(trigger_made) != std::string::npos);
        // made again, it has the new account and a time that is not earlier
        ok(ex, "DROP PROCEDURE pb");
        ok(ex, "CREATE PROCEDURE pb() SELECT 22");
        CHECK(text_of(ex, "SHOW CREATE PROCEDURE pb").find("DEFINER=`root`@`localhost` PROCEDURE `pb`") != std::string::npos);
        CHECK(flat(ex, "SELECT created >= '" + made + "' FROM information_schema.routines WHERE routine_name = 'pb'") == "1");
    }
    { // the account and the time are on disk: a server that starts again tells them
        Executor ex(dir.path);
        ok(ex, "USE v");
        CHECK(text_of(ex, "SHOW CREATE FUNCTION fc").find("DEFINER=`carol`@`host1` FUNCTION `fc`") != std::string::npos);
        CHECK(text_of(ex, "SHOW CREATE TRIGGER ta").find("DEFINER=`alice`@`%` TRIGGER `ta`") != std::string::npos);
        CHECK(std::regex_match(flat(ex, "SELECT created FROM information_schema.routines WHERE routine_name = 'fc'"), when_routine));
        CHECK(std::regex_match(flat(ex, "SELECT created FROM information_schema.triggers WHERE trigger_name = 'tr'"), when_trigger));
    }
}

TEST_CASE("R84: a routine kept before the account and the time were kept still reads", "[r84]") {
    TempDataDir dir("test_r84_legacy_data");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE v");
        ok(ex, "USE v");
        ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, n INT)");
        ok(ex, "CREATE PROCEDURE old1() SELECT 1");
        ok(ex, "CREATE TRIGGER told BEFORE INSERT ON t FOR EACH ROW SET NEW.n = 2");
    }
    // R83 wrote [database, text] for each routine; R84 writes an object (the lines below are what R83 wrote)
    {
        std::ofstream file(dir.path + "/_system/_routine_sources.json", std::ios::trunc);
        file << R"({"procedure:old1":["v","SELECT 1"],"trigger:told":["v","SET NEW.n = 2"]})";
    }
    Executor ex(dir.path);
    ok(ex, "USE v");
    CHECK(text_of(ex, "SHOW CREATE PROCEDURE old1") == "CREATE DEFINER=`root`@`localhost` PROCEDURE `old1`()\nSELECT 1");
    CHECK(flat(ex, "SELECT definer FROM information_schema.routines WHERE routine_name = 'old1'") == "root@localhost");
    CHECK(flat(ex, "SELECT created IS NULL, last_altered IS NULL FROM information_schema.routines WHERE routine_name = 'old1'") == "1,1");
    CHECK(flat(ex, "SELECT created IS NULL FROM information_schema.triggers WHERE trigger_name = 'told'") == "1");
    CHECK(text_of(ex, "SHOW CREATE TRIGGER told").find("SET NEW.n = 2") != std::string::npos);
    // making one again gives it the account and the time
    ok(ex, "DROP PROCEDURE old1");
    ok(ex, "CREATE PROCEDURE old1() SELECT 3");
    CHECK(flat(ex, "SELECT created IS NOT NULL FROM information_schema.routines WHERE routine_name = 'old1'") == "1");
}

TEST_CASE("R84: who may see the text of a routine (measured on MySQL 8.4)", "[r84][privileges]") {
    World w("test_r84_priv_data");
    for (const char* user : {"alice", "bob", "carol", "dan", "erin", "frank", "gina"}) ok(w.root, std::string("CREATE USER '") + user + "'@'%' IDENTIFIED BY 'pw'");
    ok(w.root, "GRANT SELECT ON v.* TO 'alice'@'%'");
    ok(w.root, "GRANT EXECUTE ON PROCEDURE v.p TO 'bob'@'%'");
    ok(w.root, "GRANT SELECT ON v.t TO 'carol'@'%'");
    ok(w.root, "GRANT CREATE ROUTINE ON v.* TO 'dan'@'%'");
    ok(w.root, "GRANT TRIGGER ON v.t TO 'erin'@'%'");
    ok(w.root, "GRANT SHOW_ROUTINE ON *.* TO 'frank'@'%'");
    ok(w.root, "GRANT ALTER ROUTINE ON FUNCTION v.f TO 'gina'@'%'");
    // an account that has none of EXECUTE, ALTER ROUTINE, CREATE ROUTINE (on the routine, its database or everywhere) and SHOW_ROUTINE is told there is no such routine
    const std::string no_proc = "ERROR 1305 (42000): PROCEDURE p does not exist";
    const std::string no_func = "ERROR 1305 (42000): FUNCTION f does not exist";
    struct Row { const char* user; bool proc; bool func; bool trigger; };
    for (const Row& row : {Row{"alice", false, false, false}, Row{"bob", true, false, false}, Row{"carol", false, false, false}, Row{"dan", true, true, false},
                           Row{"erin", false, false, true}, Row{"frank", true, true, false}, Row{"gina", false, true, false}}) {
        INFO(row.user);
        Executor who = w.session(row.user);
        for (const std::string sql : {"SHOW CREATE PROCEDURE p", "SHOW CREATE PROCEDURE v.p"}) {
            auto r = who.execute_sql(sql);
            INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
            CHECK(r.is_ok() == row.proc);
            if (!row.proc) CHECK(r.error() == no_proc);
        }
        auto f = who.execute_sql("SHOW CREATE FUNCTION f");
        CHECK(f.is_ok() == row.func);
        if (!row.func) CHECK(f.error() == no_func);
        auto t = who.execute_sql("SHOW CREATE TRIGGER tr");
        CHECK(t.is_ok() == row.trigger);
        if (!row.trigger) CHECK(t.error() == "ERROR 1227 (42000): Access denied; you need (at least one of) the TRIGGER privilege(s) for this operation");
        // a routine that is not there is the same error for everyone
        CHECK(errno_of(who.execute_sql("SHOW CREATE PROCEDURE nope")) == 1305);
    }
    // the database: an account that holds a privilege on it may; one that holds only a global SHOW_ROUTINE may not (1044)
    CHECK(w.session("alice").execute_sql("SHOW CREATE DATABASE v").is_ok());
    CHECK(w.session("dan").execute_sql("SHOW CREATE DATABASE v").is_ok());
    CHECK(errno_of(w.session("frank").execute_sql("SHOW CREATE DATABASE v")) == 1044);
    CHECK(errno_of(w.session("bob").execute_sql("SHOW CREATE DATABASE nope")) != 0);
}

TEST_CASE("R84: which views take writes (information_schema.views.IS_UPDATABLE) and the errors of the ones that do not (measured on MySQL 8.4)", "[r84][views]") {
    TempDataDir dir("test_r84_views_data");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE v");
    ok(ex, "USE v");
    ok(ex, "CREATE TABLE ua (id INT PRIMARY KEY, k INT, name VARCHAR(10))");
    ok(ex, "CREATE TABLE ub (id INT PRIMARY KEY, ua_id INT, qty INT)");
    ok(ex, "INSERT INTO ua VALUES (1, 10, 'a'), (2, 20, 'b')");
    ok(ex, "INSERT INTO ub VALUES (1, 1, 5), (2, 1, 7), (3, 2, 9)");
    for (const char* view : {
             "vu_plain AS SELECT id, k FROM ua", "vu_where AS SELECT id, k FROM ua WHERE k > 5", "vu_expr AS SELECT id, k + 1 AS k1 FROM ua", "vu_const AS SELECT 1 AS one",
             "vu_distinct AS SELECT DISTINCT k FROM ua", "vu_group AS SELECT k, COUNT(*) AS n FROM ua GROUP BY k", "vu_agg AS SELECT COUNT(*) AS n FROM ua",
             "vu_having AS SELECT k FROM ua GROUP BY k HAVING COUNT(*) > 0", "vu_union AS SELECT id FROM ua UNION SELECT id FROM ub", "vu_unionall AS SELECT id FROM ua UNION ALL SELECT id FROM ub",
             "vu_join AS SELECT ua.id, ub.qty FROM ua JOIN ub ON ub.ua_id = ua.id", "vu_joinagg AS SELECT ua.id, COUNT(ub.id) AS n FROM ua JOIN ub ON ub.ua_id = ua.id GROUP BY ua.id",
             "vu_leftjoin AS SELECT ua.id, ub.qty FROM ua LEFT JOIN ub ON ub.ua_id = ua.id", "vu_subsel AS SELECT id, (SELECT COUNT(*) FROM ub WHERE ub.ua_id = ua.id) AS n FROM ua",
             "vu_subwhere AS SELECT id, k FROM ua WHERE id IN (SELECT ua_id FROM ub)", "vu_limit AS SELECT id, k FROM ua LIMIT 1", "vu_order AS SELECT id, k FROM ua ORDER BY k DESC",
             "vu_ofview AS SELECT id FROM vu_plain", "vu_ofagg AS SELECT k FROM vu_group", "vu_win AS SELECT id, ROW_NUMBER() OVER (ORDER BY id) AS rn FROM ua",
             "vu_func AS SELECT id, UPPER(name) AS up FROM ua", "vu_selfjoin AS SELECT a.id, b.k FROM ua a JOIN ua b ON a.id = b.id", "vu_cross AS SELECT ua.id, ub.qty FROM ua, ub",
             "vu_chk AS SELECT id, k FROM ua WHERE k > 5 WITH CHECK OPTION", "vu_merge AS SELECT id, k FROM ua", "vu_exists AS SELECT id FROM ua WHERE EXISTS (SELECT 1 FROM ub WHERE ub.ua_id = ua.id)",
             "vu_distinctrow AS SELECT DISTINCT id, k FROM ua", "vu_star AS SELECT * FROM ua", "vu_joinexpr AS SELECT ua.id, ub.qty + 1 AS q FROM ua JOIN ub ON ub.ua_id = ua.id"}) {
        ok(ex, std::string("CREATE VIEW ") + view);
    }
    // the flag, as MySQL 8.4 gives it for each (a view with a subquery in its select list says YES although it takes no write)
    CHECK(flat(ex, "SELECT table_name, is_updatable FROM information_schema.views ORDER BY table_name") ==
          "vu_agg,NO;vu_chk,YES;vu_const,NO;vu_cross,YES;vu_distinct,NO;vu_distinctrow,NO;vu_exists,YES;vu_expr,YES;vu_func,YES;vu_group,NO;vu_having,NO;vu_join,YES;vu_joinagg,NO;"
          "vu_joinexpr,YES;vu_leftjoin,NO;vu_limit,NO;vu_merge,YES;vu_ofagg,NO;vu_ofview,YES;vu_order,YES;vu_plain,YES;vu_selfjoin,YES;vu_star,YES;vu_subsel,YES;vu_subwhere,YES;"
          "vu_union,NO;vu_unionall,NO;vu_where,YES;vu_win,NO");

    auto num = [&](const char* sql) { return error_of(ex, sql); };
    // a join view that has a GROUP BY (an aggregate, DISTINCT ...) is refused as any view of that kind is: UPDATE and DELETE 1288, INSERT 1471 (it was error 1064 and 1395)
    CHECK(num("UPDATE vu_joinagg SET n = 1") == "ERROR 1288 (HY000): The target table vu_joinagg of the UPDATE is not updatable");
    CHECK(num("DELETE FROM vu_joinagg") == "ERROR 1288 (HY000): The target table vu_joinagg of the DELETE is not updatable");
    CHECK(num("INSERT INTO vu_joinagg (id, n) VALUES (1, 1)") == "ERROR 1471 (HY000): The target table vu_joinagg of the INSERT is not insertable-into");
    // ... and a join view that takes writes keeps its own errors
    CHECK(num("DELETE FROM vu_cross WHERE id = 1").find("ERROR 1395") == 0);
    CHECK(num("UPDATE vu_leftjoin SET qty = 1").find("ERROR 1288") == 0);
    CHECK(num("INSERT INTO vu_leftjoin (id, qty) VALUES (80, 1)").find("ERROR 1471") == 0);
    CHECK(num("DELETE FROM vu_leftjoin WHERE id = 1").find("ERROR 1288") == 0);
    // a view with a subquery in its select list takes no write (the flag says YES all the same)
    CHECK(num("UPDATE vu_subsel SET id = 60 WHERE id = 1").find("ERROR 1288") == 0);
    CHECK(num("INSERT INTO vu_subsel (id) VALUES (50)").find("ERROR 1471") == 0);
    CHECK(num("DELETE FROM vu_subsel WHERE id = 1").find("ERROR 1288") == 0);
    // INSERT into a view that has a column of an expression: 1348 when the INSERT writes that column (names it, or has no list of columns), 1471 when it does not
    CHECK(num("INSERT INTO vu_expr (id, k1) VALUES (34, 1)") == "ERROR 1348 (HY000): Column 'k1' is not updatable");
    CHECK(num("INSERT INTO vu_expr VALUES (35, 1)") == "ERROR 1348 (HY000): Column 'k1' is not updatable");
    CHECK(num("INSERT INTO vu_expr (id) VALUES (33)") == "ERROR 1471 (HY000): The target table vu_expr of the INSERT is not insertable-into");
    CHECK(num("INSERT INTO vu_func (id) VALUES (70)").find("ERROR 1471") == 0);
    CHECK(num("UPDATE vu_func SET up = 'x'") == "ERROR 1348 (HY000): Column 'up' is not updatable");
    // a WHERE (or a new value) that reads a column of the view that is an expression finds the rows, as MySQL does (R84 refused it; before that it matched nothing and said nothing --
    // R85 puts the expression in its place: test_r85_view_meta.cpp has the cases)
    ok(ex, "UPDATE vu_expr SET id = 41 WHERE k1 = 11");
    CHECK(flat(ex, "SELECT id, k, name FROM ua ORDER BY id") == "2,20,b;41,10,a");
    ok(ex, "DELETE FROM vu_expr WHERE k1 = 11"); // (the row is k = 10: k1 = 11)
    CHECK(flat(ex, "SELECT id, k, name FROM ua ORDER BY id") == "2,20,b");
    ok(ex, "UPDATE vu_func SET id = 3 WHERE up = 'B'");
    CHECK(flat(ex, "SELECT id, k FROM ua ORDER BY id") == "3,20");
    ok(ex, "UPDATE vu_expr SET id = 2 WHERE id = 3"); // (a condition on the columns the view has of the table works too)
    CHECK(flat(ex, "SELECT id, k FROM ua ORDER BY id") == "2,20");
    ok(ex, "DELETE FROM vu_expr WHERE id = 2");
    CHECK(flat(ex, "SELECT id, k FROM ua ORDER BY id").empty());
}

TEST_CASE("R84: VACUUM keeps what an open snapshot reads when the transactions running at the snapshot have committed since", "[r84][mvcc]") {
    // The GC horizon was the smallest id of the OPEN transactions. A transaction W that began before the reader R and was still running when R's snapshot was taken is not open any more
    // once it has committed, and the rows it replaced are the rows R's snapshot shows: VACUUM (after every 200 changes of a table) erased them, and R found the row gone -- also a
    // `mysqldump --single-transaction` of a database that another session writes to lost a row of the dump that way.
    TempDataDir dir("test_r84_snapshot_data");
    Executor w(dir.path);
    ok(w, "CREATE DATABASE d");
    ok(w, "USE d");
    ok(w, "CREATE TABLE z (id INT PRIMARY KEY, bal INT)");
    ok(w, "INSERT INTO z VALUES (1, 1000)");
    Executor r = Executor::new_session(w.get_shared());
    r.current_db = "d";
    ok(r, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    ok(w, "BEGIN");
    ok(w, "UPDATE z SET bal = bal + 1 WHERE id = 1"); // (W is running ...)
    ok(r, "START TRANSACTION WITH CONSISTENT SNAPSHOT"); // (... when R's snapshot is taken: W began first, so its id is the smaller one)
    ok(w, "COMMIT");
    for (int i = 0; i < 450; i++) ok(w, "UPDATE z SET bal = bal + 1 WHERE id = 1"); // (a statement each; the table is vacuumed after every 200)
    CHECK(flat(r, "SELECT id, bal FROM z") == "1,1000");
    CHECK(flat(r, "SELECT COUNT(*) FROM z") == "1");
    ok(r, "COMMIT");
    CHECK(flat(w, "SELECT id, bal FROM z") == "1,1451");
    // the same when the snapshot is taken by the first read (the default of a REPEATABLE READ transaction)
    ok(w, "BEGIN");
    ok(w, "UPDATE z SET bal = 0 WHERE id = 1");
    ok(r, "START TRANSACTION");
    ok(r, "SELECT COUNT(*) FROM z"); // (the first read takes the snapshot: W is still running)
    ok(w, "COMMIT");
    for (int i = 0; i < 450; i++) ok(w, "UPDATE z SET bal = bal + 1 WHERE id = 1");
    CHECK(flat(r, "SELECT id, bal FROM z") == "1,1451");
    ok(r, "COMMIT");
    // nothing is held once the reader has ended: the next vacuum may take everything dead
    for (int i = 0; i < 450; i++) ok(w, "UPDATE z SET bal = bal + 1 WHERE id = 1");
    CHECK(flat(w, "SELECT id, bal FROM z") == "1,900");
    { // (the floors of ended transactions are forgotten)
        auto kept = w.get_shared()->read();
        CHECK(kept->snapshot_floors->lock()->empty());
    }
    ok(r, "START TRANSACTION WITH CONSISTENT SNAPSHOT"); // (a snapshot is kept while the transaction is open, and forgotten by ROLLBACK as by COMMIT)
    {
        auto kept = w.get_shared()->read();
        CHECK(kept->snapshot_floors->lock()->size() == 1);
    }
    ok(r, "ROLLBACK");
    {
        auto kept = w.get_shared()->read();
        CHECK(kept->snapshot_floors->lock()->empty());
    }
}

TEST_CASE("R84: the system schema mysql is listed (mysqldump --all-databases starts with it) and cannot be made or dropped", "[r84]") {
    TempDataDir dir("test_r84_system_data");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE app");
    CHECK(flat(ex, "SHOW DATABASES") == "app;mysql");
    CHECK(flat(ex, "SELECT schema_name FROM information_schema.schemata ORDER BY schema_name").find("mysql") != std::string::npos);
    ok(ex, "USE mysql");
    CHECK(flat(ex, "SELECT DATABASE()") == "mysql");
    CHECK(text_of(ex, "SHOW CREATE DATABASE mysql").find("CREATE DATABASE `mysql`") == 0);
    CHECK(error_of(ex, "CREATE DATABASE mysql") == "ERROR 3552 (HY000): Access to system schema 'mysql' is rejected.");
    CHECK(error_of(ex, "DROP DATABASE mysql") == "ERROR 3552 (HY000): Access to system schema 'mysql' is rejected.");
    ok(ex, "CREATE DATABASE IF NOT EXISTS mysql"); // (MySQL says OK for this one)
    CHECK(flat(ex, "SHOW DATABASES") == "app;mysql");
}

TEST_CASE("R84: which routines an account is shown, and whether it is shown their text (the matrix measured on MySQL 8.4.9)", "[r84][privileges]") {
    World w("test_r84_matrix_data");
    ok(w.root, "CREATE FUNCTION MixedFn(a INT) RETURNS INT DETERMINISTIC RETURN a"); // (a function is looked up in lower case and shown as it was written)
    ok(w.root, "CREATE DEFINER=`dan`@`%` PROCEDURE pdan() SELECT 3");
    ok(w.root, "CREATE DEFINER=`bob`@`%` PROCEDURE pb() SELECT 2");
    for (const char* user : {"alice", "bob", "dan", "erin", "fran", "gina", "hal", "jon"}) ok(w.root, std::string("CREATE USER '") + user + "'@'%' IDENTIFIED BY 'pw'");
    ok(w.root, "GRANT SELECT ON v.t TO 'alice'@'%'");
    ok(w.root, "GRANT EXECUTE ON PROCEDURE v.p TO 'bob'@'%'");
    ok(w.root, "GRANT CREATE ROUTINE ON v.* TO 'dan'@'%'");
    ok(w.root, "GRANT TRIGGER ON v.t TO 'erin'@'%'");
    ok(w.root, "GRANT SHOW_ROUTINE ON *.* TO 'fran'@'%'");
    ok(w.root, "GRANT SELECT ON v.t TO 'fran'@'%'");
    ok(w.root, "GRANT ALTER ROUTINE ON FUNCTION v.f TO 'gina'@'%'");
    ok(w.root, "GRANT EXECUTE ON v.* TO 'hal'@'%'");
    ok(w.root, "GRANT SELECT ON *.* TO 'jon'@'%'");
    const std::string list = "SELECT routine_name, routine_definition IS NULL FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name";
    // (name, 1 when the text is hidden): the routine rows each account is shown; a 1 is NULL in ROUTINE_DEFINITION
    // (the third: the rows of information_schema.parameters -- f() RETURNS INT is one, MixedFn(a INT) two, a procedure without parameters none)
    struct Row { const char* user; const char* rows; const char* parameters; };
    for (const Row& row : {Row{"alice", "", "0"}, Row{"erin", "", "0"}, Row{"bob", "p,1;pb,0", "0"}, Row{"dan", "f,1;MixedFn,1;p,1;pb,1;pdan,0", "3"}, Row{"fran", "f,0;MixedFn,0;p,0;pb,0;pdan,0", "3"},
                           Row{"gina", "f,1", "1"}, Row{"hal", "f,1;MixedFn,1;p,1;pb,1;pdan,1", "3"}, Row{"jon", "f,0;MixedFn,0;p,0;pb,0;pdan,0", "3"}}) {
        INFO(row.user);
        Executor who = w.session(row.user);
        CHECK(flat(who, list) == row.rows);
        CHECK(flat(who, "SELECT COUNT(*) FROM information_schema.parameters WHERE specific_schema = 'v'") == row.parameters);
    }
    // SHOW CREATE answers by the same rules: the row for an account that sees the routine, the text only if it may see the text (NULL for the rest)
    struct Text { const char* user; const char* proc; const char* text; }; // (the text of PROCEDURE p: "NULL" hidden, "text" shown, "none" not there)
    for (const Text& t : {Text{"alice", "p", "none"}, Text{"bob", "p", "NULL"}, Text{"bob", "pb", "text"}, Text{"bob", "pdan", "none"}, Text{"dan", "p", "NULL"}, Text{"dan", "pdan", "text"}, Text{"dan", "pb", "NULL"},
                          Text{"fran", "p", "text"}, Text{"gina", "p", "none"}, Text{"hal", "p", "NULL"}, Text{"jon", "p", "text"}}) {
        INFO(t.user << " " << t.proc);
        Executor who = w.session(t.user);
        auto r = who.execute_sql(std::string("SHOW CREATE PROCEDURE ") + t.proc);
        if (std::string(t.text) == "none") {
            REQUIRE(r.is_err());
            CHECK(r.error().find("ERROR 1305") == 0);
        } else {
            REQUIRE(r.is_ok());
            if (std::string(t.text) == "NULL") CHECK(r.value() == "NULL");
            else CHECK(r.value().find("CREATE DEFINER=") == 0);
        }
    }
    // the trigger: only an account that holds TRIGGER on its table sees it, and sees the text
    Executor erin = w.session("erin");
    CHECK(flat(erin, "SELECT trigger_name, action_statement IS NULL FROM information_schema.triggers WHERE trigger_schema = 'v'") == "tr,0");
    for (const char* user : {"alice", "bob", "dan", "fran", "gina", "hal", "jon"}) {
        Executor who = w.session(user);
        CHECK(flat(who, "SELECT trigger_name FROM information_schema.triggers WHERE trigger_schema = 'v'").empty());
    }
    // a function: shown as it was written, found in any case
    CHECK(flat(w.root, "SELECT routine_name FROM information_schema.routines WHERE routine_name = 'MixedFn'") == "MixedFn");
    CHECK(text_of(w.root, "SHOW CREATE FUNCTION mixedfn").find("FUNCTION `MixedFn`(a INT)") != std::string::npos);
    CHECK(flat(w.root, "SELECT MIXEDFN(4)") == "4");
    // an account with a privilege on one routine only may USE its database, SHOW CREATE it, and sees nothing else of it (no table)
    Executor bob = w.session("bob");
    ok(bob, "USE v");
    CHECK(text_of(bob, "SHOW CREATE DATABASE v").find("CREATE DATABASE `v`") == 0);
    CHECK(flat(bob, "SHOW TABLES").empty());
    // the database written before the name counts (the routine is in w; the session is in v)
    ok(w.root, "CREATE DATABASE w");
    ok(w.root, "USE w");
    ok(w.root, "CREATE PROCEDURE pw() SELECT 4");
    ok(w.root, "CREATE USER 'kim'@'%' IDENTIFIED BY 'pw'");
    ok(w.root, "GRANT EXECUTE ON w.* TO 'kim'@'%'");
    Executor kim = w.session("kim"); // (its current database is v: it has no right there)
    CHECK(text_of(kim, "SHOW CREATE PROCEDURE pw") == "NULL");
    CHECK(text_of(kim, "SHOW CREATE PROCEDURE w.pw") == "NULL");
    CHECK(error_of(kim, "SHOW CREATE PROCEDURE p").find("ERROR 1305") == 0);
}
