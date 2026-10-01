#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// UPDATE and DELETE find their target rows through the table's indexes (planner access path ->
// candidate rows -> verified against the real rows, see executor_dml_index.cpp) instead of
// walking every row version. The plain scan is kept as the fallback, and it is also the
// ORACLE here: the same statements run once with index paths forced on and once with them
// forced off, and everything has to come out identical.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

constexpr std::size_t ALWAYS = 0;
constexpr std::size_t NEVER = std::numeric_limits<std::size_t>::max();

struct IndexMode {
    std::size_t prev;
    explicit IndexMode(std::size_t v) : prev(Executor::dml_index_min_rows.load()) { Executor::dml_index_min_rows = v; }
    ~IndexMode() { Executor::dml_index_min_rows = prev; }
};

std::uint64_t hits() { return Executor::dml_index_hits.load(); }

StringResult run(Executor& ex, std::size_t mode, const std::string& sql) {
    IndexMode m(mode);
    return ex.execute_sql(sql);
}

std::string ok_text(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql);
    REQUIRE(r.is_ok());
    return r.value();
}

bool changed(const StringResult& r, int n, const char* verb) {
    return r.is_ok() && r.value().find(std::to_string(n) + " row(s) " + verb) != std::string::npos;
}

// The number a "SELECT COUNT(*) <from_where>" query returns (the 2nd '|' line of the table output).
int count_of(Executor& ex, const std::string& from_where) {
    auto text = ok_text(ex, "SELECT COUNT(*) " + from_where);
    std::istringstream in(text);
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars == 2) {
            int v = 0;
            bool any = false;
            for (char c : line) {
                if (c >= '0' && c <= '9') {
                    v = v * 10 + (c - '0');
                    any = true;
                }
            }
            REQUIRE(any);
            return v;
        }
    }
    FAIL("no data row in: " << text);
    return -1;
}

// RETURNING rows come back in physical order, and the two engines' physical orders legitimately
// differ after earlier deletes (a pk delete is a swap-remove) -- so compare them as sets of lines.
std::string sorted_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (auto& l : lines) out += l + '\n';
    return out;
}

std::string snap(Executor& ex, const std::string& table) { return ok_text(ex, "SELECT * FROM " + table + " ORDER BY id"); }

void open_db(Executor& ex) {
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
}

// id INT PK, a INT, b VARCHAR, c DECIMAL, d INT -- indexes on a, b (B+Tree or hash) and c.
void make_table(Executor& ex, const std::string& name, bool hash_b) {
    REQUIRE(ex.execute_sql("CREATE TABLE " + name + " (id INT PRIMARY KEY, a INT, b VARCHAR(20), c DECIMAL(10,2), d INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX " + name + "_ia ON " + name + " (a)").is_ok());
    if (hash_b) {
        REQUIRE(ex.execute_sql("CREATE INDEX " + name + "_hb ON " + name + " (b) USING HASH").is_ok());
    } else {
        REQUIRE(ex.execute_sql("CREATE INDEX " + name + "_ib ON " + name + " (b)").is_ok());
    }
    REQUIRE(ex.execute_sql("CREATE INDEX " + name + "_ic ON " + name + " (c)").is_ok());
}

// The values a column holds right now, read with a full scan (no WHERE -> no index involved).
std::vector<std::string> column_values(Executor& ex, const std::string& table, const std::string& col) {
    std::vector<std::string> out;
    std::istringstream in(ok_text(ex, "SELECT " + col + " FROM " + table));
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars <= 1) continue; // header
        std::string v = line.substr(1, line.rfind('|') - 1);
        v.erase(0, v.find_first_not_of(' '));
        v.erase(v.find_last_not_of(' ') + 1);
        out.push_back(v);
    }
    return out;
}

// Independent of the scan-vs-index DML comparison (both of whose engines share the index maintenance code):
// within ONE engine, what a lookup through an index returns must equal what a scan returns. Appending
// `AND d >= 0` (true for every row) keeps the planner from choosing an index for the same predicate.
void check_indexes(Executor& ex, const std::string& table, std::mt19937& rng) {
    for (const std::string col : {"a", "b", "c"}) {
        auto vals = column_values(ex, table, col);
        if (vals.empty()) continue;
        for (int pick = 0; pick < 4; pick++) {
            std::string v = vals[rng() % vals.size()];
            std::string lit = col == "b" ? "'" + v + "'" : v;
            for (const std::string op : {"=", ">=", "<"}) {
                std::string pred = col + " " + op + " " + lit;
                INFO(table << ": " << pred);
                REQUIRE(ok_text(ex, "SELECT id FROM " + table + " WHERE " + pred + " ORDER BY id") ==
                        ok_text(ex, "SELECT id FROM " + table + " WHERE " + pred + " AND d >= 0 ORDER BY id"));
            }
        }
    }
}

struct Gen {
    std::mt19937 rng;
    explicit Gen(unsigned seed) : rng(seed) {}
    int n(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }
    std::string num(int lo, int hi) { return std::to_string(n(lo, hi)); }
    // text values include numeric-looking ones in several spellings on purpose
    std::string bval() {
        static const char* v[] = {"x0", "x1", "x2", "x3", "x4", "x5", "07", "7", "7.0", "abc", "ab", "abd", "x"};
        return v[n(0, 12)];
    }
    // decimals in several spellings of the same number
    std::string cval() {
        static const char* v[] = {"7", "7.00", "7.0", "3.5", "12.25", "0", "100", "7.5", "8"};
        return v[n(0, 8)];
    }
    std::string pred() {
        switch (n(0, 27)) {
            case 0: return "a = " + num(0, 29);
            case 1: return "a > " + num(0, 29);
            case 2: return "a >= " + num(0, 29);
            case 3: return "a < " + num(0, 29);
            case 4: return "a <= " + num(0, 29);
            case 5: { int lo = n(0, 25); return "a BETWEEN " + std::to_string(lo) + " AND " + std::to_string(lo + n(0, 6)); }
            case 6: return "b = '" + bval() + "'";
            case 7: return "b = " + num(0, 8); // numeric literal against a text column
            case 8: return "b LIKE 'x%'";
            case 9: return "b LIKE 'ab%'";
            case 10: return "b > 'x3'";
            case 11: return "c = " + cval();
            case 12: return "c >= " + cval();
            case 13: return "c <= " + cval();
            case 14: return "c BETWEEN 3 AND " + num(4, 13);
            case 15: return "c > " + cval();
            case 16: return "id = " + num(1, 150);
            case 17: return "id > " + num(1, 150);
            case 18: return "id <= " + num(1, 150);
            case 19: { int lo = n(1, 140); return "id BETWEEN " + std::to_string(lo) + " AND " + std::to_string(lo + n(0, 12)); }
            case 20: return "d = " + num(0, 9); // no index on d
            case 21: return "a = " + num(0, 29) + " AND b = '" + bval() + "'";
            case 22: return "a >= " + num(0, 29) + " AND d < " + num(0, 9);
            case 23: return "b = '" + bval() + "' AND c > 3";
            case 24: return "a = " + num(0, 29) + " OR b = '" + bval() + "'";
            case 25: return "NOT a = " + num(0, 29);
            case 26: return "a IN (" + num(0, 29) + ", " + num(0, 29) + ")";
            default: return "a = " + num(0, 29) + " AND c = " + cval();
        }
    }
    std::string assign() {
        switch (n(0, 7)) {
            case 0: return "a = a + 1";
            case 1: return "b = '" + bval() + "'";
            case 2: return "c = " + cval();
            case 3: return "d = d + 5";
            case 4: return "a = " + num(0, 29) + ", b = '" + bval() + "'";
            case 5: return "d = " + num(0, 9);
            case 6: return "id = id + 1000"; // pk change; may collide -> an error on both sides
            default: return "a = a + 2, c = " + cval();
        }
    }
    std::string insert_row(const std::string& t) {
        return "INSERT INTO " + t + " VALUES (" + num(1, 400) + ", " + num(0, 29) + ", '" + bval() + "', " + cval() + ", " + num(0, 9) + ")";
    }
};

void load_rows(Executor& ex, const std::string& table, int count, Gen& g) {
    for (int i = 1; i <= count; i++) {
        std::string sql = "INSERT INTO " + table + " VALUES (" + std::to_string(i) + ", " + g.num(0, 29) + ", '" + g.bval() + "', " + g.cval() +
                          ", " + g.num(0, 9) + ")";
        REQUIRE(ex.execute_sql(sql).is_ok());
    }
}
} // namespace

TEST_CASE("DML index: randomized differential test, index paths vs plain scan", "[dml_index][fuzz]") {
    // 3 seeds by default; RUSQL_FUZZ_SEEDS=60 runs a much longer campaign
    unsigned seed_count = 3;
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 0; // RUSQL_FUZZ_START=44 replays a failing seed on its own
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    for (unsigned k = seed_start; k < seed_start + seed_count; k++) {
        const unsigned seed = 11u + k * 191u;
        INFO("seed " << seed);
        TempDataDir da("dml_idx_fz_a"), db("dml_idx_fz_b");
        Executor idx(da.path), scan(db.path);
        open_db(idx);
        open_db(scan);
        const std::vector<std::pair<std::string, bool>> tables = {{"t", false}, {"h", true}};
        Gen setup_gen(seed);
        for (auto& [name, hash_b] : tables) {
            make_table(idx, name, hash_b);
            make_table(scan, name, hash_b);
            // identical data on both sides
            Gen g1(seed + 1), g2(seed + 1);
            load_rows(idx, name, 120, g1);
            load_rows(scan, name, 120, g2);
        }

        Gen g(seed);
        std::mt19937 check_rng(seed ^ 0x5bd1e995u); // separate stream: the statement sequence stays reproducible
        bool in_txn = false;
        const bool trace = std::getenv("RUSQL_FUZZ_TRACE") != nullptr; // prints every statement (stderr)
        const std::uint64_t hits_before = hits();
        for (int step = 0; step < 1200; step++) {
            const std::string& tbl = tables[static_cast<std::size_t>(g.n(0, 1))].first;
            std::string sql;
            bool mutating = true;
            int kind = g.n(0, 99);
            if (kind < 30) {
                sql = "UPDATE " + tbl + " SET " + g.assign() + " WHERE " + g.pred();
            } else if (kind < 52) {
                sql = "DELETE FROM " + tbl + " WHERE " + g.pred();
            } else if (kind < 66) {
                sql = g.insert_row(tbl);
            } else if (kind < 72) {
                sql = in_txn ? (g.n(0, 2) == 0 ? "ROLLBACK" : "COMMIT") : "BEGIN";
                in_txn = !in_txn;
            } else if (kind < 76) {
                if (in_txn) continue;
                sql = "VACUUM " + tbl; // shifts physical positions under the cache
            } else if (kind < 80) {
                sql = "UPDATE " + tbl + " SET d = d + 1 WHERE " + g.pred() + " RETURNING id, a";
            } else if (kind < 84) {
                sql = "DELETE FROM " + tbl + " WHERE " + g.pred() + " RETURNING id, b";
            } else {
                mutating = false;
                sql = "SELECT id FROM " + tbl + " WHERE " + g.pred() + " ORDER BY id";
            }
            INFO("step " << step << ": " << sql);
            if (const char* dump = std::getenv("RUSQL_FUZZ_DUMP_STEP"); dump && step == std::atoi(dump)) {
                // debugging aid: both engines' table as it is just BEFORE this step
                std::cerr << "=== before step " << step << " INDEX engine:" << std::endl << snap(idx, tbl) << std::endl;
                std::cerr << "=== before step " << step << " SCAN engine:" << std::endl << snap(scan, tbl) << std::endl;
            }
            auto r1 = run(idx, ALWAYS, sql);
            auto r2 = run(scan, NEVER, sql);
            if (trace) {
                std::string first = r1.is_ok() ? r1.value() : "ERR " + r1.error();
                std::cerr << "step " << step << ": " << sql << "  -> " << first.substr(0, first.find(char(10))) << std::endl;
            }
            REQUIRE(r1.is_ok() == r2.is_ok());
            if (r1.is_ok()) {
                if (sql.find("RETURNING") != std::string::npos) {
                    REQUIRE(sorted_lines(r1.value()) == sorted_lines(r2.value()));
                } else {
                    REQUIRE(r1.value() == r2.value());
                }
            }
            if (mutating && step % 3 == 0) {
                for (auto& [name, _] : tables) REQUIRE(snap(idx, name) == snap(scan, name));
            }
            if (mutating && !in_txn && step % 9 == 0) { // (inside a transaction SELECTs scan anyway)
                for (auto& [name, _] : tables) {
                    check_indexes(idx, name, check_rng);
                    check_indexes(scan, name, check_rng);
                }
            }
        }
        if (in_txn) {
            REQUIRE(idx.execute_sql("COMMIT").is_ok());
            REQUIRE(scan.execute_sql("COMMIT").is_ok());
        }
        for (auto& [name, _] : tables) REQUIRE(snap(idx, name) == snap(scan, name));
        for (auto& [name, _] : tables) {
            check_indexes(idx, name, check_rng);
            check_indexes(scan, name, check_rng);
        }
        // the index paths really ran (otherwise this would only compare the scan with itself)
        REQUIRE(hits() - hits_before > 150);
    }
}

TEST_CASE("DML index: numerically equal spellings are found, exactly like the scan finds them", "[dml_index]") {
    IndexMode always(ALWAYS);
    TempDataDir dir("dml_idx_num");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE d (id INT PRIMARY KEY, price DECIMAL(10,2), code VARCHAR(10), qty INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX ip ON d (price)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX ic ON d (code)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX iq ON d (qty)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO d VALUES (1, 7, '07', 7), (2, 7.00, '7', 7), (3, 7.5, '7.0', 8), (4, 6.99, 'x', 6)").is_ok());

    auto h = hits();
    // `price = 7` must reach "7" AND "7.00"; the old index SELECT found only the exact text "7"
    REQUIRE(changed(ex.execute_sql("UPDATE d SET qty = 99 WHERE price = 7"), 2, "updated"));
    REQUIRE(hits() == h + 1);
    REQUIRE(count_of(ex, "FROM d WHERE qty + 0 = 99") == 2);

    // text column, numeric literal: '07', '7' and '7.0' are all equal to 7
    h = hits();
    REQUIRE(changed(ex.execute_sql("UPDATE d SET qty = 5 WHERE code = 7"), 3, "updated"));
    REQUIRE(hits() == h + 1);

    // inclusive bounds reach every spelling of the boundary value
    REQUIRE(ex.execute_sql("UPDATE d SET qty = 7 WHERE id <= 2").is_ok());
    h = hits();
    REQUIRE(changed(ex.execute_sql("UPDATE d SET price = 70 WHERE qty >= 7"), 2, "updated"));
    REQUIRE(hits() == h + 1);

    // a hash index cannot express "equal as a number": such a literal must not use it, but must still be right
    REQUIRE(ex.execute_sql("CREATE TABLE hx (id INT PRIMARY KEY, code VARCHAR(10))").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX hxc ON hx (code) USING HASH").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO hx VALUES (1, '07'), (2, '7'), (3, '7.0'), (4, 'q')").is_ok());
    h = hits();
    REQUIRE(changed(ex.execute_sql("DELETE FROM hx WHERE code = 7"), 3, "deleted"));
    REQUIRE(hits() == h);
    h = hits();
    REQUIRE(changed(ex.execute_sql("DELETE FROM hx WHERE code = 'q'"), 1, "deleted")); // exact text: the hash index is fine
    REQUIRE(hits() == h + 1);
}

TEST_CASE("DML index: which statements use an index and which fall back to the scan", "[dml_index]") {
    TempDataDir dir("dml_idx_fallback");
    Executor a(dir.path);
    open_db(a);
    make_table(a, "t", false);
    Gen g(5);
    load_rows(a, "t", 200, g);
    {
        IndexMode always(ALWAYS);
        // plain indexed predicates
        auto h = hits();
        REQUIRE(a.execute_sql("UPDATE t SET d = 1 WHERE a = 3").is_ok());
        REQUIRE(hits() == h + 1);
        // one indexable leaf of an AND is enough; the whole condition is still enforced
        h = hits();
        const int expect = count_of(a, "FROM t WHERE a + 0 = 4 AND d >= 0");
        REQUIRE(changed(a.execute_sql("UPDATE t SET d = 2 WHERE a = 4 AND d >= 0"), expect, "updated"));
        REQUIRE(hits() == h + 1);
        // OR, NOT, arithmetic and IN cannot use an index
        h = hits();
        REQUIRE(a.execute_sql("UPDATE t SET d = 3 WHERE a = 5 OR a = 6").is_ok());
        REQUIRE(a.execute_sql("UPDATE t SET d = 3 WHERE NOT a = 5").is_ok());
        REQUIRE(a.execute_sql("UPDATE t SET d = 3 WHERE a + 0 = 7").is_ok());
        REQUIRE(a.execute_sql("DELETE FROM t WHERE d + 0 = 12345").is_ok());
        REQUIRE(hits() == h);
        // a subquery condition keeps the old path
        h = hits();
        REQUIRE(a.execute_sql("DELETE FROM t WHERE a IN (SELECT a FROM t WHERE d > 1000)").is_ok());
        REQUIRE(a.execute_sql("UPDATE t SET d = 4 WHERE a IN (SELECT a FROM t WHERE d > 1000)").is_ok());
        REQUIRE(hits() == h);
        // a predicate matching a large share of the table is cheaper to scan than to parse out of JSON
        h = hits();
        REQUIRE(changed(a.execute_sql("UPDATE t SET d = 9 WHERE a >= 0"), 200, "updated"));
        REQUIRE(hits() == h);
    }
    {
        // below the size threshold a scan is used, above it the index
        IndexMode threshold(64);
        make_table(a, "small", false);
        Gen g2(6);
        load_rows(a, "small", 10, g2);
        auto h = hits();
        REQUIRE(a.execute_sql("UPDATE small SET d = 1 WHERE a = 3").is_ok());
        REQUIRE(hits() == h);
        h = hits();
        REQUIRE(a.execute_sql("UPDATE t SET d = 1 WHERE a = 3").is_ok());
        REQUIRE(hits() == h + 1);
    }
    {
        IndexMode always(ALWAYS);
        // composite and missing primary keys have no pk -> position cache: scan, but correct
        REQUIRE(a.execute_sql("CREATE TABLE cp (x INT, y INT, v INT, PRIMARY KEY (x, y))").is_ok());
        REQUIRE(a.execute_sql("CREATE INDEX icp ON cp (v)").is_ok());
        REQUIRE(a.execute_sql("CREATE TABLE np (p INT, q INT)").is_ok());
        REQUIRE(a.execute_sql("CREATE INDEX inp ON np (q)").is_ok());
        for (int i = 0; i < 30; i++) {
            REQUIRE(a.execute_sql("INSERT INTO cp VALUES (" + std::to_string(i % 5) + ", " + std::to_string(i) + ", " + std::to_string(i % 3) + ")").is_ok());
            REQUIRE(a.execute_sql("INSERT INTO np VALUES (" + std::to_string(i) + ", " + std::to_string(i % 3) + ")").is_ok());
        }
        auto h = hits();
        REQUIRE(changed(a.execute_sql("UPDATE cp SET v = 9 WHERE v = 1"), 10, "updated"));
        REQUIRE(changed(a.execute_sql("DELETE FROM cp WHERE v = 2"), 10, "deleted"));
        REQUIRE(changed(a.execute_sql("UPDATE np SET q = 9 WHERE q = 1"), 10, "updated"));
        REQUIRE(changed(a.execute_sql("DELETE FROM np WHERE q = 2"), 10, "deleted"));
        REQUIRE(hits() == h);
    }
}

TEST_CASE("DML index: another open transaction forces the scan until it ends", "[dml_index][concurrency]") {
    IndexMode always(ALWAYS);
    TempDataDir dir("dml_idx_others");
    Executor a(dir.path);
    open_db(a);
    make_table(a, "t", false);
    Gen g(7);
    load_rows(a, "t", 100, g);
    // rows with a = 3 and a = 7 are disjoint, so the two sessions never touch the same row
    REQUIRE(a.execute_sql("UPDATE t SET a = 3 WHERE id <= 10").is_ok());
    REQUIRE(a.execute_sql("UPDATE t SET a = 7 WHERE id > 10 AND id <= 20").is_ok());
    REQUIRE(a.execute_sql("UPDATE t SET a = 20 WHERE id > 20").is_ok()); // nothing else is in group 3 or 7
    Executor b = Executor::new_session(a.get_shared());
    REQUIRE(b.execute_sql("USE d").is_ok());

    REQUIRE(b.execute_sql("BEGIN").is_ok());
    REQUIRE(changed(b.execute_sql("UPDATE t SET d = 100 WHERE a = 7"), 10, "updated")); // B's uncommitted versions

    auto h = hits();
    REQUIRE(changed(a.execute_sql("UPDATE t SET d = 5 WHERE a = 3"), 10, "updated"));
    REQUIRE(changed(a.execute_sql("DELETE FROM t WHERE a = 3 AND id > 5"), 5, "deleted"));
    REQUIRE(hits() == h); // an open transaction elsewhere: no index path, and the answers are still right

    REQUIRE(b.execute_sql("COMMIT").is_ok());
    h = hits();
    REQUIRE(changed(a.execute_sql("UPDATE t SET d = 6 WHERE a = 7"), 10, "updated"));
    REQUIRE(hits() == h + 1);
    // B's committed versions are what A now updated
    REQUIRE(count_of(a, "FROM t WHERE a = 7 AND d = 6") == 10);

    // and the same transaction's own earlier writes are seen by its later indexed statements
    REQUIRE(a.execute_sql("BEGIN").is_ok());
    REQUIRE(changed(a.execute_sql("UPDATE t SET d = 50 WHERE a = 3"), 5, "updated"));
    h = hits();
    REQUIRE(changed(a.execute_sql("UPDATE t SET d = d + 1 WHERE a = 3"), 5, "updated"));
    REQUIRE(hits() == h + 1);
    REQUIRE(count_of(a, "FROM t WHERE a = 3 AND d = 51") == 5);
    REQUIRE(changed(a.execute_sql("DELETE FROM t WHERE a = 3"), 5, "deleted"));
    REQUIRE(a.execute_sql("ROLLBACK").is_ok());
    REQUIRE(count_of(a, "FROM t WHERE a = 3 AND d = 5") == 5); // back as before the txn
    REQUIRE(changed(a.execute_sql("UPDATE t SET d = 8 WHERE a = 3"), 5, "updated"));
}

TEST_CASE("DML index: a cold position cache (restart, after a scan delete) heals instead of failing", "[dml_index]") {
    IndexMode always(ALWAYS);
    TempDataDir dir("dml_idx_cold");
    {
        Executor ex(dir.path);
        open_db(ex);
        make_table(ex, "t", false);
        for (int i = 1; i <= 150; i++) {
            REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i % 10) + ", 'x" + std::to_string(i % 6) + "', 1, 0)").is_ok());
        }
    } // "crash": nothing is flushed at shutdown, the next boot recovers from disk with an empty row_pk_pos

    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    auto h = hits();
    // fast-path DELETE holds the table exclusively, so it rebuilds the cache itself and retries
    REQUIRE(changed(ex.execute_sql("DELETE FROM t WHERE a = 3"), 15, "deleted"));
    REQUIRE(hits() == h + 1);
    h = hits();
    REQUIRE(changed(ex.execute_sql("UPDATE t SET d = 1 WHERE a = 4"), 15, "updated"));
    REQUIRE(hits() == h + 1); // the swap-removes kept the cache in step

    // a scan DELETE clears the cache; the next UPDATE scans once (and heals), the one after uses the index
    REQUIRE(ex.execute_sql("DELETE FROM t WHERE d + 0 = 12345").is_ok());
    h = hits();
    REQUIRE(changed(ex.execute_sql("UPDATE t SET d = 2 WHERE a = 5"), 15, "updated"));
    REQUIRE(hits() == h);
    REQUIRE(changed(ex.execute_sql("UPDATE t SET d = 3 WHERE a = 6"), 15, "updated"));
    REQUIRE(hits() == h + 1);

    // VACUUM moves rows: positions in the cache go stale, which must be noticed, not trusted
    REQUIRE(ex.execute_sql("VACUUM t").is_ok());
    REQUIRE(changed(ex.execute_sql("UPDATE t SET d = 4 WHERE a = 7"), 15, "updated"));
    REQUIRE(changed(ex.execute_sql("DELETE FROM t WHERE a = 8"), 15, "deleted"));
    REQUIRE(count_of(ex, "FROM t") == 120); // 150 rows, minus the 15 with a = 3 and the 15 with a = 8
    REQUIRE(count_of(ex, "FROM t WHERE a = 7 AND d = 4") == 15);
}

TEST_CASE("DML index: DELETE variants (range, RETURNING order, transactions, FK parents)", "[dml_index]") {
    IndexMode always(ALWAYS);
    TempDataDir dir("dml_idx_del");
    Executor ex(dir.path);
    open_db(ex);
    make_table(ex, "t", false);
    for (int i = 1; i <= 120; i++) {
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i % 6) + ", 'x', 1, 0)").is_ok());
    }

    // pk range through the planner's PkRange path; the pk cache stays consistent afterwards
    auto h = hits();
    REQUIRE(changed(ex.execute_sql("DELETE FROM t WHERE id > 100"), 20, "deleted"));
    REQUIRE(hits() == h + 1);
    REQUIRE(changed(ex.execute_sql("UPDATE t SET d = 7 WHERE id = 50"), 1, "updated"));
    REQUIRE(count_of(ex, "FROM t WHERE id = 50 AND d = 7") == 1);

    // RETURNING comes back in table order, as the scan always gave it
    auto r = ok_text(ex, "DELETE FROM t WHERE a = 1 AND id < 40 RETURNING id");
    REQUIRE(r.find("| 1 ") < r.find("| 7 "));
    REQUIRE(r.find("| 7 ") < r.find("| 13 "));
    REQUIRE(r.find("| 13 ") < r.find("| 19 "));

    // explicit transaction: soft delete through the index, then ROLLBACK brings everything back
    REQUIRE(ex.execute_sql("BEGIN").is_ok());
    h = hits();
    REQUIRE(changed(ex.execute_sql("DELETE FROM t WHERE a = 2"), 17, "deleted"));
    REQUIRE(hits() >= h + 1);
    REQUIRE(count_of(ex, "FROM t WHERE a = 2") == 0);
    REQUIRE(ex.execute_sql("ROLLBACK").is_ok());
    REQUIRE(changed(ex.execute_sql("UPDATE t SET d = 1 WHERE a = 2"), 17, "updated"));

    // a table that foreign keys point at: RESTRICT still protects it, CASCADE still cascades
    REQUIRE(ex.execute_sql("CREATE TABLE parent (id INT PRIMARY KEY, code VARCHAR(10))").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX pc ON parent (code)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE kid (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES parent(id) ON DELETE CASCADE)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE guard (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES parent(id) ON DELETE RESTRICT)").is_ok());
    for (int i = 1; i <= 60; i++) {
        REQUIRE(ex.execute_sql("INSERT INTO parent VALUES (" + std::to_string(i) + ", 'k" + std::to_string(i % 5) + "')").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO kid VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")").is_ok());
    }
    REQUIRE(ex.execute_sql("INSERT INTO guard VALUES (1, 4)").is_ok()); // parent 4 has code 'k4'
    auto before = hits();
    auto blocked = ex.execute_sql("DELETE FROM parent WHERE code = 'k4'");
    REQUIRE(blocked.is_err());
    REQUIRE(count_of(ex, "FROM parent WHERE code = 'k4'") == 12);
    REQUIRE(ex.execute_sql("DELETE FROM guard WHERE id = 1").is_ok());
    REQUIRE(changed(ex.execute_sql("DELETE FROM parent WHERE code = 'k0'"), 12, "deleted")); // 12 rows: the physical-erase-by-position branch
    REQUIRE(count_of(ex, "FROM kid") == 48); // children followed
    REQUIRE(hits() > before);
    // a delete matching more rows than the by-position limit still works (single remove_if pass)
    for (int i = 0; i < 30; i++) {
        REQUIRE(ex.execute_sql("INSERT INTO parent VALUES (" + std::to_string(200 + i) + ", 'big')").is_ok());
    }
    REQUIRE(changed(ex.execute_sql("DELETE FROM parent WHERE code = 'big'"), 30, "deleted"));
    REQUIRE(count_of(ex, "FROM parent WHERE code = 'big'") == 0);
    REQUIRE(count_of(ex, "FROM parent") == 48); // 60 minus the 12 'k0' parents; the 12 'k4' ones were protected
}

TEST_CASE("DML index: concurrent index-driven UPDATE/DELETE/INSERT never lose or invent a row", "[dml_index][concurrency]") {
    IndexMode always(ALWAYS);
    TempDataDir dir("dml_idx_conc");
    Executor ex(dir.path);
    open_db(ex);
    make_table(ex, "t", false);
    for (int i = 1; i <= 200; i++) {
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i % 8) + ", 'x', 1, 0)").is_ok());
    }
    auto shared = ex.get_shared();
    constexpr int kIters = 70;
    std::atomic<int> failures{0};
    std::vector<std::thread> threads;
    // groups a = 0..3: each thread owns one group and increments d on all of its rows, kIters times
    for (int th = 0; th < 4; th++) {
        threads.emplace_back([&, th] {
            Executor s = Executor::new_session(shared);
            if (!s.execute_sql("USE d").is_ok()) { failures++; return; }
            for (int i = 0; i < kIters; i++) {
                auto r = s.execute_sql("UPDATE t SET d = d + 1 WHERE a = " + std::to_string(th));
                if (!changed(r, 25, "updated")) failures++;
            }
        });
    }
    // meanwhile the table's shape keeps changing: groups a = 4..7 are inserted into and deleted from
    threads.emplace_back([&] {
        Executor s = Executor::new_session(shared);
        if (!s.execute_sql("USE d").is_ok()) { failures++; return; }
        for (int i = 0; i < kIters; i++) {
            if (!s.execute_sql("INSERT INTO t VALUES (" + std::to_string(1000 + i) + ", " + std::to_string(4 + i % 4) + ", 'n', 1, 0)").is_ok()) failures++;
            if (i % 5 == 4 && !s.execute_sql("DELETE FROM t WHERE a = 6").is_ok()) failures++;
            if (i % 7 == 6 && !s.execute_sql("UPDATE t SET d = d + 1 WHERE a = 5").is_ok()) failures++;
        }
    });
    for (auto& t : threads) t.join();
    REQUIRE(failures == 0);
    // no lost update on any row of the four owned groups
    REQUIRE(count_of(ex, "FROM t WHERE a <= 3 AND d = " + std::to_string(kIters)) == 100);
    REQUIRE(count_of(ex, "FROM t WHERE a <= 3") == 100);
    // the index answers agree with a full scan
    for (int grp = 0; grp < 8; grp++) {
        REQUIRE(count_of(ex, "FROM t WHERE a = " + std::to_string(grp)) == count_of(ex, "FROM t WHERE a + 0 = " + std::to_string(grp)));
    }
}

// Found by the differential test above: UPDATE never checked PRIMARY KEY / UNIQUE, so it could make two live rows
// share a key (breaking every one-row-per-pk assumption), and a multi-row UPDATE that failed a CHECK on a later
// row had already thrown away the earlier rows.
TEST_CASE("UPDATE rejects duplicate PRIMARY KEY / UNIQUE values and never half-applies", "[update][integrity][dml_index]") {
    for (std::size_t mode : {ALWAYS, NEVER}) { // the index paths and the scan must agree
        IndexMode m(mode);
        TempDataDir dir("upd_integrity");
        Executor ex(dir.path);
        open_db(ex);
        REQUIRE(ex.execute_sql("CREATE TABLE u (id INT PRIMARY KEY, email VARCHAR(20) UNIQUE, v INT)").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO u VALUES (1, 'a@x', 0), (2, 'b@x', 0), (3, 'c@x', 0)").is_ok());

        auto onto_existing = ex.execute_sql("UPDATE u SET id = 2 WHERE id = 1");
        REQUIRE(onto_existing.is_err());
        REQUIRE(onto_existing.error().find("Duplicate value '2' for column 'id'") != std::string::npos);
        REQUIRE(count_of(ex, "FROM u WHERE id + 0 = 2") == 1);
        REQUIRE(count_of(ex, "FROM u WHERE id + 0 = 1") == 1); // the failed statement left row 1 alone

        REQUIRE(ex.execute_sql("UPDATE u SET email = 'b@x' WHERE id = 3").is_err());
        REQUIRE(changed(ex.execute_sql("UPDATE u SET email = 'c@x' WHERE id = 3"), 1, "updated")); // unchanged value: fine
        REQUIRE(ex.execute_sql("UPDATE u SET id = 9 WHERE id >= 2").is_err()); // two rows, one new key
        REQUIRE(count_of(ex, "FROM u") == 3);

        // moving into slots that the same statement vacates is fine
        REQUIRE(changed(ex.execute_sql("UPDATE u SET id = id + 1 WHERE id >= 2"), 2, "updated"));
        REQUIRE(count_of(ex, "FROM u WHERE id + 0 = 1") == 1);
        REQUIRE(count_of(ex, "FROM u WHERE id + 0 = 3") == 1);
        REQUIRE(count_of(ex, "FROM u WHERE id + 0 = 4") == 1);
        REQUIRE(count_of(ex, "FROM u WHERE id + 0 = 2") == 0);

        // NULLs are not duplicates of each other in a UNIQUE column
        auto nulls = ex.execute_sql("UPDATE u SET email = NULL WHERE id >= 3");
        INFO("mode " << mode << ": " << (nulls.is_ok() ? nulls.value() : nulls.error()));
        REQUIRE(changed(nulls, 2, "updated"));

        // a failing CHECK on a later row must leave every earlier row untouched
        REQUIRE(ex.execute_sql("CREATE TABLE ck (id INT PRIMARY KEY, v INT, CHECK (v < 10))").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO ck VALUES (1, 1), (2, 2), (3, 3)").is_ok());
        REQUIRE(ex.execute_sql("UPDATE ck SET v = v + 8").is_err());
        REQUIRE(count_of(ex, "FROM ck") == 3);
        REQUIRE(count_of(ex, "FROM ck WHERE v = 1") == 1);
        REQUIRE(count_of(ex, "FROM ck WHERE v = 2") == 1);
        REQUIRE(count_of(ex, "FROM ck WHERE v = 3") == 1);
        REQUIRE(changed(ex.execute_sql("UPDATE ck SET v = v + 4"), 3, "updated")); // and a valid one still works
    }
}
