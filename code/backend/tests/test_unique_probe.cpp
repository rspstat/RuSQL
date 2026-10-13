#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// A one-row INSERT asks a cache of the UNIQUE values the live rows hold (engine/unique_probe.hpp) before it scans the table for a duplicate. The cache may only
// ever say "no row holds this" when that is true. These tests run the statements that can make it wrong -- everything that changes the rows behind its back
// (UPDATE, DELETE, ROLLBACK, savepoints, TRUNCATE, DROP TABLE, a restart) -- and compare with an engine that has no cache; and the whole suite can be run with
// RUSQL_PROBE_CHECK=1 RUSQL_PROBE_STREAK=0, which makes every use of the cache count the rows again and compare.

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

void fails(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    REQUIRE(r.is_err());
    REQUIRE(r.error().find("Duplicate") != std::string::npos);
}

Rows rows_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
    return table_cells(r.value());
}

// the cache of a database: switched on (built from the first look when `streak` is 0) or off, and checked against the rows at every use
void configure(Executor& ex, bool enabled, unsigned streak, bool check) {
    auto g = ex.get_shared()->write();
    g->unique_probes->enabled = enabled;
    g->unique_probes->streak_needed = streak;
    g->unique_probes->check = check;
}
std::uint64_t builds(Executor& ex) { return ex.get_shared()->read()->unique_probes->builds.load(); }
std::uint64_t skipped(Executor& ex) { return ex.get_shared()->read()->unique_probes->skipped.load(); }
} // namespace

TEST_CASE("the cache of UNIQUE values is made once the INSERTs come in a row, follows UPDATE / DELETE / COMMIT, and a change it cannot follow makes it wait again", "[unique_probe]") {
    TempDataDir dir("unique_probe_counters");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    configure(ex, true, 2, true);
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, u VARCHAR(8) UNIQUE, v INT)");
    for (int i = 1; i <= 8; i++) ok(ex, "INSERT INTO t VALUES (" + std::to_string(i) + ", 'w" + std::to_string(i) + "', 0)");
    REQUIRE(builds(ex) == 1);  // made at the third INSERT
    REQUIRE(skipped(ex) >= 5); // ... and every INSERT since has skipped its scan
    const std::uint64_t before = skipped(ex);
    fails(ex, "INSERT INTO t VALUES (20, 'w3', 0)"); // a value the table holds is "maybe": the scan runs and refuses it
    fails(ex, "INSERT INTO t VALUES (21, 'W3', 0)"); // ... also in another case
    REQUIRE(skipped(ex) == before);
    // INSERT, DELETE, UPDATE and COMMIT each take what they change into the cache (or, a COMMIT, change nothing): it stays current and is not made again
    const std::uint64_t made = builds(ex);
    for (int i = 0; i < 10; i++) {
        ok(ex, "INSERT INTO t VALUES (" + std::to_string(100 + i) + ", 'x" + std::to_string(i) + "', 0)");
        ok(ex, "DELETE FROM t WHERE id = " + std::to_string(100 + i));
        ok(ex, "INSERT INTO t VALUES (" + std::to_string(150 + i) + ", 'z" + std::to_string(i) + "', 0)");
        ok(ex, "UPDATE t SET u = 'zz" + std::to_string(i) + "' WHERE id = " + std::to_string(150 + i));
        ok(ex, "BEGIN");
        ok(ex, "INSERT INTO t VALUES (" + std::to_string(170 + i) + ", 'q" + std::to_string(i) + "', 0)");
        ok(ex, "COMMIT");
    }
    REQUIRE(builds(ex) == made);
    {   // ... and it was current after the COMMIT of the last round: the next INSERT does not scan
        const std::uint64_t before_next = skipped(ex);
        ok(ex, "INSERT INTO t VALUES (399, 'ac', 0)");
        REQUIRE(skipped(ex) == before_next + 1);
        ok(ex, "DELETE FROM t WHERE id = 399");
    }
    fails(ex, "INSERT INTO t VALUES (400, 'zz3', 0)"); // (the value an UPDATE moved a row to is held)
    ok(ex, "INSERT INTO t VALUES (401, 'z3', 0)");     // (the value it left is free)
    ok(ex, "DELETE FROM t WHERE id = 401");
    // a ROLLBACK does not say what it changed: the cache is dropped, and made again after INSERTs in a row. A value deleted in the meantime is free
    ok(ex, "BEGIN");
    ok(ex, "INSERT INTO t VALUES (250, 'r', 0)");
    ok(ex, "ROLLBACK");
    ok(ex, "INSERT INTO t VALUES (260, 'n1', 0), (261, 'n2', 0), (262, 'n3', 0), (263, 'n4', 0)"); // (a statement of several rows does not make the cache)
    REQUIRE(builds(ex) == made);
    for (int i = 0; i < 5; i++) ok(ex, "INSERT INTO t VALUES (" + std::to_string(200 + i) + ", 'y" + std::to_string(i) + "', 0)");
    REQUIRE(builds(ex) == made + 1);
    const std::uint64_t before_dead = skipped(ex);
    ok(ex, "INSERT INTO t VALUES (300, 'x3', 0)"); // 'x3' was held by a row that is gone: the cache does not count dead rows, so this INSERT skips its scan
    REQUIRE(skipped(ex) == before_dead + 1);
    fails(ex, "INSERT INTO t VALUES (301, 'x3', 0)");
    // an UPDATE that moves a UNIQUE value and an INSERT of several rows ask the cache as well (and a value that is held still refuses)
    const std::uint64_t before_more = skipped(ex);
    ok(ex, "UPDATE t SET u = 'moved1' WHERE id = 5");
    REQUIRE(skipped(ex) == before_more + 1);
    ok(ex, "INSERT INTO t VALUES (500, 'm1', 0), (501, 'm2', 0), (502, 'm3', 0)");
    REQUIRE(skipped(ex) == before_more + 4);
    fails(ex, "UPDATE t SET u = 'M1' WHERE id = 6");
    fails(ex, "INSERT INTO t VALUES (503, 'MOVED1', 0), (504, 'fresh', 0)");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM t WHERE u = 'fresh'") == Rows{{"0"}});
    {   // a refused UPDATE changes nothing and the cache stays current
        const std::uint64_t before_refused = skipped(ex);
        ok(ex, "INSERT INTO t VALUES (510, 'fresh2', 0)");
        REQUIRE(skipped(ex) == before_refused + 1);
        // a DELETE takes the value out of the cache at once, outside a transaction and in one
        ok(ex, "INSERT INTO t VALUES (520, 'gone1', 0)");
        ok(ex, "DELETE FROM t WHERE id = 520");
        const std::uint64_t before_gone = skipped(ex);
        ok(ex, "INSERT INTO t VALUES (521, 'gone1', 0)");
        REQUIRE(skipped(ex) == before_gone + 1);
        ok(ex, "BEGIN");
        ok(ex, "INSERT INTO t VALUES (522, 'gone2', 0)");
        ok(ex, "DELETE FROM t WHERE id = 522");
        const std::uint64_t before_gone2 = skipped(ex);
        ok(ex, "INSERT INTO t VALUES (523, 'gone2', 0)");
        REQUIRE(skipped(ex) == before_gone2 + 1);
        ok(ex, "COMMIT");
    }
    // the same for a composite primary key and for UNIQUE (a, b)
    ok(ex, "CREATE TABLE cp (a VARCHAR(4), b INT, v INT, PRIMARY KEY (a, b), UNIQUE (b, v))");
    const std::uint64_t before_cp = skipped(ex);
    for (int i = 1; i <= 8; i++) ok(ex, "INSERT INTO cp VALUES ('k', " + std::to_string(i) + ", " + std::to_string(i) + ")");
    REQUIRE(skipped(ex) >= before_cp + 2 * 5); // (two keys per INSERT, from the third INSERT on)
    fails(ex, "INSERT INTO cp VALUES ('K', 3, 99)");
    fails(ex, "INSERT INTO cp VALUES ('z', 4, 4)");
    {   // several rows ask for both keys of the table as well
        const std::uint64_t before_multi = skipped(ex);
        ok(ex, "INSERT INTO cp VALUES ('m', 1, 101), ('m', 2, 102), ('m', 3, 103)");
        REQUIRE(skipped(ex) == before_multi + 6);
    }
    {   // a table whose cache has not been made is not asked for one by a statement of several rows, and a new key added by ALTER TABLE is in the cache once it is made again
        ok(ex, "CREATE TABLE fresh_table (id INT PRIMARY KEY, u INT UNIQUE)");
        const std::uint64_t made_before = builds(ex);
        ok(ex, "INSERT INTO fresh_table VALUES (1, 1)"); // (one row: the table gets an entry, no cache yet)
        ok(ex, "INSERT INTO fresh_table VALUES (2, 2), (3, 3), (4, 4), (5, 5), (6, 6)");
        ok(ex, "INSERT INTO fresh_table VALUES (7, 7), (8, 8), (9, 9)");
        REQUIRE(builds(ex) == made_before);
        ok(ex, "CREATE TABLE two_keys (id INT PRIMARY KEY, a INT UNIQUE, b INT)");
        for (int i = 1; i <= 5; i++) ok(ex, "INSERT INTO two_keys VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ", " + std::to_string(i * 10) + ")");
        ok(ex, "ALTER TABLE two_keys ADD UNIQUE (b)");
        // (INSERTs that are refused look at the cache and never reach the point where they would change it: the cache that was made for the old keys is made again, for the new ones)
        for (int i = 0; i < 3; i++) fails(ex, "INSERT INTO two_keys VALUES (" + std::to_string(20 + i) + ", 1, 99)");
        for (int i = 6; i <= 8; i++) ok(ex, "INSERT INTO two_keys VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ", " + std::to_string(i * 10) + ")");
        const std::uint64_t before_two = skipped(ex);
        ok(ex, "INSERT INTO two_keys VALUES (9, 9, 90)");
        REQUIRE(skipped(ex) == before_two + 2); // (a and b)
        fails(ex, "INSERT INTO two_keys VALUES (10, 10, 40)");
    }
    // a row that a transaction has deleted is still in the table until it ends, but it holds no key: the cache does not count it
    ok(ex, "BEGIN");
    ok(ex, "DELETE FROM t WHERE u = 'w1'");
    for (int i = 0; i < 3; i++) ok(ex, "INSERT INTO t VALUES (" + std::to_string(400 + i) + ", 'z" + std::to_string(i) + "', 0)");
    const std::uint64_t before_deleted = skipped(ex);
    ok(ex, "INSERT INTO t VALUES (410, 'w1', 0)");
    REQUIRE(skipped(ex) == before_deleted + 1);
    ok(ex, "ROLLBACK");
    fails(ex, "INSERT INTO t VALUES (411, 'w1', 0)"); // (the row is back)
    // a table without a key to look up has no cache
    ok(ex, "CREATE TABLE plain (id INT PRIMARY KEY, v INT)");
    const std::uint64_t base = builds(ex), saved = skipped(ex);
    for (int i = 1; i <= 6; i++) ok(ex, "INSERT INTO plain VALUES (" + std::to_string(i) + ", 0)");
    REQUIRE(builds(ex) == base);
    REQUIRE(skipped(ex) == saved);
}

TEST_CASE("the cache of UNIQUE values is not believed after the rows change behind its back", "[unique_probe]") {
    TempDataDir dir("unique_probe_stale");
    for (int mode = 0; mode < 3; mode++) { // the three kinds of key
        INFO("key kind " << mode);
        fs::remove_all(dir.path);
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        configure(ex, true, 0, true); // built at the first look, compared with the rows at every use
        const char* create[] = {"CREATE TABLE t (id INT PRIMARY KEY, k VARCHAR(8) UNIQUE, v INT)",
                                "CREATE TABLE t (id INT PRIMARY KEY, a VARCHAR(8), b INT, v INT, UNIQUE (a, b))",
                                "CREATE TABLE t (a VARCHAR(8), b INT, v INT, PRIMARY KEY (a, b))"};
        ok(ex, create[mode]);
        // the n-th row, and an INSERT of a row that would clash with it / a free one
        auto row = [&](int n) { return mode == 0 ? "(" + std::to_string(n) + ", 'k" + std::to_string(n) + "', 0)"
                                 : mode == 1     ? "(" + std::to_string(n) + ", 'a" + std::to_string(n % 3) + "', " + std::to_string(n) + ", 0)"
                                                 : "('a" + std::to_string(n % 3) + "', " + std::to_string(n) + ", 0)"; };
        auto clash = [&](int n, int fresh_id) { // the same key as row n, under another id and in another case
            std::string r = row(n);
            std::string upper;
            for (char c : r) upper += (c == 'k' || c == 'a') ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
            if (mode == 0) upper.replace(1, upper.find(',') - 1, std::to_string(fresh_id));
            else if (mode == 1) upper.replace(1, upper.find(',') - 1, std::to_string(fresh_id));
            return upper;
        };
        for (int n = 1; n <= 6; n++) ok(ex, "INSERT INTO t VALUES " + row(n)); // (the cache is built and used)
        fails(ex, "INSERT INTO t VALUES " + clash(3, 90));
        const std::string key_filter = mode == 0 ? "k = 'k3'" : mode == 1 ? "a = 'a0' AND b = 3" : "a = 'a0' AND b = 3";

        // a deleted row's key is free, and a ROLLBACK of the delete brings the row back: the key is taken again
        ok(ex, "BEGIN");
        ok(ex, "DELETE FROM t WHERE " + key_filter);
        ok(ex, "INSERT INTO t VALUES " + clash(3, 91));
        ok(ex, "ROLLBACK");
        fails(ex, "INSERT INTO t VALUES " + clash(3, 92));
        REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM t WHERE " + key_filter) == Rows{{"1"}});
        // a ROLLBACK that only brings a row back: the cache was made while the row was deleted and has seen no INSERT of its key
        ok(ex, "BEGIN");
        ok(ex, "DELETE FROM t WHERE " + key_filter);
        ok(ex, "INSERT INTO t VALUES " + row(60));
        ok(ex, "INSERT INTO t VALUES " + row(61));
        ok(ex, "ROLLBACK");
        const char* move_to_3[] = {"UPDATE t SET k = 'K3' WHERE id = 1", "UPDATE t SET a = 'A0', b = 3 WHERE id = 1", "UPDATE t SET a = 'A0', b = 3 WHERE a = 'a1' AND b = 1"};
        fails(ex, move_to_3[mode]); // (an UPDATE asks the cache too, before anything made it again: it must not believe a cache that the ROLLBACK made out of date)
        fails(ex, "INSERT INTO t VALUES " + clash(3, 89));
        // a savepoint rolled back, then a COMMIT of a transaction that wrote the table: the COMMIT does not make the cache current again
        ok(ex, "BEGIN");
        ok(ex, "INSERT INTO t VALUES " + row(70));
        ok(ex, "SAVEPOINT s9");
        ok(ex, "DELETE FROM t WHERE " + key_filter);
        ok(ex, "ROLLBACK TO s9");
        ok(ex, "COMMIT");
        fails(ex, "INSERT INTO t VALUES " + clash(3, 88));
        fails(ex, move_to_3[mode]);
        // the same through a savepoint
        ok(ex, "BEGIN");
        ok(ex, "SAVEPOINT s1");
        ok(ex, "DELETE FROM t WHERE " + key_filter);
        ok(ex, "INSERT INTO t VALUES " + clash(3, 93));
        ok(ex, "ROLLBACK TO s1");
        ok(ex, "COMMIT");
        fails(ex, "INSERT INTO t VALUES " + clash(3, 94));
        // an UPDATE that moves a key frees the old one and takes the new one
        if (mode == 0) {
            ok(ex, "UPDATE t SET k = 'moved' WHERE id = 3");
            ok(ex, "INSERT INTO t VALUES (95, 'K3', 0)");
            fails(ex, "INSERT INTO t VALUES (96, 'MOVED', 0)");
            ok(ex, "DELETE FROM t WHERE id = 95");
            ok(ex, "UPDATE t SET k = 'k3' WHERE id = 3");
        } else {
            ok(ex, "UPDATE t SET b = b + 100 WHERE " + key_filter);
            ok(ex, "INSERT INTO t VALUES " + (mode == 1 ? std::string("(95, 'A0', 3, 0)") : std::string("('A0', 3, 0)")));
            fails(ex, "INSERT INTO t VALUES " + (mode == 1 ? std::string("(96, 'a0', 103, 0)") : std::string("('a0', 103, 0)")));
            ok(ex, "DELETE FROM t WHERE " + std::string(mode == 1 ? "id = 95" : "a = 'A0' AND b = 3"));
            ok(ex, "UPDATE t SET b = b - 100 WHERE b = 103");
        }
        // a rolled-back INSERT leaves its key free
        ok(ex, "BEGIN");
        ok(ex, "INSERT INTO t VALUES " + row(40));
        fails(ex, "INSERT INTO t VALUES " + clash(40, 97));
        ok(ex, "ROLLBACK");
        ok(ex, "INSERT INTO t VALUES " + clash(40, 98));
        // TRUNCATE and DROP TABLE empty the key space
        ok(ex, "TRUNCATE TABLE t");
        for (int n = 1; n <= 3; n++) ok(ex, "INSERT INTO t VALUES " + row(n));
        ok(ex, "DROP TABLE t");
        ok(ex, create[mode]);
        for (int n = 1; n <= 3; n++) ok(ex, "INSERT INTO t VALUES " + row(n)); // the rows of the old table do not clash
        // a restart: the cache starts empty and is made again from the rows
        ex.~Executor();
        new (&ex) Executor(dir.path);
        ok(ex, "USE d");
        configure(ex, true, 0, true);
        fails(ex, "INSERT INTO t VALUES " + clash(2, 99));
        ok(ex, "INSERT INTO t VALUES " + row(50));
        fails(ex, "INSERT INTO t VALUES " + clash(50, 99));
    }
}

// The same statements, one engine with the cache (built at the first look, checked against the rows at every use) and one without it, must give the same answers.
TEST_CASE("a table with the cache of UNIQUE values and one without it give the same answers", "[unique_probe][random]") {
    unsigned seed_count = 4; // RUSQL_FUZZ_SEEDS=40 makes it a long campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    for (unsigned seed = 1; seed <= seed_count; seed++) {
        INFO("seed " << seed);
        TempDataDir dir_with("unique_probe_with"), dir_without("unique_probe_without");
        auto open = [&](std::unique_ptr<Executor>& e, const std::string& path, bool probe) {
            e = std::make_unique<Executor>(path);
            ok(*e, "CREATE DATABASE IF NOT EXISTS d");
            ok(*e, "USE d");
            configure(*e, probe, 0, probe);
        };
        std::unique_ptr<Executor> with, without;
        open(with, dir_with.path, true);
        open(without, dir_without.path, false);
        for (auto* e : {with.get(), without.get()}) {
            ok(*e, "CREATE TABLE t1 (id INT PRIMARY KEY, u VARCHAR(8) UNIQUE, w INT UNIQUE, a INT, b INT, v INT, UNIQUE (a, b))");
            ok(*e, "CREATE TABLE t2 (a VARCHAR(4), b INT, v INT, PRIMARY KEY (a, b))");
        }
        std::mt19937 rng(seed * 6700417u);
        auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
        const std::vector<std::string> words = {"a", "A", "b", "B", "c", "ab", "x", "X"};
        auto word = [&]() { return pick(8) == 0 ? std::string("NULL") : "'" + words[pick(words.size())] + "'"; };
        auto t1_row = [&]() {
            std::string a = pick(5) == 0 ? "NULL" : std::to_string(1 + pick(3)), b = pick(5) == 0 ? "NULL" : std::to_string(1 + pick(4));
            std::string w = pick(5) == 0 ? "NULL" : std::to_string(1 + pick(12));
            return "(" + std::to_string(1 + pick(30)) + ", " + word() + ", " + w + ", " + a + ", " + b + ", " + std::to_string(pick(100)) + ")";
        };
        auto t2_row = [&]() { return "('" + std::string(1, "xXyY"[pick(4)]) + "', " + std::to_string(pick(4)) + ", " + std::to_string(pick(100)) + ")"; };
        bool in_txn = false;
        int savepoints = 0;
        std::uint64_t cache_uses = 0; // (a restart starts the counters again)
        for (int step = 0; step < 260; step++) {
            std::string sql;
            const std::size_t kind = pick(100);
            const bool first = pick(2) == 0;
            const std::string table = first ? "t1" : "t2";
            auto one_row = [&]() { return first ? t1_row() : t2_row(); };
            if (kind < 34) {
                sql = "INSERT INTO " + table + " VALUES " + one_row();
            } else if (kind < 40) {
                sql = "INSERT INTO " + table + " VALUES " + one_row() + ", " + one_row();
            } else if (kind < 45) {
                sql = "INSERT IGNORE INTO " + table + " VALUES " + one_row();
            } else if (kind < 50) {
                sql = "INSERT INTO " + table + " VALUES " + one_row() + " ON DUPLICATE KEY UPDATE v = v + 1";
            } else if (kind < 54) {
                sql = "REPLACE INTO " + table + " VALUES " + one_row();
            } else if (kind < 62) { // move a key
                sql = first ? (pick(2) ? "UPDATE t1 SET u = " + word() : "UPDATE t1 SET w = " + std::to_string(1 + pick(12))) + " WHERE id = " + std::to_string(1 + pick(30))
                            : "UPDATE t2 SET b = " + std::to_string(pick(4)) + " WHERE a = '" + std::string(1, "xXyY"[pick(4)]) + "'";
            } else if (kind < 66) {
                sql = first ? "UPDATE t1 SET a = " + std::to_string(1 + pick(3)) + ", b = " + std::to_string(1 + pick(4)) + " WHERE id = " + std::to_string(1 + pick(30))
                            : "UPDATE t2 SET v = v + 1 WHERE b = " + std::to_string(pick(4));
            } else if (kind < 76) {
                sql = first ? "DELETE FROM t1 WHERE id = " + std::to_string(1 + pick(30)) : "DELETE FROM t2 WHERE a = '" + std::string(1, "xXyY"[pick(4)]) + "' AND b = " + std::to_string(pick(4));
            } else if (kind < 80) {
                sql = first ? "DELETE FROM t1 WHERE u = '" + words[pick(words.size())] + "'" : "DELETE FROM t2 WHERE b = " + std::to_string(pick(4));
            } else if (kind < 84) {
                sql = in_txn ? "COMMIT" : "BEGIN";
                in_txn = !in_txn;
                savepoints = 0;
            } else if (kind < 90) {
                if (in_txn) { sql = "ROLLBACK"; in_txn = false; savepoints = 0; } else sql = "SELECT 1";
            } else if (kind < 93) {
                if (in_txn) { sql = "SAVEPOINT sp" + std::to_string(++savepoints); } else sql = "SELECT 1";
            } else if (kind < 96) {
                if (in_txn && savepoints > 0) sql = "ROLLBACK TO sp" + std::to_string(1 + pick(static_cast<std::size_t>(savepoints)));
                else sql = "SELECT 1";
            } else if (kind < 97) {
                if (in_txn) sql = "SELECT 1"; else sql = "TRUNCATE TABLE " + table;
            } else if (kind < 98) {
                if (in_txn) sql = "SELECT 1";
                else { // a restart: nothing the cache knew is kept
                    cache_uses += builds(*with) + skipped(*with);
                    open(with, dir_with.path, true);
                    open(without, dir_without.path, false);
                    sql = "SELECT 1";
                }
            } else {
                sql = "SELECT COUNT(*) FROM " + table;
            }
            INFO("step " << step << ": " << sql);
            auto ra = with->execute_sql(sql);
            auto rb = without->execute_sql(sql);
            INFO("with: " << (ra.is_err() ? ra.error() : "ok") << " | without: " << (rb.is_err() ? rb.error() : "ok"));
            REQUIRE(ra.is_ok() == rb.is_ok());
            if (ra.is_err()) REQUIRE(ra.error() == rb.error());
            if (step % 9 == 0) {
                REQUIRE(rows_of(*with, "SELECT id, u, w, a, b, v FROM t1 ORDER BY id") == rows_of(*without, "SELECT id, u, w, a, b, v FROM t1 ORDER BY id"));
                REQUIRE(rows_of(*with, "SELECT a, b, v FROM t2 ORDER BY a, b") == rows_of(*without, "SELECT a, b, v FROM t2 ORDER BY a, b"));
            }
        }
        if (in_txn) { ok(*with, "ROLLBACK"); ok(*without, "ROLLBACK"); }
        REQUIRE(rows_of(*with, "SELECT id, u, w, a, b, v FROM t1 ORDER BY id") == rows_of(*without, "SELECT id, u, w, a, b, v FROM t1 ORDER BY id"));
        REQUIRE(rows_of(*with, "SELECT a, b, v FROM t2 ORDER BY a, b") == rows_of(*without, "SELECT a, b, v FROM t2 ORDER BY a, b"));
        REQUIRE(cache_uses + builds(*with) + skipped(*with) > 0); // (the cache was really used)
    }
}

// Sessions that write the same tables at the same time, one cache for all of them: INSERTs of values that clash, UPDATEs that move them, DELETEs that free them, and
// a ROLLBACK now and then, with the cache checked against the rows at every use (a mismatch is an error that names the cache). Afterwards no value is held twice.
TEST_CASE("sessions that insert, update and delete UNIQUE values together never hold a value twice", "[unique_probe][random]") {
    TempDataDir dir("unique_probe_threads");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    configure(ex, std::getenv("RUSQL_TEST_NO_PROBE") == nullptr, 0, true); // (RUSQL_TEST_NO_PROBE=1: the same sessions without the cache, to tell its faults from the engine's)
    ok(ex, "CREATE TABLE u (id INT PRIMARY KEY AUTO_INCREMENT, k VARCHAR(8) UNIQUE, w INT)");
    ok(ex, "CREATE TABLE c (a VARCHAR(4), b INT, v INT, PRIMARY KEY (a, b), UNIQUE (b, v))");
    unsigned rounds = 2; // RUSQL_FUZZ_SEEDS=40 makes it a long campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) rounds = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    auto shared = ex.get_shared();
    std::atomic<int> cache_errors{0};
    constexpr int kThreads = 6, kSteps = 150;
    for (unsigned round = 0; round < rounds; round++) {
        ok(ex, "DELETE FROM u");
        ok(ex, "DELETE FROM c");
        std::vector<std::thread> threads;
        for (int w = 0; w < kThreads; w++) {
            threads.emplace_back([&, w] {
                Executor sess = Executor::new_session(shared);
                if (!sess.execute_sql("USE d").is_ok()) return;
                std::mt19937 rng(round * 1000 + static_cast<unsigned>(w) * 17 + 1);
                auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
                auto run = [&](const std::string& sql) {
                    auto r = sess.execute_sql(sql);
                    if (r.is_err() && r.error().find("cache of UNIQUE") != std::string::npos) cache_errors++;
                };
                bool in_txn = false;
                for (int i = 0; i < kSteps; i++) {
                    const std::string k = "'k" + std::to_string(pick(10)) + "'";
                    const std::string a = "'" + std::string(1, "xXyY"[pick(4)]) + "'", b = std::to_string(pick(4)), v = std::to_string(pick(5));
                    std::size_t op = pick(12);
                    // (only INSERTs run inside a transaction: a DELETE or UPDATE that is rolled back brings its old row back, and by then another session may hold the
                    // key it freed -- the engine lets the INSERT in while the DELETE is uncommitted. That is the engine's, not the cache's: see the PLAN.md note)
                    if (in_txn && (op == 5 || op == 6 || op == 7 || op == 8 || op == 11)) op = 0;
                    switch (op) {
                    case 0: case 1: case 2: run("INSERT INTO u (k, w) VALUES (" + k + ", " + std::to_string(w) + ")"); break;
                    case 3: case 4: run("INSERT INTO c VALUES (" + a + ", " + b + ", " + v + ")"); break;
                    case 5: run("UPDATE u SET k = " + k + " WHERE id = " + std::to_string(1 + pick(40))); break;
                    case 6: run("UPDATE c SET v = " + v + " WHERE a = " + a + " AND b = " + b); break;
                    case 7: run("DELETE FROM u WHERE k = " + k); break;
                    case 8: run("DELETE FROM c WHERE a = " + a + " AND b = " + b); break;
                    case 9: run(in_txn ? "ROLLBACK" : "BEGIN"); in_txn = !in_txn; break;
                    case 10: run(in_txn ? "COMMIT" : "INSERT IGNORE INTO u (k, w) VALUES (" + k + ", 0)"); in_txn = false; break;
                    default: run("REPLACE INTO c VALUES (" + a + ", " + b + ", " + v + ")"); break;
                    }
                }
                if (in_txn) run("ROLLBACK");
            });
        }
        for (auto& t : threads) t.join();
        REQUIRE(rows_of(ex, "SELECT k, COUNT(*) FROM u GROUP BY k HAVING COUNT(*) > 1").empty());
        REQUIRE(rows_of(ex, "SELECT a, b, COUNT(*) FROM c GROUP BY a, b HAVING COUNT(*) > 1").empty());
        REQUIRE(rows_of(ex, "SELECT b, v, COUNT(*) FROM c GROUP BY b, v HAVING COUNT(*) > 1").empty());
    }
    REQUIRE(cache_errors.load() == 0);
}
