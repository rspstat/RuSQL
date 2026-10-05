#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// NULL in an expression: `v + 1`, `v * 2`, `v / w`, `v > 5` and the scalar functions give NULL when an operand is NULL, and x / 0 is
// NULL. They used to give "NULL1" (the two texts joined), 0 (for - * / and most numeric functions), 4 (LENGTH of the text "NULL"),
// and an UPDATE stored that into the table. Every answer is compared with something computed in the test.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

using Rows = std::vector<std::vector<std::string>>;

void open_db(Executor& ex) {
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
}

std::string ok_text(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql);
    REQUIRE(r.is_ok());
    return r.value();
}

// The cells of a result table, one vector per row (the header line is skipped).
Rows table_cells(const std::string& text) {
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

// Runs `SELECT <expr> AS v FROM t` (t has exactly one row) and returns the "v" cell.
std::string eval_expr(Executor& ex, const std::string& expr) {
    Rows rows = table_cells(ok_text(ex, "SELECT " + expr + " AS v FROM t"));
    REQUIRE(rows.size() == 1);
    REQUIRE(rows[0].size() == 1);
    return rows[0][0];
}
} // namespace

TEST_CASE("arithmetic with a NULL operand is NULL, in the select list, WHERE, CASE and UPDATE", "[null_expr]") {
    TempDataDir dir("null_expr_arith");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE a (id INT PRIMARY KEY, v INT, w INT, d DOUBLE, s VARCHAR(8))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO a VALUES (1, 10, 2, 1.5, 'x'), (2, NULL, 3, NULL, NULL), (3, 30, NULL, 2.5, 'z'), "
                           "(4, 0, 0, 0, '12'), (5, NULL, NULL, NULL, NULL)")
                .is_ok());
    const std::string N = "NULL";

    REQUIRE(table_cells(ok_text(ex, "SELECT id, v + 1, v - 1, v * 2, v / 2 FROM a ORDER BY id")) ==
            Rows{{"1", "11", "9", "20", "5"}, {"2", N, N, N, N}, {"3", "31", "29", "60", "15"}, {"4", "1", "-1", "0", "0"}, {"5", N, N, N, N}});
    // two columns: NULL when either is, and 0 / 0 is NULL
    REQUIRE(table_cells(ok_text(ex, "SELECT id, v + w, v - w, v * w, v / w FROM a ORDER BY id")) ==
            Rows{{"1", "12", "8", "20", "5"}, {"2", N, N, N, N}, {"3", N, N, N, N}, {"4", "0", "0", "0", N}, {"5", N, N, N, N}});
    // a literal on the left, a DOUBLE column, division by a zero value
    REQUIRE(table_cells(ok_text(ex, "SELECT id, 1 + v, 100 - v, 3 * d, 9 / d FROM a ORDER BY id")) ==
            Rows{{"1", "11", "90", "4.5", "6"}, {"2", N, N, N, N}, {"3", "31", "70", "7.5", "3.6"}, {"4", "1", "100", "0", N}, {"5", N, N, N, N}});
    // the NULL literal
    REQUIRE(table_cells(ok_text(ex, "SELECT NULL + 1, 1 * NULL, NULL / 2, 5 - NULL FROM a WHERE id = 1")) == Rows{{N, N, N, N}});
    // x / 0
    REQUIRE(table_cells(ok_text(ex, "SELECT id, 1 / 0, v / 0 FROM a WHERE id IN (1, 2) ORDER BY id")) == Rows{{"1", N, N}, {"2", N, N}});

    // a comparison read as a value is NULL when an operand is
    REQUIRE(table_cells(ok_text(ex, "SELECT id, v > 5, v = 10, v <> 10 FROM a ORDER BY id")) ==
            Rows{{"1", "1", "1", "0"}, {"2", N, N, N}, {"3", "1", "0", "1"}, {"4", "0", "0", "1"}, {"5", N, N, N}});
    REQUIRE(table_cells(ok_text(ex, "SELECT id, v = NULL FROM a WHERE id <= 2 ORDER BY id")) == Rows{{"1", N}, {"2", N}});

    // a row with a NULL operand matches no comparison
    REQUIRE(table_cells(ok_text(ex, "SELECT id FROM a WHERE v + 1 > 5 ORDER BY id")) == Rows{{"1"}, {"3"}});
    REQUIRE(table_cells(ok_text(ex, "SELECT id FROM a WHERE v * 2 = 20 ORDER BY id")) == Rows{{"1"}});
    REQUIRE(table_cells(ok_text(ex, "SELECT id FROM a WHERE v - w < 100 ORDER BY id")) == Rows{{"1"}, {"4"}});
    REQUIRE(table_cells(ok_text(ex, "SELECT id FROM a WHERE v / w > 0 ORDER BY id")) == Rows{{"1"}});
    REQUIRE(table_cells(ok_text(ex, "SELECT id, CASE WHEN v + 1 > 5 THEN 'big' ELSE 'small' END AS c FROM a ORDER BY id")) ==
            Rows{{"1", "big"}, {"2", "small"}, {"3", "big"}, {"4", "small"}, {"5", "small"}});

    // aggregates: an expression over a NULL MAX / MIN is NULL
    REQUIRE(table_cells(ok_text(ex, "SELECT MAX(v) - MIN(v), SUM(v) + MAX(v), MAX(w) * 2 FROM a")) == Rows{{"30", "70", "6"}});
    REQUIRE(table_cells(ok_text(ex, "SELECT id, MAX(v) - MIN(v), SUM(v) + MAX(v), MAX(v) * 2, MAX(v) / 2 FROM a WHERE id IN (2, 5) GROUP BY id ORDER BY id")) ==
            Rows{{"2", N, N, N, N}, {"5", N, N, N, N}});

    // `%` is MOD: a NULL row is not "divisible" (WHERE v % 5 = 0 used to keep it, and so the rows a RIGHT JOIN pads for a missing match)
    REQUIRE(table_cells(ok_text(ex, "SELECT id, v % 3 FROM a ORDER BY id")) == Rows{{"1", "1"}, {"2", N}, {"3", "0"}, {"4", "0"}, {"5", N}});
    REQUIRE(table_cells(ok_text(ex, "SELECT id FROM a WHERE v % 5 = 0 ORDER BY id")) == Rows{{"1"}, {"3"}, {"4"}});
    REQUIRE(ex.execute_sql("CREATE TABLE r (id INT PRIMARY KEY)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO r VALUES (1), (9)").is_ok());
    REQUIRE(table_cells(ok_text(ex, "SELECT a.id, r.id FROM a RIGHT JOIN r ON r.id = a.id WHERE a.id % 5 = 0 OR a.id = 1 ORDER BY r.id")) ==
            Rows{{"1", "1"}});

    // text operands that are not NULL keep the engine's old rules (+ joins them, - * / read them as 0)
    REQUIRE(table_cells(ok_text(ex, "SELECT id, s + 1, s * 2 FROM a WHERE id IN (1, 3, 4) ORDER BY id")) ==
            Rows{{"1", "x1", "0"}, {"3", "z1", "0"}, {"4", "13", "24"}});

    // UPDATE: a NULL stays NULL (it became "NULL1" / 0), x / 0 is NULL (it became 0)
    REQUIRE(ex.execute_sql("UPDATE a SET v = v + 1").is_ok());
    REQUIRE(table_cells(ok_text(ex, "SELECT id, v FROM a ORDER BY id")) == Rows{{"1", "11"}, {"2", N}, {"3", "31"}, {"4", "1"}, {"5", N}});
    REQUIRE(table_cells(ok_text(ex, "SELECT COUNT(*) FROM a WHERE v IS NULL")) == Rows{{"2"}});
    REQUIRE(ex.execute_sql("UPDATE a SET w = w * 2").is_ok());
    REQUIRE(table_cells(ok_text(ex, "SELECT id, w FROM a ORDER BY id")) == Rows{{"1", "4"}, {"2", "6"}, {"3", N}, {"4", "0"}, {"5", N}});
    REQUIRE(ex.execute_sql("UPDATE a SET d = d / w").is_ok());
    REQUIRE(table_cells(ok_text(ex, "SELECT id, d FROM a ORDER BY id")) == Rows{{"1", "0.375"}, {"2", N}, {"3", N}, {"4", N}, {"5", N}});
    // an UPDATE that sets NULL where the column is read again: arithmetic on it stays NULL
    REQUIRE(table_cells(ok_text(ex, "SELECT id, d + 1 FROM a ORDER BY id")) == Rows{{"1", "1.375"}, {"2", N}, {"3", N}, {"4", N}, {"5", N}});
}

TEST_CASE("scalar functions return NULL for a NULL argument and keep their value otherwise", "[null_expr][scalar_func]") {
    TempDataDir dir("null_expr_functions");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, n INT, s VARCHAR(20), dt VARCHAR(20))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, NULL, NULL, NULL)").is_ok());

    struct Call {
        std::string name;
        std::vector<std::string> args;
        std::size_t value_args; // how many leading arguments a NULL turns into a NULL result
        std::string expected;   // the call with these arguments as given
    };
    const std::vector<Call> calls = {
        {"UPPER", {"'abc'"}, 1, "ABC"}, {"LOWER", {"'ABC'"}, 1, "abc"}, {"LENGTH", {"'abcd'"}, 1, "4"},
        {"CHAR_LENGTH", {"'abcd'"}, 1, "4"}, {"CHARACTER_LENGTH", {"'abcd'"}, 1, "4"}, {"BIT_LENGTH", {"'ab'"}, 1, "16"},
        {"TRIM", {"'  a '"}, 1, "a"}, {"LTRIM", {"'  a'"}, 1, "a"}, {"RTRIM", {"'a  '"}, 1, "a"}, {"REVERSE", {"'abc'"}, 1, "cba"},
        {"SPACE", {"3"}, 1, ""}, {"ASCII", {"'A'"}, 1, "65"}, {"HEX", {"255"}, 1, "FF"}, {"UNHEX", {"'41'"}, 1, "A"},
        {"MD5", {"'a'"}, 1, "0cc175b9c0f1b6a831c399e269772661"}, {"ABS", {"-5"}, 1, "5"}, {"CEIL", {"4.1"}, 1, "5"},
        {"FLOOR", {"4.9"}, 1, "4"}, {"SQRT", {"16"}, 1, "4.000000"}, {"EXP", {"0"}, 1, "1.000000"}, {"SIN", {"0"}, 1, "0.000000"},
        {"COS", {"0"}, 1, "1.000000"}, {"TAN", {"0"}, 1, "0.000000"}, {"SIGN", {"-3"}, 1, "-1"}, {"LOG2", {"8"}, 1, "3.000000"},
        {"LOG10", {"100"}, 1, "2.000000"}, {"YEAR", {"'2026-03-04'"}, 1, "2026"}, {"MONTH", {"'2026-03-04'"}, 1, "03"},
        {"DAY", {"'2026-03-04'"}, 1, "04"}, {"DAYOFMONTH", {"'2026-03-04'"}, 1, "04"}, {"HOUR", {"'2026-03-04 05:06:07'"}, 1, "05"},
        {"MINUTE", {"'2026-03-04 05:06:07'"}, 1, "06"}, {"SECOND", {"'2026-03-04 05:06:07'"}, 1, "07"},
        {"DAYOFWEEK", {"'2026-03-04'"}, 1, "4"}, {"DAYOFYEAR", {"'2026-03-04'"}, 1, "63"}, {"WEEKDAY", {"'2026-03-04'"}, 1, "2"},
        {"LAST_DAY", {"'2026-03-04'"}, 1, "2026-03-31"}, {"FROM_UNIXTIME", {"86400"}, 1, "1970-01-02 00:00:00"},
        {"UNIX_TIMESTAMP", {"'1970-01-02 00:00:00'"}, 1, "86400"}, {"JSON_UNQUOTE", {"'\"a\"'"}, 1, "a"},
        {"ROUND", {"3.14159", "2"}, 2, "3.14"}, {"TRUNCATE", {"3.14159", "2"}, 2, "3.14"}, {"MOD", {"10", "3"}, 2, "1"},
        {"POW", {"2", "3"}, 2, "8"}, {"POWER", {"2", "10"}, 2, "1024"}, {"LOG", {"8", "2"}, 2, "3.000000"},
        {"LEFT", {"'hello'", "3"}, 2, "hel"}, {"RIGHT", {"'hello'", "3"}, 2, "llo"}, {"REPEAT", {"'ab'", "2"}, 2, "abab"},
        {"INSTR", {"'hello'", "'l'"}, 2, "3"}, {"LOCATE", {"'l'", "'hello'", "1"}, 3, "3"}, {"FORMAT", {"1234.5", "1"}, 2, "1,234.5"},
        {"DATE_FORMAT", {"'2026-03-04'", "'%Y/%m/%d'"}, 2, "2026/03/04"}, {"DATEDIFF", {"'2026-03-04'", "'2026-03-01'"}, 2, "3"},
        {"REGEXP_LIKE", {"'abc'", "'b'"}, 2, "1"}, {"REGEXP_MATCH", {"'abc'", "'b+'"}, 2, "b"}, {"REGEXP_SUBSTR", {"'abc'", "'b+'"}, 2, "b"},
        {"JSON_EXTRACT", {"'{\"a\":1}'", "'$.a'"}, 2, "1"}, {"JSON_VALUE", {"'{\"a\":\"x\"}'", "'$.a'"}, 2, "x"},
        {"SUBSTR", {"'hello'", "2", "3"}, 3, "ell"}, {"SUBSTRING", {"'hello'", "2", "3"}, 3, "ell"},
        {"REPLACE", {"'foobar'", "'foo'", "'baz'"}, 3, "bazbar"}, {"LPAD", {"'5'", "3", "'0'"}, 3, "005"},
        {"RPAD", {"'5'", "3", "'0'"}, 3, "500"}, {"REGEXP_REPLACE", {"'abc'", "'b'", "'x'"}, 3, "axc"},
        {"GREATEST", {"3", "7", "2"}, 3, "7"}, {"LEAST", {"3", "7", "2"}, 3, "2"},
    };
    auto call_text = [](const Call& c, int null_at) {
        std::string out = c.name + "(";
        for (std::size_t i = 0; i < c.args.size(); i++) out += (i ? ", " : "") + (static_cast<int>(i) == null_at ? std::string("NULL") : c.args[i]);
        return out + ")";
    };
    for (const Call& c : calls) {
        INFO(c.name);
        REQUIRE(eval_expr(ex, call_text(c, -1)) == c.expected);
        for (std::size_t i = 0; i < c.args.size() && i < c.value_args; i++) {
            INFO("NULL at argument " << i);
            REQUIRE(eval_expr(ex, call_text(c, static_cast<int>(i))) == "NULL");
        }
    }

    // the same through columns that hold NULL (the function reads the column, not a literal)
    REQUIRE(eval_expr(ex, "LENGTH(s)") == "NULL");
    REQUIRE(eval_expr(ex, "LOWER(s)") == "NULL");
    REQUIRE(eval_expr(ex, "ROUND(n)") == "NULL");
    REQUIRE(eval_expr(ex, "ABS(n)") == "NULL");
    REQUIRE(eval_expr(ex, "MOD(n, 7)") == "NULL");
    REQUIRE(eval_expr(ex, "SUBSTR(s, 1, 2)") == "NULL");
    REQUIRE(eval_expr(ex, "YEAR(dt)") == "NULL");
    REQUIRE(eval_expr(ex, "DATE_ADD(dt, INTERVAL 5 DAY)") == "NULL");
    REQUIRE(eval_expr(ex, "DATE_SUB(dt, INTERVAL 5 DAY)") == "NULL");
    REQUIRE(eval_expr(ex, "CAST(n AS INT)") == "NULL");
    REQUIRE(eval_expr(ex, "CAST(s AS DOUBLE)") == "NULL");
    // an argument that is an expression over a NULL column, and the same expression repeated (resolved once)
    REQUIRE(eval_expr(ex, "ROUND(n / 3, 2)") == "NULL");
    REQUIRE(eval_expr(ex, "GREATEST(n + 1, n + 1)") == "NULL");
    REQUIRE(eval_expr(ex, "ROUND(7 / 3, 2)") == "2.33");
    REQUIRE(eval_expr(ex, "GREATEST(2 + 1, 2 + 1)") == "3");
    REQUIRE(eval_expr(ex, "LEAST(2 + 1, 1 + 1)") == "2");
    REQUIRE(eval_expr(ex, "SUBSTR('hello', 1 + 1, 2)") == "el");

    // the functions that deal with NULL themselves are as they were
    REQUIRE(eval_expr(ex, "COALESCE(NULL, 'x')") == "x");
    REQUIRE(eval_expr(ex, "COALESCE(n, 5)") == "5");
    REQUIRE(eval_expr(ex, "IFNULL(NULL, 'y')") == "y");
    REQUIRE(eval_expr(ex, "NULLIF(NULL, 'a')") == "NULL");
    REQUIRE(eval_expr(ex, "ISNULL(NULL)") == "1");
    REQUIRE(eval_expr(ex, "CONCAT('a', NULL)") == "NULL");
    REQUIRE(eval_expr(ex, "CONCAT_WS(',', 'a', NULL, 'b')") == "a,b");
    REQUIRE(eval_expr(ex, "IF(n > 1, 'y', 'no')") == "no");
}

TEST_CASE("a function argument that is an expression over qualified columns is evaluated, not read as its last column", "[null_expr][scalar_func][qualified]") {
    TempDataDir dir("null_expr_qualified_args");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE p (id INT PRIMARY KEY, v INT, w INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE q (id INT PRIMARY KEY, k INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO p VALUES (1, 10, 2), (2, 7, 3), (3, 4, 5)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO q VALUES (1, 3), (2, 8), (3, 1)").is_ok());
    // ABS(p.id * p.w) used to read `w` (what follows the last dot) instead of multiplying
    REQUIRE(table_cells(ok_text(ex, "SELECT p.id, ABS(p.id * p.w), ROUND(p.v / p.w, 1), ABS(p.v - p.w) FROM p ORDER BY p.id")) ==
            Rows{{"1", "2", "5", "8"}, {"2", "6", "2.3", "4"}, {"3", "15", "0.8", "1"}});
    const Rows joined = {{"1", "30", "7", "13", "3"}, {"2", "56", "1", "15", "6"}, {"3", "4", "3", "5", "1"}};
    REQUIRE(table_cells(ok_text(ex, "SELECT p.id, ROUND(p.v * q.k, 1), ABS(p.v - q.k), GREATEST(p.v + q.k, 0), LEAST(p.w * 2, q.k) "
                                    "FROM p JOIN q ON q.id = p.id ORDER BY p.id")) == joined);
    REQUIRE(table_cells(ok_text(ex, "SELECT x.id, ROUND(x.v * y.k, 1), ABS(x.v - y.k), GREATEST(x.v + y.k, 0), LEAST(x.w * 2, y.k) "
                                    "FROM p x JOIN q y ON y.id = x.id ORDER BY x.id")) == joined);
    // tables that share a column name, and aliases: every `alias.column` of the argument is its table's column (only the first used to be)
    REQUIRE(ex.execute_sql("CREATE TABLE g1 (id INT PRIMARY KEY, g INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE g2 (id INT PRIMARY KEY, g INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO g1 VALUES (1, 3), (2, 5)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO g2 VALUES (1, 6), (2, 8)").is_ok());
    const Rows same_names = {{"1", "36", "18", "3", "36"}, {"2", "64", "40", "3", "64"}};
    REQUIRE(table_cells(ok_text(ex, "SELECT x.id, ROUND(y.g * y.g, 2), ROUND(x.g * y.g, 2), ABS(x.g - y.g), GREATEST(x.g + y.g, y.g * y.g) "
                                    "FROM g1 x JOIN g2 y ON y.id = x.id ORDER BY x.id")) == same_names);
    REQUIRE(table_cells(ok_text(ex, "SELECT g1.id, ROUND(g2.g * g2.g, 2), ROUND(g1.g * g2.g, 2), ABS(g1.g - g2.g), GREATEST(g1.g + g2.g, g2.g * g2.g) "
                                    "FROM g1 JOIN g2 ON g2.id = g1.id ORDER BY g1.id")) == same_names);
    // a bare column name and a plain literal argument still resolve as before
    REQUIRE(table_cells(ok_text(ex, "SELECT id, ABS(v), ROUND(3.14159, 2), ABS(-5) FROM p WHERE id = 1")) == Rows{{"1", "10", "3.14", "5"}});
    REQUIRE(table_cells(ok_text(ex, "SELECT p.id, ABS(p.v), ROUND(p.v, 1) FROM p WHERE p.id = 2")) == Rows{{"2", "7", "7"}});
}

// ---- random expressions over columns with NULLs, checked against a reference computed here ------------------------------------------

namespace {
using Val = std::optional<double>;

// How eval_arith writes an arithmetic result: a whole number as an integer, else six decimals without trailing zeros. The result is
// then read back as text by the operator that uses it, so the reference rounds the same way after every operation.
std::string format_result(double f) {
    if (std::abs(f - std::trunc(f)) < 1e-9 && std::abs(f) < 1e15) return std::to_string(static_cast<std::int64_t>(f));
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.6f", f);
    std::string s = buf;
    s.erase(s.find_last_not_of('0') + 1);
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

Val reread(double f) { return std::strtod(format_result(f).c_str(), nullptr); }

Val apply(char op, const Val& a, const Val& b) {
    if (!a || !b) return std::nullopt;
    switch (op) {
        case '+': return reread(*a + *b);
        case '-': return reread(*a - *b);
        case '*': return reread(*a * *b);
        default: return *b == 0.0 ? std::nullopt : reread(*a / *b);
    }
}

std::string show(const Val& v) { return v ? format_result(*v) : "NULL"; }

struct NRow {
    int id;
    Val a, b, c;
};

struct Operand {
    std::string text;
    int column; // 0 a, 1 b, 2 c, -1 a literal
    double literal;
    Val of(const NRow& r) const { return column == 0 ? r.a : column == 1 ? r.b : column == 2 ? r.c : Val(literal); }
};

} // namespace

TEST_CASE("expressions over NULLable columns match a reference in SELECT, WHERE and UPDATE", "[null_expr][fuzz]") {
    unsigned seed_count = 3; // RUSQL_FUZZ_SEEDS=30 runs a much longer campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 0;
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    std::size_t selects = 0, wheres = 0, updates = 0, null_results = 0;
    for (unsigned k = seed_start; k < seed_start + seed_count; k++) {
        INFO("seed " << k);
        std::mt19937 rng(4242 + k * 31);
        TempDataDir dir("null_expr_random");
        Executor ex(dir.path);
        open_db(ex);
        REQUIRE(ex.execute_sql("CREATE TABLE n (id INT PRIMARY KEY, a INT, b INT, c DOUBLE)").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE m (id INT PRIMARY KEY, a INT, b INT, c DOUBLE, r VARCHAR(40))").is_ok());
        std::vector<NRow> rows;
        std::string nv, mv;
        for (int i = 1; i <= 60; i++) {
            NRow r{i, std::nullopt, std::nullopt, std::nullopt};
            auto pick = [&](bool decimal) -> Val {
                if (rng() % 4 == 0) return std::nullopt;
                int whole = static_cast<int>(rng() % 10) - 3;
                return decimal ? Val(whole + (rng() % 2 ? 0.5 : 0.0)) : Val(static_cast<double>(whole));
            };
            r.a = pick(false);
            r.b = pick(false);
            r.c = pick(true);
            rows.push_back(r);
            std::string tuple = std::to_string(i) + ", " + show(r.a) + ", " + show(r.b) + ", " + show(r.c);
            nv += std::string(nv.empty() ? "(" : ", (") + tuple + ")";
            mv += std::string(mv.empty() ? "(" : ", (") + tuple + ", 'x')";
        }
        REQUIRE(ex.execute_sql("INSERT INTO n VALUES " + nv).is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO m VALUES " + mv).is_ok());

        std::vector<std::string> model(rows.size(), "x"); // m.r as the UPDATEs leave it
        for (int q = 0; q < 70; q++) {
            auto operand = [&]() -> Operand {
                int roll = static_cast<int>(rng() % 10);
                if (roll < 7) {
                    static const char* names[] = {"a", "b", "c"};
                    int col = roll % 3;
                    return {names[col], col, 0.0};
                }
                int lit = static_cast<int>(rng() % 5); // 0 is there for the division by zero
                return {std::to_string(lit), -1, static_cast<double>(lit)};
            };
            static const char ops[] = {'+', '-', '*', '/'};
            Operand x = operand(), y = operand(), z = operand();
            char o1 = ops[rng() % 4], o2 = ops[rng() % 4];
            std::string expr;
            std::function<Val(const NRow&)> value;
            bool numeric = true; // arithmetic shapes: the WHERE and UPDATE checks use them (their text is the number the engine formats)
            bool where_ok = true; // a WHERE that starts with "(" would read as a parenthesised condition
            int shape = static_cast<int>(rng() % 9);
            auto sp = [](char o) { return std::string(" ") + o + " "; };
            switch (shape) {
                case 0:
                case 1:
                    expr = x.text + sp(o1) + y.text;
                    value = [=](const NRow& r) { return apply(o1, x.of(r), y.of(r)); };
                    break;
                case 2: // precedence: * and / bind tighter than + and -
                    expr = x.text + sp(o1) + y.text + sp(o2) + z.text;
                    value = [=](const NRow& r) {
                        if ((o2 == '*' || o2 == '/') && (o1 == '+' || o1 == '-')) return apply(o1, x.of(r), apply(o2, y.of(r), z.of(r)));
                        return apply(o2, apply(o1, x.of(r), y.of(r)), z.of(r));
                    };
                    break;
                case 3:
                    expr = "(" + x.text + sp(o1) + y.text + ")" + sp(o2) + z.text;
                    where_ok = false;
                    value = [=](const NRow& r) { return apply(o2, apply(o1, x.of(r), y.of(r)), z.of(r)); };
                    break;
                case 4:
                    expr = x.text + sp(o1) + "(" + y.text + sp(o2) + z.text + ")";
                    value = [=](const NRow& r) { return apply(o1, x.of(r), apply(o2, y.of(r), z.of(r))); };
                    break;
                case 5:
                    expr = "ABS(" + x.text + sp(o1) + y.text + ")";
                    value = [=](const NRow& r) -> Val {
                        Val v = apply(o1, x.of(r), y.of(r));
                        return v ? Val(std::abs(*v)) : std::nullopt;
                    };
                    numeric = false;
                    break;
                case 6:
                    expr = "ROUND(" + x.text + sp(o1) + y.text + ", 2)";
                    value = [=](const NRow& r) -> Val {
                        Val v = apply(o1, x.of(r), y.of(r));
                        return v ? Val(std::round(*v * 100.0) / 100.0) : std::nullopt;
                    };
                    numeric = false;
                    break;
                default: { // a comparison read as a value
                    static const char* cmps[] = {">", "<", ">=", "<=", "=", "<>"};
                    std::string cmp = cmps[rng() % 6];
                    expr = x.text + " " + cmp + " " + y.text;
                    value = [=](const NRow& r) -> Val {
                        Val a = x.of(r), b = y.of(r);
                        if (!a || !b) return std::nullopt;
                        bool t = cmp == ">" ? *a > *b : cmp == "<" ? *a < *b : cmp == ">=" ? *a >= *b : cmp == "<=" ? *a <= *b
                               : cmp == "=" ? std::abs(*a - *b) < 1e-9 : *a != *b;
                        return t ? 1.0 : 0.0;
                    };
                    numeric = false;
                    break;
                }
            }
            INFO("expression " << expr);
            // a statement with only literals reads no column; keep one column in it so the answer has a row per table row
            auto matches = [&](const std::string& cell, const Val& want) {
                if (!want) return cell == "NULL";
                if (cell == "NULL") return false;
                char* end = nullptr;
                double got = std::strtod(cell.c_str(), &end);
                return end && *end == '\0' && std::abs(got - *want) <= 1e-5 * (1.0 + std::abs(*want));
            };

            // SELECT (half of them with the columns spelled `n.a`: the function shapes used to read the last column of `ABS(n.a - n.b)`)
            std::string select_expr = expr;
            if (rng() % 2) {
                select_expr.clear();
                for (char ch : expr) select_expr += (ch == 'a' || ch == 'b' || ch == 'c') ? std::string("n.") + ch : std::string(1, ch);
            }
            Rows got = table_cells(ok_text(ex, "SELECT id, " + select_expr + " AS e FROM n ORDER BY id"));
            REQUIRE(got.size() == rows.size());
            for (std::size_t i = 0; i < rows.size(); i++) {
                Val want = value(rows[i]);
                INFO("row id " << rows[i].id << " a=" << show(rows[i].a) << " b=" << show(rows[i].b) << " c=" << show(rows[i].c));
                REQUIRE(matches(got[i][1], want));
                if (!want) null_results++;
            }
            selects++;

            // WHERE: the rows whose value is not NULL and passes
            if (numeric && where_ok) {
                int limit = static_cast<int>(rng() % 9) - 2;
                const char* cmp = (rng() % 2) ? ">" : "<=";
                Rows ids = table_cells(ok_text(ex, "SELECT id FROM n WHERE " + expr + " " + cmp + " " + std::to_string(limit) + " ORDER BY id"));
                std::vector<std::string> want_ids;
                for (const NRow& r : rows) {
                    Val v = value(r);
                    if (v && (std::string(cmp) == ">" ? *v > limit : *v <= limit)) want_ids.push_back(std::to_string(r.id));
                }
                REQUIRE(ids.size() == want_ids.size());
                for (std::size_t i = 0; i < ids.size(); i++) REQUIRE(ids[i][0] == want_ids[i]);
                wheres++;
            }

            // UPDATE ... SET r = <expr> on part of the rows, read back through the VARCHAR column
            if (q % 3 == 0 && numeric) {
                int mod = 2 + static_cast<int>(rng() % 3), rem = static_cast<int>(rng() % static_cast<unsigned>(mod));
                REQUIRE(ex.execute_sql("UPDATE m SET r = " + expr + " WHERE id % " + std::to_string(mod) + " = " + std::to_string(rem)).is_ok());
                for (std::size_t i = 0; i < rows.size(); i++) {
                    if (rows[i].id % mod == rem) model[i] = show(value(rows[i]));
                }
                Rows back = table_cells(ok_text(ex, "SELECT id, r FROM m ORDER BY id"));
                REQUIRE(back.size() == rows.size());
                for (std::size_t i = 0; i < rows.size(); i++) {
                    INFO("row id " << rows[i].id);
                    REQUIRE(back[i][1] == model[i]);
                }
                updates++;
            }
        }
    }
    INFO("selects " << selects << " wheres " << wheres << " updates " << updates << " NULL results " << null_results);
    REQUIRE(selects > 0);
    REQUIRE(null_results > 0);
}
