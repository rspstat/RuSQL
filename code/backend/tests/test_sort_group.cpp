#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// ORDER BY and GROUP BY take what a select list gives: a name it gives (`ORDER BY total`), a position (`ORDER BY 2`), a column, or any expression
// (`ORDER BY a + b`, `ORDER BY COUNT(*) DESC`, `GROUP BY id % 2`). Sorting by a name the select list gives used to sort by nothing (the rows came back in
// the order of the table), a position or an expression was a parse error, and a UNION's ORDER BY ignored what no column was called.

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
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
}

std::string text(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
    return r.value();
}

using Rows = std::vector<std::vector<std::string>>;
const std::string N = "NULL";

Rows cells(const std::string& output) {
    Rows rows;
    std::istringstream in(output);
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

// the first column of every row
std::vector<std::string> first_column(Executor& ex, const std::string& sql) {
    std::vector<std::string> out;
    for (auto& r : q(ex, sql)) out.push_back(r[0]);
    return out;
}

std::string error_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_err());
    return r.error();
}

// ids 1..5; v: 10, NULL, 30, 0, 7; w: 2, 3, NULL, 0, 7; s: x, NULL, z, '12', abc; g: 1, 1, 2, 2, 3
void table_a(Executor& ex) {
    ok(ex, "CREATE TABLE a (id INT PRIMARY KEY, v INT, w INT, d DOUBLE, s VARCHAR(20), g INT)");
    ok(ex, "INSERT INTO a VALUES (1, 10, 2, 1.5, 'x', 1), (2, NULL, 3, NULL, NULL, 1), (3, 30, NULL, 2.5, 'z', 2), (4, 0, 0, 0, '12', 2), (5, 7, 7, 3.25, 'abc', 3)");
}
using V = std::vector<std::string>;
} // namespace

TEST_CASE("ORDER BY a name the select list gives", "[sort_group]") {
    TempDataDir dir("sg_alias");
    Executor ex(dir.path);
    open_db(ex);
    table_a(ex);
    // a column's alias, an expression's, a function's, a CASE's
    REQUIRE(first_column(ex, "SELECT v AS val FROM a ORDER BY val DESC") == V{"30", "10", "7", "0", N});
    REQUIRE(first_column(ex, "SELECT v AS val FROM a ORDER BY val") == V{N, "0", "7", "10", "30"});
    REQUIRE(first_column(ex, "SELECT id AS i FROM a ORDER BY i DESC") == V{"5", "4", "3", "2", "1"});
    REQUIRE(first_column(ex, "SELECT a.id AS i FROM a ORDER BY i DESC") == V{"5", "4", "3", "2", "1"});
    REQUIRE(q(ex, "SELECT id, v + w AS k FROM a ORDER BY k, id") == Rows{{"2", N}, {"3", N}, {"4", "0"}, {"1", "12"}, {"5", "14"}});
    REQUIRE(q(ex, "SELECT id, v * 2 AS k FROM a ORDER BY k DESC, id") == Rows{{"3", "60"}, {"1", "20"}, {"5", "14"}, {"4", "0"}, {"2", N}});
    REQUIRE(q(ex, "SELECT id, UPPER(s) AS up FROM a ORDER BY up, id") == Rows{{"2", N}, {"4", "12"}, {"5", "ABC"}, {"1", "X"}, {"3", "Z"}});
    REQUIRE(q(ex, "SELECT id, CASE WHEN v > 5 THEN 'big' ELSE 'small' END AS size FROM a ORDER BY size DESC, id") ==
            Rows{{"2", "small"}, {"4", "small"}, {"1", "big"}, {"3", "big"}, {"5", "big"}});
    // the name of a constant is the constant, not a position
    REQUIRE(first_column(ex, "SELECT id, 1 AS k FROM a ORDER BY k DESC") == V{"1", "2", "3", "4", "5"});
    REQUIRE(first_column(ex, "SELECT id, 2 AS k FROM a ORDER BY k, id DESC") == V{"5", "4", "3", "2", "1"});
    // a statement that is bound again (here the derived table) keeps the names it was resolved to
    REQUIRE(first_column(ex, "SELECT w FROM (SELECT id AS v, v AS w FROM a ORDER BY w LIMIT 10) t") == V{N, "0", "7", "10", "30"});
    // a name the select list gives wins over a column of the same name
    REQUIRE(q(ex, "SELECT id AS v, v AS id FROM a ORDER BY id, v") == Rows{{"2", N}, {"4", "0"}, {"5", "7"}, {"1", "10"}, {"3", "30"}});
    REQUIRE(q(ex, "SELECT id AS v FROM a ORDER BY v DESC") == Rows{{"5"}, {"4"}, {"3"}, {"2"}, {"1"}});
    // with LIMIT and OFFSET, DISTINCT, a WHERE
    REQUIRE(q(ex, "SELECT id, v + 1 AS k FROM a ORDER BY k DESC LIMIT 2") == Rows{{"3", "31"}, {"1", "11"}});
    REQUIRE(q(ex, "SELECT id AS i, v FROM a WHERE id > 1 ORDER BY i LIMIT 2 OFFSET 1") == Rows{{"3", "30"}, {"4", "0"}});
    REQUIRE(first_column(ex, "SELECT DISTINCT UPPER(s) AS u FROM a ORDER BY u") == V{N, "12", "ABC", "X", "Z"});
    // a window function's name
    REQUIRE(q(ex, "SELECT id, ROW_NUMBER() OVER (ORDER BY v) AS rn FROM a ORDER BY rn DESC") == Rows{{"3", "5"}, {"1", "4"}, {"5", "3"}, {"4", "2"}, {"2", "1"}});
    // aggregates and expressions of aggregates
    REQUIRE(q(ex, "SELECT g, SUM(v) AS t FROM a GROUP BY g ORDER BY t") == Rows{{"3", "7"}, {"1", "10"}, {"2", "30"}});
    REQUIRE(q(ex, "SELECT g, COALESCE(SUM(v), 0) + 1 AS t FROM a GROUP BY g ORDER BY t DESC") == Rows{{"2", "31"}, {"1", "11"}, {"3", "8"}});
    REQUIRE(q(ex, "SELECT g AS grp, COUNT(*) AS c FROM a GROUP BY g ORDER BY grp DESC") == Rows{{"3", "1"}, {"2", "2"}, {"1", "2"}});
    REQUIRE(q(ex, "SELECT g AS grp, COUNT(*) AS c FROM a GROUP BY g ORDER BY c DESC, grp") == Rows{{"1", "2"}, {"2", "2"}, {"3", "1"}});
    REQUIRE(q(ex, "SELECT g, SUM(v) AS t FROM a GROUP BY g HAVING t > 8 ORDER BY t DESC") == Rows{{"2", "30"}, {"1", "10"}});
}

TEST_CASE("ORDER BY and GROUP BY by position", "[sort_group]") {
    TempDataDir dir("sg_position");
    Executor ex(dir.path);
    open_db(ex);
    table_a(ex);
    REQUIRE(q(ex, "SELECT id, v FROM a ORDER BY 2, 1") == Rows{{"2", N}, {"4", "0"}, {"5", "7"}, {"1", "10"}, {"3", "30"}});
    REQUIRE(q(ex, "SELECT id, v FROM a ORDER BY 2 DESC, 1 DESC") == Rows{{"3", "30"}, {"1", "10"}, {"5", "7"}, {"4", "0"}, {"2", N}});
    REQUIRE(first_column(ex, "SELECT * FROM a ORDER BY 1 DESC LIMIT 2") == V{"5", "4"});
    REQUIRE(first_column(ex, "SELECT id, v + w FROM a ORDER BY 2, 1") == V{"2", "3", "4", "1", "5"});
    REQUIRE(q(ex, "SELECT g AS grp, COUNT(*) AS c FROM a GROUP BY 1 ORDER BY 2 DESC, 1") == Rows{{"1", "2"}, {"2", "2"}, {"3", "1"}});
    REQUIRE(q(ex, "SELECT g, v FROM a GROUP BY g, v ORDER BY 1, 2") == Rows{{"1", N}, {"1", "10"}, {"2", "0"}, {"2", "30"}, {"3", "7"}});
    // a position that is not in the select list is an error, and so is one of a column that cannot be sorted by
    REQUIRE(error_of(ex, "SELECT * FROM a ORDER BY 9").find("Unknown column '9' in 'order clause'") != std::string::npos);
    REQUIRE(error_of(ex, "SELECT id FROM a ORDER BY 0").find("Unknown column '0'") != std::string::npos);
    REQUIRE(error_of(ex, "SELECT id FROM a GROUP BY 3").find("Unknown column '3' in 'group statement'") != std::string::npos);
}

TEST_CASE("ORDER BY and GROUP BY expressions", "[sort_group]") {
    TempDataDir dir("sg_expression");
    Executor ex(dir.path);
    open_db(ex);
    table_a(ex);
    REQUIRE(first_column(ex, "SELECT id FROM a ORDER BY v + w, id") == V{"2", "3", "4", "1", "5"});
    REQUIRE(first_column(ex, "SELECT id FROM a ORDER BY ABS(v - 20), id") == V{"2", "1", "3", "5", "4"});
    REQUIRE(first_column(ex, "SELECT id FROM a ORDER BY CASE WHEN v IS NULL THEN 1 ELSE 0 END, id") == V{"1", "3", "4", "5", "2"});
    REQUIRE(first_column(ex, "SELECT id FROM a ORDER BY v IS NULL, v, id") == V{"4", "5", "1", "3", "2"});
    REQUIRE(first_column(ex, "SELECT id FROM a ORDER BY LENGTH(s), s, id") == V{"2", "1", "3", "4", "5"});
    REQUIRE(first_column(ex, "SELECT id FROM a WHERE v > 5 ORDER BY ABS(v) DESC, id") == V{"3", "1", "5"});
    REQUIRE(first_column(ex, "SELECT id FROM a ORDER BY -v, id") == V{"2", "3", "1", "5", "4"});
    REQUIRE(first_column(ex, "SELECT id FROM a ORDER BY COALESCE(v, 100) DESC, id") == V{"2", "3", "1", "5", "4"});
    REQUIRE(first_column(ex, "SELECT id FROM a ORDER BY id % 2, id DESC") == V{"4", "2", "5", "3", "1"});
    // grouped: an aggregate the select list does not have, expressions of aggregates, grouped expressions
    REQUIRE(q(ex, "SELECT g, COUNT(*) FROM a GROUP BY g ORDER BY COUNT(*) DESC, g DESC") == Rows{{"2", "2"}, {"1", "2"}, {"3", "1"}});
    REQUIRE(first_column(ex, "SELECT g FROM a GROUP BY g ORDER BY SUM(v) DESC") == V{"2", "1", "3"});
    REQUIRE(first_column(ex, "SELECT g FROM a GROUP BY g ORDER BY MAX(v) - MIN(v) DESC, g") == V{"2", "1", "3"});
    REQUIRE(first_column(ex, "SELECT g FROM a GROUP BY g ORDER BY SUM(v * 2) / COUNT(*), g") == V{"1", "3", "2"});
    REQUIRE(q(ex, "SELECT id % 2 AS p, COUNT(*) FROM a GROUP BY id % 2 ORDER BY p") == Rows{{"0", "2"}, {"1", "3"}});
    REQUIRE(q(ex, "SELECT id % 2 AS p, COUNT(*) AS n FROM a GROUP BY p ORDER BY n DESC") == Rows{{"1", "3"}, {"0", "2"}});
    REQUIRE(q(ex, "SELECT g + 1, COUNT(*) FROM a GROUP BY g + 1 ORDER BY 1 DESC") == Rows{{"4", "1"}, {"3", "2"}, {"2", "2"}});
    REQUIRE(q(ex, "SELECT CASE WHEN v > 5 THEN 'big' ELSE 'small' END AS size, COUNT(*) FROM a GROUP BY size ORDER BY size") ==
            Rows{{"big", "3"}, {"small", "2"}});
    REQUIRE(q(ex, "SELECT g, SUM(v) FROM a GROUP BY g ORDER BY g + 1 DESC") == Rows{{"3", "7"}, {"2", "30"}, {"1", "10"}});
    // an ORDER BY expression of a grouped query that contains a grouped expression reads the group's value of it
    REQUIRE(q(ex, "SELECT g + 1 AS k, COUNT(*) FROM a GROUP BY g + 1 ORDER BY (g + 1) * 2 DESC") == Rows{{"4", "1"}, {"3", "2"}, {"2", "2"}});
    REQUIRE(q(ex, "SELECT id % 2, SUM(v) + 1 FROM a GROUP BY id % 2 ORDER BY id % 2 DESC") == Rows{{"1", "48"}, {"0", "1"}});
    // the same grouped expression written in the select list, GROUP BY and ORDER BY
    REQUIRE(q(ex, "SELECT g * 10 + 1 AS code, MAX(v) FROM a GROUP BY g * 10 + 1 ORDER BY g * 10 + 1 DESC") == Rows{{"31", "7"}, {"21", "30"}, {"11", "10"}});
    // an ORDER BY that is a constant sorts nothing; a name that is not a column is an error
    REQUIRE(first_column(ex, "SELECT id FROM a ORDER BY 1 + 1") == V{"1", "2", "3", "4", "5"});
    REQUIRE(error_of(ex, "SELECT id FROM a ORDER BY nosuch + 1").find("Unknown column 'nosuch'") != std::string::npos);
}

TEST_CASE("ORDER BY and GROUP BY over joins, views, derived tables, CTEs and set operations", "[sort_group]") {
    TempDataDir dir("sg_sources");
    Executor ex(dir.path);
    open_db(ex);
    table_a(ex);
    ok(ex, "CREATE TABLE b (id INT PRIMARY KEY, k INT, a_id INT)");
    ok(ex, "INSERT INTO b VALUES (1, 3, 1), (2, 8, 1), (3, 1, 3), (4, 5, 9)");
    REQUIRE(q(ex, "SELECT a.id, b.k FROM a JOIN b ON a.id = b.a_id ORDER BY b.k DESC") == Rows{{"1", "8"}, {"1", "3"}, {"3", "1"}});
    REQUIRE(q(ex, "SELECT a.id, b.k * 2 AS kk FROM a JOIN b ON a.id = b.a_id ORDER BY kk") == Rows{{"3", "2"}, {"1", "6"}, {"1", "16"}});
    REQUIRE(q(ex, "SELECT x.id, y.k AS kk FROM a x JOIN b y ON x.id = y.a_id ORDER BY kk DESC") == Rows{{"1", "8"}, {"1", "3"}, {"3", "1"}});
    REQUIRE(q(ex, "SELECT x.g, SUM(y.k) AS t FROM a x JOIN b y ON x.id = y.a_id GROUP BY x.g ORDER BY t DESC") == Rows{{"1", "11"}, {"2", "1"}});
    REQUIRE(q(ex, "SELECT x.id, y.k FROM a x JOIN b y ON x.id = y.a_id ORDER BY y.k * -1, x.id") == Rows{{"1", "8"}, {"1", "3"}, {"3", "1"}});
    // a view with an ORDER BY of a name it gives, selected from and sorted again
    ok(ex, "CREATE VIEW va AS SELECT id, v + w AS vw FROM a ORDER BY vw DESC");
    REQUIRE(first_column(ex, "SELECT * FROM va") == V{"5", "1", "4", "2", "3"});
    REQUIRE(first_column(ex, "SELECT id FROM va ORDER BY vw, id") == V{"2", "3", "4", "1", "5"});
    REQUIRE(q(ex, "SELECT t.n FROM (SELECT id, v * 2 AS n FROM a) t ORDER BY n DESC") == Rows{{"60"}, {"20"}, {"14"}, {"0"}, {N}});
    REQUIRE(q(ex, "WITH c AS (SELECT id, v * 2 AS dv FROM a) SELECT id FROM c ORDER BY dv DESC, id") == Rows{{"3"}, {"1"}, {"5"}, {"4"}, {"2"}});
    REQUIRE(first_column(ex, "SELECT d.id FROM (SELECT id, v FROM a ORDER BY v DESC LIMIT 3) d ORDER BY d.id") == V{"1", "3", "5"});
    // UNION / INTERSECT / EXCEPT sort the columns of the answer by name or position, and refuse what no column is called
    REQUIRE(first_column(ex, "SELECT g FROM a UNION SELECT k FROM b ORDER BY 1 DESC") == V{"8", "5", "3", "2", "1"});
    REQUIRE(first_column(ex, "SELECT g AS x FROM a UNION SELECT k FROM b ORDER BY x DESC") == V{"8", "5", "3", "2", "1"});
    REQUIRE(q(ex, "SELECT g, id FROM a UNION SELECT k, a_id FROM b ORDER BY 2 DESC, 1 LIMIT 3") == Rows{{"5", "9"}, {"3", "5"}, {"2", "4"}});
    REQUIRE(first_column(ex, "SELECT g FROM a INTERSECT SELECT k FROM b ORDER BY 1 DESC") == V{"3", "1"});
    REQUIRE(first_column(ex, "SELECT g FROM a EXCEPT SELECT k FROM b ORDER BY 1 DESC") == V{"2"});
    REQUIRE(error_of(ex, "SELECT g FROM a UNION SELECT k FROM b ORDER BY nosuch").find("Unknown column 'nosuch' in 'order clause'") != std::string::npos);
    REQUIRE(error_of(ex, "SELECT g FROM a UNION SELECT k FROM b ORDER BY 3").find("Unknown column '3'") != std::string::npos);
}

TEST_CASE("an expression in ORDER BY, GROUP BY and an aggregate compares by the types of its columns", "[sort_group]") {
    TempDataDir dir("sg_types");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE ct (id INT PRIMARY KEY, code VARCHAR(10), n INT)");
    ok(ex, "INSERT INTO ct VALUES (1, '7', 1), (2, '007', 2), (3, '7.0', 3), (4, 'abc', 4)");
    // a text column against a text compares as text: only '007' is '007'
    REQUIRE(first_column(ex, "SELECT id FROM ct ORDER BY CASE WHEN code = '007' THEN 0 ELSE 1 END, id") == V{"2", "1", "3", "4"});
    REQUIRE(q(ex, "SELECT SUM(CASE WHEN code = '007' THEN 1 ELSE 0 END), COUNT(CASE WHEN code = '007' THEN 1 END) FROM ct") == Rows{{"1", "1"}});
    REQUIRE(q(ex, "SELECT CASE WHEN code = '007' THEN 'y' ELSE 'n' END AS hit, COUNT(*) FROM ct GROUP BY hit ORDER BY hit") == Rows{{"n", "3"}, {"y", "1"}});
    REQUIRE(q(ex, "SELECT MAX(CASE WHEN code < '8' THEN code END), COUNT(DISTINCT CASE WHEN code < '8' THEN code END) FROM ct") == Rows{{"7.0", "3"}});
    REQUIRE(first_column(ex, "SELECT id FROM ct WHERE NULLIF(code, 'zzz') = '007' ORDER BY id") == V{"2"});
    // a column in the argument of a function holds what its type says: COALESCE of a text column and a string is text, so '007' is not '7'
    REQUIRE(first_column(ex, "SELECT id FROM ct WHERE COALESCE(code, 'x') = '007' ORDER BY id") == V{"2"});
    REQUIRE(first_column(ex, "SELECT id FROM ct WHERE IFNULL(code, 'x') = '7' ORDER BY id") == V{"1"});
    REQUIRE(first_column(ex, "SELECT id FROM ct WHERE UPPER(code) = '007' ORDER BY id") == V{"2"});
    // numbers group by value: COALESCE(w, 0) over a DECIMAL column puts 0.00 and the 0 of a NULL in one group
    ok(ex, "CREATE TABLE m (id INT PRIMARY KEY, w DECIMAL(10,2), n INT)");
    ok(ex, "INSERT INTO m VALUES (1, 0.00, 1), (2, NULL, 2), (3, 1.50, 3), (4, NULL, 4), (5, 1.5, 5)");
    REQUIRE(q(ex, "SELECT COALESCE(w, 0) AS k, COUNT(*) AS c FROM m GROUP BY k ORDER BY c DESC") == Rows{{"0.00", "3"}, {"1.50", "2"}});
    REQUIRE(first_column(ex, "SELECT COUNT(*) FROM m GROUP BY COALESCE(w, 0) ORDER BY 1") == V{"2", "3"});
    REQUIRE(first_column(ex, "SELECT DISTINCT COALESCE(w, 0) FROM m ORDER BY 1") == V{"0.00", "1.50"});
    REQUIRE(first_column(ex, "SELECT DISTINCT w + 0 FROM m WHERE w IS NOT NULL ORDER BY 1") == V{"0", "1.5"});
    REQUIRE(first_column(ex, "SELECT COUNT(*) FROM m GROUP BY IFNULL(w, 0), n > 10 ORDER BY 1") == V{"2", "3"});
    // a text column is sorted as text, whatever its values look like
    REQUIRE(first_column(ex, "SELECT code AS c FROM ct ORDER BY c") == V{"007", "7", "7.0", "abc"});
    REQUIRE(first_column(ex, "SELECT code AS c FROM ct ORDER BY 1 DESC") == V{"abc", "7.0", "7", "007"});
}

// ---- random sorts and groups against a reference ----------------------------------------------------------------------------------------

namespace {
using Cell = std::optional<long long>;
using Env = std::array<Cell, 3>; // x, y, z

struct Expr {
    std::string sql;
    std::function<Cell(const Env&)> eval;
};

Expr make_expr(std::mt19937& rng, int depth) {
    const int kind = depth <= 0 ? static_cast<int>(rng() % 3) : static_cast<int>(rng() % 8);
    if (kind == 0) {
        const std::size_t c = rng() % 3;
        const char* names[] = {"x", "y", "z"};
        return {names[c], [c](const Env& e) { return e[c]; }};
    }
    if (kind == 1) {
        const long long k = static_cast<long long>(rng() % 9) - 2;
        return {std::to_string(k), [k](const Env&) { return Cell(k); }};
    }
    if (kind == 2) {
        const std::size_t c = rng() % 3;
        const char* names[] = {"x", "y", "z"};
        return {std::string("COALESCE(") + names[c] + ", 5)", [c](const Env& e) { return e[c] ? e[c] : Cell(5); }};
    }
    if (kind == 3) {
        Expr inner = make_expr(rng, depth - 1);
        return {"ABS(" + inner.sql + ")", [inner](const Env& e) -> Cell {
                    Cell v = inner.eval(e);
                    if (!v) return v;
                    return *v < 0 ? -*v : *v;
                }};
    }
    if (kind == 4) {
        Expr a = make_expr(rng, depth - 1), b = make_expr(rng, depth - 1), r = make_expr(rng, depth - 1);
        const long long k = static_cast<long long>(rng() % 6);
        return {"CASE WHEN " + a.sql + " > " + std::to_string(k) + " THEN " + b.sql + " ELSE " + r.sql + " END", [a, b, r, k](const Env& e) -> Cell {
                    Cell v = a.eval(e);
                    return v && *v > k ? b.eval(e) : r.eval(e);
                }};
    }
    Expr l = make_expr(rng, depth - 1), r = make_expr(rng, depth - 1);
    const char op = "+-*"[kind % 3];
    return {"(" + l.sql + " " + op + " " + r.sql + ")", [l, r, op](const Env& e) -> Cell {
                Cell a = l.eval(e), b = r.eval(e);
                if (!a || !b) return std::nullopt;
                return op == '+' ? *a + *b : (op == '-' ? *a - *b : *a * *b);
            }};
}

// NULL is smaller than every value
int compare_cells(const Cell& a, const Cell& b) {
    if (!a || !b) return a.has_value() == b.has_value() ? 0 : (a ? 1 : -1);
    return *a < *b ? -1 : (*a > *b ? 1 : 0);
}
} // namespace

TEST_CASE("random ORDER BY and GROUP BY items match a reference", "[sort_group][random]") {
    unsigned seed_count = 6; // RUSQL_FUZZ_SEEDS=60 runs a longer campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 1;
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    for (unsigned seed = seed_start; seed < seed_start + seed_count; seed++) {
        INFO("seed " << seed);
        std::mt19937 rng(seed);
        TempDataDir dir("sg_random");
        Executor ex(dir.path);
        open_db(ex);
        ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, g INT, x INT, y INT, z INT)");
        struct Line { int g; Env env; };
        std::vector<Line> table;
        std::string values;
        for (int i = 0; i < 40; i++) {
            Line r;
            r.g = static_cast<int>(rng() % 4);
            std::string line = "(" + std::to_string(i) + ", " + std::to_string(r.g);
            for (std::size_t c = 0; c < 3; c++) {
                if (rng() % 5 == 0) {
                    r.env[c] = std::nullopt;
                    line += ", NULL";
                } else {
                    r.env[c] = static_cast<long long>(rng() % 9) - 3;
                    line += ", " + std::to_string(*r.env[c]);
                }
            }
            table.push_back(r);
            values += (i ? ", " : "") + line + ")";
        }
        ok(ex, "INSERT INTO t VALUES " + values);
        for (int round = 0; round < 12; round++) {
            // ---- rows sorted by a mix of columns, names the select list gives, positions and expressions
            {
                const Expr e1 = make_expr(rng, 2), e2 = make_expr(rng, 2);
                // select list positions: 1 id, 2 x, 3 y, 4 z, 5 e1, 6 e2
                struct Key { std::string sql; std::function<Cell(const Env&)> value; bool ascending; };
                std::vector<Key> keys;
                const int count = 1 + static_cast<int>(rng() % 3);
                std::string order;
                for (int k = 0; k < count; k++) {
                    Key key;
                    key.ascending = rng() % 2 == 0;
                    switch (rng() % 6) {
                        case 0: { const std::size_t c = rng() % 3; key.sql = std::string("xyz").substr(c, 1); key.value = [c](const Env& e) { return e[c]; }; break; }
                        case 1: { const std::size_t c = rng() % 3; key.sql = std::string("t.") + "xyz"[c]; key.value = [c](const Env& e) { return e[c]; }; break; }
                        case 2: key.sql = "e1"; key.value = e1.eval; break;
                        case 3: key.sql = "e2"; key.value = e2.eval; break;
                        case 4: {
                            const int pos = 2 + static_cast<int>(rng() % 5);
                            key.sql = std::to_string(pos);
                            key.value = pos == 2 || pos == 3 || pos == 4 ? std::function<Cell(const Env&)>([pos](const Env& e) { return e[static_cast<std::size_t>(pos - 2)]; })
                                                                          : (pos == 5 ? e1.eval : e2.eval);
                            break;
                        }
                        default: {
                            Expr own = make_expr(rng, 2);
                            // (a number alone is a position)
                            while (own.sql.find_first_not_of("0123456789()") == std::string::npos) own = make_expr(rng, 2);
                            key.sql = own.sql;
                            key.value = own.eval;
                            break;
                        }
                    }
                    order += (k ? ", " : "") + key.sql + (key.ascending ? "" : " DESC");
                    keys.push_back(std::move(key));
                }
                std::vector<std::size_t> ids(table.size());
                for (std::size_t i = 0; i < ids.size(); i++) ids[i] = i;
                std::stable_sort(ids.begin(), ids.end(), [&](std::size_t a, std::size_t b) {
                    for (auto& key : keys) {
                        int c = compare_cells(key.value(table[a].env), key.value(table[b].env));
                        if (!key.ascending) c = -c;
                        if (c != 0) return c < 0;
                    }
                    return false;
                });
                Rows want;
                for (std::size_t i : ids) want.push_back({std::to_string(i)});
                const std::string sql = "SELECT id, x, y, z, " + e1.sql + " AS e1, " + e2.sql + " AS e2 FROM t ORDER BY " + order + ", id";
                // (the ties are in the order of the id: the reference sort is stable over rows that are in the order of their ids)
                REQUIRE(first_column(ex, sql) == [&] { std::vector<std::string> out; for (auto& r : want) out.push_back(r[0]); return out; }());
            }
            // ---- groups by an expression, sorted by what the select list gives, a position, an aggregate or an expression
            {
                Expr ge = make_expr(rng, 2);
                while (ge.sql.find_first_not_of("0123456789()") == std::string::npos) ge = make_expr(rng, 2); // (a number alone is a position)
                std::map<std::string, std::tuple<Cell, long long, long long, long long>> groups; // key text -> (key, rows, sum of x, rows with an x)
                for (auto& r : table) {
                    Cell k = ge.eval(r.env);
                    auto& g = groups[k ? std::to_string(*k) : N];
                    std::get<0>(g) = k;
                    std::get<1>(g)++;
                    if (r.env[0]) { std::get<2>(g) += *r.env[0]; std::get<3>(g)++; }
                }
                struct Out { Cell key; long long rows; Cell sum; };
                std::vector<Out> out;
                for (auto& [text, g] : groups) out.push_back({std::get<0>(g), std::get<1>(g), std::get<3>(g) ? Cell(std::get<2>(g)) : Cell()});
                const bool by_count = rng() % 2 == 0;
                const bool descending = rng() % 2 == 0;
                std::stable_sort(out.begin(), out.end(), [&](const Out& a, const Out& b) {
                    int c = by_count ? compare_cells(a.rows, b.rows) : compare_cells(a.sum, b.sum);
                    if (c == 0) c = compare_cells(a.key, b.key);
                    return descending ? c > 0 : c < 0;
                });
                // ties on the first key are broken by the group expression (ascending in the reference above is compensated: a descending sort reverses both)
                const std::string item = by_count ? (rng() % 2 ? "COUNT(*)" : "n") : (rng() % 2 ? "SUM(x)" : "s");
                const std::string sql = "SELECT " + ge.sql + " AS ge, COUNT(*) AS n, SUM(x) AS s FROM t GROUP BY " + (rng() % 2 ? ge.sql : std::string("ge")) +
                                         " ORDER BY " + item + (descending ? " DESC" : "") + ", ge" + (descending ? " DESC" : "");
                INFO(sql);
                Rows want;
                for (auto& o : out) want.push_back({o.key ? std::to_string(*o.key) : N, std::to_string(o.rows), o.sum ? std::to_string(*o.sum) : N});
                REQUIRE(q(ex, sql) == want);
            }
        }
    }
}
