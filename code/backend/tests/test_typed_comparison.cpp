#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// Values are text, but MySQL compares by type: two strings compare as strings ('10' < '9', '007' and '7' are different), a number and anything else
// compare as numbers (a string by the number it starts with: 'abc' is 0, '12abc' is 12). The reference below is written out here, independently of the
// engine, from those rules, and every way to reach a row -- scan, primary key, secondary and hash index -- has to agree with it. A VARCHAR primary
// key that mixed numeric-looking and other keys used to miss rows (a tree built on an order that was not transitive); a quoted value used to lose its
// quotes (`name = 'city'` read the column city).

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

void open_db(Executor& ex) {
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
}

void ok(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
}

std::string text(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    if (r.is_err()) INFO("error: " << r.error());
    REQUIRE(r.is_ok());
    return r.value();
}

using Rows = std::vector<std::vector<std::string>>;
const std::string N = "NULL";

Rows cells(const std::string& text) {
    Rows rows;
    std::istringstream in(text);
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

Rows q(Executor& ex, const std::string& sql) { return cells(text(ex, sql)); }

std::vector<int> ids(Executor& ex, const std::string& sql) {
    std::vector<int> out;
    for (auto& r : q(ex, sql)) out.push_back(std::stoi(r[0]));
    return out;
}

std::vector<std::string> first_column(Executor& ex, const std::string& sql) {
    std::vector<std::string> out;
    for (auto& r : q(ex, sql)) out.push_back(r[0]);
    return out;
}

// ---- the reference: MySQL's rules, from a regular expression
double mysql_number(const std::string& s) {
    static const std::regex re(R"(^[ \t\n\r\v\f]*([+-]?(?:[0-9]+\.?[0-9]*|\.[0-9]+)(?:[eE][+-]?[0-9]+)?))");
    std::smatch m;
    if (!std::regex_search(s, m, re)) return 0.0;
    return std::stod(m[1].str());
}

int sign(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }

enum class Cls { Number, Text };

// a column's value against a written value: -1, 0 or 1
int ref_compare(Cls column, const std::string& value, bool quoted, const std::string& written) {
    if (column == Cls::Text && quoted) return sign(value.compare(written)); // two strings: as strings
    const double x = mysql_number(value), y = mysql_number(written);
    return x < y ? -1 : (x > y ? 1 : 0);
}

struct Written {
    std::string sql; // as written in the statement
    std::string text; // its value
    bool quoted;
};

Written written_string(const std::string& v) { return {"'" + v + "'", v, true}; }
Written written_number(const std::string& v) { return {v, v, false}; }
} // namespace

TEST_CASE("every key of a VARCHAR column is found through every kind of index and the scan", "[typed_comparison][keys]") {
    TempDataDir dir("tc_keys");
    auto executor = std::make_unique<Executor>(dir.path);
    Executor& ex = *executor;
    open_db(ex);
    // a primary key, a secondary index, a hash index, and no index at all
    ok(ex, "CREATE TABLE pk (code VARCHAR(12) PRIMARY KEY, n INT)");
    ok(ex, "CREATE TABLE sec (id INT PRIMARY KEY, code VARCHAR(12), n INT)");
    ok(ex, "CREATE INDEX sec_code ON sec (code)");
    ok(ex, "CREATE TABLE hsh (id INT PRIMARY KEY, code VARCHAR(12), n INT)");
    ok(ex, "CREATE INDEX hsh_code ON hsh (code) USING HASH");
    ok(ex, "CREATE TABLE plain (id INT PRIMARY KEY, code VARCHAR(12), n INT)");
    std::mt19937 rng(20261006);
    std::set<std::string> distinct;
    while (distinct.size() < 450) {
        switch (rng() % 6) {
            case 0: distinct.insert(std::to_string(rng() % 400)); break;
            case 1: distinct.insert(std::to_string(rng() % 400) + std::string(1, "abc"[rng() % 3])); break;
            case 2: distinct.insert(std::string(1, "ABC"[rng() % 3]) + std::to_string(rng() % 400)); break;
            case 3: distinct.insert("0" + std::to_string(rng() % 99)); break;
            case 4: distinct.insert(std::to_string(rng() % 99) + "." + std::to_string(rng() % 10)); break;
            default: distinct.insert(std::string(1, static_cast<char>('a' + rng() % 26)) + std::to_string(rng() % 50)); break;
        }
    }
    std::vector<std::string> keys(distinct.begin(), distinct.end());
    std::shuffle(keys.begin(), keys.end(), rng);
    std::string pk_values, other_values;
    for (std::size_t i = 0; i < keys.size(); i++) {
        pk_values += std::string(i ? ", " : "") + "('" + keys[i] + "', " + std::to_string(i) + ")";
        other_values += std::string(i ? ", " : "") + "(" + std::to_string(i) + ", '" + keys[i] + "', " + std::to_string(i) + ")";
    }
    ok(ex, "INSERT INTO pk VALUES " + pk_values);
    for (const char* t : {"sec", "hsh", "plain"}) ok(ex, std::string("INSERT INTO ") + t + " VALUES " + other_values);
    for (std::size_t i = 0; i < keys.size(); i++) {
        const std::string n = std::to_string(i);
        INFO("key " << keys[i]);
        REQUIRE(first_column(ex, "SELECT n FROM pk WHERE code = '" + keys[i] + "'") == std::vector<std::string>{n});
        for (const char* t : {"sec", "hsh", "plain"}) {
            REQUIRE(first_column(ex, std::string("SELECT n FROM ") + t + " WHERE code = '" + keys[i] + "'") == std::vector<std::string>{n});
        }
    }
    // byte order, however the keys look
    std::vector<std::string> sorted = keys;
    std::sort(sorted.begin(), sorted.end());
    REQUIRE(first_column(ex, "SELECT code FROM pk ORDER BY code") == sorted);
    REQUIRE(first_column(ex, "SELECT code FROM sec ORDER BY code") == sorted);
    // a range of texts is a range of bytes: '10' < '9' < '9a'
    std::vector<std::string> in_range;
    for (auto& k : sorted) {
        if (k > "10" && k <= "9") in_range.push_back(k);
    }
    REQUIRE(first_column(ex, "SELECT code FROM pk WHERE code > '10' AND code <= '9' ORDER BY code") == in_range);
    REQUIRE(first_column(ex, "SELECT code FROM sec WHERE code > '10' AND code <= '9' ORDER BY code") == in_range);
    REQUIRE(first_column(ex, "SELECT code FROM plain WHERE code > '10' AND code <= '9' ORDER BY code") == in_range);
    // a one-sided range is a range of bytes through the primary key, the secondary index and the hash index (which cannot, so the scan) too
    for (const std::string bound : {"10", "9", "7", "0", "A5", "a5", "zz", "5.5"}) {
        for (const std::string op : {">", ">=", "<", "<="}) {
            const std::string where = "code " + op + " '" + bound + "'";
            std::vector<std::string> expected;
            for (auto& k : sorted) {
                const int c = k.compare(bound);
                if (op == ">" ? c > 0 : op == ">=" ? c >= 0 : op == "<" ? c < 0 : c <= 0) expected.push_back(k);
            }
            INFO(where);
            // (without ORDER BY the statement takes the index paths; the rows come in the order of the index, so they are compared sorted)
            for (const char* t : {"pk", "sec", "hsh", "plain"}) {
                auto got = first_column(ex, std::string("SELECT code FROM ") + t + " WHERE " + where);
                std::sort(got.begin(), got.end());
                REQUIRE(got == expected);
                auto ordered = first_column(ex, std::string("SELECT code FROM ") + t + " WHERE " + where + " ORDER BY code");
                REQUIRE(ordered == expected);
            }
        }
    }
    // UPDATE and DELETE by a range reach the same rows through the indexes (the statement picks the rows with the same tree)
    {
        ok(ex, "CREATE TABLE dpk (code VARCHAR(12) PRIMARY KEY, n INT)");
        ok(ex, "CREATE TABLE dsec (id INT PRIMARY KEY, code VARCHAR(12), n INT)");
        ok(ex, "CREATE INDEX dsec_code ON dsec (code)");
        ok(ex, "CREATE TABLE dplain (id INT PRIMARY KEY, code VARCHAR(12), n INT)");
        ok(ex, "INSERT INTO dpk VALUES " + pk_values);
        ok(ex, "INSERT INTO dsec VALUES " + other_values);
        ok(ex, "INSERT INTO dplain VALUES " + other_values);
        std::vector<std::string> above, rest;
        for (auto& k : sorted) {
            if (k > "10") above.push_back(k);
            if (!(k < "9")) rest.push_back(k);
        }
        for (const char* t : {"dpk", "dsec", "dplain"}) {
            INFO(t);
            ok(ex, std::string("UPDATE ") + t + " SET n = n + 100000 WHERE code > '10'");
            REQUIRE(first_column(ex, std::string("SELECT code FROM ") + t + " WHERE n >= 100000 ORDER BY code") == above);
            ok(ex, std::string("DELETE FROM ") + t + " WHERE code < '9'");
            REQUIRE(first_column(ex, std::string("SELECT code FROM ") + t + " ORDER BY code") == rest);
        }
    }
    // duplicates are texts that are equal: '007' and '7' are two keys
    ok(ex, "INSERT INTO pk VALUES ('007x', 1)");
    auto dup = ex.execute_sql("INSERT INTO pk VALUES ('007x', 2)");
    REQUIRE(dup.is_err());
    // a transaction that is rolled back leaves the texts findable (the trees are copied and put back)
    ok(ex, "BEGIN");
    ok(ex, "INSERT INTO pk VALUES ('9z9', 7777)");
    ok(ex, "DELETE FROM pk WHERE code = '" + keys[0] + "'");
    ok(ex, "ROLLBACK");
    REQUIRE(first_column(ex, "SELECT n FROM pk WHERE code = '9z9'").empty());
    for (std::size_t i = 0; i < keys.size(); i += 5) {
        REQUIRE(first_column(ex, "SELECT n FROM pk WHERE code = '" + keys[i] + "'") == std::vector<std::string>{std::to_string(i)});
    }
    // and a restart rebuilds every index the same way
    executor.reset();
    Executor restarted(dir.path);
    ok(restarted, "USE d");
    for (std::size_t i = 0; i < keys.size(); i += 7) {
        const std::string n = std::to_string(i);
        REQUIRE(first_column(restarted, "SELECT n FROM pk WHERE code = '" + keys[i] + "'") == std::vector<std::string>{n});
        REQUIRE(first_column(restarted, "SELECT n FROM sec WHERE code = '" + keys[i] + "'") == std::vector<std::string>{n});
        REQUIRE(first_column(restarted, "SELECT n FROM hsh WHERE code = '" + keys[i] + "'") == std::vector<std::string>{n});
    }
    REQUIRE(first_column(restarted, "SELECT code FROM pk ORDER BY code").size() == keys.size() + 1);
}

TEST_CASE("a comparison follows the types of its sides", "[typed_comparison][compare]") {
    TempDataDir dir("tc_compare");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE z (id INT PRIMARY KEY, code VARCHAR(10), n INT, p DECIMAL(10,2))");
    ok(ex, "INSERT INTO z VALUES (1, '007', 7, 7.00), (2, '7', 7, 7.50), (3, '10', 10, 10.00), (4, '9', 9, 9.00), (5, 'abc', 0, 0.00), (6, NULL, NULL, NULL), "
           "(7, '7.0', 8, 7.00), (8, '12abc', 12, 12.00), (9, '', 0, 0.10), (10, ' 5', 5, 5.00)");
    // a text column: a string compares as a string, a number as a number
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code = '7' ORDER BY id") == std::vector<int>{2});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code = '007' ORDER BY id") == std::vector<int>{1});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code = 7 ORDER BY id") == std::vector<int>{1, 2, 7});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code = 7.0 ORDER BY id") == std::vector<int>{1, 2, 7});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code > '5' ORDER BY id") == std::vector<int>{2, 4, 5, 7});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code < '10' ORDER BY id") == std::vector<int>{1, 9, 10}); // ('007', '' and ' 5' come before '10')
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code < 9 ORDER BY id") == std::vector<int>{1, 2, 5, 7, 9, 10});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code BETWEEN '1' AND '8' ORDER BY id") == std::vector<int>{2, 3, 7, 8}); // ('7', '10', '7.0', '12abc')
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code BETWEEN 1 AND 8 ORDER BY id") == std::vector<int>{1, 2, 7, 10});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code IN ('7', '10', 'abc') ORDER BY id") == std::vector<int>{2, 3, 5});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code IN (7, 10) ORDER BY id") == std::vector<int>{1, 2, 3, 7});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code NOT IN ('7', '10') ORDER BY id") == std::vector<int>{1, 4, 5, 7, 8, 9, 10});
    // a number column compares as numbers whatever the other side is
    REQUIRE(ids(ex, "SELECT id FROM z WHERE n = '7' ORDER BY id") == std::vector<int>{1, 2});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE n = '7abc' ORDER BY id") == std::vector<int>{1, 2});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE n = 'abc' ORDER BY id") == std::vector<int>{5, 9});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE n > '9' ORDER BY id") == std::vector<int>{3, 8});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE p = 7 ORDER BY id") == std::vector<int>{1, 7});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE p = '7.00' ORDER BY id") == std::vector<int>{1, 7});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE p < '7.5' ORDER BY id") == std::vector<int>{1, 5, 7, 9, 10});
    // two columns: a text and a number compare as numbers, two texts as texts
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code = n ORDER BY id") == std::vector<int>{1, 2, 3, 4, 5, 8, 9, 10});
    ok(ex, "CREATE TABLE z2 (id INT PRIMARY KEY, a VARCHAR(10), b VARCHAR(10))");
    ok(ex, "INSERT INTO z2 VALUES (1, '7', '07'), (2, '7', '7'), (3, 'x', 'x'), (4, '10', '9'), (5, '', ''), (6, NULL, 'x')");
    REQUIRE(ids(ex, "SELECT id FROM z2 WHERE a = b ORDER BY id") == std::vector<int>{2, 3, 5});
    REQUIRE(ids(ex, "SELECT id FROM z2 WHERE a < b ORDER BY id") == std::vector<int>{4});
    REQUIRE(ids(ex, "SELECT id FROM z2 WHERE a > b ORDER BY id") == std::vector<int>{1});
    // literals alone
    REQUIRE(q(ex, "SELECT '10' < '9', 10 < 9, '10' < 9, '007' = '7', 007 = 7, '1.0' = '1', 1.0 = 1, 'a' < 'b', 'abc' = 0") ==
            Rows{{"1", "0", "0", "0", "1", "0", "1", "1", "1"}});
    // a composite index: a text column compared with a number reads each text by its number, which no key of texts answers
    ok(ex, "CREATE TABLE cx (id INT PRIMARY KEY, s VARCHAR(10), n INT)");
    ok(ex, "CREATE INDEX cx_sn ON cx (s, n)");
    ok(ex, "INSERT INTO cx VALUES (1, '7', 1), (2, '007', 1), (3, 'abc', 1), (4, '7', 2), (5, '', 1), (6, '0', 1)");
    REQUIRE(ids(ex, "SELECT id FROM cx WHERE s = '7' AND n = 1 ORDER BY id") == std::vector<int>{1});
    REQUIRE(ids(ex, "SELECT id FROM cx WHERE s = 7 AND n = 1 ORDER BY id") == std::vector<int>{1, 2});
    REQUIRE(ids(ex, "SELECT id FROM cx WHERE s = 0 AND n = 1 ORDER BY id") == std::vector<int>{3, 5, 6});
    REQUIRE(ids(ex, "SELECT id FROM cx WHERE s = '' AND n = 1 ORDER BY id") == std::vector<int>{5});
    REQUIRE(ids(ex, "SELECT id FROM cx WHERE s = 'abc' AND n = 1.0 ORDER BY id") == std::vector<int>{3});
    // BETWEEN with a NULL bound (a variable that holds NULL): `x >= NULL AND x <= hi` is FALSE when x > hi and UNKNOWN otherwise, so BETWEEN
    // never selects a row and NOT BETWEEN selects the rows the other bound rules out
    ok(ex, "SET @nul = NULL");
    REQUIRE(ids(ex, "SELECT id FROM z WHERE n BETWEEN @nul AND 8 ORDER BY id").empty());
    REQUIRE(ids(ex, "SELECT id FROM z WHERE n BETWEEN 8 AND @nul ORDER BY id").empty());
    REQUIRE(ids(ex, "SELECT id FROM z WHERE n NOT BETWEEN @nul AND 8 ORDER BY id") == std::vector<int>{3, 4, 8}); // n > 8: 10, 9, 12
    REQUIRE(ids(ex, "SELECT id FROM z WHERE n NOT BETWEEN 8 AND @nul ORDER BY id") == std::vector<int>{1, 2, 5, 9, 10}); // n < 8
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code BETWEEN @nul AND 'zzz' ORDER BY id").empty());
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code NOT BETWEEN 'zzz' AND @nul ORDER BY id") == std::vector<int>{1, 2, 3, 4, 5, 7, 8, 9, 10}); // code < 'zzz'
    // the same comparisons through every index
    ok(ex, "CREATE INDEX zc ON z (code)");
    ok(ex, "CREATE INDEX zn ON z (n)");
    ok(ex, "CREATE INDEX zp ON z (p)");
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code = '7' ORDER BY id") == std::vector<int>{2});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code = 7 ORDER BY id") == std::vector<int>{1, 2, 7});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE code > '5' ORDER BY id") == std::vector<int>{2, 4, 5, 7});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE n = '7abc' ORDER BY id") == std::vector<int>{1, 2});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE p = '7.00' ORDER BY id") == std::vector<int>{1, 7});
    REQUIRE(ids(ex, "SELECT id FROM z WHERE n BETWEEN '8' AND 10 ORDER BY id") == std::vector<int>{3, 4, 7});
}

TEST_CASE("every comparison of a random table matches the reference with and without an index", "[typed_comparison][random]") {
    unsigned seed_count = 3; // RUSQL_FUZZ_SEEDS=60 runs a longer campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 0;
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    static const char* strings[] = {"7", "007", "7.0", "07", "10", "9", "abc", "ABC", "12abc", "", " 5", "-3", "1e1", "x", "9a", "10a"};
    static const char* quoted_pool[] = {"7", "007", "7.0", "10", "9", "abc", "12abc", "", "x", "-3", "5", "9a"};
    static const char* number_pool[] = {"7", "7.0", "007", "10", "9", "0", "-3", "7.5", "12", "5"};
    for (unsigned k = seed_start; k < seed_start + seed_count; k++) {
        INFO("seed " << k);
        std::mt19937 rng(8800 + k);
        TempDataDir dir("tc_random");
        Executor ex(dir.path);
        open_db(ex);
        // the same rows twice: `plain` has no index, `indexed` has an index on every column (a hash index on the text column of `hashed`)
        ok(ex, "CREATE TABLE plain (id INT PRIMARY KEY, s VARCHAR(10), n INT, p DECIMAL(10,2))");
        ok(ex, "CREATE TABLE indexed (id INT PRIMARY KEY, s VARCHAR(10), n INT, p DECIMAL(10,2))");
        ok(ex, "CREATE INDEX i_s ON indexed (s)");
        ok(ex, "CREATE INDEX i_n ON indexed (n)");
        ok(ex, "CREATE INDEX i_p ON indexed (p)");
        ok(ex, "CREATE TABLE hashed (id INT PRIMARY KEY, s VARCHAR(10), n INT, p DECIMAL(10,2))");
        ok(ex, "CREATE INDEX h_s ON hashed (s) USING HASH");
        ok(ex, "CREATE INDEX h_n ON hashed (n) USING HASH");
        ok(ex, "CREATE TABLE keyed (s VARCHAR(10) PRIMARY KEY, id INT, n INT, p DECIMAL(10,2))");
        struct Row {
            int id;
            bool s_null, n_null, p_null;
            std::string s;
            int n;
            std::string p;
        };
        std::vector<Row> rows;
        std::set<std::string> keyed_seen;
        std::string values, keyed_values;
        for (int i = 1; i <= 80; i++) {
            Row r{i, rng() % 9 == 0, rng() % 9 == 0, rng() % 9 == 0, strings[rng() % 16], static_cast<int>(rng() % 21) - 5, ""};
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(static_cast<int>(rng() % 3001) - 500) / 100.0);
            r.p = buf;
            rows.push_back(r);
            const std::string row_sql = "(" + std::to_string(i) + ", " + (r.s_null ? "NULL" : "'" + r.s + "'") + ", " + (r.n_null ? "NULL" : std::to_string(r.n)) + ", " +
                                        (r.p_null ? "NULL" : r.p) + ")";
            values += (i > 1 ? ", " : "") + row_sql;
            if (!r.s_null && keyed_seen.insert(r.s).second) {
                keyed_values += std::string(keyed_values.empty() ? "" : ", ") + "('" + r.s + "', " + std::to_string(i) + ", " + (r.n_null ? "NULL" : std::to_string(r.n)) + ", " +
                                (r.p_null ? "NULL" : r.p) + ")";
            }
        }
        for (const char* t : {"plain", "indexed", "hashed"}) ok(ex, std::string("INSERT INTO ") + t + " VALUES " + values);
        ok(ex, "INSERT INTO keyed VALUES " + keyed_values);

        for (int iter = 0; iter < 140; iter++) {
            struct Pred {
                std::string sql;
                std::function<bool(const Row&)> holds; // true; unknown and false are both "not selected"
            };
            auto make = [&]() -> Pred {
                const int column = static_cast<int>(rng() % 3); // 0 s (text), 1 n, 2 p (numbers)
                const char* col = column == 0 ? "s" : (column == 1 ? "n" : "p");
                auto value_of = [column](const Row& r) -> std::pair<bool, std::string> {
                    if (column == 0) return {!r.s_null, r.s};
                    if (column == 1) return {!r.n_null, std::to_string(r.n)};
                    return {!r.p_null, r.p};
                };
                const Cls cls = column == 0 ? Cls::Text : Cls::Number;
                auto pick = [&]() -> Written {
                    if (rng() % 2 == 0) return written_string(quoted_pool[rng() % 12]);
                    return written_number(number_pool[rng() % 10]);
                };
                const int shape = static_cast<int>(rng() % 5);
                if (shape <= 2) {
                    static const char* ops[] = {"=", "<>", "<", "<=", ">", ">="};
                    const int op = static_cast<int>(rng() % 6);
                    Written w = pick();
                    return {std::string(col) + " " + ops[op] + " " + w.sql, [=](const Row& r) {
                                auto [present, v] = value_of(r);
                                if (!present) return false;
                                const int c = ref_compare(cls, v, w.quoted, w.text);
                                switch (op) {
                                    case 0: return c == 0;
                                    case 1: return c != 0;
                                    case 2: return c < 0;
                                    case 3: return c <= 0;
                                    case 4: return c > 0;
                                    default: return c >= 0;
                                }
                            }};
                }
                if (shape == 3) {
                    Written lo = pick(), hi = pick();
                    const bool negated = rng() % 4 == 0;
                    return {std::string(col) + (negated ? " NOT BETWEEN " : " BETWEEN ") + lo.sql + " AND " + hi.sql, [=](const Row& r) {
                                auto [present, v] = value_of(r);
                                if (!present) return false;
                                const bool inside = ref_compare(cls, v, lo.quoted, lo.text) >= 0 && ref_compare(cls, v, hi.quoted, hi.text) <= 0;
                                return negated ? !inside : inside;
                            }};
                }
                std::vector<Written> list;
                for (std::size_t n = 1 + rng() % 3, i = 0; i < n; i++) list.push_back(pick());
                const bool negated = rng() % 3 == 0;
                std::string items;
                for (auto& w : list) items += (items.empty() ? "" : ", ") + w.sql;
                return {std::string(col) + (negated ? " NOT IN (" : " IN (") + items + ")", [=](const Row& r) {
                            auto [present, v] = value_of(r);
                            if (!present) return false;
                            bool any = false;
                            for (auto& w : list) any = any || ref_compare(cls, v, w.quoted, w.text) == 0;
                            return negated ? !any : any;
                        }};
            };
            Pred a = make();
            std::string sql_cond = a.sql;
            std::function<bool(const Row&)> holds = a.holds;
            if (rng() % 3 == 0) { // an AND of two
                Pred b = make();
                sql_cond += " AND " + b.sql;
                holds = [a, b](const Row& r) { return a.holds(r) && b.holds(r); };
            }
            std::vector<int> expected;
            for (auto& r : rows) {
                if (holds(r)) expected.push_back(r.id);
            }
            INFO(sql_cond);
            for (const char* t : {"plain", "indexed", "hashed"}) {
                INFO(t);
                REQUIRE(ids(ex, std::string("SELECT id FROM ") + t + " WHERE " + sql_cond + " ORDER BY id") == expected);
            }
            // (the table keyed by its text column holds each text once)
            std::vector<int> expected_keyed;
            std::set<std::string> first_seen;
            for (auto& r : rows) {
                if (!r.s_null && first_seen.insert(r.s).second && holds(r)) expected_keyed.push_back(r.id);
            }
            REQUIRE(ids(ex, "SELECT id FROM keyed WHERE " + sql_cond + " ORDER BY id") == expected_keyed);
        }
    }
}

TEST_CASE("a quoted value is a string, never the name of a column", "[typed_comparison][quoted]") {
    TempDataDir dir("tc_quoted");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, name VARCHAR(10), city VARCHAR(10), n INT)");
    ok(ex, "INSERT INTO t VALUES (1, 'city', 'x', 1), (2, 'bob', 'bob', 2), (3, 'name', 'name', 3), (4, 'city', 'city', 4), (5, 'n', 'y', 5)");
    // `name = city` compares two columns, `name = 'city'` a column with a string
    REQUIRE(ids(ex, "SELECT id FROM t WHERE name = city ORDER BY id") == std::vector<int>{2, 3, 4});
    REQUIRE(ids(ex, "SELECT id FROM t WHERE name = 'city' ORDER BY id") == std::vector<int>{1, 4});
    REQUIRE(ids(ex, "SELECT id FROM t WHERE name <> 'city' ORDER BY id") == std::vector<int>{2, 3, 5});
    REQUIRE(ids(ex, "SELECT id FROM t WHERE name IN ('city', 'n') ORDER BY id") == std::vector<int>{1, 4, 5});
    REQUIRE(ids(ex, "SELECT id FROM t WHERE name BETWEEN 'bob' AND 'city' ORDER BY id") == std::vector<int>{1, 2, 4});
    REQUIRE(ids(ex, "SELECT id FROM t WHERE name = 'n' ORDER BY id") == std::vector<int>{5});
    // a string that names the column of another table, an alias, a column of the same table
    REQUIRE(ids(ex, "SELECT id FROM t x WHERE x.name = 'x.city' ORDER BY id").empty());
    REQUIRE(ids(ex, "SELECT id FROM t WHERE name = 't.city' ORDER BY id").empty());
    ok(ex, "INSERT INTO t VALUES (6, 't.city', 'q', 6)");
    REQUIRE(ids(ex, "SELECT id FROM t WHERE name = 't.city' ORDER BY id") == std::vector<int>{6});
    // (the alias of the table is not put in front of a string: 'x.city' stays 'x.city', and 't.city' is what a column named so holds)
    REQUIRE(ids(ex, "SELECT id FROM t x WHERE x.name = 'x.city' ORDER BY id").empty());
    ok(ex, "INSERT INTO t VALUES (7, 'x.city', 'q', 7)");
    REQUIRE(ids(ex, "SELECT id FROM t x WHERE x.name = 'x.city' ORDER BY id") == std::vector<int>{7});
    REQUIRE(ids(ex, "SELECT id FROM t x WHERE x.name BETWEEN 'x.city' AND 'x.city' ORDER BY id") == std::vector<int>{7});
    ok(ex, "DELETE FROM t WHERE id = 7");
    // a key that is the name of a column, written in an ON DUPLICATE KEY UPDATE, REPLACE and a multi-row INSERT
    ok(ex, "CREATE TABLE k (name VARCHAR(10) PRIMARY KEY, v INT)");
    ok(ex, "INSERT INTO k VALUES ('name', 1), ('other', 1), ('v', 1)");
    ok(ex, "INSERT INTO k VALUES ('name', 5) ON DUPLICATE KEY UPDATE v = 2");
    REQUIRE(q(ex, "SELECT name, v FROM k ORDER BY name") == Rows{{"name", "2"}, {"other", "1"}, {"v", "1"}});
    ok(ex, "REPLACE INTO k VALUES ('v', 9)");
    REQUIRE(q(ex, "SELECT name, v FROM k ORDER BY name") == Rows{{"name", "2"}, {"other", "1"}, {"v", "9"}});
    ok(ex, "UPDATE k SET v = 0 WHERE name = 'v'");
    REQUIRE(q(ex, "SELECT name, v FROM k ORDER BY name") == Rows{{"name", "2"}, {"other", "1"}, {"v", "0"}});
    // the same through the indexes
    ok(ex, "CREATE INDEX t_name ON t (name)");
    REQUIRE(ids(ex, "SELECT id FROM t WHERE name = 'city' ORDER BY id") == std::vector<int>{1, 4});
    REQUIRE(ids(ex, "SELECT id FROM t WHERE name = city ORDER BY id") == std::vector<int>{2, 3, 4});
    // UPDATE and DELETE
    ok(ex, "UPDATE t SET n = 99 WHERE name = 'city'");
    REQUIRE(ids(ex, "SELECT id FROM t WHERE n = 99 ORDER BY id") == std::vector<int>{1, 4});
    ok(ex, "DELETE FROM t WHERE name = 'bob'");
    REQUIRE(ids(ex, "SELECT id FROM t ORDER BY id") == std::vector<int>{1, 3, 4, 5, 6});
    // a value held by a variable is a string unless it is a number
    ok(ex, "SET @w = 'city'");
    REQUIRE(ids(ex, "SELECT id FROM t WHERE name = @w ORDER BY id") == std::vector<int>{1, 4});

    // the statements the engine builds from key values (REPLACE, ON DUPLICATE KEY UPDATE, MERGE, DELETE through a join) name the key as a string
    // even when the key is the name of a column of the table: `a = 'b'` is not `a = b`
    {
        ok(ex, "CREATE TABLE rp (a VARCHAR(10) PRIMARY KEY, b VARCHAR(10))");
        ok(ex, "INSERT INTO rp VALUES ('x', 'x'), ('y', 'z'), ('b', 'q')");
        ok(ex, "REPLACE INTO rp VALUES ('b', 'new')");
        REQUIRE(q(ex, "SELECT a, b FROM rp ORDER BY a") == Rows{{"b", "new"}, {"x", "x"}, {"y", "z"}});
        ok(ex, "INSERT INTO rp VALUES ('b', 'ins') ON DUPLICATE KEY UPDATE b = 'upd'");
        REQUIRE(q(ex, "SELECT a, b FROM rp ORDER BY a") == Rows{{"b", "upd"}, {"x", "x"}, {"y", "z"}});
        ok(ex, "CREATE TABLE ms (a VARCHAR(10), b VARCHAR(10))");
        ok(ex, "INSERT INTO ms VALUES ('b', 'gone')");
        ok(ex, "MERGE INTO rp USING ms ON rp.a = ms.a WHEN MATCHED THEN DELETE");
        REQUIRE(q(ex, "SELECT a, b FROM rp ORDER BY a") == Rows{{"x", "x"}, {"y", "z"}});
        ok(ex, "CREATE TABLE ct (a VARCHAR(5), b VARCHAR(5), PRIMARY KEY (a, b))");
        ok(ex, "CREATE TABLE cu (x VARCHAR(5))");
        ok(ex, "INSERT INTO ct VALUES ('a', 'b'), ('c', 'd'), ('e', 'f'), ('a', 'a')");
        ok(ex, "INSERT INTO cu VALUES ('a')");
        ok(ex, "DELETE ct FROM ct JOIN cu ON ct.a = cu.x WHERE ct.b = 'b'");
        REQUIRE(q(ex, "SELECT a, b FROM ct ORDER BY a, b") == Rows{{"a", "a"}, {"c", "d"}, {"e", "f"}});
    }
}

TEST_CASE("ORDER BY, MIN and MAX follow the type of the column", "[typed_comparison][order]") {
    TempDataDir dir("tc_order");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE o (id INT PRIMARY KEY, s VARCHAR(10), n INT, big BIGINT, p DECIMAL(10,2))");
    ok(ex, "INSERT INTO o VALUES (1, '10', 10, 9007199254740993, 10.5), (2, '9', 9, 9007199254740992, 9.25), (3, '007', 7, 9007199254740991, 100.00), "
           "(4, 'abc', 100, 9007199254740994, 2.00), (5, '1', 1, 9007199254740992, -1.50), (6, NULL, NULL, NULL, NULL), (7, '100', 20, 5, 7.00)");
    // a text column sorts as text, a number column as numbers, NULL first
    REQUIRE(first_column(ex, "SELECT s FROM o ORDER BY s") == std::vector<std::string>{N, "007", "1", "10", "100", "9", "abc"});
    REQUIRE(first_column(ex, "SELECT s FROM o ORDER BY s DESC") == std::vector<std::string>{"abc", "9", "100", "10", "1", "007", N});
    REQUIRE(first_column(ex, "SELECT n FROM o ORDER BY n") == std::vector<std::string>{N, "1", "7", "9", "10", "20", "100"});
    REQUIRE(first_column(ex, "SELECT p FROM o ORDER BY p DESC") == std::vector<std::string>{"100.00", "10.50", "9.25", "7.00", "2.00", "-1.50", N});
    // integers beyond 2^53 are compared as integers
    REQUIRE(first_column(ex, "SELECT big FROM o ORDER BY big") == std::vector<std::string>{N, "5", "9007199254740991", "9007199254740992", "9007199254740992", "9007199254740993",
                                                                                            "9007199254740994"});
    REQUIRE(ids(ex, "SELECT id FROM o WHERE big > 9007199254740992 ORDER BY id") == std::vector<int>{1, 4});
    REQUIRE(ids(ex, "SELECT id FROM o WHERE big = 9007199254740993 ORDER BY id") == std::vector<int>{1});
    REQUIRE(ids(ex, "SELECT id FROM o WHERE big IN (9007199254740991, 9007199254740994) ORDER BY id") == std::vector<int>{3, 4});
    REQUIRE(ids(ex, "SELECT id FROM o WHERE big BETWEEN 9007199254740992 AND 9007199254740993 ORDER BY id") == std::vector<int>{1, 2, 5});
    // MIN and MAX: a text column's by text, a number column's by value
    REQUIRE(q(ex, "SELECT MIN(s), MAX(s), MIN(n), MAX(n), MIN(big), MAX(big), MIN(p), MAX(p) FROM o") ==
            Rows{{"007", "abc", "1", "100", "5", "9007199254740994", "-1.50", "100.00"}});
    REQUIRE(q(ex, "SELECT MIN(s), MAX(s) FROM o WHERE s <> 'abc'") == Rows{{"007", "9"}});
    // (a text column whose values all look like numbers is still text: the MAX of '10' and '9' is '9')
    ok(ex, "CREATE TABLE lookalike (id INT PRIMARY KEY, s VARCHAR(10), n INT)");
    ok(ex, "INSERT INTO lookalike VALUES (1, '10', 10), (2, '9', 9), (3, '100', 100), (4, '9.5', 95)");
    REQUIRE(q(ex, "SELECT MIN(s), MAX(s), MIN(n), MAX(n) FROM lookalike") == Rows{{"10", "9.5", "9", "100"}});
    REQUIRE(first_column(ex, "SELECT s FROM lookalike ORDER BY s") == std::vector<std::string>{"10", "100", "9", "9.5"});
    REQUIRE(first_column(ex, "SELECT id FROM lookalike ORDER BY s DESC, id") == std::vector<std::string>{"4", "2", "3", "1"});
    REQUIRE(q(ex, "SELECT id, MIN(s) OVER (), MAX(s) OVER () FROM lookalike WHERE id <= 2 ORDER BY id") == Rows{{"1", "10", "9"}, {"2", "10", "9"}});
    // in a group, in HAVING, over an expression with MAX, in a window
    ok(ex, "CREATE TABLE g (id INT PRIMARY KEY, grp INT, s VARCHAR(10))");
    ok(ex, "INSERT INTO g VALUES (1, 1, '10'), (2, 1, '9'), (3, 2, '7'), (4, 2, '10'), (5, 3, 'a')");
    REQUIRE(q(ex, "SELECT grp, MAX(s), MIN(s) FROM g GROUP BY grp ORDER BY grp") == Rows{{"1", "9", "10"}, {"2", "7", "10"}, {"3", "a", "a"}});
    REQUIRE(q(ex, "SELECT grp FROM g GROUP BY grp HAVING MAX(s) > '8' ORDER BY grp") == Rows{{"1"}, {"3"}}); // (the MAX of '7' and '10' is '7')
    REQUIRE(q(ex, "SELECT grp FROM g GROUP BY grp HAVING MAX(s) = '9' ORDER BY grp") == Rows{{"1"}});
    REQUIRE(q(ex, "SELECT id, MAX(s) OVER (PARTITION BY grp), MIN(s) OVER (PARTITION BY grp) FROM g ORDER BY id") ==
            Rows{{"1", "9", "10"}, {"2", "9", "10"}, {"3", "7", "10"}, {"4", "7", "10"}, {"5", "a", "a"}});
    REQUIRE(first_column(ex, "SELECT id FROM g ORDER BY s, id") == std::vector<std::string>{"1", "4", "3", "2", "5"});
    REQUIRE(q(ex, "SELECT DISTINCT s FROM g ORDER BY s") == Rows{{"10"}, {"7"}, {"9"}, {"a"}});
    // the sort of a window follows the type too
    REQUIRE(q(ex, "SELECT id, RANK() OVER (ORDER BY s) FROM g ORDER BY id") == Rows{{"1", "1"}, {"2", "4"}, {"3", "3"}, {"4", "1"}, {"5", "5"}});
}

TEST_CASE("joins meet text columns by their texts and number columns by value", "[typed_comparison][join]") {
    TempDataDir dir("tc_join");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE a (id INT PRIMARY KEY, code VARCHAR(10), n INT)");
    ok(ex, "CREATE TABLE b (id INT PRIMARY KEY, code VARCHAR(10), n INT, tag VARCHAR(10))");
    ok(ex, "INSERT INTO a VALUES (1, '7', 7), (2, '007', 7), (3, '7.0', 8), (4, 'abc', 0), (5, NULL, 9), (6, '', 0)");
    ok(ex, "INSERT INTO b VALUES (1, '007', 7, 'p'), (2, '7', 8, 'q'), (3, 'abc', 0, 'r'), (4, NULL, 9, 's'), (5, '', 1, 't')");
    auto pairs = [&](const std::string& sql) {
        std::multiset<std::string> out;
        for (auto& r : q(ex, sql)) out.insert(r[0] + ":" + r[1]);
        return out;
    };
    using Set = std::multiset<std::string>;
    // text = text: the texts are equal or they are not
    REQUIRE(pairs("SELECT a.id, b.id FROM a JOIN b ON a.code = b.code") == Set{"1:2", "2:1", "4:3", "6:5"});
    REQUIRE(pairs("SELECT a.id, b.id FROM a LEFT JOIN b ON a.code = b.code AND b.tag <> 'zzz'") == Set{"1:2", "2:1", "3:NULL", "4:3", "5:NULL", "6:5"});
    REQUIRE(pairs("SELECT a.id, b.id FROM a RIGHT JOIN b ON a.code = b.code") == Set{"1:2", "2:1", "4:3", "NULL:4", "6:5"});
    // number = number: by value
    REQUIRE(pairs("SELECT a.id, b.id FROM a JOIN b ON a.n = b.n") == Set{"1:1", "2:1", "3:2", "4:3", "5:4", "6:3"});
    // text = number: by the number each text starts with ('abc' and '' are 0)
    REQUIRE(pairs("SELECT a.id, b.id FROM a JOIN b ON a.code = b.n") == Set{"1:1", "2:1", "3:1", "4:3", "6:3"});
    REQUIRE(pairs("SELECT a.id, b.id FROM b JOIN a ON b.n = a.code") == Set{"1:1", "2:1", "3:1", "4:3", "6:3"});
    // the same with an index on the joined column (an index nested loop, a hash join)
    ok(ex, "CREATE INDEX b_code ON b (code)");
    ok(ex, "CREATE INDEX b_n ON b (n)");
    REQUIRE(pairs("SELECT a.id, b.id FROM a JOIN b ON a.code = b.code") == Set{"1:2", "2:1", "4:3", "6:5"});
    REQUIRE(pairs("SELECT a.id, b.id FROM a JOIN b ON a.n = b.n") == Set{"1:1", "2:1", "3:2", "4:3", "5:4", "6:3"});
    REQUIRE(pairs("SELECT a.id, b.id FROM a JOIN b ON a.code = b.n") == Set{"1:1", "2:1", "3:1", "4:3", "6:3"});
    // USING and NATURAL read the columns' own types
    REQUIRE(pairs("SELECT a.id, b.id FROM a JOIN b USING (code)") == Set{"1:2", "2:1", "4:3", "6:5"});
    // integers beyond 2^53 meet only themselves (hashed as doubles, 2^53 + 1 met 2^53), through a hash join and through a primary key
    ok(ex, "CREATE TABLE bx (id INT PRIMARY KEY, b BIGINT)");
    ok(ex, "CREATE TABLE bw (id INT PRIMARY KEY, b BIGINT)");
    ok(ex, "CREATE TABLE bk (b BIGINT PRIMARY KEY, id INT)");
    ok(ex, "INSERT INTO bx VALUES (1, 9007199254740991), (2, 9007199254740992), (3, 9007199254740993), (4, 9007199254740994), (5, 5)");
    ok(ex, "INSERT INTO bw VALUES (1, 9007199254740993), (2, 9007199254740992), (3, 9007199254740995), (4, 5)");
    ok(ex, "INSERT INTO bk VALUES (9007199254740993, 1), (9007199254740992, 2), (9007199254740995, 3), (5, 4)");
    REQUIRE(pairs("SELECT bx.id, bw.id FROM bx JOIN bw ON bx.b = bw.b") == Set{"2:2", "3:1", "5:4"});
    REQUIRE(pairs("SELECT bx.id, bk.id FROM bx JOIN bk ON bx.b = bk.b") == Set{"2:2", "3:1", "5:4"});
    REQUIRE(pairs("SELECT bx.id, bw.id FROM bx LEFT JOIN bw ON bx.b = bw.b") == Set{"1:NULL", "2:2", "3:1", "4:NULL", "5:4"});
    // a text primary key probed by an index nested loop: the texts are equal or they are not
    ok(ex, "CREATE TABLE kt (code VARCHAR(10) PRIMARY KEY, tag VARCHAR(10))");
    ok(ex, "INSERT INTO kt VALUES ('007', 'p'), ('7', 'q'), ('abc', 'r'), ('', 's')");
    REQUIRE(pairs("SELECT a.id, kt.tag FROM a JOIN kt ON a.code = kt.code") == Set{"1:q", "2:p", "4:r", "6:s"});
    REQUIRE(pairs("SELECT a.id, kt.tag FROM kt JOIN a ON kt.code = a.code") == Set{"1:q", "2:p", "4:r", "6:s"});
    // a self join on a text column
    REQUIRE(pairs("SELECT x.id, y.id FROM a x JOIN a y ON x.code = y.code AND x.id < y.id").empty());
    REQUIRE(pairs("SELECT x.id, y.id FROM a x JOIN a y ON x.n = y.n AND x.id < y.id") == Set{"1:2", "4:6"});
}

TEST_CASE("views, derived tables and CTEs keep the types of their columns", "[typed_comparison][derived]") {
    TempDataDir dir("tc_derived");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, code VARCHAR(10), n INT)");
    ok(ex, "INSERT INTO t VALUES (1, '007', 7), (2, '7', 7), (3, '10', 10), (4, '9', 9), (5, 'abc', 0)");
    ok(ex, "CREATE VIEW v AS SELECT id, code, n FROM t");
    ok(ex, "CREATE VIEW v2 AS SELECT id, code AS c, n + 1 AS m FROM t");
    // text: exact; number: by value
    for (const char* source : {"t", "v", "(SELECT id, code, n FROM t) d", "(SELECT * FROM t) d"}) {
        INFO(source);
        REQUIRE(ids(ex, std::string("SELECT id FROM ") + source + " WHERE code = '7' ORDER BY id") == std::vector<int>{2});
        REQUIRE(ids(ex, std::string("SELECT id FROM ") + source + " WHERE code = 7 ORDER BY id") == std::vector<int>{1, 2});
        REQUIRE(ids(ex, std::string("SELECT id FROM ") + source + " WHERE code > '5' ORDER BY id") == std::vector<int>{2, 4, 5});
        REQUIRE(first_column(ex, std::string("SELECT code FROM ") + source + " ORDER BY code") == std::vector<std::string>{"007", "10", "7", "9", "abc"});
        REQUIRE(first_column(ex, std::string("SELECT MAX(code) FROM ") + source) == std::vector<std::string>{"abc"});
    }
    REQUIRE(ids(ex, "SELECT id FROM v2 WHERE c = '007' ORDER BY id") == std::vector<int>{1});
    REQUIRE(ids(ex, "SELECT id FROM v2 WHERE c = 7 ORDER BY id") == std::vector<int>{1, 2});
    REQUIRE(ids(ex, "SELECT id FROM v2 WHERE m = '8' ORDER BY id") == std::vector<int>{1, 2});
    REQUIRE(first_column(ex, "WITH w AS (SELECT code FROM t) SELECT code FROM w ORDER BY code") == std::vector<std::string>{"007", "10", "7", "9", "abc"});
    REQUIRE(ids(ex, "WITH w AS (SELECT id, code FROM t) SELECT id FROM w WHERE code = '7' ORDER BY id") == std::vector<int>{2});
    // a subquery: the column of the inner query is a text
    REQUIRE(ids(ex, "SELECT id FROM t WHERE code = (SELECT code FROM t WHERE id = 2) ORDER BY id") == std::vector<int>{2});
    REQUIRE(ids(ex, "SELECT id FROM t WHERE code = (SELECT MAX(code) FROM t WHERE id < 5) ORDER BY id") == std::vector<int>{4});
    // (the MAX of the texts '007' and '7' is '7' -- the text -- and only the row that holds '7' equals it)
    REQUIRE(ids(ex, "SELECT id FROM t WHERE code = (SELECT MAX(code) FROM t WHERE id < 3) ORDER BY id") == std::vector<int>{2});
    REQUIRE(ids(ex, "SELECT id FROM t WHERE code = (SELECT MIN(code) FROM t WHERE id < 3) ORDER BY id") == std::vector<int>{1});
}

TEST_CASE("views keep the quotes of their strings after a restart", "[typed_comparison][persist]") {
    TempDataDir dir("tc_persist");
    {
        Executor ex(dir.path);
        open_db(ex);
        ok(ex, "CREATE TABLE pq (id INT PRIMARY KEY, code VARCHAR(10))");
        ok(ex, "INSERT INTO pq VALUES (1, '007'), (2, '7'), (3, '10'), (4, '9'), (5, 'abc')");
        ok(ex, "CREATE VIEW v_eq AS SELECT id FROM pq WHERE code = '7'");
        ok(ex, "CREATE VIEW v_in AS SELECT id FROM pq WHERE code IN ('7', '10')");
        ok(ex, "CREATE VIEW v_between AS SELECT id FROM pq WHERE code BETWEEN '9' AND 'z'");
        ok(ex, "CREATE VIEW v_not_between AS SELECT id FROM pq WHERE code NOT BETWEEN '9' AND 'z'");
        ok(ex, "CHECKPOINT");
    }
    for (int round = 0; round < 2; round++) { // the first round reads what was written, the second what was written again
        Executor ex(dir.path);
        ok(ex, "USE d");
        INFO("round " << round);
        REQUIRE(ids(ex, "SELECT id FROM v_eq ORDER BY id") == std::vector<int>{2});
        REQUIRE(ids(ex, "SELECT id FROM v_in ORDER BY id") == std::vector<int>{2, 3});
        REQUIRE(ids(ex, "SELECT id FROM v_between ORDER BY id") == std::vector<int>{4, 5});
        REQUIRE(ids(ex, "SELECT id FROM v_not_between ORDER BY id") == std::vector<int>{1, 2, 3});
        ok(ex, "CHECKPOINT");
    }
}

TEST_CASE("UPDATE and DELETE by primary key read the key the way the comparison does", "[typed_comparison][dml_pk]") {
    TempDataDir dir("tc_dml_pk");
    Executor ex(dir.path);
    open_db(ex);
    // a bare word is a column, not the key of that name: `a = b` deletes the rows whose two columns are equal
    ok(ex, "CREATE TABLE rp (a VARCHAR(10) PRIMARY KEY, b VARCHAR(10))");
    ok(ex, "INSERT INTO rp VALUES ('x', 'x'), ('y', 'z'), ('b', 'q')");
    ok(ex, "UPDATE rp SET b = 'u' WHERE a = b");
    REQUIRE(q(ex, "SELECT a, b FROM rp ORDER BY a") == Rows{{"b", "q"}, {"x", "u"}, {"y", "z"}});
    ok(ex, "INSERT INTO rp VALUES ('w', 'w')");
    ok(ex, "DELETE FROM rp WHERE a = b");
    REQUIRE(q(ex, "SELECT a, b FROM rp ORDER BY a") == Rows{{"b", "q"}, {"x", "u"}, {"y", "z"}});
    // a number against a text key reads each text by its number: '7', '007' and '7.0' are all 7; a string is the text
    ok(ex, "CREATE TABLE tp (code VARCHAR(10) PRIMARY KEY, n INT)");
    ok(ex, "INSERT INTO tp VALUES ('7', 1), ('007', 2), ('7.0', 3), ('8', 4), ('abc', 5)");
    auto changed = [&](const std::string& sql) {
        const std::string r = text(ex, sql);
        const auto at = r.find(" row(s)");
        if (at == std::string::npos) return -1;
        std::size_t begin = at;
        while (begin > 0 && std::isdigit(static_cast<unsigned char>(r[begin - 1]))) begin--;
        return std::stoi(r.substr(begin, at - begin));
    };
    REQUIRE(changed("UPDATE tp SET n = 100 WHERE code = 7") == 3);
    REQUIRE(q(ex, "SELECT code, n FROM tp ORDER BY code") == Rows{{"007", "100"}, {"7", "100"}, {"7.0", "100"}, {"8", "4"}, {"abc", "5"}});
    REQUIRE(changed("UPDATE tp SET n = 5 WHERE code = '7'") == 1);
    REQUIRE(changed("UPDATE tp SET n = 6 WHERE code = 'abc'") == 1);
    REQUIRE(changed("UPDATE tp SET n = 7 WHERE code = 0") == 1); // 'abc' is 0
    REQUIRE(q(ex, "SELECT code, n FROM tp ORDER BY code") == Rows{{"007", "100"}, {"7", "5"}, {"7.0", "100"}, {"8", "4"}, {"abc", "7"}});
    REQUIRE(changed("DELETE FROM tp WHERE code = 7.0") == 3);
    REQUIRE(q(ex, "SELECT code, n FROM tp ORDER BY code") == Rows{{"8", "4"}, {"abc", "7"}});
    // BETWEEN on a text key: strings are a range of bytes, numbers a range of values
    for (const char* table : {"tb1", "tb2"}) {
        ok(ex, std::string("CREATE TABLE ") + table + " (code VARCHAR(10) PRIMARY KEY, n INT)");
        ok(ex, std::string("INSERT INTO ") + table + " VALUES ('7', 1), ('007', 2), ('7.0', 3), ('8', 4), ('10', 5), ('9', 6)");
    }
    REQUIRE(changed("DELETE FROM tb1 WHERE code BETWEEN '7' AND '8'") == 3);
    REQUIRE(q(ex, "SELECT code FROM tb1 ORDER BY code") == Rows{{"007"}, {"10"}, {"9"}});
    REQUIRE(changed("DELETE FROM tb2 WHERE code BETWEEN 7 AND 9") == 5);
    REQUIRE(q(ex, "SELECT code FROM tb2 ORDER BY code") == Rows{{"10"}});
    // an INT key: a bound written as a decimal still reaches the key it equals
    ok(ex, "CREATE TABLE ip (id INT PRIMARY KEY, v INT)");
    ok(ex, "INSERT INTO ip VALUES (4, 0), (5, 0), (6, 0), (7, 0), (9, 0), (10, 0)");
    REQUIRE(changed("UPDATE ip SET v = 1 WHERE id = 4.0") == 1);
    REQUIRE(changed("DELETE FROM ip WHERE id BETWEEN 5.0 AND 9") == 4);
    REQUIRE(q(ex, "SELECT id, v FROM ip ORDER BY id") == Rows{{"4", "1"}, {"10", "0"}});
    REQUIRE(changed("DELETE FROM ip WHERE id BETWEEN 4 AND 4") == 1);
    REQUIRE(q(ex, "SELECT id FROM ip ORDER BY id") == Rows{{"10"}});
    // ... a bound written with leading zeros too (the key "9" sits after "009" in the tree), and a key of decimals ("9.00" sits after "9")
    ok(ex, "CREATE TABLE ip3 (id INT PRIMARY KEY)");
    ok(ex, "INSERT INTO ip3 VALUES (4), (5), (9), (10)");
    REQUIRE(changed("DELETE FROM ip3 WHERE id BETWEEN 5 AND 009") == 2);
    REQUIRE(q(ex, "SELECT id FROM ip3 ORDER BY id") == Rows{{"4"}, {"10"}});
    ok(ex, "CREATE TABLE dp (id DECIMAL(10,2) PRIMARY KEY)");
    ok(ex, "INSERT INTO dp VALUES (4), (5), (6), (9), (10)");
    REQUIRE(changed("DELETE FROM dp WHERE id BETWEEN 5 AND 9") == 3);
    REQUIRE(q(ex, "SELECT id FROM dp ORDER BY id") == Rows{{"4.00"}, {"10.00"}});
    // a table big enough for the statements to pick their rows through the index: a range above the key '10' (as bytes) reaches '11', '12' and '5'
    // (a bound taken as the number 10 would start the scan at "9.99..", after them)
    ok(ex, "CREATE TABLE sp (code VARCHAR(12) PRIMARY KEY, n INT)");
    std::string values;
    for (int i = 0; i < 100; i++) values += std::string(i ? ", " : "") + "('0" + std::to_string(100 + i).substr(1) + "', 0)";
    ok(ex, "INSERT INTO sp VALUES " + values + ", ('11', 0), ('12', 0), ('5', 0)");
    REQUIRE(changed("UPDATE sp SET n = 1 WHERE code > '10'") == 3);
    REQUIRE(q(ex, "SELECT code FROM sp WHERE n = 1 ORDER BY code") == Rows{{"11"}, {"12"}, {"5"}});
    REQUIRE(changed("DELETE FROM sp WHERE code >= '11'") == 3);
    REQUIRE(q(ex, "SELECT COUNT(*) FROM sp") == Rows{{"100"}});
}

TEST_CASE("every numeric column type compares by value, every text type by its text", "[typed_comparison][types]") {
    TempDataDir dir("tc_types");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE ty (id INT PRIMARY KEY, i INT, bi BIGINT, si SMALLINT, ti TINYINT, fl FLOAT, db DOUBLE, de DECIMAL(10,2), fg BOOLEAN, yr YEAR, "
           "vc VARCHAR(10), ch CHAR(10), tx TEXT)");
    ok(ex, "INSERT INTO ty VALUES (1, 1, 1, 1, 1, 1, 1, 1, 1, 2024, '1', '1', '1'), (2, 2, 2, 2, 2, 2, 2, 2, 0, 2025, '2', '2', '2')");
    // a number column against a string that spells the same number differently: equal by value
    for (const char* column : {"i", "bi", "si", "ti", "fl", "db", "de", "fg"}) {
        INFO(column);
        REQUIRE(ids(ex, std::string("SELECT id FROM ty WHERE ") + column + " = '1.0' ORDER BY id") == std::vector<int>{1});
        REQUIRE(ids(ex, std::string("SELECT id FROM ty WHERE ") + column + " = '01' ORDER BY id") == std::vector<int>{1});
    }
    REQUIRE(ids(ex, "SELECT id FROM ty WHERE yr = '2024.0' ORDER BY id") == std::vector<int>{1});
    // a text column against the same string: the texts are not equal
    for (const char* column : {"vc", "ch", "tx"}) {
        INFO(column);
        REQUIRE(ids(ex, std::string("SELECT id FROM ty WHERE ") + column + " = '1.0' ORDER BY id").empty());
        REQUIRE(ids(ex, std::string("SELECT id FROM ty WHERE ") + column + " = '01' ORDER BY id").empty());
        REQUIRE(ids(ex, std::string("SELECT id FROM ty WHERE ") + column + " = '1' ORDER BY id") == std::vector<int>{1});
        REQUIRE(ids(ex, std::string("SELECT id FROM ty WHERE ") + column + " = 1.0 ORDER BY id") == std::vector<int>{1});
    }
}

TEST_CASE("functions and expressions give text or numbers", "[typed_comparison][expressions]") {
    TempDataDir dir("tc_expr");
    Executor ex(dir.path);
    open_db(ex);
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, s VARCHAR(10), n INT)");
    ok(ex, "INSERT INTO t VALUES (1, '7', 7), (2, '007', 7), (3, '10', 10), (4, 'abc', 3), (5, '9', 9)");
    // a text function gives text: two strings compare as strings
    REQUIRE(ids(ex, "SELECT id FROM t WHERE UPPER(s) = 'ABC' ORDER BY id") == std::vector<int>{4});
    REQUIRE(ids(ex, "SELECT id FROM t WHERE CONCAT(s, '') = '7' ORDER BY id") == std::vector<int>{1});
    REQUIRE(ids(ex, "SELECT id FROM t WHERE CONCAT(s, '') < '8' ORDER BY id") == std::vector<int>{1, 2, 3});
    // every text function gives text: the string '7' is not the string '007'
    for (const char* f : {"UPPER(s)", "LOWER(s)", "TRIM(s)", "LTRIM(s)", "RTRIM(s)", "CONCAT(s, '')", "CONCAT_WS('', s)", "SUBSTRING(s, 1)", "REPLACE(s, 'x', 'y')",
                          "LEFT(s, 9)", "RIGHT(s, 9)", "REVERSE(REVERSE(s))", "REPEAT(s, 1)"}) {
        INFO(f);
        REQUIRE(ids(ex, std::string("SELECT id FROM t WHERE ") + f + " = '7' ORDER BY id") == std::vector<int>{1});
        REQUIRE(ids(ex, std::string("SELECT id FROM t WHERE ") + f + " = 7 ORDER BY id") == std::vector<int>{1, 2});
    }
    // an expression of no known type (COALESCE) compares the way its values look: two numbers as numbers
    ok(ex, "CREATE TABLE u2 (id INT PRIMARY KEY, n INT, p DECIMAL(10,2))");
    ok(ex, "INSERT INTO u2 VALUES (1, 7, 7.00), (2, 7, 7.50), (3, NULL, NULL)");
    REQUIRE(ids(ex, "SELECT id FROM u2 WHERE COALESCE(n, 0) = COALESCE(p, 0) ORDER BY id") == std::vector<int>{1, 3});
    // a numeric function or arithmetic gives a number
    REQUIRE(ids(ex, "SELECT id FROM t WHERE LENGTH(s) = 3 ORDER BY id") == std::vector<int>{2, 4});
    REQUIRE(ids(ex, "SELECT id FROM t WHERE s + 0 = 7 ORDER BY id") == std::vector<int>{1, 2});
    REQUIRE(ids(ex, "SELECT id FROM t WHERE n * 1 = '7' ORDER BY id") == std::vector<int>{1, 2});
    REQUIRE(ids(ex, "SELECT id FROM t WHERE ABS(n) > '9' ORDER BY id") == std::vector<int>{3});
    // CASE and a comparison in the select list
    REQUIRE(q(ex, "SELECT id, CASE WHEN s = '7' THEN 'a' WHEN s = 7 THEN 'b' ELSE 'c' END FROM t ORDER BY id") ==
            Rows{{"1", "a"}, {"2", "b"}, {"3", "c"}, {"4", "c"}, {"5", "c"}});
    REQUIRE(q(ex, "SELECT id, s = '7', s = 7, s < '8', s < 8 FROM t WHERE id <= 2 ORDER BY id") == Rows{{"1", "1", "1", "1", "1"}, {"2", "0", "1", "1", "1"}});
}
