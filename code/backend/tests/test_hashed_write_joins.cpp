#include <chrono>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/result_text.hpp"

using namespace engine;
namespace fs = std::filesystem;

// MERGE and the multi-table UPDATE / DELETE meet each row of one table with every row of the other (a merged row for each pair of rows: 300 x 300,000 rows took 109 seconds). When the ON
// holds an equality of a column of each table that a hash can answer, the rows are hashed on it and only the rows with the same key are met -- and each of those pairs still has to pass the
// whole ON. The answer must be the loop's, whatever the keys are (R68). Each test runs one statement on two copies of the same data: its ON as written (hashed), and its ON as `(... OR 1 = 0)`
// (the same condition, no longer an AND-ed equality: the loop over every pair), and requires the same answer and the same tables.

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
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
}

std::string answer(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
    return r.value();
}

Rows q(Executor& ex, const std::string& sql) {
    std::istringstream in(answer(ex, sql));
    std::string line;
    int bars = 0;
    Rows rows;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars <= 1) continue;
        rows.push_back(result_text::split_row(line));
    }
    return rows;
}

// "_h": the copy whose ON is hashed; "_n": the copy whose ON is not (the loop)
void make_pair_of_tables(Executor& ex, const std::string& target_columns, const std::string& target_rows, const std::string& source_columns, const std::string& source_rows) {
    for (const char* copy : {"_h", "_n"}) {
        ok(ex, std::string("CREATE TABLE t") + copy + " (" + target_columns + ")");
        ok(ex, std::string("CREATE TABLE s") + copy + " (" + source_columns + ")");
        ok(ex, std::string("INSERT INTO t") + copy + " VALUES " + target_rows);
        ok(ex, std::string("INSERT INTO s") + copy + " VALUES " + source_rows);
    }
}

// `pattern` names the tables t@ and s@ and holds the ON as `ON<<` ... `>>`; the hashed copy has it as written, the other copy has it as `(... OR 1 = 0)`
std::string statement_for(const std::string& pattern, const char* copy) {
    std::string out;
    const bool hashed = std::string(copy) == "_h";
    for (std::size_t i = 0; i < pattern.size(); i++) {
        if (pattern[i] == '@') out += copy;
        else if (pattern.compare(i, 2, "<<") == 0) i += 1, out += hashed ? "" : "(";
        else if (pattern.compare(i, 2, ">>") == 0) i += 1, out += hashed ? "" : " OR 1 = 0)";
        else out += pattern[i];
    }
    return out;
}

void drop_pair_of_tables(Executor& ex) {
    for (const char* table : {"t_h", "s_h", "t_n", "s_n"}) ok(ex, std::string("DROP TABLE ") + table);
}

void same_answer_and_tables(Executor& ex, const std::string& pattern, const std::vector<std::string>& order_by_for_tables) {
    INFO(pattern);
    const std::string hashed = answer(ex, statement_for(pattern, "_h"));
    const std::string looped = answer(ex, statement_for(pattern, "_n"));
    // (the message, without the time it took)
    auto message = [](const std::string& text) { return text.substr(0, text.find('\n')); };
    REQUIRE(message(hashed) == message(looped));
    for (const auto& table_order : order_by_for_tables) {
        REQUIRE(q(ex, "SELECT * FROM " + std::string("t_h") + " ORDER BY " + table_order) == q(ex, "SELECT * FROM " + std::string("t_n") + " ORDER BY " + table_order));
    }
}
} // namespace

TEST_CASE("R68: a MERGE hashed on its ON gives the answer of the loop, for text keys (case, NULL, duplicates), number keys and an ON with more than the equality", "[r68][merge]") {
    TempDataDir dir("r68_merge");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    // text keys: 'apple' twice in the target (the first is the one that matches), 'Apple' and 'apple' twice in the source (the last update wins), a NULL on both sides, '007' against '7'
    make_pair_of_tables(ex, "id INT PRIMARY KEY, k VARCHAR(10), v INT", "(1,'apple',10),(2,'Banana',20),(3,'apple',30),(4,NULL,40),(5,'cherry',50),(6,'007',60)",
                        "sid INT PRIMARY KEY, k VARCHAR(10), v INT", "(101,'Apple',1),(102,'banana',2),(103,'apple',3),(104,NULL,4),(105,'durian',5),(106,'7',6),(107,'CHERRY',7)");
    same_answer_and_tables(ex, "MERGE INTO t@ USING s@ ON<< t@.k = s@.k >> WHEN MATCHED THEN UPDATE SET v = s@.v + 100 WHEN NOT MATCHED THEN INSERT (id, k, v) VALUES (s@.sid, s@.k, s@.v)", {"id"});
    // a matched row deleted, another updated; the ON holds a condition besides the equality (it is checked for each pair the hash lets through)
    same_answer_and_tables(ex, "MERGE INTO t@ USING s@ ON<< t@.k = s@.k AND t@.v < s@.v + 200 >> WHEN MATCHED AND s@.v > 4 THEN DELETE WHEN MATCHED THEN UPDATE SET v = 0 - s@.v", {"id"});
    REQUIRE(q(ex, "SELECT COUNT(*) FROM t_h") == q(ex, "SELECT COUNT(*) FROM t_n"));
}

TEST_CASE("R68: a MERGE hashed on number keys: 7 and 7.00 are one key, a NULL is none, and every pair is still checked", "[r68][merge]") {
    TempDataDir dir("r68_merge_numbers");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    make_pair_of_tables(ex, "id INT PRIMARY KEY, k DECIMAL(6,2), v INT", "(1,7.00,10),(2,8.50,20),(3,7.00,30),(4,NULL,40),(5,9.25,50),(6,-1.50,60)",
                        "sid INT PRIMARY KEY, k INT, v INT", "(101,7,1),(102,8,2),(103,9,3),(104,NULL,4),(105,-1,5),(106,7,6)");
    same_answer_and_tables(ex, "MERGE INTO t@ USING s@ ON<< t@.k = s@.k >> WHEN MATCHED THEN UPDATE SET v = s@.v + 100 WHEN NOT MATCHED THEN INSERT (id, k, v) VALUES (s@.sid, s@.k, s@.v)", {"id"});
    // a target whose key is an INT and a source whose key is a DECIMAL
    drop_pair_of_tables(ex);
    make_pair_of_tables(ex, "id INT PRIMARY KEY, k INT, v INT", "(1,7,10),(2,8,20),(3,9,30)", "sid INT PRIMARY KEY, k DECIMAL(4,1), v INT", "(101,7.0,1),(102,8.5,2),(103,9.0,3)");
    same_answer_and_tables(ex, "MERGE INTO t@ USING s@ ON<< t@.k = s@.k >> WHEN MATCHED THEN UPDATE SET v = s@.v + 100 WHEN NOT MATCHED THEN INSERT (id, k, v) VALUES (s@.sid, s@.k, s@.v)", {"id"});
}

TEST_CASE("R68: the multi-table UPDATE and DELETE hashed on their ON give the answer of the loop for JOIN, LEFT JOIN and RIGHT JOIN", "[r68][multi]") {
    TempDataDir dir("r68_multi");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    const char* target = "(1,'apple',10),(2,'Banana',20),(3,'apple',30),(4,NULL,40),(5,'cherry',50),(6,'fig',60)";
    const char* source = "(101,'Apple',1),(102,'banana',2),(103,'apple',3),(104,NULL,4),(105,'durian',5),(106,'FIG',6)";
    // each statement runs on fresh copies of the tables
    const std::vector<std::string> statements = {
        "UPDATE t@ JOIN s@ ON<< t@.k = s@.k >> SET t@.v = s@.v + 100",
        "UPDATE t@ LEFT JOIN s@ ON<< t@.k = s@.k >> SET t@.v = 0 - 1 WHERE s@.sid IS NULL",
        "UPDATE t@ RIGHT JOIN s@ ON<< t@.k = s@.k >> SET t@.v = s@.v + 200",
        "UPDATE t@ JOIN s@ ON<< t@.k = s@.k AND s@.v > 2 >> SET t@.v = 0",
        "DELETE t@ FROM t@ JOIN s@ ON<< t@.k = s@.k >>",
        "DELETE t@ FROM t@ LEFT JOIN s@ ON<< t@.k = s@.k >> WHERE s@.sid IS NULL",
        "DELETE t@ FROM t@ RIGHT JOIN s@ ON<< t@.k = s@.k >> WHERE t@.id IS NOT NULL",
        "DELETE t@ FROM t@ JOIN s@ ON<< t@.k = s@.k AND t@.v > 25 >>",
    };
    for (const std::string& pattern : statements) {
        make_pair_of_tables(ex, "id INT PRIMARY KEY, k VARCHAR(10), v INT", target, "sid INT PRIMARY KEY, k VARCHAR(10), v INT", source);
        same_answer_and_tables(ex, pattern, {"id"});
        drop_pair_of_tables(ex);
    }
    // a text key against a number key compares by number ('abc' is 0, '5x' is 5, '007' is 7): no hash of the text answers that, and the loop does it
    for (const std::string& pattern : {std::string("UPDATE t@ JOIN s@ ON<< t@.k = s@.k >> SET t@.v = s@.v + 100"),
                                       std::string("MERGE INTO t@ USING s@ ON<< t@.k = s@.k >> WHEN MATCHED THEN UPDATE SET v = s@.v + 100")}) {
        make_pair_of_tables(ex, "id INT PRIMARY KEY, k VARCHAR(10), v INT", "(1,'abc',10),(2,'5x',20),(3,'7',30),(4,'007',40),(5,'zzz',50)", "sid INT PRIMARY KEY, k INT, v INT",
                            "(101,0,1),(102,5,2),(103,7,3),(104,9,4)");
        same_answer_and_tables(ex, pattern, {"id"});
        drop_pair_of_tables(ex);
    }
}

TEST_CASE("R68: a MERGE, a multi-table UPDATE and a multi-table DELETE of 100 rows against 60,000 do not make a merged row for every pair", "[r68][merge][multi]") {
    TempDataDir dir("r68_speed");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE dst (id INT PRIMARY KEY, v INT)");
    std::string rows = "INSERT INTO dst VALUES ";
    for (int i = 1; i <= 100; i++) rows += std::string(i == 1 ? "" : ",") + "(" + std::to_string(i) + ",0)";
    ok(ex, rows);
    ok(ex, "CREATE TABLE src (id INT PRIMARY KEY, v INT)");
    for (int start = 1; start <= 60000; start += 1000) {
        std::string batch = "INSERT INTO src VALUES ";
        for (int i = start; i < start + 1000; i++) batch += std::string(i == start ? "" : ",") + "(" + std::to_string(i) + "," + std::to_string(i % 7) + ")";
        ok(ex, batch);
    }
    auto millis = [](const std::function<void()>& work) {
        const auto started = std::chrono::steady_clock::now();
        work();
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    };
    // (the loop over every pair is 6,000,000 pairs: about six seconds)
    const long long merge_ms = millis([&] { ok(ex, "MERGE INTO dst USING src ON dst.id = src.id WHEN MATCHED THEN UPDATE SET v = src.v + 1"); });
    const long long update_ms = millis([&] { ok(ex, "UPDATE dst JOIN src ON dst.id = src.id SET dst.v = dst.v + 10"); });
    const long long delete_ms = millis([&] { ok(ex, "DELETE dst FROM dst JOIN src ON dst.id = src.id WHERE src.v = 3"); });
    std::fprintf(stderr, "[r68] MERGE %lld ms, multi-table UPDATE %lld ms, multi-table DELETE %lld ms (100 x 60,000 rows)\n", merge_ms, update_ms, delete_ms);
    REQUIRE(merge_ms < 2000);
    REQUIRE(update_ms < 2000);
    REQUIRE(delete_ms < 2000);
    // dst.v = (id % 7) + 1 + 10 for the rows that are left; the ids with id % 7 == 3 are gone
    REQUIRE(q(ex, "SELECT COUNT(*) FROM dst") == Rows{{"86"}}); // (100 rows, 14 of them with id % 7 == 3)
    REQUIRE(q(ex, "SELECT v FROM dst WHERE id = 1") == Rows{{"12"}});
    REQUIRE(q(ex, "SELECT v FROM dst WHERE id = 7") == Rows{{"11"}});
}

// An equality is a key only when it is AND-ed into the ON: of `a = b OR c = d` neither is, and a pair that meets only the second must still be met.
TEST_CASE("R68: an ON of two equalities joined by OR is not hashed on one of them", "[r68][merge][multi]") {
    TempDataDir dir("r68_or");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, k VARCHAR(5), k2 VARCHAR(5), v INT)");
    ok(ex, "INSERT INTO t VALUES (1,'a','p',0),(2,'b','q',0),(3,'c','r',0)");
    ok(ex, "CREATE TABLE s (sid INT PRIMARY KEY, k VARCHAR(5), k2 VARCHAR(5))");
    ok(ex, "INSERT INTO s VALUES (101,'a','z'),(102,'x','q')");
    // t1 meets s101 by k, t2 meets s102 by k2, t3 meets nothing
    ok(ex, "MERGE INTO t USING s ON (t.k = s.k OR t.k2 = s.k2) WHEN MATCHED THEN UPDATE SET v = 1");
    REQUIRE(q(ex, "SELECT id, v FROM t ORDER BY id") == (Rows{{"1", "1"}, {"2", "1"}, {"3", "0"}}));
    ok(ex, "UPDATE t SET v = 0");
    ok(ex, "UPDATE t JOIN s ON (t.k = s.k OR t.k2 = s.k2) SET t.v = 2");
    REQUIRE(q(ex, "SELECT id, v FROM t ORDER BY id") == (Rows{{"1", "2"}, {"2", "2"}, {"3", "0"}}));
    ok(ex, "DELETE t FROM t JOIN s ON (t.k = s.k OR t.k2 = s.k2)");
    REQUIRE(q(ex, "SELECT id FROM t ORDER BY id") == Rows{{"3"}});
}
