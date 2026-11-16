// R91: AVG in an expression or a comparison. MySQL keeps the quotient of AVG with the digits of any division (the places of the sum + 4, up to a whole word of 9 digits,
// cut off: AVG over 1, 2, 2 holds 1.666666666) and only SHOWS the places of the argument + 4 (1.6667). So AVG(v) * 3 shows 5.0000, AVG(v) = 1.6667 is false and
// AVG(v) / 1 shows 1.66666667; AVG(x) OVER () is rounded to its shown places (its * 3 is 5.0001). The answers were measured on MySQL 8.4.9;
// code/test/diff/corpus/avg_expr_r91.sql repeats them against the server.

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

void make_table(Executor& ex) {
    ok(ex, "CREATE DATABASE v");
    ok(ex, "USE v");
    ok(ex, "CREATE TABLE av (id INT PRIMARY KEY, v INT, d DECIMAL(10,2), e DECIMAL(20,10), f DOUBLE, grp INT)");
    ok(ex, "INSERT INTO av VALUES (1, 1, 1.00, 1.0000000001, 1.5, 1), (2, 2, 1.00, 1.0000000001, 2.5, 1), (3, 2, 1.01, 1.0000000002, 3.5, 2), "
           "(4, 3, 2.50, 2.0, 4.5, 2), (5, 5, 0.33, 3.3333333333, 5.5, 2)");
}
} // namespace

TEST_CASE("R91: AVG in arithmetic goes on with the quotient MySQL holds, and shows the places of the argument + 4", "[r91]") {
    TempDataDir dir("r91_data_avg_arith");
    Executor ex(dir.path);
    make_table(ex);
    const std::string where = " FROM av WHERE id <= 3";
    CHECK(flat(ex, "SELECT AVG(v)" + where) == "1.6667"); // (the average alone shows its places)
    CHECK(flat(ex, "SELECT AVG(v) * 3" + where) == "5.0000"); // (1.666666666 * 3 = 4.999999998, shown with 4 places; it was 1.6667 * 3 = 5.0001)
    CHECK(flat(ex, "SELECT AVG(v) * 6" + where) == "10.0000");
    CHECK(flat(ex, "SELECT AVG(v) * 9" + where) == "15.0000");
    CHECK(flat(ex, "SELECT AVG(v) * 3 * 1000000000" + where) == "4999999998.0000");
    CHECK(flat(ex, "SELECT AVG(v) * 1000000000" + where) == "1666666666.0000");
    CHECK(flat(ex, "SELECT AVG(v) + AVG(v)" + where) == "3.3333");
    CHECK(flat(ex, "SELECT AVG(v) - 1" + where) == "0.6667");
    CHECK(flat(ex, "SELECT 5 - AVG(v) * 3" + where) == "0.0000");
    CHECK(flat(ex, "SELECT AVG(v) / 1" + where) == "1.66666667"); // (a quotient shows 4 places more)
    CHECK(flat(ex, "SELECT AVG(v) / 3" + where) == "0.55555556");
    CHECK(flat(ex, "SELECT AVG(v) / 3 * 3" + where) == "1.66666667");
    CHECK(flat(ex, "SELECT AVG(v) * AVG(v)" + where) == "2.77777778");
    CHECK(flat(ex, "SELECT AVG(v) * 3.0" + where) == "5.00000");
    CHECK(flat(ex, "SELECT AVG(v) * 3.00" + where) == "5.000000");
    // the functions that do arithmetic on the number they are given
    CHECK(flat(ex, "SELECT FLOOR(AVG(v) * 3)" + where) == "4");
    CHECK(flat(ex, "SELECT CEIL(AVG(v) * 3)" + where) == "5");
    CHECK(flat(ex, "SELECT ROUND(AVG(v) * 3, 2)" + where) == "5.00");
    CHECK(flat(ex, "SELECT ROUND(AVG(v), 3)" + where) == "1.667");
    // IFNULL / COALESCE / IF hand the average on
    CHECK(flat(ex, "SELECT IFNULL(AVG(v), 0) * 3" + where) == "5.0000");
    CHECK(flat(ex, "SELECT COALESCE(AVG(v), 0) * 3" + where) == "5.0000");
    CHECK(flat(ex, "SELECT IF(AVG(v) > 1, AVG(v), 0) * 3" + where) == "5.0000");
    // a window average is already rounded to its places
    CHECK(flat(ex, "SELECT AVG(v) OVER () * 3 FROM av WHERE id = 1 OR id = 2 OR id = 3 ORDER BY id").substr(0, 6) == "5.0001");
    // DECIMAL arguments: the places are those of the values + 4
    CHECK(flat(ex, "SELECT AVG(d)" + where) == "1.003333");
    CHECK(flat(ex, "SELECT AVG(d) * 3" + where) == "3.010000");
    CHECK(flat(ex, "SELECT AVG(d) * 1000000000" + where) == "1003333333.000000");
    CHECK(flat(ex, "SELECT AVG(d) + AVG(d)" + where) == "2.006667");
    CHECK(flat(ex, "SELECT AVG(e) * 1000000000" + where) == "1000000000.13333333300000");
    // a DOUBLE average is a double, not a decimal
    CHECK(flat(ex, "SELECT AVG(f) * 3 FROM av") == "10.5");
}

TEST_CASE("R91: AVG in a comparison, HAVING, ORDER BY and a subquery is the held quotient", "[r91]") {
    TempDataDir dir("r91_data_avg_compare");
    Executor ex(dir.path);
    make_table(ex);
    const std::string where = " FROM av WHERE id <= 3";
    CHECK(flat(ex, "SELECT AVG(v) = 1.6667" + where) == "0");
    CHECK(flat(ex, "SELECT AVG(v) <> 1.6667" + where) == "1");
    CHECK(flat(ex, "SELECT AVG(v) < 1.6667" + where) == "1");
    CHECK(flat(ex, "SELECT AVG(v) >= 1.6667" + where) == "0");
    CHECK(flat(ex, "SELECT AVG(v) > 1.6666" + where) == "1");
    CHECK(flat(ex, "SELECT AVG(v) = 5 / 3" + where) == "1"); // (both are 1.666666666)
    CHECK(flat(ex, "SELECT AVG(v) BETWEEN 1.6666 AND 1.6666667" + where) == "1");
    CHECK(flat(ex, "SELECT AVG(v) BETWEEN 1.6667 AND 1.7" + where) == "0");
    CHECK(flat(ex, "SELECT AVG(v) IN (1.6667)" + where) == "0");
    CHECK(flat(ex, "SELECT AVG(v) * 3 = 5" + where) == "0");
    CHECK(flat(ex, "SELECT AVG(v) * 3 < 5" + where) == "1");
    CHECK(flat(ex, "SELECT CASE WHEN AVG(v) = 1.6667 THEN 'a' ELSE 'b' END" + where) == "b");
    CHECK(flat(ex, "SELECT AVG(v) * 3" + where + " HAVING AVG(v) * 3 < 5") == "5.0000");
    CHECK(flat(ex, "SELECT AVG(v) * 3" + where + " HAVING AVG(v) * 3 > 5") == "");
    // groups: grp 1 holds 1 and 2 (1.5), grp 2 holds 2, 3 and 5 (3.333333333)
    CHECK(flat(ex, "SELECT grp, AVG(v) * 3 FROM av GROUP BY grp ORDER BY grp") == "1,4.5000;2,10.0000");
    CHECK(flat(ex, "SELECT grp, AVG(v) FROM av GROUP BY grp HAVING AVG(v) = 3.3333") == "");
    CHECK(flat(ex, "SELECT grp, AVG(v) FROM av GROUP BY grp HAVING AVG(v) < 3.3334 ORDER BY grp") == "1,1.5000;2,3.3333");
    CHECK(flat(ex, "SELECT grp, AVG(v) FROM av GROUP BY grp ORDER BY AVG(v) * 3 DESC") == "2,3.3333;1,1.5000");
    CHECK(flat(ex, "SELECT (SELECT AVG(v) * 3 FROM av WHERE id <= 3)") == "5.0000");
    CHECK(flat(ex, "SELECT * FROM (SELECT AVG(v) * 3 AS x FROM av WHERE id <= 3) q") == "5.0000");
    // the average kept as the table or the sum / count says
    CHECK(flat(ex, "SELECT SUM(v) / COUNT(*) = 5 / 3" + where) == "1");
    CHECK(flat(ex, "SELECT id FROM av WHERE v > (SELECT AVG(v) FROM av) ORDER BY id") == "4;5");
}

TEST_CASE("R91: an AVG that is not in an expression is still the text of its places, and an empty one is NULL", "[r91]") {
    TempDataDir dir("r91_data_avg_plain");
    Executor ex(dir.path);
    make_table(ex);
    CHECK(flat(ex, "SELECT AVG(v) FROM av") == "2.6000");
    CHECK(flat(ex, "SELECT AVG(DISTINCT v) FROM av") == "2.7500");
    CHECK(flat(ex, "SELECT grp, AVG(v) FROM av GROUP BY grp ORDER BY grp") == "1,1.5000;2,3.3333");
    CHECK(flat(ex, "SELECT AVG(v) FROM av WHERE id > 99") == "NULL");
    CHECK(flat(ex, "SELECT AVG(v) * 3 FROM av WHERE id > 99") == "NULL");
    // the same average in the list and in an expression
    CHECK(flat(ex, "SELECT AVG(v), AVG(v) * 3 FROM av WHERE id <= 3") == "1.6667,5.0000");
    ok(ex, "CREATE TABLE tmp_avg AS SELECT AVG(v) AS a FROM av WHERE id <= 3");
    CHECK(flat(ex, "SELECT a, a * 3 FROM tmp_avg") == "1.6667,5.0001"); // (a stored AVG is the 4 places it shows, as in MySQL)
}
