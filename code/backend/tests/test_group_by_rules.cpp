#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// What a query that groups or aggregates may select (MySQL's ONLY_FULL_GROUP_BY, on by default): aggregates, the columns grouped by, what depends on them
// (the primary key of the table is grouped by), and what names no column at all. It used to be accepted whatever it was and then left out of the answer
// (`SELECT COUNT(*), 1 FROM t` was one column, `SELECT COUNT(*), v FROM t` too) or came out empty (`SELECT g, v FROM t GROUP BY g`).

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

void ok(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
}

using Rows = std::vector<std::vector<std::string>>;
const std::string N = "NULL";

Rows cells(const std::string& output, std::vector<std::string>* header = nullptr) {
    Rows rows;
    std::istringstream in(output);
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
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
        if (++bars == 1) {
            if (header) *header = row;
            continue;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

Rows q(Executor& ex, const std::string& sql, std::vector<std::string>* header = nullptr) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
    return cells(r.value(), header);
}

std::string error_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_err());
    return r.error();
}

// a: ids 1..5; v: 10, NULL, 30, 0, 7; s: x, NULL, z, '12', abc; g: 1, 1, 2, 2, 3      dept (id, name), emp (id, dept_id, sal)
void tables(Executor& ex) {
    ok(ex, "CREATE TABLE a (id INT PRIMARY KEY, v INT, s VARCHAR(20), g INT)");
    ok(ex, "INSERT INTO a VALUES (1, 10, 'x', 1), (2, NULL, NULL, 1), (3, 30, 'z', 2), (4, 0, '12', 2), (5, 7, 'abc', 3)");
    ok(ex, "CREATE TABLE dept (id INT PRIMARY KEY, name VARCHAR(20))");
    ok(ex, "CREATE TABLE emp (id INT PRIMARY KEY, dept_id INT, sal INT)");
    ok(ex, "INSERT INTO dept VALUES (1, 'eng'), (2, 'ops'), (3, 'hr')");
    ok(ex, "INSERT INTO emp VALUES (1, 1, 100), (2, 1, 200), (3, 2, 50), (4, 2, 70), (5, 2, 90)");
}
} // namespace

TEST_CASE("an item of no column is part of an aggregate answer", "[group_by_rules]") {
    TempDataDir dir("gr_constants");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    tables(ex);
    std::vector<std::string> header;
    REQUIRE(q(ex, "SELECT COUNT(*), 1 FROM a", &header) == Rows{{"5", "1"}});
    REQUIRE(header == std::vector<std::string>{"COUNT(*)", "1"});
    REQUIRE(q(ex, "SELECT COUNT(*), 'x' FROM a") == Rows{{"5", "x"}});
    REQUIRE(q(ex, "SELECT UPPER('a'), COUNT(*) FROM a") == Rows{{"A", "5"}});
    REQUIRE(q(ex, "SELECT COUNT(*) AS n, UPPER('a') AS u, 1 + 2 AS three FROM a", &header) == Rows{{"5", "A", "3"}});
    REQUIRE(header == std::vector<std::string>{"n", "u", "three"});
    REQUIRE(q(ex, "SELECT MAX(v), CASE WHEN 1 = 1 THEN 'y' ELSE 'n' END, MIN(v) FROM a") == Rows{{"30", "y", "0"}});
    REQUIRE(q(ex, "SELECT SUM(v) + 1, 7, AVG(v) FROM a WHERE g = 2") == Rows{{"31", "7", "15.0000"}});
    // no row: the aggregates are NULL / 0 and the constant is still there
    REQUIRE(q(ex, "SELECT COUNT(*), 1, MAX(v) FROM a WHERE id > 100") == Rows{{"0", "1", N}});
    // HAVING without GROUP BY keeps or drops the one row
    REQUIRE(q(ex, "SELECT COUNT(*), 1 FROM a HAVING COUNT(*) > 2") == Rows{{"5", "1"}});
    REQUIRE(q(ex, "SELECT COUNT(*), 1 FROM a HAVING COUNT(*) > 20").empty());
    // with GROUP BY they always were
    REQUIRE(q(ex, "SELECT g, COUNT(*), UPPER('a') FROM a GROUP BY g ORDER BY g") == Rows{{"1", "2", "A"}, {"2", "2", "A"}, {"3", "1", "A"}});
}

TEST_CASE("a column that is not grouped by is an error", "[group_by_rules]") {
    TempDataDir dir("gr_errors");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    tables(ex);
    const std::string no_group = "In aggregated query without GROUP BY, expression #";
    const std::string grouped = " of SELECT list is not in GROUP BY clause and contains nonaggregated column ";
    REQUIRE(error_of(ex, "SELECT COUNT(*), v FROM a").find(no_group + "2 of SELECT list contains nonaggregated column 'v'") == 0);
    REQUIRE(error_of(ex, "SELECT v, COUNT(*) FROM a").find(no_group + "1 ") == 0);
    REQUIRE(error_of(ex, "SELECT SUM(v) + id FROM a").find("nonaggregated column 'id'") != std::string::npos);
    REQUIRE(error_of(ex, "SELECT MAX(v), UPPER(s) FROM a").find(no_group + "2 ") == 0);
    REQUIRE(error_of(ex, "SELECT *, COUNT(*) FROM a").find(no_group + "1 ") == 0);
    REQUIRE(error_of(ex, "SELECT v FROM a HAVING v > 1").find(no_group) == 0);
    REQUIRE(error_of(ex, "SELECT g, id, COUNT(*) FROM a GROUP BY g").find("Expression #2" + grouped + "'id'") == 0);
    REQUIRE(error_of(ex, "SELECT COUNT(*), s FROM a GROUP BY g").find("Expression #2" + grouped) == 0);
    REQUIRE(error_of(ex, "SELECT g, SUM(v) + id FROM a GROUP BY g").find("Expression #2" + grouped) == 0);
    REQUIRE(error_of(ex, "SELECT g, CASE WHEN id > 1 THEN 1 ELSE 0 END, COUNT(*) FROM a GROUP BY g").find("Expression #2" + grouped) == 0);
    REQUIRE(error_of(ex, "SELECT d.name, COUNT(e.id) FROM dept d JOIN emp e ON e.dept_id = d.id GROUP BY e.dept_id").find(grouped + "'dept.name'") != std::string::npos);
    // (the same in a statement that writes)
    ok(ex, "CREATE TABLE copy_a (g INT, n INT)");
    REQUIRE(error_of(ex, "INSERT INTO copy_a SELECT g, id FROM a GROUP BY g").find(grouped) != std::string::npos);
}

TEST_CASE("what a grouped query may select", "[group_by_rules]") {
    TempDataDir dir("gr_ok");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    tables(ex);
    REQUIRE(q(ex, "SELECT g, COUNT(*) FROM a GROUP BY g ORDER BY g") == Rows{{"1", "2"}, {"2", "2"}, {"3", "1"}});
    REQUIRE(q(ex, "SELECT a.g, COUNT(*) FROM a GROUP BY g ORDER BY g") == Rows{{"1", "2"}, {"2", "2"}, {"3", "1"}});
    REQUIRE(q(ex, "SELECT g, COUNT(*) FROM a GROUP BY a.g ORDER BY g") == Rows{{"1", "2"}, {"2", "2"}, {"3", "1"}});
    REQUIRE(q(ex, "SELECT g + 1, COUNT(*) FROM a GROUP BY g ORDER BY g") == Rows{{"2", "2"}, {"3", "2"}, {"4", "1"}});
    REQUIRE(q(ex, "SELECT g % 2, COUNT(*) FROM a GROUP BY g % 2 ORDER BY 1") == Rows{{"0", "2"}, {"1", "3"}});
    REQUIRE(q(ex, "SELECT UPPER(s), COUNT(*) FROM a GROUP BY UPPER(s) ORDER BY 1").size() == 5);
    REQUIRE(q(ex, "SELECT g, MAX(v) - MIN(v), 'k' FROM a GROUP BY g ORDER BY g") == Rows{{"1", "0", "k"}, {"2", "30", "k"}, {"3", "0", "k"}});
    REQUIRE(q(ex, "SELECT g AS grp, COUNT(*) AS n FROM a GROUP BY grp ORDER BY grp") == Rows{{"1", "2"}, {"2", "2"}, {"3", "1"}});
    REQUIRE(q(ex, "SELECT g, COUNT(*) FROM a GROUP BY 1 ORDER BY 1") == Rows{{"1", "2"}, {"2", "2"}, {"3", "1"}});
    REQUIRE(q(ex, "SELECT g, (SELECT COUNT(*) FROM emp), COUNT(*) FROM a GROUP BY g ORDER BY g") == Rows{{"1", "5", "2"}, {"2", "5", "2"}, {"3", "5", "1"}});
    // a column of the query around is a constant for the subquery that aggregates
    REQUIRE(q(ex, "SELECT id, (SELECT a.v + COUNT(*) FROM emp) FROM a ORDER BY id") == Rows{{"1", "15"}, {"2", N}, {"3", "35"}, {"4", "5"}, {"5", "12"}});
    // a query that does not aggregate selects what it likes
    REQUIRE(q(ex, "SELECT id, v, s FROM a WHERE id = 3") == Rows{{"3", "30", "z"}});
    REQUIRE(q(ex, "SELECT DISTINCT g FROM a ORDER BY g") == Rows{{"1"}, {"2"}, {"3"}});
    REQUIRE(q(ex, "SELECT id, SUM(v) OVER (PARTITION BY g) AS gs FROM a ORDER BY id") ==
            Rows{{"1", "10"}, {"2", "10"}, {"3", "30"}, {"4", "30"}, {"5", "7"}});
}

TEST_CASE("what depends on the primary key that is grouped by is selected", "[group_by_rules]") {
    TempDataDir dir("gr_dependent");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    tables(ex);
    // (the other columns of the row used to come out empty)
    REQUIRE(q(ex, "SELECT id, s, v, COUNT(*) FROM a GROUP BY id ORDER BY id") ==
            Rows{{"1", "x", "10", "1"}, {"2", N, N, "1"}, {"3", "z", "30", "1"}, {"4", "12", "0", "1"}, {"5", "abc", "7", "1"}});
    REQUIRE(q(ex, "SELECT * FROM a GROUP BY id ORDER BY id") ==
            Rows{{"1", "10", "x", "1"}, {"2", N, N, "1"}, {"3", "30", "z", "2"}, {"4", "0", "12", "2"}, {"5", "7", "abc", "3"}});
    REQUIRE(q(ex, "SELECT d.name, COUNT(e.id), SUM(e.sal) FROM dept d LEFT JOIN emp e ON e.dept_id = d.id GROUP BY d.id ORDER BY d.id") ==
            Rows{{"eng", "2", "300"}, {"ops", "3", "210"}, {"hr", "0", N}});
    REQUIRE(q(ex, "SELECT d.id, UPPER(d.name), MAX(e.sal) FROM dept d JOIN emp e ON e.dept_id = d.id GROUP BY d.id ORDER BY d.id") ==
            Rows{{"1", "ENG", "200"}, {"2", "OPS", "90"}});
    // grouping by a column that is not the key does not make the others available
    REQUIRE(error_of(ex, "SELECT d.name, COUNT(*) FROM dept d JOIN emp e ON e.dept_id = d.id GROUP BY e.dept_id").find("'dept.name'") != std::string::npos);
}

TEST_CASE("a view with loose grouping still works", "[group_by_rules]") {
    // (the rule is checked for the statements that are typed, not for what a stored view holds)
    TempDataDir dir("gr_view");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    tables(ex);
    ok(ex, "CREATE VIEW loose AS SELECT g, COUNT(*) AS n FROM a GROUP BY g");
    REQUIRE(q(ex, "SELECT g, n FROM loose ORDER BY g") == Rows{{"1", "2"}, {"2", "2"}, {"3", "1"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM loose") == Rows{{"3"}});
}
