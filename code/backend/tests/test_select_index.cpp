#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// A WHERE clause treats "7", "7.0", "07" and "7.00" as one number, while a column stores the text it was given.
// SELECT answers through the indexes (executor_select.cpp's fast paths, the join probes, hash/sort-merge join), and an
// index that compares keys by their text finds fewer rows than the scan -- or, for a non-unique composite index,
// forgets rows altogether. The ORACLE here is the same query forced onto the plain scan: `WHERE (P) OR id < 0` -- an OR
// is never indexed, and `id < 0` is false for every row -- or, for joins, an ON clause with an extra `AND` conjunct
// (which the planner never turns into a hash/index join). Both have to return exactly the same rows.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

std::string ok_text(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql);
    REQUIRE(r.is_ok());
    return r.value();
}

// RETURNING-less SELECT output as a sorted set of lines: the two plans legitimately differ in row order.
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

// the cells of the data rows of a table printout
std::vector<std::vector<std::string>> cells(const std::string& text) {
    std::vector<std::vector<std::string>> rows;
    std::istringstream in(text);
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars <= 1) continue; // header
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

std::vector<int> ids_of(Executor& ex, const std::string& sql) {
    std::vector<int> out;
    for (auto& r : cells(ok_text(ex, sql))) out.push_back(std::atoi(r.at(0).c_str()));
    std::sort(out.begin(), out.end());
    return out;
}

// A number as its value, other text as itself: two spellings of one number print the same.
std::string canon(const std::string& v) {
    char* end = nullptr;
    double d = std::strtod(v.c_str(), &end);
    if (!v.empty() && end == v.c_str() + v.size()) {
        char buf[40];
        std::snprintf(buf, sizeof buf, "%.10g", d);
        return buf;
    }
    return v;
}

std::string canon_column(const std::string& text) {
    std::string out;
    for (auto& row : cells(text)) {
        for (auto& c : row) out += canon(c) + ",";
        out += "\n";
    }
    return out;
}

bool uses_index(Executor& ex, const std::string& select_sql) {
    auto plan = ok_text(ex, "EXPLAIN " + select_sql);
    auto at = plan.find("Access: ");
    return at != std::string::npos && plan.compare(at + 8, 8, "Seq Scan") != 0;
}

void open_db(Executor& ex) {
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
}

// `P` through whatever index the planner picks versus the same `P` forced onto the scan
void check_same(Executor& ex, const std::string& select_list, const std::string& from, const std::string& pred, const std::string& tail = "") {
    std::string indexed = "SELECT " + select_list + " FROM " + from + " WHERE " + pred + tail;
    std::string scanned = "SELECT " + select_list + " FROM " + from + " WHERE (" + pred + ") OR id < 0" + tail;
    INFO("predicate: " << pred);
    REQUIRE(sorted_lines(ok_text(ex, indexed)) == sorted_lines(ok_text(ex, scanned)));
}

// Every row the scan sees must also be reachable through the primary-key B+Tree (a full-range PK scan)
void check_pk_full(Executor& ex, const std::string& tbl) {
    INFO("full PK range on " << tbl);
    REQUIRE(sorted_lines(ok_text(ex, "SELECT id FROM " + tbl + " WHERE id >= -1000000")) == sorted_lines(ok_text(ex, "SELECT id FROM " + tbl)));
}
} // namespace

TEST_CASE("SELECT index: numerically equal spellings are found on every access path", "[select_index]") {
    TempDataDir dir("sel_idx_num");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE d (id INT PRIMARY KEY, price DECIMAL(10,2), code VARCHAR(10), qty INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX ip ON d (price)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX ic ON d (code)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX iq ON d (qty)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO d VALUES (1, 7, '07', 7), (2, 7.00, '7', 7.0), (3, 7.5, '7.0', 8), (4, 6.99, 'x', 6), (5, 8, '8', 007)").is_ok());

    SECTION("point, range and between on a decimal, text and int column") {
        // what the scan says, spelled out (not just index == scan)
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE price = 7") == std::vector<int>{1, 2});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE price = 7.0") == std::vector<int>{1, 2});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE price = 7.00") == std::vector<int>{1, 2});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE price >= 7") == std::vector<int>{1, 2, 3, 5});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE price > 7") == std::vector<int>{3, 5});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE price <= 7") == std::vector<int>{1, 2, 4});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE price < 7.00") == std::vector<int>{4});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE price BETWEEN 7 AND 7") == std::vector<int>{1, 2});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE price BETWEEN 7.0 AND 7.5") == std::vector<int>{1, 2, 3});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE code = 7") == std::vector<int>{1, 2, 3});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE code = '7.00'") == std::vector<int>{1, 2, 3});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE qty = 7") == std::vector<int>{1, 2, 5});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE qty >= 7") == std::vector<int>{1, 2, 3, 5});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE qty < 7.0") == std::vector<int>{4});
        for (const char* p : {"price = 7", "price >= 7.00", "price > 7.0", "price <= 7", "price BETWEEN 7 AND 7.5", "code = 7", "code = '07'",
                              "qty = 7.0", "qty > 6.0", "qty BETWEEN 7.0 AND 8"}) {
            REQUIRE(uses_index(ex, std::string("SELECT id FROM d WHERE ") + p));
            check_same(ex, "id", "d", p);
            check_same(ex, "*", "d", p);
        }
    }

    SECTION("primary key") {
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE id = 4.0") == std::vector<int>{4});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE id >= 4.0") == std::vector<int>{4, 5});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE id BETWEEN 2.0 AND 3") == std::vector<int>{2, 3});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE id < 2.00") == std::vector<int>{1});
        for (const char* p : {"id = 4.0", "id = 04", "id > 1.0", "id <= 3.0", "id BETWEEN 2.0 AND 4.0", "id = 9", "id >= 9.0"}) {
            REQUIRE(uses_index(ex, std::string("SELECT id FROM d WHERE ") + p));
            check_same(ex, "*", "d", p);
        }
    }

    SECTION("covering lookups return what is stored, not the lookup key") {
        // (a DECIMAL column stores 7 and 7.00 alike, as "7.00": the spellings that differ live in the VARCHAR column `code`)
        // before: `SELECT code WHERE code = 7` answered "7" once -- the key -- instead of "07", "7" and "7.0"
        REQUIRE(sorted_lines(ok_text(ex, "SELECT code FROM d WHERE code = 7")) == sorted_lines(ok_text(ex, "SELECT code FROM d WHERE code = 7 OR id < 0")));
        auto got = cells(ok_text(ex, "SELECT code FROM d WHERE code = 7"));
        REQUIRE(got.size() == 3);
        std::vector<std::string> vals{got[0][0], got[1][0], got[2][0]};
        std::sort(vals.begin(), vals.end());
        REQUIRE(vals == std::vector<std::string>{"07", "7", "7.0"});
        // a strict bound must not return the boundary value in another spelling (it used to: 7.00 for `> 7`)
        for (const char* p : {"code > 7", "code >= 7", "code < 7.00", "code <= 7.0", "code = 7.00", "price > 7", "price >= 7", "price = 7.00"}) {
            check_same(ex, p[0] == 'c' ? "code" : "price", "d", p);
        }
        // the covering shortcut only applies to the index's own column
        check_same(ex, "code, qty", "d", "code = 7");
        check_same(ex, "price, code", "d", "code > 7");
    }

    SECTION("ORDER BY ... LIMIT through the index") {
        for (const char* p : {"price >= 7", "price >= 7.00", "price > 7.0", "price <= 7.5", "price BETWEEN 7 AND 8.00"}) {
            for (const char* dir : {"ASC", "DESC"}) {
                for (int limit : {1, 2, 3, 10}) {
                    std::string tail = std::string(" ORDER BY price ") + dir + " LIMIT " + std::to_string(limit);
                    std::string a = "SELECT price FROM d WHERE " + std::string(p) + tail;
                    std::string b = "SELECT price FROM d WHERE (" + std::string(p) + ") OR id < 0" + tail;
                    INFO(a);
                    // numerically equal values may sit in either order: compare the values, not their spelling
                    REQUIRE(canon_column(ok_text(ex, a)) == canon_column(ok_text(ex, b)));
                }
            }
        }
    }

    SECTION("a LIKE prefix that reads as a number is not scanned by text") {
        REQUIRE(ex.execute_sql("INSERT INTO d VALUES (6, 12, '12', 12), (7, 13, '13', 13), (8, 120, '120', 120)").is_ok());
        for (const char* p : {"code LIKE '1%'", "code LIKE '12%'", "code LIKE '7%'", "code LIKE 'x%'", "code LIKE '0%'"}) {
            check_same(ex, "id", "d", p);
        }
    }

    SECTION("an AND of two indexed equalities (index intersection)") {
        for (const char* p : {"price = 7 AND qty = 7", "price = 7.0 AND code = 7", "qty = 7.0 AND code = '07'", "price = 8 AND qty = 7"}) {
            check_same(ex, "id", "d", p);
        }
    }
}

TEST_CASE("SELECT index: a hash index buckets by numeric value", "[select_index]") {
    TempDataDir dir("sel_idx_hash");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE hx (id INT PRIMARY KEY, code VARCHAR(10), n INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX hxc ON hx (code) USING HASH").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX hxn ON hx (n) USING HASH").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO hx VALUES (1, '07', 7), (2, '7', 07), (3, '7.0', 7.0), (4, 'q', 8), (5, '7e0', 70)").is_ok());
    REQUIRE(ids_of(ex, "SELECT id FROM hx WHERE code = 7") == std::vector<int>{1, 2, 3, 5});
    REQUIRE(ids_of(ex, "SELECT id FROM hx WHERE code = '07.0'") == std::vector<int>{1, 2, 3, 5});
    REQUIRE(ids_of(ex, "SELECT id FROM hx WHERE code = 'q'") == std::vector<int>{4});
    REQUIRE(ids_of(ex, "SELECT id FROM hx WHERE n = 7.00") == std::vector<int>{1, 2, 3});
    for (const char* p : {"code = 7", "code = '7.0'", "code = 'q'", "n = 7", "n = 8.0", "code = 7 AND n = 7.0", "code = '07' AND n = 7"}) {
        check_same(ex, "id", "hx", p);
    }
    // the bucket follows the row when its value changes spelling or value
    REQUIRE(ex.execute_sql("UPDATE hx SET code = '8.0' WHERE id = 2").is_ok());
    REQUIRE(ids_of(ex, "SELECT id FROM hx WHERE code = 8") == std::vector<int>{2});
    REQUIRE(ids_of(ex, "SELECT id FROM hx WHERE code = 7") == std::vector<int>{1, 3, 5});
    REQUIRE(ex.execute_sql("DELETE FROM hx WHERE code = 7").is_ok());
    REQUIRE(ids_of(ex, "SELECT id FROM hx") == std::vector<int>{2, 4});
}

TEST_CASE("SELECT index: a non-unique composite index keeps every row", "[select_index]") {
    TempDataDir dir("sel_idx_comp");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE k (id INT PRIMARY KEY, a INT, b DECIMAL(10,2), n VARCHAR(10), a2 INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX kab ON k (a, b)").is_ok());
    // (1,7) is shared by rows 1, 2, 3 and 6 (spelled 7, 7, 7.00, 7.0) -- before, they all fought over one index entry
    REQUIRE(ex.execute_sql("INSERT INTO k VALUES (1, 1, 7, 'x', 1), (2, 1, 7, 'y', 5), (3, 1, 7.00, 'z', 1), (4, 1, 8, 'w', 1), (5, 2, 7, 'v', 2), (6, 1, 7.0, 'u', 9)").is_ok());

    REQUIRE(uses_index(ex, "SELECT id FROM k WHERE a = 1 AND b = 7"));
    REQUIRE(ids_of(ex, "SELECT id FROM k WHERE a = 1 AND b = 7") == std::vector<int>{1, 2, 3, 6});
    REQUIRE(ids_of(ex, "SELECT id FROM k WHERE a = 1 AND b = 7.00") == std::vector<int>{1, 2, 3, 6});
    REQUIRE(ids_of(ex, "SELECT id FROM k WHERE a = 1.0 AND b = 7") == std::vector<int>{1, 2, 3, 6});
    REQUIRE(ids_of(ex, "SELECT id FROM k WHERE a = 1") == std::vector<int>{1, 2, 3, 4, 6});
    REQUIRE(ids_of(ex, "SELECT id FROM k WHERE a = 1 AND b = 7 AND n = 'y'") == std::vector<int>{2});
    REQUIRE(ids_of(ex, "SELECT id FROM k WHERE a = 1 AND n = 'u'") == std::vector<int>{6});
    // `a = a2` compares two columns; it is not a lookup key
    REQUIRE(ids_of(ex, "SELECT id FROM k WHERE a = a2 AND b = 7") == std::vector<int>{1, 3, 5});

    auto all_forms = [&] {
        for (const char* p : {"a = 1 AND b = 7", "a = 1 AND b = 7.00", "a = 2 AND b = 7", "a = 1", "a = 1.0", "a = 1 AND n = 'y'", "a = 1 AND b = 8 AND n = 'w'",
                              "a = a2 AND b = 7", "a = 1 AND a = 1.0", "a = 1 AND b = 7 AND a2 > 1", "a = 3 AND b = 7"}) {
            check_same(ex, "id", "k", p);
            check_same(ex, "*", "k", p);
        }
    };
    all_forms();

    SECTION("DELETE removes only the rows it deletes") {
        REQUIRE(ex.execute_sql("DELETE FROM k WHERE id = 2").is_ok());
        REQUIRE(ids_of(ex, "SELECT id FROM k WHERE a = 1 AND b = 7") == std::vector<int>{1, 3, 6});
        all_forms();
    }
    SECTION("UPDATE moves a row between keys, and keeps it in place when the key is unchanged") {
        REQUIRE(ex.execute_sql("UPDATE k SET n = 'zz' WHERE id = 3").is_ok()); // same key
        REQUIRE(ids_of(ex, "SELECT id FROM k WHERE a = 1 AND b = 7") == std::vector<int>{1, 2, 3, 6});
        REQUIRE(ex.execute_sql("UPDATE k SET b = 8.0 WHERE id = 1").is_ok()); // new key
        REQUIRE(ids_of(ex, "SELECT id FROM k WHERE a = 1 AND b = 7") == std::vector<int>{2, 3, 6});
        REQUIRE(ids_of(ex, "SELECT id FROM k WHERE a = 1 AND b = 8") == std::vector<int>{1, 4});
        REQUIRE(ex.execute_sql("UPDATE k SET id = 20 WHERE id = 6").is_ok()); // the primary key itself
        REQUIRE(ids_of(ex, "SELECT id FROM k WHERE a = 1 AND b = 7") == std::vector<int>{2, 3, 20});
        all_forms();
    }
    SECTION("ROLLBACK") {
        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        REQUIRE(ex.execute_sql("DELETE FROM k WHERE a = 1 AND b = 7").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO k VALUES (7, 1, 7, 'new', 1)").is_ok());
        REQUIRE(ids_of(ex, "SELECT id FROM k WHERE a = 1 AND b = 7") == std::vector<int>{7});
        REQUIRE(ex.execute_sql("ROLLBACK").is_ok());
        REQUIRE(ids_of(ex, "SELECT id FROM k WHERE a = 1 AND b = 7") == std::vector<int>{1, 2, 3, 6});
        all_forms();
    }
}

TEST_CASE("SELECT index: joins match numerically equal keys through every algorithm", "[select_index][join]") {
    TempDataDir dir("sel_idx_join");
    Executor ex(dir.path);
    open_db(ex);
    std::mt19937 rng(7);
    // some keys are NULL: NULL joins with nothing, itself included (hash and sort-merge joins used to match NULL with NULL)
    auto spell = [&](int v) -> std::string {
        if (rng() % 9 == 0) return "NULL";
        switch (rng() % 4) {
            case 0: return std::to_string(v);
            case 1: return std::to_string(v) + ".0";
            case 2: return std::to_string(v) + ".00";
            default: return v < 10 ? "0" + std::to_string(v) : std::to_string(v);
        }
    };
    // build l(lid PK, v, k) and r(rid PK, v, w, k2) with `n_l`/`n_r` rows over a small value domain so keys repeat
    auto build = [&](const std::string& sfx, int n_l, int n_r, const std::string& extra_ddl) {
        std::string l = "l" + sfx, r = "r" + sfx;
        // text columns: an INT / DECIMAL column stores every spelling of a number alike, and these joins are about the spellings
        REQUIRE(ex.execute_sql("CREATE TABLE " + l + " (lid INT PRIMARY KEY, v VARCHAR(12), k VARCHAR(12))").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE " + r + " (rid INT PRIMARY KEY, v VARCHAR(12), w INT, k2 VARCHAR(12))").is_ok());
        for (int i = 1; i <= n_l; i++) {
            REQUIRE(ex.execute_sql("INSERT INTO " + l + " VALUES (" + std::to_string(i) + ", " + spell(static_cast<int>(rng() % 9)) + ", " +
                                   spell(static_cast<int>(rng() % 12)) + ")")
                        .is_ok());
        }
        for (int i = 1; i <= n_r; i++) {
            REQUIRE(ex.execute_sql("INSERT INTO " + r + " VALUES (" + std::to_string(i) + ", " + spell(static_cast<int>(rng() % 9)) + ", " +
                                   std::to_string(i % 5) + ", " + spell(static_cast<int>(rng() % 12)) + ")")
                        .is_ok());
        }
        if (!extra_ddl.empty()) REQUIRE(ex.execute_sql(extra_ddl).is_ok());
    };
    auto same_join = [&](const std::string& sfx, const std::string& on, const char* expected_algo) {
        std::string l = "l" + sfx, r = "r" + sfx;
        std::string q = "SELECT " + l + ".lid, " + r + ".rid FROM " + l + " JOIN " + r + " ON " + on;
        std::string forced = q + " AND " + l + ".lid > 0"; // two conjuncts: always a nested loop
        INFO(q);
        REQUIRE(ok_text(ex, "EXPLAIN " + q).find(expected_algo) != std::string::npos);
        REQUIRE(ok_text(ex, "EXPLAIN " + forced).find("Nested Loop") != std::string::npos);
        auto a = sorted_lines(ok_text(ex, q)), b = sorted_lines(ok_text(ex, forced));
        REQUIRE(a == b);
        REQUIRE(cells(ok_text(ex, q)).size() > 0);
    };

    SECTION("hash join") {
        build("h", 40, 40, "");
        same_join("h", "lh.v = rh.v", "Hash Join");
    }
    SECTION("sort-merge join") {
        build("s", 7, 7, "");
        same_join("s", "ls.v = rs.v", "Sort-Merge Join");
    }
    SECTION("index nested loop: the right side's primary key is probed") {
        build("p", 50, 50, "");
        same_join("p", "lp.k = rp.rid", "Index NL Join   probe=");
    }
    SECTION("reverse index nested loop through a B+Tree on the left column") {
        build("b", 60, 8, "CREATE INDEX lbk ON lb (k)");
        same_join("b", "lb.k = rb.k2", "Reverse Index NL Join");
    }
    SECTION("the index joins inside a transaction (they fall back to a hash join there)") {
        build("t", 50, 50, "");
        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        same_join("t", "lt.k = rt.rid", "Index NL Join   probe=");
        REQUIRE(ex.execute_sql("ROLLBACK").is_ok());
        build("u", 60, 8, "CREATE INDEX luk ON lu (k)");
        REQUIRE(ex.execute_sql("BEGIN").is_ok());
        same_join("u", "lu.k = ru.k2", "Reverse Index NL Join");
        REQUIRE(ex.execute_sql("ROLLBACK").is_ok());
    }
    SECTION("reverse index nested loop through a hash index on the left column") {
        build("x", 60, 8, "CREATE INDEX lxk ON lx (k) USING HASH");
        same_join("x", "lx.k = rx.k2", "Reverse Index NL Join");
    }
}

TEST_CASE("SELECT index: a primary key that is not the first column", "[select_index]") {
    // INSERT keyed the PK B+Tree, the undo log and the upsert lookups by the table's FIRST column, so with
    // `CREATE TABLE t (name ..., id INT PRIMARY KEY)` every `WHERE id = N` answered "0 rows".
    TempDataDir dir("sel_idx_pkpos");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE t (name VARCHAR(10), id INT PRIMARY KEY, v INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES ('a', 1, 10), ('b', 2, 20), ('c', 3, 30), ('b', 4, 40)").is_ok());
    auto check = [&] {
        for (const char* p : {"id = 2", "id = 2.0", "id >= 3", "id > 1", "id < 4", "id BETWEEN 2 AND 3", "id = 9", "name = 'b'", "v = 20"}) {
            check_same(ex, "*", "t", p);
            check_same(ex, "name", "t", p);
        }
    };
    check();
    REQUIRE(ids_of(ex, "SELECT id FROM t WHERE id = 2") == std::vector<int>{2});
    REQUIRE(ids_of(ex, "SELECT id FROM t WHERE id >= 3") == std::vector<int>{3, 4});

    REQUIRE(ex.execute_sql("BEGIN").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES ('d', 5, 50)").is_ok());
    REQUIRE(ids_of(ex, "SELECT id FROM t WHERE id = 5") == std::vector<int>{5});
    REQUIRE(ex.execute_sql("ROLLBACK").is_ok());
    REQUIRE(ids_of(ex, "SELECT id FROM t WHERE id = 5").empty());
    REQUIRE(ids_of(ex, "SELECT id FROM t") == std::vector<int>{1, 2, 3, 4});

    // upsert: the duplicate is found by id, and the existing row is the one that changes
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES ('zz', 2, 99) ON DUPLICATE KEY UPDATE v = 77").is_ok());
    REQUIRE(ids_of(ex, "SELECT id FROM t WHERE v = 77") == std::vector<int>{2});
    REQUIRE(ids_of(ex, "SELECT id FROM t") == std::vector<int>{1, 2, 3, 4});
    REQUIRE(ex.execute_sql("UPDATE t SET v = 5 WHERE id = 3").is_ok());
    REQUIRE(ex.execute_sql("DELETE FROM t WHERE id = 1").is_ok());
    check();
    REQUIRE(ids_of(ex, "SELECT id FROM t WHERE id = 3 AND v = 5") == std::vector<int>{3});
    REQUIRE(ids_of(ex, "SELECT id FROM t WHERE id = 1").empty());
}

TEST_CASE("SELECT index: indexes answer correctly after a checkpoint and a restart", "[select_index][persist]") {
    // Indexes used to be loaded from `.idx` files written when they were CREATEd and never refreshed, so an index created
    // before its rows came back empty after a checkpoint + restart ("0 rows" for rows the scan finds); and the PK B+Tree was
    // rebuilt keyed by the table's FIRST column, which broke `WHERE id = 2` when the primary key was not the first column.
    TempDataDir dir("sel_idx_restart");
    const std::vector<std::string> preds = {"price = 8", "price = 8.0", "price >= 8", "price < 9", "price BETWEEN 7 AND 8", "id = 2", "id = 2.0", "id >= 3",
                                            "id BETWEEN 1 AND 3", "name = 'b'", "name = 'q'", "name LIKE 'a%'", "price = 8 AND name = 'b'", "price = 8 AND id > 0",
                                            "name = 'b' AND price = 8.00", "id = 6"};
    auto check_all = [&](Executor& ex) {
        for (auto& p : preds) {
            check_same(ex, "id", "d", p);
            check_same(ex, "*", "d", p);
        }
    };
    {
        Executor ex(dir.path);
        open_db(ex);
        REQUIRE(ex.execute_sql("CREATE TABLE d (name VARCHAR(10), id INT PRIMARY KEY, price DECIMAL(10,2))").is_ok());
        REQUIRE(ex.execute_sql("CREATE INDEX ip ON d (price)").is_ok());
        REQUIRE(ex.execute_sql("CREATE INDEX hn ON d (name) USING HASH").is_ok());
        REQUIRE(ex.execute_sql("CREATE INDEX cn ON d (name, price)").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO d VALUES ('a', 1, 7), ('b', 2, 8), ('b', 3, 8.00), ('c', 4, 9), ('a', 5, 8.0)").is_ok());
        REQUIRE(ex.execute_sql("CHECKPOINT").is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO d VALUES ('q', 6, 8)").is_ok());
        REQUIRE(ex.execute_sql("VACUUM d").is_ok());
        REQUIRE(ex.execute_sql("DELETE FROM d WHERE id = 4").is_ok());
        REQUIRE(ex.execute_sql("CHECKPOINT").is_ok());
        check_all(ex);
    }
    {
        Executor ex(dir.path);
        REQUIRE(ex.execute_sql("USE d").is_ok());
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE price = 8") == std::vector<int>{2, 3, 5, 6});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE id = 2") == std::vector<int>{2});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE name = 'b'") == std::vector<int>{2, 3});
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE name = 'b' AND price = 8.0") == std::vector<int>{2, 3});
        check_all(ex);
        // and the rebuilt indexes keep being maintained
        REQUIRE(ex.execute_sql("INSERT INTO d VALUES ('b', 7, 8.00)").is_ok());
        REQUIRE(ex.execute_sql("UPDATE d SET price = 9 WHERE id = 3").is_ok());
        REQUIRE(ids_of(ex, "SELECT id FROM d WHERE price = 8") == std::vector<int>{2, 5, 6, 7});
        check_all(ex);
    }
}

// ---------------------------------------------------------------------------------------------------------------------

namespace {
struct Gen {
    std::mt19937 rng;
    explicit Gen(unsigned seed) : rng(seed) {}
    int n(int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); }
    std::string num(int lo, int hi) { return std::to_string(n(lo, hi)); }
    // integers in several spellings of the same number
    std::string spelled_int(int lo, int hi) {
        int v = n(lo, hi);
        switch (n(0, 3)) {
            case 0: return std::to_string(v) + ".0";
            case 1: return (v < 10 ? "0" : "") + std::to_string(v);
            default: return std::to_string(v);
        }
    }
    std::string bval() {
        static const char* v[] = {"x0", "x1", "x2", "x3", "x4", "x5", "07", "7", "7.0", "abc", "ab", "abd", "x", "12", "120"};
        return v[n(0, 14)];
    }
    std::string cval() {
        static const char* v[] = {"7", "7.00", "7.0", "3.5", "12.25", "0", "100", "7.5", "8", "8.0", "3.50"};
        return v[n(0, 10)];
    }
    std::string pred() {
        switch (n(0, 39)) {
            case 0: return "a = " + spelled_int(0, 29);
            case 1: return "a > " + spelled_int(0, 29);
            case 2: return "a >= " + spelled_int(0, 29);
            case 3: return "a < " + spelled_int(0, 29);
            case 4: return "a <= " + spelled_int(0, 29);
            case 5: { int lo = n(0, 25); return "a BETWEEN " + std::to_string(lo) + ".0 AND " + std::to_string(lo + n(0, 6)); }
            case 6: return "b = '" + bval() + "'";
            case 7: return "b = " + num(0, 12);
            case 8: return "b LIKE 'x%'";
            case 9: return "b LIKE 'ab%'";
            case 10: return "b LIKE '1%'";
            case 11: return "c = " + cval();
            case 12: return "c >= " + cval();
            case 13: return "c <= " + cval();
            case 14: return "c BETWEEN 3 AND " + num(4, 13) + ".00";
            case 15: return "c > " + cval();
            case 16: return "c < " + cval();
            case 17: return "id = " + spelled_int(1, 150);
            case 18: return "id > " + spelled_int(1, 150);
            case 19: return "id <= " + spelled_int(1, 150);
            case 20: { int lo = n(1, 140); return "id BETWEEN " + std::to_string(lo) + ".0 AND " + std::to_string(lo + n(0, 12)); }
            case 21: return "d = " + num(0, 9);
            case 22: return "a = " + spelled_int(0, 29) + " AND b = '" + bval() + "'";
            case 23: return "a >= " + spelled_int(0, 29) + " AND d < " + num(0, 9);
            case 24: return "b = '" + bval() + "' AND c > 3";
            case 25: return "a = " + spelled_int(0, 29) + " AND c = " + cval();
            case 26: return "a = " + spelled_int(0, 29) + " AND c = " + cval() + " AND b = '" + bval() + "'";
            case 27: return "a = " + spelled_int(0, 29) + " AND d = " + num(0, 9);
            case 28: return "c = " + cval() + " AND a > " + spelled_int(0, 29);
            case 29: return "a = " + spelled_int(0, 29) + " AND a = " + spelled_int(0, 29);
            case 30: return "a = d AND c = " + cval();
            case 31: return "b = '" + bval() + "' AND a = " + spelled_int(0, 29);
            case 32: return "c = " + cval() + " AND b = " + num(0, 9);
            case 33: return "a = " + spelled_int(0, 29) + " OR c = " + cval();
            case 34: return "a IN (" + num(0, 29) + ", " + num(0, 29) + ")";
            case 35: return "NOT a = " + num(0, 29);
            case 36: return "id = " + num(1, 150) + " AND a >= 0";
            case 37: return "b > 'x3'";
            case 38: return "a <= " + spelled_int(0, 29) + " AND a >= " + spelled_int(0, 29);
            default: return "c <> " + cval();
        }
    }
    // range predicates on the column an ORDER BY ... LIMIT will sort by
    std::string range_pred(const std::string& col) {
        std::string v = col == "a" ? spelled_int(0, 29) : cval();
        switch (n(0, 4)) {
            case 0: return col + " >= " + v;
            case 1: return col + " > " + v;
            case 2: return col + " <= " + v;
            case 3: return col + " < " + v;
            default: return col + " BETWEEN " + v + " AND " + (col == "a" ? spelled_int(0, 29) : cval());
        }
    }
    std::string assign() {
        switch (n(0, 7)) {
            case 0: return "a = a + 1";
            case 1: return "b = '" + bval() + "'";
            case 2: return "c = " + cval();
            case 3: return "d = d + 5";
            case 4: return "a = " + spelled_int(0, 29) + ", b = '" + bval() + "'";
            case 5: return "d = " + num(0, 9);
            case 6: return "a = " + spelled_int(0, 29) + ", c = " + cval();
            default: return "a = a + 2, c = " + cval();
        }
    }
    std::string insert_row(int id) {
        return "INSERT INTO TBL VALUES (" + std::to_string(id) + ", " + spelled_int(0, 29) + ", '" + bval() + "', " + cval() + ", " + num(0, 9) + ")";
    }
};

std::string with_table(std::string sql, const std::string& tbl) {
    for (std::size_t p; (p = sql.find("TBL")) != std::string::npos;) sql.replace(p, 3, tbl);
    return sql;
}
} // namespace

TEST_CASE("SELECT index: randomized differential test, index paths vs plain scan", "[select_index][fuzz]") {
    // 3 seeds by default; RUSQL_FUZZ_SEEDS=60 runs a much longer campaign
    unsigned seed_count = 3;
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 0; // RUSQL_FUZZ_START=44 replays a failing seed on its own
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    const bool trace = std::getenv("RUSQL_FUZZ_TRACE") != nullptr;
    std::size_t indexed_plans = 0, queries = 0;
    const std::uint64_t candidate_searches_before = Executor::dml_index_hits.load();

    for (unsigned k = seed_start; k < seed_start + seed_count; k++) {
        const unsigned seed = 31u + k * 197u;
        INFO("seed " << seed);
        TempDataDir dir("sel_idx_fz");
        Executor ex(dir.path);
        open_db(ex);
        // t: B+Tree indexes on a, b, c plus a composite (a, c); h: hash indexes on a and b, B+Tree on c
        REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, a INT, b VARCHAR(20), c DECIMAL(10,2), d INT)").is_ok());
        REQUIRE(ex.execute_sql("CREATE INDEX t_ia ON t (a)").is_ok());
        REQUIRE(ex.execute_sql("CREATE INDEX t_ib ON t (b)").is_ok());
        REQUIRE(ex.execute_sql("CREATE INDEX t_ic ON t (c)").is_ok());
        REQUIRE(ex.execute_sql("CREATE INDEX t_iac ON t (a, c)").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE h (id INT PRIMARY KEY, a INT, b VARCHAR(20), c DECIMAL(10,2), d INT)").is_ok());
        REQUIRE(ex.execute_sql("CREATE INDEX h_ha ON h (a) USING HASH").is_ok());
        REQUIRE(ex.execute_sql("CREATE INDEX h_hb ON h (b) USING HASH").is_ok());
        REQUIRE(ex.execute_sql("CREATE INDEX h_ic ON h (c)").is_ok());
        const std::vector<std::string> tables = {"t", "h"};

        Gen g(seed);
        int next_id = 1;
        for (int i = 0; i < 120; i++, next_id++) {
            for (auto& tbl : tables) REQUIRE(ex.execute_sql(with_table(g.insert_row(next_id), tbl)).is_ok());
        }
        bool in_txn = false;

        for (int step = 0; step < 700; step++) {
            for (auto& t : tables) {
                INFO("before step " << step);
                check_pk_full(ex, t);
            }
            const std::string& tbl = tables[static_cast<std::size_t>(g.n(0, 1))];
            int kind = g.n(0, 99);
            if (kind < 12) {
                std::string sql = "UPDATE " + tbl + " SET " + g.assign() + " WHERE " + g.pred();
                INFO("step " << step << ": " << sql);
                auto r = ex.execute_sql(sql);
                (void)r; // an UPDATE may legitimately fail (nothing to check); the queries below are what counts
                if (trace) std::cerr << "step " << step << ": " << sql << std::endl;
            } else if (kind < 20) {
                std::string sql = "DELETE FROM " + tbl + " WHERE " + g.pred();
                INFO("step " << step << ": " << sql);
                REQUIRE(ex.execute_sql(sql).is_ok());
                if (trace) std::cerr << "step " << step << ": " << sql << std::endl;
            } else if (kind < 32) {
                std::string sql = with_table(g.insert_row(next_id++), tbl);
                INFO("step " << step << ": " << sql);
                REQUIRE(ex.execute_sql(sql).is_ok());
                if (trace) std::cerr << "step " << step << ": " << sql << std::endl;
            } else if (kind < 36) {
                std::string sql = in_txn ? (g.n(0, 2) == 0 ? "ROLLBACK" : "COMMIT") : "BEGIN";
                in_txn = !in_txn;
                REQUIRE(ex.execute_sql(sql).is_ok());
                if (trace) std::cerr << "step " << step << ": " << sql << std::endl;
            } else if (kind < 38) {
                if (in_txn) continue;
                REQUIRE(ex.execute_sql("VACUUM " + tbl).is_ok());
                if (trace) std::cerr << "step " << step << ": VACUUM " << tbl << std::endl;
            } else {
                std::string pred = g.pred();
                queries++;
                INFO("step " << step << " on " << tbl << ": " << pred);
                if (trace) std::cerr << "step " << step << ": SELECT .. FROM " << tbl << " WHERE " << pred << std::endl;
                if (uses_index(ex, "SELECT id FROM " + tbl + " WHERE " + pred)) indexed_plans++;
                check_same(ex, "id", tbl, pred);
                check_same(ex, "*", tbl, pred);
                // covering shapes: the selected column is the one the predicate is on
                if (g.n(0, 2) == 0) {
                    std::string cp = g.range_pred("c");
                    check_same(ex, "c", tbl, cp);
                    check_same(ex, "c", tbl, "c = " + g.cval());
                    check_same(ex, "a, c", tbl, "a = " + g.spelled_int(0, 29));
                }
                // aggregates, GROUP BY, DISTINCT, LIMIT and windows start from the same candidate search as UPDATE/DELETE
                if (g.n(0, 2) == 0) {
                    check_same(ex, "COUNT(*), SUM(d), MIN(a), MAX(d), COUNT(b)", tbl, pred);
                    check_same(ex, "a, COUNT(*), SUM(d)", tbl, pred, " GROUP BY a");
                    check_same(ex, "b, COUNT(*)", tbl, pred, " GROUP BY b HAVING COUNT(*) > 1");
                    check_same(ex, "DISTINCT b, d", tbl, pred);
                    check_same(ex, "id, d", tbl, pred, " ORDER BY d DESC, id LIMIT 4 OFFSET 1");
                    check_same(ex, "id, ROW_NUMBER() OVER (ORDER BY id) AS rn", tbl, pred);
                }
                // Top-K: ORDER BY the indexed column with LIMIT
                if (g.n(0, 2) == 0) {
                    std::string col = g.n(0, 1) ? "a" : "c";
                    std::string rp = g.range_pred(col);
                    std::string tail = std::string(" ORDER BY ") + col + (g.n(0, 1) ? " ASC" : " DESC") + " LIMIT " + std::to_string(g.n(1, 12));
                    std::string a = "SELECT " + col + " FROM " + tbl + " WHERE " + rp + tail;
                    std::string b = "SELECT " + col + " FROM " + tbl + " WHERE (" + rp + ") OR id < 0" + tail;
                    INFO(a);
                    REQUIRE(canon_column(ok_text(ex, a)) == canon_column(ok_text(ex, b)));
                }
            }
        }
        if (in_txn) REQUIRE(ex.execute_sql("COMMIT").is_ok());
        for (auto& tbl : tables) {
            for (int i = 0; i < 25; i++) check_same(ex, "*", tbl, g.pred());
        }
    }
    // the indexes really were in play (otherwise this would only compare the scan with itself)
    REQUIRE(indexed_plans * 3 > queries);
    // ... and so did the candidate search the aggregate / GROUP BY / LIMIT / window shapes start from
    REQUIRE(Executor::dml_index_hits.load() - candidate_searches_before > 30 * seed_count);
}
