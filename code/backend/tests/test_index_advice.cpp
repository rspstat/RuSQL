#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/result_text.hpp"

using namespace engine;
namespace fs = std::filesystem;

// SUGGEST INDEX <SELECT | UPDATE | DELETE> (R65): which indexes would let a statement stop scanning.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

using Rows = std::vector<std::vector<std::string>>;

void ok(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("answer: " << (r.is_err() ? r.error() : r.value().substr(0, 80)));
    REQUIRE(r.is_ok());
}

std::string text_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    return r.is_err() ? "ERROR: " + r.error() : r.value();
}

// the answer of a statement as rows of cells (the header is not one)
Rows rows_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("answer: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
    Rows rows;
    std::istringstream in(r.value());
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars <= 1) continue;
        rows.push_back(result_text::split_row(line));
    }
    return rows;
}

// kind, table_name, columns, detail, create_index, rows_in_table, rows_read_with_index, block, needs_rewrite
enum Cell { Kind, Table, Columns, Detail, Create, InTable, ReadWith, Block, NeedsRewrite };

Rows of_kind(const Rows& rows, const std::string& kind) {
    Rows out;
    for (const auto& r : rows) {
        if (r[Kind] == kind) out.push_back(r);
    }
    return out;
}

bool has_note(const Rows& rows, const std::string& part) {
    for (const auto& r : of_kind(rows, "note")) {
        if (r[Detail].find(part) != std::string::npos) return true;
    }
    return false;
}

void fill(Executor& ex) {
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, cat INT, grp INT, flag INT, name VARCHAR(20))");
    for (int start = 1; start <= 2000; start += 500) {
        std::string sql = "INSERT INTO t VALUES ";
        for (int i = start; i < start + 500; i++) {
            sql += std::string(i == start ? "" : ",") + "(" + std::to_string(i) + "," + std::to_string(i % 200) + "," + std::to_string(i % 20) + "," + std::to_string(i % 2) + ",'n" + std::to_string(i) + "')";
        }
        ok(ex, sql);
    }
}
} // namespace

TEST_CASE("R65: SUGGEST INDEX names the index an equality search needs, with the rows it would read, and sees it once it is made", "[r65][advice]") {
    TempDataDir dir("r65_advice_eq");
    Executor ex(dir.path);
    fill(ex);
    Rows rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE cat = 5");
    REQUIRE(of_kind(rows, "suggest").size() == 1);
    const auto s = of_kind(rows, "suggest")[0];
    REQUIRE(s[Table] == "t");
    REQUIRE(s[Columns] == "cat");
    REQUIRE(s[Create] == "CREATE INDEX idx_t_cat ON t (cat);");
    REQUIRE(s[InTable] == "2000");
    REQUIRE(s[ReadWith] == "10"); // (200 different values over 2,000 rows)
    REQUIRE(s[Detail].find("cat (=)") != std::string::npos);
    // the plan reads the table before the index, and the index after it
    REQUIRE(text_of(ex, "EXPLAIN SELECT * FROM t WHERE cat = 5").find("Seq Scan") != std::string::npos);
    ok(ex, s[Create].substr(0, s[Create].size() - 1)); // (the statement ends with the ; that goes with it in an editor: the engine takes a statement without it)
    REQUIRE(text_of(ex, "EXPLAIN SELECT * FROM t WHERE cat = 5").find("Seq Scan") == std::string::npos);
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE cat = 5");
    REQUIRE(of_kind(rows, "suggest").empty());
    REQUIRE(of_kind(rows, "already").size() == 1);
    REQUIRE(of_kind(rows, "already")[0][Detail].find("idx_t_cat") != std::string::npos);
    // the words around the statement
    REQUIRE(of_kind(rows_of(ex, "suggest indexes for select * from t where grp = 3"), "suggest").size() == 1);
    REQUIRE(of_kind(rows_of(ex, "SUGGEST INDEXES SELECT * FROM t WHERE grp = 3;"), "suggest").size() == 1);
    REQUIRE(of_kind(rows_of(ex, "SUGGEST INDEX FOR SELECT * FROM t WHERE grp = 3"), "suggest").size() == 1);
}

TEST_CASE("R65: SUGGEST INDEX puts the columns in order: equality (the most different values first), then one range, then the ORDER BY", "[r65][advice]") {
    TempDataDir dir("r65_advice_order");
    Executor ex(dir.path);
    fill(ex);
    Rows rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE grp = 3 AND cat = 7");
    REQUIRE(of_kind(rows, "suggest").size() == 1);
    REQUIRE(of_kind(rows, "suggest")[0][Columns] == "cat, grp"); // (cat has 200 different values, grp 20)
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE grp = 3 ORDER BY name LIMIT 5");
    REQUIRE(of_kind(rows, "suggest").size() == 1);
    REQUIRE(of_kind(rows, "suggest")[0][Columns] == "grp, name");
    REQUIRE(of_kind(rows, "suggest")[0][Detail].find("sorted by name") != std::string::npos);
    // a sort without a LIMIT reads every row anyway
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t ORDER BY name");
    REQUIRE(of_kind(rows, "suggest").empty());
    REQUIRE(has_note(rows, "without a LIMIT"));
    // a range: the index is suggested, the rows it would read are not known
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE cat = 7 AND name > 'n5'");
    REQUIRE(of_kind(rows, "suggest").size() == 1);
    REQUIRE(of_kind(rows, "suggest")[0][Columns] == "cat, name");
    // a GROUP BY with nothing to search by
    rows = rows_of(ex, "SUGGEST INDEX SELECT grp, COUNT(*) FROM t GROUP BY grp");
    REQUIRE(of_kind(rows, "suggest").size() == 1);
    REQUIRE(of_kind(rows, "suggest")[0][Columns] == "grp");
}

TEST_CASE("R65: SUGGEST INDEX says why an index would not help", "[r65][advice]") {
    TempDataDir dir("r65_advice_notes");
    Executor ex(dir.path);
    fill(ex);
    ok(ex, "CREATE TABLE tiny (id INT PRIMARY KEY, x INT)");
    ok(ex, "INSERT INTO tiny VALUES (1, 1), (2, 2), (3, 3)");
    Rows rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE flag = 1"); // (two different values over 2,000 rows)
    REQUIRE(of_kind(rows, "suggest").empty());
    REQUIRE(has_note(rows, "would still leave about 1000 of 2000 rows"));
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE UPPER(name) = 'N5'");
    REQUIRE(of_kind(rows, "suggest").empty());
    REQUIRE(has_note(rows, "wraps the column"));
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE name LIKE '%5'");
    REQUIRE(of_kind(rows, "suggest").empty());
    REQUIRE(has_note(rows, "starts with a wildcard"));
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE name LIKE 'n5%'");
    REQUIRE(of_kind(rows, "suggest").size() == 1);
    REQUIRE(of_kind(rows, "suggest")[0][Columns] == "name");
    // an OR over different columns: one index cannot serve it, each branch can as a statement of its own (flagged: it needs a rewrite)
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE cat = 1 OR grp = 2");
    REQUIRE(of_kind(rows, "suggest").size() == 2);
    REQUIRE(of_kind(rows, "suggest")[0][Columns] == "cat");
    REQUIRE(of_kind(rows, "suggest")[1][Columns] == "grp");
    for (const auto& s : of_kind(rows, "suggest")) {
        REQUIRE(s[NeedsRewrite] == "yes");
        REQUIRE(s[Detail].find("branch") != std::string::npos);
    }
    // a branch that no index can serve (a function around the column) leaves the whole OR unserved
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE cat = 1 OR UPPER(name) = 'N5'");
    REQUIRE(of_kind(rows, "suggest").empty());
    REQUIRE(has_note(rows, "OR"));
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE cat = 1 OR cat = 2"); // (the same column on every branch is an IN)
    REQUIRE(of_kind(rows, "suggest").size() == 1);
    REQUIRE(of_kind(rows, "suggest")[0][Columns] == "cat");
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM tiny WHERE x = 1");
    REQUIRE(of_kind(rows, "suggest").empty());
    REQUIRE(has_note(rows, "tiny has only 3 rows"));
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t");
    REQUIRE(has_note(rows, "Nothing to suggest"));
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM missing WHERE a = 1");
    REQUIRE(has_note(rows, "The table missing was not found"));
    // the primary key serves a search by it
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE id = 7");
    REQUIRE(of_kind(rows, "suggest").empty());
    REQUIRE(of_kind(rows, "already").size() == 1);
    REQUIRE(of_kind(rows, "already")[0][Detail].find("PRIMARY") != std::string::npos);
}

TEST_CASE("R65: SUGGEST INDEX reads a join, an UPDATE and a DELETE, and refuses what it cannot read", "[r65][advice]") {
    TempDataDir dir("r65_advice_kinds");
    Executor ex(dir.path);
    fill(ex);
    ok(ex, "CREATE TABLE o (id INT PRIMARY KEY, t_id INT, qty INT)");
    for (int start = 1; start <= 1000; start += 500) {
        std::string sql = "INSERT INTO o VALUES ";
        for (int i = start; i < start + 500; i++) sql += std::string(i == start ? "" : ",") + "(" + std::to_string(i) + "," + std::to_string(i % 400 + 1) + "," + std::to_string(i % 7) + ")";
        ok(ex, sql);
    }
    Rows rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t JOIN o ON o.t_id = t.id WHERE t.cat = 5");
    bool join_suggested = false, cat_suggested = false;
    for (const auto& s : of_kind(rows, "suggest")) {
        if (s[Table] == "o" && s[Columns] == "t_id") join_suggested = true;
        if (s[Table] == "t" && s[Columns] == "cat") cat_suggested = true;
    }
    REQUIRE(join_suggested); // (o has no index on the column it is joined by)
    REQUIRE(cat_suggested);
    REQUIRE(of_kind(rows, "suggest").size() == 2); // (t.id is the primary key)
    rows = rows_of(ex, "SUGGEST INDEX UPDATE t SET flag = 0 WHERE cat = 5");
    REQUIRE(of_kind(rows, "suggest").size() == 1);
    REQUIRE(of_kind(rows, "suggest")[0][Columns] == "cat");
    rows = rows_of(ex, "SUGGEST INDEX DELETE FROM t WHERE grp = 3 AND cat = 5");
    REQUIRE(of_kind(rows, "suggest").size() == 1);
    REQUIRE(of_kind(rows, "suggest")[0][Columns] == "cat, grp");
    // what it cannot read is an error, and nothing is created by asking
    REQUIRE(text_of(ex, "SUGGEST INDEX INSERT INTO t VALUES (9999, 1, 1, 1, 'x')").find("ERROR") != std::string::npos);
    REQUIRE(text_of(ex, "SUGGEST INDEX").find("ERROR") != std::string::npos);
    REQUIRE(text_of(ex, "SUGGEST INDEX SELECT FROM WHERE").find("ERROR") != std::string::npos);
    // a SELECT with no FROM reads no table (the engine's name for the one-row table it reads is not shown)
    rows = rows_of(ex, "SUGGEST INDEX SELECT 1");
    REQUIRE(of_kind(rows, "suggest").empty());
    REQUIRE(has_note(rows, "reads no table"));
    REQUIRE_FALSE(has_note(rows, "_dual_"));
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM t") == (Rows{{"2000"}}));
    REQUIRE(text_of(ex, "SHOW INDEX FROM t").find("idx_") == std::string::npos);
}

// The blocks of a statement -- a subquery after IN / EXISTS / a comparison, a derived table, a CTE, the branches of a UNION -- are read on their own, each with its own tables, and an answer
// says which block a suggestion is for (R70).
TEST_CASE("R70: SUGGEST INDEX reads the subqueries, derived tables, CTEs and UNION branches of a statement, each on its own", "[r70][advice]") {
    TempDataDir dir("r70_advice_blocks");
    Executor ex(dir.path);
    fill(ex);
    ok(ex, "CREATE TABLE o (id INT PRIMARY KEY, t_id INT, qty INT)");
    for (int start = 1; start <= 1000; start += 500) {
        std::string sql = "INSERT INTO o VALUES ";
        for (int i = start; i < start + 500; i++) sql += std::string(i == start ? "" : ",") + "(" + std::to_string(i) + "," + std::to_string(i % 400 + 1) + "," + std::to_string(i % 7) + ")";
        ok(ex, sql);
    }
    auto suggests = [&](const Rows& rows, const std::string& table, const std::string& columns) {
        for (const auto& s : of_kind(rows, "suggest")) {
            if (s[Table] == table && s[Columns] == columns) return s;
        }
        return std::vector<std::string>();
    };
    // IN (SELECT ...): the subquery is searched by its own condition
    Rows rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE id IN (SELECT t_id FROM o WHERE qty = 3)");
    auto s = suggests(rows, "o", "qty");
    REQUIRE_FALSE(s.empty());
    REQUIRE(s[Block] == "an IN subquery");
    REQUIRE(s[Detail].find("in an IN subquery") != std::string::npos);
    // a correlated EXISTS: the subquery looks its rows up once for each row of the outer query, so the outer column is a value to search by
    // (the outer column on either side of the =)
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE EXISTS (SELECT 1 FROM o WHERE t.id = o.t_id AND o.qty = 2)");
    s = suggests(rows, "o", "t_id, qty");
    if (s.empty()) s = suggests(rows, "o", "qty, t_id");
    REQUIRE_FALSE(s.empty());
    REQUIRE(s[Block] == "an EXISTS subquery");
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE EXISTS (SELECT 1 FROM o WHERE o.t_id = t.id AND o.qty = 2)");
    s = suggests(rows, "o", "t_id, qty");
    if (s.empty()) s = suggests(rows, "o", "qty, t_id");
    REQUIRE_FALSE(s.empty());
    REQUIRE(s[Block] == "an EXISTS subquery");
    // a derived table and a scalar subquery
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM (SELECT * FROM o WHERE qty = 4) d");
    s = suggests(rows, "o", "qty");
    REQUIRE_FALSE(s.empty());
    REQUIRE(s[Block] == "the derived table d");
    rows = rows_of(ex, "SUGGEST INDEX SELECT id, (SELECT COUNT(*) FROM o WHERE o.t_id = t.id) FROM t WHERE cat = 2");
    REQUIRE_FALSE(suggests(rows, "t", "cat").empty());
    // a CTE is read where it is defined, and the name it gives is no table
    rows = rows_of(ex, "SUGGEST INDEX WITH big AS (SELECT * FROM o WHERE qty = 5) SELECT * FROM big JOIN t ON 1 = 1 WHERE t.cat = 3");
    s = suggests(rows, "o", "qty");
    REQUIRE_FALSE(s.empty());
    REQUIRE(s[Block] == "the CTE big");
    REQUIRE_FALSE(suggests(rows, "t", "cat").empty());
    REQUIRE_FALSE(has_note(rows, "The table big was not found"));
    // the branches of a UNION
    rows = rows_of(ex, "SUGGEST INDEX SELECT id FROM t WHERE cat = 9 UNION SELECT id FROM o WHERE qty = 6");
    REQUIRE_FALSE(suggests(rows, "t", "cat").empty());
    REQUIRE_FALSE(suggests(rows, "o", "qty").empty());
    // the subqueries of an UPDATE and a DELETE
    rows = rows_of(ex, "SUGGEST INDEX UPDATE t SET flag = 1 WHERE id IN (SELECT t_id FROM o WHERE qty = 1)");
    REQUIRE_FALSE(suggests(rows, "o", "qty").empty());
    rows = rows_of(ex, "SUGGEST INDEX DELETE FROM t WHERE EXISTS (SELECT 1 FROM o WHERE o.qty = 2 AND o.t_id = t.id)");
    REQUIRE_FALSE(of_kind(rows, "suggest").empty());
    // the statement itself has no block name
    rows = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE cat = 5");
    REQUIRE(of_kind(rows, "suggest")[0][Block].empty());
    // nothing was created by asking
    REQUIRE(text_of(ex, "SHOW INDEX FROM o").find("idx_") == std::string::npos);
}

TEST_CASE("R71: with two range tests the index takes the column that tells more values apart, and a mixed ASC / DESC sort is said to stay", "[r71][advice]") {
    TempDataDir dir("r71_advice_range");
    Executor ex(dir.path);
    fill(ex);
    // cat has 200 different values, grp 20: whichever is written first, cat is the range column of the suggestion
    for (const char* sql : {"SUGGEST INDEX SELECT * FROM t WHERE grp > 3 AND cat > 5", "SUGGEST INDEX SELECT * FROM t WHERE cat > 5 AND grp > 3"}) {
        Rows rows = rows_of(ex, sql);
        REQUIRE(of_kind(rows, "suggest").size() == 1);
        REQUIRE(of_kind(rows, "suggest")[0][Columns] == "cat");
        REQUIRE(has_note(rows, "an index narrows by one range column only, so cat is the one in the suggestion"));
    }
    // one range test says nothing of the kind
    REQUIRE_FALSE(has_note(rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE cat > 5"), "one range column only"));
    // ORDER BY in two directions: no one index returns that order
    Rows mixed = rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE flag = 1 ORDER BY cat ASC, grp DESC LIMIT 10");
    REQUIRE(has_note(mixed, "ORDER BY mixes ASC and DESC, so no single index returns the rows in that order"));
    REQUIRE_FALSE(has_note(rows_of(ex, "SUGGEST INDEX SELECT * FROM t WHERE flag = 1 ORDER BY cat DESC, grp DESC LIMIT 10"), "mixes ASC and DESC"));
}
