#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/parser/lexer.hpp"

using namespace engine;
namespace fs = std::filesystem;

// What a file written by mysqldump holds, and what a MySQL schema script says, that the engine used to refuse (found by loading a real dump:
// code/test/diff/dump_roundtrip.py): the /*!NNNNN */ comments MySQL runs, SET lists and server variables, FOREIGN_KEY_CHECKS, table and column options,
// DEFAULT / ON UPDATE CURRENT_TIMESTAMP, the DEFINER / ALGORITHM / SQL SECURITY attributes, a view's parenthesized joins, DISABLE / ENABLE KEYS.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

using Rows = std::vector<std::vector<std::string>>;

Rows table_cells(const std::string& output) {
    Rows rows;
    std::istringstream in(output);
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars <= 1) continue;
        std::vector<std::string> row;
        std::size_t pos = 1;
        while (pos < line.size()) {
            std::size_t bar = line.find('|', pos);
            if (bar == std::string::npos) break;
            std::string v = line.substr(pos, bar - pos);
            v.erase(0, v.find_first_not_of(' '));
            v.erase(v.find_last_not_of(' ') + 1);
            row.push_back(v);
            pos = bar + 1;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

void ok(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
}

Rows rows_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
    return table_cells(r.value());
}

std::string cell(Executor& ex, const std::string& sql) {
    Rows rows = rows_of(ex, sql);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].size() == 1);
    return rows[0][0];
}

std::string error_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_err());
    return r.error();
}
} // namespace

TEST_CASE("a comment that begins /*! is run when the server is that version or newer, other comments do nothing", "[dump_syntax]") {
    REQUIRE(expand_executable_comments("SELECT 1") == "SELECT 1");
    REQUIRE(expand_executable_comments("/*!40101 SET @a = 1 */").find("SET @a = 1") != std::string::npos);
    REQUIRE(expand_executable_comments("/*!99999 SET @a = 1 */").find("SET") == std::string::npos);
    REQUIRE(expand_executable_comments("/*M!100000 SET @a = 1 */").find("SET") == std::string::npos);
    REQUIRE(expand_executable_comments("SELECT '/*!40101 x */'") == "SELECT '/*!40101 x */'"); // (inside a string it is text)
    REQUIRE(expand_executable_comments("/*!50001 CREATE */ /*!50013 DEFINER=`r`@`l` */ /*!50001 VIEW v AS SELECT 1 */").find("VIEW v") != std::string::npos);

    TempDataDir dir("dump_syntax_comments");
    Executor ex(dir.path);
    ok(ex, "/*!40101 SET @a = 5 */");
    REQUIRE(cell(ex, "SELECT @a") == "5");
    ok(ex, "/*!99999 SET @a = 6 */");
    REQUIRE(cell(ex, "SELECT @a") == "5");
    ok(ex, "/*!50003 SET @b = 7 */");
    REQUIRE(cell(ex, "SELECT @b") == "7");
    ok(ex, "-- just a comment");
    ok(ex, "/* nothing else */");
    ok(ex, "");
    ok(ex, "SELECT 1 /* inline */ AS x");
}

TEST_CASE("SET takes a list, server variables and the connection's character set", "[dump_syntax]") {
    TempDataDir dir("dump_syntax_set");
    Executor ex(dir.path);
    ok(ex, "SET @a = 1, @b = @a + 1, @c = @b * 10");
    REQUIRE(cell(ex, "SELECT @c") == "20");
    REQUIRE(cell(ex, "SELECT @@foreign_key_checks") == "1");
    REQUIRE(cell(ex, "SELECT @@unique_checks") == "1");
    REQUIRE(cell(ex, "SELECT @@character_set_client") == "utf8mb4");
    ok(ex, "SET @OLD_FK = @@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS = 0");
    REQUIRE(cell(ex, "SELECT @OLD_FK") == "1");
    REQUIRE(cell(ex, "SELECT @@foreign_key_checks") == "0");
    REQUIRE(ex.skip_fk_checks);
    ok(ex, "SET FOREIGN_KEY_CHECKS = @OLD_FK");
    REQUIRE(cell(ex, "SELECT @@foreign_key_checks") == "1");
    REQUIRE_FALSE(ex.skip_fk_checks);
    ok(ex, "SET @@session.foreign_key_checks = OFF");
    REQUIRE(ex.skip_fk_checks);
    ok(ex, "SET GLOBAL foreign_key_checks = ON");
    REQUIRE_FALSE(ex.skip_fk_checks);
    ok(ex, "SET SESSION sql_mode = 'ANSI', time_zone = '+00:00'");
    REQUIRE(cell(ex, "SELECT @@sql_mode") == "ANSI");
    ok(ex, "SET sql_mode = ONLY_FULL_GROUP_BY");
    REQUIRE(cell(ex, "SELECT @@sql_mode") == "ONLY_FULL_GROUP_BY");
    ok(ex, "SET NAMES utf8mb4");
    ok(ex, "SET NAMES utf8mb4 COLLATE utf8mb4_0900_ai_ci");
    ok(ex, "SET CHARACTER SET utf8mb4");
    ok(ex, "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED");
    ok(ex, "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE");
    // an assignment of the list that fails stops it (the ones before it were made)
    REQUIRE(error_of(ex, "SET @e1 = 1, @e2 = (SELECT 1 UNION ALL SELECT 2), @e3 = 3").size() > 0);
    REQUIRE(cell(ex, "SELECT @e1") == "1");
    REQUIRE(cell(ex, "SELECT @e3") == "NULL");
    // a server variable is read inside a statement, too
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE sv (id INT PRIMARY KEY, v INT)");
    ok(ex, "INSERT INTO sv VALUES (1, 1), (2, 0)");
    REQUIRE(rows_of(ex, "SELECT id FROM sv WHERE v = @@foreign_key_checks") == Rows{{"1"}});
    ok(ex, "SET @@foreign_key_checks = 0");
    REQUIRE(rows_of(ex, "SELECT id FROM sv WHERE v = @@session.foreign_key_checks") == Rows{{"2"}});
    REQUIRE(error_of(ex, "SET FOREIGN_KEY_CHECKS = 'maybe'").find("can't be set") != std::string::npos);
    REQUIRE(cell(ex, "SELECT @a") == "1"); // (a user variable is not a server variable)
}

TEST_CASE("FOREIGN_KEY_CHECKS = 0 lets tables refer to each other in any order, as a dump needs", "[dump_syntax]") {
    TempDataDir dir("dump_syntax_fk");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE kid (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES par (id))"); // (the parent comes later)
    REQUIRE(error_of(ex, "INSERT INTO kid VALUES (1, 10)").size() > 0);                              // the check is on: there is no parent
    ok(ex, "SET FOREIGN_KEY_CHECKS = 0");
    ok(ex, "INSERT INTO kid VALUES (1, 10), (2, 11)");                                                // ... and so do its rows
    ok(ex, "CREATE TABLE par (id INT PRIMARY KEY, name VARCHAR(5))");
    ok(ex, "INSERT INTO par VALUES (10, 'a'), (11, 'b')");
    ok(ex, "SET FOREIGN_KEY_CHECKS = 1");
    REQUIRE(rows_of(ex, "SELECT k.id, p.name FROM kid k JOIN par p ON p.id = k.pid ORDER BY k.id") == Rows{{"1", "a"}, {"2", "b"}});
    REQUIRE(error_of(ex, "INSERT INTO kid VALUES (3, 99)").find("Foreign key") != std::string::npos); // checking again
}

TEST_CASE("table options, column options and keys of a CREATE TABLE written by MySQL", "[dump_syntax]") {
    TempDataDir dir("dump_syntax_create");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, R"(CREATE TABLE `t1` (
        `id` int NOT NULL AUTO_INCREMENT,
        `name` varchar(50) CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci NOT NULL COMMENT 'the name',
        `note` text COLLATE utf8mb4_general_ci,
        `n` int unsigned DEFAULT NULL,
        `s` int NOT NULL DEFAULT '0',
        `d` int DEFAULT (7),
        `mark` varchar(5) NULL,
        PRIMARY KEY (`id`),
        UNIQUE KEY `uq_name` (`name`),
        KEY `idx_note` (`note`(10)),
        KEY `idx_two` (`n` DESC, `s`) USING BTREE
    ) ENGINE=InnoDB AUTO_INCREMENT=100 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='catalog')");
    ok(ex, "INSERT INTO t1 (name) VALUES ('a')");
    ok(ex, "INSERT INTO t1 (name) VALUES ('b')");
    REQUIRE(rows_of(ex, "SELECT id, s, d FROM t1 ORDER BY id") == Rows{{"100", "0", "7"}, {"101", "0", "7"}});
    REQUIRE(error_of(ex, "INSERT INTO t1 (name) VALUES ('A')").find("uplicate") != std::string::npos); // (UNIQUE KEY, case-insensitive)
    ok(ex, "CREATE TABLE t2 (id int) ENGINE=MyISAM, ROW_FORMAT=DYNAMIC, DEFAULT CHARACTER SET = utf8mb3");
    ok(ex, "CREATE DATABASE /*!32312 IF NOT EXISTS*/ `d2` /*!40100 DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci */ /*!80016 DEFAULT ENCRYPTION='N' */");
    // a collation or character set that compares differently would be ignored silently: refused
    REQUIRE(error_of(ex, "CREATE TABLE t3 (a varchar(5) COLLATE utf8mb4_bin)").find("not supported") != std::string::npos);
    REQUIRE(error_of(ex, "CREATE TABLE t4 (a int) DEFAULT CHARSET=binary").find("not supported") != std::string::npos);
    REQUIRE(error_of(ex, "CREATE TABLE t5 (a int) COLLATE=utf8mb4_0900_as_cs").find("not supported") != std::string::npos);
    // a key over several columns used to check its first column only
    ok(ex, "CREATE TABLE p (a INT, b INT, PRIMARY KEY (a, b))");
    REQUIRE(error_of(ex, "CREATE TABLE c (a INT, b INT, FOREIGN KEY (a, b) REFERENCES p (a, b))").find("several columns") != std::string::npos);
}

TEST_CASE("DEFAULT CURRENT_TIMESTAMP and ON UPDATE CURRENT_TIMESTAMP", "[dump_syntax]") {
    TempDataDir dir("dump_syntax_timestamp");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, v INT, created TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP, "
           "changed TIMESTAMP NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP, at DATETIME DEFAULT CURRENT_TIMESTAMP(6))");
    ok(ex, "INSERT INTO t (id, v, created, changed) VALUES (1, 1, '2000-01-01 00:00:00', '2000-01-01 00:00:00')");
    ok(ex, "INSERT INTO t (id, v) VALUES (2, 2)");
    Rows rows = rows_of(ex, "SELECT id, created, changed, at FROM t ORDER BY id");
    REQUIRE(rows[0][1] == "2000-01-01 00:00:00");
    REQUIRE(rows[1][1].size() == 19); // the time of the INSERT
    REQUIRE(rows[1][1] != "2000-01-01 00:00:00");
    REQUIRE(rows[1][3].size() == 19);
    // an UPDATE that changes the row sets the ON UPDATE column; one that sets it itself, or changes nothing, leaves it
    ok(ex, "UPDATE t SET v = 11 WHERE id = 1");
    REQUIRE(cell(ex, "SELECT changed FROM t WHERE id = 1") != "2000-01-01 00:00:00");
    REQUIRE(cell(ex, "SELECT created FROM t WHERE id = 1") == "2000-01-01 00:00:00");
    ok(ex, "UPDATE t SET changed = '2001-02-03 04:05:06' WHERE id = 1");
    REQUIRE(cell(ex, "SELECT changed FROM t WHERE id = 1") == "2001-02-03 04:05:06");
    ok(ex, "UPDATE t SET v = v WHERE id = 1");
    REQUIRE(cell(ex, "SELECT changed FROM t WHERE id = 1") == "2001-02-03 04:05:06");
    ok(ex, "UPDATE t SET v = 12, changed = '2002-01-01 00:00:00' WHERE id = 1");
    REQUIRE(cell(ex, "SELECT changed FROM t WHERE id = 1") == "2002-01-01 00:00:00");
}

TEST_CASE("DEFINER and ALGORITHM attributes, parenthesized joins of a view, DISABLE KEYS and ALTER TABLE ADD COLUMN with options", "[dump_syntax]") {
    TempDataDir dir("dump_syntax_objects");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE o (id INT PRIMARY KEY, c INT)");
    ok(ex, "CREATE TABLE i (oid INT, p INT, q INT)");
    ok(ex, "CREATE TABLE pr (id INT PRIMARY KEY, price DECIMAL(10,2))");
    ok(ex, "INSERT INTO o VALUES (1, 1), (2, 1)");
    ok(ex, "INSERT INTO pr VALUES (1, 1.50), (2, 12.00), (3, 30.25)");
    ok(ex, "ALTER TABLE i DISABLE KEYS");
    ok(ex, "INSERT INTO i VALUES (1, 1, 3), (1, 2, 1), (2, 3, 2)");
    ok(ex, "ALTER TABLE i ENABLE KEYS");
    REQUIRE(error_of(ex, "ALTER TABLE nosuch DISABLE KEYS").find("nosuch") != std::string::npos);
    // the text MySQL gives a view
    ok(ex, "/*!50001 CREATE ALGORITHM=UNDEFINED */ /*!50013 DEFINER=`root`@`localhost` SQL SECURITY DEFINER */ /*!50001 VIEW `tot` AS select `o`.`id` AS `order_id`,"
           "sum((`i`.`q` * `pr`.`price`)) AS `total` from ((`o` join `i` on((`i`.`oid` = `o`.`id`))) join `pr` on((`pr`.`id` = `i`.`p`))) group by `o`.`id` */");
    Rows rows = rows_of(ex, "SELECT order_id, total FROM tot ORDER BY order_id");
    REQUIRE(rows.size() == 2);
    REQUIRE(std::stod(rows[0][1]) == Approx(16.5));
    REQUIRE(std::stod(rows[1][1]) == Approx(60.5));
    ok(ex, "CREATE OR REPLACE ALGORITHM=MERGE DEFINER=CURRENT_USER SQL SECURITY INVOKER VIEW v2 AS SELECT id FROM o");
    REQUIRE(rows_of(ex, "SELECT id FROM v2 ORDER BY id") == Rows{{"1"}, {"2"}});
    ok(ex, "CREATE DEFINER=`root`@`localhost` PROCEDURE add_c(IN n INT) BEGIN UPDATE o SET c = c + n; END");
    ok(ex, "CALL add_c(5)");
    REQUIRE(rows_of(ex, "SELECT c FROM o ORDER BY id") == Rows{{"6"}, {"6"}});
    ok(ex, "CREATE DEFINER=`root`@`localhost` FUNCTION plus_one(n int) RETURNS int DETERMINISTIC BEGIN RETURN n + 1; END");
    REQUIRE(cell(ex, "SELECT plus_one(4)") == "5");
    ok(ex, "CREATE PROCEDURE p2(IN a BIGINT, IN b DECIMAL(10,2)) READS SQL DATA COMMENT 'x' SQL SECURITY INVOKER BEGIN UPDATE o SET c = c + a; END");
    ok(ex, "CALL p2(1, 2)");
    REQUIRE(rows_of(ex, "SELECT c FROM o ORDER BY id") == Rows{{"7"}, {"7"}});
    REQUIRE(error_of(ex, "CREATE FUNCTION two(n int) RETURNS int BEGIN DECLARE x INT; RETURN n; END").size() > 0);
    ok(ex, "CREATE DEFINER=`root`@`localhost` TRIGGER trg_o BEFORE INSERT ON o FOR EACH ROW BEGIN SET NEW.c = 0; END");
    ok(ex, "INSERT INTO o VALUES (3, 9)");
    REQUIRE(cell(ex, "SELECT c FROM o WHERE id = 3") == "0");
    // ADD COLUMN takes the options of a column
    ok(ex, "ALTER TABLE o ADD COLUMN x INT NOT NULL DEFAULT 7");
    REQUIRE(rows_of(ex, "SELECT id, x FROM o ORDER BY id") == Rows{{"1", "7"}, {"2", "7"}, {"3", "7"}});
    REQUIRE(error_of(ex, "INSERT INTO o (id, c, x) VALUES (4, 1, NULL)").size() > 0);
}

TEST_CASE("a CTE may name its columns: WITH r (n) AS (...)", "[dump_syntax][cte]") {
    TempDataDir dir("dump_syntax_cte");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE sv (id INT PRIMARY KEY, v INT)");
    ok(ex, "INSERT INTO sv VALUES (1, 10), (2, 20), (3, 30)");
    REQUIRE(rows_of(ex, "WITH RECURSIVE r (n) AS (SELECT 1 UNION ALL SELECT n + 1 FROM r WHERE n < 5) SELECT n FROM r ORDER BY n") ==
            Rows{{"1"}, {"2"}, {"3"}, {"4"}, {"5"}});
    REQUIRE(rows_of(ex, "WITH t (a, b) AS (SELECT id, v FROM sv) SELECT a, b FROM t ORDER BY a") == Rows{{"1", "10"}, {"2", "20"}, {"3", "30"}});
    REQUIRE(rows_of(ex, "WITH t (x, y) AS (SELECT id + 1, UPPER('q') FROM sv WHERE id = 1) SELECT y, x FROM t") == Rows{{"Q", "2"}});
    REQUIRE(rows_of(ex, "WITH t (total) AS (SELECT SUM(v) FROM sv), u (k) AS (SELECT COUNT(*) FROM sv) SELECT total, k FROM t, u") == Rows{{"60", "3"}});
    REQUIRE(error_of(ex, "WITH t (a) AS (SELECT id, v FROM sv) SELECT * FROM t").find("1353") != std::string::npos);
    REQUIRE(error_of(ex, "WITH t (a, b) AS (SELECT * FROM sv) SELECT a FROM t").find("*") != std::string::npos);
    // without a list the CTE keeps the names of its query
    REQUIRE(rows_of(ex, "WITH t AS (SELECT id AS k FROM sv WHERE id = 2) SELECT k FROM t") == Rows{{"2"}});
}

// A statement of several rows finds the rows the table already holds through a hash built once; a one-row statement scans. Both have to give one answer:
// the same rows written by one statement into a table and one by one into its twin leave the two tables alike, for every way a row can clash (the primary key, a
// UNIQUE column that has NULLs and capitals, a composite key) and every way of handling it (error, IGNORE, ON DUPLICATE KEY UPDATE, REPLACE).
TEST_CASE("a statement of several rows and the same rows one by one leave the same table", "[bulk_unique][random]") {
    TempDataDir dir("bulk_unique");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    unsigned seed_count = 3; // RUSQL_FUZZ_SEEDS=40 makes it a long campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    for (unsigned seed = 1; seed <= seed_count; seed++) {
        INFO("seed " << seed);
        for (const char* suffix : {"a", "b"}) {
            ok(ex, std::string("DROP TABLE IF EXISTS u_") + suffix);
            ok(ex, std::string("DROP TABLE IF EXISTS c_") + suffix);
            ok(ex, std::string("CREATE TABLE u_") + suffix + " (id INT PRIMARY KEY, u VARCHAR(8) UNIQUE, v INT)");
            ok(ex, std::string("CREATE TABLE c_") + suffix + " (a VARCHAR(4), b INT, v INT, PRIMARY KEY (a, b))");
        }
        std::mt19937 rng(seed * 7919u);
        auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
        const std::vector<std::string> words = {"a", "A", "b", "B", "c", "ab", "Ab", "x", "X"};
        for (int step = 0; step < 150; step++) {
            const bool composite = pick(2) == 0;
            const std::size_t mode = pick(4); // 0: plain, 1: IGNORE, 2: ON DUPLICATE KEY UPDATE, 3: REPLACE
            const std::size_t rows = 2 + pick(5);
            // distinct keys inside the statement (by the collation): the statement itself holds no clash
            std::vector<std::string> tuples, seen;
            std::vector<std::vector<std::string>> parts; // the key columns and v of each row, for the checks that need no twin
            for (std::size_t r = 0; r < rows; r++) {
                for (int attempt = 0; attempt < 20; attempt++) {
                    std::string tuple, key_id;
                    std::vector<std::string> current_part;
                    if (composite) {
                        const std::string a = std::string(1, "xXyY"[pick(4)]);
                        const int b = static_cast<int>(pick(3));
                        tuple = "('" + a + "', " + std::to_string(b) + ", " + std::to_string(step * 10 + static_cast<int>(r)) + ")";
                        key_id = std::string(1, static_cast<char>(std::tolower(static_cast<unsigned char>(a[0])))) + std::to_string(b);
                        current_part = {a, std::to_string(b), std::to_string(step * 10 + static_cast<int>(r))};
                    } else {
                        const int id = 1 + static_cast<int>(pick(14));
                        const std::string w = pick(5) == 0 ? "NULL" : "'" + words[pick(words.size())] + "'";
                        std::string lower = w;
                        for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                        tuple = "(" + std::to_string(id) + ", " + w + ", " + std::to_string(step * 10 + static_cast<int>(r)) + ")";
                        key_id = "id" + std::to_string(id) + (w == "NULL" ? "|null" + std::to_string(r) : "|" + lower);
                        // (two rows clash when they share the id or the lower-cased word)
                        bool clash = false;
                        for (auto& other : seen) {
                            const std::string other_id = other.substr(0, other.find('|')), other_w = other.substr(other.find('|') + 1);
                            if (other_id == "id" + std::to_string(id) || (w != "NULL" && other_w == lower)) clash = true;
                        }
                        if (clash) continue;
                        current_part = {std::to_string(id), w, std::to_string(step * 10 + static_cast<int>(r))};
                    }
                    if (composite && std::find(seen.begin(), seen.end(), key_id) != seen.end()) continue;
                    seen.push_back(key_id);
                    parts.push_back(current_part);
                    tuples.push_back(tuple);
                    break;
                }
            }
            if (tuples.size() < 2) continue;
            const std::string table = composite ? "c_" : "u_";
            std::string values;
            for (std::size_t k = 0; k < tuples.size(); k++) values += (k ? ", " : "") + tuples[k];
            const char* verbs[] = {"INSERT INTO ", "INSERT IGNORE INTO ", "INSERT INTO ", "REPLACE INTO "};
            const std::string tail = mode == 2 ? " ON DUPLICATE KEY UPDATE v = VALUES(v)" : "";
            INFO("step " << step << ": " << table << " mode " << mode << ": " << values);
            auto whole = ex.execute_sql(verbs[mode] + table + "a VALUES " + values + tail);
            if (mode == 0 && whole.is_err()) {
                // an error leaves the table as it was, and the rows one by one must hit the same clash
                bool clashed = false;
                for (auto& tuple : tuples) clashed = ex.execute_sql(std::string(verbs[mode]) + table + "b VALUES " + tuple).is_err() || clashed;
                REQUIRE(clashed);
                // the twin kept the rows that went in before the clash: bring it back to the other table's state
                const std::string cols = composite ? "a, b, v" : "id, u, v";
                ok(ex, "DELETE FROM " + table + "b");
                ok(ex, "INSERT INTO " + table + "b SELECT " + cols + " FROM " + table + "a");
                continue;
            }
            REQUIRE(whole.is_ok());
            for (auto& tuple : tuples) ok(ex, std::string(verbs[mode]) + table + "b VALUES " + tuple + tail);
            const std::string order = composite ? "SELECT a, b, v FROM c_X ORDER BY a, b" : "SELECT id, u, v FROM u_X ORDER BY id";
            std::string qa = order, qb = order;
            qa.replace(qa.find("X"), 1, "a");
            qb.replace(qb.find("X"), 1, "b");
            REQUIRE(rows_of(ex, qa) == rows_of(ex, qb));
            // what the twin cannot tell: no two rows hold one key (by the collation), and a REPLACE leaves the row it wrote
            for (const char* suffix : {"a", "b"}) {
                if (composite) {
                    REQUIRE(cell(ex, std::string("SELECT COUNT(*) FROM c_") + suffix) == cell(ex, std::string("SELECT COUNT(*) FROM (SELECT DISTINCT a, b FROM c_") + suffix + ") d"));
                } else {
                    REQUIRE(cell(ex, std::string("SELECT COUNT(u) FROM u_") + suffix) == cell(ex, std::string("SELECT COUNT(DISTINCT u) FROM u_") + suffix));
                }
            }
            if (mode == 3) {
                for (auto& part : parts) {
                    const std::string where = composite ? "a = '" + part[0] + "' AND b = " + part[1] : "id = " + part[0];
                    REQUIRE(cell(ex, "SELECT v FROM " + table + "a WHERE " + where) == part[2]);
                }
            }
        }
    }
    // a composite primary key is compared by the table's collation, whether one row is written or several
    ok(ex, "DROP TABLE IF EXISTS cc");
    ok(ex, "CREATE TABLE cc (a VARCHAR(10), b INT, PRIMARY KEY (a, b))");
    ok(ex, "INSERT INTO cc VALUES ('Alice', 1)");
    REQUIRE(error_of(ex, "INSERT INTO cc VALUES ('alice', 1)").find("Duplicate") != std::string::npos);
    REQUIRE(error_of(ex, "INSERT INTO cc VALUES ('bob', 1), ('ALICE', 1)").find("Duplicate") != std::string::npos);
    ok(ex, "INSERT INTO cc VALUES ('alice', 2), ('Bob', 1)");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM cc") == Rows{{"3"}});
}

TEST_CASE("REPLACE over a composite primary key is refused as a whole when a replaced row is referenced", "[bulk_unique]") {
    TempDataDir dir("bulk_unique_replace");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE rp (a INT, b INT, v INT, PRIMARY KEY (a, b))");
    ok(ex, "CREATE TABLE rk (id INT PRIMARY KEY, pa INT, FOREIGN KEY (pa) REFERENCES rp (a))");
    ok(ex, "INSERT INTO rp VALUES (1, 1, 10), (2, 1, 20), (3, 1, 30)");
    ok(ex, "INSERT INTO rk VALUES (1, 1)");
    // several rows: the second one is referenced, so nothing is replaced (the first one too stays)
    REQUIRE(error_of(ex, "REPLACE INTO rp VALUES (2, 1, 99), (1, 1, 99)").size() > 0);
    REQUIRE(rows_of(ex, "SELECT a, b, v FROM rp ORDER BY a") == Rows{{"1", "1", "10"}, {"2", "1", "20"}, {"3", "1", "30"}});
    // one row
    REQUIRE(error_of(ex, "REPLACE INTO rp VALUES (1, 1, 99)").size() > 0);
    REQUIRE(rows_of(ex, "SELECT v FROM rp WHERE a = 1") == Rows{{"10"}});
    // rows that nothing refers to are replaced
    ok(ex, "REPLACE INTO rp VALUES (2, 1, 98), (3, 1, 97)");
    REQUIRE(rows_of(ex, "SELECT a, b, v FROM rp ORDER BY a") == Rows{{"1", "1", "10"}, {"2", "1", "98"}, {"3", "1", "97"}});
}
