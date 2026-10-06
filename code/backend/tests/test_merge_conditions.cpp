#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// MERGE ... WHEN MATCHED AND <condition> THEN UPDATE: the condition used to be read and thrown away for an UPDATE (it was kept only for DELETE), so every matched
// row was updated. The conditions of MERGE were not bound either, so a subquery in one could not name the row, and one was never evaluated (it read as false).

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

std::string text(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
    return r.value();
}

void ok(Executor& ex, const std::string& sql) { text(ex, sql); }

using Rows = std::vector<std::vector<std::string>>;
const std::string N = "NULL";

Rows cells(const std::string& output) {
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

Rows q(Executor& ex, const std::string& sql) { return cells(text(ex, sql)); }

std::string merge_message(Executor& ex, const std::string& sql) {
    auto r = text(ex, sql);
    auto cut = r.find(" (");
    return cut == std::string::npos ? r : r.substr(0, cut);
}

// tg: (1, 10, 0) (2, 20, 0) (3, NULL, 0) (4, 40, 0)     sr: (1, 5) (2, 25) (3, 7) (5, 9)     b: (id, a_id): 1 -> 1, 2 -> 1, 3 -> 3
void tables(Executor& ex) {
    ok(ex, "CREATE TABLE tg (id INT PRIMARY KEY, n INT, m INT)");
    ok(ex, "CREATE TABLE sr (id INT PRIMARY KEY, n INT)");
    ok(ex, "CREATE TABLE b (id INT PRIMARY KEY, a_id INT)");
    ok(ex, "INSERT INTO tg VALUES (1, 10, 0), (2, 20, 0), (3, NULL, 0), (4, 40, 0)");
    ok(ex, "INSERT INTO sr VALUES (1, 5), (2, 25), (3, 7), (5, 9)");
    ok(ex, "INSERT INTO b VALUES (1, 1), (2, 1), (3, 3)");
}
Rows tg_rows(Executor& ex) { return q(ex, "SELECT id, n, m FROM tg ORDER BY id"); }
} // namespace

TEST_CASE("the condition of WHEN MATCHED ... THEN UPDATE applies", "[merge_conditions]") {
    TempDataDir dir("mc_update");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    // a condition on the target, one on the source, one on both; ids 1..3 match, and 3 has a NULL n (unknown: the update does not apply)
    REQUIRE(merge_message(ex, "MERGE INTO tg t USING sr s ON t.id = s.id WHEN MATCHED AND t.id = 1 THEN UPDATE SET m = 1") == "MERGE: 1 updated, 0 deleted, 0 inserted.");
    REQUIRE(tg_rows(ex) == Rows{{"1", "10", "1"}, {"2", "20", "0"}, {"3", N, "0"}, {"4", "40", "0"}});
    REQUIRE(merge_message(ex, "MERGE INTO tg t USING sr s ON t.id = s.id WHEN MATCHED AND s.n > 6 THEN UPDATE SET m = 2") == "MERGE: 2 updated, 0 deleted, 0 inserted.");
    REQUIRE(tg_rows(ex) == Rows{{"1", "10", "1"}, {"2", "20", "2"}, {"3", N, "2"}, {"4", "40", "0"}});
    REQUIRE(merge_message(ex, "MERGE INTO tg t USING sr s ON t.id = s.id WHEN MATCHED AND t.n > s.n THEN UPDATE SET m = 3") == "MERGE: 1 updated, 0 deleted, 0 inserted.");
    REQUIRE(tg_rows(ex) == Rows{{"1", "10", "3"}, {"2", "20", "2"}, {"3", N, "2"}, {"4", "40", "0"}}); // (2: 20 > 25 is false, 3: NULL > 7 is unknown)
    // a condition nothing satisfies changes nothing
    REQUIRE(merge_message(ex, "MERGE INTO tg t USING sr s ON t.id = s.id WHEN MATCHED AND t.id > 100 THEN UPDATE SET m = 9") == "MERGE: 0 updated, 0 deleted, 0 inserted.");
    // the names of the tables when there are no aliases
    REQUIRE(merge_message(ex, "MERGE INTO tg USING sr ON tg.id = sr.id WHEN MATCHED AND sr.n < 8 AND tg.id <> 1 THEN UPDATE SET m = 4") == "MERGE: 1 updated, 0 deleted, 0 inserted.");
    REQUIRE(tg_rows(ex) == Rows{{"1", "10", "3"}, {"2", "20", "2"}, {"3", N, "4"}, {"4", "40", "0"}});
    // not matched rows are still inserted; the condition belongs to the matched rows
    REQUIRE(merge_message(ex, "MERGE INTO tg t USING sr s ON t.id = s.id WHEN MATCHED AND s.n > 100 THEN UPDATE SET m = 5 WHEN NOT MATCHED THEN INSERT (id, n, m) VALUES (s.id, s.n, 0)") ==
            "MERGE: 0 updated, 0 deleted, 1 inserted.");
    REQUIRE(q(ex, "SELECT id, n, m FROM tg WHERE id = 5") == Rows{{"5", "9", "0"}});
}

TEST_CASE("the first WHEN MATCHED clause whose condition holds is the one used", "[merge_conditions]") {
    TempDataDir dir("mc_order");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    // id 2 (s.n = 25) satisfies both conditions: the one written first wins
    REQUIRE(merge_message(ex, "MERGE INTO tg t USING sr s ON t.id = s.id WHEN MATCHED AND s.n > 20 THEN DELETE WHEN MATCHED THEN UPDATE SET m = 1") ==
            "MERGE: 2 updated, 1 deleted, 0 inserted.");
    REQUIRE(tg_rows(ex) == Rows{{"1", "10", "1"}, {"3", N, "1"}, {"4", "40", "0"}});
    ok(ex, "DELETE FROM tg");
    ok(ex, "INSERT INTO tg VALUES (1, 10, 0), (2, 20, 0), (3, NULL, 0), (4, 40, 0)");
    REQUIRE(merge_message(ex, "MERGE INTO tg t USING sr s ON t.id = s.id WHEN MATCHED AND s.n > 20 THEN UPDATE SET m = 1 WHEN MATCHED THEN DELETE") ==
            "MERGE: 1 updated, 2 deleted, 0 inserted.");
    REQUIRE(tg_rows(ex) == Rows{{"2", "20", "1"}, {"4", "40", "0"}});
    ok(ex, "DELETE FROM tg");
    ok(ex, "INSERT INTO tg VALUES (1, 10, 0), (2, 20, 0), (3, NULL, 0), (4, 40, 0)");
    // two conditions that split the rows
    REQUIRE(merge_message(ex, "MERGE INTO tg t USING sr s ON t.id = s.id WHEN MATCHED AND t.id = 1 THEN DELETE WHEN MATCHED AND t.id = 2 THEN UPDATE SET m = 7") ==
            "MERGE: 1 updated, 1 deleted, 0 inserted.");
    REQUIRE(tg_rows(ex) == Rows{{"2", "20", "7"}, {"3", N, "0"}, {"4", "40", "0"}});
}

TEST_CASE("a subquery in a MERGE condition names the row", "[merge_conditions]") {
    TempDataDir dir("mc_subquery");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    // b has rows for ids 1 and 3 only (an outer column on either side, through the alias or the table name)
    REQUIRE(merge_message(ex, "MERGE INTO tg t USING sr s ON t.id = s.id WHEN MATCHED AND EXISTS (SELECT 1 FROM b WHERE b.a_id = t.id) THEN UPDATE SET m = 1") ==
            "MERGE: 2 updated, 0 deleted, 0 inserted.");
    REQUIRE(tg_rows(ex) == Rows{{"1", "10", "1"}, {"2", "20", "0"}, {"3", N, "1"}, {"4", "40", "0"}});
    REQUIRE(merge_message(ex, "MERGE INTO tg t USING sr s ON t.id = s.id WHEN MATCHED AND NOT EXISTS (SELECT 1 FROM b WHERE s.id = b.a_id) THEN UPDATE SET m = 2") ==
            "MERGE: 1 updated, 0 deleted, 0 inserted.");
    REQUIRE(tg_rows(ex) == Rows{{"1", "10", "1"}, {"2", "20", "2"}, {"3", N, "1"}, {"4", "40", "0"}});
    // the counts of b by a_id are 2, 0, 1 for ids 1, 2, 3: the source's n less 4 is 1, 21, 3
    REQUIRE(merge_message(ex, "MERGE INTO tg USING sr ON tg.id = sr.id WHEN MATCHED AND sr.n - 4 > (SELECT COUNT(*) FROM b WHERE b.a_id = tg.id) THEN UPDATE SET m = 3") ==
            "MERGE: 2 updated, 0 deleted, 0 inserted.");
    REQUIRE(tg_rows(ex) == Rows{{"1", "10", "1"}, {"2", "20", "3"}, {"3", N, "3"}, {"4", "40", "0"}});
    // a subquery that names no row: the same for all (the smallest id of b is 1)
    REQUIRE(merge_message(ex, "MERGE INTO tg t USING sr s ON t.id = s.id WHEN MATCHED AND s.n - 5 > (SELECT MIN(id) FROM b) THEN DELETE") ==
            "MERGE: 0 updated, 2 deleted, 0 inserted.");
    REQUIRE(tg_rows(ex) == Rows{{"1", "10", "1"}, {"4", "40", "0"}});
    // one in the ON
    REQUIRE(merge_message(ex, "MERGE INTO tg t USING sr s ON t.id = s.id AND EXISTS (SELECT 1 FROM b WHERE b.a_id = s.id) WHEN MATCHED THEN UPDATE SET m = 8") ==
            "MERGE: 1 updated, 0 deleted, 0 inserted.");
    REQUIRE(tg_rows(ex) == Rows{{"1", "10", "8"}, {"4", "40", "0"}});
}

TEST_CASE("a subquery in a MERGE condition names a column of the source, unaliased", "[merge_conditions]") {
    TempDataDir dir("mc_source");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    // the source's n less 4 is 1, 21, 3: b has an id 1 and an id 3 (the target's own n, 10, 20, NULL, would find none of them)
    REQUIRE(merge_message(ex, "MERGE INTO tg USING sr ON tg.id = sr.id WHEN MATCHED AND EXISTS (SELECT 1 FROM b WHERE b.id = sr.n - 4) THEN UPDATE SET m = 9") ==
            "MERGE: 2 updated, 0 deleted, 0 inserted.");
    REQUIRE(tg_rows(ex) == Rows{{"1", "10", "9"}, {"2", "20", "0"}, {"3", N, "9"}, {"4", "40", "0"}});
    // and a column of the target that the source has too
    REQUIRE(merge_message(ex, "MERGE INTO tg USING sr ON tg.id = sr.id WHEN MATCHED AND EXISTS (SELECT 1 FROM b WHERE b.id = tg.n - 18) THEN UPDATE SET m = 5") ==
            "MERGE: 1 updated, 0 deleted, 0 inserted.");
    REQUIRE(tg_rows(ex) == Rows{{"1", "10", "9"}, {"2", "20", "5"}, {"3", N, "9"}, {"4", "40", "0"}});
}

TEST_CASE("a MERGE with conditions in a stored procedure and after a restart", "[merge_conditions]") {
    TempDataDir dir("mc_persist");
    {
        Executor ex(dir.path);
        open_db(ex);
        tables(ex);
        ok(ex, "CREATE PROCEDURE do_merge() BEGIN MERGE INTO tg t USING sr s ON t.id = s.id WHEN MATCHED AND s.n > 6 THEN UPDATE SET m = m + 1 WHEN MATCHED THEN DELETE; END");
        ok(ex, "CALL do_merge()");
        REQUIRE(tg_rows(ex) == Rows{{"2", "20", "1"}, {"3", N, "1"}, {"4", "40", "0"}}); // (id 1 fails the condition of the first clause: the second deletes it)
    }
    Executor ex(dir.path);
    ok(ex, "USE d");
    ok(ex, "CALL do_merge()");
    REQUIRE(tg_rows(ex) == Rows{{"2", "20", "2"}, {"3", N, "2"}, {"4", "40", "0"}});
}

TEST_CASE("an unknown column in a MERGE condition is an error", "[merge_conditions]") {
    TempDataDir dir("mc_unknown");
    Executor ex(dir.path);
    open_db(ex);
    tables(ex);
    auto r = ex.execute_sql("MERGE INTO tg t USING sr s ON t.id = s.id WHEN MATCHED AND t.nosuch > 1 THEN UPDATE SET m = 1");
    REQUIRE(r.is_err());
    REQUIRE(r.error().find("Unknown column") != std::string::npos);
    REQUIRE(tg_rows(ex) == Rows{{"1", "10", "0"}, {"2", "20", "0"}, {"3", N, "0"}, {"4", "40", "0"}});
}

// ---- random MERGEs against a reference ----------------------------------------------------------------------------------------------------

namespace {
using Cell = std::optional<long long>;
using Truth = std::optional<bool>;
struct Side { Cell n, m, sn; }; // the target's n and m, the source's n

struct Operand {
    std::string sql;
    std::function<Cell(const Side&)> value;
};

Operand operand(std::mt19937& rng, const std::string& t, const std::string& s) {
    switch (rng() % 4) {
        case 0: return {t + ".n", [](const Side& r) { return r.n; }};
        case 1: return {t + ".m", [](const Side& r) { return r.m; }};
        case 2: return {s + ".n", [](const Side& r) { return r.sn; }};
        default: {
            const long long k = static_cast<long long>(rng() % 30);
            return {std::to_string(k), [k](const Side&) { return Cell(k); }};
        }
    }
}

struct MergeCondition {
    std::string sql;
    std::function<Truth(const Side&)> eval;
};

MergeCondition random_condition(std::mt19937& rng, const std::string& t, const std::string& s) {
    static const char* ops[] = {"=", "<>", "<", "<=", ">", ">="};
    auto comparison = [&]() {
        Operand a = operand(rng, t, s), b = operand(rng, t, s);
        const std::string op = ops[rng() % 6];
        return MergeCondition{a.sql + " " + op + " " + b.sql, [a, b, op](const Side& r) -> Truth {
                             const Cell x = a.value(r), y = b.value(r);
                             if (!x || !y) return std::nullopt;
                             if (op == "=") return *x == *y;
                             if (op == "<>") return *x != *y;
                             if (op == "<") return *x < *y;
                             if (op == "<=") return *x <= *y;
                             if (op == ">") return *x > *y;
                             return *x >= *y;
                         }};
    };
    MergeCondition first = comparison();
    if (rng() % 3 != 0) return first;
    MergeCondition second = comparison();
    const bool conjunction = rng() % 2 == 0;
    return {"(" + first.sql + (conjunction ? " AND " : " OR ") + second.sql + ")", [first, second, conjunction](const Side& r) -> Truth {
                const Truth a = first.eval(r), b = second.eval(r);
                if (conjunction) return (a == false || b == false) ? Truth(false) : ((!a || !b) ? Truth() : Truth(true));
                return (a == true || b == true) ? Truth(true) : ((!a || !b) ? Truth() : Truth(false));
            }};
}
} // namespace

TEST_CASE("random MERGE conditions match a reference", "[merge_conditions][random]") {
    unsigned seed_count = 8; // RUSQL_FUZZ_SEEDS=60 runs a longer campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 1;
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    for (unsigned seed = seed_start; seed < seed_start + seed_count; seed++) {
        INFO("seed " << seed);
        std::mt19937 rng(seed);
        TempDataDir dir("mc_random");
        Executor ex(dir.path);
        open_db(ex);
        for (int round = 0; round < 15; round++) {
            ok(ex, "DROP TABLE IF EXISTS tg");
            ok(ex, "DROP TABLE IF EXISTS sr");
            ok(ex, "CREATE TABLE tg (id INT PRIMARY KEY, n INT, m INT)");
            ok(ex, "CREATE TABLE sr (id INT PRIMARY KEY, n INT)");
            struct Target { long long id; Cell n, m; };
            std::vector<Target> target;
            std::map<long long, Cell> source;
            std::string tvalues, svalues;
            auto cell_sql = [](const Cell& c) { return c ? std::to_string(*c) : std::string("NULL"); };
            for (long long id = 0; id < 14; id++) {
                if (rng() % 3 != 0) {
                    Target t{id, rng() % 6 == 0 ? Cell() : Cell(static_cast<long long>(rng() % 30)), rng() % 6 == 0 ? Cell() : Cell(static_cast<long long>(rng() % 30))};
                    target.push_back(t);
                    tvalues += (tvalues.empty() ? "" : ", ") + std::string("(") + std::to_string(id) + ", " + cell_sql(t.n) + ", " + cell_sql(t.m) + ")";
                }
                if (rng() % 3 != 0) {
                    Cell n = rng() % 6 == 0 ? Cell() : Cell(static_cast<long long>(rng() % 30));
                    source[id] = n;
                    svalues += (svalues.empty() ? "" : ", ") + std::string("(") + std::to_string(id) + ", " + cell_sql(n) + ")";
                }
            }
            if (tvalues.empty() || svalues.empty()) continue;
            ok(ex, "INSERT INTO tg VALUES " + tvalues);
            ok(ex, "INSERT INTO sr VALUES " + svalues);
            const bool aliases = rng() % 2 == 0;
            const std::string t = aliases ? "t" : "tg", s = aliases ? "s" : "sr";
            struct Clause { bool is_update; std::optional<MergeCondition> condition; };
            std::vector<Clause> clauses;
            const std::size_t kinds = rng() % 3; // 0: update only, 1: delete only, 2: both
            auto add = [&](bool is_update) {
                Clause c{is_update, std::nullopt};
                if (rng() % 3 != 0) c.condition = random_condition(rng, t, s);
                clauses.push_back(std::move(c));
            };
            if (kinds == 0) add(true);
            else if (kinds == 1) add(false);
            else if (rng() % 2) { add(true); add(false); }
            else { add(false); add(true); }
            const bool insert_missing = rng() % 2 == 0;
            const long long bump = static_cast<long long>(rng() % 7);
            std::string sql = "MERGE INTO tg" + (aliases ? std::string(" t USING sr s") : std::string(" USING sr")) + " ON " + t + ".id = " + s + ".id";
            for (auto& c : clauses) {
                sql += " WHEN MATCHED" + (c.condition ? " AND " + c.condition->sql : std::string()) + " THEN ";
                sql += c.is_update ? "UPDATE SET n = " + s + ".n + " + std::to_string(bump) + ", m = " + t + ".m + 1" : "DELETE";
            }
            if (insert_missing) sql += " WHEN NOT MATCHED THEN INSERT (id, n, m) VALUES (" + s + ".id, " + s + ".n, 0)";
            INFO(sql);
            // the reference
            std::map<long long, std::pair<Cell, Cell>> expected;
            for (auto& r : target) expected[r.id] = {r.n, r.m};
            std::size_t updated = 0, deleted = 0, inserted = 0;
            for (auto& [id, sn] : source) {
                auto it = std::find_if(target.begin(), target.end(), [&](const Target& r) { return r.id == id; });
                if (it == target.end()) {
                    if (insert_missing) {
                        expected[id] = {sn, Cell(0)};
                        inserted++;
                    }
                    continue;
                }
                const Side side{it->n, it->m, sn};
                for (auto& c : clauses) {
                    if (c.condition && c.condition->eval(side) != true) continue;
                    if (c.is_update) {
                        expected[id] = {sn ? Cell(*sn + bump) : Cell(), it->m ? Cell(*it->m + 1) : Cell()};
                        updated++;
                    } else {
                        expected.erase(id);
                        deleted++;
                    }
                    break;
                }
            }
            REQUIRE(merge_message(ex, sql) == "MERGE: " + std::to_string(updated) + " updated, " + std::to_string(deleted) + " deleted, " + std::to_string(inserted) + " inserted.");
            Rows want;
            for (auto& [id, nm] : expected) want.push_back({std::to_string(id), cell_sql(nm.first), cell_sql(nm.second)});
            REQUIRE(tg_rows(ex) == want);
        }
    }
}
