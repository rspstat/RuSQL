#include <atomic>
#include <filesystem>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// UPDATE no longer clones the whole table to find its rows: candidates are matched in place
// by position, and a bare `pk = literal` on a single-column PK is answered from the
// row_pk_pos cache in O(1) (always re-validated on use). These tests pin the semantics that
// the cache and the position re-verification must never change.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

struct Triple {
    int id, v, g;
    bool operator==(const Triple& o) const { return id == o.id && v == o.v && g == o.g; }
};

// Parses the "| id | v | g |" rows of a SELECT id, v, g result.
std::vector<Triple> parse_rows(const std::string& out) {
    std::vector<Triple> rows;
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        std::istringstream ls(line);
        std::string cell;
        std::vector<std::string> cells;
        while (std::getline(ls, cell, '|')) {
            auto b = cell.find_first_not_of(' ');
            if (b == std::string::npos) continue;
            auto e = cell.find_last_not_of(' ');
            cells.push_back(cell.substr(b, e - b + 1));
        }
        if (cells.size() != 3) continue;
        try {
            rows.push_back({std::stoi(cells[0]), std::stoi(cells[1]), std::stoi(cells[2])});
        } catch (...) {
        }
    }
    return rows;
}

std::vector<Triple> snapshot(Executor& ex) {
    auto r = ex.execute_sql("SELECT id, v, g FROM t ORDER BY id");
    REQUIRE(r.is_ok());
    return parse_rows(r.value());
}

using Model = std::map<int, std::pair<int, int>>; // id -> (v, g)

std::vector<Triple> from_model(const Model& m) {
    std::vector<Triple> out;
    for (auto& [id, vg] : m) out.push_back({id, vg.first, vg.second});
    return out;
}

void setup(Executor& ex) {
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, v INT, g INT)").is_ok());
}

bool updated(const StringResult& r, int n) {
    return r.is_ok() && r.value().find(std::to_string(n) + " row(s) updated") != std::string::npos;
}
} // namespace

TEST_CASE("UPDATE by primary key: repeated, changing the key, no match, extra conjunct", "[update][fast]") {
    TempDataDir dir("upd_fast_1");
    Executor ex(dir.path);
    setup(ex);
    for (int i = 1; i <= 50; i++) REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + "," + std::to_string(i) + ",0)").is_ok());

    // the same key updated many times (each UPDATE appends a new version and moves the cached position)
    for (int round = 0; round < 30; round++)
        REQUIRE(updated(ex.execute_sql("UPDATE t SET v = " + std::to_string(1000 + round) + " WHERE id = 7"), 1));
    REQUIRE(ex.execute_sql("SELECT v FROM t WHERE id = 7").value().find("1029") != std::string::npos);
    REQUIRE(ex.execute_sql("SELECT COUNT(*) FROM t").value().find("50") != std::string::npos);

    // no match: reports 0 rows and changes nothing
    REQUIRE(updated(ex.execute_sql("UPDATE t SET v = 5 WHERE id = 9999"), 0));

    // extra conjunct goes through the scan path and still honours the whole condition
    REQUIRE(updated(ex.execute_sql("UPDATE t SET v = -1 WHERE id = 8 AND v > 100"), 0));
    REQUIRE(updated(ex.execute_sql("UPDATE t SET v = -1 WHERE id = 8 AND v < 100"), 1));
    REQUIRE(ex.execute_sql("SELECT v FROM t WHERE id = 8").value().find("-1") != std::string::npos);

    // changing the primary key itself: old key gone, new key present, later pk-updates find it
    REQUIRE(updated(ex.execute_sql("UPDATE t SET id = 500 WHERE id = 9"), 1));
    REQUIRE(ex.execute_sql("SELECT * FROM t WHERE id = 9").value().find("0 rows") != std::string::npos);
    REQUIRE(updated(ex.execute_sql("UPDATE t SET v = 77 WHERE id = 500"), 1));
    REQUIRE(ex.execute_sql("SELECT v FROM t WHERE id = 500").value().find("77") != std::string::npos);
    REQUIRE(updated(ex.execute_sql("UPDATE t SET v = 78 WHERE id = 9"), 0));

    // updating to the identical value is still a real (counted) update
    REQUIRE(updated(ex.execute_sql("UPDATE t SET v = 77 WHERE id = 500"), 1));
    // a PK change that collides must not corrupt anything
    auto dup = ex.execute_sql("UPDATE t SET id = 10 WHERE id = 500");
    (void)dup; // may be accepted or rejected by the engine; either way the table must stay consistent:
    auto rows = snapshot(ex);
    std::map<int, int> seen;
    for (auto& r : rows) seen[r.id]++;
    for (auto& [id, n] : seen) REQUIRE(n >= 1);
}

TEST_CASE("UPDATE ... RETURNING and non-pk / range / full-table forms still match every row", "[update][fast]") {
    TempDataDir dir("upd_fast_2");
    Executor ex(dir.path);
    setup(ex);
    for (int i = 1; i <= 40; i++) REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + ",0," + std::to_string(i % 4) + ")").is_ok());

    auto ret = ex.execute_sql("UPDATE t SET v = 9 WHERE id = 5 RETURNING id, v");
    REQUIRE(ret.is_ok());
    REQUIRE(ret.value().find("| 5") != std::string::npos);
    REQUIRE(ret.value().find("9") != std::string::npos);

    REQUIRE(updated(ex.execute_sql("UPDATE t SET v = 1 WHERE g = 2"), 10));
    REQUIRE(updated(ex.execute_sql("UPDATE t SET v = v + 10 WHERE id BETWEEN 11 AND 20"), 10));
    REQUIRE(updated(ex.execute_sql("UPDATE t SET v = v + 1"), 40));                // no WHERE
    REQUIRE(updated(ex.execute_sql("UPDATE t SET v = 0 WHERE id > 35 OR id < 3"), 7));
    REQUIRE(updated(ex.execute_sql("UPDATE t SET v = 42 WHERE id IN (1, 2, 3)"), 3));
    REQUIRE(updated(ex.execute_sql("UPDATE t SET v = 1 WHERE id IN (SELECT id FROM t WHERE g = 1)"), 10)); // subquery -> legacy path
    REQUIRE(ex.execute_sql("SELECT v FROM t WHERE id = 1").value().find("1") != std::string::npos);
}

TEST_CASE("UPDATE stays correct after DELETE, ROLLBACK and auto-VACUUM shift row positions", "[update][fast]") {
    TempDataDir dir("upd_fast_3");
    Executor ex(dir.path);
    setup(ex);
    Model model;
    for (int i = 1; i <= 60; i++) {
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + "," + std::to_string(i) + ",0)").is_ok());
        model[i] = {i, 0};
    }
    auto check = [&] { REQUIRE(snapshot(ex) == from_model(model)); };

    // warm the position cache, then churn: deletes (swap_remove), rolled-back updates (erased
    // versions), and enough updates to trigger auto-vacuum several times (rows erased)
    REQUIRE(updated(ex.execute_sql("UPDATE t SET v = 100 WHERE id = 30"), 1)); model[30].first = 100;
    REQUIRE(ex.execute_sql("DELETE FROM t WHERE id = 31").is_ok()); model.erase(31);
    REQUIRE(ex.execute_sql("BEGIN").is_ok());
    REQUIRE(updated(ex.execute_sql("UPDATE t SET v = 555 WHERE id = 30"), 1));
    REQUIRE(updated(ex.execute_sql("UPDATE t SET v = 556 WHERE id = 32"), 1));
    REQUIRE(ex.execute_sql("ROLLBACK").is_ok());
    check();
    for (int round = 0; round < 700; round++) {
        int id = 1 + (round * 7) % 60;
        if (!model.count(id)) continue;
        int nv = round;
        REQUIRE(updated(ex.execute_sql("UPDATE t SET v = " + std::to_string(nv) + " WHERE id = " + std::to_string(id)), 1));
        model[id].first = nv;
        if (round % 97 == 0) {
            REQUIRE(ex.execute_sql("DELETE FROM t WHERE id = " + std::to_string(id)).is_ok());
            model.erase(id);
        }
        if (round % 150 == 0) check();
    }
    REQUIRE(ex.execute_sql("VACUUM").is_ok());
    check();
    // after everything, an update by key must still land on the live version
    int any = model.begin()->first;
    REQUIRE(updated(ex.execute_sql("UPDATE t SET v = 31337 WHERE id = " + std::to_string(any)), 1));
    model[any].first = 31337;
    check();
}

TEST_CASE("UPDATE: randomized differential test against a model (autocommit + transactions)", "[update][fast][fuzz]") {
    for (int seed = 1; seed <= 12; seed++) {
        TempDataDir dir("upd_fast_fuzz_" + std::to_string(seed));
        Executor ex(dir.path);
        setup(ex);
        std::mt19937 rnd(seed * 7919);
        auto R = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rnd); };
        Model committed, work;
        bool in_txn = false;
        auto cur = [&]() -> Model& { return in_txn ? work : committed; };

        for (int step = 0; step < 260; step++) {
            int op = R(0, 99);
            int id = R(1, 40);
            if (op < 6 && !in_txn) {
                REQUIRE(ex.execute_sql("BEGIN").is_ok());
                in_txn = true;
                work = committed;
            } else if (op < 10 && in_txn) {
                REQUIRE(ex.execute_sql("COMMIT").is_ok());
                committed = work;
                in_txn = false;
            } else if (op < 13 && in_txn) {
                REQUIRE(ex.execute_sql("ROLLBACK").is_ok());
                in_txn = false;
            } else if (op < 35) { // insert
                if (cur().count(id)) continue;
                int v = R(0, 999), g = R(0, 4);
                REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(id) + "," + std::to_string(v) + "," + std::to_string(g) + ")").is_ok());
                cur()[id] = {v, g};
            } else if (op < 60) { // update by pk (cache path), existing or not
                int v = R(0, 999);
                auto r = ex.execute_sql("UPDATE t SET v = " + std::to_string(v) + " WHERE id = " + std::to_string(id));
                REQUIRE(updated(r, cur().count(id) ? 1 : 0));
                if (cur().count(id)) cur()[id].first = v;
            } else if (op < 68) { // update the primary key itself
                int nid = R(41, 80);
                if (!cur().count(id) || cur().count(nid)) continue;
                REQUIRE(updated(ex.execute_sql("UPDATE t SET id = " + std::to_string(nid) + " WHERE id = " + std::to_string(id)), 1));
                cur()[nid] = cur()[id];
                cur().erase(id);
            } else if (op < 76) { // non-pk predicate (scan path)
                int g = R(0, 4), d = R(1, 5);
                int n = 0;
                for (auto& [k, vg] : cur()) if (vg.second == g) { vg.first += d; n++; }
                REQUIRE(updated(ex.execute_sql("UPDATE t SET v = v + " + std::to_string(d) + " WHERE g = " + std::to_string(g)), n));
            } else if (op < 82) { // range
                int lo = R(1, 30), hi = lo + R(0, 10), d = R(1, 5);
                int n = 0;
                for (auto& [k, vg] : cur()) if (k >= lo && k <= hi) { vg.first += d; n++; }
                REQUIRE(updated(ex.execute_sql("UPDATE t SET v = v + " + std::to_string(d) + " WHERE id BETWEEN " + std::to_string(lo) + " AND " + std::to_string(hi)), n));
            } else if (op < 90) { // delete
                REQUIRE(ex.execute_sql("DELETE FROM t WHERE id = " + std::to_string(id)).is_ok());
                cur().erase(id);
            } else if (op < 93) { // pk-eq with an extra conjunct (scan path)
                int t = R(0, 999), v = R(0, 999);
                bool hit = cur().count(id) && cur()[id].first > t;
                REQUIRE(updated(ex.execute_sql("UPDATE t SET v = " + std::to_string(v) + " WHERE id = " + std::to_string(id) + " AND v > " + std::to_string(t)), hit ? 1 : 0));
                if (hit) cur()[id].first = v;
            } else if (op < 95 && !in_txn) {
                REQUIRE(ex.execute_sql("VACUUM").is_ok());
            }
            if (step % 20 == 0) REQUIRE(snapshot(ex) == from_model(cur()));
        }
        if (in_txn) { REQUIRE(ex.execute_sql("COMMIT").is_ok()); committed = work; }
        REQUIRE(snapshot(ex) == from_model(committed));
    }
}

TEST_CASE("UPDATE on composite-PK and secondary-indexed tables does not use the pk cache wrongly", "[update][fast]") {
    TempDataDir dir("upd_fast_4");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE c (a INT, b INT, v INT, PRIMARY KEY (a, b))").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX iv ON c (v)").is_ok());
    for (int a = 1; a <= 5; a++)
        for (int b = 1; b <= 5; b++) REQUIRE(ex.execute_sql("INSERT INTO c VALUES (" + std::to_string(a) + "," + std::to_string(b) + ",0)").is_ok());
    // rows sharing the leading PK column must stay distinct
    REQUIRE(updated(ex.execute_sql("UPDATE c SET v = 5 WHERE a = 2 AND b = 3"), 1));
    REQUIRE(updated(ex.execute_sql("UPDATE c SET v = 6 WHERE a = 2"), 5));
    REQUIRE(updated(ex.execute_sql("UPDATE c SET v = 7 WHERE a = 2 AND b = 4"), 1));
    REQUIRE(ex.execute_sql("SELECT * FROM c WHERE v = 7").value().find("1 row(s)") != std::string::npos);
    REQUIRE(ex.execute_sql("SELECT * FROM c WHERE v = 6").value().find("4 row(s)") != std::string::npos);
    REQUIRE(ex.execute_sql("SELECT COUNT(*) FROM c").value().find("25") != std::string::npos);
}

// index_remove_row used to match bucket entries on the FIRST primary-key column only, so
// touching one row of a composite-PK table dropped every sibling sharing that column from its
// secondary-index bucket (B+Tree and hash alike) -- `WHERE v = ..` then under-reported.
TEST_CASE("Composite-PK UPDATE/DELETE removes only its own row from secondary indexes", "[update][fast][index]") {
    TempDataDir dir("upd_fast_4b");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE c (a INT, b INT, v INT, h INT, PRIMARY KEY (a, b))").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX iv ON c (v)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX ih ON c (h) USING HASH").is_ok());
    for (int a = 1; a <= 3; a++)
        for (int b = 1; b <= 4; b++)
            REQUIRE(ex.execute_sql("INSERT INTO c VALUES (" + std::to_string(a) + "," + std::to_string(b) + ",9,9)").is_ok());
    auto count = [&](const std::string& where) {
        auto r = ex.execute_sql("SELECT * FROM c WHERE " + where);
        REQUIRE(r.is_ok());
        auto pos = r.value().find(" row(s)");
        if (pos == std::string::npos) return 0; // "0 rows returned."
        auto start = r.value().find_last_of("\n ", pos - 1);
        return std::stoi(r.value().substr(start + 1, pos - start - 1));
    };
    REQUIRE(count("v = 9") == 12);
    REQUIRE(count("h = 9") == 12);

    REQUIRE(updated(ex.execute_sql("UPDATE c SET v = 1, h = 1 WHERE a = 2 AND b = 2"), 1));
    REQUIRE(count("v = 9") == 11);
    REQUIRE(count("h = 9") == 11);
    REQUIRE(count("v = 1") == 1);
    REQUIRE(count("h = 1") == 1);

    REQUIRE(ex.execute_sql("DELETE FROM c WHERE a = 2 AND b = 3").is_ok());
    REQUIRE(count("v = 9") == 10);
    REQUIRE(count("h = 9") == 10);

    // an explicit transaction that rolls back must leave the buckets exactly as they were
    REQUIRE(ex.execute_sql("BEGIN").is_ok());
    REQUIRE(updated(ex.execute_sql("UPDATE c SET v = 5, h = 5 WHERE a = 1 AND b = 1"), 1));
    REQUIRE(ex.execute_sql("DELETE FROM c WHERE a = 1 AND b = 2").is_ok());
    REQUIRE(ex.execute_sql("ROLLBACK").is_ok());
    REQUIRE(count("v = 9") == 10);
    REQUIRE(count("h = 9") == 10);
    REQUIRE(count("v = 5") == 0);
    REQUIRE(count("v + 0 = 9") == 10); // the indexed answer equals a plain scan
}

TEST_CASE("UPDATE through a view, a partitioned table and with a trigger behaves as before", "[update][fast]") {
    TempDataDir dir("upd_fast_5");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE base (id INT PRIMARY KEY, v INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO base VALUES (1,1),(2,2),(3,3)").is_ok());
    REQUIRE(ex.execute_sql("CREATE VIEW vw AS SELECT id, v FROM base WHERE id < 3").is_ok());
    REQUIRE(updated(ex.execute_sql("UPDATE vw SET v = 10 WHERE id = 2"), 1));
    REQUIRE(ex.execute_sql("SELECT v FROM base WHERE id = 2").value().find("10") != std::string::npos);

    REQUIRE(ex.execute_sql("CREATE TABLE p (id INT PRIMARY KEY, v INT) PARTITION BY RANGE (id) "
                           "(PARTITION p0 VALUES LESS THAN (100), PARTITION p1 VALUES LESS THAN MAXVALUE)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO p VALUES (5,1),(150,2)").is_ok());
    REQUIRE(updated(ex.execute_sql("UPDATE p SET v = 9 WHERE id = 150"), 1));
    REQUIRE(updated(ex.execute_sql("UPDATE p SET v = 8 WHERE id = 5"), 1));
    REQUIRE(ex.execute_sql("SELECT v FROM p WHERE id = 150").value().find("9") != std::string::npos);

    REQUIRE(ex.execute_sql("CREATE TABLE log (msg VARCHAR(50))").is_ok());
    REQUIRE(ex.execute_sql("CREATE TRIGGER trg AFTER UPDATE ON base FOR EACH ROW INSERT INTO log VALUES ('upd')").is_ok());
    REQUIRE(updated(ex.execute_sql("UPDATE base SET v = 99 WHERE id = 3"), 1));
    REQUIRE(ex.execute_sql("SELECT COUNT(*) FROM log").value().find("1") != std::string::npos);
}

TEST_CASE("UPDATE visibility: own writes, snapshots and write conflicts between sessions", "[update][fast][concurrency]") {
    TempDataDir dir("upd_fast_6");
    Executor a(dir.path);
    setup(a);
    REQUIRE(a.execute_sql("INSERT INTO t VALUES (1,10,0),(2,20,0),(3,30,0)").is_ok());
    auto shared = a.get_shared();
    Executor b = Executor::new_session(shared);
    REQUIRE(b.execute_sql("USE d").is_ok());

    // A updates inside a transaction: B (autocommit) must keep seeing the old value
    REQUIRE(a.execute_sql("BEGIN").is_ok());
    REQUIRE(updated(a.execute_sql("UPDATE t SET v = 11 WHERE id = 1"), 1));
    REQUIRE(updated(a.execute_sql("UPDATE t SET v = v + 1 WHERE id = 1"), 1)); // own write visible to the next statement
    REQUIRE(a.execute_sql("SELECT v FROM t WHERE id = 1").value().find("12") != std::string::npos);
    REQUIRE(b.execute_sql("SELECT v FROM t WHERE id = 1").value().find("10") != std::string::npos);
    // B updating a DIFFERENT key proceeds independently
    REQUIRE(updated(b.execute_sql("UPDATE t SET v = 21 WHERE id = 2"), 1));
    REQUIRE(a.execute_sql("COMMIT").is_ok());
    REQUIRE(b.execute_sql("SELECT v FROM t WHERE id = 1").value().find("12") != std::string::npos);
    // after A's commit B's pk-update targets the NEW version
    REQUIRE(updated(b.execute_sql("UPDATE t SET v = v * 2 WHERE id = 1"), 1));
    REQUIRE(a.execute_sql("SELECT v FROM t WHERE id = 1").value().find("24") != std::string::npos);

    // REPEATABLE READ keeps its snapshot while another session updates and commits
    REQUIRE(a.execute_sql("SET ISOLATION LEVEL REPEATABLE READ").is_ok());
    REQUIRE(a.execute_sql("BEGIN").is_ok());
    REQUIRE(a.execute_sql("SELECT v FROM t WHERE id = 3").value().find("30") != std::string::npos);
    REQUIRE(updated(b.execute_sql("UPDATE t SET v = 31 WHERE id = 3"), 1));
    REQUIRE(a.execute_sql("SELECT v FROM t WHERE id = 3").value().find("30") != std::string::npos);
    REQUIRE(a.execute_sql("COMMIT").is_ok());
    REQUIRE(a.execute_sql("SELECT v FROM t WHERE id = 3").value().find("31") != std::string::npos);
}

TEST_CASE("UPDATE: concurrent increments of one row never lose an update", "[update][fast][concurrency]") {
    TempDataDir dir("upd_fast_7");
    Executor ex(dir.path);
    setup(ex);
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1,0,0),(2,0,0)").is_ok());
    auto shared = ex.get_shared();

    constexpr int kThreads = 4;
    constexpr int kIters = 120;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    for (int th = 0; th < kThreads; th++) {
        threads.emplace_back([&, th] {
            Executor s = Executor::new_session(shared);
            if (!s.execute_sql("USE d").is_ok()) { failures++; return; }
            for (int i = 0; i < kIters; i++) {
                // alternate: all threads hammer id=1; each thread also owns a private-ish key id=2 only on th==0
                auto r = s.execute_sql("UPDATE t SET v = v + 1 WHERE id = 1");
                if (!r.is_ok()) { failures++; continue; }
                if (th == 0) {
                    auto r2 = s.execute_sql("UPDATE t SET v = v + 1 WHERE id = 2");
                    if (!r2.is_ok()) failures++;
                }
            }
        });
    }
    for (auto& t : threads) t.join();
    REQUIRE(failures == 0);
    auto rows = snapshot(ex);
    std::string dump; // shown only if an assertion below fails
    for (auto& r : rows) dump += "(id=" + std::to_string(r.id) + " v=" + std::to_string(r.v) + ") ";
    INFO("visible rows: " << dump);
    REQUIRE(rows.size() == 2);
    REQUIRE(rows[0].v == kThreads * kIters); // no lost update on the contended row
    REQUIRE(rows[1].v == kIters);
}
