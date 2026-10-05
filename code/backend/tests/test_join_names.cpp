#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/parser/ast_json.hpp"
#include "engine/parser/parser.hpp"

using namespace engine;
namespace fs = std::filesystem;

// How a join names its tables and their columns, as SQL says it: a table alias is written with or without AS, a table can be used twice
// (a self-join) when each use has an alias, `*` and `t.*` stand for the columns of the tables they name -- each table's own, also where two
// tables share a column name -- NATURAL and USING show the column they merge once, a comma is a cross join, and `JOIN (SELECT ...) AS d`
// joins a derived table. An outer join pads the side that has no row with NULL for every column of every table on that side.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

using Rows = std::vector<std::vector<std::string>>;
const std::string N = "NULL";

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

std::string fails(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_err());
    return r.error();
}

std::vector<std::string> split_row(const std::string& line) {
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
    return row;
}

// the header line and the rows of a printed answer ("0 rows returned." has neither)
std::pair<std::vector<std::string>, Rows> answer(const std::string& text) {
    std::pair<std::vector<std::string>, Rows> out;
    std::istringstream in(text);
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars == 1) out.first = split_row(line);
        else out.second.push_back(split_row(line));
    }
    return out;
}

Rows q(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
    return answer(r.value()).second;
}

std::vector<std::string> header(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
    return answer(r.value()).first;
}

Rows sorted(Rows rows) {
    std::sort(rows.begin(), rows.end());
    return rows;
}

using Header = std::vector<std::string>;

void seed(Executor& ex) {
    ok(ex, "CREATE TABLE a (id INT PRIMARY KEY, g INT, v INT)");
    ok(ex, "INSERT INTO a VALUES (1, 1, 10), (2, 1, NULL), (3, 2, 30), (4, NULL, 40)");
    ok(ex, "CREATE TABLE b (id INT PRIMARY KEY, g INT, k INT, a_id INT)");
    ok(ex, "INSERT INTO b VALUES (1, 1, 100, 1), (2, 2, 200, 1), (3, 2, 300, 3), (4, NULL, 400, 9), (5, 3, 500, NULL)");
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY, b_id INT)");
    ok(ex, "INSERT INTO c VALUES (1, 2), (2, 3), (3, 3)");
}
} // namespace

TEST_CASE("a table alias is written with or without AS, in SELECT, UPDATE and DELETE", "[join_names][alias]") {
    TempDataDir dir("jn_alias");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    REQUIRE(q(ex, "SELECT x.id FROM a AS x WHERE x.v > 15 ORDER BY x.id") == Rows{{"3"}, {"4"}});
    REQUIRE(q(ex, "SELECT x.id FROM a x WHERE x.v > 15 ORDER BY x.id") == Rows{{"3"}, {"4"}});
    const Rows pairs = {{"1", "100"}, {"1", "200"}, {"3", "300"}};
    REQUIRE(q(ex, "SELECT x.id, y.k FROM a AS x JOIN b AS y ON x.id = y.a_id ORDER BY y.k") == pairs);
    REQUIRE(q(ex, "SELECT x.id, y.k FROM a x JOIN b AS y ON x.id = y.a_id ORDER BY y.k") == pairs);
    REQUIRE(q(ex, "SELECT x.id, y.k FROM a AS x LEFT OUTER JOIN b y ON x.id = y.a_id WHERE y.k > 150 ORDER BY y.k") == Rows{{"1", "200"}, {"3", "300"}});

    ok(ex, "CREATE TABLE u (id INT PRIMARY KEY, v INT)");
    ok(ex, "INSERT INTO u VALUES (1, 1), (2, 2), (3, 3)");
    ok(ex, "UPDATE u AS t SET t.v = 99 WHERE t.id = 2");
    REQUIRE(q(ex, "SELECT v FROM u WHERE id = 2") == Rows{{"99"}});
    ok(ex, "UPDATE u t SET t.v = 98 WHERE t.id = 1");
    REQUIRE(q(ex, "SELECT v FROM u WHERE id = 1") == Rows{{"98"}});
    ok(ex, "UPDATE u SET u.v = 97 WHERE u.id = 1");
    REQUIRE(q(ex, "SELECT v FROM u WHERE id = 1") == Rows{{"97"}});
    // a column the table does not have is an error, not a hidden value on every row it matched
    REQUIRE(fails(ex, "UPDATE u SET zz = 5 WHERE id = 1").find("Unknown column 'zz' in 'field list'") != std::string::npos);
    REQUIRE(fails(ex, "UPDATE u AS t SET t.zz = 5").find("Unknown column 'zz' in 'field list'") != std::string::npos);
    REQUIRE(q(ex, "SELECT id, v FROM u ORDER BY id") == Rows{{"1", "97"}, {"2", "99"}, {"3", "3"}});
    ok(ex, "DELETE FROM u AS t WHERE t.id = 3");
    REQUIRE(q(ex, "SELECT id FROM u ORDER BY id") == Rows{{"1"}, {"2"}});
    ok(ex, "UPDATE u AS t JOIN b AS y ON t.id = y.a_id SET t.v = y.k WHERE y.id = 1");
    REQUIRE(q(ex, "SELECT v FROM u WHERE id = 1") == Rows{{"100"}});

    // every use of a table needs a name of its own
    REQUIRE(fails(ex, "SELECT * FROM a JOIN a ON a.id = a.id").find("Not unique table/alias: 'a'") != std::string::npos);
    REQUIRE(fails(ex, "SELECT * FROM a x JOIN b x ON x.id = x.id").find("Not unique table/alias: 'x'") != std::string::npos);
    REQUIRE(fails(ex, "SELECT * FROM a JOIN b ON a.id = b.a_id JOIN b ON b.id = a.id").find("Not unique table/alias: 'b'") != std::string::npos);
    REQUIRE(fails(ex, "SELECT * FROM a x JOIN a ON x.id = a.id").find("Not unique table/alias: 'a'") != std::string::npos);
}

TEST_CASE("a table used twice is two tables: a self-join keeps its uses apart", "[join_names][self_join]") {
    TempDataDir dir("jn_self");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE emp (id INT PRIMARY KEY, name VARCHAR(10), mgr INT)");
    ok(ex, "INSERT INTO emp VALUES (1, 'ann', NULL), (2, 'bob', 1), (3, 'cy', 1), (4, 'di', 2)");
    REQUIRE(q(ex, "SELECT e.name, m.name FROM emp e JOIN emp m ON e.mgr = m.id ORDER BY e.id") == Rows{{"bob", "ann"}, {"cy", "ann"}, {"di", "bob"}});
    REQUIRE(q(ex, "SELECT e.name, m.name FROM emp AS e JOIN emp AS m ON e.mgr = m.id ORDER BY e.id") == Rows{{"bob", "ann"}, {"cy", "ann"}, {"di", "bob"}});
    REQUIRE(q(ex, "SELECT e.name, m.name FROM emp e LEFT JOIN emp m ON e.mgr = m.id ORDER BY e.id") ==
            Rows{{"ann", N}, {"bob", "ann"}, {"cy", "ann"}, {"di", "bob"}});
    REQUIRE(q(ex, "SELECT e.name, m.name FROM emp e RIGHT JOIN emp m ON e.mgr = m.id ORDER BY m.id, e.id") ==
            Rows{{"bob", "ann"}, {"cy", "ann"}, {"di", "bob"}, {N, "cy"}, {N, "di"}});
    REQUIRE(q(ex, "SELECT e.name, m.name, g.name FROM emp e LEFT JOIN emp m ON e.mgr = m.id LEFT JOIN emp g ON m.mgr = g.id ORDER BY e.id") ==
            Rows{{"ann", N, N}, {"bob", "ann", N}, {"cy", "ann", N}, {"di", "bob", "ann"}});
    // not only equality: every pair once
    REQUIRE(q(ex, "SELECT a.name, b.name FROM emp a JOIN emp b ON a.id < b.id ORDER BY a.id, b.id") ==
            Rows{{"ann", "bob"}, {"ann", "cy"}, {"ann", "di"}, {"bob", "cy"}, {"bob", "di"}, {"cy", "di"}});
    REQUIRE(q(ex, "SELECT a.name, b.name FROM emp a JOIN emp b ON a.mgr = b.mgr AND a.id < b.id") == Rows{{"bob", "cy"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM emp a JOIN emp b ON a.mgr = b.mgr") == Rows{{"5"}}); // (bob, cy) both ways, and each of bob, cy, di with itself
    REQUIRE(q(ex, "SELECT m.name, COUNT(e.id) FROM emp m LEFT JOIN emp e ON e.mgr = m.id GROUP BY m.name ORDER BY m.name") ==
            Rows{{"ann", "2"}, {"bob", "1"}, {"cy", "0"}, {"di", "0"}});
    // the first use needs no alias; the WHERE reads each use by its own name
    REQUIRE(q(ex, "SELECT emp.name, m.name FROM emp JOIN emp m ON emp.mgr = m.id WHERE m.name = 'bob'") == Rows{{"di", "bob"}});
    REQUIRE(q(ex, "SELECT e.name FROM emp e JOIN emp m ON e.mgr = m.id WHERE e.id > 2 AND m.id < 2") == Rows{{"cy"}});
    REQUIRE(q(ex, "SELECT e.name FROM emp e JOIN emp m ON e.mgr = m.id ORDER BY m.name DESC, e.name") == Rows{{"di"}, {"bob"}, {"cy"}});
    // a view keeps the aliases (it is stored as a syntax tree)
    ok(ex, "CREATE VIEW boss AS SELECT e.name AS emp, m.name AS boss FROM emp e JOIN emp m ON e.mgr = m.id");
    REQUIRE(q(ex, "SELECT * FROM boss ORDER BY emp") == Rows{{"bob", "ann"}, {"cy", "ann"}, {"di", "bob"}});
    // a table of 3 uses
    REQUIRE(q(ex, "SELECT x.id FROM emp x JOIN emp y ON y.id = x.mgr JOIN emp z ON z.id = y.mgr") == Rows{{"4"}});
}

TEST_CASE("`*` and `t.*` read each table's own columns, also where two tables share a column name", "[join_names][star]") {
    TempDataDir dir("jn_star");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    const std::string on = " FROM a JOIN b ON a.id = b.a_id ORDER BY b.id";
    REQUIRE(header(ex, "SELECT *" + on) == Header{"id", "g", "v", "id", "g", "k", "a_id"});
    REQUIRE(q(ex, "SELECT *" + on) == Rows{{"1", "1", "10", "1", "1", "100", "1"}, {"1", "1", "10", "2", "2", "200", "1"}, {"3", "2", "30", "3", "2", "300", "3"}});
    REQUIRE(header(ex, "SELECT a.*, b.k" + on) == Header{"id", "g", "v", "k"});
    REQUIRE(q(ex, "SELECT a.*, b.k" + on) == Rows{{"1", "1", "10", "100"}, {"1", "1", "10", "200"}, {"3", "2", "30", "300"}});
    REQUIRE(q(ex, "SELECT b.*" + on) == Rows{{"1", "1", "100", "1"}, {"2", "2", "200", "1"}, {"3", "2", "300", "3"}});
    REQUIRE(q(ex, "SELECT b.k, a.*" + on) == Rows{{"100", "1", "1", "10"}, {"200", "1", "1", "10"}, {"300", "3", "2", "30"}});
    REQUIRE(q(ex, "SELECT b.id, b.*" + on).front().size() == 5);
    // aliases, in any order of the tables
    REQUIRE(q(ex, "SELECT y.*, x.id FROM a AS x JOIN b AS y ON x.id = y.a_id ORDER BY y.id") == Rows{{"1", "1", "100", "1", "1"}, {"2", "2", "200", "1", "1"}, {"3", "2", "300", "3", "3"}});
    // an outer join pads the whole side: the unmatched row of a LEFT JOIN, all of b's columns
    REQUIRE(q(ex, "SELECT * FROM a LEFT JOIN b ON a.id = b.a_id WHERE a.id = 2") == Rows{{"2", "1", N, N, N, N, N}});
    // a plain `*` next to another column, and under DISTINCT (rows inserted by separate statements are equal rows)
    ok(ex, "CREATE TABLE dup (x INT, y INT)");
    ok(ex, "INSERT INTO dup VALUES (1, 1)");
    ok(ex, "INSERT INTO dup VALUES (1, 1)");
    ok(ex, "INSERT INTO dup VALUES (2, 2)");
    REQUIRE(sorted(q(ex, "SELECT DISTINCT * FROM dup")) == Rows{{"1", "1"}, {"2", "2"}});
    REQUIRE(header(ex, "SELECT *, x + 1 FROM dup").size() == 3);
    REQUIRE(sorted(q(ex, "SELECT *, x + 1 FROM dup")) == Rows{{"1", "1", "2"}, {"1", "1", "2"}, {"2", "2", "3"}});
    REQUIRE(sorted(q(ex, "SELECT y, * FROM dup")) == Rows{{"1", "1", "1"}, {"1", "1", "1"}, {"2", "2", "2"}});
    REQUIRE(sorted(q(ex, "SELECT dup.* FROM dup")) == Rows{{"1", "1"}, {"1", "1"}, {"2", "2"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM (SELECT DISTINCT * FROM dup) AS t") == Rows{{"2"}});
    // a table the query does not have
    REQUIRE(fails(ex, "SELECT z.* FROM a JOIN b ON a.id = b.a_id").find("Unknown table 'z'") != std::string::npos);
}

TEST_CASE("NATURAL and USING compare the columns they name, show them once, and are joins like any other", "[join_names][using]") {
    TempDataDir dir("jn_using");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    // a(id, g, v) and b(id, g, k, a_id): g is NULL in a4 and b4, which match nothing
    const Rows matched = {{"1", "1", "10", "1", "100", "1"}, {"1", "2", N, "1", "100", "1"}, {"2", "3", "30", "2", "200", "1"}, {"2", "3", "30", "3", "300", "3"}};
    REQUIRE(header(ex, "SELECT * FROM a JOIN b USING (g)") == Header{"g", "id", "v", "id", "k", "a_id"});
    REQUIRE(sorted(q(ex, "SELECT * FROM a JOIN b USING (g)")) == sorted(matched));
    Rows left = matched;
    left.push_back({N, "4", "40", N, N, N});
    REQUIRE(sorted(q(ex, "SELECT * FROM a LEFT JOIN b USING (g)")) == sorted(left));
    // RIGHT / FULL: the merged column of a row without a partner is the right row's
    Rows right = matched;
    right.push_back({N, N, N, "4", "400", "9"});
    right.push_back({"3", N, N, "5", "500", N});
    REQUIRE(sorted(q(ex, "SELECT * FROM a RIGHT JOIN b USING (g)")) == sorted(right));
    Rows full = right;
    full.push_back({N, "4", "40", N, N, N});
    REQUIRE(sorted(q(ex, "SELECT * FROM a FULL OUTER JOIN b USING (g)")) == sorted(full));
    // each table's own value is still there under its name; the plain name is the merged column
    REQUIRE(sorted(q(ex, "SELECT a.g, b.g, g FROM a RIGHT JOIN b USING (g)")) ==
            sorted(Rows{{"1", "1", "1"}, {"1", "1", "1"}, {"2", "2", "2"}, {"2", "2", "2"}, {N, N, N}, {N, "3", "3"}}));
    REQUIRE(sorted(q(ex, "SELECT * FROM a RIGHT JOIN b USING (g) WHERE a.id IS NULL")) == sorted(Rows{{N, N, N, "4", "400", "9"}, {"3", N, N, "5", "500", N}}));
    // two columns, and a column by itself
    REQUIRE(header(ex, "SELECT * FROM a JOIN b USING (id)") == Header{"id", "g", "v", "g", "k", "a_id"});
    REQUIRE(q(ex, "SELECT * FROM a JOIN b USING (id) ORDER BY id") ==
            Rows{{"1", "1", "10", "1", "100", "1"}, {"2", "1", N, "2", "200", "1"}, {"3", "2", "30", "2", "300", "3"}, {"4", N, "40", N, "400", "9"}});
    REQUIRE(q(ex, "SELECT * FROM a JOIN b USING (id, g)") == Rows{{"1", "1", "10", "100", "1"}, {"3", "2", "30", "300", "3"}});
    // NATURAL: every column the tables share (id and g); the NULLs of a4 / b4 match nothing
    REQUIRE(header(ex, "SELECT * FROM a NATURAL JOIN b") == Header{"id", "g", "v", "k", "a_id"});
    REQUIRE(q(ex, "SELECT * FROM a NATURAL JOIN b ORDER BY id") == Rows{{"1", "1", "10", "100", "1"}, {"3", "2", "30", "300", "3"}});
    // no column in common: every pair
    ok(ex, "CREATE TABLE w (x INT)");
    ok(ex, "INSERT INTO w VALUES (7), (8)");
    REQUIRE(q(ex, "SELECT COUNT(*) FROM a NATURAL JOIN w") == Rows{{"8"}});
    // USING over a join of three, and with aliases
    REQUIRE(q(ex, "SELECT x.id, y.k FROM a x JOIN b y USING (id) WHERE y.k > 250 ORDER BY x.id") == Rows{{"3", "300"}, {"4", "400"}});
    REQUIRE(fails(ex, "SELECT * FROM a JOIN b USING (zz)").find("Unknown column 'zz' in 'from clause'") != std::string::npos);
}

TEST_CASE("a comma is a cross join, and what the WHERE pairs up is joined on", "[join_names][comma]") {
    TempDataDir dir("jn_comma");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    REQUIRE(q(ex, "SELECT a.id, b.k FROM a, b WHERE a.id = b.a_id ORDER BY b.k") == Rows{{"1", "100"}, {"1", "200"}, {"3", "300"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM a, b") == Rows{{"20"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM a x, a y") == Rows{{"16"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM a x, a y WHERE x.id < y.id") == Rows{{"6"}});
    REQUIRE(q(ex, "SELECT a.id, b.k, c.id FROM a, b, c WHERE a.id = b.a_id AND b.id = c.b_id ORDER BY c.id") == Rows{{"1", "200", "1"}, {"3", "300", "2"}, {"3", "300", "3"}});
    REQUIRE(q(ex, "SELECT a.id, c.id FROM a, b, c WHERE a.id = b.a_id AND b.id = c.b_id AND c.id > 1 ORDER BY c.id") == Rows{{"3", "2"}, {"3", "3"}});
    REQUIRE(q(ex, "SELECT a.id, c.id FROM a, b JOIN c ON b.id = c.b_id WHERE a.id = b.a_id ORDER BY c.id") == Rows{{"1", "1"}, {"3", "2"}, {"3", "3"}});
    // a WHERE that is not a plain equality of two columns keeps its meaning
    REQUIRE(q(ex, "SELECT a.id, b.id FROM a, b WHERE a.id = b.a_id OR b.id = 5 ORDER BY b.id, a.id") ==
            Rows{{"1", "1"}, {"1", "2"}, {"3", "3"}, {"1", "5"}, {"2", "5"}, {"3", "5"}, {"4", "5"}});
    REQUIRE(q(ex, "SELECT x.id, y.id FROM a x, b y WHERE x.id = y.a_id AND y.k > 150 ORDER BY y.id") == Rows{{"1", "2"}, {"3", "3"}});
}

TEST_CASE("JOIN (SELECT ...) AS d joins a derived table, empty or not", "[join_names][derived]") {
    TempDataDir dir("jn_derived");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    REQUIRE(q(ex, "SELECT a.id, d.cnt FROM a JOIN (SELECT a_id, COUNT(*) AS cnt FROM b GROUP BY a_id) AS d ON a.id = d.a_id ORDER BY a.id") ==
            Rows{{"1", "2"}, {"3", "1"}});
    REQUIRE(q(ex, "SELECT a.id, d.cnt FROM a LEFT JOIN (SELECT a_id, COUNT(*) AS cnt FROM b GROUP BY a_id) d ON a.id = d.a_id ORDER BY a.id") ==
            Rows{{"1", "2"}, {"2", N}, {"3", "1"}, {"4", N}});
    REQUIRE(q(ex, "SELECT d.id, d.k FROM (SELECT id, k FROM b WHERE k > 250) AS d ORDER BY d.id") == Rows{{"3", "300"}, {"4", "400"}, {"5", "500"}});
    REQUIRE(q(ex, "SELECT x.id, d.* FROM a x JOIN (SELECT * FROM b WHERE k > 250) AS d ON x.id = d.a_id") == Rows{{"3", "3", "2", "300", "3"}});
    // two derived tables
    REQUIRE(q(ex, "SELECT d1.id, d2.id FROM (SELECT id FROM a WHERE id < 3) d1 JOIN (SELECT id FROM b WHERE id > 3) AS d2 ON d1.id < d2.id ORDER BY d1.id, d2.id") ==
            Rows{{"1", "4"}, {"1", "5"}, {"2", "4"}, {"2", "5"}});
    // no row: the columns are still the select list's
    REQUIRE(q(ex, "SELECT a.id FROM a JOIN (SELECT id, k FROM b WHERE k > 9999) AS d ON a.id = d.id").empty());
    REQUIRE(q(ex, "SELECT a.id, d.k FROM a LEFT JOIN (SELECT id, k FROM b WHERE k > 9999) AS d ON a.id = d.id ORDER BY a.id") ==
            Rows{{"1", N}, {"2", N}, {"3", N}, {"4", N}});
    REQUIRE(q(ex, "SELECT d.id, b.id FROM (SELECT id FROM a WHERE v > 9999) AS d RIGHT JOIN b ON d.id = b.id ORDER BY b.id") ==
            Rows{{N, "1"}, {N, "2"}, {N, "3"}, {N, "4"}, {N, "5"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM (SELECT id FROM a WHERE v > 9999) AS d") == Rows{{"0"}});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM a JOIN (SELECT id FROM b WHERE k > 9999) AS d ON a.id = d.id") == Rows{{"0"}});
    // the alias is a table only for the statement
    REQUIRE(fails(ex, "SELECT * FROM d").find("not found") != std::string::npos);
    REQUIRE(fails(ex, "SELECT * FROM a JOIN (SELECT id FROM b) AS a ON a.id = a.id").find("Not unique table/alias: 'a'") != std::string::npos);
    REQUIRE(fails(ex, "SELECT * FROM a JOIN (SELECT id FROM b) AS d ON d.id = a.id JOIN (SELECT id FROM c) AS d ON d.id = a.id").find("Not unique") != std::string::npos);
}

TEST_CASE("an outer join pads every column of every table on the side without a row", "[join_names][padding]") {
    TempDataDir dir("jn_pad");
    Executor ex(dir.path);
    open_db(ex);
    seed(ex);
    // the left side has no row at all, then a joined table's own columns (a2.*) are NULL too
    REQUIRE(sorted(q(ex, "SELECT x.id, y.k, z.v FROM (SELECT id FROM a WHERE v > 9999) AS x RIGHT JOIN b y ON x.id = y.id RIGHT JOIN a z ON y.a_id = z.id")) ==
            sorted(Rows{{N, "100", "10"}, {N, "200", "10"}, {N, N, N}, {N, "300", "30"}, {N, N, "40"}}));
    // FULL OUTER JOIN of a join: b's side of an unmatched c row is NULL, `b.id` as well as the plain `id`
    REQUIRE(sorted(q(ex, "SELECT b.id, c.id FROM a JOIN b ON a.id = b.a_id FULL OUTER JOIN c ON c.b_id = b.id + 10")) ==
            sorted(Rows{{"1", N}, {"2", N}, {"3", N}, {N, "1"}, {N, "2"}, {N, "3"}}));
    // an earlier table's columns read through the alias of a self-join
    ok(ex, "CREATE TABLE emp (id INT PRIMARY KEY, name VARCHAR(10), mgr INT)");
    ok(ex, "INSERT INTO emp VALUES (1, 'ann', NULL), (2, 'bob', 1)");
    REQUIRE(sorted(q(ex, "SELECT e.name, m.name, o.name FROM emp e JOIN emp m ON e.mgr = m.id RIGHT JOIN emp o ON o.mgr = 99")) ==
            sorted(Rows{{N, N, "ann"}, {N, N, "bob"}}));
}

TEST_CASE("the alias of a used-twice table and `t.*` survive the syntax tree's JSON", "[join_names][json]") {
    Parser p("SELECT x.*, y.id FROM emp x JOIN emp y ON x.mgr = y.id");
    auto parsed = p.parse();
    REQUIRE(parsed.is_ok());
    const Statement stmt = parsed.value();
    auto& select = std::get<Statement::Select>(stmt.data);
    REQUIRE(select.joins.size() == 1);
    REQUIRE(select.joins[0].table == "emp");
    REQUIRE(select.joins[0].alias == "y");
    auto* star = std::get_if<SelectColumn::All>(&select.columns[0].data);
    REQUIRE(star);
    REQUIRE(star->table == "emp"); // the first use is known by its table name, like any alias
    nlohmann::json j = stmt;
    Statement back = nlohmann::json::parse(j.dump()).get<Statement>();
    REQUIRE(nlohmann::json(back) == j);
    REQUIRE(std::get<Statement::Select>(back.data).joins[0].alias == "y");
    // a statement stored before aliases were kept has no "alias" key and reads as before
    nlohmann::json old = j;
    old["Select"]["joins"][0].erase("alias");
    REQUIRE(std::get<Statement::Select>(old.get<Statement>().data).joins[0].alias.empty());
    // a plain `*` still serializes as before
    Parser plain("SELECT * FROM emp");
    nlohmann::json jp = plain.parse().value();
    REQUIRE(jp["Select"]["columns"][0] == "All");
}

namespace {
// ---- a model of what a join answers ------------------------------------------------------------------------------------------------
using Cell = std::optional<int>;
using TRow = std::vector<Cell>;

struct ModelTable {
    std::string name;
    std::vector<std::string> cols;
    std::vector<TRow> rows;
};

enum class T3 { F, T, U };
T3 cmp3(const std::string& op, Cell a, Cell b) {
    if (!a || !b) return T3::U;
    bool r = op == "=" ? *a == *b : op == "<" ? *a < *b : op == "<=" ? *a <= *b : op == ">" ? *a > *b : op == ">=" ? *a >= *b : *a != *b;
    return r ? T3::T : T3::F;
}

struct Inst {
    int table;
    std::string alias;
    bool as;
};
struct Operand {
    int inst = -1; // -1: a constant
    int col = 0;
    int constant = 0;
};
struct Atom {
    std::string op;
    Operand l, r;
};
struct JoinSpec {
    std::string kind; // INNER LEFT RIGHT FULL
    std::vector<Atom> on; // AND
};
struct Spec {
    std::vector<Inst> insts;
    std::vector<JoinSpec> joins;
    std::vector<Atom> where; // AND
    // select: (inst, col) pairs; inst = -1 is `*`, col = -1 is `inst.*`
    std::vector<std::pair<int, int>> items;
};

const ModelTable* g_tables = nullptr; // p, q, r of the current seed

std::string eff(const Spec& s, int i) { return s.insts[static_cast<std::size_t>(i)].alias.empty() ? g_tables[s.insts[static_cast<std::size_t>(i)].table].name : s.insts[static_cast<std::size_t>(i)].alias; }

std::string operand_sql(const Spec& s, const Operand& o) {
    if (o.inst < 0) return std::to_string(o.constant);
    return eff(s, o.inst) + "." + g_tables[s.insts[static_cast<std::size_t>(o.inst)].table].cols[static_cast<std::size_t>(o.col)];
}

std::string atoms_sql(const Spec& s, const std::vector<Atom>& atoms) {
    std::string out;
    for (auto& a : atoms) out += (out.empty() ? "" : " AND ") + operand_sql(s, a.l) + " " + a.op + " " + operand_sql(s, a.r);
    return out;
}

Cell value_of(const Spec& s, const std::vector<const TRow*>& tup, const Operand& o) {
    if (o.inst < 0) return o.constant;
    const TRow* row = tup[static_cast<std::size_t>(o.inst)];
    (void)s;
    return row ? (*row)[static_cast<std::size_t>(o.col)] : std::nullopt;
}

bool atoms_hold(const Spec& s, const std::vector<const TRow*>& tup, const std::vector<Atom>& atoms) {
    for (auto& a : atoms) {
        if (cmp3(a.op, value_of(s, tup, a.l), value_of(s, tup, a.r)) != T3::T) return false;
    }
    return true;
}

std::vector<std::vector<const TRow*>> model_rows(const Spec& s) {
    std::vector<std::vector<const TRow*>> current;
    for (auto& r : g_tables[s.insts[0].table].rows) current.push_back({&r});
    for (std::size_t i = 1; i < s.insts.size(); i++) {
        const JoinSpec& j = s.joins[i - 1];
        const auto& right = g_tables[s.insts[i].table].rows;
        std::vector<std::vector<const TRow*>> out;
        std::vector<bool> used(right.size(), false);
        for (auto& l : current) {
            bool hit = false;
            for (std::size_t ri = 0; ri < right.size(); ri++) {
                auto tup = l;
                tup.push_back(&right[ri]);
                if (atoms_hold(s, tup, j.on)) {
                    out.push_back(tup);
                    used[ri] = true;
                    hit = true;
                }
            }
            if (!hit && (j.kind == "LEFT" || j.kind == "FULL")) {
                auto tup = l;
                tup.push_back(nullptr);
                out.push_back(tup);
            }
        }
        if (j.kind == "RIGHT" || j.kind == "FULL") {
            for (std::size_t ri = 0; ri < right.size(); ri++) {
                if (used[ri]) continue;
                std::vector<const TRow*> tup(i, nullptr);
                tup.push_back(&right[ri]);
                out.push_back(tup);
            }
        }
        current = std::move(out);
    }
    std::vector<std::vector<const TRow*>> kept;
    for (auto& t : current) {
        if (atoms_hold(s, t, s.where)) kept.push_back(t);
    }
    return kept;
}

std::string show(Cell c) { return c ? std::to_string(*c) : "NULL"; }
} // namespace

TEST_CASE("random joins answer what the model says: aliases, self-joins, stars, every join type", "[join_names][fuzz]") {
    unsigned seed_count = 4; // RUSQL_FUZZ_SEEDS=40 runs a much longer campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 1;
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    const std::vector<std::string> ops = {"=", "<", "<=", ">", ">=", "<>"};
    for (unsigned seed = seed_start; seed < seed_start + seed_count; seed++) {
        std::mt19937 rng(seed);
        auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
        TempDataDir dir("jn_fuzz_" + std::to_string(seed));
        Executor ex(dir.path);
        open_db(ex);
        std::vector<ModelTable> tables = {{"p", {"id", "g", "v"}, {}}, {"q", {"id", "g", "w", "p_id"}, {}}, {"r", {"id", "g", "k"}, {}}};
        const int sizes[3] = {5 + static_cast<int>(pick(4)), 6 + static_cast<int>(pick(5)), 3 + static_cast<int>(pick(3))};
        for (std::size_t t = 0; t < tables.size(); t++) {
            std::string ddl = "CREATE TABLE " + tables[t].name + " (id INT PRIMARY KEY";
            for (std::size_t c = 1; c < tables[t].cols.size(); c++) ddl += ", " + tables[t].cols[c] + " INT";
            ok(ex, ddl + ")");
            std::string values;
            for (int i = 1; i <= sizes[t]; i++) {
                TRow row = {i};
                for (std::size_t c = 1; c < tables[t].cols.size(); c++) {
                    const int hi = tables[t].cols[c] == "p_id" ? sizes[0] + 1 : 3;
                    row.push_back(pick(7) == 0 ? Cell() : Cell(static_cast<int>(pick(static_cast<std::size_t>(hi) + 1))));
                }
                std::string tuple;
                for (auto& c : row) tuple += (tuple.empty() ? "" : ", ") + show(c);
                values += (values.empty() ? "" : ", ") + std::string("(") + tuple + ")";
                tables[t].rows.push_back(row);
            }
            ok(ex, "INSERT INTO " + tables[t].name + " VALUES " + values);
        }
        g_tables = tables.data();

        for (int round = 0; round < 40; round++) {
            Spec s;
            const std::size_t n = 2 + pick(3);
            std::set<int> seen;
            for (std::size_t i = 0; i < n; i++) {
                int t = static_cast<int>(pick(3));
                if (i == 1 && pick(3) == 0) t = s.insts[0].table; // a self-join is common
                const bool repeated = seen.count(t) > 0;
                seen.insert(t);
                s.insts.push_back({t, repeated || pick(2) == 0 ? tables[static_cast<std::size_t>(t)].name + std::to_string(i) : "", pick(2) == 0});
            }
            auto operand = [&](std::size_t up_to) {
                Operand o;
                o.inst = static_cast<int>(pick(up_to + 1));
                o.col = static_cast<int>(pick(tables[static_cast<std::size_t>(s.insts[static_cast<std::size_t>(o.inst)].table)].cols.size()));
                return o;
            };
            for (std::size_t i = 1; i < n; i++) {
                JoinSpec j;
                j.kind = std::vector<std::string>{"INNER", "LEFT", "RIGHT", "FULL"}[pick(4)];
                Atom a{pick(3) == 0 ? ops[pick(ops.size())] : "=", {}, {}};
                a.l = operand(i);
                a.l.inst = static_cast<int>(i);
                a.l.col = static_cast<int>(pick(tables[static_cast<std::size_t>(s.insts[i].table)].cols.size()));
                a.r = operand(i - 1);
                j.on.push_back(a);
                if (pick(3) == 0) {
                    Atom extra{ops[pick(ops.size())], operand(i), {}};
                    extra.r.constant = static_cast<int>(pick(4));
                    j.on.push_back(extra);
                }
                s.joins.push_back(j);
            }
            if (pick(2) == 0) {
                Atom a{ops[pick(ops.size())], operand(n - 1), {}};
                a.r.constant = static_cast<int>(pick(4));
                s.where.push_back(a);
            }
            const std::size_t kind = pick(6);
            if (kind == 0) s.items = {{-1, 0}};
            else if (kind == 1) s.items = {{static_cast<int>(pick(n)), -1}, {static_cast<int>(pick(n)), 1}};
            else {
                for (std::size_t k = 0, count = 1 + pick(3); k < count; k++) {
                    int inst = static_cast<int>(pick(n));
                    s.items.push_back({inst, static_cast<int>(pick(tables[static_cast<std::size_t>(s.insts[static_cast<std::size_t>(inst)].table)].cols.size()))});
                }
            }

            std::string from;
            for (std::size_t i = 0; i < n; i++) {
                const Inst& in = s.insts[i];
                std::string use = tables[static_cast<std::size_t>(in.table)].name + (in.alias.empty() ? "" : (in.as ? " AS " : " ") + in.alias);
                if (i == 0) from = use;
                else from += " " + s.joins[i - 1].kind + " JOIN " + use + " ON " + atoms_sql(s, s.joins[i - 1].on);
            }
            std::string select;
            Header expect_header;
            std::vector<std::pair<int, int>> columns; // (inst, col) of the answer, in order
            for (auto& [inst, col] : s.items) {
                if (inst < 0) {
                    for (std::size_t i = 0; i < n; i++)
                        for (std::size_t c = 0; c < tables[static_cast<std::size_t>(s.insts[i].table)].cols.size(); c++) columns.push_back({static_cast<int>(i), static_cast<int>(c)});
                    select += (select.empty() ? "" : ", ") + std::string("*");
                } else if (col < 0) {
                    for (std::size_t c = 0; c < tables[static_cast<std::size_t>(s.insts[static_cast<std::size_t>(inst)].table)].cols.size(); c++) columns.push_back({inst, static_cast<int>(c)});
                    select += (select.empty() ? "" : ", ") + eff(s, inst) + ".*";
                } else {
                    columns.push_back({inst, col});
                    select += (select.empty() ? "" : ", ") + eff(s, inst) + "." + tables[static_cast<std::size_t>(s.insts[static_cast<std::size_t>(inst)].table)].cols[static_cast<std::size_t>(col)];
                }
            }
            for (auto& [inst, col] : columns) expect_header.push_back(tables[static_cast<std::size_t>(s.insts[static_cast<std::size_t>(inst)].table)].cols[static_cast<std::size_t>(col)]);
            std::string sql = "SELECT " + select + " FROM " + from;
            if (!s.where.empty()) sql += " WHERE " + atoms_sql(s, s.where);

            Rows expect;
            for (auto& tup : model_rows(s)) {
                std::vector<std::string> row;
                for (auto& [inst, col] : columns) {
                    const TRow* r = tup[static_cast<std::size_t>(inst)];
                    row.push_back(r ? show((*r)[static_cast<std::size_t>(col)]) : N);
                }
                expect.push_back(row);
            }
            INFO("seed " << seed << ", round " << round << ": " << sql);
            auto result = ex.execute_sql(sql);
            if (result.is_err()) INFO("error: " << result.error());
            REQUIRE(result.is_ok());
            auto got = answer(result.value());
            if (!expect.empty()) REQUIRE(got.first == expect_header);
            REQUIRE(sorted(got.second) == sorted(expect));
        }
        g_tables = nullptr;
    }
}
