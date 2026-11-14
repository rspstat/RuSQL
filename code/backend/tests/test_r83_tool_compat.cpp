// R83: what mysqldump, mysqlshow, the tools that read a schema (DBeaver, Workbench, ORMs) and hand-written SQL send (RECOMMAND.md, the "R81" rows): a string as the name of a column
// (`AS 'Total'`), the words that only advise the optimizer (SQL_NO_CACHE, HIGH_PRIORITY, STRAIGHT_JOIN ...), LOCK TABLES ... READ LOCAL, SHOW CREATE DATABASE / PROCEDURE / FUNCTION /
// TRIGGER with the text the routine was written as, and information_schema.routines / parameters / triggers / check_constraints with the UNIQUE keys in key_column_usage.
// The answers were measured on MySQL 8.4; code/test/diff/corpus/tool_compat_r83.sql, tool_check_r83.py and dump_to_mysql.py repeat them against the server.

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

// an answer as a table: the header and the rows, every cell trimmed
struct Answer {
    std::vector<std::string> header;
    std::vector<std::vector<std::string>> rows;
};

Answer answer_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    Answer out;
    std::istringstream in(r.value());
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        std::vector<std::string> cells;
        std::size_t from = 1;
        while (from < line.size()) {
            std::size_t end = line.find('|', from);
            if (end == std::string::npos) break;
            std::string cell = line.substr(from, end - from);
            const std::size_t a = cell.find_first_not_of(' '), b = cell.find_last_not_of(' ');
            cells.push_back(a == std::string::npos ? std::string() : cell.substr(a, b - a + 1));
            from = end + 1;
        }
        if (++bars == 1) out.header = cells;
        else out.rows.push_back(cells);
    }
    return out;
}

// the rows of an answer as "a,b;c,d"
std::string flat(Executor& ex, const std::string& sql) {
    std::string out;
    for (const auto& row : answer_of(ex, sql).rows) {
        std::string line;
        for (const auto& cell : row) line += (line.empty() ? "" : ",") + cell;
        out += (out.empty() ? "" : ";") + line;
    }
    return out;
}

std::string header(Executor& ex, const std::string& sql) {
    std::string out;
    for (const auto& cell : answer_of(ex, sql).header) out += (out.empty() ? "" : ",") + cell;
    return out;
}

std::string created(Executor& ex, const char* kind, const std::string& name) {
    auto r = ex.show_create_routine_mysql(kind, name);
    INFO(kind << " " << name << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    return r.value();
}
} // namespace

TEST_CASE("R83: a string names a column, two strings side by side are one", "[r83]") {
    TempDataDir dir("test_r83_alias_data");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE par (id INT PRIMARY KEY, name VARCHAR(20))");
    ok(ex, "INSERT INTO par VALUES (1,'a'),(2,'b')");

    CHECK(header(ex, "SELECT id AS 'a b' FROM par") == "a b");
    CHECK(header(ex, "SELECT id 'a2' FROM par") == "a2");
    CHECK(header(ex, "SELECT 1 AS 'x', 2 'y z', name AS \"dq\" FROM par LIMIT 1") == "x,y z,dq");
    CHECK(header(ex, "SELECT id AS 'it''s' FROM par") == "it's");
    CHECK(header(ex, "SELECT COUNT(*) AS 'how many' FROM par") == "how many");
    CHECK(header(ex, "SELECT id, id + 1 'next' FROM par") == "id,next");
    // a word that is a keyword names the column as it was typed, not in lower case
    CHECK(header(ex, "SELECT 1 AS Count, 2 AS Privileges, 3 AS Date, 4 AS MAX") == "Count,Privileges,Date,MAX");
    CHECK(flat(ex, "SELECT id AS 'a b' FROM par ORDER BY id") == "1;2");
    // two string literals side by side are one string (MySQL), also in a condition and a list of values
    CHECK(flat(ex, "SELECT 'a' 'b'") == "ab");
    CHECK(flat(ex, "SELECT 'a' 'b' 'c' AS 'abc'") == "abc");
    CHECK(header(ex, "SELECT 'a' 'b' 'c' AS 'abc'") == "abc");
    CHECK(flat(ex, "SELECT id FROM par WHERE name = 'a' 'b' OR name = 'b'") == "2");
    ok(ex, "INSERT INTO par VALUES (3, 'x' 'y')");
    CHECK(flat(ex, "SELECT name FROM par WHERE id = 3") == "xy");
    // a literal that did not read (X'4G', X'4') is a syntax error, not a column X with the name 4G
    CHECK(error_of(ex, "SELECT X'4G'").find("Unknown column") == std::string::npos);
    CHECK(error_of(ex, "SELECT X'4'").find("Unknown column") == std::string::npos);
    CHECK(error_of(ex, "SELECT X '4'").find("Unknown column") != std::string::npos); // (a space makes it the column X)
    // a hex literal is a value of its own: it neither joins a string nor names a column
    CHECK_FALSE(ex.execute_sql("SELECT 'a' X'62'").is_ok());
    CHECK_FALSE(ex.execute_sql("SELECT 1 AS X'62'").is_ok());
    // two strings after AS are not one name (MySQL joins them in the grammar of a value, not in a name)
    CHECK_FALSE(ex.execute_sql("SELECT id AS 'a' 'b' FROM par").is_ok());
    CHECK_FALSE(ex.execute_sql("SELECT id 'a' 'b' FROM par").is_ok());
    // the name of a table is no string: `FROM par AS 'p'` stays an error
    CHECK(error_of(ex, "SELECT * FROM par AS 'p'").find("alias") != std::string::npos);
}

TEST_CASE("R83: the words that advise the optimizer are read and left alone", "[r83]") {
    TempDataDir dir("test_r83_modifiers_data");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE par (id INT PRIMARY KEY, name VARCHAR(20))");
    ok(ex, "CREATE TABLE chi (id INT PRIMARY KEY, pid INT, qty INT)");
    ok(ex, "INSERT INTO par VALUES (1,'a'),(2,'b'),(3,'c')");
    ok(ex, "INSERT INTO chi VALUES (1,1,5),(2,2,7),(3,2,1)");

    for (const char* word : {"SQL_NO_CACHE", "HIGH_PRIORITY", "SQL_SMALL_RESULT", "SQL_BIG_RESULT", "SQL_BUFFER_RESULT", "STRAIGHT_JOIN", "ALL", "DISTINCTROW"}) {
        INFO(word);
        CHECK(flat(ex, std::string("SELECT ") + word + " id FROM par ORDER BY id") == "1;2;3");
        CHECK(flat(ex, std::string("select ") + word + " id from par order by id") == "1;2;3");
    }
    // in any order, with DISTINCT, in front of * and an aggregate, in a subquery and after a UNION
    CHECK(flat(ex, "SELECT SQL_NO_CACHE HIGH_PRIORITY DISTINCT pid FROM chi ORDER BY pid") == "1;2");
    CHECK(flat(ex, "SELECT DISTINCT SQL_NO_CACHE pid FROM chi ORDER BY pid") == "1;2");
    CHECK(flat(ex, "SELECT DISTINCTROW pid FROM chi ORDER BY pid") == "1;2");
    CHECK(flat(ex, "SELECT ALL pid FROM chi ORDER BY pid") == "1;2;2");
    CHECK(flat(ex, "SELECT SQL_NO_CACHE * FROM par ORDER BY id") == "1,a;2,b;3,c");
    CHECK(flat(ex, "SELECT SQL_NO_CACHE COUNT(*) FROM par") == "3");
    CHECK(flat(ex, "SELECT id FROM par WHERE id IN (SELECT SQL_NO_CACHE pid FROM chi) ORDER BY id") == "1;2");
    CHECK(flat(ex, "SELECT HIGH_PRIORITY id FROM par UNION SELECT SQL_NO_CACHE pid FROM chi ORDER BY 1") == "1;2;3");
    CHECK(flat(ex, "SELECT /*!40001 SQL_NO_CACHE */ * FROM par ORDER BY id") == "1,a;2,b;3,c"); // (what mysqldump writes)
    // STRAIGHT_JOIN is a join too: it reads the left table first and is otherwise an inner join
    CHECK(flat(ex, "SELECT par.id, chi.qty FROM par STRAIGHT_JOIN chi ON chi.pid = par.id ORDER BY par.id, chi.qty") == "1,5;2,1;2,7");
    CHECK(flat(ex, "SELECT STRAIGHT_JOIN par.id, chi.qty FROM par JOIN chi ON chi.pid = par.id ORDER BY par.id, chi.qty") == "1,5;2,1;2,7");
    CHECK(flat(ex, "SELECT par.id FROM par STRAIGHT_JOIN chi ON chi.pid = par.id WHERE chi.qty > 1 ORDER BY par.id") == "1;2");
    // in backticks the word is a name: a column called `sql_no_cache`, a table called `straight_join`
    ok(ex, "CREATE TABLE odd (`sql_no_cache` INT, `high_priority` INT)");
    ok(ex, "INSERT INTO odd VALUES (7, 8)");
    CHECK(flat(ex, "SELECT `sql_no_cache`, `high_priority` FROM odd") == "7,8");
    CHECK(flat(ex, "SELECT SQL_NO_CACHE `sql_no_cache` FROM odd") == "7");
    // a word that is only like one is a column
    CHECK(error_of(ex, "SELECT SQL_NO_CACHE_X id FROM par").find("Unknown column") != std::string::npos);
}

TEST_CASE("R83: LOCK TABLES takes READ LOCAL", "[r83]") {
    TempDataDir dir("test_r83_lock_data");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE par (id INT PRIMARY KEY)");
    ok(ex, "CREATE TABLE chi (id INT PRIMARY KEY)");
    ok(ex, "LOCK TABLES `par` READ /*!32311 LOCAL */");
    ok(ex, "UNLOCK TABLES");
    ok(ex, "LOCK TABLES par READ LOCAL, chi WRITE");
    ok(ex, "UNLOCK TABLES");
    CHECK_FALSE(ex.execute_sql("LOCK TABLES par LOW_PRIORITY WRITE").is_ok()); // (MySQL 8.4 no longer takes it: error 1064)
    CHECK_FALSE(ex.execute_sql("LOCK TABLES par LOW_PRIORITY READ").is_ok());
    CHECK_FALSE(ex.execute_sql("LOCK TABLES par LOCAL").is_ok());
}

TEST_CASE("R83: SHOW CREATE PROCEDURE / FUNCTION / TRIGGER give the text the routine was written as", "[r83]") {
    TempDataDir dir("test_r83_routines_data");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE v");
        ok(ex, "USE v");
        ok(ex, "CREATE TABLE par (id INT PRIMARY KEY, name VARCHAR(20) NOT NULL)");
        ok(ex, "CREATE PROCEDURE sp_one(IN x INT, OUT y INT) SET y = x + 1");
        ok(ex, "CREATE FUNCTION fn_two(x INT) RETURNS INT DETERMINISTIC RETURN x * 2");
        ok(ex, "CREATE FUNCTION fn_three(s VARCHAR(20), d DECIMAL(8,2)) RETURNS VARCHAR(30) CHARSET utf8mb4 COMMENT 'hello' READS SQL DATA BEGIN RETURN CONCAT(s, 'x'); END");
        ok(ex, "CREATE FUNCTION fn_up(a TINYINT UNSIGNED, t TEXT) RETURNS TINYINT UNSIGNED DETERMINISTIC RETURN a");
        ok(ex, "CREATE FUNCTION fn_t(a INT) RETURNS TEXT DETERMINISTIC RETURN 'x'");
        ok(ex, "CREATE FUNCTION fn_dt(a INT) RETURNS DATETIME(3) DETERMINISTIC RETURN NOW()");
        ok(ex, "CREATE PROCEDURE sp_all(IN a INT) COMMENT 'it''s' SQL SECURITY INVOKER NOT DETERMINISTIC MODIFIES SQL DATA LANGUAGE SQL SELECT 1");
        ok(ex, "CREATE PROCEDURE sp_two() NO SQL DETERMINISTIC BEGIN SELECT 1; END");
        ok(ex, "CREATE TRIGGER tr_par BEFORE INSERT ON par FOR EACH ROW SET NEW.name = LOWER(NEW.name)");

        // the text of each, as MySQL 8.4 wrote it for the same statements
        CHECK(created(ex, "procedure", "sp_one") == "CREATE DEFINER=`root`@`localhost` PROCEDURE `sp_one`(IN x INT, OUT y INT)\nSET y = x + 1");
        CHECK(created(ex, "function", "fn_two") == "CREATE DEFINER=`root`@`localhost` FUNCTION `fn_two`(x INT) RETURNS int\n    DETERMINISTIC\nRETURN x * 2");
        CHECK(created(ex, "function", "fn_three") ==
              "CREATE DEFINER=`root`@`localhost` FUNCTION `fn_three`(s VARCHAR(20), d DECIMAL(8,2)) RETURNS varchar(30) CHARSET utf8mb4\n    READS SQL DATA\n    COMMENT 'hello'\nBEGIN RETURN CONCAT(s, 'x'); END");
        CHECK(created(ex, "function", "fn_up") == "CREATE DEFINER=`root`@`localhost` FUNCTION `fn_up`(a TINYINT UNSIGNED, t TEXT) RETURNS tinyint unsigned\n    DETERMINISTIC\nRETURN a");
        CHECK(created(ex, "function", "fn_t") == "CREATE DEFINER=`root`@`localhost` FUNCTION `fn_t`(a INT) RETURNS text CHARSET utf8mb4\n    DETERMINISTIC\nRETURN 'x'");
        CHECK(created(ex, "function", "fn_dt") == "CREATE DEFINER=`root`@`localhost` FUNCTION `fn_dt`(a INT) RETURNS datetime(3)\n    DETERMINISTIC\nRETURN NOW()");
        CHECK(created(ex, "procedure", "sp_all") ==
              "CREATE DEFINER=`root`@`localhost` PROCEDURE `sp_all`(IN a INT)\n    MODIFIES SQL DATA\n    SQL SECURITY INVOKER\n    COMMENT 'it''s'\nSELECT 1");
        CHECK(created(ex, "procedure", "sp_two") == "CREATE DEFINER=`root`@`localhost` PROCEDURE `sp_two`()\n    NO SQL\n    DETERMINISTIC\nBEGIN SELECT 1; END");
        CHECK(created(ex, "trigger", "tr_par") ==
              "CREATE DEFINER=`root`@`localhost` TRIGGER `tr_par` BEFORE INSERT ON `par` FOR EACH ROW SET NEW.name = LOWER(NEW.name)");
        // the name does not depend on case, and a routine that is not there is error 1305
        CHECK(created(ex, "procedure", "SP_ONE").find("`sp_one`") != std::string::npos);
        CHECK(ex.show_create_routine_mysql("procedure", "nope").error() == "ERROR 1305 (42000): PROCEDURE nope does not exist");
        CHECK(ex.show_create_routine_mysql("function", "nope").error() == "ERROR 1305 (42000): FUNCTION nope does not exist");
        CHECK(ex.show_create_routine_mysql("trigger", "nope").error() == "ERROR 1360 (HY000): Trigger does not exist");
        // a routine made again has the new text; a dropped one has none
        ok(ex, "DROP FUNCTION fn_two");
        ok(ex, "CREATE FUNCTION fn_two(x INT) RETURNS INT DETERMINISTIC RETURN x * 3");
        CHECK(created(ex, "function", "fn_two").find("RETURN x * 3") != std::string::npos);
        ok(ex, "DROP PROCEDURE sp_two");
        CHECK(ex.show_create_routine_mysql("procedure", "sp_two").is_err());
        ok(ex, "DROP TRIGGER tr_par");
        CHECK(ex.show_create_routine_mysql("trigger", "tr_par").is_err());
        { // (the text of a dropped routine is gone from what is kept, and so from the file)
            auto kept = ex.get_shared()->read();
            CHECK(kept->routine_sources.count("procedure:sp_two") == 0);
            CHECK(kept->routine_sources.count("trigger:tr_par") == 0);
            CHECK(kept->routine_sources.count("procedure:sp_one") == 1);
        }
        ok(ex, "CREATE TRIGGER tr_par BEFORE INSERT ON par FOR EACH ROW SET NEW.name = UPPER(NEW.name)");
    }
    { // the text is kept on disk: a server that starts again gives it back
        Executor ex(dir.path);
        CHECK(created(ex, "procedure", "sp_one") == "CREATE DEFINER=`root`@`localhost` PROCEDURE `sp_one`(IN x INT, OUT y INT)\nSET y = x + 1");
        CHECK(created(ex, "trigger", "tr_par").find("SET NEW.name = UPPER(NEW.name)") != std::string::npos);
        CHECK(ex.show_create_routine_mysql("procedure", "sp_two").is_err());
        CHECK(flat(ex, "SELECT routine_name FROM information_schema.routines WHERE routine_schema = 'v' ORDER BY routine_name") == "fn_dt;fn_t;fn_three;fn_two;fn_up;sp_all;sp_one");
    }
}

TEST_CASE("R83: the text SHOW CREATE writes can be run again and gives the same text", "[r83]") {
    TempDataDir dir("test_r83_again_data");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE v");
    ok(ex, "USE v");
    ok(ex, "CREATE TABLE par (id INT PRIMARY KEY, name VARCHAR(20) NOT NULL)");
    ok(ex, "CREATE PROCEDURE sp_a(IN x INT, INOUT s VARCHAR(10) CHARSET utf8mb4, OUT d DECIMAL(8,2)) NO SQL DETERMINISTIC COMMENT 'it''s' BEGIN SELECT 1; END");
    ok(ex, "CREATE FUNCTION fn_a(s VARCHAR(20), n INT) RETURNS VARCHAR(30) CHARSET utf8mb4 COLLATE utf8mb4_bin COMMENT 'hello' READS SQL DATA BEGIN RETURN CONCAT(s, 'x'); END");
    ok(ex, "CREATE FUNCTION fn_b(a INT) RETURNS DECIMAL(10,2) DETERMINISTIC RETURN a / 3");
    ok(ex, "CREATE FUNCTION fn_c(a VARCHAR(5) CHARACTER SET latin1) RETURNS VARCHAR(5) CHARACTER SET latin1 COLLATE latin1_bin DETERMINISTIC RETURN a");
    ok(ex, "CREATE FUNCTION fn_d(a VARCHAR(5) COLLATE utf8mb4_bin) RETURNS VARCHAR(5) CHARSET latin1 DETERMINISTIC RETURN a");
    ok(ex, "CREATE TRIGGER tr_a BEFORE INSERT ON par FOR EACH ROW BEGIN SET NEW.name = LOWER(NEW.name); END");
    for (const auto& [kind, name] : std::vector<std::pair<const char*, const char*>>{{"procedure", "sp_a"}, {"function", "fn_a"}, {"function", "fn_b"}, {"function", "fn_c"}, {"function", "fn_d"}, {"trigger", "tr_a"}}) {
        INFO(kind << " " << name);
        const std::string before = created(ex, kind, name);
        ok(ex, std::string("DROP ") + (std::string(kind) == "procedure" ? "PROCEDURE" : std::string(kind) == "function" ? "FUNCTION" : "TRIGGER") + " " + name);
        ok(ex, before); // (a dump runs exactly this text, with its DEFINER)
        CHECK(created(ex, kind, name) == before);
    }
    CHECK(flat(ex, "SELECT fn_b(10)") == "3.33");
    ok(ex, "INSERT INTO par VALUES (1, 'ABC')");
    CHECK(flat(ex, "SELECT name FROM par") == "abc");
}

TEST_CASE("R83: mysqldump locks a view, reads the empty tablespace tables and runs with sql_mode = ''", "[r83]") {
    TempDataDir dir("test_r83_dump_data");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE v");
    ok(ex, "USE v");
    ok(ex, "CREATE TABLE par (id INT PRIMARY KEY COMMENT 'the key', name VARCHAR(20))");
    ok(ex, "CREATE TABLE chi (id INT PRIMARY KEY, pid INT, qty INT)");
    ok(ex, "INSERT INTO par VALUES (1,'a'),(2,'b')");
    ok(ex, "INSERT INTO chi VALUES (1,1,5),(2,2,7),(3,2,1)");
    ok(ex, "CREATE VIEW vw AS SELECT id, name FROM par");
    ok(ex, "LOCK TABLES `par` READ /*!32311 LOCAL */, `vw` READ /*!32311 LOCAL */");
    ok(ex, "UNLOCK TABLES");
    CHECK(flat(ex, "SELECT COUNT(*) FROM information_schema.files") == "0");
    CHECK(flat(ex, "SELECT COUNT(*) FROM information_schema.column_statistics") == "0");
    CHECK(header(ex, "SELECT COLUMN_NAME, JSON_EXTRACT(HISTOGRAM, '$.\"number-of-buckets-specified\"') FROM information_schema.COLUMN_STATISTICS WHERE SCHEMA_NAME = 'v' AND TABLE_NAME = 'par'").find("COLUMN_NAME") == 0);
    // a view column has the COMMENT of the column it shows
    CHECK(flat(ex, "SELECT column_comment FROM information_schema.columns WHERE table_schema = 'v' AND table_name = 'vw' AND column_name = 'id'") == "the key");
    // ONLY_FULL_GROUP_BY is on until a session takes it out of its sql_mode
    CHECK(error_of(ex, "SELECT pid, qty FROM chi GROUP BY pid").find("only_full_group_by") != std::string::npos);
    ok(ex, "SET sql_mode = ''");
    CHECK(flat(ex, "SELECT pid, COUNT(*) FROM chi GROUP BY pid ORDER BY pid") == "1,1;2,2");
    CHECK(flat(ex, "SELECT pid, qty FROM chi GROUP BY pid ORDER BY pid") == "1,5;2,7"); // (a row of the group: the first)
    CHECK(flat(ex, "SELECT id, COUNT(*) FROM chi") == "1,3");
    // (an answer that was given without the rule is not given again once the rule is on: the cache of answers knows the mode)
    CHECK(flat(ex, "SELECT pid, qty FROM chi GROUP BY pid ORDER BY pid") == "1,5;2,7");
    ok(ex, "SET sql_mode = 'ONLY_FULL_GROUP_BY'");
    CHECK(error_of(ex, "SELECT pid, qty FROM chi GROUP BY pid").find("only_full_group_by") != std::string::npos);
    CHECK(error_of(ex, "SELECT pid, qty FROM chi GROUP BY pid ORDER BY pid").find("only_full_group_by") != std::string::npos);
    ok(ex, "SET sql_mode = 'ANSI'"); // (ANSI has ONLY_FULL_GROUP_BY in it)
    CHECK(error_of(ex, "SELECT id, COUNT(*) FROM chi").find("only_full_group_by") != std::string::npos);
    ok(ex, "SET sql_mode = 'STRICT_TRANS_TABLES'");
    CHECK(flat(ex, "SELECT id, COUNT(*) FROM chi") == "1,3");
    // no row at all: the column is NULL, and so is an expression of it
    ok(ex, "CREATE TABLE em (id INT, v INT)");
    CHECK(flat(ex, "SELECT id, v + 1, COUNT(*) FROM em") == "NULL,NULL,0");
    ok(ex, "SET sql_mode = 'ONLY_FULL_GROUP_BY'");
    CHECK(error_of(ex, "SELECT id, v + 1, COUNT(*) FROM em").find("only_full_group_by") != std::string::npos);
}

TEST_CASE("R83: the new information_schema tables show an account what it may see", "[r83]") {
    TempDataDir dir("test_r83_privileges_data");
    Executor root(dir.path);
    ok(root, "CREATE DATABASE v");
    ok(root, "CREATE DATABASE w");
    ok(root, "USE v");
    ok(root, "CREATE TABLE t (id INT PRIMARY KEY, CHECK (id > 0))");
    ok(root, "CREATE PROCEDURE sp_v() SELECT 1");
    ok(root, "CREATE TRIGGER tr_v BEFORE INSERT ON t FOR EACH ROW SET NEW.id = NEW.id");
    ok(root, "USE w");
    ok(root, "CREATE TABLE t (id INT PRIMARY KEY, CHECK (id > 0))");
    ok(root, "CREATE PROCEDURE sp_w() SELECT 1");
    ok(root, "CREATE TRIGGER tr_w BEFORE INSERT ON t FOR EACH ROW SET NEW.id = NEW.id");
    ok(root, "USE v");
    ok(root, "CREATE TABLE u (id INT PRIMARY KEY)");
    ok(root, "CREATE TRIGGER tr_u BEFORE INSERT ON u FOR EACH ROW SET NEW.id = NEW.id");
    ok(root, "CREATE USER 'one'@'%' IDENTIFIED BY 'pw'");
    ok(root, "GRANT SELECT ON v.t TO 'one'@'%'");
    ok(root, "CREATE USER 'low'@'%' IDENTIFIED BY 'pw'");
    ok(root, "CREATE USER 'none'@'%' IDENTIFIED BY 'pw'");
    ok(root, "GRANT SELECT ON v.* TO 'low'@'%'");
    auto session = [&](const std::string& user) {
        Executor ex = Executor::new_session(root.get_shared());
        ex.auth_user = user;
        ex.activate_default_roles();
        ex.current_db = "v";
        return ex;
    };
    Executor low = session("low"), none = session("none"), one = session("one");
    CHECK(flat(root, "SELECT routine_schema FROM information_schema.routines ORDER BY routine_schema") == "v;w");
    CHECK(flat(low, "SELECT routine_schema FROM information_schema.routines ORDER BY routine_schema") == "v");
    CHECK(flat(low, "SELECT trigger_schema FROM information_schema.triggers ORDER BY trigger_schema") == "v;v"); // (tr_v and tr_u; nothing of w)
    CHECK(flat(low, "SELECT constraint_schema FROM information_schema.check_constraints ORDER BY constraint_schema") == "v");
    // a grant on one table shows the triggers of that table and not the ones of the table next to it
    CHECK(flat(one, "SELECT trigger_name FROM information_schema.triggers ORDER BY trigger_name") == "tr_v");
    CHECK(flat(low, "SELECT trigger_name FROM information_schema.triggers WHERE trigger_schema = 'v' ORDER BY trigger_name") == "tr_u;tr_v");
    CHECK(flat(none, "SELECT routine_schema FROM information_schema.routines").empty());
    CHECK(flat(none, "SELECT trigger_schema FROM information_schema.triggers").empty());
    CHECK(flat(none, "SELECT constraint_schema FROM information_schema.check_constraints").empty());
}

TEST_CASE("R83: SHOW CREATE DATABASE", "[r83]") {
    TempDataDir dir("test_r83_database_data");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE v");
    auto plain = ex.show_create_database_mysql("v", false);
    REQUIRE(plain.is_ok());
    CHECK(plain.value() == "CREATE DATABASE `v` /*!40100 DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci */ /*!80016 DEFAULT ENCRYPTION='N' */");
    auto guarded = ex.show_create_database_mysql("V", true);
    REQUIRE(guarded.is_ok());
    CHECK(guarded.value() == "CREATE DATABASE /*!32312 IF NOT EXISTS*/ `v` /*!40100 DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci */ /*!80016 DEFAULT ENCRYPTION='N' */");
    CHECK(ex.show_create_database_mysql("nodb", false).error() == "ERROR 1049 (42000): Unknown database 'nodb'");
}

TEST_CASE("R83: information_schema tells the routines, their parameters, the triggers and the checks", "[r83]") {
    TempDataDir dir("test_r83_schema_data");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE v");
    ok(ex, "USE v");
    ok(ex, "CREATE TABLE par (id INT PRIMARY KEY, name VARCHAR(20) NOT NULL, UNIQUE KEY uq_name (name))");
    ok(ex, "CREATE TABLE chi (id INT PRIMARY KEY AUTO_INCREMENT, pid INT NOT NULL, qty INT DEFAULT 1, CONSTRAINT fk_chi_par FOREIGN KEY (pid) REFERENCES par (id), CONSTRAINT ck_qty CHECK (qty >= 0))");
    ok(ex, "CREATE TABLE cu (a INT, b INT, c INT, UNIQUE KEY ab (a, b), UNIQUE (c))");
    ok(ex, "CREATE TABLE ckt (id INT, q INT CHECK (q > 0), CONSTRAINT named CHECK (id < 9), CHECK (id <> q))");
    ok(ex, "CREATE PROCEDURE sp_one(IN x INT, OUT y INT) SET y = x + 1");
    ok(ex, "CREATE PROCEDURE sp_two(IN a INT, INOUT b VARCHAR(20), OUT c DECIMAL(8,2)) NO SQL DETERMINISTIC COMMENT 'it''s' BEGIN SELECT 1; END");
    ok(ex, "CREATE FUNCTION fn_two(x INT) RETURNS INT DETERMINISTIC RETURN x * 2");
    ok(ex, "CREATE FUNCTION fn_three(s VARCHAR(20), d DECIMAL(8,2)) RETURNS VARCHAR(30) COMMENT 'hello' READS SQL DATA BEGIN RETURN CONCAT(s, 'x'); END");
    ok(ex, "CREATE TRIGGER tr_par BEFORE INSERT ON par FOR EACH ROW SET NEW.name = LOWER(NEW.name)");
    ok(ex, "CREATE TRIGGER tr_chi AFTER DELETE ON chi FOR EACH ROW BEGIN UPDATE par SET name = name WHERE id = OLD.pid; END");

    CHECK(flat(ex, "SELECT routine_name, routine_type, data_type, dtd_identifier, character_maximum_length, numeric_precision FROM information_schema.routines "
                   "WHERE routine_schema = 'v' ORDER BY routine_name") ==
          "fn_three,FUNCTION,varchar,varchar(30),30,NULL;fn_two,FUNCTION,int,int,NULL,10;sp_one,PROCEDURE,,NULL,NULL,NULL;sp_two,PROCEDURE,,NULL,NULL,NULL");
    CHECK(flat(ex, "SELECT routine_name, is_deterministic, sql_data_access, security_type, routine_comment, external_language, routine_definition FROM information_schema.routines "
                   "WHERE routine_schema = 'v' ORDER BY routine_name") ==
          "fn_three,NO,READS SQL DATA,DEFINER,hello,SQL,BEGIN RETURN CONCAT(s, 'x'); END;fn_two,YES,CONTAINS SQL,DEFINER,,SQL,RETURN x * 2;"
          "sp_one,NO,CONTAINS SQL,DEFINER,,SQL,SET y = x + 1;sp_two,YES,NO SQL,DEFINER,it's,SQL,BEGIN SELECT 1; END");
    // the parameters: a function's value is parameter 0
    CHECK(flat(ex, "SELECT specific_name, ordinal_position, parameter_mode, parameter_name, dtd_identifier FROM information_schema.parameters WHERE specific_schema = 'v' "
                   "ORDER BY specific_name, ordinal_position") ==
          "fn_three,0,NULL,NULL,varchar(30);fn_three,1,IN,s,varchar(20);fn_three,2,IN,d,decimal(8,2);fn_two,0,NULL,NULL,int;fn_two,1,IN,x,int;"
          "sp_one,1,IN,x,int;sp_one,2,OUT,y,int;sp_two,1,IN,a,int;sp_two,2,INOUT,b,varchar(20);sp_two,3,OUT,c,decimal(8,2)");
    CHECK(flat(ex, "SELECT trigger_name, event_manipulation, event_object_table, action_timing, action_statement FROM information_schema.triggers WHERE trigger_schema = 'v' ORDER BY trigger_name") ==
          "tr_chi,DELETE,chi,AFTER,BEGIN UPDATE par SET name = name WHERE id = OLD.pid; END;tr_par,INSERT,par,BEFORE,SET NEW.name = LOWER(NEW.name)");
    // a database that has none of them lists none
    ok(ex, "CREATE DATABASE other");
    CHECK(flat(ex, "SELECT routine_name FROM information_schema.routines WHERE routine_schema = 'other'").empty());
    // SHOW CREATE TABLE lists the checks in the order of their names, the unnamed ones are <table>_chk_<n> counting only the unnamed, and a check written on a column is there too
    ok(ex, "CREATE TABLE ck2 (id INT, q INT CHECK (q > 0), CONSTRAINT zz CHECK (id > 0), CONSTRAINT aa CHECK (id < 9), CHECK (id <> 5))");
    {
        auto text = ex.show_create_table_mysql("ck2");
        REQUIRE(text.is_ok());
        const std::string& t = text.value();
        const auto at = [&](const char* name) { return t.find(std::string("CONSTRAINT `") + name + "` CHECK"); };
        REQUIRE(at("aa") != std::string::npos);
        REQUIRE(at("ck2_chk_1") != std::string::npos);
        REQUIRE(at("ck2_chk_2") != std::string::npos);
        REQUIRE(at("zz") != std::string::npos);
        CHECK(at("aa") < at("ck2_chk_1"));
        CHECK(at("ck2_chk_1") < at("ck2_chk_2"));
        CHECK(at("ck2_chk_2") < at("zz"));
        CHECK(t.find("(q > 0)") < t.find("(id <> 5)")); // (the one on the column is the first of the unnamed)
    }
    // the CHECK constraints (the names MySQL makes count the checks that were not named) and the UNIQUE keys of several columns
    CHECK(flat(ex, "SELECT constraint_name FROM information_schema.check_constraints WHERE constraint_schema = 'v' ORDER BY constraint_name") == "aa;ck_qty;ck2_chk_1;ck2_chk_2;ckt_chk_1;ckt_chk_2;named;zz");
    CHECK(flat(ex, "SELECT table_name, constraint_name, constraint_type FROM information_schema.table_constraints WHERE table_schema = 'v' AND table_name IN ('cu', 'par') "
                   "ORDER BY table_name, constraint_name") == "cu,ab,UNIQUE;cu,c,UNIQUE;par,PRIMARY,PRIMARY KEY;par,uq_name,UNIQUE");
    CHECK(flat(ex, "SELECT table_name, constraint_name, column_name, ordinal_position FROM information_schema.key_column_usage WHERE table_schema = 'v' AND table_name IN ('cu', 'par') "
                   "ORDER BY table_name, constraint_name, ordinal_position") == "cu,ab,a,1;cu,ab,b,2;cu,c,c,1;par,PRIMARY,id,1;par,uq_name,name,1");
}

TEST_CASE("R83: a view says whether it takes writes, and the collations a table editor lists are there", "[r83]") {
    TempDataDir dir("test_r83_views_data");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE v");
    ok(ex, "USE v");
    ok(ex, "CREATE TABLE par (id INT PRIMARY KEY, name VARCHAR(20))");
    ok(ex, "CREATE TABLE chi (id INT PRIMARY KEY, pid INT, qty INT)");
    ok(ex, "CREATE VIEW vw AS SELECT id, name FROM par");
    ok(ex, "CREATE VIEW vw_agg AS SELECT pid, COUNT(*) AS n FROM chi GROUP BY pid");
    ok(ex, "CREATE VIEW vw_join AS SELECT par.id, chi.qty FROM par JOIN chi ON chi.pid = par.id");
    ok(ex, "CREATE VIEW vw_chk AS SELECT id, name FROM par WHERE id > 1 WITH CHECK OPTION");
    CHECK(flat(ex, "SELECT table_name, is_updatable, check_option FROM information_schema.views WHERE table_schema = 'v' ORDER BY table_name") ==
          "vw,YES,NONE;vw_agg,NO,NONE;vw_chk,YES,CASCADED;vw_join,YES,NONE");
    CHECK(flat(ex, "SELECT COUNT(*) FROM information_schema.views WHERE table_schema = 'v' AND view_definition <> ''") == "4");
    CHECK(flat(ex, "SELECT collation_name, id, is_default, sortlen, pad_attribute FROM information_schema.collations WHERE collation_name IN "
                   "('utf8mb4_general_ci', 'utf8mb4_bin', 'utf8mb4_0900_ai_ci', 'utf8mb4_0900_bin') ORDER BY id") ==
          "utf8mb4_general_ci,45,,1,PAD SPACE;utf8mb4_bin,46,,1,PAD SPACE;utf8mb4_0900_ai_ci,255,Yes,0,NO PAD;utf8mb4_0900_bin,309,,1,NO PAD");
}
