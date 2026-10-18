#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// How a number is written. DECIMAL and integer arithmetic keeps the places MySQL gives the result (a sum 2, a product the sum of both, a quotient 4 more),
// SUM and AVG keep the places of their values (AVG 4 more), and a FLOAT / DOUBLE column or a text (MySQL reads it as a double) is added and written the
// way a double is: the shortest text that reads back, with the noise of binary arithmetic (0.1 + 0.2 is 0.30000000000000004). Every expected text is
// what MySQL 8.4.9 answered to the same statement.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

// the rows of a SELECT as "c1,c2;c1,c2"
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
} // namespace

TEST_CASE("sums, averages and arithmetic of FLOAT, DOUBLE and text columns are doubles", "[decimal_display]") {
    TempDataDir dir("decimal_display_doubles");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE dd (id INT PRIMARY KEY, d DOUBLE, f FLOAT, vc VARCHAR(10), n INT)");
    ok(ex, "INSERT INTO dd VALUES (1, 0.1, 0.5, '10.50', 2), (2, 0.2, 0.25, '2', 3), (3, 0.3, 0.125, '1.5', 5)");

    // the select list
    REQUIRE(rows_of(ex, "SELECT SUM(d), AVG(d), SUM(f), AVG(f), SUM(vc), AVG(vc) FROM dd") == "0.6000000000000001,0.20000000000000004,0.875,0.2916666666666667,14,4.666666666666667");
    // an aggregate inside an expression or a comparison (the value goes on as the double it is: 0.6000000000000001 is not 0.6)
    REQUIRE(rows_of(ex, "SELECT SUM(d) * 10, SUM(d) = 0.6, SUM(d) + 1, AVG(d) * 3, MAX(d) + MIN(d) FROM dd") == "6.000000000000001,0,1.6,0.6000000000000001,0.4");
    REQUIRE(rows_of(ex, "SELECT n FROM dd GROUP BY n HAVING SUM(d) = 0.1 ORDER BY n") == "2");
    REQUIRE(rows_of(ex, "SELECT 1 FROM dd WHERE (SELECT SUM(d) FROM dd) = 0.6").empty());
    // a window
    REQUIRE(rows_of(ex, "SELECT id, SUM(d) OVER (), AVG(d) OVER (), SUM(vc) OVER (), AVG(f) OVER (), SUM(d) OVER (ORDER BY id) FROM dd ORDER BY id") ==
            "1,0.6000000000000001,0.20000000000000004,14,0.2916666666666667,0.1;"
            "2,0.6000000000000001,0.20000000000000004,14,0.2916666666666667,0.30000000000000004;"
            "3,0.6000000000000001,0.20000000000000004,14,0.2916666666666667,0.6000000000000001");
    // an expression as the argument: floating point when a column or a function in it is
    REQUIRE(rows_of(ex, "SELECT SUM(SQRT(n)), AVG(SQRT(n)), SUM(POW(n, 2)), SUM(n / 4), SUM(d * 2), AVG(d + vc) FROM dd") ==
            "5.382332347441762,1.7941107824805875,38,2.5000,1.2000000000000002,4.866666666666667");
    // groups
    REQUIRE(rows_of(ex, "SELECT n % 2 AS g, SUM(d), AVG(d), SUM(vc) FROM dd GROUP BY n % 2 ORDER BY g") == "0,0.1,0.1,10.5;1,0.5,0.25,3.5");
    // arithmetic on a text column or a string is a double (21, not 21.00)
    REQUIRE(rows_of(ex, "SELECT vc * 2, vc + 1, vc / 4, '2.5' * 2, d * 3, vc + vc FROM dd ORDER BY id") ==
            "21,11.5,2.625,5,0.30000000000000004,21;4,3,0.5,5,0.6000000000000001,4;3,2.5,0.375,5,0.8999999999999999,3");
}

TEST_CASE("DECIMAL arithmetic, sums and ROUND keep MySQL's places", "[decimal_display]") {
    TempDataDir dir("decimal_display_places");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE pd (id INT PRIMARY KEY, price DECIMAL(10,2), qty INT, rate DECIMAL(5,3))");
    ok(ex, "INSERT INTO pd VALUES (1, 10.50, 3, 1.250), (2, 20.25, 2, 0.500), (3, 5.00, 4, 2.000), (4, NULL, NULL, NULL)");
    REQUIRE(rows_of(ex, "SELECT SUM(price), AVG(price), MIN(price), MAX(price), SUM(qty), AVG(qty) FROM pd") == "35.75,11.916667,5.00,20.25,9,3.0000");
    REQUIRE(rows_of(ex, "SELECT price * 2, price + 1, price - 0.5, price / 4, price * qty, price * rate FROM pd WHERE id = 1") == "21.00,11.50,10.00,2.625000,31.50,13.12500");
    REQUIRE(rows_of(ex, "SELECT qty * 2, qty / 2, qty + 0.5, rate * 2, rate / 3, 10 / 4, 1.5 * 2, 100 * 1.10 FROM pd WHERE id = 1") == "6,1.5000,3.5,2.500,0.4166667,2.5000,3.0,110.00");
    REQUIRE(rows_of(ex, "SELECT SUM(price * qty), AVG(price * qty), SUM(price) / SUM(qty), SUM(price) / COUNT(*) FROM pd") == "92.00,30.666667,3.972222,8.937500");
    REQUIRE(rows_of(ex, "SELECT ROUND(price), ROUND(price, 1), ROUND(price / 3, 2), ROUND(rate, 1), ROUND(price, 4), ROUND(qty, 2), TRUNCATE(price, 1), TRUNCATE(price, 4) FROM pd WHERE id = 1") ==
            "11,10.5,3.50,1.3,10.50,3,10.5,10.50");
    REQUIRE(rows_of(ex, "SELECT ABS(-price), MOD(price, 3), price % 3, MOD(price, 0.5), MOD(-price, 3), MOD(qty, 2) FROM pd WHERE id = 1") == "10.50,1.50,1.50,0.00,-1.50,1");
    // a window and a group keep them too
    REQUIRE(rows_of(ex, "SELECT SUM(price) OVER (ORDER BY id), AVG(price) OVER (ORDER BY id) FROM pd WHERE id < 3 ORDER BY id") == "10.50,10.500000;30.75,15.375000");
    REQUIRE(rows_of(ex, "SELECT qty, SUM(price), AVG(price) FROM pd GROUP BY qty HAVING SUM(price) > 5 ORDER BY qty") == "2,20.25,20.250000;3,10.50,10.500000");
}
