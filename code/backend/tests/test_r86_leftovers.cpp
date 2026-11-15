// R86: what R85 left. A write through a view over a JOIN that reads a column of the view that is an expression (`ja.x * 2 AS dbl`) finds its rows, ALTER TABLE / CREATE INDEX /
// DROP INDEX / OPTIMIZE TABLE give a table a new creation time (RENAME, TRUNCATE, ANALYZE, a write and a failed statement do not), the native protocol's SHOW CREATE VIEW writes
// ALGORITHM / DEFINER / SQL SECURITY, and the empty system schema `mysql` is listed to a MySQL-protocol session only and is never the schema a session starts in.
// The answers were measured on MySQL 8.4.9; code/test/diff/corpus/join_view_expr_r86.sql, create_time_r86.sql and system_schema_r86.sql repeat them against the server.

#include <filesystem>
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

bool says(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// the rows of a SELECT / SHOW as "a,b;c,d"
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

struct World {
    TempDataDir dir;
    Executor ex;
    explicit World(const std::string& name) : dir(name), ex(dir.path) {
        ok(ex, "CREATE DATABASE v");
        ok(ex, "USE v");
    }
};

void join_tables(Executor& ex) {
    ok(ex, "CREATE TABLE ja (id INT PRIMARY KEY, x INT, g INT)");
    ok(ex, "CREATE TABLE jb (id INT PRIMARY KEY, y INT, aid INT, z INT)");
    ok(ex, "INSERT INTO ja VALUES (1,10,1),(2,20,1),(3,30,2),(4,40,NULL)");
    ok(ex, "INSERT INTO jb VALUES (1,100,1,5),(2,200,1,6),(3,300,2,7),(4,400,9,8)");
    ok(ex, "CREATE VIEW jx1 AS SELECT ja.id AS aid, ja.x * 2 AS dbl, jb.id AS bid, jb.y FROM ja JOIN jb ON jb.aid = ja.id");
    ok(ex, "CREATE VIEW jx2 AS SELECT ja.id AS aid, ja.x + jb.y AS s, jb.id AS bid, jb.y FROM ja JOIN jb ON jb.aid = ja.id");
    ok(ex, "CREATE VIEW jx3 AS SELECT ja.id AS aid, CASE WHEN ja.x > 15 THEN 1 ELSE 0 END AS big, jb.id AS bid, jb.y FROM ja JOIN jb ON jb.aid = ja.id");
    ok(ex, "CREATE VIEW jx4 AS SELECT * FROM jx1 WHERE dbl > 30");
    ok(ex, "CREATE VIEW jx5 AS SELECT aid, dbl * 2 AS q, bid, y FROM jx1");
    ok(ex, "CREATE VIEW jx6 AS SELECT ja.id AS aid, ja.x - jb.z AS dz, jb.id AS bid, jb.z FROM ja, jb WHERE jb.aid = ja.id");
}

const char* const kOld = "2000-01-01 00:00:00";

// the table made "long ago": a statement that makes it again moves the time
void age(Executor& ex, const std::string& table) {
    auto w = ex.shared->write();
    TableSchema* schema = w->catalog.get_table_mut(table.find('.') == std::string::npos ? "v." + table : table);
    REQUIRE(schema != nullptr);
    schema->created = kOld;
}

std::string created_of(Executor& ex, const std::string& table) {
    return flat(ex, "SELECT CREATE_TIME FROM information_schema.tables WHERE table_schema = 'v' AND table_name = '" + table + "'");
}
} // namespace

TEST_CASE("R86: a write through a join view finds its rows by a column that is an expression", "[r86]") {
    World w("r86_data_join_expr");
    Executor& ex = w.ex;
    join_tables(ex);
    // jx1 has the rows (aid, dbl, bid) (1, 20, 1) (1, 20, 2) (2, 40, 3); the new value or the WHERE reads dbl = ja.x * 2
    ok(ex, "UPDATE jx1 SET y = 1 WHERE dbl = 40");
    CHECK(flat(ex, "SELECT y FROM jb ORDER BY id") == "100;200;1;400");
    ok(ex, "UPDATE jx1 SET y = dbl WHERE bid = 1"); // (the new value reads it)
    CHECK(flat(ex, "SELECT y FROM jb ORDER BY id") == "20;200;1;400");
    ok(ex, "UPDATE jx1 SET y = y + 1 WHERE dbl > 20 AND bid > 1");
    CHECK(flat(ex, "SELECT y FROM jb ORDER BY id") == "20;200;2;400");
    ok(ex, "UPDATE jx1 SET y = 5 WHERE dbl = aid * 20"); // (both sides of the comparison: a column of the view that is an expression and one that is not)
    CHECK(flat(ex, "SELECT y FROM jb ORDER BY id") == "5;5;5;400");
    ok(ex, "UPDATE jx1 SET y = 3 WHERE NOT dbl > 20");
    CHECK(flat(ex, "SELECT y FROM jb ORDER BY id") == "3;3;5;400");
    // an expression of the columns of both tables
    ok(ex, "UPDATE jx2 SET y = 0 WHERE s = 25"); // (ja 2 x = 20, jb 3 y = 5)
    CHECK(flat(ex, "SELECT y FROM jb ORDER BY id") == "3;3;0;400");
    ok(ex, "UPDATE jx2 SET y = s WHERE bid = 2"); // (10 + 3)
    CHECK(flat(ex, "SELECT y FROM jb ORDER BY id") == "3;13;0;400");
    // a CASE
    ok(ex, "UPDATE jx3 SET y = 7 WHERE big = 1");
    CHECK(flat(ex, "SELECT y FROM jb ORDER BY id") == "3;13;7;400");
    ok(ex, "UPDATE jx3 SET y = big + 40 WHERE bid = 1");
    CHECK(flat(ex, "SELECT y FROM jb ORDER BY id") == "40;13;7;400");
    // a view over the view: the expression of the one below, read in this view's own WHERE, and an expression made of it
    ok(ex, "UPDATE jx4 SET y = 9 WHERE bid = 3");
    ok(ex, "UPDATE jx4 SET y = 10 WHERE bid = 1"); // (jb 1 is paired with dbl = 20: not a row of jx4)
    CHECK(flat(ex, "SELECT y FROM jb ORDER BY id") == "40;13;9;400");
    ok(ex, "UPDATE jx4 SET y = 6 WHERE dbl = 60"); // (no row)
    CHECK(flat(ex, "SELECT y FROM jb ORDER BY id") == "40;13;9;400");
    ok(ex, "UPDATE jx5 SET y = 11 WHERE q = 80");
    CHECK(flat(ex, "SELECT y FROM jb ORDER BY id") == "40;13;11;400");
    // a comma join
    ok(ex, "UPDATE jx6 SET z = 99 WHERE dz = 5");
    CHECK(flat(ex, "SELECT z FROM jb ORDER BY id") == "99;6;7;8");
    ok(ex, "UPDATE jx6 SET z = dz WHERE bid = 3"); // (20 - 7)
    CHECK(flat(ex, "SELECT z FROM jb ORDER BY id") == "99;6;13;8");
    // the rest is as before: the column that is an expression is not written, a write that reaches two tables is refused, nothing was changed by a refusal
    CHECK(says(error_of(ex, "UPDATE jx1 SET dbl = 1"), "ERROR 1348"));
    CHECK(says(error_of(ex, "UPDATE jx1 SET dbl = 1 WHERE dbl = 40"), "ERROR 1348"));
    CHECK(says(error_of(ex, "UPDATE jx1 SET y = 1, aid = 7 WHERE dbl = 40"), "ERROR 1393"));
    CHECK(says(error_of(ex, "DELETE FROM jx1 WHERE dbl = 40"), "ERROR 1395"));
    CHECK(says(error_of(ex, "INSERT INTO jx1 (aid) VALUES (9)"), "ERROR 1471"));
    CHECK(flat(ex, "SELECT y FROM jb ORDER BY id") == "40;13;11;400");
    CHECK(flat(ex, "SELECT x FROM ja ORDER BY id") == "10;20;30;40");
}

TEST_CASE("R86: CREATE_TIME moves when a statement makes the table again, and not otherwise", "[r86]") {
    World w("r86_data_create_time");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE t (a INT PRIMARY KEY, b INT)");
    CHECK(created_of(ex, "t") != "NULL");
    CHECK(created_of(ex, "t").size() == 19);

    struct Step {
        std::string sql;
        bool moves; // the creation time moves
        bool fails; // the statement is refused
    };
    const std::vector<Step> steps = {
        {"ALTER TABLE t ADD COLUMN c INT", true, false},
        {"CREATE INDEX ic ON t (b)", true, false},
        {"DROP INDEX ic ON t", true, false},
        {"OPTIMIZE TABLE t", true, false},
        {"ALTER TABLE t DROP COLUMN c", true, false},
        {"ALTER TABLE t ADD INDEX id1 (b)", true, false},
        {"CREATE INDEX id2 ON t (b)", true, false},
        {"DROP INDEX id2", true, false}, // (without ON: the table the index is on)
        {"ANALYZE TABLE t", false, false},
        {"TRUNCATE TABLE t", false, false},
        {"INSERT INTO t VALUES (1, 1)", false, false},
        {"UPDATE t SET b = 2", false, false},
        {"DELETE FROM t", false, false},
        {"DROP INDEX nosuch ON t", false, true}, // (1091: nothing was dropped)
        {"DROP INDEX IF EXISTS nosuch ON t", false, false}, // (RuSQL's own IF EXISTS: skipped)
        {"CREATE INDEX id1 ON t (b)", false, true}, // (the name is taken)
        {"ALTER TABLE t DROP COLUMN nosuch", false, true},
        {"CREATE INDEX ie ON t (nosuch)", false, true}, // (1072)
        {"ALTER TABLE t RENAME TO t2", false, false}, // (a rename alone keeps it)
    };
    std::string table = "t";
    for (const Step& step : steps) {
        age(ex, table);
        auto r = ex.execute_sql(step.sql);
        INFO(step.sql << " -> " << (r.is_ok() ? r.value() : r.error()));
        CHECK(r.is_err() == step.fails);
        if (step.sql == "ALTER TABLE t RENAME TO t2") table = "t2";
        CHECK((created_of(ex, table) == kOld) == !step.moves);
    }
    // a rename with another action in one statement makes the table again, under the new name
    age(ex, "t2");
    ok(ex, "ALTER TABLE t2 RENAME TO t3, ADD COLUMN c INT");
    CHECK(created_of(ex, "t3") != kOld);
    CHECK(created_of(ex, "t3").size() == 19);
    // RENAME TABLE is a rename alone too
    age(ex, "t3");
    ok(ex, "RENAME TABLE t3 TO t4");
    CHECK(created_of(ex, "t4") == kOld);
    // two renames in one statement are still no more than a rename
    age(ex, "t4");
    ok(ex, "ALTER TABLE t4 RENAME TO t5, RENAME TO t6");
    CHECK(created_of(ex, "t6") == kOld);
}

TEST_CASE("R86: the moved creation time is kept over a restart", "[r86]") {
    TempDataDir dir("r86_data_create_time_restart");
    std::string moved;
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE v");
        ok(ex, "USE v");
        ok(ex, "CREATE TABLE t (a INT PRIMARY KEY, b INT)");
        age(ex, "v.t");
        ok(ex, "ALTER TABLE t ADD COLUMN c INT");
        moved = created_of(ex, "t");
        CHECK(moved != kOld);
        age(ex, "v.t");
        ok(ex, "CREATE INDEX ib ON t (b)");
        CHECK(created_of(ex, "t") != kOld);
        age(ex, "v.t");
        ok(ex, "OPTIMIZE TABLE t");
        CHECK(created_of(ex, "t") != kOld);
        age(ex, "v.t");
        ok(ex, "ALTER TABLE t DROP COLUMN c"); // (the last statement that touched the file: the restart reads what it wrote)
        moved = created_of(ex, "t");
        CHECK(moved != kOld);
    }
    Executor again(dir.path);
    ok(again, "USE v");
    CHECK(created_of(again, "t") == moved);
    CHECK(moved.size() == 19);
}

TEST_CASE("R86: SHOW CREATE VIEW of the native protocol writes ALGORITHM, DEFINER and SQL SECURITY", "[r86]") {
    World w("r86_data_native_view");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE ua (id INT PRIMARY KEY, k INT)");
    ok(ex, "CREATE VIEW v1 AS SELECT id FROM ua");
    ok(ex, "CREATE ALGORITHM = TEMPTABLE DEFINER = `bob`@`%` SQL SECURITY INVOKER VIEW v2 AS SELECT id, k FROM ua");
    ok(ex, "CREATE ALGORITHM = MERGE VIEW v3 AS SELECT id FROM ua WHERE k > 1 WITH CHECK OPTION");
    const auto text = [&](const std::string& view) {
        auto r = ex.execute_sql("SHOW CREATE VIEW " + view);
        INFO(view << " -> " << (r.is_ok() ? r.value() : r.error()));
        REQUIRE(r.is_ok());
        return r.value();
    };
    CHECK(says(text("v1"), "View: v1\nCreate View: CREATE ALGORITHM=UNDEFINED DEFINER=`root`@`localhost` SQL SECURITY DEFINER VIEW `v1` AS "));
    CHECK(says(text("v2"), "Create View: CREATE ALGORITHM=TEMPTABLE DEFINER=`bob`@`%` SQL SECURITY INVOKER VIEW `v2` AS "));
    CHECK(says(text("v3"), "Create View: CREATE ALGORITHM=MERGE DEFINER=`root`@`localhost` SQL SECURITY DEFINER VIEW `v3` AS "));
    CHECK(says(text("v3"), "CHECK OPTION"));
    // the text is the one the MySQL protocol shows
    for (const char* view : {"v1", "v2", "v3"}) {
        auto mysql = ex.show_create_view_mysql(view);
        REQUIRE(mysql.is_ok());
        CHECK(says(text(view), mysql.value().substr(0, mysql.value().find(" AS ") + 4)));
    }
}

TEST_CASE("R86: the empty system schema mysql is listed to a MySQL-protocol session only", "[r86]") {
    TempDataDir dir("r86_data_system_schema");
    Executor ex(dir.path);
    CHECK(ex.current_db != "mysql"); // (nothing but the system schema: the session starts in no schema of it)
    CHECK(flat(ex, "SHOW DATABASES") == "");
    ok(ex, "CREATE DATABASE beta");
    ok(ex, "CREATE DATABASE alpha");
    CHECK(flat(ex, "SHOW DATABASES") == "alpha;beta");
    CHECK(flat(ex, "SELECT schema_name FROM information_schema.schemata WHERE schema_name <> 'information_schema' ORDER BY schema_name") == "alpha;beta");
    // a session that comes after starts in the first schema of the user's, never in `mysql`
    {
        Executor second = Executor::new_session(ex.shared);
        CHECK(second.current_db == "alpha");
    }
    // the session of the MySQL protocol lists it
    ex.lists_system_schema = true;
    CHECK(flat(ex, "SHOW DATABASES") == "alpha;beta;mysql");
    CHECK(flat(ex, "SELECT schema_name FROM information_schema.schemata WHERE schema_name <> 'information_schema' ORDER BY schema_name") == "alpha;beta;mysql");
    ex.lists_system_schema = false;
    // it cannot be made or dropped, listed or not
    CHECK(says(error_of(ex, "CREATE DATABASE mysql"), "ERROR 3552"));
    CHECK(says(error_of(ex, "DROP DATABASE mysql"), "ERROR 3552"));
    // dropping the schema the session is in leaves it in another one of the user's
    ok(ex, "USE beta");
    ok(ex, "DROP DATABASE beta");
    CHECK(ex.current_db == "alpha");
    ok(ex, "DROP DATABASE alpha");
    CHECK(ex.current_db != "mysql");
    CHECK(flat(ex, "SHOW DATABASES") == "");
}

TEST_CASE("R86: a session starts in a schema of the user's even when its name sorts after mysql", "[r86]") {
    TempDataDir dir("r86_data_system_schema_late");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE zed");
    Executor second = Executor::new_session(ex.shared);
    CHECK(second.current_db == "zed");
    Executor again(dir.path); // (the data directory read again at the start of a server)
    CHECK(again.current_db == "zed");
}

TEST_CASE("R86: a write through a view reaches every kind of expression of its WHERE and its columns", "[r86]") {
    World w("r86_data_walker");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE ua (id INT PRIMARY KEY, k INT, name VARCHAR(10))");
    ok(ex, "INSERT INTO ua VALUES (1, 10, 'a'), (2, 20, 'b'), (3, 30, 'c'), (4, 40, 'k1')");
    ok(ex, "CREATE VIEW vr AS SELECT id AS ident, k AS kk, name AS nm FROM ua"); // (the names of the columns are not the table's)
    ok(ex, "CREATE VIEW vx AS SELECT ident, kk + 1 AS k1, nm FROM vr");
    ok(ex, "CREATE VIEW vs AS SELECT ident, kk - ident AS d, kk * ident AS m, nm FROM vr");
    const auto names = [&]() { return flat(ex, "SELECT name FROM ua ORDER BY id"); };
    ok(ex, "UPDATE vr SET nm = 'f' WHERE ABS(kk) = 20"); // (the argument of a function)
    CHECK(names() == "a;f;c;k1");
    ok(ex, "UPDATE vr SET nm = 'g' WHERE kk = ident * 10 AND ident > 2"); // (the right side of a comparison is an expression)
    CHECK(names() == "a;f;g;g");
    ok(ex, "UPDATE vs SET nm = 's' WHERE d = 18"); // (kk - ident: both sides are the table's columns)
    CHECK(names() == "a;s;g;g");
    ok(ex, "UPDATE vs SET nm = 'm' WHERE m = 90");
    CHECK(names() == "a;s;m;g");
    ok(ex, "UPDATE vx SET nm = 'rhs' WHERE ident + 10 = k1"); // (the right side is the column that is an expression, as it stands)
    CHECK(names() == "rhs;s;m;g");
    ok(ex, "UPDATE vx SET nm = 'hit' WHERE nm = 'rhs'"); // (a text that is the name of a column is a text)
    CHECK(names() == "hit;s;m;g");
    ok(ex, "INSERT INTO ua VALUES (5, 50, 'k1')");
    ok(ex, "UPDATE vx SET nm = 'quoted' WHERE nm = 'k1'"); // ('k1' is also the name of a column of the view)
    CHECK(names() == "hit;s;m;g;quoted");
}

TEST_CASE("R86: a schema named mysql that a user made before R84 is theirs: listed, dropped, and a session may start in it", "[r86]") {
    TempDataDir dir("r86_data_user_mysql");
    fs::create_directories(fs::path(dir.path) / "mysql"); // (the folder of a schema a user made when the name was free)
    Executor ex(dir.path);
    CHECK(flat(ex, "SHOW DATABASES") == "mysql");
    CHECK(ex.current_db == "mysql");
    ok(ex, "CREATE DATABASE zed");
    CHECK(flat(ex, "SHOW DATABASES") == "mysql;zed");
    ok(ex, "DROP DATABASE mysql"); // (it has a folder: not the system schema)
    CHECK(flat(ex, "SHOW DATABASES") == "zed");
}

TEST_CASE("R86: dropping a column takes it out of the indexes that read it, and an index with no column left goes", "[r86]") {
    World w("r86_data_drop_column_index");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE x (id INT PRIMARY KEY, a INT, b INT, c INT)");
    ok(ex, "INSERT INTO x VALUES (1, 1, 10, 100), (2, 2, 20, 200)");
    ok(ex, "CREATE INDEX ia ON x (a)");
    ok(ex, "CREATE INDEX ib ON x (b)");
    ok(ex, "CREATE INDEX iab ON x (a, b)");
    ok(ex, "CREATE INDEX ibc ON x (b, c)");
    ok(ex, "CREATE INDEX ihb USING HASH ON x (b)");
    const std::string indexes = "SELECT index_name, column_name, seq_in_index FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'x' ORDER BY index_name, seq_in_index";
    CHECK(flat(ex, indexes) == "ia,a,1;iab,a,1;iab,b,2;ib,b,1;ibc,b,1;ibc,c,2;ihb,b,1;PRIMARY,id,1");
    ok(ex, "ALTER TABLE x DROP COLUMN b");
    CHECK(flat(ex, indexes) == "ia,a,1;iab,a,1;ibc,c,1;PRIMARY,id,1"); // (ib and ihb had only b; iab and ibc lose it)
    CHECK(flat(ex, "SELECT id FROM x WHERE a = 2") == "2");
    CHECK(flat(ex, "SELECT id FROM x WHERE c = 200") == "2");
    ok(ex, "CREATE TABLE y LIKE x"); // (copies the indexes it has left: nothing reads a column that is gone)
    CHECK(flat(ex, "SELECT index_name, column_name FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = 'y' ORDER BY index_name, seq_in_index") == "ia,a;iab,a;ibc,c;PRIMARY,id");
    ok(ex, "ALTER TABLE x DROP COLUMN a");
    CHECK(flat(ex, indexes) == "ibc,c,1;PRIMARY,id,1");
    CHECK(flat(ex, "SELECT id FROM x WHERE c = 100") == "1");
    CHECK(says(error_of(ex, "CREATE INDEX izz ON x (zz)"), "ERROR 1072"));
    CHECK(says(error_of(ex, "CREATE INDEX izz ON x (c, zz)"), "ERROR 1072"));
    CHECK(says(error_of(ex, "DROP INDEX ia ON x"), "ERROR 1091"));
}
