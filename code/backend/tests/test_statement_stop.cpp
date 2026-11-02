#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#endif

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/join.hpp"
#include "engine/parallel_util.hpp"
#include "engine/result_text.hpp"
#include "engine/statement_deadline.hpp"
#include "engine/storage/numeric_key.hpp"

using namespace engine;
namespace fs = std::filesystem;

// A statement that is stopped (KILL QUERY) and a join that does not make every pair of rows before it looks at the WHERE (R63).

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

Rows q(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
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

std::string session_of(Executor& ex, const std::string& user) {
    auto rows = q(ex, "SELECT ID FROM information_schema.processlist WHERE USER = '" + user + "'");
    REQUIRE(rows.size() == 1);
    return rows[0][0];
}

void wait_for_command(Executor& ex, const std::string& user, const std::string& command) {
    for (int i = 0; i < 300; i++) {
        if (q(ex, "SELECT ID FROM information_schema.processlist WHERE USER = '" + user + "' AND COMMAND = '" + command + "'").size() == 1) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    FAIL("the session of " << user << " never showed the command " << command);
}

// The memory this process holds (bytes); 0 where it cannot be read. A statement's own use is the most it holds while it runs, less what it held before.
std::size_t working_set() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return counters.WorkingSetSize;
#endif
    return 0;
}

struct MemoryWatch {
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> top{0};
    std::size_t before = working_set();
    std::thread watcher{[this] {
        while (!stop.load()) {
            top = std::max(top.load(), working_set());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }};
    // megabytes more than the process held when the watch began
    std::size_t grown_mb() {
        stop = true;
        watcher.join();
        const std::size_t t = top.load();
        return t > before ? (t - before) / (1024 * 1024) : 0;
    }
};

// An environment variable that holds a value until it goes out of scope (RUSTDB_PARALLEL is read at every statement)
struct EnvVar {
    std::string name;
    EnvVar(const std::string& n, const std::string& value) : name(n) {
#ifdef _WIN32
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
    }
    ~EnvVar() {
#ifdef _WIN32
        _putenv_s(name.c_str(), "");
#else
        unsetenv(name.c_str());
#endif
    }
};

void fill(Executor& ex, const std::string& table, int rows) {
    ok(ex, "CREATE TABLE " + table + " (id INT PRIMARY KEY, v INT)");
    for (int start = 1; start <= rows; start += 500) {
        std::string sql = "INSERT INTO " + table + " VALUES ";
        for (int i = start; i < std::min(start + 500, rows + 1); i++) sql += std::string(i == start ? "" : ",") + "(" + std::to_string(i) + "," + std::to_string(i % 10) + ")";
        ok(ex, sql);
    }
}

// A statement that the session `victim` (user bob) runs on a thread of its own and `ex` stops with KILL QUERY `delay_ms` after it began: what it answered, and how long it
// went on after the KILL. (It does not look at the process list first: a statement that holds the database exclusively -- a CREATE -- keeps the list from being read.)
struct Stopped {
    bool failed = false;
    std::string answer;
    std::chrono::milliseconds after_kill{0};
};

Stopped run_and_kill(Executor& ex, Executor& victim, const std::string& victim_id, const std::string& sql, int delay_ms = 150) {
    Stopped out;
    std::thread runner([&] {
        auto r = victim.execute_sql(sql);
        out.failed = r.is_err();
        out.answer = out.failed ? r.error() : r.value();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    const auto killed_at = std::chrono::steady_clock::now();
    REQUIRE(ex.execute_sql("KILL QUERY " + victim_id).is_ok());
    runner.join();
    out.after_kill = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - killed_at);
    std::fprintf(stderr, "[r67] %s: stopped %lld ms after KILL\n", sql.substr(0, 60).c_str(), static_cast<long long>(out.after_kill.count()));
    return out;
}
} // namespace

TEST_CASE("R63: KILL QUERY stops a cross join of three tables, and the join does not hold every pair of rows", "[r63][kill][join]") {
    TempDataDir dir("r63_cross_join_kill");
    Executor ex(dir.path);
    ex.register_process("root", "localhost");
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    fill(ex, "a", 300);
    fill(ex, "b", 300);
    fill(ex, "c", 300);
    auto shared = ex.get_shared();
    Executor victim = Executor::new_session(shared);
    victim.auth_user = "bob";
    victim.register_process("bob", "localhost");
    ok(victim, "USE d");
    const std::string victim_id = session_of(ex, "bob");

    std::string answer;
    bool failed = false;
    MemoryWatch watch;
    std::thread runner([&] {
        auto r = victim.execute_sql("SELECT COUNT(*) FROM a x, b y, c z WHERE x.id + y.id + z.id = 7");
        failed = r.is_err();
        answer = failed ? r.error() : r.value();
    });
    wait_for_command(ex, "bob", "Query");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const auto killed_at = std::chrono::steady_clock::now();
    REQUIRE(ex.execute_sql("KILL QUERY " + victim_id).is_ok());
    runner.join();
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - killed_at).count();
    const std::size_t grown = watch.grown_mb();
    INFO("stopped " << waited << " ms after KILL, the process grew " << grown << " MB, answer: " << answer.substr(0, 120));
    std::fprintf(stderr, "[r63] cross join: stopped %lld ms after KILL, grew %zu MB\n", static_cast<long long>(waited), grown);
    REQUIRE(failed);
    REQUIRE(answer.find("ERROR 1317 (70100): Query execution was interrupted") != std::string::npos);
    REQUIRE(waited < 3000);
}

TEST_CASE("R63: a join does not hold every pair of rows before the WHERE, and keeps the rows the WHERE wants", "[r63][join]") {
    TempDataDir dir("r63_join_where");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    fill(ex, "a", 600);
    fill(ex, "b", 600);
    MemoryWatch watch;
    REQUIRE(q(ex, "SELECT COUNT(*) FROM a x, b y WHERE x.id + y.id = 7") == Rows{{"6"}});
    const std::size_t grown = watch.grown_mb();
    std::fprintf(stderr, "[r63] 600 x 600 cross join with a WHERE: the process grew %zu MB\n", grown);
    if (working_set() != 0) REQUIRE(grown < 80); // (the 360,000 merged rows held first took about 640 MB)
}

TEST_CASE("R63: the last join of a query makes the same rows as joining first and filtering after", "[r63][join]") {
    TempDataDir dir("r63_join_same");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE t1 (id INT PRIMARY KEY, g INT, name VARCHAR(10))");
    ok(ex, "CREATE TABLE t2 (id INT PRIMARY KEY, g INT, name VARCHAR(10))");
    ok(ex, "CREATE TABLE t3 (id INT PRIMARY KEY, g INT, name VARCHAR(10))");
    ok(ex, "INSERT INTO t1 VALUES (1, 1, 'a'), (2, 1, 'b'), (3, 2, 'c'), (4, NULL, 'd'), (5, 3, NULL)");
    ok(ex, "INSERT INTO t2 VALUES (1, 1, 'x'), (2, 2, 'y'), (3, 2, NULL), (4, 5, 'w')");
    ok(ex, "INSERT INTO t3 VALUES (1, 1, 'p'), (2, 3, 'q'), (3, NULL, 'r')");
    // each pair: the query as it is written (the last join keeps the rows of the WHERE) and the same rows through a derived table (the join, then the WHERE)
    struct Pair { const char* written; const char* reference; };
    const Pair pairs[] = {
        {"SELECT a.id, b.id, c.id FROM t1 a, t2 b, t3 c WHERE a.id + b.id + c.id = 7 ORDER BY a.id, b.id, c.id",
         "SELECT i, j, k FROM (SELECT a.id i, b.id j, c.id k FROM t1 a, t2 b, t3 c) d WHERE i + j + k = 7 ORDER BY i, j, k"},
        {"SELECT a.id, b.id FROM t1 a, t2 b WHERE a.g = b.g OR a.id = 5 ORDER BY a.id, b.id",
         "SELECT i, j FROM (SELECT a.id i, b.id j, a.g ag, b.g bg FROM t1 a, t2 b) d WHERE ag = bg OR i = 5 ORDER BY i, j"},
        {"SELECT a.id, b.id FROM t1 a JOIN t2 b ON a.g < b.g WHERE a.id <> 2 ORDER BY a.id, b.id",
         "SELECT i, j FROM (SELECT a.id i, b.id j FROM t1 a JOIN t2 b ON a.g < b.g) d WHERE i <> 2 ORDER BY i, j"},
        {"SELECT a.id, b.id, c.id FROM t1 a LEFT JOIN t2 b ON a.g = b.g JOIN t3 c ON c.id <= a.id WHERE b.id IS NULL OR b.id > 1 ORDER BY a.id, b.id, c.id",
         "SELECT i, j, k FROM (SELECT a.id i, b.id j, c.id k FROM t1 a LEFT JOIN t2 b ON a.g = b.g JOIN t3 c ON c.id <= a.id) d WHERE j IS NULL OR j > 1 ORDER BY i, j, k"},
        {"SELECT a.id, b.id FROM t1 a, t2 b WHERE a.id + b.id IN (SELECT id + 3 FROM t3) ORDER BY a.id, b.id",
         "SELECT i, j FROM (SELECT a.id i, b.id j FROM t1 a, t2 b) d WHERE i + j IN (SELECT id + 3 FROM t3) ORDER BY i, j"},
        {"SELECT COUNT(*), SUM(a.id * b.id) FROM t1 a, t2 b WHERE a.id > b.id",
         "SELECT COUNT(*), SUM(i * j) FROM (SELECT a.id i, b.id j FROM t1 a, t2 b) d WHERE i > j"},
        {"SELECT a.g, COUNT(*) FROM t1 a, t2 b WHERE a.id + b.id > 4 GROUP BY a.g ORDER BY a.g",
         "SELECT g, COUNT(*) FROM (SELECT a.id i, a.g g, b.id j FROM t1 a, t2 b) d WHERE i + j > 4 GROUP BY g ORDER BY g"},
        {"SELECT a.id, b.id FROM t1 a, t2 b WHERE a.id + b.id < 0",
         "SELECT i, j FROM (SELECT a.id i, b.id j FROM t1 a, t2 b) d WHERE i + j < 0"},
        {"SELECT a.id, b.id FROM t1 a, t2 b WHERE a.name = 'a' AND b.name IS NOT NULL ORDER BY b.id",
         "SELECT i, j FROM (SELECT a.id i, a.name an, b.id j, b.name bn FROM t1 a, t2 b) d WHERE an = 'a' AND bn IS NOT NULL ORDER BY j"},
        {"SELECT a.id FROM t1 a, t2 b, t3 c WHERE a.id = b.id AND b.id + c.id = 3 ORDER BY a.id",
         "SELECT i FROM (SELECT a.id i, b.id j, c.id k FROM t1 a, t2 b, t3 c) d WHERE i = j AND j + k = 3 ORDER BY i"},
    };
    for (const Pair& p : pairs) {
        INFO(p.written);
        REQUIRE(q(ex, p.written) == q(ex, p.reference));
    }
    // and it is not nothing: the first of them has rows
    REQUIRE_FALSE(q(ex, pairs[0].written).empty());
}

// The stages that are one piece of work (a sort, the groups, DISTINCT, a hash join) ask the clock and the KILL flag as they go: with a limit of a third of the time the whole query takes the
// statement ends with 3024 in the middle of the stage (before 60% of the whole time), not at its end. (MAX_EXECUTION_TIME and KILL QUERY use the same places.) R64 added the
// stages that were left: the sort of a window function, the sort of a UNION (in slices on the pool's threads, or on the statement's own thread), the scan in chunks, and reading a long answer back.
TEST_CASE("R63: a sort, a GROUP BY, a DISTINCT, a hash join, a window sort, a UNION sort and a parallel scan are stopped in the middle of their work", "[r63][r64][kill]") {
    TempDataDir dir("r63_stage_stop");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE big (id INT PRIMARY KEY, g INT, v INT, s VARCHAR(20))");
    for (int start = 1; start <= 300000; start += 1000) {
        std::string sql = "INSERT INTO big VALUES ";
        for (int i = start; i < start + 1000; i++) sql += std::string(i == start ? "" : ",") + "(" + std::to_string(i) + "," + std::to_string(i % 1000) + "," + std::to_string((i * 7919) % 10007) + ",'row" + std::to_string(i) + "')";
        ok(ex, sql);
    }
    ok(ex, "CREATE TABLE little (id INT PRIMARY KEY, g INT)");
    ok(ex, "INSERT INTO little VALUES (1, 1), (2, 2), (3, 3)");
    const char* queries[] = {
        "SELECT * FROM big ORDER BY s DESC, v",
        "SELECT g, v, COUNT(*) FROM big GROUP BY g, v",
        "SELECT DISTINCT s, v FROM big",
        "SELECT COUNT(*) FROM big a JOIN little b ON a.g = b.g",
        "SELECT id, ROW_NUMBER() OVER (ORDER BY s DESC, v) FROM big",
        "SELECT id, v, s FROM big UNION ALL SELECT id, g, 'x' FROM little ORDER BY s DESC, v",
        "SELECT COUNT(*) FROM big WHERE UPPER(CONCAT(UPPER(s), LOWER(s), REVERSE(s), UPPER(CONCAT(s, s)), REVERSE(CONCAT(s, 'ab')))) LIKE '%ZZ%' OR v * g > 99999999",
        "SELECT id, v, s FROM big UNION ALL SELECT id, g, 'x' FROM little",
    };
    // (the limit is a part of the whole time, and the statement must have stopped before the other part: a third and 60% unless the stage is in the second half)
    auto stops_in_the_middle = [&](const char* sql, long long limit_per_mille = 333, long long bound_per_mille = 600) {
        INFO(sql);
        auto millis = [](std::chrono::steady_clock::time_point since) { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count(); };
        // (a trivial UPDATE empties the cache of answers: an answer that is cached takes no time the second time)
        ok(ex, "SET max_execution_time = 0");
        ok(ex, "UPDATE big SET v = v WHERE id = 1");
        auto started = std::chrono::steady_clock::now();
        auto whole = ex.execute_sql(sql);
        const auto whole_ms = millis(started);
        REQUIRE(whole.is_ok());
        // a third of the time the whole query takes: the rows are read by then (that stage ends before the clock is looked at), the stage that follows is what is stopped
        const auto limit = std::max<long long>(30, whole_ms * limit_per_mille / 1000);
        ok(ex, "UPDATE big SET v = v WHERE id = 1");
        ok(ex, "SET max_execution_time = " + std::to_string(limit));
        started = std::chrono::steady_clock::now();
        auto stopped = ex.execute_sql(sql);
        const auto stopped_ms = millis(started);
        ok(ex, "SET max_execution_time = 0");
        std::fprintf(stderr, "[r63] %s -> the whole of it takes %lld ms, with a limit of %lld ms it stopped in %lld ms\n", sql, static_cast<long long>(whole_ms), static_cast<long long>(limit), static_cast<long long>(stopped_ms));
        REQUIRE(stopped.is_err());
        REQUIRE(stopped.error().find("3024") != std::string::npos);
        REQUIRE(stopped_ms * 1000 < whole_ms * bound_per_mille); // (stopped in the middle of the stage, not at its end: a stage that is not asked ends it first)
    };
    for (const char* sql : queries) stops_in_the_middle(sql);
    // R67: with six aggregates the rows of the groups are a good part of the time; the workers that make them ask as they go, so the statement stops at the limit (a third of the time
    // the whole of it takes, 40% here) and not at the end of that stage (63% when they do not ask): the bound is half
    stops_in_the_middle("SELECT g, v, COUNT(*), SUM(id), AVG(v), MAX(s), MIN(s), COUNT(DISTINCT s) FROM big GROUP BY g, v", 333, 500);
    // The sort of a UNION is made in slices on the pool's threads from some thousand rows up; below that (here at every size: RUSTDB_PARALLEL=0) it is one sort on the statement's thread
    EnvVar sequential("RUSTDB_PARALLEL", "0");
    // (the reading of the rows and of the answer come first, a good part of the time: the limit goes to where the sort is)
    stops_in_the_middle("SELECT id, v, s FROM big UNION ALL SELECT id, g, 'x' FROM little ORDER BY s DESC, v", 700, 880);
    // R67: the rows of the groups are made one group after the other on the statement's thread when RUSTDB_PARALLEL=0 (on the pool's threads, in chunks, otherwise: the GROUP BY
    // above), and that stage asks as well
    stops_in_the_middle("SELECT g, v, COUNT(*) FROM big GROUP BY g, v");
    stops_in_the_middle("SELECT g, v, COUNT(*), SUM(id), AVG(v), MAX(s), MIN(s), COUNT(DISTINCT s) FROM big GROUP BY g, v");
}

// An UPDATE or DELETE that is still looking for the rows it will change has written nothing: KILL QUERY stops it there, and the table is as it was. (Once rows are being written the
// statement runs on to its end -- there is no way back from half of it.) The same for the SELECT of an INSERT ... SELECT (R64): the statement's first word is INSERT, and the
// SELECT it runs only reads.
TEST_CASE("R63: KILL QUERY stops an UPDATE, a DELETE and an INSERT ... SELECT while they look for their rows, and nothing is changed", "[r63][r64][kill]") {
    TempDataDir dir("r63_write_kill");
    Executor ex(dir.path);
    ex.register_process("root", "localhost");
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE big (id INT PRIMARY KEY, g INT, v INT)");
    for (int start = 1; start <= 40000; start += 1000) {
        std::string sql = "INSERT INTO big VALUES ";
        for (int i = start; i < start + 1000; i++) sql += std::string(i == start ? "" : ",") + "(" + std::to_string(i) + "," + std::to_string(i % 100) + "," + std::to_string(i % 7) + ")";
        ok(ex, sql);
    }
    ok(ex, "CREATE TABLE other (id INT PRIMARY KEY, g INT, v INT)"); // (MySQL does not let a statement read the table it writes in a subquery: error 1093)
    ok(ex, "INSERT INTO other SELECT id, g, v FROM big");
    ok(ex, "CREATE TABLE sink (id INT PRIMARY KEY, g INT, v INT)");
    auto shared = ex.get_shared();
    Executor victim = Executor::new_session(shared);
    victim.auth_user = "bob";
    victim.register_process("bob", "localhost");
    ok(victim, "USE d");
    const std::string victim_id = session_of(ex, "bob");
    const Rows before = q(ex, "SELECT COUNT(*), SUM(v), SUM(g) FROM big");

    // (a correlated subquery for every row: it would run for minutes)
    for (const char* sql : {"UPDATE big SET v = v + 100 WHERE EXISTS (SELECT 1 FROM other b WHERE b.v = big.v AND b.g = big.g + 1000)",
                            "DELETE FROM big WHERE EXISTS (SELECT 1 FROM other b WHERE b.v = big.v AND b.g = big.g + 1000)",
                            "INSERT INTO sink SELECT id, g, v FROM big WHERE EXISTS (SELECT 1 FROM other b WHERE b.v = big.v AND b.g = big.g + 1000)"}) {
        INFO(sql);
        std::string answer;
        bool failed = false;
        std::thread runner([&] {
            auto r = victim.execute_sql(sql);
            failed = r.is_err();
            answer = failed ? r.error() : r.value();
        });
        wait_for_command(ex, "bob", "Query");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const auto killed_at = std::chrono::steady_clock::now();
        REQUIRE(ex.execute_sql("KILL QUERY " + victim_id).is_ok());
        runner.join();
        REQUIRE(std::chrono::steady_clock::now() - killed_at < std::chrono::seconds(5));
        INFO("answer: " << answer.substr(0, 200));
        REQUIRE(failed);
        REQUIRE(answer.find("ERROR 1317 (70100): Query execution was interrupted") != std::string::npos);
        REQUIRE(q(ex, "SELECT COUNT(*), SUM(v), SUM(g) FROM big") == before);
        REQUIRE(q(ex, "SELECT COUNT(*) FROM sink") == Rows{{"0"}});
        REQUIRE(q(victim, "SELECT COUNT(*) FROM big") == Rows{{"40000"}}); // (the session is whole)
    }
}

// The workers of a parallel join probe have no clock and no KILL flag of their own: the statement's thread hands them a Watch. With a limit of one millisecond the probe of 300,000 rows
// stops, and the statement's thread throws.
TEST_CASE("R63: the workers of a parallel hash join stop when the statement is stopped", "[r63][kill][join]") {
    auto row = [](const std::string& k, const std::string& id) {
        Row r;
        r["k"] = k;
        r["id"] = id;
        return r;
    };
    std::vector<Row> left;
    left.reserve(300000);
    for (int i = 0; i < 300000; i++) left.push_back(row(std::to_string(i % 3), std::to_string(i)));
    std::vector<Row> right;
    for (int k = 0; k < 3; k++) {
        Row r;
        r["k"] = std::to_string(k);
        r["name"] = "n" + std::to_string(k);
        right.push_back(r);
    }
    {
        deadline::Scope limit(1);
        REQUIRE_THROWS_AS(hash_join(left, right, JoinType::Inner, "r", "k", "k", {"k", "name"}), StatementError);
    }
    // (with no limit the same join makes its 300,000 rows)
    REQUIRE(hash_join(left, right, JoinType::Inner, "r", "k", "k", {"k", "name"}).size() == 300000);
}

// R64: the same for the slices of a parallel sort (the answer of a UNION, the groups of a GROUP BY): the slices ask on the pool's threads, the statement's thread throws.
TEST_CASE("R64: the slices of a parallel sort stop when the statement is stopped, and sort when it is not", "[r64][kill]") {
    std::vector<Row> rows;
    rows.reserve(150000);
    for (int i = 0; i < 150000; i++) {
        Row r;
        r["k"] = std::to_string((i * 7919) % 100003);
        rows.push_back(std::move(r));
    }
    auto less = [](const Row& a, const Row& b) { return std::stoi(a.at("k")) < std::stoi(b.at("k")); };
    std::vector<Row> stopped = rows;
    {
        deadline::Scope limit(1);
        REQUIRE_THROWS_AS(parallel_sort(stopped, less), StatementError);
    }
    parallel_sort(rows, less);
    REQUIRE(rows.size() == 150000);
    REQUIRE(std::is_sorted(rows.begin(), rows.end(), less));
}

// The merge of a UNION (the rows kept so far are looked through for every row), an INTERSECT, an EXCEPT and the frames of a window function are slow when the answers are long, and
// they ask the clock as they go: with a limit of a third of the whole time the statement ends in the middle of the stage.
TEST_CASE("R64: the merge of a UNION, an INTERSECT and an EXCEPT and the frames of a window function are stopped in the middle of their work", "[r64][kill]") {
    TempDataDir dir("r64_merge_stop");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    fill(ex, "u", 4000);
    const char* queries[] = {
        "SELECT id, v FROM u UNION SELECT id + 4000, v FROM u",
        "SELECT id, v FROM u INTERSECT SELECT id, v FROM u",
        "SELECT id, v FROM u EXCEPT SELECT id + 1, v FROM u",
        "SELECT id, SUM(v) OVER (ORDER BY id ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) FROM u",
    };
    for (const char* sql : queries) {
        INFO(sql);
        auto millis = [](std::chrono::steady_clock::time_point since) { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count(); };
        ok(ex, "SET max_execution_time = 0");
        ok(ex, "UPDATE u SET v = v WHERE id = 1");
        auto started = std::chrono::steady_clock::now();
        auto whole = ex.execute_sql(sql);
        const auto whole_ms = millis(started);
        REQUIRE(whole.is_ok());
        const auto limit = std::max<long long>(30, whole_ms / 3);
        ok(ex, "UPDATE u SET v = v WHERE id = 1");
        ok(ex, "SET max_execution_time = " + std::to_string(limit));
        started = std::chrono::steady_clock::now();
        auto stopped = ex.execute_sql(sql);
        const auto stopped_ms = millis(started);
        ok(ex, "SET max_execution_time = 0");
        std::fprintf(stderr, "[r64] %s -> the whole of it takes %lld ms, with a limit of %lld ms it stopped in %lld ms\n", sql, static_cast<long long>(whole_ms), static_cast<long long>(limit), static_cast<long long>(stopped_ms));
        REQUIRE(whole_ms >= 90); // (a query that is too quick to be stopped in the middle says nothing: make the table longer)
        REQUIRE(stopped.is_err());
        REQUIRE(stopped.error().find("3024") != std::string::npos);
        REQUIRE(stopped_ms * 10 < whole_ms * 6);
    }
}

// KILL QUERY on a CALL: the SELECT of its body is a query like any other, and a loop of the body is left between two statements. What the statements before it did stays (MySQL does
// the same); the CALL answers with the error and the session goes on.
TEST_CASE("R64: KILL QUERY stops a CALL in the SELECT of its body and in a loop of it", "[r64][kill]") {
    TempDataDir dir("r64_call_kill");
    Executor ex(dir.path);
    ex.register_process("root", "localhost");
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    fill(ex, "a", 200);
    fill(ex, "b", 200);
    fill(ex, "c", 200);
    ok(ex, "CREATE TABLE trail (id INT PRIMARY KEY)");
    ok(ex, "CREATE PROCEDURE slow_select() BEGIN INSERT INTO trail VALUES (1); SELECT COUNT(*) FROM a x, b y, c z WHERE x.id + y.id + z.id = 7; INSERT INTO trail VALUES (2); END");
    ok(ex, "CREATE PROCEDURE spin() BEGIN DECLARE i INT DEFAULT 0; DECLARE j INT DEFAULT 0; INSERT INTO trail VALUES (3); "
           "WHILE i < 2000 DO SET j = 0; WHILE j < 2000 DO SET j = j + 1; END WHILE; SET i = i + 1; END WHILE; INSERT INTO trail VALUES (4); END");
    auto shared = ex.get_shared();
    Executor victim = Executor::new_session(shared);
    victim.auth_user = "bob";
    victim.register_process("bob", "localhost");
    ok(victim, "USE d");
    const std::string victim_id = session_of(ex, "bob");
    for (const char* call : {"CALL slow_select()", "CALL spin()"}) {
        INFO(call);
        std::string answer;
        bool failed = false;
        std::thread runner([&] {
            auto r = victim.execute_sql(call);
            failed = r.is_err();
            answer = failed ? r.error() : r.value();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto killed_at = std::chrono::steady_clock::now();
        REQUIRE(ex.execute_sql("KILL QUERY " + victim_id).is_ok());
        runner.join();
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - killed_at).count();
        std::fprintf(stderr, "[r64] %s: stopped %lld ms after KILL\n", call, static_cast<long long>(waited));
        REQUIRE(failed);
        REQUIRE(answer.find("ERROR 1317 (70100): Query execution was interrupted") != std::string::npos);
        REQUIRE(waited < 3000);
        REQUIRE(q(victim, "SELECT COUNT(*) FROM a") == Rows{{"200"}}); // (the session is whole)
    }
    // what the statements before the stop did stays; what came after it never ran
    REQUIRE(q(ex, "SELECT id FROM trail ORDER BY id") == (Rows{{"1"}, {"3"}}));
}

// R67: statements whose first word is not SELECT but that only read, or read before they write, were not stopped by KILL QUERY: EXPLAIN ANALYZE, the EXECUTE of a prepared
// SELECT, the SELECT of a CREATE TABLE ... AS SELECT, MERGE and the multi-table UPDATE / DELETE (until the first row is written) and the build of a CREATE INDEX (before the index is put
// in the database). Each of them ran to its end after the KILL.
namespace {
// a second session (bob) on the database of `ex` (root), the one that a test stops
struct Victim {
    Executor session;
    std::string id;
    explicit Victim(Executor& ex) : session(Executor::new_session(ex.get_shared())) {
        ex.register_process("root", "localhost");
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        session.auth_user = "bob";
        session.register_process("bob", "localhost");
        ok(session, "USE d");
        id = session_of(ex, "bob");
    }
};

void require_stopped(const Stopped& stopped, std::chrono::milliseconds within = std::chrono::milliseconds(1500)) {
    INFO("answer: " << stopped.answer.substr(0, 200));
    REQUIRE(stopped.failed);
    REQUIRE(stopped.answer.find("ERROR 1317 (70100): Query execution was interrupted") != std::string::npos);
    REQUIRE(stopped.after_kill < within);
}
} // namespace

TEST_CASE("R67: KILL QUERY stops an EXPLAIN ANALYZE, an EXECUTE and a CREATE TABLE ... AS SELECT while the query runs, and no table is made", "[r67][kill]") {
    TempDataDir dir("r67_query_kill");
    Executor ex(dir.path);
    Victim bob(ex);
    ok(ex, "CREATE TABLE big (id INT PRIMARY KEY, g INT, v INT)");
    for (int start = 1; start <= 30000; start += 1000) {
        std::string sql = "INSERT INTO big VALUES ";
        for (int i = start; i < start + 1000; i++) sql += std::string(i == start ? "" : ",") + "(" + std::to_string(i) + "," + std::to_string(i % 100) + "," + std::to_string(i % 40) + ")";
        ok(ex, sql);
    }
    ok(ex, "CREATE TABLE other (id INT PRIMARY KEY, g INT, v INT)");
    ok(ex, "INSERT INTO other SELECT id, g, v FROM big");
    // (a correlated subquery for every row, none of which finds a row: tens of millions of comparisons)
    const std::string slow = "SELECT id FROM big WHERE EXISTS (SELECT 1 FROM other b WHERE b.v = big.v AND b.g = big.g + 1000)";
    ok(bob.session, "PREPARE slow_q FROM '" + slow + "'");
    for (const std::string& sql : {"EXPLAIN ANALYZE " + slow, std::string("EXECUTE slow_q"), "CREATE TABLE made AS " + slow}) {
        INFO(sql);
        require_stopped(run_and_kill(ex, bob.session, bob.id, sql));
    }
    for (const auto& row : q(ex, "SHOW TABLES")) REQUIRE(row[0] != "made");
    REQUIRE(q(bob.session, "SELECT COUNT(*) FROM big") == Rows{{"30000"}}); // (the session is whole)
}

TEST_CASE("R67: KILL QUERY stops a MERGE, a multi-table UPDATE and a multi-table DELETE while they look for their rows, and nothing is changed", "[r67][kill]") {
    TempDataDir dir("r67_multi_kill");
    Executor ex(dir.path);
    Victim bob(ex);
    ok(ex, "CREATE TABLE dst (id INT PRIMARY KEY, v INT)");
    std::string sql = "INSERT INTO dst VALUES ";
    for (int i = 1; i <= 300; i++) sql += std::string(i == 1 ? "" : ",") + "(" + std::to_string(i) + ",0)";
    ok(ex, sql);
    ok(ex, "CREATE TABLE src (id INT PRIMARY KEY, v INT)");
    for (int start = 1; start <= 20000; start += 1000) {
        std::string batch = "INSERT INTO src VALUES ";
        for (int i = start; i < start + 1000; i++) batch += std::string(i == start ? "" : ",") + "(" + std::to_string(i) + "," + std::to_string(i % 7) + ")";
        ok(ex, batch);
    }
    const Rows before = q(ex, "SELECT COUNT(*), SUM(v) FROM dst");
    // (a nested loop over 300 x 20,000 rows: seconds. The ON is one a hash cannot answer -- `dst.id + 0` is no column --, or the hash join of R68 would be done before the KILL)
    // (the DELETE has a loop of its own for each kind of join it can be written with: JOIN, LEFT JOIN, RIGHT JOIN)
    for (const char* statement : {"MERGE INTO dst USING src ON dst.id + 0 = src.id WHEN MATCHED THEN UPDATE SET v = src.v + 1 WHEN NOT MATCHED THEN INSERT VALUES (src.id, src.v)",
                                  "UPDATE dst JOIN src ON dst.id + 0 = src.id SET dst.v = src.v + 1", "DELETE dst FROM dst JOIN src ON dst.id + 0 = src.id",
                                  "DELETE dst FROM dst LEFT JOIN src ON dst.id + 0 = src.id", "DELETE dst FROM dst RIGHT JOIN src ON dst.id + 0 = src.id"}) {
        INFO(statement);
        require_stopped(run_and_kill(ex, bob.session, bob.id, statement));
        REQUIRE(q(ex, "SELECT COUNT(*), SUM(v) FROM dst") == before);
    }
    REQUIRE(q(bob.session, "SELECT COUNT(*) FROM src") == Rows{{"20000"}}); // (the session is whole)
}

TEST_CASE("R67: KILL QUERY stops a CREATE INDEX while it builds, with nothing changed, and an index that is not stopped finds the same rows", "[r67][kill]") {
    TempDataDir dir("r67_index_kill");
    Executor ex(dir.path);
    Victim bob(ex);
    ok(ex, "CREATE TABLE big (id INT PRIMARY KEY, g INT, v INT, s VARCHAR(20))");
    for (int start = 1; start <= 300000; start += 1000) {
        std::string sql = "INSERT INTO big VALUES ";
        for (int i = start; i < start + 1000; i++) sql += std::string(i == start ? "" : ",") + "(" + std::to_string(i) + "," + std::to_string(i % 1000) + "," + std::to_string((i * 7919) % 10007) + ",'row" + std::to_string(i) + "')";
        ok(ex, sql);
    }
    const Rows by_s = q(ex, "SELECT COUNT(*) FROM big WHERE s = 'row777'");
    const Rows by_gv = q(ex, "SELECT COUNT(*) FROM big WHERE g = 5 AND v = 7");
    const Rows by_v = q(ex, "SELECT COUNT(*) FROM big WHERE v = 7");
    REQUIRE(by_s == Rows{{"1"}});
    for (const char* create : {"CREATE INDEX i_s ON big (s)", "CREATE INDEX i_gv ON big (g, v)", "CREATE INDEX i_v ON big (v) USING HASH"}) {
        INFO(create);
        const std::string name = std::string(create).substr(13, std::string(create).find(' ', 13) - 13);
        const bool listed = std::string(create).find("USING HASH") == std::string::npos; // (SHOW INDEX does not list a hash index)
        require_stopped(run_and_kill(ex, bob.session, bob.id, create, 40), std::chrono::milliseconds(700)); // (a build that is not stopped goes on for a second or more)
        if (listed) REQUIRE(ex.execute_sql("SHOW INDEX FROM big").value().find(name) == std::string::npos);
        ok(ex, create); // (nothing was made: the same statement, not stopped, makes the index -- it would say "Duplicate key name" if half of one were there)
        if (listed) REQUIRE(ex.execute_sql("SHOW INDEX FROM big").value().find(name) != std::string::npos);
        REQUIRE(q(ex, "SELECT COUNT(*) FROM big WHERE s = 'row777'") == by_s);
        REQUIRE(q(ex, "SELECT COUNT(*) FROM big WHERE g = 5 AND v = 7") == by_gv);
        REQUIRE(q(ex, "SELECT COUNT(*) FROM big WHERE v = 7") == by_v);
    }
}

// An index that CREATE INDEX builds apart from the database may be stopped while it is filled; the indexes that the server rebuilds for the database itself (at its start, after a
// recovery) are filled the same way and must not be: stopped half way, one would stay in the database half made.
TEST_CASE("R67: the filling of an index asks KILL QUERY only when it is built apart from the database", "[r67][kill]") {
    BPlusTree tree;
    IndexBuckets bucket(tree.kinds());
    for (int i = 1; i <= 5; i++) {
        Row row;
        row["id"] = std::to_string(i);
        bucket.add(row["id"], row);
    }
    std::atomic<int> told_to_stop{1};
    deadline::KillScope asked(&told_to_stop, /*reads=*/true); // (a statement that reads, and has been told to stop)
    REQUIRE_THROWS_AS(bucket.fill(tree, /*stoppable=*/true), StatementError);
    BPlusTree whole;
    REQUIRE_NOTHROW(bucket.fill(whole)); // (the default: not stopped)
    REQUIRE(whole.search("3").has_value());
}
