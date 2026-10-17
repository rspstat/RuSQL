#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// COUNT(DISTINCT a, b) counts the different pairs of values; a row with a NULL in the pair is not counted (MySQL). Pairs are equal when their parts are equal
// as MySQL compares them (texts by the default collation), and ('1', '11') is not ('11', '1').

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

// the data rows of a SELECT as "c1,c2;c1,c2" and its header line
std::string rows_of(Executor& ex, const std::string& sql, std::string* header = nullptr) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_ok());
    std::istringstream in(r.value());
    std::string line, out;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars == 1) {
            if (header) *header = line;
            continue;
        }
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
} // namespace

TEST_CASE("COUNT(DISTINCT a, b) counts the different pairs", "[count_distinct]") {
    TempDataDir dir("count_distinct_columns");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, a INT, b INT, s VARCHAR(5))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 1, 1, 'x'), (2, 1, 1, 'X'), (3, 1, 2, 'x'), (4, 2, 1, NULL), (5, NULL, 1, 'y'), (6, 1, NULL, 'y'), (7, 1, 1, 'x')").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE u (p INT, q INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO u VALUES (1, 11), (11, 1), (1, 11)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE e (a INT, b INT)").is_ok());
    // texts that hold what a join of the parts would use, and the same letter in other forms (the default collation does not tell 'e' from 'E' or from 'é')
    REQUIRE(ex.execute_sql("CREATE TABLE v (x VARCHAR(8), y VARCHAR(8))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO v VALUES ('a,b', 'c'), ('a', 'b,c'), ('a:1', '2'), ('a', '1:2')").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE w (x VARCHAR(8), y VARCHAR(8))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO w VALUES ('e', 'x'), ('\xC3\xA9', 'x'), ('E', 'X')").is_ok());

    std::string header;
    REQUIRE(rows_of(ex, "SELECT COUNT(DISTINCT a, b) FROM t", &header) == "3"); // (1,1) (1,2) (2,1); the rows with a NULL are not counted
    REQUIRE(header.find("COUNT(DISTINCT a, b)") != std::string::npos);
    REQUIRE(rows_of(ex, "SELECT COUNT(DISTINCT a, s) FROM t") == "2");           // (1,'x') = (1,'X') under the default collation, and (1,'y')
    REQUIRE(rows_of(ex, "SELECT COUNT(DISTINCT a, b, s) FROM t") == "2");
    REQUIRE(rows_of(ex, "SELECT COUNT(DISTINCT a + b, a) FROM t") == "3");       // expressions as parts
    REQUIRE(rows_of(ex, "SELECT COUNT(DISTINCT p, q) FROM u") == "2");           // (1, 11) and (11, 1) are two pairs
    REQUIRE(rows_of(ex, "SELECT COUNT(DISTINCT x, y) FROM v") == "4");
    REQUIRE(rows_of(ex, "SELECT COUNT(DISTINCT x, y) FROM w") == "1");
    REQUIRE(rows_of(ex, "SELECT COUNT(DISTINCT a, b) FROM e") == "0");
    REQUIRE(rows_of(ex, "SELECT COUNT(DISTINCT a) FROM t") == "2");              // one part: as before
    REQUIRE(rows_of(ex, "SELECT COUNT(DISTINCT a, b) AS pairs FROM t") == "3");
    REQUIRE(rows_of(ex, "SELECT COUNT(DISTINCT a, b) pairs FROM t") == "3");
    REQUIRE(rows_of(ex, "SELECT COUNT(DISTINCT a, b) + 1 FROM t") == "4");
    REQUIRE(rows_of(ex, "SELECT s, COUNT(DISTINCT a, b) FROM t GROUP BY s ORDER BY s") == "NULL,1;x,2;y,0");
    REQUIRE(rows_of(ex, "SELECT s FROM t GROUP BY s HAVING COUNT(DISTINCT a, b) > 1 ORDER BY s") == "x");
    REQUIRE(rows_of(ex, "SELECT s, COUNT(DISTINCT a, b) n FROM t GROUP BY s ORDER BY COUNT(DISTINCT a, b) DESC, s LIMIT 1") == "x,2");

    // only COUNT takes several columns after DISTINCT
    REQUIRE(ex.execute_sql("SELECT SUM(DISTINCT a, b) FROM t").is_err());
    REQUIRE(ex.execute_sql("SELECT SUM(DISTINCT a, b) + 1 FROM t").is_err());
    REQUIRE(ex.execute_sql("SELECT COUNT(a, b) FROM t").is_err());
    REQUIRE(ex.execute_sql("SELECT COUNT(DISTINCT a,) FROM t").is_err());
}
