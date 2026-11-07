#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/parser/ast_json.hpp"
#include "engine/parser/parser.hpp"
#include "engine/result_text.hpp"

using namespace engine;
namespace fs = std::filesystem;

// R71: what MySQL does around a transaction that the engine did not: DDL commits the open transaction, READ ONLY, the isolation variables, COMMIT AND CHAIN, savepoint errors;
// a statement of a table with triggers is all or nothing, BEFORE UPDATE triggers set NEW.x; TRUNCATE of a referenced table, the definition of a foreign key; writes through views;
// OPTIMIZE TABLE and CHECK TABLE.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

using Rows = std::vector<std::vector<std::string>>;

void ok(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("answer: " << (r.is_err() ? r.error() : r.value().substr(0, 80)));
    REQUIRE(r.is_ok());
}

// the statement must fail, and the error must say `part`
void refused(Executor& ex, const std::string& sql, const std::string& part) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("answer: " << (r.is_err() ? r.error() : r.value().substr(0, 120)));
    REQUIRE(r.is_err());
    REQUIRE(r.error().find(part) != std::string::npos);
}

Rows rows_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("answer: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
    Rows rows;
    std::istringstream in(r.value());
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars <= 1) continue;
        rows.push_back(result_text::split_row(line));
    }
    return rows;
}

std::string one(Executor& ex, const std::string& sql) {
    Rows rows = rows_of(ex, sql);
    REQUIRE(rows.size() == 1);
    REQUIRE(!rows[0].empty());
    return rows[0][0];
}

void shop(Executor& ex) {
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE t (id INT AUTO_INCREMENT PRIMARY KEY, v INT, name VARCHAR(10))");
    ok(ex, "INSERT INTO t (v, name) VALUES (10, 'a'), (20, 'b'), (30, 'c')");
}
} // namespace

TEST_CASE("R71: DDL and BEGIN commit the open transaction first", "[r71][txn]") {
    TempDataDir dir("r71_implicit_commit");
    Executor ex(dir.path);
    shop(ex);
    ok(ex, "BEGIN");
    ok(ex, "INSERT INTO t (v, name) VALUES (90, 'h')");
    ok(ex, "CREATE TABLE other (id INT)");
    ok(ex, "ROLLBACK"); // (nothing to roll back: the CREATE TABLE committed the INSERT)
    REQUIRE(one(ex, "SELECT COUNT(*) FROM t WHERE name = 'h'") == "1");
    ok(ex, "BEGIN");
    ok(ex, "INSERT INTO t (v, name) VALUES (91, 'i')");
    ok(ex, "TRUNCATE TABLE other");
    ok(ex, "ROLLBACK");
    REQUIRE(one(ex, "SELECT COUNT(*) FROM t WHERE name = 'i'") == "1");
    // BEGIN inside a transaction commits it and begins another
    ok(ex, "BEGIN");
    ok(ex, "INSERT INTO t (v, name) VALUES (92, 'j')");
    ok(ex, "BEGIN");
    ok(ex, "INSERT INTO t (v, name) VALUES (93, 'k')");
    ok(ex, "ROLLBACK");
    REQUIRE(one(ex, "SELECT COUNT(*) FROM t WHERE name = 'j'") == "1");
    REQUIRE(one(ex, "SELECT COUNT(*) FROM t WHERE name = 'k'") == "0");
}

TEST_CASE("R71: READ ONLY transactions and sessions refuse the statements that write", "[r71][txn]") {
    TempDataDir dir("r71_read_only");
    Executor ex(dir.path);
    shop(ex);
    ok(ex, "START TRANSACTION READ ONLY");
    refused(ex, "INSERT INTO t (v, name) VALUES (1, 'ro')", "ERROR 1792 (25006): Cannot execute statement in a READ ONLY transaction.");
    refused(ex, "UPDATE t SET v = 0", "1792");
    refused(ex, "DELETE FROM t", "1792");
    REQUIRE(one(ex, "SELECT COUNT(*) FROM t") == "3");
    ok(ex, "COMMIT");
    ok(ex, "START TRANSACTION READ WRITE");
    ok(ex, "INSERT INTO t (v, name) VALUES (1, 'rw')");
    ok(ex, "COMMIT");
    // the session: SET SESSION TRANSACTION READ ONLY and the variable say the same
    ok(ex, "SET SESSION TRANSACTION READ ONLY");
    REQUIRE(one(ex, "SELECT @@transaction_read_only") == "1");
    refused(ex, "INSERT INTO t (v, name) VALUES (5, 'ro2')", "1792");
    refused(ex, "CREATE TABLE nope (id INT)", "1792");
    refused(ex, "TRUNCATE TABLE t", "1792");
    ok(ex, "START TRANSACTION READ WRITE"); // (a transaction may ask for the other mode)
    ok(ex, "INSERT INTO t (v, name) VALUES (5, 'rw2')");
    ok(ex, "COMMIT");
    ok(ex, "SET SESSION TRANSACTION READ WRITE");
    REQUIRE(one(ex, "SELECT @@transaction_read_only") == "0");
    ok(ex, "SET transaction_read_only = 1");
    refused(ex, "INSERT INTO t (v, name) VALUES (6, 'ro3')", "1792");
    ok(ex, "SET transaction_read_only = OFF");
    ok(ex, "INSERT INTO t (v, name) VALUES (6, 'rw3')");
    refused(ex, "SET transaction_read_only = 'maybe'", "ERROR 1231");
    // SET TRANSACTION (no SESSION) is for the next transaction only: BEGIN takes it
    ok(ex, "SET TRANSACTION READ ONLY");
    refused(ex, "INSERT INTO t (v, name) VALUES (7, 'ro4')", "1792");
    ok(ex, "BEGIN");
    refused(ex, "INSERT INTO t (v, name) VALUES (7, 'ro5')", "1792");
    ok(ex, "COMMIT");
    ok(ex, "INSERT INTO t (v, name) VALUES (7, 'rw4')");
    REQUIRE(one(ex, "SELECT COUNT(*) FROM t WHERE name LIKE 'ro%'") == "0");
}

TEST_CASE("R71: @@transaction_isolation says the level the session runs at, however it was set", "[r71][txn]") {
    TempDataDir dir("r71_isolation");
    Executor ex(dir.path);
    shop(ex);
    REQUIRE(one(ex, "SELECT @@transaction_isolation") == "READ-COMMITTED"); // (this engine's level when nothing was set)
    ok(ex, "SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE");
    REQUIRE(one(ex, "SELECT @@transaction_isolation") == "SERIALIZABLE");
    REQUIRE(one(ex, "SELECT @@session.transaction_isolation") == "SERIALIZABLE");
    REQUIRE(ex.execute_sql("SHOW ISOLATION LEVEL").value().find("SERIALIZABLE") != std::string::npos);
    ok(ex, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    REQUIRE(one(ex, "SELECT @@transaction_isolation") == "REPEATABLE-READ");
    ok(ex, "SET SESSION TRANSACTION ISOLATION LEVEL READ UNCOMMITTED");
    REQUIRE(one(ex, "SELECT @@transaction_isolation") == "READ-UNCOMMITTED");
    ok(ex, "SET transaction_isolation = 'read-committed'");
    REQUIRE(one(ex, "SELECT @@transaction_isolation") == "READ-COMMITTED");
    ok(ex, "SET SESSION transaction_isolation = 'SERIALIZABLE'");
    REQUIRE(one(ex, "SELECT @@transaction_isolation") == "SERIALIZABLE");
    ok(ex, "SET @@session.transaction_isolation = 'REPEATABLE-READ'");
    REQUIRE(one(ex, "SELECT @@transaction_isolation") == "REPEATABLE-READ");
    ok(ex, "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED, READ ONLY");
    REQUIRE(one(ex, "SELECT @@transaction_isolation") == "READ-COMMITTED");
    REQUIRE(one(ex, "SELECT @@transaction_read_only") == "1");
    ok(ex, "SET SESSION TRANSACTION READ WRITE, ISOLATION LEVEL SERIALIZABLE");
    REQUIRE(one(ex, "SELECT @@transaction_read_only") == "0");
    REQUIRE(one(ex, "SELECT @@transaction_isolation") == "SERIALIZABLE");
    // names MySQL does not take
    refused(ex, "SET transaction_isolation = 'READ COMMITTED'", "ERROR 1231 (42000): Variable 'transaction_isolation' can't be set to the value of 'READ COMMITTED'");
    refused(ex, "SET transaction_isolation = 'nonsense'", "1231");
    ok(ex, "SET transaction_isolation = DEFAULT");
    REQUIRE(one(ex, "SELECT @@transaction_isolation") == "READ-COMMITTED");
    // SET TRANSACTION (no SESSION) is for the next transaction: the variable keeps the session's level
    ok(ex, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    ok(ex, "SET TRANSACTION ISOLATION LEVEL SERIALIZABLE");
    REQUIRE(one(ex, "SELECT @@transaction_isolation") == "REPEATABLE-READ");
    ok(ex, "BEGIN");
    REQUIRE(ex.txn.isolation_level() == IsolationLevel::Serializable);
    // ... not while a transaction is open (1568); the session forms are allowed, for the next one
    refused(ex, "SET TRANSACTION ISOLATION LEVEL READ COMMITTED", "ERROR 1568 (25001): Transaction characteristics can't be changed while a transaction is in progress");
    ok(ex, "SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED");
    REQUIRE(ex.txn.isolation_level() == IsolationLevel::Serializable); // (this transaction goes on as it began)
    ok(ex, "COMMIT");
    ok(ex, "BEGIN");
    REQUIRE(ex.txn.isolation_level() == IsolationLevel::ReadCommitted); // (the one-shot level went with the first transaction)
    ok(ex, "COMMIT");
    REQUIRE(one(ex, "SELECT @@transaction_isolation") == "READ-COMMITTED");
}

TEST_CASE("R71: COMMIT / ROLLBACK AND [NO] CHAIN, and the errors of savepoints", "[r71][txn]") {
    TempDataDir dir("r71_chain");
    Executor ex(dir.path);
    shop(ex);
    ok(ex, "SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE");
    ok(ex, "START TRANSACTION READ ONLY");
    ok(ex, "COMMIT AND CHAIN");
    REQUIRE(ex.txn.is_active());
    REQUIRE(ex.txn.isolation_level() == IsolationLevel::Serializable);
    refused(ex, "INSERT INTO t (v, name) VALUES (1, 'x')", "1792"); // (the chained transaction is READ ONLY too)
    ok(ex, "ROLLBACK AND NO CHAIN");
    REQUIRE(!ex.txn.is_active());
    ok(ex, "BEGIN");
    ok(ex, "INSERT INTO t (v, name) VALUES (1, 'x')");
    ok(ex, "ROLLBACK AND CHAIN");
    REQUIRE(ex.txn.is_active());
    REQUIRE(one(ex, "SELECT COUNT(*) FROM t WHERE name = 'x'") == "0");
    ok(ex, "COMMIT WORK AND NO CHAIN RELEASE");
    REQUIRE(!ex.txn.is_active());
    // a COMMIT with nothing open chains nothing
    ok(ex, "COMMIT AND CHAIN");
    REQUIRE(!ex.txn.is_active());
    // savepoints: outside a transaction one is taken and keeps nothing, and a missing one is error 1305
    ok(ex, "SAVEPOINT nothing_open");
    refused(ex, "ROLLBACK TO SAVEPOINT nothing_open", "ERROR 1305 (42000): SAVEPOINT nothing_open does not exist");
    refused(ex, "RELEASE SAVEPOINT nothing_open", "1305");
    ok(ex, "BEGIN");
    ok(ex, "SAVEPOINT s1");
    refused(ex, "ROLLBACK TO SAVEPOINT s2", "SAVEPOINT s2 does not exist");
    ok(ex, "RELEASE SAVEPOINT s1");
    refused(ex, "ROLLBACK TO SAVEPOINT s1", "1305");
    ok(ex, "ROLLBACK");
}

TEST_CASE("R71: the new transaction forms survive the AST JSON", "[r71][txn][ast]") {
    for (const char* sql : {"START TRANSACTION READ ONLY", "COMMIT AND CHAIN", "ROLLBACK AND CHAIN", "SET TRANSACTION READ ONLY", "SET SESSION TRANSACTION ISOLATION LEVEL SERIALIZABLE, READ WRITE"}) {
        INFO(sql);
        Parser parser(sql);
        auto parsed = parser.parse();
        REQUIRE(parsed.is_ok());
        nlohmann::json j = parsed.value();
        Statement back = j.get<Statement>();
        REQUIRE(nlohmann::json(back) == j);
    }
    Parser parser("START TRANSACTION READ ONLY");
    auto parsed = parser.parse();
    REQUIRE(parsed.is_ok());
    REQUIRE(std::get<Statement::Begin>(parsed.value().data).read_only == std::optional<bool>(true));
    Parser chain("COMMIT AND CHAIN");
    REQUIRE(std::get<Statement::Commit>(chain.parse().value().data).chain);
    Parser plain("COMMIT");
    REQUIRE(!std::get<Statement::Commit>(plain.parse().value().data).chain);
}

TEST_CASE("R71: a trigger sets several NEW columns in one SET, and a BEFORE UPDATE trigger may set NEW.x", "[r71][trigger]") {
    TempDataDir dir("r71_before_update");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE ta (id INT AUTO_INCREMENT PRIMARY KEY, v INT, note VARCHAR(20), upd INT DEFAULT 0)");
    ok(ex, "CREATE TRIGGER ta_bi BEFORE INSERT ON ta FOR EACH ROW SET NEW.note = CONCAT('bi:', COALESCE(NEW.note, '-')), NEW.v = NEW.v * 10");
    ok(ex, "CREATE TRIGGER ta_bu BEFORE UPDATE ON ta FOR EACH ROW SET NEW.upd = OLD.upd + 1, NEW.note = CONCAT(OLD.note, '>', NEW.v)");
    ok(ex, "INSERT INTO ta (v, note) VALUES (1, 'x'), (2, NULL)");
    REQUIRE(rows_of(ex, "SELECT id, v, note, upd FROM ta ORDER BY id") == Rows{{"1", "10", "bi:x", "0"}, {"2", "20", "bi:-", "0"}});
    ok(ex, "UPDATE ta SET v = v + 1 WHERE id = 1");
    REQUIRE(rows_of(ex, "SELECT id, v, note, upd FROM ta ORDER BY id") == Rows{{"1", "11", "bi:x>11", "1"}, {"2", "20", "bi:-", "0"}});
    ok(ex, "UPDATE ta SET v = v + 5"); // (every row, each with the values its own trigger run made)
    REQUIRE(rows_of(ex, "SELECT id, v, note, upd FROM ta ORDER BY id") == Rows{{"1", "16", "bi:x>11>16", "2"}, {"2", "25", "bi:->25", "1"}});
    // the trigger sees both rows of an update by key; an AFTER trigger reads what the BEFORE trigger made
    ok(ex, "CREATE TABLE log (n INT AUTO_INCREMENT PRIMARY KEY, what VARCHAR(40))");
    ok(ex, "CREATE TRIGGER ta_au AFTER UPDATE ON ta FOR EACH ROW INSERT INTO log (what) VALUES (CONCAT(OLD.v, '->', NEW.v, ' ', NEW.upd))");
    ok(ex, "UPDATE ta SET v = 0 WHERE id = 2");
    REQUIRE(rows_of(ex, "SELECT what FROM log") == Rows{{"25->0 2"}});
    // a body with IF and a compound statement
    ok(ex, "CREATE TABLE tc (id INT PRIMARY KEY, kind VARCHAR(10), qty INT, ts INT)");
    ok(ex, "CREATE TRIGGER tc_bu BEFORE UPDATE ON tc FOR EACH ROW BEGIN IF NEW.qty < 0 THEN SET NEW.qty = OLD.qty; END IF; IF NEW.qty <> OLD.qty THEN SET NEW.kind = 'moved'; END IF; SET NEW.ts = OLD.ts + 1; END");
    ok(ex, "INSERT INTO tc VALUES (1, 'x', 5, 0), (2, 'x', 5, 0)");
    ok(ex, "UPDATE tc SET qty = -1 WHERE id = 1");
    ok(ex, "UPDATE tc SET qty = qty + 1 WHERE id = 2");
    REQUIRE(rows_of(ex, "SELECT id, kind, qty, ts FROM tc ORDER BY id") == Rows{{"1", "x", "5", "1"}, {"2", "moved", "6", "1"}});
    // the primary key may be changed by the statement; the trigger still finds its rows
    ok(ex, "UPDATE tc SET id = id + 10 WHERE id = 2");
    REQUIRE(rows_of(ex, "SELECT id, kind, qty, ts FROM tc ORDER BY id") == Rows{{"1", "x", "5", "1"}, {"12", "moved", "6", "2"}});
}

TEST_CASE("R71: a statement of a table with triggers is all or nothing, and the error is the trigger's own", "[r71][trigger]") {
    TempDataDir dir("r71_trigger_atomic");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE td (id INT PRIMARY KEY, n INT NOT NULL)");
    ok(ex, "CREATE TABLE te (id INT PRIMARY KEY)");
    ok(ex, "CREATE TRIGGER td_ai AFTER INSERT ON td FOR EACH ROW INSERT INTO te VALUES (NEW.id)");
    ok(ex, "INSERT INTO td VALUES (1, 1), (2, 2)");
    ok(ex, "INSERT INTO te VALUES (5)");
    // the third row's trigger fails: neither the rows of td nor the rows the trigger wrote before it stay
    auto r = ex.execute_sql("INSERT INTO td VALUES (3, 3), (5, 5), (6, 6)");
    REQUIRE(r.is_err());
    REQUIRE(r.error().find("Duplicate") != std::string::npos);
    REQUIRE(r.error().find("Trigger") == std::string::npos);
    REQUIRE(rows_of(ex, "SELECT id FROM td ORDER BY id") == Rows{{"1"}, {"2"}});
    REQUIRE(rows_of(ex, "SELECT id FROM te ORDER BY id") == Rows{{"1"}, {"2"}, {"5"}});
    refused(ex, "INSERT INTO td VALUES (7, NULL)", "NULL");
    REQUIRE(rows_of(ex, "SELECT id FROM te ORDER BY id") == Rows{{"1"}, {"2"}, {"5"}});
    // inside a transaction only the statement is taken back; the transaction goes on
    ok(ex, "BEGIN");
    ok(ex, "INSERT INTO td VALUES (8, 8)");
    REQUIRE(ex.execute_sql("INSERT INTO td VALUES (9, 9), (5, 5)").is_err());
    REQUIRE(rows_of(ex, "SELECT id FROM td ORDER BY id") == Rows{{"1"}, {"2"}, {"8"}});
    ok(ex, "COMMIT");
    REQUIRE(rows_of(ex, "SELECT id FROM td ORDER BY id") == Rows{{"1"}, {"2"}, {"8"}});
    REQUIRE(rows_of(ex, "SELECT id FROM te ORDER BY id") == Rows{{"1"}, {"2"}, {"5"}, {"8"}});
    ok(ex, "BEGIN");
    ok(ex, "INSERT INTO td VALUES (10, 10)");
    ok(ex, "ROLLBACK");
    REQUIRE(one(ex, "SELECT COUNT(*) FROM td WHERE id = 10") == "0");
    REQUIRE(one(ex, "SELECT COUNT(*) FROM te WHERE id = 10") == "0");
    // the same for an UPDATE and a DELETE whose trigger fails
    ok(ex, "CREATE TABLE tf (id INT PRIMARY KEY, v INT)");
    ok(ex, "INSERT INTO tf VALUES (1, 1), (2, 2), (3, 3)");
    ok(ex, "CREATE TRIGGER tf_au AFTER UPDATE ON tf FOR EACH ROW INSERT INTO te VALUES (NEW.v)");
    REQUIRE(ex.execute_sql("UPDATE tf SET v = v + 20").is_ok()); // (21, 22, 23 are not in te)
    REQUIRE(ex.execute_sql("UPDATE tf SET v = v - 19").is_err()); // (the first row would write 2, which te has: the statement is taken back as a whole)
    REQUIRE(rows_of(ex, "SELECT id, v FROM tf ORDER BY id") == Rows{{"1", "21"}, {"2", "22"}, {"3", "23"}});
    REQUIRE(rows_of(ex, "SELECT id FROM te ORDER BY id") == Rows{{"1"}, {"2"}, {"5"}, {"8"}, {"21"}, {"22"}, {"23"}});
}

TEST_CASE("R71: TRUNCATE of a referenced table (1701) and what MySQL refuses in a foreign key definition", "[r71][fk]") {
    TempDataDir dir("r71_fk_definition");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE fp (id INT PRIMARY KEY, name VARCHAR(10))");
    ok(ex, "CREATE TABLE fc (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES fp(id))");
    ok(ex, "INSERT INTO fp VALUES (1, 'a')");
    refused(ex, "TRUNCATE TABLE fp", "ERROR 1701 (42000): Cannot truncate a table referenced in a foreign key constraint (`d`.`fc`, CONSTRAINT `fc_ibfk_1`)");
    REQUIRE(one(ex, "SELECT COUNT(*) FROM fp") == "1");
    ok(ex, "TRUNCATE TABLE fc"); // (the child side is free)
    ok(ex, "SET FOREIGN_KEY_CHECKS = 0");
    ok(ex, "TRUNCATE TABLE fp");
    ok(ex, "SET FOREIGN_KEY_CHECKS = 1");
    // 1824: the parent is not there; 3780: the types do not go together; 1830: SET NULL on a NOT NULL column
    refused(ex, "CREATE TABLE ft2 (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES nosuch(id))", "ERROR 1824 (HY000): Failed to open the referenced table 'nosuch'");
    refused(ex, "CREATE TABLE ft3 (id INT PRIMARY KEY, p VARCHAR(5), FOREIGN KEY (p) REFERENCES fp(id))", "ERROR 3780");
    refused(ex, "CREATE TABLE ft5 (id INT PRIMARY KEY, p INT NOT NULL, FOREIGN KEY (p) REFERENCES fp(id) ON DELETE SET NULL)", "ERROR 1830");
    ok(ex, "CREATE TABLE ft4 (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES fp(id) ON DELETE SET NULL)");
    ok(ex, "CREATE TABLE fq (id INT PRIMARY KEY, p VARCHAR(5))");
    refused(ex, "ALTER TABLE fq ADD FOREIGN KEY (p) REFERENCES fp(id)", "ERROR 3780");
    // a dump creates tables in any order with FOREIGN_KEY_CHECKS = 0
    ok(ex, "SET FOREIGN_KEY_CHECKS = 0");
    ok(ex, "CREATE TABLE early (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES late(id))");
    ok(ex, "CREATE TABLE late (id INT PRIMARY KEY)");
    ok(ex, "SET FOREIGN_KEY_CHECKS = 1");
}

TEST_CASE("R71: writes through views", "[r71][view]") {
    TempDataDir dir("r71_views");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE vb (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(10) NOT NULL, qty INT DEFAULT 5, price INT, grp INT)");
    ok(ex, "INSERT INTO vb (name, qty, price, grp) VALUES ('a', 1, 10, 1), ('b', 2, 20, 1), ('c', 3, 30, 2), ('d', 4, 40, 2)");
    ok(ex, "CREATE TABLE vg (id INT PRIMARY KEY, label VARCHAR(10))");
    ok(ex, "CREATE VIEW v_all AS SELECT id, name, qty FROM vb");
    ok(ex, "CREATE VIEW v_ren AS SELECT id AS ident, name AS title, price * 2 AS twice FROM vb");
    ok(ex, "CREATE VIEW v_ren2 AS SELECT id AS ident, name AS title FROM vb");
    ok(ex, "CREATE VIEW v_noname AS SELECT id, qty FROM vb");
    ok(ex, "CREATE VIEW v_join AS SELECT vb.id, vb.name, vg.label FROM vb JOIN vg ON vg.id = vb.grp");
    ok(ex, "CREATE VIEW v_agg AS SELECT grp, COUNT(*) AS n FROM vb GROUP BY grp");
    ok(ex, "CREATE VIEW v_dist AS SELECT DISTINCT grp FROM vb");
    ok(ex, "CREATE VIEW v_lim AS SELECT id, name FROM vb ORDER BY id LIMIT 2");
    ok(ex, "CREATE VIEW v_big AS SELECT id, name, qty FROM vb WHERE qty > 2");
    ok(ex, "CREATE VIEW v_over AS SELECT id, name FROM v_big");
    // an INSERT without a column list takes the columns of the view, not of the table
    ok(ex, "INSERT INTO v_all VALUES (NULL, 'f', 7)");
    REQUIRE(rows_of(ex, "SELECT id, name, qty, price FROM vb WHERE name = 'f'") == Rows{{"5", "f", "7", "NULL"}});
    ok(ex, "INSERT INTO v_all (name, qty) VALUES ('e', 6)");
    ok(ex, "UPDATE v_all SET qty = qty + 100 WHERE name = 'a'");
    ok(ex, "DELETE FROM v_all WHERE name = 'f'");
    REQUIRE(rows_of(ex, "SELECT name, qty FROM vb ORDER BY id") == Rows{{"a", "101"}, {"b", "2"}, {"c", "3"}, {"d", "4"}, {"e", "6"}});
    // renamed columns: the view's names are mapped to the table's, in the SET and in the WHERE
    ok(ex, "UPDATE v_ren2 SET title = 'ren' WHERE ident = 2");
    REQUIRE(one(ex, "SELECT name FROM vb WHERE id = 2") == "ren");
    ok(ex, "UPDATE v_ren2 SET title = CONCAT(title, '!') WHERE ident = 2"); // (the value reads a column of the view by its name there too)
    REQUIRE(one(ex, "SELECT name FROM vb WHERE id = 2") == "ren!");
    ok(ex, "UPDATE v_ren2 SET title = 'ren' WHERE ident = 2");
    ok(ex, "DELETE FROM v_ren2 WHERE title = 'e'");
    REQUIRE(one(ex, "SELECT COUNT(*) FROM vb WHERE name = 'e'") == "0");
    refused(ex, "UPDATE v_ren SET twice = 1 WHERE ident = 2", "ERROR 1348 (HY000): Column 'twice' is not updatable");
    refused(ex, "INSERT INTO v_ren (title) VALUES ('t')", "ERROR 1471 (HY000): The target table v_ren of the INSERT is not insertable-into");
    refused(ex, "INSERT INTO v_all (nosuch) VALUES (1)", "Unknown column 'nosuch'");
    // a column the view hides, NOT NULL with no default
    refused(ex, "INSERT INTO v_noname (qty) VALUES (1)", "ERROR 1423 (HY000): Field of view 'd.v_noname' underlying table doesn't have a default value");
    // what a view cannot take
    refused(ex, "DELETE FROM v_join WHERE id = 1", "ERROR 1395 (HY000): Can not delete from join view 'd.v_join'");
    refused(ex, "UPDATE v_agg SET n = 1", "ERROR 1288 (HY000): The target table v_agg of the UPDATE is not updatable");
    refused(ex, "DELETE FROM v_agg", "ERROR 1288 (HY000): The target table v_agg of the DELETE is not updatable");
    refused(ex, "INSERT INTO v_agg (grp) VALUES (9)", "ERROR 1471 (HY000): The target table v_agg of the INSERT is not insertable-into");
    refused(ex, "UPDATE v_dist SET grp = 0", "ERROR 1288");
    refused(ex, "UPDATE v_lim SET name = 'lm' WHERE id = 1", "ERROR 1288 (HY000): The target table v_lim of the UPDATE is not updatable");
    refused(ex, "DELETE FROM v_lim WHERE id = 1", "ERROR 1288");
    // a view over a view reaches the table through both, with both WHEREs
    ok(ex, "UPDATE v_over SET name = 'ov' WHERE id = 4");
    REQUIRE(rows_of(ex, "SELECT id, name FROM vb WHERE name = 'ov'") == Rows{{"4", "ov"}});
    ok(ex, "UPDATE v_over SET name = 'zz' WHERE id = 1"); // (qty 101 > 2: in the view)
    ok(ex, "UPDATE v_over SET name = 'zz2' WHERE id = 2"); // (qty 2: not in the view: nothing to update)
    REQUIRE(one(ex, "SELECT name FROM vb WHERE id = 2") == "ren");
    // WITH CHECK OPTION
    ok(ex, "CREATE VIEW v_chk AS SELECT id, name, qty FROM vb WHERE qty < 10 WITH CHECK OPTION");
    ok(ex, "CREATE VIEW v_cas AS SELECT id, name, qty FROM v_chk WHERE qty > 0 WITH CASCADED CHECK OPTION");
    ok(ex, "CREATE VIEW v_loc AS SELECT id, name, qty FROM vb WHERE qty < 10 WITH LOCAL CHECK OPTION");
    ok(ex, "INSERT INTO v_chk (name, qty) VALUES ('ok', 3)");
    refused(ex, "INSERT INTO v_chk (name, qty) VALUES ('bad', 30)", "ERROR 1369 (HY000): CHECK OPTION failed 'd.v_chk'");
    refused(ex, "UPDATE v_chk SET qty = 30 WHERE name = 'ok'", "ERROR 1369 (HY000): CHECK OPTION failed 'd.v_chk'");
    ok(ex, "UPDATE v_chk SET qty = 4 WHERE name = 'ok'");
    refused(ex, "INSERT INTO v_cas (name, qty) VALUES ('c0', 0)", "CHECK OPTION failed 'd.v_cas'");
    ok(ex, "INSERT INTO v_cas (name, qty) VALUES ('c1', 1)");
    refused(ex, "INSERT INTO v_loc (name, qty) VALUES ('l', 50)", "CHECK OPTION failed 'd.v_loc'");
    REQUIRE(rows_of(ex, "SELECT name, qty FROM vb WHERE name IN ('ok', 'bad', 'c0', 'c1', 'l') ORDER BY name") == Rows{{"c1", "1"}, {"ok", "4"}});
    REQUIRE(ex.execute_sql("SHOW CREATE VIEW v_chk").value().find("WITH CASCADED CHECK OPTION") != std::string::npos);
}

TEST_CASE("R71: OPTIMIZE TABLE and CHECK TABLE answer as MySQL does and do their work", "[r71][maint]") {
    TempDataDir dir("r71_maintenance");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE p (id INT PRIMARY KEY)");
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, p INT, u INT UNIQUE, FOREIGN KEY (p) REFERENCES p(id))");
    ok(ex, "INSERT INTO p VALUES (1), (2)");
    ok(ex, "INSERT INTO c VALUES (1, 1, 10), (2, 2, 20)");
    REQUIRE(rows_of(ex, "CHECK TABLE c") == Rows{{"d.c", "check", "status", "OK"}});
    REQUIRE(rows_of(ex, "CHECK TABLE p, c QUICK") == Rows{{"d.p", "check", "status", "OK"}, {"d.c", "check", "status", "OK"}});
    REQUIRE(rows_of(ex, "check table `c`") == Rows{{"d.c", "check", "status", "OK"}});
    REQUIRE(rows_of(ex, "CHECK TABLE nosuch") == Rows{{"d.nosuch", "check", "Error", "Table 'd.nosuch' doesn't exist"}, {"d.nosuch", "check", "status", "Operation failed"}});
    // a row whose parent is not there (made with the check off) is found
    ok(ex, "SET FOREIGN_KEY_CHECKS = 0");
    ok(ex, "INSERT INTO c VALUES (3, 99, 30)");
    ok(ex, "SET FOREIGN_KEY_CHECKS = 1");
    Rows found = rows_of(ex, "CHECK TABLE c");
    REQUIRE(found.size() == 2);
    REQUIRE(found[0][2] == "error");
    REQUIRE(found[0][3].find("99") != std::string::npos);
    REQUIRE(found[1] == std::vector<std::string>({"d.c", "check", "status", "Corrupt"}));
    // ... and the rows two of which share a key, or that hold NULL where it is refused (put there below the SQL, as a crash or a copied file could)
    {
        auto w = ex.get_shared()->write();
        auto& rows = w->tables.at("d.c");
        Row twin = rows.front();
        twin["p"] = "1";
        rows.push_back(twin); // the same id 1, the same u 10
        Row hole = rows[1];
        hole["id"] = "7";
        hole["u"] = "70";
        hole["p"] = EXECUTOR_NULL_VALUE; // (p may be NULL; id may not)
        hole["id"] = EXECUTOR_NULL_VALUE;
        rows.push_back(hole);
    }
    Rows broken = rows_of(ex, "CHECK TABLE c");
    std::string said;
    for (const auto& r : broken) said += r[2] + ": " + r[3] + "; ";
    REQUIRE(said.find("error: Duplicate entry '1' for key 'PRIMARY'") != std::string::npos);
    REQUIRE(said.find("error: Duplicate entry '10' for key 'u'") != std::string::npos);
    REQUIRE(said.find("error: Column 'id' is NOT NULL but a row holds NULL") != std::string::npos);
    REQUIRE(broken.back() == std::vector<std::string>({"d.c", "check", "status", "Corrupt"}));
    {
        auto w = ex.get_shared()->write();
        w->tables.at("d.c").resize(3); // (the three rows the table had)
    }
    // OPTIMIZE: the dead row versions go and the statistics are taken again
    ok(ex, "DELETE FROM c WHERE id = 3");
    for (int i = 0; i < 20; i++) ok(ex, "UPDATE p SET id = id WHERE id = 1");
    REQUIRE(rows_of(ex, "OPTIMIZE TABLE p") == Rows{{"d.p", "optimize", "note", "Table does not support optimize, doing recreate + analyze instead"}, {"d.p", "optimize", "status", "OK"}});
    REQUIRE(rows_of(ex, "OPTIMIZE NO_WRITE_TO_BINLOG TABLE p, c").size() == 4);
    REQUIRE(rows_of(ex, "OPTIMIZE TABLE nosuch").size() == 2);
    ok(ex, "CREATE VIEW pv AS SELECT * FROM p");
    REQUIRE(rows_of(ex, "CHECK TABLE pv")[0][3].find("not BASE TABLE") != std::string::npos);
    REQUIRE(one(ex, "SELECT COUNT(*) FROM p") == "2");
    // OPTIMIZE commits the open transaction, like the other statements that define things
    ok(ex, "BEGIN");
    ok(ex, "INSERT INTO p VALUES (3)");
    ok(ex, "OPTIMIZE TABLE p");
    ok(ex, "ROLLBACK");
    REQUIRE(one(ex, "SELECT COUNT(*) FROM p WHERE id = 3") == "1");
}

TEST_CASE("R71: with FOREIGN_KEY_CHECKS = 0 a parent row can be deleted or re-keyed and its children are left as they are", "[r71][fk]") {
    TempDataDir dir("r71_fk_checks_off");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE fp (id INT PRIMARY KEY)");
    ok(ex, "CREATE TABLE fr (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES fp(id) ON DELETE RESTRICT ON UPDATE RESTRICT)");
    ok(ex, "CREATE TABLE fc (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES fp(id) ON DELETE CASCADE ON UPDATE CASCADE)");
    ok(ex, "INSERT INTO fp VALUES (1), (2), (3)");
    ok(ex, "INSERT INTO fr VALUES (10, 1)");
    ok(ex, "INSERT INTO fc VALUES (20, 2), (21, 3)");
    refused(ex, "DELETE FROM fp WHERE id = 1", "Foreign key violation");
    ok(ex, "SET FOREIGN_KEY_CHECKS = 0");
    ok(ex, "DELETE FROM fp WHERE id = 1"); // (RESTRICT is not looked at)
    ok(ex, "DELETE FROM fp WHERE id = 2"); // (CASCADE does not happen)
    ok(ex, "UPDATE fp SET id = 30 WHERE id = 3"); // (nor does ON UPDATE CASCADE)
    ok(ex, "SET FOREIGN_KEY_CHECKS = 1");
    REQUIRE(rows_of(ex, "SELECT id FROM fp ORDER BY id") == Rows{{"30"}});
    REQUIRE(rows_of(ex, "SELECT id, p FROM fc ORDER BY id") == Rows{{"20", "2"}, {"21", "3"}});
    REQUIRE(rows_of(ex, "SELECT id, p FROM fr ORDER BY id") == Rows{{"10", "1"}});
    // CHECK TABLE finds the children that lost their parent
    REQUIRE(rows_of(ex, "CHECK TABLE fc").back() == std::vector<std::string>({"d.fc", "check", "status", "Corrupt"}));
}

TEST_CASE("R71: dates and times are read as MySQL reads them", "[r71][date]") {
    TempDataDir dir("r71_dates");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    // STR_TO_DATE with the day of the year
    REQUIRE(one(ex, "SELECT STR_TO_DATE('2024 10', '%Y %j')") == "2024-01-10");
    REQUIRE(one(ex, "SELECT STR_TO_DATE('2024 366', '%Y %j')") == "2024-12-31");
    REQUIRE(one(ex, "SELECT STR_TO_DATE('2023 366', '%Y %j')") == "NULL");
    // a TIME value stops at 838:59:59
    REQUIRE(one(ex, "SELECT SEC_TO_TIME(3020400)") == "838:59:59");
    REQUIRE(one(ex, "SELECT SEC_TO_TIME(-3020400)") == "-838:59:59");
    REQUIRE(one(ex, "SELECT ADDTIME('838:59:59', '01:00:00')") == "838:59:59");
    REQUIRE(one(ex, "SELECT SEC_TO_TIME(3020399)") == "838:59:59");
    // strings that are no dates
    REQUIRE(one(ex, "SELECT DATE('31/03/2024')") == "NULL");
    REQUIRE(one(ex, "SELECT CAST('31/03/2024' AS DATE)") == "NULL");
    REQUIRE(one(ex, "SELECT CAST('2024-02-29 25:00:00' AS DATETIME)") == "NULL");
    REQUIRE(one(ex, "SELECT DATE('2024-02-29')") == "2024-02-29");
    REQUIRE(one(ex, "SELECT DATE('24-02-29')") == "2024-02-29");
    REQUIRE(one(ex, "SELECT CAST(240229 AS DATE)") == "2024-02-29");
    REQUIRE(one(ex, "SELECT CAST('240229' AS DATE)") == "2024-02-29");
    REQUIRE(one(ex, "SELECT CAST(20240229 AS DATE)") == "2024-02-29");
    // CONVERT_TZ: offsets and UTC; a named zone is NULL without the time zone tables
    REQUIRE(one(ex, "SELECT CONVERT_TZ('2024-01-01 12:00:00', '+00:00', '+09:00')") == "2024-01-01 21:00:00");
    REQUIRE(one(ex, "SELECT CONVERT_TZ('2024-01-01 12:00:00', '+09:00', '-05:00')") == "2023-12-31 22:00:00");
    REQUIRE(one(ex, "SELECT CONVERT_TZ('2024-01-01 12:00:00', 'UTC', '+01:30')") == "2024-01-01 13:30:00");
    REQUIRE(one(ex, "SELECT CONVERT_TZ('2024-01-01 12:00:00', 'Asia/Seoul', 'UTC')") == "NULL");
    REQUIRE(one(ex, "SELECT CONVERT_TZ('nonsense', '+00:00', '+01:00')") == "NULL");
    REQUIRE(one(ex, "SELECT CONVERT_TZ(NULL, '+00:00', '+01:00')") == "NULL");
    // FROM_UNIXTIME with a fraction of a second keeps it (the day depends on the time zone of the machine)
    const std::string fractional = one(ex, "SELECT FROM_UNIXTIME(1700000000.5)");
    REQUIRE(fractional.size() == 21);
    REQUIRE(fractional.substr(0, 8) == "2023-11-");
    REQUIRE(fractional.substr(19) == ".5");
    REQUIRE(one(ex, "SELECT FROM_UNIXTIME(NULL)") == "NULL");
}

TEST_CASE("R71: a DATE column meets a date string or a DATETIME as the dates they are", "[r71][date]") {
    TempDataDir dir("r71_date_compare");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE dd (id INT PRIMARY KEY, d DATE, t DATETIME)");
    ok(ex, "INSERT INTO dd VALUES (1, '2024-01-31', '2024-01-31 00:00:00'), (2, '2024-02-29', '2024-02-29 12:00:00')");
    REQUIRE(rows_of(ex, "SELECT id FROM dd WHERE d = '2024-1-31'") == Rows{{"1"}});
    REQUIRE(rows_of(ex, "SELECT id FROM dd WHERE d < '2024-2-1'") == Rows{{"1"}});
    REQUIRE(rows_of(ex, "SELECT id FROM dd WHERE d >= '2024-2-1'") == Rows{{"2"}});
    REQUIRE(rows_of(ex, "SELECT id FROM dd WHERE d = '2024-02-29 00:00:00'") == Rows{{"2"}});     // (a DATE is a DATETIME at 00:00:00)
    REQUIRE(rows_of(ex, "SELECT id FROM dd WHERE d = '2024-02-29 12:00:00'").empty());
    REQUIRE(rows_of(ex, "SELECT id FROM dd WHERE t = d ORDER BY id") == Rows{{"1"}});
    REQUIRE(rows_of(ex, "SELECT id FROM dd WHERE t > d") == Rows{{"2"}});
    REQUIRE(rows_of(ex, "SELECT d = '2024-1-31', t = d FROM dd ORDER BY id") == Rows{{"1", "1"}, {"0", "0"}});
    // a number is still the number the date stands for
    REQUIRE(rows_of(ex, "SELECT id FROM dd WHERE d = 20240131") == Rows{{"1"}});
}
