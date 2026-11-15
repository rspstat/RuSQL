// R85: what the CREATE of a view wrote (the account, ALGORITHM, SQL SECURITY) and when a table or a view was made are kept and told (SHOW CREATE VIEW, information_schema.views and
// .tables), a view over a derived table says YES to IS_UPDATABLE although it takes no write, and a write that reads a column of a view that is `k + 1 AS k1` finds its rows.
// The answers were measured on MySQL 8.4.9; code/test/diff/corpus/view_meta_r85.sql and views_updatable_r84.sql repeat them against the server.

#include <filesystem>
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

std::string error_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_err());
    return r.error();
}

std::string flat(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    std::istringstream in(r.value());
    std::string line, out;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars == 1) continue;
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

std::string created_view(Executor& ex, const std::string& view) {
    auto r = ex.show_create_view_mysql(view);
    INFO(view << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    return r.value();
}

struct World {
    TempDataDir dir;
    Executor ex;
    explicit World(const std::string& name) : dir(name), ex(dir.path) {
        ok(ex, "CREATE DATABASE v");
        ok(ex, "USE v");
        ok(ex, "CREATE TABLE ua (id INT PRIMARY KEY, k INT, name VARCHAR(10))");
        ok(ex, "INSERT INTO ua VALUES (1, 10, 'a'), (2, 20, 'b'), (3, 30, 'c')");
    }
};
} // namespace

TEST_CASE("R85: a view keeps the account, ALGORITHM and SQL SECURITY its CREATE wrote, and when it was made", "[r85]") {
    const std::regex when(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})");
    TempDataDir dir("test_r85_meta_data");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE v");
        ok(ex, "USE v");
        ok(ex, "CREATE TABLE ua (id INT PRIMARY KEY, k INT, name VARCHAR(10))");
        ok(ex, "INSERT INTO ua VALUES (1, 10, 'a'), (2, 20, 'b')");
        ok(ex, "CREATE VIEW v1 AS SELECT id, k FROM ua");
        ok(ex, "CREATE DEFINER=`bob`@`%` VIEW v2 AS SELECT id FROM ua");
        ok(ex, "CREATE ALGORITHM = TEMPTABLE VIEW v3 AS SELECT id, k FROM ua");
        ok(ex, "CREATE ALGORITHM = MERGE VIEW v4 AS SELECT id, k FROM ua");
        ok(ex, "CREATE ALGORITHM = UNDEFINED VIEW v5 AS SELECT id, k FROM ua");
        ok(ex, "CREATE SQL SECURITY INVOKER VIEW v7 AS SELECT id FROM ua");
        ok(ex, "CREATE ALGORITHM = MERGE DEFINER = `carol`@`host1` SQL SECURITY INVOKER VIEW v8 AS SELECT id FROM ua");
        const std::string head = "CREATE ALGORITHM=";
        CHECK(created_view(ex, "v1").find(head + "UNDEFINED DEFINER=`root`@`localhost` SQL SECURITY DEFINER VIEW `v1` AS ") == 0);
        CHECK(created_view(ex, "v2").find(head + "UNDEFINED DEFINER=`bob`@`%` SQL SECURITY DEFINER VIEW `v2` AS ") == 0);
        CHECK(created_view(ex, "v3").find(head + "TEMPTABLE DEFINER=`root`@`localhost` SQL SECURITY DEFINER VIEW `v3` AS ") == 0);
        CHECK(created_view(ex, "v4").find(head + "MERGE DEFINER=`root`@`localhost` SQL SECURITY DEFINER VIEW `v4` AS ") == 0);
        CHECK(created_view(ex, "v5").find(head + "UNDEFINED DEFINER") == 0);
        CHECK(created_view(ex, "v7").find("SQL SECURITY INVOKER VIEW `v7`") != std::string::npos);
        CHECK(created_view(ex, "v8").find(head + "MERGE DEFINER=`carol`@`host1` SQL SECURITY INVOKER VIEW `v8` AS ") == 0);
        CHECK(flat(ex, "SELECT table_name, definer, security_type FROM information_schema.views ORDER BY table_name") ==
              "v1,root@localhost,DEFINER;v2,bob@%,DEFINER;v3,root@localhost,DEFINER;v4,root@localhost,DEFINER;v5,root@localhost,DEFINER;v7,root@localhost,INVOKER;v8,carol@host1,INVOKER");
        // the time: a table and a view have one (to the second); information_schema.tables and SHOW TABLE STATUS (read from it) tell it
        const std::string table_made = flat(ex, "SELECT create_time FROM information_schema.tables WHERE table_name = 'ua'");
        const std::string view_made = flat(ex, "SELECT create_time FROM information_schema.tables WHERE table_name = 'v1'");
        CHECK(std::regex_match(table_made, when));
        CHECK(std::regex_match(view_made, when));
        CHECK(flat(ex, "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema = 'v' AND create_time IS NULL") == "0");
        CHECK(flat(ex, "SELECT update_time IS NULL FROM information_schema.tables WHERE table_name = 'v1'") == "1"); // (a view has no update time; a table's is not kept)
        // TEMPTABLE: the rows are made first, so no write goes through: IS_UPDATABLE NO, 1288 / 1471; MERGE and UNDEFINED are the views that were always writable
        CHECK(flat(ex, "SELECT table_name, is_updatable FROM information_schema.views ORDER BY table_name") == "v1,YES;v2,YES;v3,NO;v4,YES;v5,YES;v7,YES;v8,YES");
        CHECK(error_of(ex, "UPDATE v3 SET k = 11 WHERE id = 1") == "ERROR 1288 (HY000): The target table v3 of the UPDATE is not updatable");
        CHECK(error_of(ex, "DELETE FROM v3 WHERE id = 99") == "ERROR 1288 (HY000): The target table v3 of the DELETE is not updatable");
        CHECK(error_of(ex, "INSERT INTO v3 (id, k) VALUES (9, 9)") == "ERROR 1471 (HY000): The target table v3 of the INSERT is not insertable-into");
        ok(ex, "CREATE VIEW v3b AS SELECT id FROM v3"); // (a view over a view that takes no write takes none either)
        CHECK(flat(ex, "SELECT is_updatable FROM information_schema.views WHERE table_name = 'v3b'") == "NO");
        CHECK(error_of(ex, "UPDATE v3b SET id = 5 WHERE id = 1").find("ERROR 1288") == 0);
        ok(ex, "UPDATE v4 SET k = 11 WHERE id = 1");
        CHECK(flat(ex, "SELECT k FROM ua WHERE id = 1") == "11");
        // a join view that is TEMPTABLE takes no write either (its DELETE is 1288, not the 1395 of a join view that is written through)
        ok(ex, "CREATE TABLE ub (id INT PRIMARY KEY, ua_id INT, qty INT)");
        ok(ex, "CREATE VIEW jv AS SELECT ua.id, ub.qty FROM ua JOIN ub ON ub.ua_id = ua.id");
        ok(ex, "CREATE ALGORITHM = TEMPTABLE VIEW tj AS SELECT ua.id, ub.qty FROM ua JOIN ub ON ub.ua_id = ua.id");
        CHECK(flat(ex, "SELECT table_name, is_updatable FROM information_schema.views WHERE table_name IN ('jv', 'tj') ORDER BY table_name") == "jv,YES;tj,NO");
        CHECK(error_of(ex, "DELETE FROM jv WHERE id = 99").find("ERROR 1395") == 0);
        CHECK(error_of(ex, "DELETE FROM tj WHERE id = 99") == "ERROR 1288 (HY000): The target table tj of the DELETE is not updatable");
        CHECK(error_of(ex, "UPDATE tj SET qty = 5 WHERE id = 1").find("ERROR 1288") == 0);
        CHECK(error_of(ex, "INSERT INTO tj (id, qty) VALUES (9, 9)").find("ERROR 1471") == 0);
        ok(ex, "UPDATE jv SET qty = 12 WHERE id = 1"); // (the one that is not TEMPTABLE takes the UPDATE: one table's column)
        // DROP DATABASE forgets what was kept about its views
        ok(ex, "CREATE DATABASE w2");
        ok(ex, "USE w2");
        ok(ex, "CREATE TABLE t (id INT)");
        ok(ex, "CREATE VIEW wv AS SELECT id FROM t");
        { auto kept = ex.get_shared()->read(); CHECK(kept->routine_sources.count("view:w2.wv") == 1); }
        ok(ex, "DROP DATABASE w2");
        { auto kept = ex.get_shared()->read(); CHECK(kept->routine_sources.count("view:w2.wv") == 0); }
        ok(ex, "USE v");
        // making a view again is a new view: its own account, algorithm and time; dropping it forgets them
        ok(ex, "DROP VIEW v3");
        ok(ex, "CREATE VIEW v3 AS SELECT id, k FROM ua");
        CHECK(created_view(ex, "v3").find(head + "UNDEFINED") == 0);
        CHECK(flat(ex, "SELECT is_updatable FROM information_schema.views WHERE table_name = 'v3'") == "YES");
        { // (nothing of a dropped view stays in what is kept)
            ok(ex, "DROP VIEW v8");
            auto kept = ex.get_shared()->read();
            CHECK(kept->routine_sources.count("view:v.v8") == 0);
            CHECK(kept->routine_sources.count("view:v.v7") == 1);
        }
    }
    { // a server that starts again tells the same
        Executor ex(dir.path);
        ok(ex, "USE v");
        CHECK(created_view(ex, "v2").find("DEFINER=`bob`@`%`") != std::string::npos);
        CHECK(created_view(ex, "v4").find("CREATE ALGORITHM=MERGE") == 0);
        CHECK(std::regex_match(flat(ex, "SELECT create_time FROM information_schema.tables WHERE table_name = 'ua'"), when));
        CHECK(std::regex_match(flat(ex, "SELECT create_time FROM information_schema.tables WHERE table_name = 'v7'"), when));
        CHECK(flat(ex, "SELECT security_type FROM information_schema.views WHERE table_name = 'v7'") == "INVOKER");
        { // (a dropped database took the records of its views with it, also from the file)
            auto kept = ex.get_shared()->read();
            CHECK(kept->routine_sources.count("view:w2.wv") == 0);
            CHECK(kept->routine_sources.count("view:v.v7") == 1);
        }
    }
}

TEST_CASE("R85: a table or a view made before its time and account were kept tells MySQL's defaults", "[r85]") {
    World w("test_r85_legacy_data");
    Executor& ex = w.ex;
    ok(ex, "CREATE VIEW vl AS SELECT id, k FROM ua");
    { // (what a server of R84 left: no record of the view, no time on the table)
        auto kept = ex.get_shared()->write();
        kept->routine_sources.erase("view:v.vl");
        kept->catalog.get_table_mut("v.ua")->created.clear();
    }
    CHECK(created_view(ex, "vl").find("CREATE ALGORITHM=UNDEFINED DEFINER=`root`@`localhost` SQL SECURITY DEFINER VIEW `vl` AS ") == 0);
    CHECK(flat(ex, "SELECT definer, security_type FROM information_schema.views WHERE table_name = 'vl'") == "root@localhost,DEFINER");
    CHECK(flat(ex, "SELECT table_name, create_time IS NULL FROM information_schema.tables WHERE table_schema = 'v' ORDER BY table_name") == "ua,1;vl,1");
    CHECK(flat(ex, "SELECT is_updatable FROM information_schema.views WHERE table_name = 'vl'") == "YES");
    ok(ex, "UPDATE vl SET k = 11 WHERE id = 1");
    // a table made again by CREATE TABLE ... LIKE / AS SELECT has the time of that statement
    ok(ex, "CREATE TABLE ub LIKE ua");
    ok(ex, "CREATE TABLE uc AS SELECT id FROM ua");
    CHECK(flat(ex, "SELECT table_name, create_time IS NULL FROM information_schema.tables WHERE table_schema = 'v' AND table_type = 'BASE TABLE' ORDER BY table_name") == "ua,1;ub,0;uc,0");
}

TEST_CASE("R85: a view over a derived table says IS_UPDATABLE YES, as one with a subquery in its select list does, and takes no write", "[r85]") {
    World w("test_r85_derived_data");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE ub (id INT PRIMARY KEY, ua_id INT, qty INT)");
    for (const char* view : {"d1 AS SELECT d.id FROM (SELECT id FROM ua) d", "d2 AS SELECT d.k FROM (SELECT k FROM ua GROUP BY k) d", "d3 AS SELECT d.id FROM (SELECT DISTINCT id FROM ua) d",
                             "d4 AS SELECT d.id, d.k FROM (SELECT id, k FROM ua LIMIT 1) d", "d5 AS SELECT a.id, d.k FROM ua a JOIN (SELECT id, k FROM ua) d ON a.id = d.id",
                             "d6 AS SELECT d.id FROM (SELECT id FROM ua UNION SELECT id FROM ub) d", "d7 AS SELECT d.id FROM (SELECT id, COUNT(*) AS n FROM ua GROUP BY id) d",
                             "d8 AS SELECT id, (SELECT 1) AS one FROM ua GROUP BY id", "d9 AS SELECT d.id, COUNT(*) AS n FROM (SELECT id FROM ua) d GROUP BY d.id"}) {
        ok(ex, std::string("CREATE VIEW ") + view);
    }
    ok(ex, "CREATE ALGORITHM = MERGE VIEW m1 AS SELECT id, k FROM ua GROUP BY id, k");
    ok(ex, "CREATE ALGORITHM = TEMPTABLE VIEW t3 AS SELECT id, (SELECT 1) AS one FROM ua");
    CHECK(flat(ex, "SELECT table_name, is_updatable FROM information_schema.views ORDER BY table_name") == "d1,YES;d2,YES;d3,YES;d4,YES;d5,YES;d6,YES;d7,YES;d8,NO;d9,NO;m1,NO;t3,NO");
    // the flag says YES, the write says no (1288 / 1471), as for a view with a subquery in its select list
    CHECK(error_of(ex, "UPDATE d1 SET id = 5 WHERE id = 99") == "ERROR 1288 (HY000): The target table d1 of the UPDATE is not updatable");
    CHECK(error_of(ex, "DELETE FROM d1 WHERE id = 99").find("ERROR 1288") == 0);
    CHECK(error_of(ex, "INSERT INTO d1 (id) VALUES (50)").find("ERROR 1471") == 0);
    // a derived table in a join: an inner join says YES (and its DELETE is the 1395 of a join view), an outer join NO (1288)
    ok(ex, "CREATE VIEW dj1 AS SELECT a.id, d.k FROM ua a LEFT JOIN (SELECT id, k FROM ua) d ON a.id = d.id");
    ok(ex, "CREATE VIEW dj3 AS SELECT a.id, d.k FROM ua a JOIN (SELECT id, k FROM ua) d ON a.id = d.id");
    CHECK(flat(ex, "SELECT table_name, is_updatable FROM information_schema.views WHERE table_name IN ('dj1', 'dj3') ORDER BY table_name") == "dj1,NO;dj3,YES");
    CHECK(error_of(ex, "DELETE FROM dj3 WHERE id = 99").find("ERROR 1395") == 0);
    CHECK(error_of(ex, "DELETE FROM d5 WHERE id = 99").find("ERROR 1395") == 0);
    CHECK(error_of(ex, "UPDATE dj3 SET k = 5 WHERE id = 99").find("ERROR 1288") == 0);
    CHECK(error_of(ex, "INSERT INTO dj3 (id) VALUES (9)").find("ERROR 1471") == 0);
    ok(ex, "CREATE ALGORITHM = TEMPTABLE VIEW tdj AS SELECT a.id, d.k FROM ua a JOIN (SELECT id, k FROM ua) d ON a.id = d.id");
    CHECK(flat(ex, "SELECT is_updatable FROM information_schema.views WHERE table_name = 'tdj'") == "NO");
    CHECK(error_of(ex, "DELETE FROM tdj WHERE id = 99").find("ERROR 1288") == 0); // (TEMPTABLE: not the 1395 of a join view)
    CHECK(error_of(ex, "DELETE FROM dj1 WHERE id = 99").find("ERROR 1288") == 0);
    CHECK(error_of(ex, "INSERT INTO dj1 (id) VALUES (9)").find("ERROR 1471") == 0);
}

TEST_CASE("R85: a write that reads a column of a view that is an expression finds its rows (MySQL puts the expression in its place)", "[r85][views]") {
    World w("test_r85_expr_data");
    Executor& ex = w.ex;
    ok(ex, "CREATE VIEW vx AS SELECT id, k + 1 AS k1, name FROM ua");
    ok(ex, "CREATE VIEW vx2 AS SELECT id, k * 2 + id AS m FROM ua");
    ok(ex, "CREATE VIEW vxx AS SELECT id, k1 FROM vx"); // (a view over a view that takes the expression as it is)
    ok(ex, "CREATE VIEW vf AS SELECT id, UPPER(name) AS up FROM ua"); // (a function of a column is an expression too)
    ok(ex, "CREATE VIEW vc AS SELECT id, CASE WHEN k > 5 THEN 'big' ELSE 'small' END AS size FROM ua"); // (a CASE too)
    // WHERE
    ok(ex, "UPDATE vx SET name = 'x' WHERE k1 = 11");
    CHECK(flat(ex, "SELECT id, name FROM ua ORDER BY id") == "1,x;2,b;3,c");
    ok(ex, "UPDATE vx SET name = 'y' WHERE k1 > 11 AND k1 < 31"); // (a range over the expression)
    CHECK(flat(ex, "SELECT id, name FROM ua ORDER BY id") == "1,x;2,y;3,c");
    ok(ex, "UPDATE vx SET name = 'z' WHERE id = k1 - 10"); // (the other side of the comparison is the expression)
    CHECK(flat(ex, "SELECT id, name FROM ua ORDER BY id") == "1,z;2,y;3,c");
    // the new value reads it
    ok(ex, "UPDATE vx SET id = k1 WHERE id = 3");
    CHECK(flat(ex, "SELECT id, k FROM ua ORDER BY id") == "1,10;2,20;31,30");
    // an expression of two columns, in a DELETE
    ok(ex, "DELETE FROM vx2 WHERE m = 21"); // (k * 2 + id = 10 * 2 + 1)
    CHECK(flat(ex, "SELECT id, k FROM ua ORDER BY id") == "2,20;31,30");
    // through a view over the view
    ok(ex, "UPDATE vxx SET id = 40 WHERE k1 = 21");
    CHECK(flat(ex, "SELECT id, k FROM ua ORDER BY id") == "31,30;40,20");
    ok(ex, "DELETE FROM vxx WHERE k1 = 31");
    CHECK(flat(ex, "SELECT id, k FROM ua ORDER BY id") == "40,20");
    // no row matches: nothing is changed, and it is said (0 rows), not refused
    CHECK(ex.execute_sql("DELETE FROM vx WHERE k1 = 999").is_ok());
    CHECK(flat(ex, "SELECT COUNT(*) FROM ua") == "1");
    // a function of a column is put in place too
    ok(ex, "UPDATE vf SET id = 5 WHERE up = 'Y'");
    CHECK(flat(ex, "SELECT id, k FROM ua") == "5,20");
    // ... and a CASE
    ok(ex, "UPDATE vc SET id = 6 WHERE size = 'big'");
    CHECK(flat(ex, "SELECT id, k FROM ua") == "6,20");
    ok(ex, "DELETE FROM vc WHERE size = 'small'"); // (no row is small: nothing goes)
    CHECK(flat(ex, "SELECT COUNT(*) FROM ua") == "1");
    ok(ex, "DELETE FROM vc WHERE size = 'big'");
    CHECK(flat(ex, "SELECT COUNT(*) FROM ua") == "0");
    // the other shapes: the bare name of the column on the right of a comparison, the column in the argument of a function, and a view that renames a column of the table
    ok(ex, "INSERT INTO ua VALUES (2, 20, 'b'), (11, 10, 'e')"); // (the second has id = k + 1)
    ok(ex, "UPDATE vx SET name = 'lit' WHERE id = k1");
    CHECK(flat(ex, "SELECT id, name FROM ua ORDER BY id") == "2,b;11,lit");
    ok(ex, "UPDATE vx SET name = 'abs' WHERE ABS(k1) = 21");
    CHECK(flat(ex, "SELECT id, name FROM ua ORDER BY id") == "2,abs;11,lit");
    ok(ex, "CREATE VIEW vr AS SELECT id AS ident, k + 1 AS k1 FROM ua");
    ok(ex, "UPDATE vr SET ident = 77 WHERE ident = 2 AND k1 = 21");
    CHECK(flat(ex, "SELECT id, k FROM ua ORDER BY id") == "11,10;77,20");
    // the expression column itself is no column to write (1348), and an INSERT that leaves it out is 1471
    CHECK(error_of(ex, "UPDATE vx SET k1 = 5").find("ERROR 1348") == 0);
    CHECK(error_of(ex, "INSERT INTO vx (id) VALUES (70)").find("ERROR 1471") == 0);
}

TEST_CASE("R85: a dropped database takes the records of its views with it, also from the file", "[r85]") {
    TempDataDir dir("test_r85_dropdb_data");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE w2");
        ok(ex, "USE w2");
        ok(ex, "CREATE TABLE t (id INT)");
        ok(ex, "CREATE VIEW wv AS SELECT id FROM t");
        ok(ex, "DROP DATABASE w2"); // (the last thing written: nothing after it writes the file again)
    }
    Executor ex(dir.path);
    auto kept = ex.get_shared()->read();
    CHECK(kept->routine_sources.count("view:w2.wv") == 0);
}
