// R87: ALTER TABLE ... DROP COLUMN and the keys that read the column. MySQL takes the column out of a UNIQUE key (a key left with no column goes) and out of a composite PRIMARY KEY,
// refuses (1062) when the shorter key would repeat over the rows the table holds, and refuses a column a foreign key needs (1828 / 1829) or the only column of the table (1090).
// The answers were measured on MySQL 8.4.9; code/test/diff/corpus/drop_column_constraints_r87.sql repeats them against the server.

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

std::string keys_of(Executor& ex, const std::string& table) {
    return flat(ex, "SELECT index_name, column_name, seq_in_index, non_unique FROM information_schema.statistics WHERE table_schema = 'v' AND table_name = '" + table +
                        "' ORDER BY index_name, seq_in_index");
}

struct World {
    TempDataDir dir;
    Executor ex;
    explicit World(const std::string& name) : dir(name), ex(dir.path) {
        ok(ex, "CREATE DATABASE v");
        ok(ex, "USE v");
    }
};
} // namespace

TEST_CASE("R87: DROP COLUMN takes the column out of a UNIQUE key, and the key goes when no column is left", "[r87]") {
    World w("r87_data_unique");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE d1 (id INT PRIMARY KEY, a INT, b INT, c INT, UNIQUE KEY uab (a, b), UNIQUE KEY ub (b))");
    ok(ex, "INSERT INTO d1 VALUES (1, 1, 1, 1), (2, 2, 2, 2)");
    ok(ex, "ALTER TABLE d1 DROP COLUMN b");
    CHECK(keys_of(ex, "d1") == "PRIMARY,id,1,0;uab,a,1,0"); // (ub had only b; uab is a UNIQUE key over a)
    CHECK(says(error_of(ex, "INSERT INTO d1 VALUES (3, 1, 9)"), "Duplicate")); // (a UNIQUE key over one column)
    ok(ex, "INSERT INTO d1 VALUES (3, 3, 9)");
    CHECK(flat(ex, "SELECT id, a, c FROM d1 ORDER BY id") == "1,1,1;2,2,2;3,3,9");
    ok(ex, "INSERT INTO d1 VALUES (9, 1, 9) ON DUPLICATE KEY UPDATE c = 99"); // (the shorter key is the one a duplicate is found by)
    CHECK(flat(ex, "SELECT id, a, c FROM d1 ORDER BY id") == "1,1,99;2,2,2;3,3,9");
    // three columns: the key keeps the other two
    ok(ex, "CREATE TABLE d3 (id INT PRIMARY KEY, a INT, b INT, c INT, UNIQUE KEY uabc (a, b, c))");
    ok(ex, "INSERT INTO d3 VALUES (1, 1, 1, 1), (2, 1, 2, 2)");
    ok(ex, "ALTER TABLE d3 DROP COLUMN b");
    CHECK(keys_of(ex, "d3") == "PRIMARY,id,1,0;uabc,a,1,0;uabc,c,2,0");
    CHECK(says(error_of(ex, "INSERT INTO d3 VALUES (3, 1, 1)"), "Duplicate entry"));
    ok(ex, "INSERT INTO d3 VALUES (3, 1, 3)");
    // a key that is left with the column of a UNIQUE key the table has already: both stay
    ok(ex, "CREATE TABLE d13 (a INT, b INT, UNIQUE KEY ua (a), UNIQUE KEY uab (a, b))");
    ok(ex, "ALTER TABLE d13 DROP COLUMN b");
    CHECK(keys_of(ex, "d13") == "ua,a,1,0;uab,a,1,0");
    ok(ex, "INSERT INTO d13 VALUES (1)");
    CHECK(says(error_of(ex, "INSERT INTO d13 VALUES (1)"), "Duplicate"));
    // a key that has no column left when the second of two DROP COLUMN runs
    ok(ex, "CREATE TABLE d18 (a INT, b INT, c INT, UNIQUE KEY ub (b), UNIQUE KEY uab (a, b))");
    ok(ex, "ALTER TABLE d18 DROP COLUMN a, DROP COLUMN b");
    CHECK(keys_of(ex, "d18") == "");
    ok(ex, "INSERT INTO d18 VALUES (1)");
    ok(ex, "INSERT INTO d18 VALUES (1)");
    // two columns of the table in one statement: every key that read them is gone
    ok(ex, "CREATE TABLE d11 (id INT PRIMARY KEY, a INT, b INT, c INT, UNIQUE KEY uab (a, b), KEY ib (b), KEY iab (a, b))");
    ok(ex, "INSERT INTO d11 VALUES (1, 1, 1, 1), (2, 2, 2, 2)");
    ok(ex, "ALTER TABLE d11 DROP COLUMN a, DROP COLUMN b");
    CHECK(keys_of(ex, "d11") == "PRIMARY,id,1,0");
    ok(ex, "INSERT INTO d11 VALUES (3, 1)");
}

TEST_CASE("R87: DROP COLUMN is refused when the shorter key would repeat, and nothing is changed", "[r87]") {
    World w("r87_data_repeat");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE d4 (id INT PRIMARY KEY, a INT, b INT, UNIQUE KEY uab (a, b))");
    ok(ex, "INSERT INTO d4 VALUES (1, 1, 1), (2, 1, 2)");
    CHECK(says(error_of(ex, "ALTER TABLE d4 DROP COLUMN b"), "Duplicate entry '1' for key 'uab'"));
    CHECK(flat(ex, "SELECT id, a, b FROM d4 ORDER BY id") == "1,1,1;2,1,2"); // (the column is still there)
    ok(ex, "INSERT INTO d4 VALUES (3, 1, 3)"); // (so is the key: (1, 3) is new)
    CHECK(says(error_of(ex, "INSERT INTO d4 VALUES (4, 1, 3)"), "Duplicate entry"));
    // a NULL in the key repeats nothing while the key is long, but the shorter key has no NULL there
    ok(ex, "CREATE TABLE k (a INT, b INT, c INT, UNIQUE KEY uab (a, b))");
    ok(ex, "INSERT INTO k VALUES (1, NULL, 1), (1, NULL, 2)");
    CHECK(says(error_of(ex, "ALTER TABLE k DROP COLUMN b"), "Duplicate entry"));
    // a NULL in the key that is left repeats nothing
    ok(ex, "CREATE TABLE d16 (a INT, b INT, c INT, UNIQUE KEY uabc (a, b, c))");
    ok(ex, "INSERT INTO d16 VALUES (1, NULL, 1), (1, NULL, 2)");
    ok(ex, "ALTER TABLE d16 DROP COLUMN c");
    CHECK(keys_of(ex, "d16") == "uabc,a,1,0;uabc,b,2,0");
    ok(ex, "INSERT INTO d16 VALUES (1, NULL)");
    CHECK(flat(ex, "SELECT COUNT(*) FROM d16") == "3");
    // a row that was deleted does not count (it stays in the table until VACUUM)
    ok(ex, "CREATE TABLE d17 (a INT, b INT, UNIQUE KEY uab (a, b))");
    ok(ex, "INSERT INTO d17 VALUES (1, 1), (1, 2)");
    ok(ex, "DELETE FROM d17 WHERE b = 2");
    ok(ex, "ALTER TABLE d17 DROP COLUMN b");
    CHECK(flat(ex, "SELECT a FROM d17") == "1");
    // the same for a composite PRIMARY KEY
    ok(ex, "CREATE TABLE d6 (a INT, b INT, c INT, PRIMARY KEY (a, b))");
    ok(ex, "INSERT INTO d6 VALUES (1, 1, 1), (1, 2, 2)");
    CHECK(says(error_of(ex, "ALTER TABLE d6 DROP COLUMN b"), "Duplicate entry '1' for key 'PRIMARY'"));
    CHECK(flat(ex, "SELECT a, b, c FROM d6 ORDER BY b") == "1,1,1;1,2,2");
    // several DROP COLUMN in one statement are looked at together, and the statement is all or nothing
    ok(ex, "CREATE TABLE m (a INT, b INT, c INT, UNIQUE KEY uabc (a, b, c))");
    ok(ex, "INSERT INTO m VALUES (1, 1, 1), (1, 2, 2)");
    CHECK(says(error_of(ex, "ALTER TABLE m DROP COLUMN c, DROP COLUMN b"), "Duplicate entry '1' for key 'uabc'"));
    CHECK(flat(ex, "SELECT a, b, c FROM m ORDER BY c") == "1,1,1;1,2,2"); // (c was not dropped before b was looked at)
    ok(ex, "ALTER TABLE m DROP COLUMN c"); // (a, b is still unique)
    CHECK(keys_of(ex, "m") == "uabc,a,1,0;uabc,b,2,0");
    CHECK(says(error_of(ex, "ALTER TABLE m DROP COLUMN a, DROP COLUMN b"), "ERROR 1090"));
    CHECK(flat(ex, "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = 'v' AND table_name = 'm'") == "2");
}

TEST_CASE("R87: DROP COLUMN shortens a composite PRIMARY KEY", "[r87]") {
    World w("r87_data_primary");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE d5 (a INT, b INT, c INT, PRIMARY KEY (a, b))");
    ok(ex, "INSERT INTO d5 VALUES (1, 1, 1), (2, 1, 2)");
    CHECK(keys_of(ex, "d5") == "PRIMARY,a,1,0;PRIMARY,b,2,0");
    ok(ex, "ALTER TABLE d5 DROP COLUMN b");
    CHECK(keys_of(ex, "d5") == "PRIMARY,a,1,0");
    CHECK(says(error_of(ex, "INSERT INTO d5 VALUES (1, 5)"), "Duplicate"));
    ok(ex, "INSERT INTO d5 VALUES (3, 5)");
    // a PRIMARY KEY of one column that is dropped leaves the table with none
    ok(ex, "CREATE TABLE d7 (a INT PRIMARY KEY, b INT)");
    ok(ex, "ALTER TABLE d7 DROP COLUMN a");
    ok(ex, "INSERT INTO d7 VALUES (1)");
    ok(ex, "INSERT INTO d7 VALUES (1)");
    CHECK(flat(ex, "SELECT COUNT(*) FROM d7") == "2");
}

TEST_CASE("R87: DROP COLUMN refuses a column a foreign key needs and the only column of the table", "[r87]") {
    World w("r87_data_fk");
    Executor& ex = w.ex;
    ok(ex, "CREATE TABLE p (id INT PRIMARY KEY, x INT)");
    ok(ex, "CREATE TABLE ch (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES p (id))");
    CHECK(says(error_of(ex, "ALTER TABLE ch DROP COLUMN p"), "ERROR 1828 (HY000): Cannot drop column 'p': needed in a foreign key constraint 'ch_ibfk_1'"));
    CHECK(says(error_of(ex, "ALTER TABLE p DROP COLUMN id"), "ERROR 1829 (HY000): Cannot drop column 'id': needed in a foreign key constraint 'ch_ibfk_1' of table 'ch'"));
    ok(ex, "ALTER TABLE p DROP COLUMN x"); // (not the column the key refers to)
    CHECK(says(error_of(ex, "ALTER TABLE p DROP COLUMN id"), "ERROR 1090")); // (the last column of p: that is said before the key is looked at)
    ok(ex, "CREATE TABLE other (id INT PRIMARY KEY, y INT)"); // (a column that is called id, in a table no key refers to)
    ok(ex, "ALTER TABLE other DROP COLUMN id");
    CHECK(flat(ex, "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = 'v' AND table_name = 'other'") == "1");
    ok(ex, "CREATE TABLE one (a INT)");
    CHECK(says(error_of(ex, "ALTER TABLE one DROP COLUMN a"), "ERROR 1090 (42000)"));
    CHECK(flat(ex, "SELECT COUNT(*) FROM information_schema.columns WHERE table_schema = 'v' AND table_name = 'one'") == "1");
}

TEST_CASE("R87: the shorter keys are kept over a restart", "[r87]") {
    TempDataDir dir("r87_data_restart");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE v");
        ok(ex, "USE v");
        ok(ex, "CREATE TABLE d1 (id INT PRIMARY KEY, a INT, b INT, c INT, UNIQUE KEY uabc (a, b, c))");
        ok(ex, "CREATE TABLE d5 (a INT, b INT, c INT, PRIMARY KEY (a, b))");
        ok(ex, "ALTER TABLE d1 DROP COLUMN b");
        ok(ex, "ALTER TABLE d5 DROP COLUMN b");
    }
    Executor again(dir.path);
    ok(again, "USE v");
    CHECK(keys_of(again, "d1") == "PRIMARY,id,1,0;uabc,a,1,0;uabc,c,2,0");
    CHECK(keys_of(again, "d5") == "PRIMARY,a,1,0");
    ok(again, "INSERT INTO d1 VALUES (1, 1, 1)");
    CHECK(says(error_of(again, "INSERT INTO d1 VALUES (2, 1, 1)"), "Duplicate entry"));
    ok(again, "INSERT INTO d5 VALUES (1, 1)");
    CHECK(says(error_of(again, "INSERT INTO d5 VALUES (1, 2)"), "Duplicate"));
}
