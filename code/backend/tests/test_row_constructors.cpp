#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// Row constructors: (a, b) = (1, 2), <> < <= > >= (the first values that differ decide, NULLs only matter when the values before them do not), (a, b) IN ((1, 2), (3, 4)),
// and (a, b) [NOT] IN (SELECT x, y ...). A comparison with a NULL is UNKNOWN unless another pair of values makes it FALSE, so NOT IN with a NULL somewhere selects nothing.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

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

void make_tables(Executor& ex) {
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, a INT, b INT, n VARCHAR(5))").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE s (k INT PRIMARY KEY, x INT, y INT, m VARCHAR(5))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 1, 1, 'A'), (2, 1, 2, 'b'), (3, 2, 1, 'C'), (4, 2, 2, 'd'), (5, NULL, 1, 'e'), (6, 1, NULL, 'f')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO s VALUES (1, 1, 1, 'a'), (2, 2, 2, 'B'), (3, NULL, 3, 'z')").is_ok());
}
} // namespace

TEST_CASE("row constructors compare and match as MySQL does", "[row_constructor]") {
    TempDataDir dir("row_constructors");
    Executor ex(dir.path);
    make_tables(ex);

    struct Case {
        std::string where;
        std::string expected;
    };
    const std::vector<Case> cases = {
        // a parenthesised value is not a row
        {"(a) = 1", "1,2,6"},
        {"(a + b) * 2 > 5", "2,3,4"},
        // a row against a row
        {"(a, b) = (1, 2)", "2"},
        {"(a, b) <> (1, 2)", "1,3,4,5"},
        {"(a, b) < (2, 1)", "1,2,6"},
        {"(a, b) <= (2, 1)", "1,2,3,6"},
        {"(a, b) > (1, 1)", "2,3,4"},
        {"(a, b) >= (1, 1)", "1,2,3,4"},
        {"(a, b, id) = (2, 2, 4)", "4"},
        {"(a, b, id) < (2, 2, 4)", "1,2,3,6"},
        {"(a, b) = (1 + 1, 4 / 2)", "4"},
        // a list of rows
        {"(a, b) IN ((1, 1), (2, 2))", "1,4"},
        {"(a, b) NOT IN ((1, 1), (2, 2))", "2,3"},
        {"(a, b) IN ((1, NULL))", ""},
        {"(a, b) NOT IN ((1, NULL))", "3,4"},
        // together with other conditions
        {"id > 1 AND (a, b) = (2, 2)", "4"},
        {"(a, b) = (1, 2) OR id = 6", "2,6"},
        {"NOT ((a, b) = (1, 2))", "1,3,4,5"},    // ((NULL, 1) = (1, 2) is FALSE: 1 <> 2 decides it)
        // against a subquery of rows
        {"(a, b) IN (SELECT x, y FROM s)", "1,4"},
        {"(a, b) NOT IN (SELECT x, y FROM s)", "2,3"},
        {"(a, b) IN (SELECT x, y FROM s WHERE x > 9)", ""},
        {"(a, b) NOT IN (SELECT x, y FROM s WHERE x > 9)", "1,2,3,4,5,6"},
        {"(a, b) IN (SELECT x, y FROM s WHERE s.x = t.a)", "1,4"},
        {"(n, a) IN (SELECT m, x FROM s)", "1"},
        {"(a, b) IN (SELECT x, y FROM s WHERE k < 3) OR id = 5", "1,4,5"},
    };
    for (auto& c : cases) {
        INFO("WHERE " << c.where);
        REQUIRE(ids(ex, "SELECT id FROM t WHERE " + c.where + " ORDER BY id") == c.expected);
    }

    // wrong shapes stay errors
    for (const char* bad : {"SELECT id FROM t WHERE (a, b) IN (SELECT x FROM s)", "SELECT id FROM t WHERE (a, b) = (1, 2, 3)", "SELECT id FROM t WHERE (a, b) IN ((1, 2), (3))",
                            "SELECT id FROM t WHERE (a, b) = (SELECT x, y FROM s WHERE k = 1)", "SELECT id FROM t WHERE (a, b) LIKE (1, 2)",
                            "SELECT id FROM t WHERE (a, b) IN (SELECT x FROM s UNION SELECT y FROM s)"}) {
        INFO(bad);
        REQUIRE(ex.execute_sql(bad).is_err());
    }

    // in UPDATE and DELETE
    REQUIRE(ex.execute_sql("UPDATE t SET n = 'hit' WHERE (a, b) IN (SELECT x, y FROM s)").is_ok());
    REQUIRE(ids(ex, "SELECT id FROM t WHERE n = 'hit' ORDER BY id") == "1,4");
    REQUIRE(ex.execute_sql("DELETE FROM t WHERE (a, b) IN ((1, 1), (2, 2))").is_ok());
    REQUIRE(ids(ex, "SELECT id FROM t ORDER BY id") == "2,3,5,6");
}

TEST_CASE("a view over a row-constructor condition means the same after a restart", "[row_constructor]") {
    TempDataDir dir("row_constructor_view");
    {
        Executor ex(dir.path);
        make_tables(ex);
        REQUIRE(ex.execute_sql("CREATE VIEW matched AS SELECT id FROM t WHERE (a, b) IN (SELECT x, y FROM s)").is_ok());
        REQUIRE(ex.execute_sql("CREATE VIEW ordered AS SELECT id FROM t WHERE (a, b) < (2, 1)").is_ok());
        REQUIRE(ids(ex, "SELECT id FROM matched ORDER BY id") == "1,4");
        REQUIRE(ids(ex, "SELECT id FROM ordered ORDER BY id") == "1,2,6");
    }
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ids(ex, "SELECT id FROM matched ORDER BY id") == "1,4");
    REQUIRE(ids(ex, "SELECT id FROM ordered ORDER BY id") == "1,2,6");
}
