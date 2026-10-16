#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// x op ANY (subquery), x op SOME (subquery), x op ALL (subquery) for = <> < <= > >=: ANY holds when some value of the subquery satisfies the comparison,
// ALL when every one does; a NULL (of x or of the subquery) leaves the answer UNKNOWN unless another value decides it; an empty subquery is FALSE for ANY and
// TRUE for ALL, whatever x is.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

// the ids a SELECT id ... returns, in the order of its ORDER BY, as "1,3"
std::string ids(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_ok());
    std::istringstream in(r.value());
    std::string line, out;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars < 2) continue;
        std::string cell = line.substr(1, line.find('|', 1) - 1);
        cell.erase(0, cell.find_first_not_of(" \t\r"));
        cell.erase(cell.find_last_not_of(" \t\r") + 1);
        out += (out.empty() ? "" : ",") + cell;
    }
    return out;
}
} // namespace

TEST_CASE("x op ANY / SOME / ALL (subquery)", "[quantified]") {
    TempDataDir dir("quantified");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, a INT, name VARCHAR(10))").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE s (k INT PRIMARY KEY, grp INT, c INT, nm VARCHAR(10))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 1, 'Alice'), (2, 2, 'bob'), (3, 3, 'carol'), (4, NULL, 'dave')").is_ok());
    // group 1: {2, 3}; group 2: {2, NULL}; group 3: {} (no row)
    REQUIRE(ex.execute_sql("INSERT INTO s VALUES (1, 1, 2, 'ALICE'), (2, 1, 3, 'zed'), (3, 2, 2, 'x'), (4, 2, NULL, 'y')").is_ok());

    struct Case {
        std::string where;
        std::string expected;
    };
    const std::vector<Case> cases = {
        // some value of {2, 3}
        {"a > ANY (SELECT c FROM s WHERE grp = 1)", "3"},
        {"a >= ANY (SELECT c FROM s WHERE grp = 1)", "2,3"},
        {"a < ANY (SELECT c FROM s WHERE grp = 1)", "1,2"},
        {"a = ANY (SELECT c FROM s WHERE grp = 1)", "2,3"},
        {"a = SOME (SELECT c FROM s WHERE grp = 1)", "2,3"},
        {"a <> ANY (SELECT c FROM s WHERE grp = 1)", "1,2,3"},
        // every value of {2, 3}
        {"a < ALL (SELECT c FROM s WHERE grp = 1)", "1"},
        {"a <= ALL (SELECT c FROM s WHERE grp = 1)", "1,2"},
        {"a > ALL (SELECT c FROM s WHERE grp = 1)", ""},
        {"a = ALL (SELECT c FROM s WHERE grp = 1)", ""},
        {"a <> ALL (SELECT c FROM s WHERE grp = 1)", "1"},
        // a NULL among the values: {2, NULL}
        {"a = ANY (SELECT c FROM s WHERE grp = 2)", "2"},
        {"a <> ALL (SELECT c FROM s WHERE grp = 2)", ""},        // (1 <> 2 is true but 1 <> NULL is unknown; 2 <> 2 is false only for a = 2, ...)
        {"a > ALL (SELECT c FROM s WHERE grp = 2)", ""},         // (3 > 2 is true, 3 > NULL unknown)
        {"a < ALL (SELECT c FROM s WHERE grp = 2)", ""},         // (1 < 2 true, 1 < NULL unknown; 2 < 2 false)
        {"a >= ANY (SELECT c FROM s WHERE grp = 2)", "2,3"},
        {"NOT (a = ANY (SELECT c FROM s WHERE grp = 2))", ""},   // (NOT TRUE = FALSE for 2, NOT UNKNOWN stays UNKNOWN)
        // no value at all: ANY is FALSE, ALL is TRUE -- a NULL x included
        {"a > ANY (SELECT c FROM s WHERE grp = 9)", ""},
        {"a > ALL (SELECT c FROM s WHERE grp = 9)", "1,2,3,4"},
        {"a = ALL (SELECT c FROM s WHERE grp = 9)", "1,2,3,4"},
        {"NOT (a > ANY (SELECT c FROM s WHERE grp = 9))", "1,2,3,4"},
        // NOT around a quantified comparison
        {"NOT (a > ANY (SELECT c FROM s WHERE grp = 1))", "1,2"},
        // the same as IN / NOT IN
        {"a = ANY (SELECT c FROM s)", "2,3"},                    // (1 = NULL keeps the answer for a = 1 UNKNOWN)
        {"a <> ALL (SELECT c FROM s WHERE c IS NOT NULL)", "1"},
        // texts compare by the default collation
        {"name = ANY (SELECT nm FROM s)", "1"},
        {"name <> ALL (SELECT nm FROM s)", "2,3,4"},
        // correlated: the values come from the group whose number is the id of the row (group 3 and 4 have none)
        {"a <= ANY (SELECT c FROM s WHERE s.grp = t.id)", "1,2"},
        {"a <= ALL (SELECT c FROM s WHERE s.grp = t.id)", "1,3,4"},
        // together with other conditions
        {"a > ANY (SELECT c FROM s WHERE grp = 1) OR id = 1", "1,3"},
        {"a >= ALL (SELECT c FROM s WHERE grp = 1) AND id < 4", "3"},
    };
    for (auto& c : cases) {
        INFO("WHERE " << c.where);
        REQUIRE(ids(ex, "SELECT id FROM t WHERE " + c.where + " ORDER BY id") == c.expected);
    }

    // as a value, in HAVING, and in UPDATE / DELETE
    REQUIRE(ids(ex, "SELECT a > ALL (SELECT c FROM s WHERE grp = 1) FROM t ORDER BY id") == "0,0,0,NULL");
    REQUIRE(ids(ex, "SELECT grp FROM s GROUP BY grp HAVING grp < ALL (SELECT id FROM t WHERE id > 1) ORDER BY grp") == "1");
    REQUIRE(ex.execute_sql("UPDATE t SET name = 'big' WHERE a >= ALL (SELECT c FROM s WHERE grp = 1)").is_ok());
    REQUIRE(ids(ex, "SELECT id FROM t WHERE name = 'big' ORDER BY id") == "3");
    REQUIRE(ex.execute_sql("DELETE FROM t WHERE a < ANY (SELECT c FROM s WHERE grp = 1)").is_ok());
    REQUIRE(ids(ex, "SELECT id FROM t ORDER BY id") == "3,4");

    // words that are not quantifiers stay names; wrong forms stay errors
    REQUIRE(ex.execute_sql("SELECT id FROM t WHERE a > ANY (1, 2)").is_err());
    REQUIRE(ex.execute_sql("SELECT id FROM t WHERE a > ALL").is_err());
    REQUIRE(ex.execute_sql("SELECT id FROM t WHERE a IS ANY (SELECT c FROM s)").is_err());
    // (the quantified comparison is a whole condition: arithmetic after it is a syntax error, not "a > (the one value) + 1")
    REQUIRE(ex.execute_sql("SELECT id FROM t WHERE a > ANY (SELECT c FROM s WHERE k = 1) + 1").is_err());
}

// A view is saved as the JSON of its parsed statement: the quantifier has to be in it.
TEST_CASE("a view over a quantified comparison still means ANY / ALL after a restart", "[quantified]") {
    TempDataDir dir("quantified_view");
    {
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
        REQUIRE(ex.execute_sql("USE d").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, a INT)").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE s (k INT PRIMARY KEY, c INT)").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 1), (2, 2), (3, 3), (4, 9)").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO s VALUES (1, 2), (2, 3)").is_ok());
        REQUIRE(ex.execute_sql("CREATE VIEW above_some AS SELECT id FROM t WHERE a > ANY (SELECT c FROM s)").is_ok());
        REQUIRE(ex.execute_sql("CREATE VIEW above_all AS SELECT id FROM t WHERE a > ALL (SELECT c FROM s)").is_ok());
        REQUIRE(ids(ex, "SELECT id FROM above_some ORDER BY id") == "3,4");
        REQUIRE(ids(ex, "SELECT id FROM above_all ORDER BY id") == "4");
    }
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ids(ex, "SELECT id FROM above_some ORDER BY id") == "3,4");
    REQUIRE(ids(ex, "SELECT id FROM above_all ORDER BY id") == "4");
}
