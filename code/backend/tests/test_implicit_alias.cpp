#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// A select item may name its result column without AS (`COUNT(*) c`, `a x`, `UPPER(s) u`, `ROW_NUMBER() OVER (...) r`): only a plain identifier is taken as the
// alias, so every word that ends the item (FROM, INTO, WHERE, UNION, LIMIT, FOR, ...) still does. The answer must be exactly what the AS form gives.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

// the table the statement printed (header, borders and rows), without the status and timing lines
std::string table_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_ok());
    std::string out, line;
    std::istringstream in(r.value());
    while (std::getline(in, line)) {
        if (!line.empty() && (line[0] == '|' || line[0] == '+')) out += line + "\n";
    }
    return out;
}
} // namespace

TEST_CASE("a select item can name its column without AS", "[parser][alias]") {
    TempDataDir dir("implicit_alias");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE w (id INT PRIMARY KEY, a INT, b INT, s VARCHAR(5))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO w VALUES (1,1,2,'x'),(2,3,4,'y'),(3,5,6,'z'),(4,7,8,'x')").is_ok());

    // {with AS, without AS}
    const std::vector<std::pair<std::string, std::string>> pairs = {
        {"SELECT COUNT(*) AS c FROM w", "SELECT COUNT(*) c FROM w"},
        {"SELECT s, COUNT(*) AS c FROM w GROUP BY s HAVING c > 1", "SELECT s, COUNT(*) c FROM w GROUP BY s HAVING c > 1"},
        {"SELECT SUM(a) AS total, MAX(b) AS top FROM w", "SELECT SUM(a) total, MAX(b) top FROM w"},
        {"SELECT COUNT(DISTINCT s) AS n FROM w", "SELECT COUNT(DISTINCT s) n FROM w"},
        {"SELECT GROUP_CONCAT(s) AS g FROM w", "SELECT GROUP_CONCAT(s) g FROM w"},
        {"SELECT a AS x FROM w ORDER BY x", "SELECT a x FROM w ORDER BY x"},
        {"SELECT w.a AS x FROM w ORDER BY x", "SELECT w.a x FROM w ORDER BY x"},
        {"SELECT a + 1 AS y FROM w ORDER BY y DESC", "SELECT a + 1 y FROM w ORDER BY y DESC"},
        {"SELECT UPPER(s) AS u FROM w ORDER BY id", "SELECT UPPER(s) u FROM w ORDER BY id"},
        {"SELECT ROUND(a / 3) AS r FROM w ORDER BY id", "SELECT ROUND(a / 3) r FROM w ORDER BY id"},
        {"SELECT CASE WHEN a > 2 THEN 'big' ELSE 'small' END AS k FROM w ORDER BY id", "SELECT CASE WHEN a > 2 THEN 'big' ELSE 'small' END k FROM w ORDER BY id"},
        {"SELECT CAST(a AS CHAR) AS t FROM w ORDER BY id", "SELECT CAST(a AS CHAR) t FROM w ORDER BY id"},
        {"SELECT id, ROW_NUMBER() OVER (ORDER BY id) AS r FROM w", "SELECT id, ROW_NUMBER() OVER (ORDER BY id) r FROM w"},
        {"SELECT id, SUM(a) OVER (ORDER BY id) AS run FROM w", "SELECT id, SUM(a) OVER (ORDER BY id) run FROM w"},
        {"SELECT (SELECT MAX(b) FROM w) AS m", "SELECT (SELECT MAX(b) FROM w) m"},
        {"SELECT 1 AS one, 'x' AS two", "SELECT 1 one, 'x' two"},
        {"SELECT t.c FROM (SELECT COUNT(*) AS c FROM w) AS t", "SELECT t.c FROM (SELECT COUNT(*) c FROM w) t"},
        {"SELECT a AS x, b AS y FROM w WHERE a > 1 ORDER BY x LIMIT 2", "SELECT a x, b y FROM w WHERE a > 1 ORDER BY x LIMIT 2"},
        {"SELECT a AS x FROM w UNION SELECT b AS x FROM w ORDER BY x", "SELECT a x FROM w UNION SELECT b x FROM w ORDER BY x"},
    };
    for (auto& [with_as, without_as] : pairs) {
        INFO("without AS: " << without_as);
        const std::string want = table_of(ex, with_as);
        REQUIRE(table_of(ex, without_as) == want);
    }

    // an alias is really the name of the column
    REQUIRE(table_of(ex, "SELECT COUNT(*) c FROM w").find("| c ") != std::string::npos);
    REQUIRE(table_of(ex, "SELECT a x FROM w").find("| x ") != std::string::npos);

    // in a view, an INSERT ... SELECT and a stored result
    REQUIRE(ex.execute_sql("CREATE VIEW v AS SELECT s, COUNT(*) n FROM w GROUP BY s").is_ok());
    REQUIRE(table_of(ex, "SELECT n FROM v WHERE s = 'x'").find("| 2 ") != std::string::npos);
    REQUIRE(ex.execute_sql("CREATE TABLE w2 (p INT, q INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO w2 SELECT a x, b y FROM w").is_ok());
    REQUIRE(table_of(ex, "SELECT COUNT(*) FROM w2").find("| 4 ") != std::string::npos);

    // two words in a row are still an error, and a word that ends the item still does
    REQUIRE(ex.execute_sql("SELECT a b c FROM w").is_err());
    REQUIRE(ex.execute_sql("SELECT COUNT(*) c d FROM w").is_err());
    REQUIRE(table_of(ex, "SELECT a FROM w LIMIT 1") == table_of(ex, "SELECT a AS a FROM w LIMIT 1"));
}
