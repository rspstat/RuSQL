#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// R94 -- OPEN ISSUE, found by the KILL fuzzer (code/test/crash/fuzz_kill.py), whose killer thread reads information_schema.processlist every few milliseconds while four sessions
// write: an UPDATE now and then answered "Internal error: invalid unordered_map<K, T> key" (about once in 100 rounds on a Release build). The call stack of the throw was
// CascadeSim::CascadeSim (executor_cascade_plan.cpp), which walks the shared catalog's table map twice, the second time with .at(). The map had lost an entry in between: a SELECT
// of an information_schema table puts the rows it has now into a temporary table of the SHARED database (s.tables, s.catalog) and takes it away again, and table_lock_set_for lets
// that SELECT in on the per-table path, which holds the structural lock only SHARED -- so it changes the maps under every session that is reading them (a data race; the in-process
// test below, with two sessions that poll without pause, even ends in a segmentation fault).
//
// Routing such a SELECT to the exclusive path (what is_pure_read_only() says it is) removes the race but is not acceptable: every poll of information_schema.processlist (the app's Activity
// panel, the MCP list_locks / kill_session) then waits for any long statement to end -- measured 0.00 s -> 3.13 s behind a 3.5 s SELECT -- and the R53 / R63 / R67 KILL tests,
// which read the list while a statement runs, fail. The fix has to stop changing the shared maps (a private place for the rows of a virtual table); see docs/mds/DATE.md R94.
//
// So the reproducer is HIDDEN (run it by name: engine_tests "[r94-open]"; expect a crash or "Internal error" until the issue is closed), and the control test next to it -- the same
// writers and readers over an ordinary table -- stays in the suite; it passed before and after, which is what shows that the test program itself is sound.

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

TEST_CASE("R94 (open issue): information_schema SELECTs run while other sessions write, and neither fails", "[.][r94-open][concurrency][info_schema]") {
    writers_and_readers({"SELECT COUNT(*) FROM information_schema.processlist", "SELECT COUNT(*) FROM information_schema.tables", "SELECT COUNT(*) FROM information_schema.columns"});
}

// The same test with SELECTs of an ordinary table: it passed before the fix too, which is what shows that the test program itself is sound.
TEST_CASE("R94: control -- SELECTs of an ordinary table run while other sessions write", "[r94][concurrency]") {
    writers_and_readers({"SELECT COUNT(*) FROM c", "SELECT SUM(v) FROM c WHERE id < 100", "SELECT MAX(v) FROM c"});
}
