#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// The statement forms a dump or an application uses besides the plain ones (CREATE TABLE ... LIKE / AS SELECT, RENAME TABLE, DROP TABLE a, b, ALTER TABLE
// with several actions, JOIN db.table), the counters of a session (LAST_INSERT_ID, ROW_COUNT, FOUND_ROWS), autocommit = 0, information_schema read as
// tables, and the window / date functions that were missing. Every expected value is MySQL 8.4's own answer (tables.sql in test/diff/corpus holds the same
// statements and is compared with a real MySQL).

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

// the data rows of a SELECT as "c1,c2;c1,c2" (an empty cell is left out)
std::string rows_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_ok());
    std::istringstream in(r.value());
    std::string line, out;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars == 1) continue;
        std::string row, cell;
        std::istringstream cs(line);
        while (std::getline(cs, cell, '|')) {
            cell.erase(0, cell.find_first_not_of(" \t\r"));
            cell.erase(cell.find_last_not_of(" \t\r") + 1);
            if (cell.empty()) continue;
            row += (row.empty() ? "" : ",") + cell;
        }
        out += (out.empty() ? "" : ";") + row;
    }
    return out;
}

void ok(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_ok());
}

void refused(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_err());
}

void open_database(Executor& ex) {
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
}
} // namespace

TEST_CASE("CREATE TABLE ... LIKE / AS SELECT, RENAME TABLE, DROP TABLE a, b, TRUNCATE t", "[ddl_forms]") {
    TempDataDir dir("ddl_session_forms_create");
    Executor ex(dir.path);
    open_database(ex);
    ok(ex, "CREATE TABLE ds (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(20) NOT NULL, pay DECIMAL(8,2) DEFAULT 1.50, grp INT, UNIQUE KEY uq_name (name), KEY idx_grp (grp))");
    ok(ex, "INSERT INTO ds (name, pay, grp) VALUES ('a', 10.25, 1), ('b', 20.50, 1), ('c', 30.75, 2)");

    ok(ex, "CREATE TABLE ds_like LIKE ds");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM ds_like") == "0");
    ok(ex, "INSERT INTO ds_like (name, grp) VALUES ('x', 1)");
    refused(ex, "INSERT INTO ds_like (name, grp) VALUES ('x', 2)"); // the UNIQUE key was copied
    REQUIRE(rows_of(ex, "SELECT id, name, pay, grp FROM ds_like") == "1,x,1.50,1");

    ok(ex, "CREATE TABLE ds_copy AS SELECT id, name, id * 2 AS double_id FROM ds WHERE grp = 1");
    REQUIRE(rows_of(ex, "SELECT id, name, double_id FROM ds_copy ORDER BY id") == "1,a,2;2,b,4");
    ok(ex, "CREATE TABLE ds_sum AS SELECT grp, COUNT(*) AS n, MAX(id) AS top FROM ds GROUP BY grp");
    REQUIRE(rows_of(ex, "SELECT grp, n, top FROM ds_sum ORDER BY grp") == "1,2,2;2,1,3");
    ok(ex, "CREATE TABLE ds_empty AS SELECT id, name FROM ds WHERE 1 = 0");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM ds_empty") == "0");
    refused(ex, "CREATE TABLE ds_copy AS SELECT 1");
    ok(ex, "CREATE TABLE IF NOT EXISTS ds_copy AS SELECT 1");
    ok(ex, "CREATE TABLE ds_expl (id INT PRIMARY KEY, name VARCHAR(5)) AS SELECT id, name FROM ds");
    REQUIRE(rows_of(ex, "SELECT id, name FROM ds_expl ORDER BY id") == "1,a;2,b;3,c");

    ok(ex, "RENAME TABLE ds_empty TO ds_none");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM ds_none") == "0");
    refused(ex, "SELECT COUNT(*) FROM ds_empty");
    ok(ex, "RENAME TABLE ds_none TO ds_empty, ds_sum TO ds_totals");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM ds_totals") == "2");

    ok(ex, "DROP TABLE ds_empty, ds_totals");
    refused(ex, "DROP TABLE ds_copy, nosuch_table"); // none goes when one is missing
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM ds_copy") == "2");
    ok(ex, "DROP TABLE IF EXISTS ds_copy, nosuch_table");
    refused(ex, "SELECT COUNT(*) FROM ds_copy");

    ok(ex, "TRUNCATE ds_like");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM ds_like") == "0");
}

TEST_CASE("ALTER TABLE with several actions is all or nothing", "[ddl_forms]") {
    TempDataDir dir("ddl_session_forms_alter");
    Executor ex(dir.path);
    open_database(ex);
    ok(ex, "CREATE TABLE a1 (id INT PRIMARY KEY, name VARCHAR(20), pay DECIMAL(8,2), grp INT)");
    ok(ex, "INSERT INTO a1 VALUES (1, 'a', 10.25, 1)");

    ok(ex, "ALTER TABLE a1 ADD COLUMN extra INT DEFAULT 7 AFTER name, ADD COLUMN first_c INT FIRST");
    REQUIRE(rows_of(ex, "SELECT * FROM a1") == "NULL,1,a,7,10.25,1"); // first_c, id, name, extra, pay, grp
    ok(ex, "ALTER TABLE a1 CHANGE COLUMN extra bonus BIGINT NOT NULL DEFAULT 3, MODIFY COLUMN pay DECIMAL(10,2)");
    REQUIRE(rows_of(ex, "SELECT bonus FROM a1") == "7");
    ok(ex, "ALTER TABLE a1 ADD INDEX idx_pay (pay), ADD KEY (bonus)");
    ok(ex, "ALTER TABLE a1 ALTER COLUMN bonus SET DEFAULT 9");
    ok(ex, "INSERT INTO a1 (id, name) VALUES (2, 'd')");
    REQUIRE(rows_of(ex, "SELECT bonus FROM a1 WHERE id = 2") == "9");
    ok(ex, "ALTER TABLE a1 MODIFY COLUMN grp INT AFTER id");
    REQUIRE(rows_of(ex, "SELECT * FROM a1 WHERE id = 1") == "NULL,1,1,a,7,10.25"); // first_c, id, grp, name, bonus, pay
    ok(ex, "ALTER TABLE a1 ENGINE=InnoDB");

    // an action that cannot be done leaves the table as it was (the ones before it are not kept)
    const std::string before = rows_of(ex, "SELECT * FROM a1 WHERE id = 1");
    refused(ex, "ALTER TABLE a1 ADD COLUMN dup_col INT, ADD COLUMN DUP_COL INT");
    refused(ex, "ALTER TABLE a1 ADD COLUMN ok_col INT, DROP COLUMN no_such_col");
    refused(ex, "ALTER TABLE a1 ADD COLUMN ok_col INT, ADD COLUMN after_missing INT AFTER no_such_col");
    refused(ex, "ALTER TABLE a1 ADD COLUMN ok_col INT, MODIFY COLUMN no_such_col INT");
    refused(ex, "ALTER TABLE a1 ADD COLUMN ok_col INT, ALTER COLUMN no_such_col SET DEFAULT 1");
    refused(ex, "ALTER TABLE a1 ADD COLUMN ok_col INT, ADD INDEX idx_no (no_such_col)");
    refused(ex, "SELECT ok_col FROM a1");
    refused(ex, "SELECT dup_col FROM a1");
    REQUIRE(rows_of(ex, "SELECT * FROM a1 WHERE id = 1") == before);

    // a single action on a column that is not there is an error too
    refused(ex, "ALTER TABLE a1 DROP COLUMN no_such_col");
    refused(ex, "ALTER TABLE a1 RENAME COLUMN no_such_col TO zzz");
    refused(ex, "ALTER TABLE a1 MODIFY COLUMN pay DECIMAL(12,3) AFTER no_such_col");
    REQUIRE(rows_of(ex, "SELECT column_type FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'a1' AND column_name = 'pay'") == "decimal(10,2)"); // nothing was changed

    // DROP / CHANGE / MODIFY / ALTER COLUMN name a column of the table as it was before the statement (a column an earlier action added is not there for
    // them), an index may name one that was just added
    refused(ex, "ALTER TABLE a1 ADD COLUMN ok_col INT, ALTER COLUMN ok_col SET DEFAULT 4");
    refused(ex, "ALTER TABLE a1 ADD COLUMN ok_col INT, DROP COLUMN ok_col");
    refused(ex, "ALTER TABLE a1 DROP COLUMN pay, DROP COLUMN pay");
    ok(ex, "ALTER TABLE a1 ADD COLUMN ok_col INT, CHANGE COLUMN name name2 VARCHAR(30), ALTER COLUMN pay SET DEFAULT 4, DROP COLUMN first_c, ADD INDEX idx_ok (ok_col)");
    REQUIRE(rows_of(ex, "SELECT name2 FROM a1 WHERE id = 1") == "a");
    refused(ex, "SELECT name FROM a1");
    refused(ex, "SELECT first_c FROM a1");
}

TEST_CASE("MODIFY / CHANGE COLUMN keeps the UNIQUE KEY of the column", "[ddl_forms]") {
    TempDataDir dir("ddl_session_forms_modify_unique");
    Executor ex(dir.path);
    open_database(ex);
    ok(ex, "CREATE TABLE u1 (id INT PRIMARY KEY, name VARCHAR(20) NOT NULL, UNIQUE KEY uq_name (name))");
    ok(ex, "INSERT INTO u1 VALUES (1, 'a')");
    ok(ex, "ALTER TABLE u1 MODIFY COLUMN name VARCHAR(30) NOT NULL");
    refused(ex, "INSERT INTO u1 VALUES (2, 'a')");
    ok(ex, "ALTER TABLE u1 CHANGE COLUMN name name2 VARCHAR(40) NOT NULL");
    refused(ex, "INSERT INTO u1 VALUES (3, 'A')"); // the default collation does not tell 'a' from 'A'
    ok(ex, "INSERT INTO u1 VALUES (4, 'b')");
    REQUIRE(rows_of(ex, "SELECT index_name, column_name FROM information_schema.statistics WHERE table_schema = DATABASE() AND table_name = 'u1' ORDER BY index_name") ==
            "PRIMARY,id;uq_name,name2");
}

TEST_CASE("JOIN names a table as db.table", "[ddl_forms]") {
    TempDataDir dir("ddl_session_forms_join");
    Executor ex(dir.path);
    open_database(ex);
    ok(ex, "CREATE TABLE p (id INT PRIMARY KEY, g INT)");
    ok(ex, "CREATE TABLE q (g INT, b INT)");
    ok(ex, "INSERT INTO p VALUES (1, 1), (2, 2), (3, 9)");
    ok(ex, "INSERT INTO q VALUES (1, 10), (1, 11), (2, 20)");
    REQUIRE(rows_of(ex, "SELECT x.id, k.b FROM d.p x JOIN d.q k ON k.g = x.g ORDER BY x.id, k.b") == "1,10;1,11;2,20");
    REQUIRE(rows_of(ex, "SELECT p.id, q.b FROM d.p JOIN d.q ON q.g = p.g ORDER BY p.id, q.b") == "1,10;1,11;2,20");
    REQUIRE(rows_of(ex, "SELECT x.id, k.b FROM p AS x LEFT JOIN d.q AS k ON k.g = x.g ORDER BY x.id, k.b") == "1,10;1,11;2,20;3,NULL");
    REQUIRE(rows_of(ex, "SELECT d.p.id FROM d.p WHERE d.p.id = 2") == "2");
    ok(ex, "SELECT id FROM p WHERE id = 1 FOR UPDATE");
    ok(ex, "SELECT id FROM p WHERE id = 1 LOCK IN SHARE MODE");
    ok(ex, "SELECT id FROM p WHERE id = 1 FOR UPDATE NOWAIT");
    ok(ex, "SELECT id FROM p WHERE id = 1 FOR SHARE SKIP LOCKED");
    REQUIRE(rows_of(ex, "(SELECT id FROM p WHERE id < 2) UNION (SELECT id FROM p WHERE id > 2) ORDER BY id") == "1;3");
}

TEST_CASE("a locking read outside a transaction holds no lock", "[session_forms]") {
    TempDataDir dir("ddl_session_forms_locking_read");
    Executor s1(dir.path);
    open_database(s1);
    Executor s2 = Executor::new_session(s1.get_shared());
    ok(s2, "USE d");
    ok(s2, "SET @lock_wait_timeout = 300");
    ok(s1, "CREATE TABLE k (id INT PRIMARY KEY, v INT)");
    ok(s1, "INSERT INTO k VALUES (1, 1)");
    ok(s1, "SELECT id FROM k WHERE id = 1 FOR UPDATE"); // autocommit: the lock would be gone with the statement
    ok(s2, "UPDATE k SET v = 2 WHERE id = 1");
    REQUIRE(rows_of(s2, "SELECT v FROM k WHERE id = 1") == "2");
    ok(s1, "BEGIN"); // inside a transaction the lock is held until it ends
    ok(s1, "SELECT id FROM k WHERE id = 1 FOR UPDATE");
    refused(s2, "UPDATE k SET v = 3 WHERE id = 1");
    ok(s1, "ROLLBACK");
    ok(s2, "UPDATE k SET v = 3 WHERE id = 1");
}

TEST_CASE("LAST_INSERT_ID, ROW_COUNT, FOUND_ROWS and SQL_CALC_FOUND_ROWS", "[session_forms]") {
    TempDataDir dir("ddl_session_forms_counters");
    Executor ex(dir.path);
    open_database(ex);
    ok(ex, "CREATE TABLE sess (id INT PRIMARY KEY AUTO_INCREMENT, v INT)");
    ok(ex, "INSERT INTO sess (v) VALUES (10), (20), (30)");
    REQUIRE(rows_of(ex, "SELECT LAST_INSERT_ID()") == "1"); // the first value the statement made
    ok(ex, "UPDATE sess SET v = v + 1 WHERE id < 3");
    REQUIRE(rows_of(ex, "SELECT ROW_COUNT()") == "2");
    ok(ex, "DELETE FROM sess WHERE id = 3");
    REQUIRE(rows_of(ex, "SELECT ROW_COUNT()") == "1");
    ok(ex, "SELECT id FROM sess ORDER BY id LIMIT 1");
    REQUIRE(rows_of(ex, "SELECT FOUND_ROWS()") == "1");
    ok(ex, "SELECT SQL_CALC_FOUND_ROWS id FROM sess LIMIT 1");
    REQUIRE(rows_of(ex, "SELECT FOUND_ROWS()") == "2");
    ok(ex, "INSERT INTO sess (id, v) VALUES (50, 1)");
    REQUIRE(rows_of(ex, "SELECT LAST_INSERT_ID()") == "1"); // a value given by the statement is not "made"
    ok(ex, "INSERT INTO sess (v) VALUES (2)");
    REQUIRE(rows_of(ex, "SELECT LAST_INSERT_ID()") == "51");
    REQUIRE(rows_of(ex, "SELECT LAST_INSERT_ID(99)") == "99");
    REQUIRE(rows_of(ex, "SELECT LAST_INSERT_ID()") == "99");
}

TEST_CASE("SET autocommit = 0 keeps the writes in a transaction until COMMIT", "[session_forms]") {
    TempDataDir dir("ddl_session_forms_autocommit");
    Executor ex(dir.path);
    open_database(ex);
    ok(ex, "CREATE TABLE ac (id INT PRIMARY KEY, v INT)");
    ok(ex, "SET autocommit = 0");
    ok(ex, "INSERT INTO ac VALUES (1, 1)");
    ok(ex, "ROLLBACK");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM ac") == "0");
    ok(ex, "INSERT INTO ac VALUES (2, 2)");
    ok(ex, "COMMIT");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM ac") == "1");
    ok(ex, "INSERT INTO ac VALUES (3, 3)");
    ok(ex, "SET autocommit = 1"); // commits what is open
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM ac") == "2");
    ok(ex, "ROLLBACK");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM ac") == "2");
    REQUIRE(rows_of(ex, "SELECT @@autocommit") == "1");
    ok(ex, "SET autocommit = 0");
    ok(ex, "INSERT INTO ac VALUES (4, 4)");
    ok(ex, "CREATE TABLE ac2 (x INT)"); // a statement that defines a table commits what is open
    ok(ex, "ROLLBACK");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM ac") == "3");
    ok(ex, "SET autocommit = 1");
}

TEST_CASE("information_schema is read as tables", "[session_forms]") {
    TempDataDir dir("ddl_session_forms_infoschema");
    Executor ex(dir.path);
    open_database(ex);
    ok(ex, "CREATE TABLE ds (id INT PRIMARY KEY, name VARCHAR(20) NOT NULL, pay DECIMAL(8,2), grp INT, UNIQUE KEY uq_name (name), KEY idx_grp (grp))");
    ok(ex, "CREATE TABLE ds_k (a INT, b INT, KEY idx_ab (a, b), KEY (b))");

    REQUIRE(rows_of(ex, "SELECT column_name, column_key FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name = 'ds' ORDER BY ordinal_position") ==
            "id,PRI;name,UNI;pay;grp,MUL");
    REQUIRE(rows_of(ex, "SELECT index_name, non_unique, seq_in_index, column_name FROM information_schema.statistics WHERE table_schema = DATABASE() AND table_name = 'ds' ORDER BY index_name, seq_in_index") ==
            "idx_grp,1,1,grp;PRIMARY,0,1,id;uq_name,0,1,name");
    REQUIRE(rows_of(ex, "SELECT table_name, COUNT(*) FROM information_schema.columns WHERE table_schema = DATABASE() GROUP BY table_name ORDER BY table_name") == "ds,4;ds_k,2");
    REQUIRE(rows_of(ex, "SELECT COUNT(DISTINCT table_name) FROM information_schema.columns WHERE table_schema = DATABASE() AND table_name LIKE 'ds%'") == "2");
    REQUIRE(rows_of(ex, "SELECT t.table_name, COUNT(c.column_name) FROM information_schema.tables t JOIN information_schema.columns c ON c.table_schema = t.table_schema AND c.table_name = t.table_name WHERE t.table_schema = DATABASE() AND t.table_name = 'ds_k' GROUP BY t.table_name") == "ds_k,2");
    REQUIRE(rows_of(ex, "SHOW TABLES LIKE 'ds\\_%'") == "ds_k");
    REQUIRE(rows_of(ex, "SHOW TABLES FROM d LIKE 'ds'") == "ds");
    REQUIRE(rows_of(ex, "SHOW TABLES LIKE 'nomatch%'") == "");
}

TEST_CASE("window functions take a default and name their columns by their text", "[session_forms]") {
    TempDataDir dir("ddl_session_forms_window");
    Executor ex(dir.path);
    open_database(ex);
    ok(ex, "CREATE TABLE wf (id INT PRIMARY KEY, g VARCHAR(3), v INT)");
    ok(ex, "INSERT INTO wf VALUES (1, 'a', 10), (2, 'a', NULL), (3, 'b', 30), (4, 'b', 40)");
    REQUIRE(rows_of(ex, "SELECT id, COUNT(*) OVER (), COUNT(v) OVER () FROM wf ORDER BY id") == "1,4,3;2,4,3;3,4,3;4,4,3");
    REQUIRE(rows_of(ex, "SELECT id, LAG(v, 2, 0) OVER (ORDER BY id), LEAD(v, 1, -1) OVER (ORDER BY id) FROM wf ORDER BY id") == "1,0,NULL;2,0,30;3,10,40;4,NULL,-1");
    REQUIRE(rows_of(ex, "SELECT id, PERCENT_RANK() OVER (ORDER BY id), CUME_DIST() OVER (ORDER BY id) FROM wf ORDER BY id") ==
            "1,0,0.25;2,0.3333333333333333,0.5;3,0.6666666666666666,0.75;4,1,1");
    REQUIRE(rows_of(ex, "SELECT PERCENT_RANK() OVER (ORDER BY id) FROM wf WHERE id = 1") == "0"); // one row
    auto named = ex.execute_sql("SELECT COUNT(*) OVER (), LAG(v, 2, 0) OVER (ORDER BY id) FROM wf");
    REQUIRE(named.is_ok());
    REQUIRE(named.value().find("COUNT(*) OVER ()") != std::string::npos);
    REQUIRE(named.value().find("LAG(v, 2, 0) OVER (ORDER BY id)") != std::string::npos);
}

TEST_CASE("TIME_FORMAT, ADDTIME, SUBTIME, TIMEDIFF, PERIOD_*, YEARWEEK, BIT_COUNT, EXPORT_SET, MAKE_SET, REGEXP_INSTR", "[session_forms]") {
    TempDataDir dir("ddl_session_forms_functions");
    Executor ex(dir.path);
    REQUIRE(rows_of(ex, "SELECT TIME_FORMAT('10:20:30', '%H.%i.%s'), ADDTIME('10:00:00', '02:30:00'), SUBTIME('10:00:00', '00:30:00'), TIMEDIFF('12:00:00', '10:30:00')") ==
            "10.20.30,12:30:00,09:30:00,01:30:00");
    REQUIRE(rows_of(ex, "SELECT ADDTIME('2024-01-01 10:00:00', '36:00:00'), SUBTIME('2024-01-01 10:00:00', '11:00:00'), TIMEDIFF('2024-01-02 10:00:00', '2024-01-01 08:30:00')") ==
            "2024-01-02 22:00:00,2023-12-31 23:00:00,25:30:00");
    REQUIRE(rows_of(ex, "SELECT PERIOD_ADD(202401, 3), PERIOD_ADD(2401, 12), PERIOD_DIFF(202403, 202401), YEARWEEK('2024-03-05'), YEARWEEK('2024-01-01', 1)") ==
            "202404,202501,2,202409,202401");
    REQUIRE(rows_of(ex, "SELECT BIT_COUNT(7), BIT_COUNT(255), BIT_COUNT(0), EXPORT_SET(5, 'Y', 'N', ',', 4), MAKE_SET(3, 'a', 'b', 'c'), MAKE_SET(5, 'x', 'y', 'z')") ==
            "3,8,0,Y,N,Y,N,a,b,x,z");
    REQUIRE(rows_of(ex, "SELECT REGEXP_INSTR('abc123', '[0-9]'), REGEXP_INSTR('abc', '[0-9]')") == "4,0");
    // the week of a date near the year's end belongs to the year the week is counted in
    REQUIRE(rows_of(ex, "SELECT YEARWEEK('2024-01-01'), YEARWEEK('2023-01-01'), YEARWEEK('2021-01-03', 3), YEARWEEK('2020-12-31', 1)") == "202353,202301,202053,202053");
    REQUIRE(rows_of(ex, "SELECT YEARWEEK('2024-12-30'), YEARWEEK('2024-12-30', 3), YEARWEEK('2022-01-01', 0)") == "202452,202501,202152");
}

TEST_CASE("a trigger sees NEW.col as the column writes it", "[session_forms]") {
    TempDataDir dir("ddl_session_forms_trigger");
    Executor ex(dir.path);
    open_database(ex);
    ok(ex, "CREATE TABLE tg (id INT PRIMARY KEY, bal DECIMAL(10,2))");
    ok(ex, "CREATE TABLE tg_log (note VARCHAR(40))");
    ok(ex, "INSERT INTO tg VALUES (1, 100)");
    ok(ex, "CREATE TRIGGER tg_upd AFTER UPDATE ON tg FOR EACH ROW INSERT INTO tg_log VALUES (CONCAT(OLD.bal, '->', NEW.bal))");
    ok(ex, "UPDATE tg SET bal = bal + 1");
    REQUIRE(rows_of(ex, "SELECT note FROM tg_log") == "100.00->101.00");
}
