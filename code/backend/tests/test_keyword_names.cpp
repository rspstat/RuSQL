#include <filesystem>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/parser/parser.hpp"

using namespace engine;
namespace fs = std::filesystem;

// Words that MySQL does not reserve may be the name of a column, a table or an alias without quotes: `date`, `year`, `count`, `level`, `user`, `text`, `end` ...
// They are keywords of this engine's lexer, and CREATE TABLE (id INT, date INT) used to be refused ("Expected identifier") and the expressions that
// read such a column (`SELECT count FROM t`, `WHERE level > 1`, `year + 1`) failed or read something else. The test is a differential one: the same
// statements run on a table whose column has the keyword's name and on a table whose column has an ordinary name, and the answers must be the same (but
// for the name).

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

// what a statement answered, with the timing taken out and `word` (as a whole word, in lower case) written as `zz9`
std::string answer(Executor& ex, const std::string& sql, const std::string& word) {
    auto r = ex.execute_sql(sql);
    std::string out = r.is_ok() ? "OK\n" + r.value() : "ERR\n" + r.error();
    out = std::regex_replace(out, std::regex("\\(\\d+\\.\\d+ sec\\)"), "");
    if (word != "zz9") out = std::regex_replace(out, std::regex("(^|[^A-Za-z0-9_])" + word + "(?![A-Za-z0-9_])"), "$1zz9");
    // (a table is as wide as its widest cell, and the name is a cell: the borders and the padding are not what is compared)
    std::string normalized;
    std::istringstream lines(out);
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line[0] == '+') continue;
        normalized += std::regex_replace(line, std::regex(" +"), " ") + "\n";
    }
    return normalized;
}

std::string with_name(std::string sql, const std::string& name) {
    for (std::size_t at = sql.find("{c}"); at != std::string::npos; at = sql.find("{c}", at + name.size())) sql.replace(at, 3, name);
    return sql;
}

const std::vector<std::string>& statements() {
    static const std::vector<std::string> list = {
        "CREATE TABLE kw (id INT PRIMARY KEY, {c} INT, other INT)",
        "INSERT INTO kw ({c}, id, other) VALUES (5, 1, 6), (7, 2, 8), (NULL, 3, 6), (7, 4, NULL)",
        "INSERT INTO kw VALUES (5, 9, 10)",
        "SELECT {c} FROM kw ORDER BY id",
        "SELECT kw.{c} FROM kw ORDER BY id",
        "SELECT {c} AS x FROM kw ORDER BY id",
        "SELECT other AS {c} FROM kw ORDER BY id",
        "SELECT {c}, other FROM kw WHERE ({c} = 7 OR other = 6) AND {c} IS NOT NULL ORDER BY id",
        "SELECT * FROM kw ORDER BY id",
        "SELECT id FROM kw WHERE {c} > 5 ORDER BY id",
        "SELECT id FROM kw WHERE kw.{c} = 7 ORDER BY id",
        "SELECT id FROM kw WHERE other > {c} ORDER BY id",
        "SELECT id FROM kw WHERE {c} IN (5, 7) ORDER BY id",
        "SELECT id FROM kw WHERE {c} NOT IN (5) ORDER BY id",
        "SELECT id FROM kw WHERE {c} BETWEEN 6 AND 8 ORDER BY id",
        "SELECT id FROM kw WHERE {c} IS NULL",
        "SELECT id FROM kw WHERE {c} <> 5 AND {c} < 100 ORDER BY id",
        "SELECT id FROM kw ORDER BY {c}, id",
        "SELECT id FROM kw ORDER BY {c} DESC, id",
        "SELECT id, {c} + 1 FROM kw ORDER BY id",
        "SELECT id, {c} * 2 - other FROM kw ORDER BY id",
        "SELECT id, ABS({c}) FROM kw ORDER BY id",
        "SELECT id, COALESCE({c}, 0) FROM kw ORDER BY id",
        "SELECT id, IFNULL({c}, -1) FROM kw ORDER BY id",
        "SELECT id, CASE WHEN {c} > 5 THEN 1 ELSE 0 END FROM kw ORDER BY id",
        "SELECT id, CASE WHEN {c} IS NULL THEN 'none' ELSE 'some' END FROM kw ORDER BY id",
        "SELECT id, IF({c} > 5, {c}, 0) FROM kw ORDER BY id",
        "SELECT DISTINCT {c} FROM kw ORDER BY {c}",
        "SELECT {c}, COUNT(*) FROM kw GROUP BY {c} ORDER BY {c}",
        "SELECT other, SUM({c}), MIN({c}), MAX({c}), AVG({c}), COUNT({c}) FROM kw GROUP BY other ORDER BY other",
        "SELECT other FROM kw GROUP BY other HAVING SUM({c}) > 5 ORDER BY other",
        "SELECT COUNT(DISTINCT {c}) FROM kw",
        "SELECT SUM({c}) FROM kw",
        "SELECT SUM({c} * 2) FROM kw",
        "SELECT id, ROW_NUMBER() OVER (ORDER BY {c}, id) AS rn FROM kw ORDER BY id",
        "SELECT id, SUM({c}) OVER (PARTITION BY other) AS w FROM kw ORDER BY id",
        "SELECT a.id, b.id FROM kw a JOIN kw b ON a.{c} = b.{c} AND a.id < b.id ORDER BY a.id, b.id",
        "SELECT a.id FROM kw a LEFT JOIN kw b ON a.{c} = b.{c} + 1 ORDER BY a.id",
        "SELECT id FROM kw WHERE {c} > (SELECT MIN({c}) FROM kw) ORDER BY id",
        "SELECT id FROM kw WHERE EXISTS (SELECT 1 FROM kw k2 WHERE k2.{c} = kw.{c} AND k2.id <> kw.id) ORDER BY id",
        "SELECT id FROM kw WHERE {c} IN (SELECT {c} FROM kw WHERE id > 1) ORDER BY id",
        "SELECT id, (SELECT COUNT(*) FROM kw k2 WHERE k2.{c} >= kw.{c}) FROM kw ORDER BY id",
        "SELECT x.{c} FROM (SELECT {c} FROM kw WHERE id < 3) x ORDER BY x.{c}",
        "SELECT {c} FROM kw UNION SELECT other FROM kw ORDER BY 1",
        "CREATE VIEW kv AS SELECT id, {c}, other FROM kw",
        "SELECT {c} FROM kv ORDER BY id",
        "UPDATE kw SET {c} = {c} + 1 WHERE id = 1",
        "UPDATE kw SET other = 0 WHERE {c} = 7",
        "UPDATE kw SET {c} = COALESCE({c}, 0), other = other + {c} WHERE id = 3",
        "SELECT id, {c}, other FROM kw ORDER BY id",
        "CREATE INDEX idx_kw ON kw ({c})",
        "SELECT id FROM kw WHERE {c} = 7 ORDER BY id",
        "SELECT id FROM kw WHERE {c} >= 7 ORDER BY id",
        "INSERT INTO kw (id, {c}, other) VALUES (4, 70, 1) ON DUPLICATE KEY UPDATE {c} = VALUES({c})",
        "SELECT id, {c} FROM kw WHERE id = 4",
        "DELETE FROM kw WHERE {c} = 9",
        "SELECT COUNT(*) FROM kw",
        "DESCRIBE kw",
        "SHOW CREATE TABLE kw",
        "DROP VIEW kv",
        "DROP TABLE kw",
    };
    return list;
}

// every word below is a keyword of the lexer that MySQL does not reserve (and a few that are plain words, for company)
const std::vector<std::string>& words() {
    static const std::vector<std::string> list = {
        "date", "time", "year", "timestamp", "datetime", "text", "boolean", "enum", "json", "count", "sum", "avg", "min", "max", "stddev", "variance", "median",
        "filter", "upper", "lower", "length", "trim", "concat", "substr", "substring", "now", "curdate", "coalesce", "ifnull", "round", "abs", "ceil", "floor",
        "nullif", "lpad", "rpad", "cast", "truncate", "level", "user", "password", "role", "next", "current", "view", "offset", "full", "merge", "matched",
        "following", "preceding", "unbounded", "savepoint", "checkpoint", "isolation", "uncommitted", "committed", "repeatable", "serializable", "vacuum",
        "locks", "modify", "duplicate", "returning", "prepare", "execute", "deallocate", "synonym", "share", "only", "after", "do", "body", "privileges",
        "date_add", "date_sub", "datediff", "date_format", "group_concat", "json_agg", "array_agg", "bit_and", "bit_or", "std", "stddev_pop", "var_pop",
        "grants", "identified", "end", "until", "handler", "tables", "begin", "month", "day", "name", "status", "value", "action", "first", "last", "type",
    };
    return list;
}
} // namespace

TEST_CASE("a keyword that MySQL does not reserve is an ordinary column name", "[keyword_names]") {
    TempDataDir dir_a("kn_a"), dir_b("kn_b");
    Executor with_word(dir_a.path), with_plain(dir_b.path);
    for (Executor* ex : {&with_word, &with_plain}) {
        REQUIRE(ex->execute_sql("CREATE DATABASE d").is_ok());
        REQUIRE(ex->execute_sql("USE d").is_ok());
    }
    for (const std::string& word : words()) {
        for (const std::string& sql : statements()) {
            INFO("word " << word << ": " << with_name(sql, word));
            const std::string got = answer(with_word, with_name(sql, word), word);
            const std::string want = answer(with_plain, with_name(sql, "zz9"), "zz9");
            REQUIRE(got == want);
            // (nothing may fail for a reason that is the name: the reference runs the same statements)
            if (want.rfind("ERR", 0) == 0) FAIL("the reference statement itself failed: " << want);
        }
    }
}

TEST_CASE("keywords as the names of tables, aliases and dotted names", "[keyword_names]") {
    TempDataDir dir("kn_tables");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    auto ok = [&](const std::string& sql) {
        INFO(sql);
        auto r = ex.execute_sql(sql);
        INFO("error: " << (r.is_err() ? r.error() : std::string()));
        REQUIRE(r.is_ok());
        return r.value();
    };
    ok("CREATE TABLE level (id INT PRIMARY KEY, date INT, user INT)");
    ok("INSERT INTO level VALUES (1, 20240101, 7), (2, 20240202, 8)");
    REQUIRE(ok("SELECT level.date, level.user FROM level WHERE level.id = 2").find("20240202") != std::string::npos);
    REQUIRE(ok("SELECT l.date AS year, l.user AS count FROM level l ORDER BY l.id").find("20240101") != std::string::npos);
    // DATE_ADD's unit and a column called like it
    ok("CREATE TABLE ev (id INT PRIMARY KEY, d DATE, month INT)");
    ok("INSERT INTO ev VALUES (1, '2024-01-15', 3)");
    REQUIRE(ok("SELECT DATE_ADD(d, INTERVAL 1 MONTH) FROM ev").find("2024-02-15") != std::string::npos);
    REQUIRE(ok("SELECT DATE_ADD(d, INTERVAL month MONTH) FROM ev").find("2024-04-15") != std::string::npos);
    REQUIRE(ok("SELECT TIMESTAMPDIFF(YEAR, '2020-01-01', '2024-01-01') FROM ev").find("| 4 ") != std::string::npos);
    REQUIRE(ok("SELECT TIMESTAMPDIFF(MONTH, '2020-01-01', '2024-01-01') FROM ev").find("| 48 ") != std::string::npos);
    REQUIRE(ok("SELECT TIMESTAMPDIFF(DAY, '2024-01-01', '2024-01-11') FROM ev").find("| 10 ") != std::string::npos);
    // the functions are still functions with their parentheses, and NOW needs none
    REQUIRE(ok("SELECT YEAR('2024-05-06'), LENGTH('abc'), UPPER('a'), ABS(-3), FLOOR(2.5) FROM ev").find("2024") != std::string::npos);
    REQUIRE(ok("SELECT COUNT(*), MAX(id), MIN(id), SUM(id), AVG(id) FROM ev").find("| 1 ") != std::string::npos);
    REQUIRE(ok("SELECT NOW() IS NOT NULL, CURRENT_TIMESTAMP IS NOT NULL, CURRENT_DATE IS NOT NULL FROM ev").find("| 1 ") != std::string::npos);
    ok("CREATE TABLE nw (id INT PRIMARY KEY, now INT, curdate INT)");
    ok("INSERT INTO nw VALUES (1, 5, 6)");
    REQUIRE(ok("SELECT now, curdate, now + curdate FROM nw").find("| 5 ") != std::string::npos);
    // RANK (reserved by MySQL 8, but a word the engine always took for a column in an expression) is not a window function without its parentheses
    ok("CREATE TABLE rk (id INT PRIMARY KEY, `rank` INT)");
    ok("INSERT INTO rk VALUES (1, 4)");
    REQUIRE(ok("SELECT rank FROM rk").find("| 4 ") != std::string::npos);
    REQUIRE(ok("SELECT id, RANK() OVER (ORDER BY id) AS r FROM rk").find("| 1 ") != std::string::npos);
    // quoted names are still names
    ok("CREATE TABLE q (id INT PRIMARY KEY, `select` INT, `order` INT)");
    ok("INSERT INTO q VALUES (1, 2, 3)");
    REQUIRE(ok("SELECT `select`, `order` FROM q").find("select") != std::string::npos);
}

TEST_CASE("a reserved word is not a name", "[keyword_names]") {
    TempDataDir dir("kn_reserved");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    for (const char* word : {"select", "from", "where", "insert", "delete", "table", "join", "union", "and", "or", "not", "null", "case", "when", "then",
                             "else", "between", "like", "in", "is", "as", "on", "into", "values", "set", "create", "drop", "alter", "group", "order", "by", "having"}) {
        INFO(word);
        REQUIRE(ex.execute_sql(std::string("CREATE TABLE r (id INT PRIMARY KEY, ") + word + " INT)").is_err());
    }
}
