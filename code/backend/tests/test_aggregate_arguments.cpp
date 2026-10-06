#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// The argument of an aggregate is any expression, not just a column: SUM(price * qty), AVG(a + b), COUNT(1), SUM(COALESCE(x, 0)), MAX(a * (b + 1)).
// Every row gets the value of the expression, and the aggregate reads it as it reads a column -- in the select list, in HAVING, in an
// expression over aggregates, as a window function, in a select without FROM, through table aliases, with a variable in it.
// Two conditional aggregates of one select list (SUM(a > 1), SUM(a > 4)) used to be the same result column, and showed one value twice.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

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

std::string text(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
    return r.value();
}

using Rows = std::vector<std::vector<std::string>>;
const std::string N = "NULL";

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

Rows q(Executor& ex, const std::string& sql) { return cells(text(ex, sql)); }

// the names of the result columns
std::vector<std::string> headers(Executor& ex, const std::string& sql) {
    std::istringstream in(text(ex, sql));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        std::vector<std::string> names;
        std::size_t pos = 1;
        while (pos < line.size()) {
            std::size_t bar = line.find('|', pos);
            if (bar == std::string::npos) break;
            std::string v = line.substr(pos, bar - pos);
            v.erase(0, v.find_first_not_of(' '));
            v.erase(v.find_last_not_of(' ') + 1);
            names.push_back(v);
            pos = bar + 1;
        }
        return names;
    }
    return {};
}

std::string one(Executor& ex, const std::string& sql) {
    Rows rows = q(ex, sql);
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].size() == 1);
    return rows[0][0];
}

// a number as the engine prints it (65.5, 65.5000, 40) against the number it should be
bool is_number(const std::string& got, double want) {
    if (got == N) return false;
    char* end = nullptr;
    const double v = std::strtod(got.c_str(), &end);
    return end != got.c_str() && *end == '\0' && std::fabs(v - want) < 1e-9;
}

std::string error_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_err());
    return r.error();
}

void orders(Executor& ex) {
    ok(ex, "CREATE TABLE o (id INT PRIMARY KEY, price DECIMAL(10,2), qty INT, grp INT, a INT, b INT, s VARCHAR(10))");
    ok(ex, "INSERT INTO o VALUES (1, 2.50, 4, 1, 1, 2, 'x'), (2, 10.00, 3, 1, 3, NULL, 'yy'), (3, 0.99, 10, 2, 5, 6, 'zzz'), (4, 7.25, 2, 2, NULL, 1, NULL), "
           "(5, 1.10, 1, 3, 2, 2, 'x')");
}
} // namespace

TEST_CASE("an aggregate takes an expression as its argument", "[aggregate_arguments]") {
    TempDataDir dir("agg_args_basic");
    Executor ex(dir.path);
    open_db(ex);
    orders(ex);
    // a product of two columns, per row and then summed (10 + 30 + 9.9 + 14.5 + 1.1)
    REQUIRE(is_number(one(ex, "SELECT SUM(price * qty) FROM o"), 65.5));
    REQUIRE(q(ex, "SELECT SUM(price * qty) AS revenue, AVG(a + b) AS m FROM o")[0][1] == "6.0000"); // a + b: 3, NULL, 11, NULL, 4
    Rows by_group = q(ex, "SELECT grp, SUM(price * qty), COUNT(1), MAX(a + b), MIN(price * qty) FROM o GROUP BY grp ORDER BY grp");
    REQUIRE(by_group.size() == 3);
    const double sums[] = {40, 24.4, 1.1}, mins[] = {10, 9.9, 1.1};
    const char* counts[] = {"2", "2", "1"};
    const char* maxima[] = {"3", "11", "4"};
    for (int i = 0; i < 3; i++) {
        INFO("group " << i + 1);
        REQUIRE(is_number(by_group[i][1], sums[i]));
        REQUIRE(by_group[i][2] == counts[i]);
        REQUIRE(by_group[i][3] == maxima[i]);
        REQUIRE(is_number(by_group[i][4], mins[i]));
    }
    // a function and a parenthesized sum inside it
    Rows more = q(ex, "SELECT grp, SUM(COALESCE(b, 0)), SUM((a + 1) * 2) FROM o GROUP BY grp ORDER BY grp");
    REQUIRE(more == Rows{{"1", "2", "12"}, {"2", "7", "12"}, {"3", "2", "6"}});
    // the grouping of the expression is kept: (a + b) * 2 is not a + b * 2
    REQUIRE(one(ex, "SELECT SUM((a + b) * 2) FROM o") == "36"); // (1+2)*2 + (5+6)*2 + (2+2)*2 (the rows with a NULL are skipped)
    REQUIRE(one(ex, "SELECT SUM(a + b * 2) FROM o") == "28");   // 1+4 + 5+12 + 2+4
    REQUIRE(one(ex, "SELECT SUM(a - (b - 1)) FROM o") == "1");  // 1-1 + 5-5 + 2-1
    // the result column is named as the statement wrote the expression (grouping included)
    REQUIRE(headers(ex, "SELECT SUM(a + (b - 1)), SUM(a * (b * 2)), SUM(a - (b - 1)), SUM((a + b) * 2), SUM(a + b * 2), SUM(a / (b / 2)) FROM o") ==
            std::vector<std::string>{"SUM(a + (b - 1))", "SUM(a * (b * 2))", "SUM(a - (b - 1))", "SUM((a + b) * 2)", "SUM(a + b * 2)", "SUM(a / (b / 2))"});
    REQUIRE(one(ex, "SELECT SUM(12 / (2 * 3)) FROM o") == "10");  // 12 / 6 for each of the 5 rows, not 12 / 2 * 3
    // constants and NULL
    Rows constants = q(ex, "SELECT COUNT(1), COUNT(*), SUM(2), COUNT('x'), COUNT(NULL), SUM(2 * 3) FROM o");
    REQUIRE(constants == Rows{{"5", "5", "10", "5", "0", "30"}});
    // DISTINCT of an expression
    ok(ex, "INSERT INTO o VALUES (6, 1.00, 1, 3, 1, 2, 'q'), (7, 1.00, 1, 3, 2, 1, 'q'), (8, 1.00, 1, 3, 3, 0, 'q')");
    REQUIRE(q(ex, "SELECT grp, COUNT(DISTINCT a + b) FROM o GROUP BY grp ORDER BY grp") == Rows{{"1", "1"}, {"2", "1"}, {"3", "2"}});
    REQUIRE(one(ex, "SELECT COUNT(DISTINCT a + b) FROM o") == "3"); // 3, 11 and 4
    REQUIRE(q(ex, "SELECT grp FROM o GROUP BY grp HAVING COUNT(DISTINCT a + b) = 2 ORDER BY grp") == Rows{{"3"}}); // (grp 3 has 4 values: 4, 3, 3, 3)
    REQUIRE(q(ex, "SELECT grp FROM o GROUP BY grp HAVING COUNT(a + b) = 4 ORDER BY grp") == Rows{{"3"}});
}

TEST_CASE("the aggregate of an expression in HAVING, in expressions over aggregates and as a window function", "[aggregate_arguments]") {
    TempDataDir dir("agg_args_having");
    Executor ex(dir.path);
    open_db(ex);
    orders(ex);
    REQUIRE(q(ex, "SELECT grp FROM o GROUP BY grp HAVING SUM(price * qty) > 20 ORDER BY grp") == Rows{{"1"}, {"2"}});
    REQUIRE(q(ex, "SELECT grp, SUM(price * qty) AS rev FROM o GROUP BY grp HAVING rev > 20 ORDER BY grp").size() == 2);
    REQUIRE(q(ex, "SELECT grp FROM o GROUP BY grp HAVING COUNT(DISTINCT a + b) = 1 AND MAX(a * b) > 5 ORDER BY grp") == Rows{{"2"}});
    REQUIRE(is_number(one(ex, "SELECT SUM(price * qty) / COUNT(*) FROM o"), 13.1));
    REQUIRE(is_number(one(ex, "SELECT SUM(price * qty) - SUM(price) AS d FROM o"), 43.66));
    // a window function over an expression
    Rows partition = q(ex, "SELECT grp, SUM(price * qty) OVER (PARTITION BY grp) FROM o ORDER BY id");
    const double want[] = {40, 40, 24.4, 24.4, 1.1};
    REQUIRE(partition.size() == 5);
    for (int i = 0; i < 5; i++) REQUIRE(is_number(partition[i][1], want[i]));
    Rows running = q(ex, "SELECT id, SUM(price * qty) OVER (ORDER BY id) FROM o ORDER BY id");
    const double total[] = {10, 40, 49.9, 64.4, 65.5};
    REQUIRE(running.size() == 5);
    for (int i = 0; i < 5; i++) REQUIRE(is_number(running[i][1], total[i]));
    // a subquery
    REQUIRE(is_number(one(ex, "SELECT (SELECT SUM(price * qty) FROM o WHERE grp = 1) AS g1"), 40));
    REQUIRE(q(ex, "SELECT id FROM o WHERE price * qty > (SELECT AVG(price * qty) FROM o) ORDER BY id") == Rows{{"2"}, {"4"}});
}

TEST_CASE("the aggregate of an expression: NULL, no rows, GROUP_CONCAT, MIN and MAX of text, a select without FROM", "[aggregate_arguments]") {
    TempDataDir dir("agg_args_nulls");
    Executor ex(dir.path);
    open_db(ex);
    orders(ex);
    REQUIRE(one(ex, "SELECT SUM(price * qty) FROM o WHERE id > 100") == N);
    REQUIRE(one(ex, "SELECT AVG(a * b) FROM o WHERE id > 100") == N);
    REQUIRE(one(ex, "SELECT COUNT(a * b) FROM o WHERE id > 100") == "0");
    // a group where the expression is NULL for every row
    ok(ex, "INSERT INTO o VALUES (9, 1.00, 1, 4, NULL, 5, 'n'), (10, 1.00, 1, 4, 7, NULL, 'n')");
    REQUIRE(q(ex, "SELECT grp, SUM(a * b), COUNT(a * b) FROM o WHERE grp = 4 GROUP BY grp") == Rows{{"4", N, "0"}});
    REQUIRE(one(ex, "SELECT GROUP_CONCAT(CONCAT(s, '-', a)) FROM o WHERE grp < 4") == "x-1,yy-3,zzz-5,x-2"); // (a NULL s gives NULL, which is skipped)
    REQUIRE(q(ex, "SELECT MIN(CONCAT(s, 'z')), MAX(CONCAT(s, 'z')) FROM o WHERE grp < 4") == Rows{{"xz", "zzzz"}});
    REQUIRE(q(ex, "SELECT SUM(2 * 3), COUNT(1 + 1)") == Rows{{"6", "1"}});
    // a string with a quote in it, and what an expression holds (texts compare as texts: '9' is after '10')
    REQUIRE(one(ex, "SELECT MAX(CONCAT(s, 'o''k')) FROM o WHERE grp < 4") == "zzzo'k");
    ok(ex, "CREATE TABLE nn (id INT PRIMARY KEY, n INT)");
    ok(ex, "INSERT INTO nn VALUES (1, 9), (2, 10)");
    REQUIRE(q(ex, "SELECT MAX(CONCAT(n, '')), MIN(CONCAT(n, '')), MAX(n * 1), MIN(n * 1) FROM nn") == Rows{{"9", "10", "10", "9"}});
    REQUIRE(error_of(ex, "SELECT SUM(nosuch * qty) FROM o").find("Unknown column 'nosuch'") != std::string::npos);
    REQUIRE(error_of(ex, "SELECT grp, AVG(qty + nosuch) OVER (PARTITION BY grp) FROM o").find("Unknown column 'nosuch'") != std::string::npos);
}

TEST_CASE("the aggregate of an expression through table aliases, in a view, with a variable", "[aggregate_arguments]") {
    TempDataDir dir("agg_args_views");
    {
        Executor ex(dir.path);
        open_db(ex);
        orders(ex);
        ok(ex, "CREATE TABLE cust (cid INT PRIMARY KEY, rate INT)");
        ok(ex, "INSERT INTO cust VALUES (1, 2), (2, 3), (3, 5)");
        // 2.5*2 + 10*2 + 0.99*3 + 7.25*3 + 1.1*5
        REQUIRE(is_number(one(ex, "SELECT SUM(x.price * c.rate) FROM o x JOIN cust c ON x.grp = c.cid"), 55.22));
        REQUIRE(is_number(one(ex, "SELECT SUM(o.price * cust.rate) FROM o JOIN cust ON o.grp = cust.cid"), 55.22));
        Rows per = q(ex, "SELECT c.cid, SUM(x.price * x.qty * c.rate) FROM o x JOIN cust c ON x.grp = c.cid GROUP BY c.cid ORDER BY c.cid");
        REQUIRE(per.size() == 3);
        REQUIRE(is_number(per[0][1], 80));    // (10 + 30) * 2
        REQUIRE(is_number(per[1][1], 73.2));  // (9.9 + 14.5) * 3
        REQUIRE(is_number(per[2][1], 5.5));   // 1.1 * 5
        // a variable in the expression
        ok(ex, "SET @rate = 2");
        REQUIRE(is_number(one(ex, "SELECT SUM(price * @rate) FROM o"), 43.68));
        ok(ex, "CREATE VIEW revenue AS SELECT grp, SUM(price * qty) AS rev, COUNT(1) AS n FROM o GROUP BY grp");
        Rows view = q(ex, "SELECT grp, rev, n FROM revenue ORDER BY grp");
        REQUIRE(view.size() == 3);
        REQUIRE(is_number(view[1][1], 24.4));
        ok(ex, "CHECKPOINT");
    }
    // the view reads its expression from what was saved
    Executor ex(dir.path);
    ok(ex, "USE d");
    Rows view = q(ex, "SELECT grp, rev, n FROM revenue ORDER BY grp");
    REQUIRE(view.size() == 3);
    REQUIRE(is_number(view[0][1], 40));
    REQUIRE(view[2][2] == "1");
}

TEST_CASE("the text of an expression keeps its grouping: a function argument is not re-read as another expression", "[aggregate_arguments][grouping]") {
    TempDataDir dir("agg_args_grouping");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE o (id INT PRIMARY KEY, a INT, b INT)");
    ok(ex, "INSERT INTO o VALUES (1, 1, 2), (2, 3, 4)");
    // ROUND((a + b) * 2, 1) was computed as a + (b * 2)
    REQUIRE(q(ex, "SELECT id, ROUND((a + b) * 2, 1), ABS((a - b) * 2), COALESCE(NULL, (a + b) * 2) FROM o ORDER BY id") ==
            Rows{{"1", "6", "2", "6"}, {"2", "14", "2", "14"}});
    REQUIRE(q(ex, "SELECT id, ROUND(a - (b - 1), 1), ROUND(10 / (a + b), 2) FROM o ORDER BY id")[1][1] == "0"); // 3 - (4 - 1)
}

TEST_CASE("DISTINCT compares numbers by value and texts by their text", "[aggregate_arguments][distinct]") {
    TempDataDir dir("agg_args_distinct");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE d (id INT PRIMARY KEY, w DECIMAL(10,2), s VARCHAR(10), n INT)");
    ok(ex, "INSERT INTO d VALUES (1, 7.00, '7', 7), (2, NULL, '07', 7), (3, 8.50, '7', 8), (4, NULL, '8', 9)");
    // COALESCE(w, 7) is "7.00", "7", "8.50", "7": two numbers, not three texts
    REQUIRE(one(ex, "SELECT COUNT(DISTINCT COALESCE(w, 7)) FROM d") == "2");
    REQUIRE(is_number(one(ex, "SELECT SUM(DISTINCT COALESCE(w, 7)) FROM d"), 15.5));
    REQUIRE(is_number(one(ex, "SELECT AVG(DISTINCT COALESCE(w, 7)) FROM d"), 7.75));
    REQUIRE(one(ex, "SELECT COUNT(DISTINCT w * 2) FROM d") == "2");
    // (the same through HAVING: group n = 7 holds "7.00" and "7", one number)
    REQUIRE(q(ex, "SELECT n FROM d GROUP BY n HAVING COUNT(DISTINCT COALESCE(w, 7)) = 1 ORDER BY n") == Rows{{"7"}, {"8"}, {"9"}});
    REQUIRE(q(ex, "SELECT n, COUNT(DISTINCT COALESCE(w, 7)) FROM d GROUP BY n ORDER BY n") == Rows{{"7", "1"}, {"8", "1"}, {"9", "1"}});
    // texts: '7' and '07' are two strings
    REQUIRE(one(ex, "SELECT COUNT(DISTINCT s) FROM d") == "3");
    REQUIRE(one(ex, "SELECT COUNT(DISTINCT CONCAT(s, '')) FROM d") == "3");
    REQUIRE(q(ex, "SELECT n FROM d GROUP BY n HAVING COUNT(DISTINCT s) = 2 ORDER BY n") == Rows{{"7"}});
    REQUIRE(one(ex, "SELECT COUNT(DISTINCT n) FROM d") == "3");
}

TEST_CASE("conditional aggregates of one select list are separate result columns", "[aggregate_arguments][case_labels]") {
    TempDataDir dir("agg_args_case");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE o (id INT PRIMARY KEY, a INT, g INT)");
    ok(ex, "INSERT INTO o VALUES (1, 1, 1), (2, 3, 1), (3, 5, 2), (4, NULL, 2), (5, 2, 2)");
    REQUIRE(q(ex, "SELECT SUM(CASE WHEN a > 1 THEN 1 ELSE 0 END), SUM(CASE WHEN a > 4 THEN 1 ELSE 0 END), SUM(CASE WHEN a > 0 THEN 1 ELSE 0 END) FROM o") ==
            Rows{{"3", "1", "4"}});
    REQUIRE(q(ex, "SELECT g, SUM(a > 1), SUM(a > 4), COUNT(CASE WHEN a > 1 THEN 1 END), COUNT(CASE WHEN a > 4 THEN 1 END) FROM o GROUP BY g ORDER BY g") ==
            Rows{{"1", "1", "0", "1", "0"}, {"2", "2", "1", "2", "1"}});
    // (with an alias each is its own column as before)
    REQUIRE(q(ex, "SELECT SUM(a > 1) AS x, SUM(a > 4) AS y FROM o") == Rows{{"3", "1"}});
    // SUM(expression > value): the predicate's left side is an expression
    REQUIRE(q(ex, "SELECT SUM(a * 2 > 5), SUM(a + 1 > 5), SUM(a * 2 IN (2, 6)) FROM o") == Rows{{"2", "1", "2"}});
}

// ---- a random table and random expressions against a reference

namespace {
using Cell = std::optional<long long>;
using Env = std::array<Cell, 3>; // x, y, z

struct Expr {
    std::string sql;
    int strength; // how tightly the outermost operator binds: + - 1, * 2, anything else 3
    std::function<Cell(const Env&)> eval;
};

Expr make_expr(std::mt19937& rng, int depth) {
    const int kind = depth <= 0 ? static_cast<int>(rng() % 2) : static_cast<int>(rng() % 6);
    if (kind == 0) {
        const int c = static_cast<int>(rng() % 3);
        const char* names[] = {"x", "y", "z"};
        return {names[c], 3, [c](const Env& e) { return e[static_cast<std::size_t>(c)]; }};
    }
    if (kind == 1) {
        const long long k = static_cast<long long>(rng() % 10);
        return {std::to_string(k), 3, [k](const Env&) { return Cell(k); }};
    }
    if (kind == 2) { // COALESCE(e, k)
        Expr inner = make_expr(rng, depth - 1);
        const long long k = static_cast<long long>(rng() % 10);
        return {"COALESCE(" + inner.sql + ", " + std::to_string(k) + ")", 3, [inner, k](const Env& e) {
                    Cell v = inner.eval(e);
                    return v ? v : Cell(k);
                }};
    }
    Expr l = make_expr(rng, depth - 1), r = make_expr(rng, depth - 1);
    const char op = "+-*+-*"[kind % 6];
    const int strength = op == '*' ? 2 : 1;
    // an operand needs parentheses when it binds less tightly than this operator (and, on the right, as tightly: a - (b - c))
    auto wrap = [](const Expr& e, int needs) { return e.strength < needs ? "(" + e.sql + ")" : e.sql; };
    const std::string sql = wrap(l, strength) + " " + op + " " + wrap(r, strength + 1);
    return {sql, strength, [l, r, op](const Env& e) -> Cell {
                Cell a = l.eval(e), b = r.eval(e);
                if (!a || !b) return std::nullopt;
                return op == '+' ? *a + *b : (op == '-' ? *a - *b : *a * *b);
            }};
}

std::string cell_text(const Cell& c) { return c ? std::to_string(*c) : N; }

// the AVG of the numbers, exactly, rounded half away from zero to 4 places (as the select list prints it)
std::string avg_text(long long sum, long long count) {
    if (count == 0) return N;
    long long scaled = sum * 10000;
    long long q = scaled / count, rem = scaled % count;
    if (std::llabs(rem) * 2 >= count) q += scaled < 0 ? -1 : 1;
    const bool neg = q < 0;
    long long a = std::llabs(q);
    std::string frac = std::to_string(a % 10000);
    frac = std::string(4 - frac.size(), '0') + frac;
    return std::string(neg ? "-" : "") + std::to_string(a / 10000) + "." + frac;
}
} // namespace

TEST_CASE("aggregates of random expressions match a reference, with GROUP BY and HAVING", "[aggregate_arguments][random]") {
    unsigned seed_count = 6; // RUSQL_FUZZ_SEEDS=60 runs a longer campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 1;
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    for (unsigned seed = seed_start; seed < seed_start + seed_count; seed++) {
        INFO("seed " << seed);
        std::mt19937 rng(seed);
        TempDataDir dir("agg_args_random");
        Executor ex(dir.path);
        open_db(ex);
        ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, g INT, x INT, y INT, z INT)");
        struct Line { int g; Env env; };
        std::vector<Line> table;
        std::string values;
        for (int i = 0; i < 60; i++) {
            Line r;
            r.g = static_cast<int>(rng() % 4);
            std::string line = "(" + std::to_string(i) + ", " + std::to_string(r.g);
            for (std::size_t c = 0; c < 3; c++) {
                if (rng() % 6 == 0) {
                    r.env[c] = std::nullopt;
                    line += ", NULL";
                } else {
                    r.env[c] = static_cast<long long>(rng() % 21) - 5;
                    line += ", " + std::to_string(*r.env[c]);
                }
            }
            table.push_back(r);
            values += (i ? ", " : "") + line + ")";
        }
        ok(ex, "INSERT INTO t VALUES " + values);
        for (int round = 0; round < 12; round++) {
            Expr e = make_expr(rng, 3);
            const int threshold = static_cast<int>(rng() % 40) - 10;
            INFO("expression " << e.sql << ", HAVING > " << threshold);
            // the whole table
            {
                long long sum = 0, count = 0;
                Cell lo, hi;
                std::set<long long> distinct;
                for (auto& r : table) {
                    Cell v = e.eval(r.env);
                    if (!v) continue;
                    sum += *v;
                    count++;
                    lo = lo ? std::min(*lo, *v) : *v;
                    hi = hi ? std::max(*hi, *v) : *v;
                    distinct.insert(*v);
                }
                Rows got = q(ex, "SELECT SUM(" + e.sql + "), COUNT(" + e.sql + "), MIN(" + e.sql + "), MAX(" + e.sql + "), AVG(" + e.sql + "), COUNT(DISTINCT " + e.sql + ") FROM t");
                REQUIRE(got == Rows{{count ? std::to_string(sum) : N, std::to_string(count), cell_text(lo), cell_text(hi), avg_text(sum, count), std::to_string(distinct.size())}});
            }
            // per group, and the groups HAVING keeps
            Rows expected, kept;
            for (int g = 0; g < 4; g++) {
                long long sum = 0, count = 0;
                Cell lo, hi;
                bool any_row = false;
                for (auto& r : table) {
                    if (r.g != g) continue;
                    any_row = true;
                    Cell v = e.eval(r.env);
                    if (!v) continue;
                    sum += *v;
                    count++;
                    lo = lo ? std::min(*lo, *v) : *v;
                    hi = hi ? std::max(*hi, *v) : *v;
                }
                if (!any_row) continue;
                expected.push_back({std::to_string(g), count ? std::to_string(sum) : N, std::to_string(count), cell_text(lo), cell_text(hi)});
                if (count && sum > threshold) kept.push_back({std::to_string(g), std::to_string(sum)});
            }
            REQUIRE(q(ex, "SELECT g, SUM(" + e.sql + "), COUNT(" + e.sql + "), MIN(" + e.sql + "), MAX(" + e.sql + ") FROM t GROUP BY g ORDER BY g") == expected);
            REQUIRE(q(ex, "SELECT g, SUM(" + e.sql + ") FROM t GROUP BY g HAVING SUM(" + e.sql + ") > " + std::to_string(threshold) + " ORDER BY g") == kept);
        }
    }
}
