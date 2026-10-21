#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
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

using namespace engine;
namespace fs = std::filesystem;

// Texts compare by MySQL's default collation wherever a statement looks at them: a comparison, ORDER BY, GROUP BY, DISTINCT, MIN / MAX, IN, LIKE, a join, UNIQUE, a
// PRIMARY KEY -- by a scan and through every kind of index. The random statements below run on the same rows in tables without an index, with a B+Tree index, with a
// hash index and keyed by the text, and every answer is compared with a reference computed here from the rows with collation::compare (which the pairs of
// test_collation.cpp check against a real MySQL).

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

std::string ok_text(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
    return r.value();
}

Rows rows_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
    return table_cells(r.value());
}

std::vector<int> ids_of(Executor& ex, const std::string& sql) {
    std::vector<int> out;
    for (auto& r : rows_of(ex, sql)) out.push_back(std::stoi(r[0]));
    return out;
}

// the texts: upper and lower case, accents, a space at the end, other scripts, punctuation, numbers and the empty text (no quote and no backslash)
const std::vector<std::string> kPool = {
    "alice", "Alice", "ALICE", "bob", "Bob", "BOB", "bob ", "\xC3\x89mile", "emile", "\xC3\xA9mile", "stra\xC3\x9F" "e", "STRASSE", "\xED\x95\x9C\xEA\xB8\x80", "\xEA\xB0\x80\xEB\x82\x98",
    "abc", "ABC", "Abd", "b", "B", "", " ", "a b", "_x", "-x", "1x", "10", "9", "9a", "Zed", "zed", "z", "\xC3\xA4rger", "Arger", "x1", "X10", "x9"};

std::string sql_text(const std::string& v) { return "'" + v + "'"; }

// (a result is a text table whose cells lose the spaces at their ends)
std::string trimmed(std::string v) {
    v.erase(0, v.find_first_not_of(' '));
    v.erase(v.find_last_not_of(' ') + 1);
    return v;
}

bool ends_with_space(const std::string& v) { return !v.empty() && v.back() == ' '; }
} // namespace

TEST_CASE("every way to reach a text finds what the default collation says", "[collation][random][fuzz]") {
    unsigned seed_count = 4; // RUSQL_FUZZ_SEEDS=40 runs a much longer campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 0;
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    std::size_t checks = 0;
    for (unsigned k = seed_start; k < seed_start + seed_count; k++) {
        INFO("seed " << k);
        std::mt19937 rng(31337 + k * 101);
        TempDataDir dir("collation_sql");
        auto ex_ptr = std::make_unique<Executor>(dir.path);
        Executor* ex = ex_ptr.get();
        ok(*ex, "CREATE DATABASE d");
        ok(*ex, "USE d");
        ok(*ex, "CREATE TABLE plain (id INT PRIMARY KEY, s VARCHAR(20), g INT)");
        ok(*ex, "CREATE TABLE idx (id INT PRIMARY KEY, s VARCHAR(20), g INT)");
        ok(*ex, "CREATE INDEX idx_s ON idx (s)");
        ok(*ex, "CREATE TABLE hsh (id INT PRIMARY KEY, s VARCHAR(20), g INT)");
        ok(*ex, "CREATE INDEX hsh_s ON hsh (s) USING HASH");
        ok(*ex, "CREATE TABLE keyed (s VARCHAR(20) PRIMARY KEY, id INT, g INT)");

        struct R {
            int id;
            std::string s;
            int g;
        };
        std::vector<R> rows;
        for (int i = 1; i <= 70; i++) rows.push_back({i, kPool[rng() % kPool.size()], static_cast<int>(rng() % 5)});
        std::string values;
        for (auto& r : rows) values += std::string(values.empty() ? "" : ", ") + "(" + std::to_string(r.id) + ", " + sql_text(r.s) + ", " + std::to_string(r.g) + ")";
        INFO("rows " << values);
        for (const char* t : {"plain", "idx", "hsh"}) ok(*ex, std::string("INSERT INTO ") + t + " VALUES " + values);
        // keyed: one row for each text the collation tells apart
        std::vector<R> keyed;
        {
            std::set<std::string> seen;
            for (auto& r : rows) {
                if (seen.insert(collation::key(r.s)).second) keyed.push_back(r);
            }
            std::string kv;
            for (auto& r : keyed) kv += std::string(kv.empty() ? "" : ", ") + "(" + sql_text(r.s) + ", " + std::to_string(r.id) + ", " + std::to_string(r.g) + ")";
            ok(*ex, "INSERT INTO keyed VALUES " + kv);
        }

        auto reference_ids = [&](const std::vector<R>& source, const std::function<bool(const R&)>& wanted) {
            std::vector<int> out;
            for (auto& r : source) {
                if (wanted(r)) out.push_back(r.id);
            }
            std::sort(out.begin(), out.end());
            return out;
        };

        for (int round = 0; round < 2; round++) {
            for (int q = 0; q < 40; q++) {
                const std::string probe = kPool[rng() % kPool.size()], probe2 = kPool[rng() % kPool.size()];
                struct Case {
                    std::string where;
                    std::function<bool(const R&)> wanted;
                };
                const std::string lo = collation::compare(probe, probe2) <= 0 ? probe : probe2, hi = collation::compare(probe, probe2) <= 0 ? probe2 : probe;
                std::string prefix = probe.substr(0, 1 + rng() % 2);
                while (!prefix.empty() && (static_cast<unsigned char>(prefix.back()) & 0xC0) == 0x80) prefix.pop_back(); // (not in the middle of a character)
                if (!prefix.empty() && (static_cast<unsigned char>(prefix.back()) >= 0xC0)) prefix.pop_back();
                const std::vector<Case> cases = {
                    {"s = " + sql_text(probe), [&](const R& r) { return collation::compare(r.s, probe) == 0; }},
                    {"s <> " + sql_text(probe), [&](const R& r) { return collation::compare(r.s, probe) != 0; }},
                    {"s < " + sql_text(probe), [&](const R& r) { return collation::compare(r.s, probe) < 0; }},
                    {"s >= " + sql_text(probe), [&](const R& r) { return collation::compare(r.s, probe) >= 0; }},
                    {"s BETWEEN " + sql_text(lo) + " AND " + sql_text(hi), [&](const R& r) { return collation::compare(r.s, lo) >= 0 && collation::compare(r.s, hi) <= 0; }},
                    {"s IN (" + sql_text(probe) + ", " + sql_text(probe2) + ")", [&](const R& r) { return collation::compare(r.s, probe) == 0 || collation::compare(r.s, probe2) == 0; }},
                    {"s NOT IN (" + sql_text(probe) + ", " + sql_text(probe2) + ")", [&](const R& r) { return collation::compare(r.s, probe) != 0 && collation::compare(r.s, probe2) != 0; }},
                    {"s LIKE " + sql_text(prefix + "%"), [&](const R& r) { return collation::like(r.s, prefix + "%"); }},
                };
                const Case& c = cases[rng() % cases.size()];
                INFO("WHERE " << c.where);
                for (const char* t : {"plain", "idx", "hsh"}) {
                    INFO("table " << t << ", round " << round);
                    REQUIRE(ids_of(*ex, std::string("SELECT id FROM ") + t + " WHERE " + c.where + " ORDER BY id") == reference_ids(rows, c.wanted));
                }
                REQUIRE(ids_of(*ex, "SELECT id FROM keyed WHERE " + c.where + " ORDER BY id") == reference_ids(keyed, c.wanted));
                checks++;
            }
            // ordering: by the collation, ties by id
            std::vector<R> sorted = rows;
            std::stable_sort(sorted.begin(), sorted.end(), [](const R& a, const R& b) {
                const int c = collation::compare(a.s, b.s);
                return c != 0 ? c < 0 : a.id < b.id;
            });
            std::vector<int> expected_order;
            for (auto& r : sorted) expected_order.push_back(r.id);
            for (const char* t : {"plain", "idx", "hsh"}) REQUIRE(ids_of(*ex, std::string("SELECT id FROM ") + t + " ORDER BY s, id") == expected_order);
            std::stable_sort(sorted.begin(), sorted.end(), [](const R& a, const R& b) {
                const int c = collation::compare(a.s, b.s);
                return c != 0 ? c > 0 : a.id < b.id;
            });
            std::vector<int> expected_desc;
            for (auto& r : sorted) expected_desc.push_back(r.id);
            for (const char* t : {"plain", "idx"}) REQUIRE(ids_of(*ex, std::string("SELECT id FROM ") + t + " ORDER BY s DESC, id") == expected_desc);

            // grouping: the number of distinct texts, groups, and the group sizes
            std::map<std::string, std::size_t> groups, plain_groups;
            std::size_t without_space_at_end = 0;
            for (auto& r : rows) {
                groups[collation::key(r.s)]++;
                if (!ends_with_space(r.s)) plain_groups[collation::key(r.s)]++, without_space_at_end++;
            }
            std::vector<std::size_t> sizes;
            for (auto& [key, n] : groups) sizes.push_back(n);
            std::sort(sizes.begin(), sizes.end());
            for (const char* t : {"plain", "idx", "hsh"}) {
                REQUIRE(rows_of(*ex, std::string("SELECT COUNT(DISTINCT s) FROM ") + t) == Rows{{std::to_string(groups.size())}});
                // (a derived table is read back as text, which loses a space at the end of a value: only the values without one go through it)
                REQUIRE(rows_of(*ex, std::string("SELECT COUNT(*) FROM (SELECT s FROM ") + t + " WHERE s NOT LIKE '% ' GROUP BY s) q") == Rows{{std::to_string(plain_groups.size())}});
                REQUIRE(rows_of(*ex, std::string("SELECT COUNT(*) FROM (SELECT DISTINCT s FROM ") + t + " WHERE s NOT LIKE '% ') q") == Rows{{std::to_string(plain_groups.size())}});
                std::vector<std::size_t> got;
                for (auto& r : rows_of(*ex, std::string("SELECT COUNT(*) FROM ") + t + " GROUP BY s")) got.push_back(std::stoul(r[0]));
                std::sort(got.begin(), got.end());
                REQUIRE(got == sizes);
                // MIN and MAX: a text the collation puts first / last
                std::string first = rows[0].s, last = rows[0].s;
                for (auto& r : rows) {
                    if (collation::compare(r.s, first) < 0) first = r.s;
                    if (collation::compare(r.s, last) > 0) last = r.s;
                }
                auto min_max = rows_of(*ex, std::string("SELECT MIN(s), MAX(s) FROM ") + t);
                REQUIRE(min_max.size() == 1);
                REQUIRE(collation::compare(min_max[0][0], trimmed(first)) == 0);
                REQUIRE(collation::compare(min_max[0][1], trimmed(last)) == 0);
            }
            // joins: the number of pairs of rows with equal texts
            std::size_t pairs = 0;
            for (auto& a : rows) {
                for (auto& b : rows) pairs += collation::compare(a.s, b.s) == 0;
            }
            REQUIRE(rows_of(*ex, "SELECT COUNT(*) FROM plain a JOIN idx b ON a.s = b.s") == Rows{{std::to_string(pairs)}});
            REQUIRE(rows_of(*ex, "SELECT COUNT(*) FROM idx a JOIN hsh b ON a.s = b.s") == Rows{{std::to_string(pairs)}});
            REQUIRE(rows_of(*ex, "SELECT COUNT(*) FROM plain a JOIN keyed b ON a.s = b.s") == Rows{{std::to_string(rows.size())}});
            REQUIRE(rows_of(*ex, "SELECT COUNT(*) FROM plain WHERE s NOT LIKE '% ' AND s IN (SELECT s FROM keyed WHERE s NOT LIKE '% ')") ==
                    Rows{{std::to_string(without_space_at_end)}});

            // a write: an INSERT of a text the collation already has fails, and so does an UPDATE to one
            for (int w = 0; w < 6; w++) {
                const std::string text = kPool[rng() % kPool.size()];
                bool taken = false;
                for (auto& r : keyed) taken = taken || collation::compare(r.s, text) == 0;
                auto result = ex->execute_sql("INSERT INTO keyed VALUES (" + sql_text(text) + ", 1000, 0)");
                INFO("INSERT " << text);
                REQUIRE(result.is_ok() == !taken);
                if (!taken) keyed.push_back({1000 + w, text, 0}), ok(*ex, "UPDATE keyed SET id = " + std::to_string(1000 + w) + " WHERE s = " + sql_text(text));
            }
            if (round == 0) { // after a restart the indexes are built again from the rows and answer the same
                ex_ptr.reset();
                ex_ptr = std::make_unique<Executor>(dir.path);
                ex = ex_ptr.get();
                ok(*ex, "USE d");
            }
        }
    }
    REQUIRE(checks > 0);
}

TEST_CASE("UNIQUE and PRIMARY KEY follow the collation, in INSERT, UPDATE, REPLACE and ON DUPLICATE KEY UPDATE", "[collation]") {
    TempDataDir dir("collation_unique");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE u (id INT PRIMARY KEY AUTO_INCREMENT, email VARCHAR(30) UNIQUE, tag VARCHAR(10))");
    ok(ex, "INSERT INTO u (email, tag) VALUES ('A@x.com', 'p'), ('b@x.com', 'q')");
    REQUIRE(ex.execute_sql("INSERT INTO u (email, tag) VALUES ('a@X.COM', 'r')").is_err());
    REQUIRE(ex.execute_sql("INSERT INTO u (email, tag) VALUES ('B@X.COM', 'r'), ('c@x.com', 's')").is_err());
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM u") == Rows{{"2"}});
    ok(ex, "INSERT INTO u (email, tag) VALUES ('c@x.com', 's')");
    REQUIRE(ex.execute_sql("UPDATE u SET email = 'C@X.com' WHERE email = 'b@x.com'").is_err());
    ok(ex, "UPDATE u SET email = 'B@X.COM' WHERE email = 'b@x.com'"); // (the same text in other case: no conflict with itself)
    ok(ex, "REPLACE INTO u (email, tag) VALUES ('A@X.COM', 't')");
    REQUIRE(rows_of(ex, "SELECT tag FROM u WHERE email = 'a@x.com'") == Rows{{"t"}});
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM u") == Rows{{"3"}});
    ok(ex, "INSERT INTO u (email, tag) VALUES ('c@X.com', 'u') ON DUPLICATE KEY UPDATE tag = 'dup'");
    REQUIRE(rows_of(ex, "SELECT tag FROM u WHERE email = 'C@x.com'") == Rows{{"dup"}});
    ok(ex, "INSERT IGNORE INTO u (email, tag) VALUES ('B@x.com', 'v')");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM u") == Rows{{"3"}});
    // a primary key of text, and a composite unique key over text
    ok(ex, "CREATE TABLE k (name VARCHAR(20) PRIMARY KEY, v INT)");
    ok(ex, "INSERT INTO k VALUES ('Alice', 1), ('bob', 2)");
    REQUIRE(ex.execute_sql("INSERT INTO k VALUES ('ALICE', 3)").is_err());
    REQUIRE(ex.execute_sql("INSERT INTO k VALUES ('new', 3), ('NEW', 4)").is_err());
    REQUIRE(ex.execute_sql("UPDATE k SET name = 'BOB' WHERE name = 'alice'").is_err());
    REQUIRE(rows_of(ex, "SELECT v FROM k WHERE name = 'ALICE'") == Rows{{"1"}});
    ok(ex, "DELETE FROM k WHERE name = 'BOB'");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM k") == Rows{{"1"}});
    // a change of case only: the index entry takes the new spelling and the row stays reachable by any spelling
    ok(ex, "UPDATE k SET name = 'ALICE' WHERE name = 'Alice'");
    REQUIRE(rows_of(ex, "SELECT name, v FROM k WHERE name = 'alice'") == Rows({{"ALICE", "1"}}));
    REQUIRE(rows_of(ex, "SELECT name FROM k WHERE name BETWEEN 'a' AND 'b'") == Rows{{"ALICE"}});
    ok(ex, "UPDATE u SET email = 'A@x.com' WHERE email = 'a@X.COM'");
    REQUIRE(rows_of(ex, "SELECT id FROM u WHERE email = 'a@x.com'").size() == 1);
    ok(ex, "CREATE TABLE c (a VARCHAR(10), b VARCHAR(10), UNIQUE (a, b))");
    ok(ex, "INSERT INTO c VALUES ('x', 'Y')");
    REQUIRE(ex.execute_sql("INSERT INTO c VALUES ('X', 'y')").is_err());
    ok(ex, "INSERT INTO c VALUES ('X', 'z')");
    REQUIRE(ex.execute_sql("UPDATE c SET b = 'y' WHERE b = 'z'").is_err()); // ('X', 'y') is ('x', 'Y')
    ok(ex, "UPDATE c SET b = 'Z' WHERE b = 'z'");
    // ADD UNIQUE over rows that are equal in the collation
    ok(ex, "CREATE TABLE d (id INT PRIMARY KEY, w VARCHAR(10))");
    ok(ex, "INSERT INTO d VALUES (1, 'x'), (2, 'X'), (3, 'y')");
    REQUIRE(ex.execute_sql("ALTER TABLE d ADD UNIQUE (w)").is_err());
    ok(ex, "DELETE FROM d WHERE id = 2");
    ok(ex, "ALTER TABLE d ADD UNIQUE (w)");
    REQUIRE(ex.execute_sql("INSERT INTO d VALUES (4, 'Y')").is_err());

    // the rows of one statement: a repeat in another case is an error, whichever spelling comes first, for a column key and for a composite one
    ok(ex, "CREATE TABLE bt (name VARCHAR(10) PRIMARY KEY)");
    REQUIRE(ex.execute_sql("INSERT INTO bt VALUES ('NEW'), ('new')").is_err());
    REQUIRE(ex.execute_sql("INSERT INTO bt VALUES ('new'), ('NEW')").is_err());
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM bt") == Rows{{"0"}});
    ok(ex, "CREATE TABLE bp (a VARCHAR(5), b VARCHAR(5), PRIMARY KEY (a, b))");
    REQUIRE(ex.execute_sql("INSERT INTO bp VALUES ('A', 'x'), ('a', 'X')").is_err());
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM bp") == Rows{{"0"}});

    // REPLACE: a later row of the statement takes the place of an earlier one that is equal to it in the collation (a UNIQUE column, a composite PRIMARY KEY,
    // a composite UNIQUE key)
    ok(ex, "CREATE TABLE ru (id INT PRIMARY KEY, email VARCHAR(30) UNIQUE)");
    ok(ex, "REPLACE INTO ru VALUES (1, 'a@x.com'), (2, 'A@X.COM')");
    REQUIRE(rows_of(ex, "SELECT id FROM ru") == Rows{{"2"}});
    ok(ex, "CREATE TABLE rp (a VARCHAR(5), b VARCHAR(5), v INT, PRIMARY KEY (a, b))");
    ok(ex, "REPLACE INTO rp VALUES ('x', 'y', 1), ('X', 'Y', 2)");
    REQUIRE(rows_of(ex, "SELECT v FROM rp") == Rows{{"2"}});
    ok(ex, "CREATE TABLE rq (id INT PRIMARY KEY, a VARCHAR(5), b VARCHAR(5), UNIQUE (a, b))");
    ok(ex, "REPLACE INTO rq VALUES (1, 'x', 'y'), (2, 'X', 'Y')");
    REQUIRE(rows_of(ex, "SELECT id FROM rq") == Rows{{"2"}});

    // UPDATE of a UNIQUE column: the values the statement leaves behind must differ, and the rows are written one after the other (in key order, or the order
    // ORDER BY gives): a row takes a value another row holds right now, in any case, and it fails
    ok(ex, "CREATE TABLE up (id INT PRIMARY KEY, s VARCHAR(10) UNIQUE)");
    ok(ex, "INSERT INTO up VALUES (1, 'x'), (2, 'Y')");
    REQUIRE(ex.execute_sql("UPDATE up SET s = IF(id = 1, 'Zed', 'zed')").is_err());
    REQUIRE(ex.execute_sql("UPDATE up SET s = IF(id = 1, 'y', 'z')").is_err());
    ok(ex, "UPDATE up SET s = IF(id = 1, 'y', 'z') ORDER BY id DESC");
    REQUIRE(rows_of(ex, "SELECT id, s FROM up ORDER BY id") == Rows({{"1", "y"}, {"2", "z"}}));
    ok(ex, "CREATE TABLE up2 (id INT PRIMARY KEY, s VARCHAR(10) UNIQUE)");
    ok(ex, "INSERT INTO up2 VALUES (1, 'x'), (2, 'y')");
    ok(ex, "UPDATE up2 SET s = IF(id = 1, 'Y', 'z') ORDER BY id DESC");
    REQUIRE(rows_of(ex, "SELECT id, s FROM up2 ORDER BY id") == Rows({{"1", "Y"}, {"2", "z"}}));
    ok(ex, "INSERT INTO up2 VALUES (3, 'W')");
    REQUIRE(ex.execute_sql("UPDATE up2 SET s = 'w' WHERE id = 1").is_err());
}

TEST_CASE("foreign keys find the parent and the child rows by the collation, in every action", "[collation]") {
    TempDataDir dir("collation_fk");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE fp (code VARCHAR(10) PRIMARY KEY)");
    ok(ex, "INSERT INTO fp VALUES ('abc'), ('Def'), ('ghi')");
    ok(ex, "CREATE TABLE fc (id INT PRIMARY KEY, code VARCHAR(10), FOREIGN KEY (code) REFERENCES fp (code) ON DELETE CASCADE ON UPDATE CASCADE)");
    ok(ex, "INSERT INTO fc VALUES (1, 'ABC'), (2, 'def'), (3, 'GHI')");
    REQUIRE(ex.execute_sql("INSERT INTO fc VALUES (4, 'xyz')").is_err());
    ok(ex, "DELETE FROM fp WHERE code = 'abc'"); // the child 'ABC' goes with it
    REQUIRE(rows_of(ex, "SELECT id, code FROM fc ORDER BY id") == Rows({{"2", "def"}, {"3", "GHI"}}));
    ok(ex, "INSERT INTO fc VALUES (1, 'def')"); // id 1 is free again, in the primary key index too
    REQUIRE(rows_of(ex, "SELECT code FROM fc WHERE id = 1") == Rows{{"def"}});
    ok(ex, "DELETE FROM fc WHERE id = 1");
    ok(ex, "UPDATE fp SET code = 'xyz' WHERE code = 'DEF'"); // the child 'def' follows
    REQUIRE(rows_of(ex, "SELECT id, code FROM fc ORDER BY id") == Rows({{"2", "xyz"}, {"3", "GHI"}}));

    ok(ex, "CREATE TABLE fq (code VARCHAR(10) PRIMARY KEY)");
    ok(ex, "INSERT INTO fq VALUES ('aaa'), ('bbb')");
    ok(ex, "CREATE TABLE fr (id INT PRIMARY KEY, code VARCHAR(10), FOREIGN KEY (code) REFERENCES fq (code) ON DELETE SET NULL ON UPDATE SET NULL)");
    ok(ex, "INSERT INTO fr VALUES (1, 'AAA'), (2, 'BBB')");
    ok(ex, "DELETE FROM fq WHERE code = 'aaa'");
    ok(ex, "UPDATE fq SET code = 'ccc' WHERE code = 'BBB'");
    REQUIRE(rows_of(ex, "SELECT id, code FROM fr ORDER BY id") == Rows({{"1", "NULL"}, {"2", "NULL"}}));

    // SET DEFAULT (RuSQL has it, InnoDB does not)
    ok(ex, "CREATE TABLE fd (code VARCHAR(10) PRIMARY KEY)");
    ok(ex, "INSERT INTO fd VALUES ('base'), ('Tom'), ('Ann')");
    ok(ex, "CREATE TABLE fe (id INT PRIMARY KEY, code VARCHAR(10) DEFAULT 'base', FOREIGN KEY (code) REFERENCES fd (code) ON DELETE SET DEFAULT ON UPDATE SET DEFAULT)");
    ok(ex, "INSERT INTO fe VALUES (1, 'TOM'), (2, 'ann')");
    ok(ex, "DELETE FROM fd WHERE code = 'tom'");
    ok(ex, "UPDATE fd SET code = 'Anna' WHERE code = 'ANN'");
    REQUIRE(rows_of(ex, "SELECT id, code FROM fe ORDER BY id") == Rows({{"1", "base"}, {"2", "base"}}));

    ok(ex, "CREATE TABLE fs (code VARCHAR(10) PRIMARY KEY)");
    ok(ex, "INSERT INTO fs VALUES ('kkk')");
    ok(ex, "CREATE TABLE ft (id INT PRIMARY KEY, code VARCHAR(10), FOREIGN KEY (code) REFERENCES fs (code))");
    ok(ex, "INSERT INTO ft VALUES (1, 'KKK')");
    REQUIRE(ex.execute_sql("DELETE FROM fs WHERE code = 'kkk'").is_err());
    REQUIRE(ex.execute_sql("UPDATE fs SET code = 'zzz' WHERE code = 'KKK'").is_err());
    REQUIRE(rows_of(ex, "SELECT code FROM fs") == Rows{{"kkk"}});

    // a table that refers to itself: the row (or an earlier row of the statement) it names may be spelled in another case
    ok(ex, "CREATE TABLE emp (id VARCHAR(10) PRIMARY KEY, boss VARCHAR(10), FOREIGN KEY (boss) REFERENCES emp (id))");
    ok(ex, "INSERT INTO emp VALUES ('Root', NULL), ('kid', 'ROOT')");
    ok(ex, "INSERT INTO emp VALUES ('Solo', 'solo')");
    REQUIRE(ex.execute_sql("INSERT INTO emp VALUES ('lost', 'nobody')").is_err());
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM emp") == Rows{{"3"}});
}

TEST_CASE("an IN subquery, a recursive CTE and a LIKE prefix scan follow the collation", "[collation]") {
    TempDataDir dir("collation_misc");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE ia (id INT PRIMARY KEY, name VARCHAR(10), n INT)");
    ok(ex, "CREATE TABLE ib (id INT PRIMARY KEY, who VARCHAR(10))");
    ok(ex, "INSERT INTO ia VALUES (1, 'Alice', 1), (2, 'bob', 2), (3, 'Zed', 3)");
    ok(ex, "INSERT INTO ib VALUES (1, 'ALICE'), (2, 'BOB'), (3, 'carol')");
    // (a correlated subquery runs once for each row: its values are compared one by one)
    REQUIRE(ids_of(ex, "SELECT id FROM ia WHERE name IN (SELECT who FROM ib WHERE ib.id <= ia.n) ORDER BY id") == std::vector<int>({1, 2}));
    REQUIRE(ids_of(ex, "SELECT id FROM ia WHERE name NOT IN (SELECT who FROM ib WHERE ib.id <= ia.n) ORDER BY id") == std::vector<int>({3}));
    REQUIRE(ids_of(ex, "SELECT id FROM ia WHERE name IN (SELECT who FROM ib) ORDER BY id") == std::vector<int>({1, 2}));

    // UNION drops a row that repeats one by the collation; so does the recursion of a CTE
    REQUIRE(rows_of(ex, "WITH RECURSIVE r AS (SELECT 'a' AS s UNION SELECT IF(s = 'a', 'A', 'a') FROM r) SELECT COUNT(*) FROM r") == Rows{{"1"}});
    REQUIRE(rows_of(ex, "WITH RECURSIVE r AS (SELECT 1 AS n, 'a' AS s UNION SELECT n, UPPER(s) FROM r WHERE n < 3) SELECT COUNT(*) FROM r") == Rows{{"1"}});
    REQUIRE(rows_of(ex, "WITH RECURSIVE r AS (SELECT 1 AS n, 'a' AS s UNION ALL SELECT n + 1, UPPER(s) FROM r WHERE n < 3) SELECT COUNT(*) FROM r") == Rows{{"3"}});

    // LIKE 'prefix%' through an index finds the rows whatever their case, whichever plan reads them: SELECT without a sort, with ORDER BY the column and a
    // LIMIT, an UPDATE, a DELETE
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, s VARCHAR(20), v INT)");
    ok(ex, "CREATE INDEX t_s ON t (s)");
    static const char* const texts[] = {"ABC", "abd", "Abe", "alpha", "BETA", "b", "gamma", "\xC3\x84rger"};
    struct Item {
        int id;
        std::string s;
    };
    std::vector<Item> rows;
    std::string values;
    for (int i = 1; i <= 160; i++) {
        rows.push_back({i, std::string(texts[i % 8]) + std::to_string(i)});
        values += std::string(i > 1 ? ", " : "") + "(" + std::to_string(i) + ", '" + rows.back().s + "', 0)";
    }
    ok(ex, "INSERT INTO t VALUES " + values);
    auto expected_ids = [&](const std::string& pattern) {
        std::vector<int> out;
        for (auto& r : rows) {
            if (collation::like(r.s, pattern)) out.push_back(r.id);
        }
        return out;
    };
    for (const std::string prefix : {"abe1", "ABC8", "abd1", "arger3", "arh", "alpha4", "BETA5"}) {
        INFO("prefix " << prefix);
        const std::string where = " WHERE s LIKE '" + prefix + "%'";
        REQUIRE(ok_text(ex, "EXPLAIN SELECT id FROM t" + where).find("Index Prefix Scan") != std::string::npos);
        auto got = ids_of(ex, "SELECT id FROM t" + where);
        std::sort(got.begin(), got.end());
        REQUIRE(got == expected_ids(prefix + "%"));
        REQUIRE(ids_of(ex, "SELECT id FROM t" + where + " ORDER BY id") == expected_ids(prefix + "%"));
        // the first rows of the index order: the texts are all different, so their order is the collation's
        std::vector<Item> matching;
        for (auto& r : rows) {
            if (collation::like(r.s, prefix + "%")) matching.push_back(r);
        }
        std::sort(matching.begin(), matching.end(), [](const Item& a, const Item& b) { return collation::compare(a.s, b.s) < 0; });
        std::vector<int> first;
        for (std::size_t i = 0; i < matching.size() && i < 4; i++) first.push_back(matching[i].id);
        REQUIRE(ids_of(ex, "SELECT id FROM t" + where + " ORDER BY s LIMIT 4") == first);
    }
    ok(ex, "UPDATE t SET v = 1 WHERE s LIKE 'aB%'");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM t WHERE v = 1") == Rows{{std::to_string(expected_ids("ab%").size())}});
    ok(ex, "DELETE FROM t WHERE s LIKE 'BE%'");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM t") == Rows{{std::to_string(rows.size() - expected_ids("be%").size())}});
}

TEST_CASE("two sessions that insert one text in two cases meet on one row lock and one gap lock", "[collation][concurrency]") {
    TempDataDir dir("collation_locks");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE t (name VARCHAR(20) PRIMARY KEY, v INT)");
    auto shared = ex.get_shared();
    constexpr int kHoldMs = 300;
    auto millis_since = [](std::chrono::steady_clock::time_point t0) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    };

    // A inserts 'Bob' and holds its transaction open: B's INSERT of 'bob' is a duplicate of a row it can see is there
    {
        std::thread a([&] {
            Executor sess = Executor::new_session(shared);
            REQUIRE(sess.execute_sql("USE d").is_ok());
            REQUIRE(sess.execute_sql("BEGIN").is_ok());
            REQUIRE(sess.execute_sql("INSERT INTO t VALUES ('Bob', 1)").is_ok());
            std::this_thread::sleep_for(std::chrono::milliseconds(kHoldMs));
            REQUIRE(sess.execute_sql("COMMIT").is_ok());
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        Executor b = Executor::new_session(shared);
        REQUIRE(b.execute_sql("USE d").is_ok());
        const auto t0 = std::chrono::steady_clock::now();
        auto result = b.execute_sql("INSERT INTO t VALUES ('bob', 2)");
        const auto waited = millis_since(t0);
        a.join();
        (void)waited;
        REQUIRE(result.is_err());
        REQUIRE(rows_of(b, "SELECT name FROM t") == Rows{{"Bob"}});
    }

    // A locks the range ['a', 'c'] of the key: B's INSERT of 'Bar' (the capital letter is below 'a' by bytes, inside the range by the collation) waits for it
    {
        std::thread a([&] {
            Executor sess = Executor::new_session(shared);
            REQUIRE(sess.execute_sql("USE d").is_ok());
            REQUIRE(sess.execute_sql("SET ISOLATION LEVEL REPEATABLE READ").is_ok());
            REQUIRE(sess.execute_sql("BEGIN").is_ok());
            REQUIRE(sess.execute_sql("SELECT * FROM t WHERE name BETWEEN 'a' AND 'c' FOR UPDATE").is_ok());
            std::this_thread::sleep_for(std::chrono::milliseconds(kHoldMs));
            REQUIRE(sess.execute_sql("COMMIT").is_ok());
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        Executor b = Executor::new_session(shared);
        REQUIRE(b.execute_sql("USE d").is_ok());
        const auto t0 = std::chrono::steady_clock::now();
        auto result = b.execute_sql("INSERT INTO t VALUES ('Bar', 3)");
        const auto waited = millis_since(t0);
        a.join();
        REQUIRE(result.is_ok());
        REQUIRE(waited >= 100);
    }

    // the lock list shows the PK as it was written
    {
        Executor sess = Executor::new_session(shared);
        REQUIRE(sess.execute_sql("USE d").is_ok());
        REQUIRE(sess.execute_sql("BEGIN").is_ok());
        REQUIRE(sess.execute_sql("UPDATE t SET v = 9 WHERE name = 'BOB'").is_ok());
        REQUIRE(ok_text(sess, "SHOW LOCKS").find("Bob") != std::string::npos);
        REQUIRE(sess.execute_sql("ROLLBACK").is_ok());
    }
}

TEST_CASE("sessions racing to insert one text in many cases leave one row for each text", "[collation][concurrency][random]") {
    TempDataDir dir("collation_race");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE t (name VARCHAR(20) PRIMARY KEY, who INT)");
    auto shared = ex.get_shared();
    // (only the PRIMARY KEY: a UNIQUE column that is not the key has no claim between concurrent INSERTs -- two sessions can both pass the check and both write,
    // with identical bytes as well -- a hole that does not belong to the collation)
    constexpr int kThreads = 6, kRounds = 150, kKeys = 12;
    std::vector<std::thread> threads;
    for (int w = 0; w < kThreads; w++) {
        threads.emplace_back([&, w] {
            Executor sess = Executor::new_session(shared);
            REQUIRE(sess.execute_sql("USE d").is_ok());
            std::mt19937 rng(1000 + w);
            for (int i = 0; i < kRounds; i++) {
                std::string text = "Key" + std::to_string(rng() % kKeys);
                for (char& c : text) {
                    if (rng() % 2) c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : (c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
                }
                // (a duplicate is an error; that is the point -- only one of the spellings may get in)
                (void)sess.execute_sql("INSERT INTO t VALUES ('" + text + "', " + std::to_string(w) + ")");
                if (i % 7 == 0) (void)sess.execute_sql("DELETE FROM t WHERE name = '" + text + "'");
            }
        });
    }
    for (auto& t : threads) t.join();
    const auto names = rows_of(ex, "SELECT name FROM t");
    std::set<std::string> keys;
    for (auto& r : names) REQUIRE(keys.insert(collation::key(r[0])).second);
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM t") == Rows{{std::to_string(names.size())}});
}

TEST_CASE("an index built over rows that spell one text in several cases keeps every row, and so does the one rebuilt after a restart", "[collation]") {
    TempDataDir dir("collation_build");
    std::vector<std::vector<int>> ids_of_name(30);
    auto check = [&](Executor& ex) {
        for (int n = 0; n < 30; n++) {
            INFO("name " << n);
            for (const char* spelling : {"name", "Name", "NAME", "nAmE"}) {
                const std::string where = std::string(" WHERE s = '") + spelling + std::to_string(n) + "' ORDER BY id";
                REQUIRE(ids_of(ex, "SELECT id FROM t" + where) == ids_of_name[n]);
                REQUIRE(ids_of(ex, "SELECT id FROM t" + std::string(" WHERE h = '") + spelling + std::to_string(n) + "' ORDER BY id") == ids_of_name[n]);
            }
        }
        REQUIRE(ok_text(ex, "EXPLAIN SELECT id FROM t WHERE s = 'Name3'").find("Index Scan") != std::string::npos);
        REQUIRE(ok_text(ex, "EXPLAIN SELECT id FROM t WHERE h = 'Name3'").find("Hash Index Scan") != std::string::npos);
        REQUIRE(rows_of(ex, "SELECT COUNT(DISTINCT s) FROM t") == Rows{{"30"}});
    };
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, s VARCHAR(20), h VARCHAR(20))");
        std::string values;
        for (int i = 0; i < 300; i++) {
            static const char* const spellings[] = {"name", "Name", "NAME"};
            const std::string text = std::string(spellings[(i / 30) % 3]) + std::to_string(i % 30);
            ids_of_name[i % 30].push_back(i);
            values += std::string(i ? ", " : "") + "(" + std::to_string(i) + ", '" + text + "', '" + text + "')";
        }
        ok(ex, "INSERT INTO t VALUES " + values);
        ok(ex, "CREATE INDEX t_s ON t (s)"); // (built from the rows that are there)
        ok(ex, "CREATE INDEX t_h ON t (h) USING HASH");
        check(ex);
    }
    {
        Executor ex(dir.path); // (built again when the database starts)
        ok(ex, "USE d");
        check(ex);
    }
}

TEST_CASE("the partitions of a text key follow the collation", "[collation][partitioning]") {
    TempDataDir dir("collation_partitions");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE tl (id INT, region VARCHAR(10), PRIMARY KEY(id, region)) PARTITION BY LIST (region) "
           "(PARTITION p_north VALUES IN ('NY', 'MA'), PARTITION p_south VALUES IN ('TX', 'FL'))");
    ok(ex, "INSERT INTO tl VALUES (1, 'ny'), (2, 'Tx'), (3, 'MA')");
    REQUIRE(ex.execute_sql("INSERT INTO tl VALUES (4, 'zz')").is_err());
    REQUIRE(ids_of(ex, "SELECT id FROM tl WHERE region = 'NY' ORDER BY id") == std::vector<int>({1}));
    REQUIRE(ids_of(ex, "SELECT id FROM tl WHERE region = 'tx' ORDER BY id") == std::vector<int>({2}));
    REQUIRE(ids_of(ex, "SELECT id FROM tl WHERE region IN ('ma', 'TX') ORDER BY id") == std::vector<int>({2, 3}));
    ok(ex, "CREATE TABLE tr (id INT, name VARCHAR(10), PRIMARY KEY(id, name)) PARTITION BY RANGE (name) "
           "(PARTITION p0 VALUES LESS THAN ('m'), PARTITION p1 VALUES LESS THAN MAXVALUE)");
    ok(ex, "INSERT INTO tr VALUES (1, 'Zed'), (2, 'zed'), (3, 'Bob'), (4, 'bob'), (5, 'Mia'), (6, 'mia')");
    REQUIRE(ids_of(ex, "SELECT id FROM tr WHERE name = 'zed' ORDER BY id") == std::vector<int>({1, 2}));
    REQUIRE(ids_of(ex, "SELECT id FROM tr WHERE name = 'BOB' ORDER BY id") == std::vector<int>({3, 4}));
    REQUIRE(ids_of(ex, "SELECT id FROM tr WHERE name >= 'M' ORDER BY id") == std::vector<int>({1, 2, 5, 6}));
    REQUIRE(ids_of(ex, "SELECT id FROM tr WHERE name < 'm' ORDER BY id") == std::vector<int>({3, 4}));
    REQUIRE(ids_of(ex, "SELECT id FROM tr WHERE name BETWEEN 'a' AND 'c' ORDER BY id") == std::vector<int>({3, 4}));
}
