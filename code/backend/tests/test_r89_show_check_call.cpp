// R89: SHOW DATABASES | SCHEMAS LIKE 'pattern' | WHERE condition (the schemas the account may see, filtered, the column named `Database (pattern)` for LIKE), a CHECK condition
// written the way MySQL writes it (SHOW CREATE TABLE and information_schema.check_constraints), and what CALL says for ROW_COUNT() (the rows of the last statement of the body that
// sets them). The answers were measured on MySQL 8.4.9; code/test/diff/corpus/show_databases_r89.sql, check_text_r89.sql and call_rows_r89.sql repeat them against the server.

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

bool says(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

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

// the header of an answer table, cells trimmed and joined by a comma
std::string header(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    std::istringstream in(r.value());
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        std::string out;
        std::size_t from = 1;
        while (from < line.size()) {
            const std::size_t end = line.find('|', from);
            if (end == std::string::npos) break;
            std::string cell = line.substr(from, end - from);
            const std::size_t a = cell.find_first_not_of(' '), b = cell.find_last_not_of(' ');
            out += (out.empty() ? "" : ",") + (a == std::string::npos ? std::string() : cell.substr(a, b - a + 1));
            from = end + 1;
        }
        return out;
    }
    return std::string();
}

std::uint64_t reported(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_ok() ? r.value() : r.error()));
    REQUIRE(r.is_ok());
    return ex.statement_affected_rows(r.value());
}
} // namespace

TEST_CASE("R89: SHOW DATABASES | SCHEMAS LIKE and WHERE give the schemas that match, in the protocol of the app", "[r89]") {
    TempDataDir dir("r89_data_show_databases");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE zz_a");
    ok(ex, "CREATE DATABASE zz_b");
    ok(ex, "CREATE DATABASE yy_c");
    CHECK(flat(ex, "SHOW DATABASES LIKE 'zz%'") == "zz_a;zz_b");
    CHECK(flat(ex, "SHOW DATABASES LIKE 'zz_a'") == "zz_a");
    CHECK(flat(ex, "SHOW DATABASES LIKE 'ZZ_A'") == "zz_a"); // (the pattern ignores the case)
    CHECK(flat(ex, "SHOW DATABASES LIKE 'zz\\_a'") == "zz_a");
    CHECK(flat(ex, "SHOW DATABASES LIKE 'zz\\_'") == ""); // (an escaped _ is a _)
    CHECK(flat(ex, "SHOW DATABASES LIKE 'nosuch'") == "");
    CHECK(flat(ex, "SHOW DATABASES LIKE \"zz%\"") == "zz_a;zz_b");
    CHECK(flat(ex, "SHOW SCHEMAS LIKE 'yy%'") == "yy_c");
    CHECK(flat(ex, "show  schemas  like  'yy%' ;") == "yy_c");
    CHECK(flat(ex, "SHOW SCHEMAS") == "yy_c;zz_a;zz_b");
    CHECK(flat(ex, "SHOW DATABASES WHERE `Database` = 'zz_b'") == "zz_b");
    CHECK(flat(ex, "SHOW DATABASES WHERE `Database` LIKE 'zz%' AND `Database` <> 'zz_a'") == "zz_b");
    CHECK(flat(ex, "SHOW SCHEMAS WHERE `Database` > 'yy' AND `Database` < 'zzz'") == "yy_c;zz_a;zz_b");
    // the column is called `Database (pattern)` for LIKE, `Database` for WHERE
    CHECK(header(ex, "SHOW DATABASES LIKE 'zz%'") == "Database (zz%)");
    CHECK(header(ex, "SHOW DATABASES LIKE 'zz''_a'") == "Database (zz'_a)");
    CHECK(header(ex, "SHOW DATABASES WHERE `Database` = 'zz_b'") == "Database");
    // what is not this statement is a syntax error, as it is in MySQL
    CHECK(error_of(ex, "SHOW DATABASES LIKE").size() > 0);
    CHECK(error_of(ex, "SHOW DATABASES LIKE zz").size() > 0);
    CHECK(error_of(ex, "SHOW DATABASES LIKE 5").size() > 0);
    CHECK(error_of(ex, "SHOW DATABASES LIKE 'zz%' WHERE 1").size() > 0);
    CHECK(error_of(ex, "SHOW DATABASES FROM zz_a").size() > 0);
    CHECK(says(error_of(ex, "SHOW DATABASES WHERE nosuch = 1"), "Unknown column 'nosuch'"));
    // the empty system schema is listed to a MySQL-protocol session only
    CHECK(flat(ex, "SHOW DATABASES LIKE 'my%'") == "");
    ex.lists_system_schema = true;
    CHECK(flat(ex, "SHOW DATABASES LIKE 'my%'") == "mysql");
    CHECK(flat(ex, "SHOW SCHEMAS WHERE `Database` = 'mysql'") == "mysql");
}

TEST_CASE("R89: SHOW DATABASES LIKE shows an account only the schemas it may see", "[r89][privileges]") {
    TempDataDir dir("r89_data_show_databases_grants");
    Executor root(dir.path);
    ok(root, "CREATE DATABASE yy_c");
    ok(root, "CREATE DATABASE zz_a");
    ok(root, "CREATE USER 'bob'@'%' IDENTIFIED BY 'pw'");
    ok(root, "GRANT SELECT ON yy_c.* TO 'bob'@'%'");
    Executor bob = Executor::new_session(root.get_shared());
    bob.auth_user = "bob";
    bob.activate_default_roles();
    CHECK(flat(bob, "SHOW DATABASES") == "yy_c");
    CHECK(flat(bob, "SHOW DATABASES LIKE '%'") == "yy_c");
    CHECK(flat(bob, "SHOW DATABASES LIKE 'zz%'") == "");
    CHECK(flat(bob, "SHOW SCHEMAS WHERE `Database` <> 'nosuch'") == "yy_c");
    CHECK(flat(root, "SHOW DATABASES LIKE '%'") == "yy_c;zz_a");
}

TEST_CASE("R89: a CHECK condition is written as MySQL writes it", "[r89]") {
    TempDataDir dir("r89_data_check_text");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE v");
    ok(ex, "USE v");
    ok(ex, "CREATE TABLE ck (qty INT, price DECIMAL(10,2), name VARCHAR(20), age INT CHECK (age > 0), "
           "CONSTRAINT c1 CHECK (qty >= 0), CONSTRAINT c2 CHECK (qty > 0 AND price < 100 AND name <> ''), CONSTRAINT c3 CHECK (NOT (qty = 5)), "
           "CONSTRAINT c4 CHECK (name LIKE 'a%' OR qty IN (1, 2, 3)), CONSTRAINT c5 CHECK (QTY BETWEEN 1 AND 10), CONSTRAINT c6 CHECK (UPPER(name) = 'X' OR NOT (qty > 3 AND price > 0)), "
           "CONSTRAINT c7 CHECK (name REGEXP '^a'), CONSTRAINT c8 CHECK ((qty + 1) * 2 > -5), CONSTRAINT c9 CHECK (-qty < 100))");
    auto created = ex.show_create_table_mysql("ck");
    REQUIRE(created.is_ok());
    const std::string shown = created.value();
    CHECK(says(shown, "CONSTRAINT `c1` CHECK ((`qty` >= 0))"));
    CHECK(says(shown, "CONSTRAINT `c2` CHECK (((`qty` > 0) and (`price` < 100) and (`name` <> _utf8mb4'')))")); // (a chain of ANDs is one pair of brackets)
    CHECK(says(shown, "CONSTRAINT `c3` CHECK ((`qty` <> 5))")); // (a NOT goes into the comparison)
    CHECK(says(shown, "CONSTRAINT `c4` CHECK (((`name` like _utf8mb4'a%') or (`qty` in (1,2,3))))"));
    CHECK(says(shown, "CONSTRAINT `c5` CHECK ((`QTY` between 1 and 10))")); // (the column in the case it was typed in)
    CHECK(says(shown, "CONSTRAINT `c6` CHECK (((upper(`name`) = _utf8mb4'X') or ((`qty` <= 3) or (`price` <= 0))))")); // (NOT of an AND: the opposite comparisons, OR)
    CHECK(says(shown, "CONSTRAINT `c7` CHECK (regexp_like(`name`,_utf8mb4'^a'))"));
    CHECK(says(shown, "CONSTRAINT `c9` CHECK ((- qty < 100))")); // (one this does not write -- `-qty` is read as `0 - qty` -- is shown as it was typed)
    CHECK(says(shown, "CONSTRAINT `c8` CHECK ((((`qty` + 1) * 2) > -(5)))"));
    CHECK(says(shown, "CONSTRAINT `ck_chk_1` CHECK ((`age` > 0))")); // (the check of a column)
    // information_schema: the same text, with the quotes and the backslashes of it written with a backslash
    CHECK(flat(ex, "SELECT check_clause FROM information_schema.check_constraints WHERE constraint_name = 'c1'") == "(`qty` >= 0)");
    const std::string c2 = "((`qty` > 0) and (`price` < 100) and (`name` <> _utf8mb4\\'\\'))"; // (the two quotes of the empty string, each with its backslash)
    CHECK(flat(ex, "SELECT LENGTH(check_clause) FROM information_schema.check_constraints WHERE constraint_name = 'c2'") == std::to_string(c2.size()));
    CHECK(flat(ex, "SELECT check_clause LIKE '%_utf8mb4%' FROM information_schema.check_constraints WHERE constraint_name = 'c2'") == "1");
    CHECK(flat(ex, "SELECT check_clause FROM information_schema.check_constraints WHERE constraint_name = 'c9'") == "(- qty < 100)");
    // what SHOW CREATE TABLE says can be run again, and the table checks as before
    ok(ex, "DROP TABLE ck");
    ok(ex, "CREATE TABLE ck (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT c2 CHECK (((`qty` > 0) and (`price` < 100) and (`name` <> _utf8mb4''))), "
           "CONSTRAINT c4 CHECK (((`name` like _utf8mb4'a%') or (`qty` in (1,2,3)))))");
    ok(ex, "INSERT INTO ck VALUES (1, 5, 'x')");
    CHECK(says(error_of(ex, "INSERT INTO ck VALUES (1, 500, 'x')"), "CHECK"));
    CHECK(says(error_of(ex, "INSERT INTO ck VALUES (1, 5, '')"), "CHECK"));
}

TEST_CASE("R89: more CHECK shapes are written as MySQL writes them", "[r89]") {
    TempDataDir dir("r89_data_check_shapes");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE v");
    ok(ex, "USE v");
    struct Shape {
        const char* typed;
        const char* written; // (what MySQL 8.4.9 writes for it, in SHOW CREATE TABLE)
    };
    const Shape shapes[] = {
        {"NOT (qty IN (1, 2))", "(`qty` not in (1,2))"},
        {"NOT (qty BETWEEN 1 AND 2)", "(`qty` not between 1 and 2)"},
        {"NOT (qty IS NULL)", "(`qty` is not null)"},
        {"name NOT LIKE 'a%'", "(not((`name` like _utf8mb4'a%')))"},
        {"NOT (name LIKE 'a')", "(not((`name` like _utf8mb4'a')))"},
        {"qty > 007", "(`qty` > 7)"},
        {"name = 'it''s'", "(`name` = _utf8mb4'it\\'s')"},
        {"LCASE(name) = 'x'", "(lower(`name`) = _utf8mb4'x')"},
        {"CEIL(price) > 1", "(ceiling(`price`) > 1)"},
        {"SUBSTRING(name, 1, 2) = 'ab'", "(substr(`name`,1,2) = _utf8mb4'ab')"},
        {"SUBSTRING(name FROM 1 FOR 2) = 'ab'", "(substr(`name`,1,2) = _utf8mb4'ab')"},
        {"ROUND(price) > 1", "(round(`price`,0) > 1)"},
        {"ROUND(price, 1) > 1", "(round(`price`,1) > 1)"},
        {"MOD(qty, 2) = 0", "((`qty` % 2) = 0)"},
        {"CASE WHEN qty > 0 THEN 1 ELSE 0 END = 1", "((case when (`qty` > 0) then 1 else 0 end) = 1)"},
        {"CASE WHEN qty > 0 THEN 'p' WHEN qty < 0 THEN 'n' END = 'p'", "((case when (`qty` > 0) then _utf8mb4'p' when (`qty` < 0) then _utf8mb4'n' end) = _utf8mb4'p')"},
        {"name LIKE price", "(`name` like `price`)"},
        {"NOT (qty > 3)", "(`qty` <= 3)"},
        {"NOT (qty >= 3)", "(`qty` < 3)"},
        {"NOT (qty < 3)", "(`qty` >= 3)"},
        {"NOT (qty <= 3)", "(`qty` > 3)"},
        {"NOT (qty > 0 OR price > 0)", "((`qty` <= 0) and (`price` <= 0))"},
        {"qty > 0 OR qty < -5 OR price = 0", "((`qty` > 0) or (`qty` < -(5)) or (`price` = 0))"},
    };
    for (const Shape& shape : shapes) {
        INFO(shape.typed);
        ok(ex, "DROP TABLE IF EXISTS ck");
        ok(ex, std::string("CREATE TABLE ck (qty INT, price DECIMAL(10,2), name VARCHAR(20), CONSTRAINT c1 CHECK (") + shape.typed + "))");
        auto created = ex.show_create_table_mysql("ck");
        REQUIRE(created.is_ok());
        CHECK(says(created.value(), std::string("CONSTRAINT `c1` CHECK (") + shape.written + ")"));
    }
}

TEST_CASE("R89: a CHECK with arithmetic, a function, % or REGEXP is kept as it was typed and is enforced", "[r89]") {
    TempDataDir dir("r89_data_check_typed");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE v");
    ok(ex, "USE v");
    // (these CHECKs were stored with the word "Token" for `*`, `%` and the function and REGEXP words: every row of such a table was refused, "FUNCTION token does not exist")
    ok(ex, "CREATE TABLE ck (qty INT, name VARCHAR(20), CONSTRAINT c1 CHECK (qty * 2 > 0), CONSTRAINT c2 CHECK (LENGTH(name) > 2), CONSTRAINT c3 CHECK (qty % 2 = 0), "
           "CONSTRAINT c4 CHECK (name REGEXP '^a'), CONSTRAINT c5 CHECK (qty + 1 <> 3 AND -qty < 100))");
    ok(ex, "INSERT INTO ck VALUES (4, 'abcd')");
    CHECK(says(error_of(ex, "INSERT INTO ck VALUES (-4, 'abcd')"), "CHECK")); // (qty * 2 > 0)
    CHECK(says(error_of(ex, "INSERT INTO ck VALUES (4, 'ab')"), "CHECK"));    // (LENGTH(name) > 2)
    CHECK(says(error_of(ex, "INSERT INTO ck VALUES (3, 'abcd')"), "CHECK"));  // (qty % 2 = 0)
    CHECK(says(error_of(ex, "INSERT INTO ck VALUES (4, 'xbcd')"), "CHECK"));  // (name REGEXP '^a')
    CHECK(says(error_of(ex, "INSERT INTO ck VALUES (2, 'abcd')"), "CHECK"));  // (qty + 1 <> 3)
    CHECK(flat(ex, "SELECT COUNT(*) FROM ck") == "1");
    CHECK(flat(ex, "SELECT check_clause LIKE 'regexp_like(%' FROM information_schema.check_constraints WHERE constraint_name = 'c4'") == "1");
    ok(ex, "ALTER TABLE ck ADD CONSTRAINT c6 CHECK (qty / 2 < 10)");
    CHECK(says(error_of(ex, "INSERT INTO ck VALUES (40, 'abcd')"), "CHECK"));
}

TEST_CASE("R89: CALL says the rows of the last statement of its body that sets a row count", "[r89]") {
    TempDataDir dir("r89_data_call_rows");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE v");
    ok(ex, "USE v");
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, a INT)");
    ok(ex, "INSERT INTO t VALUES (1, 1), (2, 2), (3, 3), (4, 4)");
    struct Case {
        const char* name;
        const char* body;
        std::uint64_t rows;
    };
    const Case cases[] = {
        {"p_insert2", "INSERT INTO t VALUES (10, 1), (11, 1)", 2},
        {"p_update3", "UPDATE t SET a = a + 100 WHERE id <= 3", 3},
        {"p_update_none", "UPDATE t SET a = a WHERE id <= 3", 0},
        {"p_delete1", "DELETE FROM t WHERE id = 10", 1},
        {"p_select_into", "SELECT COUNT(*) INTO @n FROM t", 1},
        {"p_select", "SELECT * FROM t", 0},
        {"p_set", "SET @x = 5", 0},
        {"p_two", "INSERT INTO t VALUES (20, 1); UPDATE t SET a = 0 WHERE id <= 2", 2},
        {"p_insert_then_select", "INSERT INTO t VALUES (21, 1); SELECT 1", 0},
        {"p_insert_then_set", "INSERT INTO t VALUES (22, 1); SET @y = 1", 0},
        {"p_declare", "DECLARE v INT DEFAULT 0; SELECT COUNT(*) INTO v FROM t", 1},
        {"p_ddl", "CREATE TABLE IF NOT EXISTS q (z INT)", 0},
        {"p_if", "IF 1 = 1 THEN UPDATE t SET a = a + 1 WHERE id = 1; END IF", 1},
        {"p_loop", "DECLARE i INT DEFAULT 0; WHILE i < 3 DO INSERT INTO t VALUES (100 + i, i); SET i = i + 1; END WHILE", 1},
        {"p_update_then_local", "DECLARE k INT DEFAULT 0; UPDATE t SET a = a + 1 WHERE id <= 2; SET k = 5", 2},
    };
    for (const Case& c : cases) ok(ex, std::string("CREATE PROCEDURE ") + c.name + "() BEGIN " + c.body + "; END");
    for (const Case& c : cases) {
        INFO(c.name);
        CHECK(reported(ex, std::string("CALL ") + c.name + "()") == c.rows);
        CHECK(flat(ex, "SELECT ROW_COUNT()") == std::to_string(c.rows));
    }
}
