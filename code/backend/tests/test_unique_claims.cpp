#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "catch.hpp"
#include "engine/collation.hpp"
#include "engine/executor/executor.hpp"
#include "engine/lock_manager.hpp"

using namespace engine;
namespace fs = std::filesystem;

// Two sessions that write the same unique value at the same moment must not both get in. The duplicate check of an INSERT / UPDATE reads the table; the row of another
// session that is about to be written is not there yet, so the check alone cannot see it: a claim on the value (the lock manager, like the one on a primary key)
// makes the second one wait for the first and then check again. These tests are for every shape of unique key: a UNIQUE column, a UNIQUE (a, b) key, a composite
// PRIMARY KEY, a table without a primary key.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

using Rows = std::vector<std::vector<std::string>>;

Rows table_cells(const std::string& output) {
    Rows rows;
    std::istringstream in(output);
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars <= 1) continue;
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
        rows.push_back(std::move(row));
    }
    return rows;
}

void ok(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
}

Rows rows_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
    return table_cells(r.value());
}
} // namespace

TEST_CASE("sessions racing to write one unique value leave one row for it, whatever the shape of the key", "[unique_claim][concurrency][random]") {
    TempDataDir dir("unique_claims_race");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE u (id INT PRIMARY KEY AUTO_INCREMENT, email VARCHAR(20) UNIQUE, who INT)");
    ok(ex, "CREATE TABLE c (id INT PRIMARY KEY AUTO_INCREMENT, a VARCHAR(5), b VARCHAR(5), UNIQUE (a, b))");
    ok(ex, "CREATE TABLE p (a VARCHAR(5), b VARCHAR(5), who INT, PRIMARY KEY (a, b))");
    ok(ex, "CREATE TABLE n (v VARCHAR(20) UNIQUE, who INT)");
    ok(ex, "CREATE TABLE w (id INT PRIMARY KEY, v VARCHAR(20) UNIQUE)");
    ok(ex, "CREATE TABLE x (id INT PRIMARY KEY, a VARCHAR(5), b VARCHAR(5), UNIQUE (a, b))");
    std::string wv, xv;
    for (int i = 0; i < 12; i++) {
        wv += std::string(i ? ", " : "") + "(" + std::to_string(i) + ", 'v" + std::to_string(i) + "')";
        xv += std::string(i ? ", " : "") + "(" + std::to_string(i) + ", 'a" + std::to_string(i) + "', 'b')";
    }
    ok(ex, "INSERT INTO w VALUES " + wv);
    ok(ex, "INSERT INTO x VALUES " + xv);
    auto shared = ex.get_shared();

    unsigned seed_count = 3; // RUSQL_FUZZ_SEEDS=40 makes it a long campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    // Every step all the sessions write the SAME new value at the same moment (a barrier lines them up), then one session looks at every table for a value held twice.
    constexpr int kThreads = 8, kSteps = 100;
    std::atomic<int> arrived{0}, generation{0};
    auto barrier = [&] {
        const int gen = generation.load();
        if (arrived.fetch_add(1) + 1 == kThreads) {
            arrived = 0;
            generation++;
        } else {
            while (generation.load() == gen) std::this_thread::yield();
        }
    };
    std::atomic<int> violations{0};
    std::set<std::string> offenders; // the tables that held a value twice
    std::string details;
    std::mutex violation_mutex;
    const std::vector<std::pair<std::string, std::string>> keys = {{"u", "email"}, {"c", "a, b"}, {"p", "a, b"}, {"n", "v"}, {"w", "v"}, {"x", "a, b"}};
    for (unsigned round = 0; round < seed_count; round++) {
        for (const char* t : {"u", "c", "p", "n"}) ok(ex, std::string("DELETE FROM ") + t);
        ok(ex, "DELETE FROM w");
        ok(ex, "DELETE FROM x");
        ok(ex, "INSERT INTO w VALUES " + wv);
        ok(ex, "INSERT INTO x VALUES " + xv);
        std::vector<std::thread> threads;
        for (int w = 0; w < kThreads; w++) {
            threads.emplace_back([&, w] {
                Executor sess = Executor::new_session(shared);
                const bool opened = sess.execute_sql("USE d").is_ok();
                for (int i = 0; i < kSteps; i++) {
                    barrier();
                    if (opened) {
                        const std::string k = std::to_string(i);
                        (void)sess.execute_sql("INSERT INTO u (email, who) VALUES ('k" + k + "', " + std::to_string(w) + ")");
                        (void)sess.execute_sql("INSERT INTO c (a, b) VALUES ('a" + k + "', 'b')");
                        (void)sess.execute_sql("INSERT INTO p VALUES ('a" + k + "', 'b', " + std::to_string(w) + ")");
                        (void)sess.execute_sql("INSERT INTO n VALUES ('k" + k + "', " + std::to_string(w) + ")");
                        (void)sess.execute_sql("UPDATE w SET v = 'n" + k + "' WHERE id = " + std::to_string(w));
                        (void)sess.execute_sql("UPDATE x SET a = 'z" + k + "' WHERE id = " + std::to_string(w));
                    }
                    barrier();
                    if (opened && w == 0) {
                        for (auto& [table, cols] : keys) {
                            auto r = sess.execute_sql("SELECT " + cols + ", COUNT(*) FROM " + table + " GROUP BY " + cols + " HAVING COUNT(*) > 1");
                            if (r.is_ok() && !table_cells(r.value()).empty()) {
                                std::lock_guard<std::mutex> g(violation_mutex);
                                violations++;
                                if (offenders.insert(table).second) {
                                    auto all = sess.execute_sql("SELECT * FROM " + table + " ORDER BY 1, 2");
                                    details += table + " at step " + std::to_string(i) + ": " + (all.is_ok() ? all.value() : all.error()) + "\n";
                                }
                            }
                        }
                    }
                    barrier();
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    std::string names;
    for (auto& t : offenders) names += t + " ";
    INFO("tables with a value twice: " << names << "\n" << details);
    REQUIRE(violations.load() == 0);
}

// The race itself needs two sessions inside a window of microseconds. These tests hold the window open: a "session" that is in the middle of its statement is
// a claim in the lock manager (the same claim an INSERT takes), taken by hand.
namespace {
struct ClaimFixture {
    TempDataDir dir;
    Executor ex;
    std::shared_ptr<RwLock<SharedDatabase>> shared;
    LockManager& lm;
    explicit ClaimFixture(const std::string& path)
        : dir(path), ex(dir.path), shared((ok_open(ex), ex.get_shared())), lm(const_cast<LockManager&>(shared->read()->lock_mgr)) {}
    static void ok_open(Executor& e) {
        ok(e, "CREATE DATABASE d");
        ok(e, "USE d");
        ok(e, "CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT, email VARCHAR(20) UNIQUE, tag VARCHAR(10))");
        ok(e, "CREATE TABLE c (id INT PRIMARY KEY AUTO_INCREMENT, a VARCHAR(5), b VARCHAR(5), tag VARCHAR(10), UNIQUE (a, b))");
        ok(e, "CREATE TABLE p (a VARCHAR(5), b VARCHAR(5), tag VARCHAR(10), PRIMARY KEY (a, b))");
        ok(e, "CREATE TABLE n (v VARCHAR(20) UNIQUE, tag VARCHAR(10))");
    }
};

// The statement runs in a thread of its own; the claim is let go after `hold_ms`. Returns the answer and how long it took.
struct Timed {
    bool ok = false;
    std::string text;
    long long ms = 0;
};

Timed run_while_claimed(ClaimFixture& f, const std::string& sql, std::uint64_t holder, const std::function<void()>& meanwhile, int hold_ms = 250,
                        const std::string& before = "") {
    Timed out;
    std::thread b([&] {
        Executor sess = Executor::new_session(f.shared);
        REQUIRE(sess.execute_sql("USE d").is_ok());
        if (!before.empty()) REQUIRE(sess.execute_sql(before).is_ok());
        const auto t0 = std::chrono::steady_clock::now();
        auto r = sess.execute_sql(sql);
        out.ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        out.ok = r.is_ok();
        out.text = r.is_ok() ? r.value() : r.error();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    if (meanwhile) meanwhile();
    f.lm.release(holder);
    b.join();
    return out;
}
} // namespace

TEST_CASE("an INSERT of a unique value waits for the claim on it, whatever the shape of the key", "[unique_claim]") {
    ClaimFixture f("unique_claims_wait");
    constexpr std::uint64_t kHolder = 900001;
    struct Case {
        const char* claim_table;
        const char* claim_key;
        const char* sql;
    };
    const Case cases[] = {
        {"d.t#uk:email", "BOB", "INSERT INTO t (email) VALUES ('bob')"},       // a UNIQUE column (the lock is by the collation: 'BOB' = 'bob')
        {"d.c#ck:u0", "1:x1:y", "INSERT INTO c (a, b) VALUES ('X', 'y')"},      // a UNIQUE (a, b) key
        {"d.p#ck:pk", "1:x1:y", "INSERT INTO p VALUES ('x', 'Y', 'new')"},     // a composite PRIMARY KEY
        {"d.n#uk:v", "k1", "INSERT INTO n VALUES ('K1', 'new')"},               // a table without a primary key
    };
    for (auto& c : cases) {
        INFO(c.sql);
        REQUIRE(f.lm.acquire(c.claim_table, c.claim_key, kHolder).kind == LockResult::Kind::Granted);
        Timed t = run_while_claimed(f, c.sql, kHolder, nullptr);
        REQUIRE(t.ok);        // (nobody wrote the value: it goes in once the claim is gone)
        REQUIRE(t.ms >= 150); // ... but it waited for the claim first
    }
    // a value nobody claims does not wait
    Timed free_value = run_while_claimed(f, "INSERT INTO t (email) VALUES ('other')", kHolder, nullptr, 0);
    REQUIRE(free_value.ok);
    REQUIRE(free_value.ms < 150);
}

TEST_CASE("after the wait an INSERT looks for the value again, and does what its conflict clause says", "[unique_claim]") {
    ClaimFixture f("unique_claims_again");
    // "A" is a session in the middle of an INSERT: it has its transaction (an id), claimed 'bob' and has not written the row yet
    Executor a = Executor::new_session(f.shared);
    REQUIRE(a.execute_sql("USE d").is_ok());
    auto holder_of = [&](const std::string& table_suffix) {
        for (auto& [table, key, holder] : f.lm.lock_rows()) {
            if (table.size() >= table_suffix.size() && table.compare(table.size() - table_suffix.size(), table_suffix.size(), table_suffix) == 0) return holder;
        }
        return std::uint64_t{0};
    };
    const std::string modes[] = {"INSERT INTO t (email, tag) VALUES ('BOB', 'b')", "INSERT IGNORE INTO t (email, tag) VALUES ('BOB', 'b')",
                                 "INSERT INTO t (email, tag) VALUES ('BOB', 'b') ON DUPLICATE KEY UPDATE tag = 'updated'"};
    for (const std::string& sql : modes) {
        INFO(sql);
        ok(f.ex, "DELETE FROM t");
        REQUIRE(a.execute_sql("BEGIN").is_ok());
        REQUIRE(a.execute_sql("INSERT INTO t (email, tag) VALUES ('first', 'a')").is_ok());
        const std::uint64_t a_id = holder_of("#uk:email");
        REQUIRE(a_id != 0);
        REQUIRE(f.lm.acquire("d.t#uk:email", "bob", a_id).kind == LockResult::Kind::Granted);
        Timed t = run_while_claimed(f, sql, a_id, [&] {
            REQUIRE(a.execute_sql("INSERT INTO t (email, tag) VALUES ('bob', 'a')").is_ok()); // (the claim is its own: no wait)
            REQUIRE(a.execute_sql("COMMIT").is_ok());
        });
        REQUIRE(t.ms >= 150);
        if (sql.find("IGNORE") != std::string::npos) {
            REQUIRE(t.ok);
            REQUIRE(t.text.find("0 row(s) inserted") != std::string::npos);
            REQUIRE(rows_of(f.ex, "SELECT tag FROM t WHERE email = 'bob'") == Rows{{"a"}});
        } else if (sql.find("DUPLICATE") != std::string::npos) {
            REQUIRE(t.ok);
            REQUIRE(rows_of(f.ex, "SELECT tag FROM t WHERE email = 'bob'") == Rows{{"updated"}});
        } else {
            REQUIRE_FALSE(t.ok);
            REQUIRE(t.text.find("Duplicate value 'BOB' for column 'email'") != std::string::npos);
        }
        REQUIRE(rows_of(f.ex, "SELECT COUNT(*) FROM t WHERE email = 'bob'") == Rows{{"1"}});
    }
    // a key over several columns, and a composite primary key
    struct Case {
        const char* table;
        const char* claim_table;
        const char* claim_key;
        const char* a_first;
        const char* a_same;
        const char* b_sql;
        const char* message;
    };
    const Case cases[] = {
        {"c", "d.c#ck:u0", "1:x1:y", "INSERT INTO c (a, b) VALUES ('first', 'z')", "INSERT INTO c (a, b) VALUES ('x', 'y')", "INSERT INTO c (a, b) VALUES ('X', 'Y')",
         "Duplicate entry 'X-Y' for key 'c.a'"},
        {"p", "d.p#ck:pk", "1:x1:y", "INSERT INTO p VALUES ('first', 'z', 't')", "INSERT INTO p VALUES ('x', 'y', 't')", "INSERT INTO p VALUES ('X', 'Y', 'u')",
         "Duplicate entry 'X-Y' for key 'PRIMARY'"},
    };
    for (auto& c : cases) {
        INFO(c.b_sql);
        REQUIRE(a.execute_sql("BEGIN").is_ok());
        REQUIRE(a.execute_sql(c.a_first).is_ok());
        const std::uint64_t a_id = holder_of(std::string("#ck:") + (c.table[0] == 'c' ? "u0" : "pk"));
        REQUIRE(a_id != 0);
        REQUIRE(f.lm.acquire(c.claim_table, c.claim_key, a_id).kind == LockResult::Kind::Granted);
        Timed t = run_while_claimed(f, c.b_sql, a_id, [&] {
            REQUIRE(a.execute_sql(c.a_same).is_ok());
            REQUIRE(a.execute_sql("COMMIT").is_ok());
        });
        REQUIRE(t.ms >= 150);
        REQUIRE_FALSE(t.ok);
        REQUIRE(t.text.find(c.message) != std::string::npos);
        REQUIRE(rows_of(f.ex, std::string("SELECT COUNT(*) FROM ") + c.table + " WHERE a = 'x'") == Rows{{"1"}});
    }
}

TEST_CASE("an UPDATE that gives a row a unique value waits for the claim on it, and starts again", "[unique_claim]") {
    ClaimFixture f("unique_claims_update");
    ok(f.ex, "INSERT INTO t (email, tag) VALUES ('one', 'a'), ('two', 'b')");
    ok(f.ex, "INSERT INTO c (a, b) VALUES ('x', 'y'), ('x', 'z')");
    constexpr std::uint64_t kHolder = 900002;
    // nobody writes the value: the UPDATE waits and then does its work
    REQUIRE(f.lm.acquire("d.t#uk:email", "three", kHolder).kind == LockResult::Kind::Granted);
    Timed t = run_while_claimed(f, "UPDATE t SET email = 'THREE' WHERE id = 1", kHolder, nullptr);
    REQUIRE(t.ok);
    REQUIRE(t.ms >= 150);
    REQUIRE(rows_of(f.ex, "SELECT email FROM t WHERE id = 1") == Rows{{"THREE"}});
    // a key over several columns
    REQUIRE(f.lm.acquire("d.c#ck:u0", "1:x1:w", kHolder).kind == LockResult::Kind::Granted);
    t = run_while_claimed(f, "UPDATE c SET b = 'w' WHERE b = 'z'", kHolder, nullptr);
    REQUIRE(t.ok);
    REQUIRE(t.ms >= 150);
    // the primary key value of a row that changes it
    REQUIRE(f.lm.acquire("d.t", "50", kHolder).kind == LockResult::Kind::Granted);
    t = run_while_claimed(f, "UPDATE t SET id = 50 WHERE id = 2", kHolder, nullptr);
    REQUIRE(t.ok);
    REQUIRE(t.ms >= 150);
    // while it waits another session writes the value: the UPDATE finds it when it starts again
    Executor a = Executor::new_session(f.shared);
    REQUIRE(a.execute_sql("USE d").is_ok());
    REQUIRE(a.execute_sql("BEGIN").is_ok());
    REQUIRE(a.execute_sql("INSERT INTO t (email, tag) VALUES ('first', 'a')").is_ok());
    std::uint64_t a_id = 0;
    for (auto& [table, key, holder] : f.lm.lock_rows()) {
        if (table == "d.t#uk:email" && key == "first") a_id = holder;
    }
    REQUIRE(a_id != 0);
    REQUIRE(f.lm.acquire("d.t#uk:email", "four", a_id).kind == LockResult::Kind::Granted);
    t = run_while_claimed(f, "UPDATE t SET email = 'FOUR' WHERE id = 1", a_id, [&] {
        REQUIRE(a.execute_sql("INSERT INTO t (email, tag) VALUES ('four', 'a')").is_ok());
        REQUIRE(a.execute_sql("COMMIT").is_ok());
    });
    REQUIRE_FALSE(t.ok);
    REQUIRE(t.text.find("Duplicate value 'FOUR' for column 'email'") != std::string::npos);
    REQUIRE(rows_of(f.ex, "SELECT email FROM t WHERE id = 1") == Rows{{"THREE"}});
}

TEST_CASE("only a value that is new and not NULL is claimed, and two different keys never share a claim", "[unique_claim]") {
    ClaimFixture f("unique_claims_none");
    ok(f.ex, "INSERT INTO t (email, tag) VALUES ('one', 'a')");
    constexpr std::uint64_t kHolder = 900003;
    // NULL never repeats
    REQUIRE(f.lm.acquire("d.t#uk:email", "NULL", kHolder).kind == LockResult::Kind::Granted);
    Timed t = run_while_claimed(f, "INSERT INTO t (email, tag) VALUES (NULL, 'n')", kHolder, nullptr);
    REQUIRE(t.ok);
    REQUIRE(t.ms < 150);
    // an UPDATE that leaves the key as it is claims nothing
    REQUIRE(f.lm.acquire("d.t#uk:email", "one", kHolder).kind == LockResult::Kind::Granted);
    t = run_while_claimed(f, "UPDATE t SET tag = 'x' WHERE email = 'one'", kHolder, nullptr);
    REQUIRE(t.ok);
    REQUIRE(t.ms < 150);
    // the keys of a UNIQUE (a, b) are tuples that have been through the collation: 'x' with a Hangul syllable and 'x' with another one are two keys
    auto encoded = [](std::initializer_list<std::string> parts) {
        std::string out;
        for (auto& part : parts) {
            const std::string key = collation::key(part);
            out += std::to_string(key.size()) + ":" + key;
        }
        return out;
    };
    REQUIRE(f.lm.acquire("d.c#ck:u0", encoded({"\352\260\200", "x"}), kHolder).kind == LockResult::Kind::Granted);
    t = run_while_claimed(f, "INSERT INTO c (a, b) VALUES ('\353\202\230', 'x')", kHolder, nullptr);
    REQUIRE(t.ok);
    REQUIRE(t.ms < 150);
    REQUIRE(f.lm.acquire("d.c#ck:u0", encoded({"\352\260\200", "x"}), kHolder).kind == LockResult::Kind::Granted);
    t = run_while_claimed(f, "INSERT INTO c (a, b) VALUES ('\352\260\200', 'X')", kHolder, nullptr);
    REQUIRE(t.ok);
    REQUIRE(t.ms >= 150);
}

TEST_CASE("after any wait an INSERT looks again at the keys it claimed after its check: the PRIMARY KEY, and the others after a wait for a gap lock", "[unique_claim]") {
    ClaimFixture f("unique_claims_gap");
    Executor a = Executor::new_session(f.shared);
    REQUIRE(a.execute_sql("USE d").is_ok());
    // the value of the PRIMARY KEY is claimed by a session that has not written the row yet
    REQUIRE(a.execute_sql("BEGIN").is_ok());
    REQUIRE(a.execute_sql("INSERT INTO t (email, tag) VALUES ('first', 'a')").is_ok());
    std::uint64_t a_id = 0;
    for (auto& [table, key, holder] : f.lm.lock_rows()) {
        if (table == "d.t#uk:email") a_id = holder;
    }
    REQUIRE(a_id != 0);
    REQUIRE(f.lm.acquire("d.t", "50", a_id).kind == LockResult::Kind::Granted);
    Timed t = run_while_claimed(f, "INSERT INTO t (id, email, tag) VALUES (50, 'b', 'b')", a_id, [&] {
        REQUIRE(a.execute_sql("INSERT INTO t (id, email, tag) VALUES (50, 'a', 'a')").is_ok());
        REQUIRE(a.execute_sql("COMMIT").is_ok());
    });
    REQUIRE(t.ms >= 150);
    REQUIRE_FALSE(t.ok);
    REQUIRE(t.text.find("Duplicate value '50' for column 'id'") != std::string::npos);

    // A holds a gap lock over ids 100..200 (REPEATABLE READ) and writes into its own gap while B waits for it: when the lock goes, B's INSERT has to look for
    // its PRIMARY KEY value and its UNIQUE value again -- both claims are free by then, and no claim was in its way
    for (const char* b_sql : {"INSERT INTO t (id, email, tag) VALUES (150, 'b2', 'b')", "INSERT INTO t (id, email, tag) VALUES (160, 'a2', 'b')"}) {
        INFO(b_sql);
        ok(f.ex, "DELETE FROM t WHERE id >= 100");
        REQUIRE(a.execute_sql("SET ISOLATION LEVEL REPEATABLE READ").is_ok());
        REQUIRE(a.execute_sql("BEGIN").is_ok());
        REQUIRE(a.execute_sql("SELECT * FROM t WHERE id BETWEEN 100 AND 200 FOR UPDATE").is_ok());
        t = run_while_claimed(f, b_sql, 0, [&] {
            REQUIRE(a.execute_sql("INSERT INTO t (id, email, tag) VALUES (150, 'a2', 'a')").is_ok());
            REQUIRE(a.execute_sql("COMMIT").is_ok());
        });
        REQUIRE(t.ms >= 150);
        REQUIRE_FALSE(t.ok);
        REQUIRE(t.text.find("Duplicate value") != std::string::npos);
        REQUIRE(rows_of(f.ex, "SELECT COUNT(*) FROM t WHERE id >= 100").front().front() == "1");
    }
}

TEST_CASE("a NULL, a row that is gone and a claim that nobody lets go of are not duplicates", "[unique_claim]") {
    ClaimFixture f("unique_claims_not_dups");
    constexpr std::uint64_t kHolder = 900004;
    // a row whose UNIQUE column is NULL does not hold the key 'null' (a text)
    ok(f.ex, "INSERT INTO t (email, tag) VALUES (NULL, 'n')");
    REQUIRE(f.lm.acquire("d.t#uk:email", "null", kHolder).kind == LockResult::Kind::Granted);
    Timed t = run_while_claimed(f, "INSERT INTO t (email, tag) VALUES ('null', 'x')", kHolder, nullptr);
    REQUIRE(t.ok);
    REQUIRE(t.ms >= 150);
    // a row that has been deleted does not hold its value (it is still in the table for a while, ended). While the transaction that deleted it is open it does hold the claim
    // on the value, and an INSERT of the value waits for it: see test_uncommitted_unique.cpp
    ok(f.ex, "INSERT INTO t (email, tag) VALUES ('gone', 'g')");
    ok(f.ex, "DELETE FROM t WHERE email = 'gone'");
    REQUIRE(f.lm.acquire("d.t#uk:email", "gone", kHolder).kind == LockResult::Kind::Granted);
    t = run_while_claimed(f, "INSERT INTO t (email, tag) VALUES ('gone', 'again')", kHolder, nullptr);
    REQUIRE(t.ok);
    REQUIRE(t.ms >= 150);
}

TEST_CASE("a wait for a claim ends with the lock wait timeout, in an INSERT and in an UPDATE", "[unique_claim][blocking]") {
    ClaimFixture f("unique_claims_timeout");
    ok(f.ex, "INSERT INTO t (email, tag) VALUES ('one', 'a')");
    constexpr std::uint64_t kHolder = 900005;
    // (the claim is kept for a second, the timeout is 300 ms)
    for (const char* sql : {"INSERT INTO t (email, tag) VALUES ('bob', 'x')", "UPDATE t SET email = 'BOB' WHERE email = 'one'"}) {
        INFO(sql);
        REQUIRE(f.lm.acquire("d.t#uk:email", "bob", kHolder).kind == LockResult::Kind::Granted);
        Timed t = run_while_claimed(f, sql, kHolder, nullptr, 1000, "SET @lock_wait_timeout = 300");
        REQUIRE_FALSE(t.ok);
        REQUIRE(t.text.find("1205") != std::string::npos);
        REQUIRE(t.ms >= 250);
        REQUIRE(t.ms < 900);
    }
    REQUIRE(rows_of(f.ex, "SELECT email FROM t") == Rows{{"one"}});
}

// ---- foreign keys: a child that looks for its parent and then writes, a parent that looks for children and then goes

namespace {
struct FkFixture {
    TempDataDir dir;
    Executor ex;
    std::shared_ptr<RwLock<SharedDatabase>> shared;
    LockManager& lm;
    int next = 0;
    explicit FkFixture(const std::string& path)
        : dir(path), ex(dir.path), shared((open(ex), ex.get_shared())), lm(const_cast<LockManager&>(shared->read()->lock_mgr)) {}
    static void open(Executor& e) {
        ok(e, "CREATE DATABASE d");
        ok(e, "USE d");
        ok(e, "CREATE TABLE par (id INT PRIMARY KEY, name VARCHAR(10) UNIQUE)");
        ok(e, "CREATE TABLE kid (id INT PRIMARY KEY AUTO_INCREMENT, pid INT, FOREIGN KEY (pid) REFERENCES par (id))");
        ok(e, "CREATE TABLE cas (id INT PRIMARY KEY AUTO_INCREMENT, pid INT, FOREIGN KEY (pid) REFERENCES par (id) ON DELETE CASCADE ON UPDATE CASCADE)");
        std::string values;
        for (int i = 0; i < 200; i++) values += std::string(i ? ", " : "") + "(" + std::to_string(i) + ", 'p" + std::to_string(i) + "')";
        ok(e, "INSERT INTO par VALUES " + values);
        // a foreign key to a UNIQUE column that is not the primary key (it may be NULL), and a parent that two tables refer to by two of its columns
        ok(e, "CREATE TABLE par2 (id INT PRIMARY KEY, code VARCHAR(10) UNIQUE)");
        ok(e, "CREATE TABLE kid2 (id INT PRIMARY KEY AUTO_INCREMENT, c VARCHAR(10), FOREIGN KEY (c) REFERENCES par2 (code))");
        ok(e, "INSERT INTO par2 VALUES (1, 'a'), (2, NULL), (3, 'c')");
        ok(e, "CREATE TABLE par3 (id INT PRIMARY KEY, code VARCHAR(10) UNIQUE)");
        ok(e, "CREATE TABLE kid3a (id INT PRIMARY KEY AUTO_INCREMENT, pid INT, FOREIGN KEY (pid) REFERENCES par3 (id))");
        ok(e, "CREATE TABLE kid3b (id INT PRIMARY KEY AUTO_INCREMENT, c VARCHAR(10), FOREIGN KEY (c) REFERENCES par3 (code))");
        ok(e, "INSERT INTO par3 VALUES (1, 'a'), (2, 'b')");
    }
    // a session in the middle of a transaction: it has an id (the id its claims are made under)
    std::uint64_t transaction_id(Executor& a) {
        REQUIRE(a.execute_sql("USE d").is_ok());
        REQUIRE(a.execute_sql("BEGIN").is_ok());
        const std::string name = "x" + std::to_string(++next);
        REQUIRE(a.execute_sql("INSERT INTO par VALUES (" + std::to_string(1000 + next) + ", '" + name + "')").is_ok());
        std::uint64_t id = 0;
        for (auto& [table, key, holder] : lm.lock_rows()) {
            if (table == "d.par#uk:name" && key == name) id = holder;
        }
        REQUIRE(id != 0);
        return id;
    }
};
} // namespace

TEST_CASE("sessions racing a child row against its parent leave no child without a parent", "[unique_claim][fk_claim][concurrency][random]") {
    TempDataDir dir("fk_claims_race");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE par (id INT PRIMARY KEY, name VARCHAR(10))");
    ok(ex, "CREATE TABLE kid (id INT PRIMARY KEY AUTO_INCREMENT, pid INT, FOREIGN KEY (pid) REFERENCES par (id))");
    ok(ex, "CREATE TABLE mov (id INT PRIMARY KEY AUTO_INCREMENT, pid INT, FOREIGN KEY (pid) REFERENCES par (id))");
    ok(ex, "CREATE TABLE cas (id INT PRIMARY KEY AUTO_INCREMENT, pid INT, FOREIGN KEY (pid) REFERENCES par (id) ON DELETE CASCADE ON UPDATE CASCADE)");
    auto shared = ex.get_shared();
    unsigned seed_count = 3; // RUSQL_FUZZ_SEEDS=40 makes it a long campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    constexpr int kThreads = 5, kSteps = 80;
    std::atomic<int> arrived{0}, generation{0};
    auto barrier = [&] {
        const int gen = generation.load();
        if (arrived.fetch_add(1) + 1 == kThreads) {
            arrived = 0;
            generation++;
        } else {
            while (generation.load() == gen) std::this_thread::yield();
        }
    };
    std::atomic<int> orphans{0};
    // a watchdog: sessions that wait for each other for ever are a failure that says what each one was doing, not a test that never ends
    std::vector<std::string> doing(kThreads);
    std::mutex doing_mutex;
    std::atomic<bool> finished{false};
    std::thread watchdog([&] {
        int last = -1, still = 0;
        while (!finished.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            const int now = generation.load();
            still = now == last ? still + 1 : 0;
            last = now;
            if (still >= 60) { // 30 seconds without a step
                std::lock_guard<std::mutex> g(doing_mutex);
                std::string report = "no step for 30 s:";
                for (int w = 0; w < kThreads; w++) report += "\n  session " + std::to_string(w) + ": " + doing[static_cast<std::size_t>(w)];
                LockManager& lm = const_cast<LockManager&>(shared->read()->lock_mgr);
                report += "\n  locks held:";
                for (auto& [table, key, holder] : lm.lock_rows()) report += "\n    " + table + " [" + key + "] by " + std::to_string(holder);
                report += "\n  waiting:";
                for (auto& [waiter, holder] : lm.wait_for_rows()) report += "\n    " + std::to_string(waiter) + " waits for " + std::to_string(holder);
                std::fprintf(stderr, "%s\n", report.c_str());
                std::abort();
            }
        }
    });
    auto run_sql = [&](Executor& sess, int w, const std::string& sql) {
        {
            std::lock_guard<std::mutex> g(doing_mutex);
            doing[static_cast<std::size_t>(w)] = sql;
        }
        (void)sess.execute_sql(sql);
        std::lock_guard<std::mutex> g(doing_mutex);
        doing[static_cast<std::size_t>(w)] = "(between statements)";
    };
    for (unsigned round = 0; round < seed_count; round++) {
        ok(ex, "DELETE FROM kid");
        ok(ex, "DELETE FROM mov");
        ok(ex, "DELETE FROM cas");
        ok(ex, "DELETE FROM par");
        std::string values;
        for (int i = 0; i < 3 * kSteps; i++) values += std::string(i ? ", " : "") + "(" + std::to_string(i) + ", 'p" + std::to_string(i) + "')";
        ok(ex, "INSERT INTO par VALUES " + values);
        ok(ex, "INSERT INTO mov (pid) VALUES (0)"); // a child that moves between parents
        std::vector<std::thread> threads;
        for (int w = 0; w < kThreads; w++) {
            threads.emplace_back([&, w] {
                Executor sess = Executor::new_session(shared);
                const bool opened = sess.execute_sql("USE d").is_ok();
                for (int i = 0; i < kSteps; i++) {
                    barrier();
                    if (opened) {
                        const std::string a = std::to_string(i), b = std::to_string(kSteps + i), c = std::to_string(2 * kSteps + i);
                        if (w == 0) run_sql(sess, w, "DELETE FROM par WHERE id = " + a);                                  // the parent goes ...
                        if (w == 1) {
                            run_sql(sess, w, "INSERT INTO kid (pid) VALUES (" + a + ")");                                 // ... while a child is written for it
                            run_sql(sess, w, "INSERT INTO cas (pid) VALUES (" + b + ")");                                 // (one that goes with its parent, or must not be left)
                        }
                        if (w == 2) run_sql(sess, w, "INSERT INTO kid (pid) VALUES (" + b + ")");                         // a parent loses its id ...
                        if (w == 3) run_sql(sess, w, "UPDATE par SET id = " + std::to_string(1000 + i) + " WHERE id = " + b); // ... while a child is written for it
                        if (w == 4) {
                            run_sql(sess, w, "UPDATE mov SET pid = " + c);                                                // a child moves to a parent ...
                            run_sql(sess, w, "DELETE FROM par WHERE id = " + c);                                          // ... that goes
                        }
                    }
                    barrier();
                    if (opened && w == 0) {
                        for (const char* t : {"kid", "mov", "cas"}) {
                            auto r = sess.execute_sql(std::string("SELECT COUNT(*) FROM ") + t + " WHERE pid NOT IN (SELECT id FROM par)");
                            if (r.is_ok() && table_cells(r.value()) != Rows{{"0"}}) orphans++;
                        }
                    }
                    barrier();
                }
            });
        }
        for (auto& t : threads) t.join();
    }
    finished = true;
    watchdog.join();
    REQUIRE(orphans.load() == 0);
}

TEST_CASE("a child waits for a parent that is being deleted and then finds it gone; a parent waits for a child that is being written and then finds it", "[unique_claim][fk_claim]") {
    FkFixture f("fk_claims_wait");
    Executor a = Executor::new_session(f.shared);
    auto run = [&](const std::string& sql, std::uint64_t holder, const std::function<void()>& meanwhile, int hold_ms = 250, const std::string& before = "") {
        Timed out;
        std::thread b([&] {
            Executor sess = Executor::new_session(f.shared);
            REQUIRE(sess.execute_sql("USE d").is_ok());
            if (!before.empty()) REQUIRE(sess.execute_sql(before).is_ok());
            const auto t0 = std::chrono::steady_clock::now();
            auto r = sess.execute_sql(sql);
            out.ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
            out.ok = r.is_ok();
            out.text = r.is_ok() ? r.value() : r.error();
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
        if (meanwhile) meanwhile();
        f.lm.release(holder);
        b.join();
        return out;
    };

    // 1. A is deleting parent 5 (its claim is on the value, the row is still there): B's INSERT of a child of 5 waits, and when A commits there is no parent
    std::uint64_t a_id = f.transaction_id(a);
    REQUIRE(f.lm.acquire("d.par#fk:id", "5", a_id).kind == LockResult::Kind::Granted);
    Timed t = run("INSERT INTO kid (pid) VALUES (5)", a_id, [&] {
        REQUIRE(a.execute_sql("DELETE FROM par WHERE id = 5").is_ok());
        REQUIRE(a.execute_sql("COMMIT").is_ok());
    });
    REQUIRE(t.ms >= 150);
    REQUIRE_FALSE(t.ok);
    REQUIRE(t.text.find("Foreign key violation") != std::string::npos);
    REQUIRE(rows_of(f.ex, "SELECT COUNT(*) FROM kid").front().front() == "0");

    // 2. the same wait when nothing happens to the parent: the child is written
    a_id = f.transaction_id(a);
    REQUIRE(f.lm.acquire("d.par#fk:id", "6", a_id).kind == LockResult::Kind::Granted);
    t = run("INSERT INTO kid (pid) VALUES (6)", a_id, [&] { REQUIRE(a.execute_sql("COMMIT").is_ok()); });
    REQUIRE(t.ok);
    REQUIRE(t.ms >= 150);

    // 3. A is writing a child of 7 (its claim is on the value, the row is not there yet): B's DELETE of 7 waits, starts again and finds the child
    a_id = f.transaction_id(a);
    REQUIRE(f.lm.acquire_shared("d.par#fk:id", "7", a_id).kind == LockResult::Kind::Granted);
    t = run("DELETE FROM par WHERE id = 7", a_id, [&] {
        REQUIRE(a.execute_sql("INSERT INTO kid (pid) VALUES (7)").is_ok());
        REQUIRE(a.execute_sql("COMMIT").is_ok());
    });
    REQUIRE(t.ms >= 150);
    REQUIRE_FALSE(t.ok);
    REQUIRE(t.text.find("Foreign key violation") != std::string::npos);
    REQUIRE(rows_of(f.ex, "SELECT COUNT(*) FROM par WHERE id = 7").front().front() == "1");

    // 4. the same with ON DELETE CASCADE: the DELETE takes the child that was written meanwhile with it
    a_id = f.transaction_id(a);
    REQUIRE(f.lm.acquire_shared("d.par#fk:id", "8", a_id).kind == LockResult::Kind::Granted);
    t = run("DELETE FROM par WHERE id = 8", a_id, [&] {
        REQUIRE(a.execute_sql("INSERT INTO cas (pid) VALUES (8)").is_ok());
        REQUIRE(a.execute_sql("COMMIT").is_ok());
    });
    REQUIRE(t.ok);
    REQUIRE(t.ms >= 150);
    REQUIRE(rows_of(f.ex, "SELECT COUNT(*) FROM cas WHERE pid = 8").front().front() == "0");
    REQUIRE(rows_of(f.ex, "SELECT COUNT(*) FROM par WHERE id = 8").front().front() == "0");

    // 5. a parent that loses its id waits for a child being written, and finds it
    a_id = f.transaction_id(a);
    REQUIRE(f.lm.acquire_shared("d.par#fk:id", "9", a_id).kind == LockResult::Kind::Granted);
    t = run("UPDATE par SET id = 900 WHERE id = 9", a_id, [&] {
        REQUIRE(a.execute_sql("INSERT INTO kid (pid) VALUES (9)").is_ok());
        REQUIRE(a.execute_sql("COMMIT").is_ok());
    });
    REQUIRE(t.ms >= 150);
    REQUIRE_FALSE(t.ok);
    REQUIRE(rows_of(f.ex, "SELECT COUNT(*) FROM par WHERE id = 9").front().front() == "1");

    // 6. a child that moves to parent 10 waits for the DELETE of 10 and then finds no parent
    ok(f.ex, "INSERT INTO kid (pid) VALUES (11)");
    a_id = f.transaction_id(a);
    REQUIRE(f.lm.acquire("d.par#fk:id", "10", a_id).kind == LockResult::Kind::Granted);
    t = run("UPDATE kid SET pid = 10 WHERE pid = 11", a_id, [&] {
        REQUIRE(a.execute_sql("DELETE FROM par WHERE id = 10").is_ok());
        REQUIRE(a.execute_sql("COMMIT").is_ok());
    });
    REQUIRE(t.ms >= 150);
    REQUIRE_FALSE(t.ok);
    REQUIRE(rows_of(f.ex, "SELECT pid FROM kid WHERE pid >= 10 ORDER BY pid").front().front() == "11");

    // 7. a parent that is deleted while a child is already there (its row is visible, the transaction is open) is refused at once, not after a wait
    REQUIRE(a.execute_sql("USE d").is_ok());
    REQUIRE(a.execute_sql("BEGIN").is_ok());
    REQUIRE(a.execute_sql("INSERT INTO kid (pid) VALUES (12)").is_ok());
    t = run("DELETE FROM par WHERE id = 12", 0, nullptr, 0);
    REQUIRE_FALSE(t.ok);
    REQUIRE(t.ms < 150);
    REQUIRE(a.execute_sql("ROLLBACK").is_ok());

    // 8. children do not wait for each other (the claim of a child is shared), a NULL refers to nothing, a parent row whose referenced column is NULL claims nothing
    a_id = f.transaction_id(a);
    REQUIRE(f.lm.acquire_shared("d.par#fk:id", "14", a_id).kind == LockResult::Kind::Granted);
    t = run("INSERT INTO kid (pid) VALUES (14)", a_id, nullptr);
    REQUIRE(t.ok);
    REQUIRE(t.ms < 150);
    REQUIRE(f.lm.acquire_shared("d.par#fk:id", "15", a_id).kind == LockResult::Kind::Granted);
    t = run("UPDATE kid SET pid = 15 WHERE pid = 14", a_id, nullptr);
    REQUIRE(t.ok);
    REQUIRE(t.ms < 150);
    REQUIRE(f.lm.acquire("d.par#fk:id", "NULL", a_id).kind == LockResult::Kind::Granted);
    t = run("INSERT INTO kid (pid) VALUES (NULL)", a_id, nullptr);
    REQUIRE(t.ok);
    REQUIRE(t.ms < 150);
    REQUIRE(f.lm.acquire("d.par2#fk:code", "NULL", a_id).kind == LockResult::Kind::Granted);
    t = run("DELETE FROM par2 WHERE id = 2", a_id, nullptr);
    REQUIRE(t.ok);
    REQUIRE(t.ms < 150);
    // a parent that two tables refer to by two columns claims both values
    REQUIRE(f.lm.acquire_shared("d.par3#fk:code", "b", a_id).kind == LockResult::Kind::Granted);
    t = run("DELETE FROM par3 WHERE id = 2", a_id, nullptr);
    REQUIRE(t.ok);
    REQUIRE(t.ms >= 150);
    REQUIRE(a.execute_sql("ROLLBACK").is_ok());

    // 9. the wait ends with the lock wait timeout
    a_id = f.transaction_id(a);
    REQUIRE(f.lm.acquire("d.par#fk:id", "13", a_id).kind == LockResult::Kind::Granted);
    t = run("INSERT INTO kid (pid) VALUES (13)", a_id, nullptr, 1000, "SET @lock_wait_timeout = 300");
    REQUIRE_FALSE(t.ok);
    REQUIRE(t.text.find("1205") != std::string::npos);
    REQUIRE(t.ms >= 250);
    REQUIRE(t.ms < 900);
    REQUIRE(a.execute_sql("ROLLBACK").is_ok());
}

TEST_CASE("children of one parent do not wait for each other, and a statement claims only what it changes", "[unique_claim][fk_claim]") {
    FkFixture f("fk_claims_shared");
    Executor b = Executor::new_session(f.shared), c = Executor::new_session(f.shared), a = Executor::new_session(f.shared);
    REQUIRE(b.execute_sql("USE d").is_ok());
    REQUIRE(c.execute_sql("USE d").is_ok());
    auto timed = [](Executor& sess, const std::string& sql) {
        const auto t0 = std::chrono::steady_clock::now();
        auto r = sess.execute_sql(sql);
        Timed out;
        out.ok = r.is_ok();
        out.text = r.is_ok() ? r.value() : r.error();
        out.ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        return out;
    };
    auto release_later = [&](std::uint64_t holder, int ms) {
        return std::thread([&f, holder, ms] {
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            f.lm.release(holder);
        });
    };
    ok(f.ex, "ALTER TABLE kid ADD COLUMN note VARCHAR(10)");

    // a child that is written in a transaction holds its claim shared: another child of the same parent does not wait for the transaction
    REQUIRE(b.execute_sql("BEGIN").is_ok());
    REQUIRE(b.execute_sql("INSERT INTO kid (pid) VALUES (16)").is_ok());
    Timed t = timed(c, "INSERT INTO kid (pid) VALUES (16)");
    REQUIRE(t.ok);
    REQUIRE(t.ms < 150);
    REQUIRE(b.execute_sql("COMMIT").is_ok());

    // ... also after it has had to wait for the parent's DELETE (the wait takes the claim shared, too)
    std::uint64_t a_id = f.transaction_id(a);
    REQUIRE(f.lm.acquire("d.par#fk:id", "17", a_id).kind == LockResult::Kind::Granted);
    REQUIRE(b.execute_sql("BEGIN").is_ok());
    auto releaser = release_later(a_id, 250);
    t = timed(b, "INSERT INTO kid (pid) VALUES (17)");
    releaser.join();
    REQUIRE(t.ok);
    REQUIRE(t.ms >= 150);
    t = timed(c, "INSERT INTO kid (pid) VALUES (17)");
    REQUIRE(t.ok);
    REQUIRE(t.ms < 150);
    REQUIRE(b.execute_sql("COMMIT").is_ok());
    REQUIRE(a.execute_sql("ROLLBACK").is_ok());

    // ... and a child that moves to another parent after such a wait
    a_id = f.transaction_id(a);
    REQUIRE(f.lm.acquire("d.par#fk:id", "18", a_id).kind == LockResult::Kind::Granted);
    REQUIRE(b.execute_sql("BEGIN").is_ok());
    releaser = release_later(a_id, 250);
    t = timed(b, "UPDATE kid SET pid = 18 WHERE pid = 16");
    releaser.join();
    REQUIRE(t.ok);
    REQUIRE(t.ms >= 150);
    t = timed(c, "INSERT INTO kid (pid) VALUES (18)");
    REQUIRE(t.ok);
    REQUIRE(t.ms < 150);
    REQUIRE(b.execute_sql("COMMIT").is_ok());
    REQUIRE(a.execute_sql("ROLLBACK").is_ok());

    // a child UPDATE that leaves the parent value alone claims nothing (a parent being deleted does not hold it up) ...
    a_id = f.transaction_id(a);
    REQUIRE(f.lm.acquire("d.par#fk:id", "17", a_id).kind == LockResult::Kind::Granted);
    t = timed(c, "UPDATE kid SET note = 'x' WHERE pid = 17");
    REQUIRE(t.ok);
    REQUIRE(t.ms < 150);
    // ... nor does a parent UPDATE that leaves the referenced column alone (children being written do not hold it up)
    REQUIRE(f.lm.acquire_shared("d.par#fk:id", "20", a_id).kind == LockResult::Kind::Granted);
    t = timed(c, "UPDATE par SET name = 'renamed' WHERE id = 20");
    REQUIRE(t.ok);
    REQUIRE(t.ms < 150);
    // a column that no foreign key refers to is not claimed (par.name: only par5.name is referred to)
    ok(f.ex, "CREATE TABLE par5 (id INT PRIMARY KEY, name VARCHAR(10) UNIQUE)");
    ok(f.ex, "CREATE TABLE kid5 (id INT PRIMARY KEY AUTO_INCREMENT, n VARCHAR(10), FOREIGN KEY (n) REFERENCES par5 (name))");
    REQUIRE(f.lm.acquire_shared("d.par#fk:name", "p21", a_id).kind == LockResult::Kind::Granted);
    t = timed(c, "DELETE FROM par WHERE id = 21");
    REQUIRE(t.ok);
    REQUIRE(t.ms < 150);
    REQUIRE(a.execute_sql("ROLLBACK").is_ok());

    // a parent whose two columns are referred to claims the value of each
    for (const char* held : {"d.par3#fk:id|1", "d.par3#fk:code|a"}) {
        const std::string spec = held;
        const std::string table = spec.substr(0, spec.find('|')), key = spec.substr(spec.find('|') + 1);
        INFO(spec);
        a_id = f.transaction_id(a);
        REQUIRE(f.lm.acquire_shared(table, key, a_id).kind == LockResult::Kind::Granted);
        releaser = release_later(a_id, 250);
        t = timed(c, "DELETE FROM par3 WHERE id = 1");
        releaser.join();
        REQUIRE(t.ok);
        REQUIRE(t.ms >= 150);
        REQUIRE(a.execute_sql("ROLLBACK").is_ok());
        ok(f.ex, "INSERT INTO par3 VALUES (1, 'a')");
    }
}
