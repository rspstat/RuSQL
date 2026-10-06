#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// A scalar subquery is one value: with two rows the statement fails ("Subquery returns more than 1 row", MySQL 1242), with two columns it fails
// before it runs ("Operand should contain 1 column(s)", MySQL 1241), with no row it is NULL. An error inside a subquery is the statement's error
// (it used to read as "no row", FALSE), HAVING compares with a subquery like WHERE does (it used to answer FALSE for every comparison), and a
// statement that fails this way has changed nothing.

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

using Rows = std::vector<std::vector<std::string>>;
const std::string N = "NULL";
const std::string ROWS = "Subquery returns more than 1 row";
const std::string COLUMNS = "Operand should contain 1 column(s)";

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
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
    return cells(r.value());
}

// the error a statement fails with
std::string error_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_err());
    return r.error();
}

void tables(Executor& ex) {
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, v INT, g INT)");
    ok(ex, "CREATE TABLE u (id INT PRIMARY KEY, a INT, b INT, g INT)");
    ok(ex, "INSERT INTO t VALUES (1, 10, 1), (2, 20, 1), (3, 30, 2)");
    ok(ex, "INSERT INTO u VALUES (1, 10, 100, 1), (2, 20, 200, 1), (3, 20, 300, 2)");
}
} // namespace

TEST_CASE("a scalar subquery that returns two rows is an error, in every comparison and every clause", "[subquery_cardinality]") {
    TempDataDir dir("sq_card_rows");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    for (const char* op : {"=", "<>", "<", "<=", ">", ">="}) {
        INFO(op);
        REQUIRE(error_of(ex, std::string("SELECT id FROM t WHERE v ") + op + " (SELECT a FROM u)") == ROWS);
        REQUIRE(error_of(ex, std::string("SELECT g FROM t GROUP BY g HAVING SUM(v) ") + op + " (SELECT a FROM u)") == ROWS);
    }
    REQUIRE(error_of(ex, "SELECT id, (SELECT a FROM u) AS x FROM t") == ROWS);
    REQUIRE(error_of(ex, "SELECT (SELECT a FROM u)") == ROWS);
    // a statement that fails changes nothing
    REQUIRE(error_of(ex, "UPDATE t SET v = 0 WHERE v = (SELECT a FROM u)") == ROWS);
    REQUIRE(error_of(ex, "DELETE FROM t WHERE v = (SELECT a FROM u)") == ROWS);
    REQUIRE(q(ex, "SELECT id, v FROM t ORDER BY id") == Rows{{"1", "10"}, {"2", "20"}, {"3", "30"}});
    // one row, no row, a NULL, LIMIT 1: fine
    REQUIRE(q(ex, "SELECT id FROM t WHERE v = (SELECT a FROM u WHERE id = 1)") == Rows{{"1"}});
    REQUIRE(q(ex, "SELECT id FROM t WHERE v > (SELECT a FROM u WHERE id > 100)").empty());
    REQUIRE(q(ex, "SELECT id FROM t WHERE v = (SELECT a FROM u ORDER BY id LIMIT 1)") == Rows{{"1"}});
    REQUIRE(q(ex, "SELECT id, (SELECT a FROM u WHERE id > 100) AS x FROM t WHERE id = 1") == Rows{{"1", N}});
    REQUIRE(q(ex, "SELECT id FROM t WHERE v = (SELECT MAX(a) FROM u)") == Rows{{"2"}});
    // the subquery is not run when no row needs it
    REQUIRE(q(ex, "SELECT id, (SELECT a FROM u) AS x FROM t WHERE id > 100").empty());
    ok(ex, "CREATE TABLE empty_t (id INT PRIMARY KEY, v INT)");
    REQUIRE(q(ex, "SELECT id FROM empty_t WHERE v = (SELECT a FROM u)").empty());
    ok(ex, "DELETE FROM empty_t WHERE v = (SELECT a FROM u)");
}

TEST_CASE("a subquery that returns two columns is an error even when it returns no row", "[subquery_cardinality]") {
    TempDataDir dir("sq_card_columns");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    REQUIRE(error_of(ex, "SELECT id FROM t WHERE v = (SELECT a, b FROM u WHERE id = 1)") == COLUMNS);
    REQUIRE(error_of(ex, "SELECT id FROM t WHERE v = (SELECT a, b FROM u WHERE id > 100)") == COLUMNS);
    REQUIRE(error_of(ex, "SELECT id FROM t WHERE v IN (SELECT a, b FROM u)") == COLUMNS);
    REQUIRE(error_of(ex, "SELECT id FROM t WHERE v NOT IN (SELECT a, b FROM u)") == COLUMNS);
    REQUIRE(error_of(ex, "SELECT id FROM t WHERE v = (SELECT * FROM u WHERE id = 1)") == COLUMNS);
    REQUIRE(error_of(ex, "SELECT id, (SELECT a, b FROM u WHERE id = 1) AS x FROM t") == COLUMNS);
    REQUIRE(error_of(ex, "SELECT id FROM t WHERE id IN (SELECT id FROM u WHERE a = (SELECT a, b FROM u WHERE id = 1))") == COLUMNS);
    REQUIRE(error_of(ex, "UPDATE t SET v = 0 WHERE v IN (SELECT a, b FROM u)") == COLUMNS);
    REQUIRE(error_of(ex, "DELETE FROM t WHERE v IN (SELECT a, b FROM u)") == COLUMNS);
    REQUIRE(q(ex, "SELECT COUNT(*) FROM t") == Rows{{"3"}});
    // EXISTS does not care how many columns; one column is fine everywhere
    REQUIRE(q(ex, "SELECT id FROM t WHERE EXISTS (SELECT a, b FROM u WHERE u.g = t.g) ORDER BY id") == Rows{{"1"}, {"2"}, {"3"}});
    REQUIRE(q(ex, "SELECT id FROM t WHERE v IN (SELECT a FROM u) ORDER BY id") == Rows{{"1"}, {"2"}});
    REQUIRE(q(ex, "SELECT id FROM t WHERE v NOT IN (SELECT a FROM u) ORDER BY id") == Rows{{"3"}});
}

TEST_CASE("a correlated subquery fails when a row that is looked at has two matches", "[subquery_cardinality]") {
    TempDataDir dir("sq_card_correlated");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex); // u has two rows with g = 1 and one with g = 2
    REQUIRE(error_of(ex, "SELECT id FROM t WHERE v = (SELECT a FROM u WHERE u.g = t.g)") == ROWS);
    REQUIRE(error_of(ex, "SELECT id, (SELECT a FROM u WHERE u.g = t.g) AS x FROM t") == ROWS);
    // only the rows of g = 2 are looked at
    REQUIRE(q(ex, "SELECT id FROM t WHERE g = 2 AND v = (SELECT a FROM u WHERE u.g = t.g)").empty());
    REQUIRE(q(ex, "SELECT id, (SELECT a FROM u WHERE u.g = t.g) AS x FROM t WHERE g = 2") == Rows{{"3", "20"}});
    ok(ex, "UPDATE t SET v = 99 WHERE g = 2 AND v = (SELECT a + 10 FROM u WHERE u.g = t.g)");
    REQUIRE(q(ex, "SELECT id, v FROM t ORDER BY id") == Rows{{"1", "10"}, {"2", "20"}, {"3", "99"}}); // (the g = 2 row: its one match, 20 + 10, is its v)
}

TEST_CASE("an error inside a subquery is the statement's error", "[subquery_cardinality]") {
    TempDataDir dir("sq_card_inner");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    // the inner subquery returns two rows: it used to read as "no row", so IN and EXISTS answered FALSE
    REQUIRE(error_of(ex, "SELECT id FROM t WHERE v IN (SELECT a FROM u WHERE b = (SELECT b FROM u))") == ROWS);
    REQUIRE(error_of(ex, "SELECT id FROM t WHERE v NOT IN (SELECT a FROM u WHERE b = (SELECT b FROM u))") == ROWS);
    REQUIRE(error_of(ex, "SELECT id FROM t WHERE EXISTS (SELECT 1 FROM u WHERE b = (SELECT b FROM u))") == ROWS);
    REQUIRE(error_of(ex, "SELECT id FROM t WHERE NOT EXISTS (SELECT 1 FROM u WHERE b = (SELECT b FROM u))") == ROWS);
    REQUIRE(error_of(ex, "SELECT id FROM t WHERE v = (SELECT a FROM u WHERE b = (SELECT b FROM u))") == ROWS);
    REQUIRE(error_of(ex, "SELECT id FROM t WHERE v > (SELECT AVG(a) FROM u WHERE b = (SELECT b FROM u))") == ROWS);
    // the executor is usable afterwards
    REQUIRE(q(ex, "SELECT COUNT(*) FROM t WHERE v IN (SELECT a FROM u)") == Rows{{"2"}});
}

TEST_CASE("a subquery that fails is the statement's error: a table that does not exist", "[subquery_cardinality]") {
    TempDataDir dir("sq_card_missing");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    const std::string missing = "Table 'd.nosuch' not found";
    // (the error used to be "no row": IN and EXISTS answered FALSE, a scalar subquery NULL, UPDATE and DELETE did nothing and said "0 rows")
    for (const char* sql : {"SELECT id FROM t WHERE v IN (SELECT a FROM nosuch)", "SELECT id FROM t WHERE v NOT IN (SELECT a FROM nosuch)",
                            "SELECT id FROM t WHERE v = (SELECT a FROM nosuch)", "SELECT id FROM t WHERE EXISTS (SELECT 1 FROM nosuch)",
                            "SELECT id FROM t WHERE NOT EXISTS (SELECT 1 FROM nosuch)", "SELECT id, (SELECT a FROM nosuch) AS x FROM t",
                            "SELECT id FROM t WHERE v = (SELECT a FROM nosuch WHERE nosuch.g = t.g)", "SELECT id, (SELECT a FROM nosuch WHERE nosuch.g = t.g) AS x FROM t",
                            "SELECT (SELECT a FROM nosuch)", "SELECT g FROM t GROUP BY g HAVING SUM(v) > (SELECT a FROM nosuch)",
                            "UPDATE t SET v = 0 WHERE v IN (SELECT a FROM nosuch)", "DELETE FROM t WHERE v = (SELECT a FROM nosuch)"}) {
        INFO(sql);
        REQUIRE(error_of(ex, sql) == missing);
    }
    REQUIRE(q(ex, "SELECT COUNT(*) FROM t") == Rows{{"3"}});
    // a subquery that is not run (no row of the outer statement asks for it) is not an error either
    ok(ex, "CREATE TABLE empty_t (id INT PRIMARY KEY, v INT)");
    REQUIRE(q(ex, "SELECT id, (SELECT a FROM nosuch) AS x FROM empty_t").empty());
    REQUIRE(q(ex, "SELECT id, (SELECT a FROM u) AS x FROM empty_t").empty());
    REQUIRE(q(ex, "SELECT id, (SELECT a FROM u) AS x FROM t WHERE id > 100").empty());
}

TEST_CASE("HAVING compares with a subquery", "[subquery_cardinality]") {
    TempDataDir dir("sq_card_having");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex); // the sums of v: g = 1 -> 30, g = 2 -> 30; the sum of the a of u: 50
    ok(ex, "INSERT INTO t VALUES (4, 5, 3)");
    REQUIRE(q(ex, "SELECT g FROM t GROUP BY g HAVING SUM(v) > (SELECT MIN(a) FROM u) ORDER BY g") == Rows{{"1"}, {"2"}});
    REQUIRE(q(ex, "SELECT g FROM t GROUP BY g HAVING SUM(v) = (SELECT SUM(a) FROM u WHERE id < 3) ORDER BY g") == Rows{{"1"}, {"2"}});
    REQUIRE(q(ex, "SELECT g FROM t GROUP BY g HAVING SUM(v) < (SELECT MIN(a) FROM u) ORDER BY g") == Rows{{"3"}});
    REQUIRE(q(ex, "SELECT g FROM t GROUP BY g HAVING SUM(v) IN (SELECT a + 10 FROM u) ORDER BY g") == Rows{{"1"}, {"2"}});
    REQUIRE(q(ex, "SELECT g, SUM(v) FROM t GROUP BY g HAVING COUNT(*) = (SELECT COUNT(*) FROM u WHERE g = 2) ORDER BY g") == Rows{{"2", "30"}, {"3", "5"}});
    // without GROUP BY
    REQUIRE(q(ex, "SELECT SUM(v) FROM t HAVING SUM(v) > (SELECT MAX(a) FROM u)") == Rows{{"65"}});
    REQUIRE(q(ex, "SELECT SUM(v) FROM t HAVING SUM(v) < (SELECT MAX(a) FROM u)").empty());
    // HAVING without GROUP BY is about the one row of aggregates (it was applied to the rows before they were aggregated: NULL, 0)
    REQUIRE(q(ex, "SELECT SUM(v) FROM t HAVING SUM(v) > 5") == Rows{{"65"}});
    REQUIRE(q(ex, "SELECT SUM(v) FROM t HAVING SUM(v) > 500").empty());
    REQUIRE(q(ex, "SELECT COUNT(*) FROM t HAVING COUNT(*) = 4") == Rows{{"4"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM t HAVING COUNT(*) = 3").empty());
    REQUIRE(q(ex, "SELECT SUM(v) AS s, COUNT(*) AS n FROM t HAVING s > 5 AND n = 4") == Rows{{"65", "4"}});
    REQUIRE(q(ex, "SELECT SUM(v) + 1 AS s1 FROM t HAVING SUM(v) > 5") == Rows{{"66"}});
    REQUIRE(q(ex, "SELECT MAX(v) FROM t WHERE g = 1 HAVING MIN(v) = 10") == Rows{{"20"}});
}

TEST_CASE("a failing statement leaves a transaction usable and a stored procedure callable", "[subquery_cardinality]") {
    TempDataDir dir("sq_card_txn");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    ok(ex, "BEGIN");
    ok(ex, "UPDATE t SET v = v + 1 WHERE id = 1");
    REQUIRE(error_of(ex, "UPDATE t SET v = 0 WHERE v = (SELECT a FROM u)") == ROWS);
    REQUIRE(error_of(ex, "DELETE FROM t WHERE v = (SELECT a FROM u)") == ROWS);
    ok(ex, "UPDATE t SET v = v + 1 WHERE id = 2");
    ok(ex, "COMMIT");
    REQUIRE(q(ex, "SELECT id, v FROM t ORDER BY id") == Rows{{"1", "11"}, {"2", "21"}, {"3", "30"}});
    // a stored procedure whose body fails
    ok(ex, "CREATE PROCEDURE shrink(IN lim INT) BEGIN UPDATE t SET v = v - 1 WHERE v > lim; UPDATE t SET v = 0 WHERE v = (SELECT a FROM u); END");
    REQUIRE(error_of(ex, "CALL shrink(15)") == ROWS);
    // (the first statement of the body ran, as it would in MySQL outside a transaction; the second changed nothing) and the next call works
    ok(ex, "DROP PROCEDURE shrink");
    ok(ex, "CREATE PROCEDURE grow(IN lim INT) BEGIN UPDATE t SET v = v + 1 WHERE v > lim; END");
    ok(ex, "CALL grow(15)");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM t") == Rows{{"3"}});
}

// ---- random data against a reference

namespace {
struct Inner {
    int id;
    std::optional<int> a;
    int g;
};
struct Outer {
    int id;
    std::optional<int> v;
    int g;
};
} // namespace

TEST_CASE("scalar subqueries over random tables follow the cardinality rules", "[subquery_cardinality][random]") {
    unsigned seed_count = 8; // RUSQL_FUZZ_SEEDS=80 runs a longer campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 1;
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    for (unsigned seed = seed_start; seed < seed_start + seed_count; seed++) {
        INFO("seed " << seed);
        std::mt19937 rng(seed);
        TempDataDir dir("sq_card_random");
        Executor ex(dir.path);
        open_db(ex);
        ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, v INT, g INT)");
        ok(ex, "CREATE TABLE u (id INT PRIMARY KEY, a INT, g INT)");
        std::vector<Outer> outer;
        std::vector<Inner> inner;
        std::string tv, uv;
        for (int i = 0; i < 12; i++) {
            Outer o{i, rng() % 5 == 0 ? std::nullopt : std::optional<int>(static_cast<int>(rng() % 6)), static_cast<int>(rng() % 4)};
            outer.push_back(o);
            tv += std::string(i ? ", " : "") + "(" + std::to_string(o.id) + ", " + (o.v ? std::to_string(*o.v) : N) + ", " + std::to_string(o.g) + ")";
        }
        const int inner_rows = static_cast<int>(rng() % 6);
        for (int i = 0; i < inner_rows; i++) {
            Inner n{i, rng() % 5 == 0 ? std::nullopt : std::optional<int>(static_cast<int>(rng() % 6)), static_cast<int>(rng() % 4)};
            inner.push_back(n);
            uv += std::string(i ? ", " : "") + "(" + std::to_string(n.id) + ", " + (n.a ? std::to_string(*n.a) : N) + ", " + std::to_string(n.g) + ")";
        }
        ok(ex, "INSERT INTO t VALUES " + tv);
        if (!uv.empty()) ok(ex, "INSERT INTO u VALUES " + uv);

        for (int round = 0; round < 10; round++) {
            const char* ops[] = {"=", "<>", "<", "<=", ">", ">="};
            const std::string op = ops[rng() % 6];
            auto holds = [&](int v, int a) { return op == "=" ? v == a : op == "<>" ? v != a : op == "<" ? v < a : op == "<=" ? v <= a : op == ">" ? v > a : v >= a; };
            // uncorrelated: the subquery picks the rows of u with id <= k
            {
                const int k = static_cast<int>(rng() % 6);
                std::vector<Inner> picked;
                for (auto& n : inner) {
                    if (n.id <= k) picked.push_back(n);
                }
                const std::string sql = "SELECT id FROM t WHERE v " + op + " (SELECT a FROM u WHERE id <= " + std::to_string(k) + ") ORDER BY id";
                INFO(sql);
                // (a comparison with a NULL on its left does not evaluate the subquery, as in MySQL: it needs a row whose v is not NULL)
                const bool asked = std::any_of(outer.begin(), outer.end(), [](const Outer& o) { return o.v.has_value(); });
                if (picked.size() > 1 && asked) {
                    REQUIRE(error_of(ex, sql) == ROWS);
                } else {
                    Rows want;
                    if (picked.size() == 1 && picked[0].a) {
                        for (auto& o : outer) {
                            if (o.v && holds(*o.v, *picked[0].a)) want.push_back({std::to_string(o.id)});
                        }
                    }
                    REQUIRE(q(ex, sql) == want);
                }
            }
            // correlated: the subquery picks the rows of u with the same g
            {
                bool two = false, two_where = false; // two matches for some row of t / for some row of t whose v is not NULL (the others do not ask)
                Rows want, want_where;
                for (auto& o : outer) {
                    int count = 0;
                    std::optional<int> a;
                    for (auto& n : inner) {
                        if (n.g == o.g) {
                            count++;
                            a = n.a;
                        }
                    }
                    two = two || count > 1;
                    two_where = two_where || (count > 1 && o.v);
                    want.push_back({std::to_string(o.id), a ? std::to_string(*a) : N});
                    if (count <= 1 && o.v && a && holds(*o.v, *a)) want_where.push_back({std::to_string(o.id)});
                }
                const std::string sql = "SELECT id, (SELECT a FROM u WHERE u.g = t.g) AS x FROM t ORDER BY id";
                const std::string where_sql = "SELECT id FROM t WHERE v " + op + " (SELECT a FROM u WHERE u.g = t.g) ORDER BY id";
                INFO(sql);
                INFO(where_sql);
                if (two) REQUIRE(error_of(ex, sql) == ROWS);
                else REQUIRE(q(ex, sql) == want);
                if (two_where) REQUIRE(error_of(ex, where_sql) == ROWS);
                else REQUIRE(q(ex, where_sql) == want_where);
            }
        }
    }
}
