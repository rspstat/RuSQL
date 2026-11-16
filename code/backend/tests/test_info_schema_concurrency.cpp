#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/result_text.hpp"

using namespace engine;
namespace fs = std::filesystem;

// R94 found it, R95 closed it. The KILL fuzzer (code/test/crash/fuzz_kill.py) has a killer thread that reads information_schema.processlist every few milliseconds while four sessions
// write: an UPDATE now and then answered "Internal error: invalid unordered_map<K, T> key" (about once in 100 rounds on a Release build). The call stack of the throw was
// CascadeSim::CascadeSim (executor_cascade_plan.cpp), which walks the shared catalog's table map twice, the second time with .at(). The map had lost an entry in between: a SELECT
// of an information_schema table put the rows it had into a temporary table of the SHARED database (s.tables, s.catalog) and took it away again, and table_lock_set_for let that SELECT
// in on the per-table path, which holds the structural lock only SHARED -- so it changed the maps under every session that was reading them (a data race; the in-process test
// below, with two sessions that poll without pause, even ended in a segmentation fault, 3 runs of 3).
//
// Sending such a SELECT to the exclusive path (what is_pure_read_only() says it is) removes the race but is not acceptable: every poll of information_schema.processlist (the app's
// Activity panel, the MCP list_locks / kill_session) then waits for any long statement to end -- measured 0.00 s -> 3.13 s behind a 3.5 s SELECT -- and the R53 / R63 / R67 KILL tests,
// which read the list while a statement runs, fail. R95's answer (engine/info_schema_scope.hpp): a SELECT of information_schema tables alone, with no subquery, runs on a PRIVATE database
// and changes nothing of the shared maps; one that is mixed with a real table or a subquery goes to the exclusive path. The test below is the reproducer, no longer hidden; the control
// next to it -- the same writers and readers over an ordinary table -- shows the test program itself is sound.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};
} // namespace

// Three sessions UPDATE their own rows of table c while two sessions keep running `reads` (one statement each round, in turn); no statement may fail, every UPDATE has to land.
static void writers_and_readers(const std::vector<std::string>& reads) {
    TempDataDir dir("r94_info_schema_concurrency");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE c (id INT PRIMARY KEY, v INT)").is_ok());
    {
        std::string sql = "INSERT INTO c VALUES ";
        for (int i = 1; i <= 200; i++) sql += std::string(i == 1 ? "" : ",") + "(" + std::to_string(i) + ",0)";
        REQUIRE(ex.execute_sql(sql).is_ok());
    }
    auto shared = ex.get_shared();

    constexpr int kWriters = 3, kUpdatesEach = 1500;
    std::atomic<bool> stop{false};
    std::atomic<int> failures{0};
    std::atomic<long> reads_done{0};
    std::mutex note_mutex;
    std::string first_failure;
    auto note = [&](const std::string& what) {
        failures++;
        std::lock_guard<std::mutex> guard(note_mutex);
        if (first_failure.empty()) first_failure = what;
    };

    std::vector<std::thread> writers, readers;
    for (int w = 0; w < kWriters; w++) {
        writers.emplace_back([&, w] {
            Executor session = Executor::new_session(shared);
            session.register_process("root", "localhost");
            session.execute_sql("USE d");
            for (int i = 0; i < kUpdatesEach; i++) {
                const int key = w * 60 + 1 + (i % 60); // (a range of its own: no row is waited for)
                auto r = session.execute_sql("UPDATE c SET v = v + 1 WHERE id = " + std::to_string(key));
                if (r.is_err()) note("UPDATE: " + r.error().substr(0, 200));
            }
        });
    }
    for (int t = 0; t < 2; t++) {
        readers.emplace_back([&, t] {
            Executor session = Executor::new_session(shared);
            session.register_process("root", "localhost");
            session.execute_sql("USE d");
            for (std::size_t i = 0; !stop.load(); i++) {
                const std::string& sql = reads[(i + static_cast<std::size_t>(t)) % reads.size()];
                auto r = session.execute_sql(sql);
                if (r.is_err()) note(sql + ": " + r.error().substr(0, 200));
                reads_done++;
            }
        });
    }
    for (auto& w : writers) w.join();
    stop = true;
    for (auto& r : readers) r.join();

    INFO("the first failure: " << first_failure);
    REQUIRE(failures.load() == 0);
    REQUIRE(reads_done.load() > 0);
    auto sum = ex.execute_sql("SELECT SUM(v) FROM c");
    REQUIRE(sum.is_ok());
    REQUIRE(sum.value().find(std::to_string(kWriters * kUpdatesEach)) != std::string::npos); // (every update landed)
}

TEST_CASE("R95: information_schema SELECTs run while other sessions write, and neither fails", "[r95][r94][concurrency][info_schema]") {
    writers_and_readers({"SELECT COUNT(*) FROM information_schema.processlist", "SELECT COUNT(*) FROM information_schema.tables", "SELECT COUNT(*) FROM information_schema.columns"});
}

// The same test with SELECTs of an ordinary table: it passed before the fix too, which is what shows that the test program itself is sound.
TEST_CASE("R94: control -- SELECTs of an ordinary table run while other sessions write", "[r94][concurrency]") {
    writers_and_readers({"SELECT COUNT(*) FROM c", "SELECT SUM(v) FROM c WHERE id < 100", "SELECT MAX(v) FROM c"});
}

namespace {
using Rows = std::vector<std::vector<std::string>>;

Rows rows_of(Executor& ex, const std::string& sql) {
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

void must(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
}

// The names of the temporary tables of information_schema SELECTs that sit in the shared database (there must be none: they are made in a private one)
std::vector<std::string> virtual_tables_in_shared(const std::shared_ptr<RwLock<SharedDatabase>>& shared) {
    std::vector<std::string> found;
    auto database = shared->read();
    for (const auto& [name, rows] : database->tables) {
        if (name.rfind("information_schema.", 0) == 0) found.push_back(name);
    }
    for (const auto& [name, schema] : database->catalog.tables) {
        if (name.rfind("information_schema.", 0) == 0) found.push_back(name);
    }
    return found;
}
} // namespace

// R95: the three shapes of a SELECT that names an information_schema table. Information_schema tables alone (a join of them included) run on the private database; one next to a real
// table, and one with a subquery either way round, make their temporary tables in the shared maps under the exclusive lock. All answer as they always did, and none leaves a table behind.
TEST_CASE("R95: SELECTs of information_schema tables alone, mixed with a table and with a subquery answer, and leave nothing in the shared database", "[r95][info_schema]") {
    TempDataDir dir("r95_info_schema_shapes");
    Executor ex(dir.path);
    ex.register_process("root", "localhost");
    must(ex, "CREATE DATABASE d");
    must(ex, "USE d");
    must(ex, "CREATE TABLE a (id INT PRIMARY KEY, name VARCHAR(20))");
    must(ex, "CREATE TABLE b (id INT PRIMARY KEY, a_id INT, note VARCHAR(20))");
    must(ex, "INSERT INTO a VALUES (1, 'x'), (3, 'name')");
    auto shared = ex.get_shared();

    // information_schema tables alone: one, a join of two, aggregates over the join
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM information_schema.tables WHERE TABLE_SCHEMA = 'd'") == Rows{{"2"}});
    REQUIRE(rows_of(ex, "SELECT t.TABLE_NAME, COUNT(*) FROM information_schema.tables t JOIN information_schema.columns c ON c.TABLE_SCHEMA = t.TABLE_SCHEMA AND c.TABLE_NAME = t.TABLE_NAME "
                        "WHERE t.TABLE_SCHEMA = 'd' GROUP BY t.TABLE_NAME ORDER BY t.TABLE_NAME") == Rows{{"a", "2"}, {"b", "3"}});
    REQUIRE(rows_of(ex, "SELECT COLUMN_NAME FROM information_schema.columns WHERE TABLE_SCHEMA = 'd' AND TABLE_NAME = 'b' ORDER BY ORDINAL_POSITION DESC LIMIT 2") == Rows{{"note"}, {"a_id"}});
    REQUIRE(virtual_tables_in_shared(shared).empty());

    // next to a real table
    REQUIRE(rows_of(ex, "SELECT a.id, c.COLUMN_NAME FROM a JOIN information_schema.columns c ON c.COLUMN_NAME = a.name WHERE c.TABLE_SCHEMA = 'd' AND c.TABLE_NAME = 'a'") == Rows{{"3", "name"}});
    REQUIRE(virtual_tables_in_shared(shared).empty());

    // a subquery: the real table inside, the real table outside
    REQUIRE(rows_of(ex, "SELECT COLUMN_NAME FROM information_schema.columns WHERE TABLE_SCHEMA = 'd' AND TABLE_NAME = 'a' AND COLUMN_NAME IN (SELECT name FROM a) ORDER BY COLUMN_NAME") == Rows{{"name"}});
    REQUIRE(rows_of(ex, "SELECT id FROM a WHERE name IN (SELECT COLUMN_NAME FROM information_schema.columns WHERE TABLE_SCHEMA = 'd') ORDER BY id") == Rows{{"3"}});
    REQUIRE(rows_of(ex, "SELECT id, (SELECT COUNT(*) FROM information_schema.tables WHERE TABLE_SCHEMA = 'd') FROM a ORDER BY id") == Rows{{"1", "2"}, {"3", "2"}});
    REQUIRE(virtual_tables_in_shared(shared).empty());

    // a statement that fails (a column the table does not have) leaves nothing either, and the next one answers
    REQUIRE(ex.execute_sql("SELECT no_such_column FROM information_schema.tables").is_err());
    REQUIRE(ex.execute_sql("SELECT x.no_such FROM information_schema.tables x JOIN information_schema.columns y ON x.TABLE_NAME = y.TABLE_NAME").is_err());
    REQUIRE(virtual_tables_in_shared(shared).empty());
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM information_schema.tables WHERE TABLE_SCHEMA = 'd'") == Rows{{"2"}});
}

// R95: the private database sees the world of the session: its transaction, its snapshot, the table it has just created. A SERIALIZABLE transaction that reads information_schema registers
// no predicate that fails its COMMIT when another session writes.
TEST_CASE("R95: an information_schema SELECT inside a transaction, at every isolation level", "[r95][info_schema][rr]") {
    for (const char* level : {"READ COMMITTED", "REPEATABLE READ", "SERIALIZABLE"}) {
        INFO(level);
        TempDataDir dir("r95_info_schema_txn");
        Executor a(dir.path);
        a.register_process("root", "localhost");
        must(a, "CREATE DATABASE d");
        must(a, "USE d");
        must(a, "CREATE TABLE t (id INT PRIMARY KEY, v INT)");
        must(a, "INSERT INTO t VALUES (1, 10), (2, 20)");
        auto shared = a.get_shared();
        Executor b = Executor::new_session(shared);
        b.register_process("root", "localhost");
        must(b, "USE d");
        must(a, std::string("SET TRANSACTION ISOLATION LEVEL ") + level);
        must(a, "BEGIN");
        REQUIRE(rows_of(a, "SELECT COUNT(*) FROM information_schema.tables WHERE TABLE_SCHEMA = 'd'") == Rows{{"1"}});
        must(b, "INSERT INTO t VALUES (3, 30)"); // (committed by another session while a's transaction is open)
        must(b, "CREATE TABLE u (id INT PRIMARY KEY)");
        REQUIRE(rows_of(a, "SELECT COUNT(*) FROM information_schema.tables WHERE TABLE_SCHEMA = 'd'") == Rows{{"2"}}); // (the list of tables is the current one)
        REQUIRE(rows_of(a, "SELECT COUNT(*) FROM information_schema.processlist") == Rows{{"2"}});
        must(a, "COMMIT");
        REQUIRE(virtual_tables_in_shared(shared).empty());
    }
}
