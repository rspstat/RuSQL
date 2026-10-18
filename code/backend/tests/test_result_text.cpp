#include <filesystem>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/result_text.hpp"

using namespace engine;
namespace fs = std::filesystem;

// The text of a result table has to give every value back as it was: a space or tab at either end of a value is written \s / \t (the padding of the
// column and the space after the bar would swallow it), a backslash \\, a newline \n and a bar \|. Where the engine reads such a text again -- a derived
// table, a CTE, UNION, INSERT ... SELECT, CREATE TABLE ... AS SELECT, IN (subquery), a scalar subquery -- ' x ' used to come back as 'x'.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

void ok(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_ok());
}

// the cells of the data rows of a SELECT, exactly (read back with the same rule everything else uses)
std::vector<std::vector<std::string>> rows_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_ok());
    std::vector<std::vector<std::string>> rows;
    std::istringstream in(r.value());
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars == 1) continue;
        rows.push_back(result_text::split_row(line));
    }
    return rows;
}

using Rows = std::vector<std::vector<std::string>>;
} // namespace

TEST_CASE("a cell is written and read back whatever it holds", "[result_text]") {
    using namespace result_text;
    REQUIRE(escape_cell(" x ") == "\\sx\\s");
    REQUIRE(escape_cell("a b") == "a b");                  // (a space inside is just a space)
    REQUIRE(escape_cell("\t a\t") == "\\t\\sa\\t");
    REQUIRE(escape_cell("   ") == "\\s\\s\\s");           // (nothing but spaces: all of them are at an end)
    REQUIRE(escape_cell("") == "");
    REQUIRE(escape_cell("a|b\\c\nd") == "a\\|b\\\\c\\nd");
    REQUIRE(unescape_cell("\\sx\\s") == " x ");
    REQUIRE(unescape_cell("\\q") == "\\q");               // (a lone backslash stands for itself)
    REQUIRE(split_row("| 1  | \\sx\\s | a\\|b |") == std::vector<std::string>{"1", " x ", "a|b"});
    REQUIRE(split_row("|   | x |") == std::vector<std::string>{"", "x"});

    // every string of a small alphabet, in rows of one to four cells, written the way a table is and read back
    const char alphabet[] = {' ', '\t', '\\', '|', '\n', 'a', 'n', 's', 't', '0'};
    std::mt19937 rng(7);
    for (int round = 0; round < 20000; round++) {
        const std::size_t cell_count = 1 + rng() % 4;
        std::vector<std::string> cells(cell_count);
        for (std::string& cell : cells) {
            const std::size_t length = rng() % 9;
            for (std::size_t k = 0; k < length; k++) cell.push_back(alphabet[rng() % sizeof alphabet]);
        }
        std::vector<std::size_t> widths;
        for (const std::string& cell : cells) widths.push_back(escape_cell(cell).size() + rng() % 3); // (a column wider than its widest cell: more padding)
        std::string line = "|";
        for (std::size_t i = 0; i < cells.size(); i++) {
            const std::string escaped = escape_cell(cells[i]);
            line += " " + escaped + std::string(widths[i] - escaped.size(), ' ') + " |";
        }
        INFO("row " << line);
        REQUIRE(split_row(line) == cells);
    }
}

TEST_CASE("a value with a space at either end survives every way the engine reads a result again", "[result_text]") {
    TempDataDir dir("result_text_sql");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE sp (id INT PRIMARY KEY, s VARCHAR(20))");
    ok(ex, "INSERT INTO sp VALUES (1, ' x '), (2, 'a|b'), (3, '  '), (4, ''), (5, CONCAT('tab', CHAR(9))), (6, 'bob'), (7, 'bob '), "
           "(8, CONCAT('multi', CHAR(10), 'line')), (9, CONCAT('back', CHAR(92), 'slash')), (10, ' lead')");
    const Rows all = {{"1", " x "}, {"2", "a|b"}, {"3", "  "}, {"4", ""}, {"5", "tab\t"}, {"6", "bob"}, {"7", "bob "}, {"8", "multi\nline"}, {"9", "back\\slash"}, {"10", " lead"}};

    REQUIRE(rows_of(ex, "SELECT id, s FROM sp ORDER BY id") == all);
    REQUIRE(rows_of(ex, "SELECT d.id, d.s FROM (SELECT id, s FROM sp) d ORDER BY d.id") == all);                       // a derived table
    REQUIRE(rows_of(ex, "WITH c AS (SELECT id, s FROM sp) SELECT id, s FROM c ORDER BY id") == all);                     // a CTE
    REQUIRE(rows_of(ex, "SELECT s FROM sp WHERE id IN (1, 3, 7) UNION SELECT s FROM sp WHERE id = 10 ORDER BY 1") ==     // a set operation
            Rows{{"  "}, {" lead"}, {" x "}, {"bob "}});
    ok(ex, "CREATE TABLE sp_copy AS SELECT id, s FROM sp");                                                                // CREATE TABLE ... AS SELECT
    REQUIRE(rows_of(ex, "SELECT id, s FROM sp_copy ORDER BY id") == all);
    ok(ex, "INSERT INTO sp (id, s) SELECT id + 100, s FROM sp");                                                           // INSERT ... SELECT
    REQUIRE(rows_of(ex, "SELECT id - 100, s FROM sp WHERE id > 100 ORDER BY id") == all);
    REQUIRE(rows_of(ex, "SELECT id FROM sp WHERE id < 100 AND s IN (SELECT s FROM sp WHERE id IN (101, 107)) ORDER BY id") == Rows{{"1"}, {"7"}}); // IN (subquery)
    REQUIRE(rows_of(ex, "SELECT (SELECT s FROM sp WHERE id = 1) = ' x ', (SELECT s FROM sp WHERE id = 7) = 'bob', (SELECT s FROM sp WHERE id = 7) = 'bob '") ==
            Rows{{"1", "0", "1"}});                                                                                         // a scalar subquery
    // 'bob' and 'bob ' are two values under the default collation (NO PAD)
    REQUIRE(rows_of(ex, "SELECT COUNT(DISTINCT s) FROM (SELECT s FROM sp) d") == Rows{{"10"}});
    REQUIRE(rows_of(ex, "SELECT LENGTH(s) FROM sp WHERE id > 100 ORDER BY id") == Rows{{"3"}, {"3"}, {"2"}, {"0"}, {"4"}, {"3"}, {"4"}, {"10"}, {"10"}, {"5"}});
    // a row of values against the answer of a subquery
    REQUIRE(rows_of(ex, "SELECT id FROM sp WHERE id < 100 AND (id, s) IN (SELECT id - 100, s FROM sp WHERE id > 100) ORDER BY id") ==
            Rows{{"1"}, {"2"}, {"3"}, {"4"}, {"5"}, {"6"}, {"7"}, {"8"}, {"9"}, {"10"}});
    REQUIRE(rows_of(ex, "SELECT id FROM sp WHERE id < 100 AND (id, s) IN (SELECT id - 100, s FROM sp WHERE id = 107) ORDER BY id") == Rows{{"7"}}); // ('bob ' is not 'bob')
    // the text the user sees: the edge spaces are written \s
    auto shown = ex.execute_sql("SELECT s FROM sp WHERE id = 1");
    REQUIRE(shown.is_ok());
    REQUIRE(shown.value().find("| \\sx\\s |") != std::string::npos);
    // RETURNING writes its cells the same way
    REQUIRE(rows_of(ex, "INSERT INTO sp (id, s) VALUES (200, ' y | z\\\\ ') RETURNING id, s") == Rows{{"200", " y | z\\ "}});
}

// A CTE is a temporary table the final query is read against, so its columns need types: the type of the column they show, a number for arithmetic and
// for what a COALESCE / CASE / IF gives out of numbers, text otherwise. With every column text, ORDER BY id put 10 before 9 and MAX(id) was 9.
TEST_CASE("a CTE column is ordered and compared by what it holds", "[result_text][cte]") {
    TempDataDir dir("result_text_cte");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE nums (id INT PRIMARY KEY, price DECIMAL(10,2), name VARCHAR(10), code VARCHAR(5))");
    ok(ex, "INSERT INTO nums VALUES (9, 1.5, 'nine', '9'), (10, 20.25, 'ten', '10'), (2, NULL, 'two', '2')");
    ok(ex, "CREATE TABLE crowd (id INT PRIMARY KEY, g INT)");
    ok(ex, "INSERT INTO crowd VALUES (1, 1), (2, 1), (3, 1), (4, 1), (5, 1), (6, 1), (7, 1), (8, 1), (9, 1), (10, 1), (11, 1), (12, 1), (13, 2), (14, 2), (15, 2)");
    const Rows descending = {{"10"}, {"9"}, {"2"}};

    REQUIRE(rows_of(ex, "WITH c AS (SELECT id FROM nums) SELECT id FROM c ORDER BY id DESC") == descending);                       // a column
    REQUIRE(rows_of(ex, "WITH c AS (SELECT id * 1 AS v FROM nums) SELECT v FROM c ORDER BY v DESC") == descending);                // arithmetic
    REQUIRE(rows_of(ex, "WITH c AS (SELECT price * 2 AS v FROM nums) SELECT MAX(v), MIN(v) FROM c") == Rows{{"40.50", "3.00"}});     // (with a NULL among them)
    REQUIRE(rows_of(ex, "WITH c AS (SELECT COALESCE(id, 0) AS v FROM nums) SELECT v FROM c ORDER BY v DESC") == descending);        // what a COALESCE gives
    REQUIRE(rows_of(ex, "WITH c AS (SELECT IFNULL(price, 0) + id AS v FROM nums) SELECT MAX(v) FROM c") == Rows{{"30.25"}});
    REQUIRE(rows_of(ex, "WITH c AS (SELECT CASE WHEN id > 5 THEN id ELSE 0 END AS v FROM nums) SELECT v FROM c ORDER BY v DESC") == (Rows{{"10"}, {"9"}, {"0"}}));
    REQUIRE(rows_of(ex, "WITH c AS (SELECT IF(id > 1, id, NULL) AS v FROM nums) SELECT MAX(v) FROM c") == Rows{{"10"}});
    REQUIRE(rows_of(ex, "WITH c AS (SELECT CAST(name AS CHAR) AS v FROM nums) SELECT v FROM c ORDER BY v DESC") == (Rows{{"two"}, {"ten"}, {"nine"}}));
    REQUIRE(rows_of(ex, "WITH c AS (SELECT CAST(id AS SIGNED) AS v FROM nums) SELECT v FROM c ORDER BY v DESC") == descending);                      // CAST to a number
    REQUIRE(rows_of(ex, "WITH c AS (SELECT CAST(id AS CHAR) AS v FROM nums) SELECT v FROM c ORDER BY v DESC") == (Rows{{"9"}, {"2"}, {"10"}}));   // ... to text
    REQUIRE(rows_of(ex, "WITH c AS (SELECT id % 3 AS g, COALESCE(SUM(id), 0) AS v FROM nums GROUP BY id % 3) SELECT v FROM c ORDER BY v DESC") ==  // an aggregate in a function
            (Rows{{"10"}, {"9"}, {"2"}}));
    REQUIRE(rows_of(ex, "WITH c AS (SELECT MAX(id) AS v FROM nums GROUP BY id % 3) SELECT v FROM c ORDER BY v DESC") == descending);
    REQUIRE(rows_of(ex, "WITH c AS (SELECT (SELECT MAX(id) FROM nums) AS v UNION ALL SELECT 5) SELECT v FROM c ORDER BY v DESC") == (Rows{{"10"}, {"5"}})); // a subquery
    REQUIRE(rows_of(ex, "WITH c AS (SELECT g, COALESCE(COUNT(*), 0) AS k FROM crowd GROUP BY g) SELECT g, k FROM c ORDER BY k DESC") == (Rows{{"1", "12"}, {"2", "3"}}));
    REQUIRE(rows_of(ex, "WITH c AS (SELECT id, COALESCE(MAX(DISTINCT id), 0) AS v FROM nums GROUP BY id) SELECT v FROM c ORDER BY v DESC") == descending);
    // what a pass-through gives out of a text is a text: '9' is above '10'
    const Rows as_text = {{"9"}, {"2"}, {"10"}};
    REQUIRE(rows_of(ex, "WITH c AS (SELECT NULLIF(code, 'x') AS v FROM nums) SELECT v FROM c ORDER BY v DESC") == as_text);
    REQUIRE(rows_of(ex, "WITH c AS (SELECT COALESCE(code, NULL) AS v FROM nums) SELECT v FROM c ORDER BY v DESC") == as_text);
    REQUIRE(rows_of(ex, "WITH c AS (SELECT id, NULLIF(MAX(code), '') AS v FROM nums GROUP BY id) SELECT v FROM c ORDER BY v DESC") == as_text);
    // ... and so is one that has a text among its choices, even when the row takes the number
    REQUIRE(rows_of(ex, "WITH c AS (SELECT IF(id > 1, id, 'none') AS v FROM nums) SELECT v FROM c ORDER BY v DESC") == as_text);
    REQUIRE(rows_of(ex, "WITH c AS (SELECT IF(id > 100, 'none', id) AS v FROM nums) SELECT v FROM c ORDER BY v DESC") == as_text);
    REQUIRE(rows_of(ex, "WITH c AS (SELECT CASE WHEN id > 1 THEN id ELSE 'small' END AS v FROM nums) SELECT v FROM c ORDER BY v DESC") == as_text);
    REQUIRE(rows_of(ex, "WITH c AS (SELECT CASE WHEN id > 100 THEN 'big' WHEN id > 1 THEN id END AS v FROM nums) SELECT v FROM c ORDER BY v DESC") == as_text);
    // CTE on CTE, and a recursive one
    REQUIRE(rows_of(ex, "WITH a AS (SELECT id FROM nums), b AS (SELECT id + 1 AS n FROM a) SELECT MAX(n) FROM b") == Rows{{"11"}});
    REQUIRE(rows_of(ex, "WITH RECURSIVE r AS (SELECT 1 AS n UNION ALL SELECT n + 1 FROM r WHERE n < 12) SELECT MAX(n) FROM r") == Rows{{"12"}});
    // text stays text: '10' sorts before '9' and 'ten' is no number
    REQUIRE(rows_of(ex, "WITH c AS (SELECT CONCAT(id, '') AS v FROM nums) SELECT v FROM c ORDER BY v DESC") == (Rows{{"9"}, {"2"}, {"10"}}));
    REQUIRE(rows_of(ex, "WITH c AS (SELECT COALESCE(name, 'x') AS v FROM nums) SELECT MAX(v) FROM c") == Rows{{"two"}});
}
