#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// What a write may and may not leave behind. MySQL (strict mode) is the reference: a statement either succeeds or fails and, when it fails,
// changes nothing; NOT NULL, PRIMARY KEY, UNIQUE (any number of NULLs), CHECK (UNKNOWN passes), the type of every column and both sides of a
// FOREIGN KEY are enforced by INSERT, REPLACE, INSERT ... ON DUPLICATE KEY UPDATE, UPDATE (also over a join) and MERGE alike; an empty string is a
// value, not NULL.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

using Rows = std::vector<std::vector<std::string>>;
const std::string N = "NULL";

void open_db(Executor& ex) {
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
}

void ok(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
}

// the error text of a statement that has to fail
std::string fails(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_err());
    return r.error();
}

Rows cells(const std::string& text) {
    Rows rows;
    std::istringstream in(text);
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

Rows q(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_ok());
    return cells(r.value());
}
} // namespace

TEST_CASE("an empty string is a value, not NULL", "[write_integrity][empty_string]") {
    TempDataDir dir("wi_empty");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE e (id INT PRIMARY KEY, s VARCHAR(5), u VARCHAR(5) UNIQUE, n VARCHAR(5) NOT NULL DEFAULT 'dd')");
    ok(ex, "INSERT INTO e VALUES (1, '', NULL, DEFAULT), (2, NULL, '', ''), (3, 'a', NULL, 'x')");
    // '' stays '' (it was stored as NULL), NULL stays NULL
    REQUIRE(q(ex, "SELECT id, s, u, n FROM e ORDER BY id") == Rows{{"1", "", N, "dd"}, {"2", N, "", ""}, {"3", "a", N, "x"}});
    REQUIRE(q(ex, "SELECT id FROM e WHERE s IS NULL") == Rows{{"2"}});
    REQUIRE(q(ex, "SELECT id FROM e WHERE s IS NOT NULL ORDER BY id") == Rows{{"1"}, {"3"}});
    REQUIRE(q(ex, "SELECT id FROM e WHERE s = ''") == Rows{{"1"}});
    REQUIRE(q(ex, "SELECT id FROM e WHERE s <> '' ORDER BY id") == Rows{{"3"}});
    REQUIRE(q(ex, "SELECT id, COALESCE(s, 'z'), IFNULL(s, 'z'), ISNULL(s) FROM e ORDER BY id") ==
            Rows{{"1", "", "", "0"}, {"2", "z", "z", "1"}, {"3", "a", "a", "0"}});
    REQUIRE(q(ex, "SELECT COUNT(*), COUNT(s), COUNT(u) FROM e") == Rows{{"3", "2", "1"}});
    // GROUP BY keeps '' and NULL apart; a join matches '' with '' but never NULL with NULL
    REQUIRE(q(ex, "SELECT s, COUNT(*) FROM e GROUP BY s ORDER BY s") == Rows{{N, "1"}, {"", "1"}, {"a", "1"}});
    ok(ex, "CREATE TABLE f (id INT PRIMARY KEY, s VARCHAR(5))");
    ok(ex, "INSERT INTO f VALUES (1, ''), (2, NULL)");
    REQUIRE(q(ex, "SELECT e.id, f.id FROM e JOIN f ON e.s = f.s ORDER BY e.id") == Rows{{"1", "1"}});
    // UNIQUE: '' is a value (a second one is a duplicate), NULLs are any number
    REQUIRE(fails(ex, "INSERT INTO e VALUES (4, 'b', '', 'y')").find("Duplicate value") != std::string::npos);
    ok(ex, "INSERT INTO e VALUES (5, 'b', NULL, 'y'), (6, 'c', NULL, 'z')");
    ok(ex, "INSERT INTO e (id) VALUES (7)");
    // a column that is left out takes its default, or NULL; DEFAULT and an empty slot say the same
    REQUIRE(q(ex, "SELECT id, s, u, n FROM e WHERE id = 7") == Rows{{"7", N, N, "dd"}});
    ok(ex, "INSERT INTO e VALUES (8, , 'q', DEFAULT)");
    REQUIRE(q(ex, "SELECT id, s, u, n FROM e WHERE id = 8") == Rows{{"8", N, "q", "dd"}});
    // NOT NULL accepts '' but not NULL
    ok(ex, "INSERT INTO e VALUES (9, 'a', 'w', '')");
    REQUIRE(fails(ex, "INSERT INTO e VALUES (10, 'a', 'v', NULL)").find("cannot be NULL") != std::string::npos);
    // UPDATE and INSERT ... SELECT carry '' as well
    ok(ex, "UPDATE e SET s = '' WHERE id = 3");
    REQUIRE(q(ex, "SELECT s FROM e WHERE id = 3") == Rows{{""}});
    ok(ex, "CREATE TABLE g (id INT PRIMARY KEY, s VARCHAR(5))");
    ok(ex, "INSERT INTO g SELECT id, s FROM e WHERE id <= 3");
    REQUIRE(q(ex, "SELECT id, s FROM g ORDER BY id") == Rows{{"1", ""}, {"2", N}, {"3", ""}});
}

namespace {
struct TypeCase {
    std::string type, literal, stored; // stored: the text a SELECT shows; or, when `error` is set, a part of the message
    bool error;
};
} // namespace

TEST_CASE("a value has to fit its column and is stored in the column's own form", "[write_integrity][types]") {
    TempDataDir dir("wi_types");
    Executor ex(dir.path);
    open_db(ex);
    const std::vector<TypeCase> cases = {
        {"INT", "7", "7", false}, {"INT", "'7'", "7", false}, {"INT", "7.9", "8", false}, {"INT", "-7.5", "-8", false}, {"INT", "'0042'", "42", false},
        {"INT", "' 8 '", "8", false}, {"INT", "TRUE", "1", false}, {"INT", "-2147483648", "-2147483648", false},
        {"INT", "'abc'", "Incorrect integer value", true}, {"INT", "'12abc'", "Incorrect integer value", true}, {"INT", "''", "Incorrect integer value", true},
        {"INT", "2147483648", "Out of range", true},
        {"TINYINT", "127", "127", false}, {"TINYINT", "128", "Out of range", true}, {"TINYINT", "-128", "-128", false}, {"TINYINT", "-129", "Out of range", true},
        {"BIGINT", "9223372036854775807", "9223372036854775807", false}, {"BIGINT", "9223372036854775808", "Out of range", true},
        {"DOUBLE", "'1.50'", "1.5", false}, {"DOUBLE", "3", "3", false}, {"DOUBLE", "1.0", "1", false}, {"DOUBLE", "'x'", "Incorrect double value", true},
        {"FLOAT", "'1.5'", "1.5", false}, {"FLOAT", "3", "3", false},
        {"DECIMAL(5,2)", "1.256", "1.26", false}, {"DECIMAL(5,2)", "1.255", "1.26", false}, {"DECIMAL(5,2)", "5", "5.00", false},
        {"DECIMAL(5,2)", "-0.001", "0.00", false}, {"DECIMAL(5,2)", "999.99", "999.99", false}, {"DECIMAL(5,2)", "'7'", "7.00", false},
        {"DECIMAL(5,2)", "999.995", "Out of range", true}, {"DECIMAL(5,2)", "1000", "Out of range", true},
        {"DECIMAL(5,2)", "'x'", "Incorrect decimal value", true}, {"DECIMAL(5,2)", "'12abc'", "Incorrect decimal value", true},
        {"VARCHAR(3)", "'abc'", "abc", false}, {"VARCHAR(3)", "'abc  '", "abc", false}, {"VARCHAR(3)", "'abcd'", "Data too long", true},
        {"VARCHAR(3)", "'가나다'", "가나다", false}, {"VARCHAR(3)", "'가나다라'", "Data too long", true},
        {"DATE", "'2024-02-29'", "2024-02-29", false}, {"DATE", "'20240315'", "2024-03-15", false}, {"DATE", "'2024-1-5'", "2024-01-05", false},
        {"DATE", "'2024-03-15 10:00:00'", "2024-03-15", false}, {"DATE", "'2023-02-29'", "Incorrect date value", true},
        {"DATE", "'not a date'", "Incorrect date value", true}, {"DATE", "'2024-13-01'", "Incorrect date value", true},
        {"DATE", "'0000-00-00'", "Incorrect date value", true},
        {"DATETIME", "'2024-03-15'", "2024-03-15 00:00:00", false}, {"DATETIME", "'2024-3-5 1:2:3'", "2024-03-05 01:02:03", false},
        {"DATETIME", "'2024-03-15 25:00:00'", "Incorrect datetime value", true},
        {"TIME", "'12:30:45'", "12:30:45", false}, {"TIME", "'838:59:59'", "838:59:59", false}, {"TIME", "'25:61:00'", "Incorrect time value", true},
        {"YEAR", "2024", "2024", false}, {"YEAR", "1800", "Out of range", true},
        {"BOOLEAN", "TRUE", "1", false}, {"BOOLEAN", "FALSE", "0", false}, {"BOOLEAN", "1", "1", false}, {"BOOLEAN", "'maybe'", "Incorrect integer value", true},
        {"JSON", "'{\"a\": 1}'", "{\"a\": 1}", false}, {"JSON", "'{bad'", "Invalid JSON text", true},
    };
    int n = 0;
    for (const TypeCase& c : cases) {
        const std::string table = "ty" + std::to_string(n++);
        INFO(c.type << " <- " << c.literal);
        ok(ex, "CREATE TABLE " + table + " (id INT PRIMARY KEY AUTO_INCREMENT, v " + c.type + ")");
        ok(ex, "INSERT INTO " + table + " (v) VALUES (NULL)"); // a row for UPDATE to rewrite
        if (!c.error) {
            ok(ex, "INSERT INTO " + table + " (v) VALUES (" + c.literal + ")");
            REQUIRE(q(ex, "SELECT v FROM " + table + " WHERE id = 2") == Rows{{c.stored}});
            ok(ex, "UPDATE " + table + " SET v = " + c.literal + " WHERE id = 1");
            REQUIRE(q(ex, "SELECT v FROM " + table + " WHERE id = 1") == Rows{{c.stored}});
        } else {
            REQUIRE(fails(ex, "INSERT INTO " + table + " (v) VALUES (" + c.literal + ")").find(c.stored) != std::string::npos);
            REQUIRE(fails(ex, "UPDATE " + table + " SET v = " + c.literal + " WHERE id = 1").find(c.stored) != std::string::npos);
            // nothing was written: a statement with one bad row inserts none of its rows
            REQUIRE(fails(ex, "INSERT INTO " + table + " (v) VALUES (NULL), (" + c.literal + ")").find(c.stored) != std::string::npos);
            REQUIRE(q(ex, "SELECT id, v FROM " + table) == Rows{{"1", N}});
        }
    }
    // the position of the offending row is in the message, and a multi-row UPDATE is checked as a whole
    ok(ex, "CREATE TABLE m (id INT PRIMARY KEY, v INT, w VARCHAR(2))");
    ok(ex, "INSERT INTO m VALUES (1, 1, 'a'), (2, 2, 'bb'), (3, 3, 'c')");
    REQUIRE(fails(ex, "INSERT INTO m VALUES (4, 4, 'a'), (5, 'x', 'b')").find("at row 2") != std::string::npos);
    REQUIRE(fails(ex, "UPDATE m SET w = CONCAT(w, 'zz') WHERE id >= 1").find("Data too long") != std::string::npos);
    REQUIRE(q(ex, "SELECT id, w FROM m ORDER BY id") == Rows{{"1", "a"}, {"2", "bb"}, {"3", "c"}});
    // two spellings of one number are one key
    REQUIRE(fails(ex, "INSERT INTO m VALUES ('01', 9, 'z')").find("Duplicate value") != std::string::npos);
}

TEST_CASE("UPDATE checks NOT NULL and both sides of a foreign key, and a failed UPDATE changes nothing", "[write_integrity][update]") {
    TempDataDir dir("wi_update");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE p (id INT PRIMARY KEY, a INT NOT NULL, u INT UNIQUE)");
    ok(ex, "CREATE TABLE c (cid INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p(id))");
    ok(ex, "INSERT INTO p VALUES (1, 10, 100), (2, 20, 200), (3, 30, 300)");
    ok(ex, "INSERT INTO c VALUES (1, 2), (2, NULL)");
    const Rows p_before = q(ex, "SELECT id, a, u FROM p ORDER BY id");

    // NOT NULL: NULL itself, and an expression that is NULL (x / 0, NULL arithmetic)
    REQUIRE(fails(ex, "UPDATE p SET a = NULL WHERE id = 1").find("cannot be NULL") != std::string::npos);
    REQUIRE(fails(ex, "UPDATE p SET a = a / 0 WHERE id = 1").find("cannot be NULL") != std::string::npos);
    REQUIRE(fails(ex, "UPDATE p SET id = NULL WHERE id = 1").find("cannot be NULL") != std::string::npos);
    // a statement over several rows is all or nothing: the first row would pass, the second does not
    REQUIRE(fails(ex, "UPDATE p SET a = 100 / (id - 2) WHERE id <= 3").find("cannot be NULL") != std::string::npos);
    REQUIRE(q(ex, "SELECT id, a, u FROM p ORDER BY id") == p_before);
    // UNIQUE over the statement's whole result: a chain may shift, a repeat may not
    ok(ex, "UPDATE p SET u = u + 100");
    REQUIRE(q(ex, "SELECT u FROM p ORDER BY id") == Rows{{"200"}, {"300"}, {"400"}});
    REQUIRE(fails(ex, "UPDATE p SET u = 5 WHERE id <= 2").find("Duplicate value") != std::string::npos);
    REQUIRE(q(ex, "SELECT u FROM p ORDER BY id") == Rows{{"200"}, {"300"}, {"400"}});
    ok(ex, "UPDATE p SET u = u - 100");

    // the child side: a value must name a parent, NULL is fine, a value the statement leaves alone is not checked again
    REQUIRE(fails(ex, "UPDATE c SET pid = 99 WHERE cid = 1").find("Foreign key violation") != std::string::npos);
    REQUIRE(fails(ex, "UPDATE c SET pid = pid + 5").find("Foreign key violation") != std::string::npos); // 2 -> 7: no such parent (NULL + 5 is NULL: fine)
    REQUIRE(q(ex, "SELECT cid, pid FROM c ORDER BY cid") == Rows{{"1", "2"}, {"2", N}});
    ok(ex, "UPDATE c SET pid = 3 WHERE cid = 1");
    ok(ex, "UPDATE c SET pid = NULL WHERE cid = 1");
    REQUIRE(q(ex, "SELECT cid, pid FROM c ORDER BY cid") == Rows{{"1", N}, {"2", N}});
    // the parent side: a referenced key can neither be changed (ON UPDATE RESTRICT) nor deleted -- and nothing changes when it fails
    ok(ex, "UPDATE c SET pid = 2 WHERE cid = 1");
    REQUIRE(fails(ex, "UPDATE p SET id = 22 WHERE id = 2").find("Foreign key violation") != std::string::npos);
    REQUIRE(q(ex, "SELECT id FROM p ORDER BY id") == Rows{{"1"}, {"2"}, {"3"}});
    ok(ex, "UPDATE p SET id = 11 WHERE id = 1"); // not referenced
    REQUIRE(q(ex, "SELECT id FROM p ORDER BY id") == Rows{{"2"}, {"3"}, {"11"}});
    ok(ex, "UPDATE p SET id = id WHERE id = 2");    // not a change
    // the old version of a changed key stays in the table until a vacuum, and is no parent either
    REQUIRE(fails(ex, "INSERT INTO c VALUES (3, 1)").find("Foreign key violation") != std::string::npos);
    REQUIRE(fails(ex, "UPDATE c SET pid = 1 WHERE cid = 2").find("Foreign key violation") != std::string::npos);
    ok(ex, "INSERT INTO c VALUES (3, 11)");
    ok(ex, "DELETE FROM c WHERE cid = 3");
    // a deleted parent is no parent (INSERT used to accept a child of a row that was already deleted)
    ok(ex, "DELETE FROM p WHERE id = 3");
    REQUIRE(fails(ex, "INSERT INTO c VALUES (3, 3)").find("Foreign key violation") != std::string::npos);
    REQUIRE(fails(ex, "UPDATE c SET pid = 3 WHERE cid = 2").find("Foreign key violation") != std::string::npos);
    // ... also when the delete is not committed yet (the row stays in the table, marked as deleted by the transaction)
    ok(ex, "BEGIN");
    ok(ex, "DELETE FROM p WHERE id = 11");
    REQUIRE(fails(ex, "INSERT INTO c VALUES (3, 11)").find("Foreign key violation") != std::string::npos);
    ok(ex, "ROLLBACK");
    ok(ex, "INSERT INTO c VALUES (3, 11)");
    ok(ex, "DELETE FROM c WHERE cid = 3");
    // a self-referencing key is read while the table is rewritten
    ok(ex, "CREATE TABLE tree (id INT PRIMARY KEY, up INT, FOREIGN KEY (up) REFERENCES tree(id))");
    ok(ex, "INSERT INTO tree VALUES (1, NULL), (2, 1), (3, 2)");
    ok(ex, "UPDATE tree SET up = 1 WHERE id = 3");
    REQUIRE(fails(ex, "UPDATE tree SET up = 9 WHERE id = 3").find("Foreign key violation") != std::string::npos);
}

TEST_CASE("REPLACE that fails changes nothing; one that works deletes every row it conflicts with", "[write_integrity][replace]") {
    TempDataDir dir("wi_replace");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE p (id INT PRIMARY KEY, a INT NOT NULL CHECK (a < 100), u INT UNIQUE, s VARCHAR(3))");
    ok(ex, "CREATE TABLE kid_cascade (kid INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p(id) ON DELETE CASCADE)");
    ok(ex, "INSERT INTO p VALUES (1, 10, 100, 'a'), (2, 20, 200, 'b'), (3, 30, 300, 'c')");
    ok(ex, "INSERT INTO kid_cascade VALUES (11, 1), (12, 1), (13, 2)");
    const Rows p_before = q(ex, "SELECT id, a, u, s FROM p ORDER BY id");
    const Rows kids_before = q(ex, "SELECT kid, pid FROM kid_cascade ORDER BY kid");
    // every way the new row can be refused: the old row (and the children a delete would cascade away) stays
    REQUIRE(fails(ex, "REPLACE INTO p VALUES (1, NULL, 101, 'x')").find("cannot be NULL") != std::string::npos);        // NOT NULL
    REQUIRE(fails(ex, "REPLACE INTO p VALUES (1, 500, 101, 'x')").find("CHECK constraint") != std::string::npos);        // CHECK
    REQUIRE(fails(ex, "REPLACE INTO p VALUES (1, 'q', 101, 'x')").find("Incorrect integer value") != std::string::npos); // type
    REQUIRE(fails(ex, "REPLACE INTO p VALUES (1, 5, 101, 'toolong')").find("Data too long") != std::string::npos);       // type
    REQUIRE(fails(ex, "REPLACE INTO p VALUES (2, 5, 100, 'x'), (1, NULL, 1, 'y')").find("cannot be NULL") != std::string::npos); // 2nd row bad
    REQUIRE(q(ex, "SELECT id, a, u, s FROM p ORDER BY id") == p_before);
    REQUIRE(q(ex, "SELECT kid, pid FROM kid_cascade ORDER BY kid") == kids_before);
    // a row that conflicts on the PRIMARY KEY and on a UNIQUE column of ANOTHER row deletes both
    ok(ex, "REPLACE INTO p VALUES (1, 15, 200, 'z')");
    REQUIRE(q(ex, "SELECT id, a, u, s FROM p ORDER BY id") == Rows{{"1", "15", "200", "z"}, {"3", "30", "300", "c"}});
    REQUIRE(q(ex, "SELECT kid FROM kid_cascade ORDER BY kid") == Rows{}); // the cascade of the two deletes
    // a later row of the statement replaces an earlier one: the last one wins; each row still deletes what it conflicts with
    ok(ex, "INSERT INTO p VALUES (4, 40, 400, 'd')");
    ok(ex, "REPLACE INTO p VALUES (4, 41, 401, 'e'), (5, 50, 401, 'f')"); // 5 replaces 4 through u = 401
    REQUIRE(q(ex, "SELECT id, a, u FROM p ORDER BY id") == Rows{{"1", "15", "200"}, {"3", "30", "300"}, {"5", "50", "401"}});
    ok(ex, "REPLACE INTO p VALUES (3, 31, 7, 'g'), (6, 60, 7, 'h')"); // row 1 replaces the old 3; row 2 replaces row 1 through u = 7
    REQUIRE(q(ex, "SELECT id, a, u FROM p ORDER BY id") == Rows{{"1", "15", "200"}, {"5", "50", "401"}, {"6", "60", "7"}});
    // a child that RESTRICTs the delete refuses the REPLACE as a whole, before anything is deleted
    ok(ex, "CREATE TABLE kid_restrict (kid INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p(id))");
    ok(ex, "INSERT INTO kid_restrict VALUES (1, 5)");
    REQUIRE(fails(ex, "REPLACE INTO p VALUES (6, 61, 8, 'i'), (5, 51, 402, 'j')").find("Foreign key violation") != std::string::npos);
    REQUIRE(fails(ex, "REPLACE INTO p VALUES (7, 70, 401, 'k')").find("Foreign key violation") != std::string::npos); // conflicts on u with row 5
    REQUIRE(q(ex, "SELECT id, a, u FROM p ORDER BY id") == Rows{{"1", "15", "200"}, {"5", "50", "401"}, {"6", "60", "7"}});
    // inside a transaction a REPLACE is undone by ROLLBACK
    ok(ex, "BEGIN");
    ok(ex, "REPLACE INTO p VALUES (6, 66, 77, 'r')");
    REQUIRE(q(ex, "SELECT a FROM p WHERE id = 6") == Rows{{"66"}});
    ok(ex, "ROLLBACK");
    REQUIRE(q(ex, "SELECT id, a, u FROM p WHERE id = 6") == Rows{{"6", "60", "7"}});
    // a composite primary key
    ok(ex, "CREATE TABLE cp (a INT, b INT, v INT NOT NULL, PRIMARY KEY (a, b))");
    ok(ex, "INSERT INTO cp VALUES (1, 1, 10), (1, 2, 20)");
    REQUIRE(fails(ex, "REPLACE INTO cp VALUES (1, 1, NULL)").find("cannot be NULL") != std::string::npos);
    ok(ex, "REPLACE INTO cp VALUES (1, 1, 11), (1, 1, 12)");
    REQUIRE(q(ex, "SELECT a, b, v FROM cp ORDER BY a, b") == Rows{{"1", "1", "12"}, {"1", "2", "20"}});
}


TEST_CASE("INSERT ... ON DUPLICATE KEY UPDATE is an UPDATE of the conflicting row", "[write_integrity][odku]") {
    TempDataDir dir("wi_odku");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE p (id INT PRIMARY KEY, a INT NOT NULL CHECK (a < 100), u INT UNIQUE, s VARCHAR(3))");
    ok(ex, "CREATE TABLE ref (id INT PRIMARY KEY, f INT, FOREIGN KEY (f) REFERENCES p(id))");
    ok(ex, "INSERT INTO p VALUES (1, 10, 100, 'a'), (2, 20, 200, 'b')");
    const Rows before = q(ex, "SELECT id, a, u, s FROM p ORDER BY id");
    // the constraints of an UPDATE apply, and a refused statement changes nothing
    REQUIRE(fails(ex, "INSERT INTO p VALUES (1, 1, 1, 'x') ON DUPLICATE KEY UPDATE a = NULL").find("cannot be NULL") != std::string::npos);
    REQUIRE(fails(ex, "INSERT INTO p VALUES (1, 1, 1, 'x') ON DUPLICATE KEY UPDATE a = a * 10").find("CHECK constraint") != std::string::npos);
    REQUIRE(fails(ex, "INSERT INTO p VALUES (1, 1, 1, 'x') ON DUPLICATE KEY UPDATE u = 200").find("Duplicate value") != std::string::npos);
    REQUIRE(fails(ex, "INSERT INTO p VALUES (1, 1, 1, 'x') ON DUPLICATE KEY UPDATE s = 'toolong'").find("Data too long") != std::string::npos);
    REQUIRE(fails(ex, "INSERT INTO p VALUES (1, 1, 1, 'x') ON DUPLICATE KEY UPDATE id = 2").find("Duplicate value") != std::string::npos);
    REQUIRE(fails(ex, "INSERT INTO p VALUES (3, NULL, 3, 'x') ON DUPLICATE KEY UPDATE a = 1").find("cannot be NULL") != std::string::npos); // the new row itself
    REQUIRE(q(ex, "SELECT id, a, u, s FROM p ORDER BY id") == before);
    // VALUES(col) is the value the statement was going to insert; the row it hits may be found through UNIQUE as well as the key
    ok(ex, "INSERT INTO p VALUES (1, 11, 101, 'n') ON DUPLICATE KEY UPDATE a = VALUES(a) + 1, s = VALUES(s)");
    REQUIRE(q(ex, "SELECT id, a, u, s FROM p WHERE id = 1") == Rows{{"1", "12", "100", "n"}});
    ok(ex, "INSERT INTO p VALUES (9, 50, 200, 'u') ON DUPLICATE KEY UPDATE a = a + VALUES(a)"); // u = 200 is row 2's: 20 + 50
    REQUIRE(q(ex, "SELECT id, a, u, s FROM p ORDER BY id") == Rows{{"1", "12", "100", "n"}, {"2", "70", "200", "b"}});
    // several rows: the ones that conflict update, the others insert; the message counts both
    auto both = ex.execute_sql("INSERT INTO p VALUES (1, 1, 1, 'x'), (5, 50, 500, 'y') ON DUPLICATE KEY UPDATE a = a + 1");
    REQUIRE(both.is_ok());
    REQUIRE(both.value() == "1 row(s) inserted, 1 row(s) updated.");
    REQUIRE(q(ex, "SELECT id, a FROM p ORDER BY id") == Rows{{"1", "13"}, {"2", "70"}, {"5", "50"}});
    // foreign keys: the child side of an assigned column is checked
    ok(ex, "INSERT INTO ref VALUES (1, 1)");
    REQUIRE(fails(ex, "INSERT INTO ref VALUES (1, 1) ON DUPLICATE KEY UPDATE f = 99").find("Foreign key violation") != std::string::npos);
    REQUIRE(q(ex, "SELECT id, f FROM ref") == Rows{{"1", "1"}});
    // inside a transaction the update is undone by ROLLBACK (it was written in place, without an undo entry)
    ok(ex, "BEGIN");
    ok(ex, "INSERT INTO p VALUES (5, 1, 1, 'x') ON DUPLICATE KEY UPDATE a = 77");
    REQUIRE(q(ex, "SELECT a FROM p WHERE id = 5") == Rows{{"77"}});
    ok(ex, "ROLLBACK");
    REQUIRE(q(ex, "SELECT a FROM p WHERE id = 5") == Rows{{"50"}});
    // a composite key: only the row that matches every column is rewritten
    ok(ex, "CREATE TABLE cp (a INT, b INT, v INT, PRIMARY KEY (a, b))");
    ok(ex, "INSERT INTO cp VALUES (1, 1, 10), (1, 2, 20)");
    ok(ex, "INSERT INTO cp VALUES (1, 2, 0) ON DUPLICATE KEY UPDATE v = v + 1");
    REQUIRE(q(ex, "SELECT a, b, v FROM cp ORDER BY a, b") == Rows{{"1", "1", "10"}, {"1", "2", "21"}});
}

TEST_CASE("three-valued logic: a comparison with NULL is UNKNOWN, and only TRUE keeps a row", "[write_integrity][three_valued]") {
    TempDataDir dir("wi_tri");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE j (id INT PRIMARY KEY, x INT, y INT)");
    ok(ex, "INSERT INTO j VALUES (1, NULL, 1), (2, 5, 2), (3, 0, NULL), (4, 7, 7)");
    auto ids = [&](const std::string& where) {
        Rows rows = q(ex, "SELECT id FROM j WHERE " + where + " ORDER BY id");
        std::string out;
        for (auto& r : rows) out += (out.empty() ? "" : ",") + r[0];
        return out;
    };
    REQUIRE(ids("x > 1") == "2,4");
    REQUIRE(ids("NOT (x > 1)") == "3");      // NOT UNKNOWN is UNKNOWN: row 1 (x NULL) is not kept (it was)
    REQUIRE(ids("x <= 1") == "3");
    REQUIRE(ids("NOT (x <= 1)") == "2,4");
    REQUIRE(ids("x = x") == "2,3,4");
    REQUIRE(ids("x <> 5") == "3,4");
    REQUIRE(ids("NOT (x = 5)") == "3,4");
    // AND / OR (Kleene): FALSE AND UNKNOWN is FALSE, TRUE OR UNKNOWN is TRUE
    REQUIRE(ids("x > 1 OR y = 1") == "1,2,4");
    REQUIRE(ids("NOT (x > 1 OR y = 1)") == "");           // row 3: FALSE OR UNKNOWN is UNKNOWN, and so is its NOT
    REQUIRE(ids("x > 100 AND y > 0") == "");
    REQUIRE(ids("NOT (x > 100 AND y > 0)") == "2,3,4");  // row 1: UNKNOWN AND TRUE is UNKNOWN; the others are FALSE

    // UPDATE, DELETE and a join's ON clause evaluate the same conditions on their own (the SELECT above reads them with subqueries in mind)
    ok(ex, "CREATE TABLE j2 (id INT PRIMARY KEY, x INT, y INT)");
    ok(ex, "INSERT INTO j2 VALUES (1, NULL, 1), (2, 5, 2), (3, 0, NULL), (4, 7, 7)");
    auto j2 = [&] {
        std::string out;
        for (auto& r : q(ex, "SELECT id, y FROM j2 ORDER BY id")) out += (out.empty() ? "" : ",") + r[0] + ":" + r[1];
        return out;
    };
    ok(ex, "UPDATE j2 SET y = 100 WHERE NOT (x > 1)");              // row 3 only: x NULL (row 1) is UNKNOWN
    REQUIRE(j2() == "1:1,2:2,3:100,4:7");
    ok(ex, "UPDATE j2 SET y = 200 WHERE NOT (x > 1 OR y = 1)");     // row 1: UNKNOWN OR TRUE is TRUE; row 3: FALSE OR FALSE (y is 100 now)
    REQUIRE(j2() == "1:1,2:2,3:200,4:7");
    ok(ex, "UPDATE j2 SET y = NULL WHERE id = 3");
    ok(ex, "UPDATE j2 SET y = 300 WHERE NOT (x > 1 OR y = 1)");     // row 3: FALSE OR UNKNOWN is UNKNOWN, and so is its NOT
    REQUIRE(j2() == "1:1,2:2,3:NULL,4:7");
    ok(ex, "UPDATE j2 SET y = 400 WHERE NOT (x > 100 AND y > 0)"); // row 1: UNKNOWN AND TRUE is UNKNOWN; the others have a FALSE
    REQUIRE(j2() == "1:1,2:400,3:400,4:400");
    ok(ex, "DELETE FROM j2 WHERE NOT (x <= 1)");                    // x = 5 and 7; not the NULL
    REQUIRE(j2() == "1:1,3:400");
    // ON: the pairs of (x, y) that p = `a.x > b.x` and q = `a.y < b.y` select, each of them TRUE, FALSE or UNKNOWN (NULL)
    ok(ex, "CREATE TABLE jb (id INT PRIMARY KEY, x INT, y INT)");
    ok(ex, "INSERT INTO jb VALUES (1, NULL, 1), (2, 5, 2), (3, 0, NULL), (4, 7, 7)");
    const std::vector<std::pair<std::optional<int>, std::optional<int>>> xy = {{std::nullopt, 1}, {5, 2}, {0, std::nullopt}, {7, 7}};
    enum class T3 { F, T, U };
    auto p3 = [&](std::size_t a, std::size_t b) { return !xy[a].first || !xy[b].first ? T3::U : *xy[a].first > *xy[b].first ? T3::T : T3::F; };
    auto q3 = [&](std::size_t a, std::size_t b) { return !xy[a].second || !xy[b].second ? T3::U : *xy[a].second < *xy[b].second ? T3::T : T3::F; };
    auto pairs_where = [&](const std::string& on, auto keep) {
        Rows expected;
        for (std::size_t a = 0; a < xy.size(); a++)
            for (std::size_t b = 0; b < xy.size(); b++)
                if (keep(p3(a, b), q3(a, b))) expected.push_back({std::to_string(a + 1), std::to_string(b + 1)});
        REQUIRE(q(ex, "SELECT a.id, b.id FROM j a JOIN jb b ON " + on + " ORDER BY a.id, b.id") == expected);
    };
    pairs_where("a.x > b.x OR a.y < b.y", [](T3 p, T3 qq) { return p == T3::T || qq == T3::T; });
    pairs_where("a.x > b.x AND a.y < b.y", [](T3 p, T3 qq) { return p == T3::T && qq == T3::T; });
    pairs_where("NOT (a.x > b.x)", [](T3 p, T3) { return p == T3::F; });
    pairs_where("NOT (a.x > b.x OR a.y < b.y)", [](T3 p, T3 qq) { return p == T3::F && qq == T3::F; });
    pairs_where("NOT (a.x > b.x AND a.y < b.y)", [](T3 p, T3 qq) { return p == T3::F || qq == T3::F; });
}

TEST_CASE("IN, NOT IN, BETWEEN, LIKE and CHECK treat NULL as UNKNOWN", "[write_integrity][three_valued]") {
    TempDataDir dir("wi_tri2");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE j (id INT PRIMARY KEY, x INT, s VARCHAR(5))");
    ok(ex, "INSERT INTO j VALUES (1, NULL, NULL), (2, 5, 'ab'), (3, 0, 'xy'), (4, 7, 'abc')");
    auto ids = [&](const std::string& where) {
        Rows rows = q(ex, "SELECT id FROM j WHERE " + where + " ORDER BY id");
        std::string out;
        for (auto& r : rows) out += (out.empty() ? "" : ",") + r[0];
        return out;
    };
    REQUIRE(ids("x IN (5, 7)") == "2,4");
    REQUIRE(ids("x NOT IN (5, 7)") == "3");               // the NULL row is UNKNOWN, not "not in"
    REQUIRE(ids("x IN (5, NULL)") == "2");                // a match is TRUE, a miss is UNKNOWN
    REQUIRE(ids("x NOT IN (5, NULL)") == "");             // a match is FALSE, a miss is UNKNOWN: nothing is TRUE
    REQUIRE(ids("x BETWEEN 1 AND 6") == "2");
    REQUIRE(ids("x NOT BETWEEN 1 AND 6") == "3,4");
    REQUIRE(ids("s LIKE 'ab%'") == "2,4");
    REQUIRE(ids("s NOT LIKE 'ab%'") == "3");
    // a subquery's NULL: IN is TRUE or UNKNOWN, NOT IN is FALSE or UNKNOWN
    ok(ex, "CREATE TABLE k (v INT)");
    ok(ex, "INSERT INTO k VALUES (5), (NULL)");
    REQUIRE(ids("x IN (SELECT v FROM k)") == "2");
    REQUIRE(ids("x NOT IN (SELECT v FROM k)") == "");
    REQUIRE(ids("x NOT IN (SELECT v FROM k WHERE v IS NOT NULL)") == "3,4");
    // a scalar subquery with no row is NULL
    REQUIRE(ids("x > (SELECT v FROM k WHERE v > 100)") == "");
    REQUIRE(ids("NOT (x > (SELECT v FROM k WHERE v > 100))") == "");
    REQUIRE(ids("x <> (SELECT v FROM k WHERE v = 5)") == "3,4");
    // CHECK is violated only by FALSE: a NULL operand passes
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, k INT CHECK (k > 0), CONSTRAINT lim CHECK (id < 100))");
    ok(ex, "INSERT INTO c VALUES (1, NULL), (2, 5)");
    REQUIRE(fails(ex, "INSERT INTO c VALUES (3, 0)").find("CHECK constraint") != std::string::npos);
    ok(ex, "UPDATE c SET k = NULL WHERE id = 2");
    REQUIRE(fails(ex, "UPDATE c SET k = -1 WHERE id = 2").find("CHECK constraint") != std::string::npos);
    REQUIRE(fails(ex, "INSERT INTO c VALUES (100, 1)").find("lim") != std::string::npos);
    REQUIRE(q(ex, "SELECT id, k FROM c ORDER BY id") == Rows{{"1", N}, {"2", N}});
}

TEST_CASE("NULL sorts first in ASC and last in DESC", "[write_integrity][order]") {
    TempDataDir dir("wi_order");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE o (id INT PRIMARY KEY, s VARCHAR(10), n INT)");
    ok(ex, "INSERT INTO o VALUES (1, 'Alice', 5), (2, NULL, NULL), (3, 'Zed', -3), (4, 'bob', 10), (5, '', 7)");
    auto ids = [&](const std::string& tail) {
        Rows rows = q(ex, "SELECT id FROM o " + tail);
        std::string out;
        for (auto& r : rows) out += (out.empty() ? "" : ",") + r[0];
        return out;
    };
    REQUIRE(ids("ORDER BY n") == "2,3,1,5,4");          // NULL, -3, 5, 7, 10
    REQUIRE(ids("ORDER BY n DESC") == "4,5,1,3,2");
    REQUIRE(ids("ORDER BY s") == "2,5,1,3,4");          // NULL, '', 'Alice', 'Zed', 'bob' (byte order: this engine has no case-insensitive collation)
    REQUIRE(ids("ORDER BY s DESC") == "4,3,1,5,2");
    REQUIRE(ids("ORDER BY n, s DESC") == "2,3,1,5,4");
    REQUIRE(q(ex, "SELECT DISTINCT s FROM o ORDER BY s") == Rows{{N}, {""}, {"Alice"}, {"Zed"}, {"bob"}});
    REQUIRE(q(ex, "SELECT s, COUNT(*) FROM o GROUP BY s ORDER BY s LIMIT 2") == Rows{{N, "1"}, {"", "1"}});
    REQUIRE(q(ex, "SELECT id, ROW_NUMBER() OVER (ORDER BY n) FROM o ORDER BY id") ==
            Rows{{"1", "3"}, {"2", "1"}, {"3", "2"}, {"4", "5"}, {"5", "4"}});
}

TEST_CASE("multi-table UPDATE / DELETE and MERGE are as strict as the one-table statements", "[write_integrity][merge]") {
    TempDataDir dir("wi_merge");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, a INT NOT NULL, u INT UNIQUE)");
    ok(ex, "CREATE TABLE src (id INT PRIMARY KEY, a INT, u INT)");
    ok(ex, "CREATE TABLE ch (cid INT PRIMARY KEY, tid INT, FOREIGN KEY (tid) REFERENCES t(id))");
    ok(ex, "INSERT INTO t VALUES (1, 1, 10), (2, 2, 20), (3, 3, 30)");
    ok(ex, "INSERT INTO src VALUES (1, 100, 20), (2, NULL, 5), (3, 300, 30)");
    ok(ex, "INSERT INTO ch VALUES (1, 2)");
    const Rows t_before = q(ex, "SELECT id, a, u FROM t ORDER BY id");

    // UPDATE ... JOIN: NOT NULL, as a whole
    REQUIRE(fails(ex, "UPDATE t JOIN src ON t.id = src.id SET t.a = src.a").find("cannot be NULL") != std::string::npos);
    // UNIQUE over the whole result: u moves to 20, 5, 30 -- a chain, fine
    auto moved = ex.execute_sql("UPDATE t JOIN src ON t.id = src.id SET t.u = src.u");
    REQUIRE(moved.is_ok());
    REQUIRE(moved.value() == "3 row(s) updated.");
    REQUIRE(q(ex, "SELECT id, u FROM t ORDER BY id") == Rows{{"1", "20"}, {"2", "5"}, {"3", "30"}});
    REQUIRE(fails(ex, "UPDATE t JOIN src ON t.id = src.id SET t.u = 7").find("Duplicate value") != std::string::npos); // all three get 7
    REQUIRE(q(ex, "SELECT id, u FROM t ORDER BY id") == Rows{{"1", "20"}, {"2", "5"}, {"3", "30"}});
    ok(ex, "UPDATE t JOIN src ON t.id = src.id SET t.u = 10 + t.id");
    // ROLLBACK undoes it (it used to rewrite the rows in place)
    ok(ex, "BEGIN");
    ok(ex, "UPDATE t JOIN src ON t.id = src.id SET t.a = 777 WHERE src.id = 3");
    REQUIRE(q(ex, "SELECT a FROM t WHERE id = 3") == Rows{{"777"}});
    ok(ex, "ROLLBACK");
    REQUIRE(q(ex, "SELECT a FROM t WHERE id = 3") == Rows{{"3"}});

    // DELETE ... JOIN: a child that RESTRICTs refuses the whole statement; a free row goes, with its undo log
    REQUIRE(fails(ex, "DELETE t FROM t JOIN src ON t.id = src.id WHERE src.id >= 2").find("Foreign key violation") != std::string::npos);
    REQUIRE(q(ex, "SELECT id FROM t ORDER BY id") == Rows{{"1"}, {"2"}, {"3"}});
    ok(ex, "BEGIN");
    ok(ex, "DELETE t FROM t JOIN src ON t.id = src.id WHERE src.id = 3");
    REQUIRE(q(ex, "SELECT id FROM t ORDER BY id") == Rows{{"1"}, {"2"}});
    ok(ex, "ROLLBACK");
    REQUIRE(q(ex, "SELECT id FROM t ORDER BY id") == Rows{{"1"}, {"2"}, {"3"}});

    // MERGE: update, delete and insert branches; the inserted row takes its number from AUTO_INCREMENT's counter
    ok(ex, "CREATE TABLE m (id INT PRIMARY KEY AUTO_INCREMENT, code VARCHAR(5) UNIQUE, v INT NOT NULL)");
    ok(ex, "CREATE TABLE ms (code VARCHAR(5), v INT)");
    ok(ex, "INSERT INTO m VALUES (1, 'A', 1), (2, 'B', 2), (3, 'C', 3)");
    ok(ex, "INSERT INTO ms VALUES ('A', 10), ('B', NULL), ('D', 40), ('E', 50)");
    const Rows m_before = q(ex, "SELECT id, code, v FROM m ORDER BY id");
    // a NULL for a NOT NULL column in the update branch: nothing at all happens (not the other rows' updates, deletes or inserts either)
    REQUIRE(fails(ex, "MERGE INTO m USING ms ON m.code = ms.code WHEN MATCHED THEN UPDATE SET v = ms.v "
                      "WHEN NOT MATCHED THEN INSERT (code, v) VALUES (ms.code, ms.v)").find("cannot be NULL") != std::string::npos);
    REQUIRE(q(ex, "SELECT id, code, v FROM m ORDER BY id") == m_before);
    // ... and one in the insert branch is found before the update is written
    ok(ex, "UPDATE ms SET v = 20 WHERE code = 'B'");
    ok(ex, "INSERT INTO ms VALUES ('F', NULL)");
    REQUIRE(fails(ex, "MERGE INTO m USING ms ON m.code = ms.code WHEN MATCHED THEN UPDATE SET v = ms.v "
                      "WHEN NOT MATCHED THEN INSERT (code, v) VALUES (ms.code, ms.v)").find("cannot be NULL") != std::string::npos);
    REQUIRE(q(ex, "SELECT id, code, v FROM m ORDER BY id") == m_before);
    ok(ex, "DELETE FROM ms WHERE code = 'F'");
    auto merged = ex.execute_sql("MERGE INTO m USING ms ON m.code = ms.code WHEN MATCHED AND ms.v = 20 THEN DELETE "
                                 "WHEN MATCHED THEN UPDATE SET v = ms.v WHEN NOT MATCHED THEN INSERT (code, v) VALUES (ms.code, ms.v)");
    REQUIRE(merged.is_ok());
    REQUIRE(merged.value() == "MERGE: 1 updated, 1 deleted, 2 inserted.");
    REQUIRE(q(ex, "SELECT id, code, v FROM m ORDER BY id") == Rows{{"1", "A", "10"}, {"3", "C", "3"}, {"4", "D", "40"}, {"5", "E", "50"}});

    // a row the MERGE would delete that a child still references: the whole MERGE is refused, the other row's update included
    ok(ex, "CREATE TABLE mch (cid INT PRIMARY KEY, mid INT, FOREIGN KEY (mid) REFERENCES m(id))");
    ok(ex, "INSERT INTO mch VALUES (1, 3)");
    ok(ex, "CREATE TABLE ms2 (code VARCHAR(5), v INT)");
    ok(ex, "INSERT INTO ms2 VALUES ('A', 11), ('C', 0)");
    const Rows m_now = q(ex, "SELECT id, code, v FROM m ORDER BY id");
    REQUIRE(fails(ex, "MERGE INTO m USING ms2 ON m.code = ms2.code WHEN MATCHED AND ms2.v = 0 THEN DELETE "
                      "WHEN MATCHED THEN UPDATE SET v = ms2.v").find("Foreign key violation") != std::string::npos);
    REQUIRE(q(ex, "SELECT id, code, v FROM m ORDER BY id") == m_now);
    // the same MERGE without the referenced row goes through
    ok(ex, "DELETE FROM ms2 WHERE code = 'C'");
    ok(ex, "MERGE INTO m USING ms2 ON m.code = ms2.code WHEN MATCHED AND ms2.v = 0 THEN DELETE WHEN MATCHED THEN UPDATE SET v = ms2.v");
    REQUIRE(q(ex, "SELECT v FROM m WHERE code = 'A'") == Rows{{"11"}});
}

TEST_CASE("AUTO_INCREMENT follows the numbers inserted by hand, and survives a restart", "[write_integrity][auto_increment]") {
    TempDataDir dir("wi_autoinc");
    {
        Executor ex(dir.path);
        open_db(ex);
        ok(ex, "CREATE TABLE ai (id INT PRIMARY KEY AUTO_INCREMENT, v VARCHAR(5))");
        ok(ex, "INSERT INTO ai (id, v) VALUES (1, 'a'), (2, 'b')");
        ok(ex, "INSERT INTO ai (v) VALUES ('c')");        // 3 (it was a duplicate-key error: the counter ignored 1 and 2)
        ok(ex, "INSERT INTO ai (id, v) VALUES (10, 'd')");
        ok(ex, "INSERT INTO ai (v) VALUES ('e')");        // 11
        ok(ex, "INSERT INTO ai VALUES (NULL, 'f')");      // NULL and 0 ask for the next number
        ok(ex, "INSERT INTO ai VALUES (0, 'g')");
        REQUIRE(q(ex, "SELECT id, v FROM ai ORDER BY id") ==
                Rows{{"1", "a"}, {"2", "b"}, {"3", "c"}, {"10", "d"}, {"11", "e"}, {"12", "f"}, {"13", "g"}});
        // a failed INSERT does not take a number back, a deleted row's number is not reused
        ok(ex, "DELETE FROM ai WHERE id >= 10");
        ok(ex, "INSERT INTO ai (v) VALUES ('h')");
        REQUIRE(q(ex, "SELECT id FROM ai WHERE v = 'h'") == Rows{{"14"}});
        ok(ex, "INSERT INTO ai (id, v) VALUES (100, 'x')");
    }
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    ok(ex, "INSERT INTO ai (v) VALUES ('after')");
    REQUIRE(q(ex, "SELECT id FROM ai WHERE v = 'after'") == Rows{{"101"}});
}

// ---- random statements against a model of what MySQL does --------------------------------------------------------------------------------
//   t (id INT PRIMARY KEY, a INT NOT NULL, u INT UNIQUE, s VARCHAR(3))     c (cid INT PRIMARY KEY, tid INT REFERENCES t(id))
// Every statement either succeeds or fails and, on failure, changes nothing; after each one both tables are read back and compared.
// (code/test/diff/verify_writes.py is the same model against a running server, for many more statements.)

namespace {
struct Val {
    std::string sql;
    bool null = false;
    std::optional<double> number; // the value when it reads as a number
    std::string text;             // the value as text
    bool is_text = false;         // a quoted string
};

// A value as the INSERT of an INT column sees it: NULL, a number rounded half away from zero, or an error (nullopt inside optional-of-optional)
struct IntResult {
    bool error = false;
    std::optional<long long> value; // nullopt = NULL
};

IntResult as_int(const Val& v) {
    if (v.null) return {false, std::nullopt};
    if (v.is_text) {
        // a quoted string must be a number: digits, optionally with a fraction
        char* end = nullptr;
        double d = std::strtod(v.text.c_str(), &end);
        if (v.text.empty() || *end != '\0') return {true, std::nullopt};
        return {false, static_cast<long long>(d < 0 ? d - 0.5 : d + 0.5)};
    }
    double d = *v.number;
    return {false, static_cast<long long>(d < 0 ? d - 0.5 : d + 0.5)};
}

struct TRow {
    long long a;
    std::optional<long long> u;
    std::optional<std::string> s;
};

struct Model {
    std::map<long long, TRow> t;
    std::map<long long, std::optional<long long>> c;

    bool referenced(long long id) const {
        return std::any_of(c.begin(), c.end(), [&](auto& kv) { return kv.second && *kv.second == id; });
    }
    static bool unique_ok(const std::map<long long, TRow>& rows) {
        std::vector<long long> us;
        for (auto& kv : rows) {
            if (kv.second.u) us.push_back(*kv.second.u);
        }
        std::sort(us.begin(), us.end());
        return std::adjacent_find(us.begin(), us.end()) == us.end();
    }
    // the row an INSERT builds from three values, or nullopt when a column refuses one
    static std::optional<TRow> build(const Val& a, const Val& u, const Val& s) {
        IntResult ia = as_int(a), iu = as_int(u);
        if (ia.error || iu.error || !ia.value) return std::nullopt;
        std::optional<std::string> text;
        if (!s.null) {
            if (s.text.size() > 3) return std::nullopt;
            text = s.text;
        }
        return TRow{*ia.value, iu.value, text};
    }
};
} // namespace

TEST_CASE("random writes behave like the model: constraints, atomic statements, REPLACE, ON DUPLICATE KEY UPDATE", "[write_integrity][fuzz]") {
    unsigned seed_count = 3; // RUSQL_FUZZ_SEEDS=30 runs a much longer campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 0;
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    std::size_t succeeded = 0, rejected = 0;
    for (unsigned seed = seed_start; seed < seed_start + seed_count; seed++) {
        INFO("seed " << seed);
        TempDataDir dir("wi_random");
        Executor ex(dir.path);
        open_db(ex);
        ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, a INT NOT NULL, u INT UNIQUE, s VARCHAR(3))");
        ok(ex, "CREATE TABLE c (cid INT PRIMARY KEY, tid INT, FOREIGN KEY (tid) REFERENCES t(id))");
        std::mt19937 rng(7000 + seed * 13);
        auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
        auto num = [](long long n) { return Val{std::to_string(n), false, static_cast<double>(n), std::to_string(n), false}; };
        auto gen_a = [&]() {
            switch (pick(9)) {
                case 0: return Val{"NULL", true, std::nullopt, "", false};
                case 6: return Val{"'3'", false, 3.0, "3", true};
                case 7: return Val{"'q'", false, std::nullopt, "q", true};
                case 8: return Val{"7.5", false, 7.5, "7.5", false};
                default: return num(static_cast<long long>(pick(5)) + 1);
            }
        };
        auto gen_u = [&]() {
            switch (pick(8)) {
                case 0: return Val{"NULL", true, std::nullopt, "", false};
                case 1: return Val{"'q'", false, std::nullopt, "q", true};
                default: return num(static_cast<long long>(pick(6)) + 1);
            }
        };
        auto gen_s = [&]() {
            static const char* texts[] = {"", "x", "yy", "toolong"};
            if (pick(5) == 0) return Val{"NULL", true, std::nullopt, "", false};
            std::string t = texts[pick(4)];
            return Val{"'" + t + "'", false, std::nullopt, t, true};
        };
        Model model;
        for (int n = 0; n < 220; n++) {
            std::string sql;
            bool want = false;
            switch (pick(14)) {
                case 0: case 1: case 2: case 3: case 4: { // INSERT / REPLACE of one to three rows
                    const bool replace = pick(3) == 0;
                    Model m = model;
                    std::string values;
                    bool good = true;
                    for (std::size_t r = 0, count = 1 + pick(replace ? 2 : 3); r < count; r++) {
                        long long id = static_cast<long long>(pick(8)) + 1;
                        Val a = gen_a(), u = gen_u(), s = gen_s();
                        values += (values.empty() ? "(" : ", (") + std::to_string(id) + ", " + a.sql + ", " + u.sql + ", " + s.sql + ")";
                        auto row = Model::build(a, u, s);
                        if (!row) { good = false; continue; }
                        if (replace) {
                            std::vector<long long> victims;
                            for (auto& kv : m.t) {
                                if (kv.first == id || (row->u && kv.second.u == row->u)) victims.push_back(kv.first);
                            }
                            for (long long v : victims) {
                                if (m.referenced(v)) good = false;
                                m.t.erase(v);
                            }
                            m.t[id] = *row;
                        } else {
                            bool dup = m.t.count(id) > 0 || (row->u && std::any_of(m.t.begin(), m.t.end(), [&](auto& kv) { return kv.second.u == row->u; }));
                            if (dup) good = false;
                            else m.t[id] = *row;
                        }
                    }
                    sql = std::string(replace ? "REPLACE" : "INSERT") + " INTO t VALUES " + values;
                    want = good;
                    if (good) model = m;
                    break;
                }
                case 5: case 6: case 7: { // INSERT ... ON DUPLICATE KEY UPDATE, one row
                    long long id = static_cast<long long>(pick(8)) + 1;
                    Val a = gen_a(), u = gen_u(), s = gen_s();
                    auto row = Model::build(a, u, s);
                    static const char* forms[] = {"a = a + 1", "u = VALUES(u)", "s = 'z'", "a = NULL", "a = VALUES(a), s = VALUES(s)"};
                    std::size_t form = pick(5);
                    sql = "INSERT INTO t VALUES (" + std::to_string(id) + ", " + a.sql + ", " + u.sql + ", " + s.sql + ") ON DUPLICATE KEY UPDATE " + forms[form];
                    if (!row) { want = false; break; }
                    std::vector<long long> hits;
                    for (auto& kv : model.t) {
                        if (kv.first == id || (row->u && kv.second.u == row->u)) hits.push_back(kv.first);
                    }
                    if (hits.size() > 1) continue; // which row MySQL would update is not defined
                    if (hits.empty()) { model.t[id] = *row; want = true; break; }
                    TRow nr = model.t[hits[0]];
                    bool null_a = false;
                    switch (form) {
                        case 0: nr.a += 1; break;
                        case 1: nr.u = row->u; break;
                        case 2: nr.s = "z"; break;
                        case 3: null_a = true; break;
                        default: nr.a = row->a; nr.s = row->s; break;
                    }
                    Model m = model;
                    m.t[hits[0]] = nr;
                    want = !null_a && Model::unique_ok(m.t);
                    if (want) model = m;
                    break;
                }
                case 8: case 9: { // UPDATE t over several rows
                    long long upto = static_cast<long long>(pick(8)) + 1;
                    std::size_t form = pick(5);
                    Model m = model;
                    bool good = true;
                    std::string assign;
                    Val v = gen_u(), sv = gen_s();
                    switch (form) {
                        case 0: assign = "a = a + 2"; break;
                        case 1: assign = "a = NULL"; break;
                        case 2: assign = "u = u + 1"; break;
                        case 3: assign = "u = " + v.sql; break;
                        default: assign = "s = " + sv.sql; break;
                    }
                    for (auto& kv : m.t) {
                        if (kv.first > upto) continue;
                        TRow& r = kv.second;
                        switch (form) {
                            case 0: r.a += 2; break;
                            case 1: good = false; break;
                            case 2: if (r.u) r.u = *r.u + 1; break;
                            case 3: {
                                IntResult iv = as_int(v);
                                if (iv.error) good = false; else r.u = iv.value;
                                break;
                            }
                            default:
                                if (sv.null) r.s = std::nullopt;
                                else if (sv.text.size() > 3) good = false;
                                else r.s = sv.text;
                                break;
                        }
                    }
                    sql = "UPDATE t SET " + assign + " WHERE id <= " + std::to_string(upto);
                    want = good && Model::unique_ok(m.t);
                    if (want) model = m;
                    break;
                }
                case 10: { // UPDATE the key of one row
                    long long k = static_cast<long long>(pick(8)) + 1, newid = pick(3) ? 9 + static_cast<long long>(pick(6)) : static_cast<long long>(pick(8)) + 1;
                    sql = "UPDATE t SET id = " + std::to_string(newid) + " WHERE id = " + std::to_string(k);
                    want = true;
                    if (model.t.count(k) && newid != k) {
                        if (model.referenced(k) || model.t.count(newid)) want = false;
                        else { model.t[newid] = model.t[k]; model.t.erase(k); }
                    }
                    break;
                }
                case 11: { // DELETE from t
                    long long upto = static_cast<long long>(pick(9));
                    sql = "DELETE FROM t WHERE id <= " + std::to_string(upto);
                    want = true;
                    for (auto& kv : model.t) {
                        if (kv.first <= upto && model.referenced(kv.first)) want = false;
                    }
                    if (want) {
                        for (auto it = model.t.begin(); it != model.t.end();) it = it->first <= upto ? model.t.erase(it) : std::next(it);
                    }
                    break;
                }
                case 12: { // the child table
                    long long cid = static_cast<long long>(pick(10)) + 1;
                    bool null_tid = pick(5) == 0;
                    long long tid = static_cast<long long>(pick(9)) + 1;
                    if (pick(2)) {
                        sql = "INSERT INTO c VALUES (" + std::to_string(cid) + ", " + (null_tid ? "NULL" : std::to_string(tid)) + ")";
                        want = !model.c.count(cid) && (null_tid || model.t.count(tid));
                        if (want) model.c[cid] = null_tid ? std::nullopt : std::optional<long long>(tid);
                    } else {
                        sql = "UPDATE c SET tid = " + (null_tid ? std::string("NULL") : std::to_string(tid)) + " WHERE cid = " + std::to_string(cid);
                        want = true;
                        if (model.c.count(cid)) {
                            want = null_tid || model.t.count(tid);
                            if (want) model.c[cid] = null_tid ? std::nullopt : std::optional<long long>(tid);
                        }
                    }
                    break;
                }
                default: {
                    long long cid = static_cast<long long>(pick(10)) + 1;
                    sql = "DELETE FROM c WHERE cid = " + std::to_string(cid);
                    want = true;
                    model.c.erase(cid);
                    break;
                }
            }
            INFO("statement " << n << ": " << sql);
            auto r = ex.execute_sql(sql);
            if (r.is_err()) INFO("engine error: " << r.error());
            REQUIRE(r.is_ok() == want);
            (want ? succeeded : rejected)++;
            Rows rows_t = q(ex, "SELECT id, a, u, s FROM t ORDER BY id");
            Rows rows_c = q(ex, "SELECT cid, tid FROM c ORDER BY cid");
            Rows want_t, want_c;
            for (auto& kv : model.t) {
                want_t.push_back({std::to_string(kv.first), std::to_string(kv.second.a), kv.second.u ? std::to_string(*kv.second.u) : N,
                                  kv.second.s ? *kv.second.s : N});
            }
            for (auto& kv : model.c) want_c.push_back({std::to_string(kv.first), kv.second ? std::to_string(*kv.second) : N});
            REQUIRE(rows_t == want_t);
            REQUIRE(rows_c == want_c);
        }
    }
    INFO("succeeded " << succeeded << " rejected " << rejected);
    REQUIRE(succeeded > 100);
    REQUIRE(rejected > 100);
}
