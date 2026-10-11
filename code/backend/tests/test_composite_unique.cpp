#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// A UNIQUE key of several columns (UNIQUE (a, b), CREATE UNIQUE INDEX, ALTER TABLE ... ADD UNIQUE) rejects a repeated combination of the columns,
// and a combination with a NULL in it is never a repeat. It used to keep the first column only, so (1, 1) and (1, 2) clashed and a real repeat of
// both columns was not noticed when the first column differed in no way the engine looked at. The statements and their outcomes below were taken
// from a real MySQL 8.4; the random test checks every write against a model of the three keys.

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

void open_db(Executor& ex) {
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
}

void ok(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
}

void fails(Executor& ex, const std::string& sql) {
    INFO(sql);
    REQUIRE(ex.execute_sql(sql).is_err());
}

Rows rows_of(Executor& ex, const std::string& sql) {
    INFO(sql);
    auto r = ex.execute_sql(sql);
    INFO("error: " << (r.is_err() ? r.error() : std::string()));
    REQUIRE(r.is_ok());
    return table_cells(r.value());
}

const std::string N = "NULL";
} // namespace

TEST_CASE("a UNIQUE key of several columns follows MySQL", "[composite_unique]") {
    TempDataDir dir("composite_unique_basic");
    {
        Executor ex(dir.path);
        open_db(ex);
        ok(ex, "CREATE TABLE cu (id INT PRIMARY KEY AUTO_INCREMENT, a INT, b INT, v INT, UNIQUE KEY ab (a, b))");
        ok(ex, "INSERT INTO cu (a, b, v) VALUES (1, 1, 10), (1, 2, 20), (2, 1, 30)");
        fails(ex, "INSERT INTO cu (a, b, v) VALUES (1, 1, 99)");
        // a NULL in the combination makes it differ from every other
        ok(ex, "INSERT INTO cu (a, b, v) VALUES (1, NULL, 1), (1, NULL, 2), (NULL, NULL, 3), (NULL, NULL, 4)");

        ok(ex, "UPDATE cu SET b = 2 WHERE a = 2 AND b = 1");
        fails(ex, "UPDATE cu SET b = 1 WHERE a = 1 AND b = 2");
        ok(ex, "UPDATE cu SET a = 3 WHERE a = 2");
        REQUIRE(rows_of(ex, "SELECT a, b, v FROM cu ORDER BY id") ==
                Rows{{"1", "1", "10"}, {"1", "2", "20"}, {"3", "2", "30"}, {"1", N, "1"}, {"1", N, "2"}, {N, N, "3"}, {N, N, "4"}});

        // REPLACE removes the row it repeats and adds the new one
        ok(ex, "REPLACE INTO cu (a, b, v) VALUES (1, 1, 77)");
        REQUIRE(rows_of(ex, "SELECT a, b, v FROM cu ORDER BY v") ==
                Rows{{"1", N, "1"}, {"1", N, "2"}, {N, N, "3"}, {N, N, "4"}, {"1", "2", "20"}, {"3", "2", "30"}, {"1", "1", "77"}});

        ok(ex, "INSERT INTO cu (a, b, v) VALUES (3, 2, 5) ON DUPLICATE KEY UPDATE v = v + 100");
        REQUIRE(rows_of(ex, "SELECT a, b, v FROM cu WHERE a = 3 ORDER BY id") == Rows{{"3", "2", "130"}});

        // one REPLACE statement with two rows that repeat each other: the later one stays
        ok(ex, "REPLACE INTO cu (a, b, v) VALUES (5, 5, 1000), (5, 5, 2000)");
        REQUIRE(rows_of(ex, "SELECT v FROM cu WHERE a = 5") == Rows{{"2000"}});
        ok(ex, "DELETE FROM cu WHERE a = 5");

        // a statement that fails changes nothing; IGNORE skips the repeated row and adds the others
        fails(ex, "INSERT INTO cu (a, b, v) VALUES (1, 1, 0), (9, 9, 9)");
        REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM cu") == Rows{{"7"}});
        ok(ex, "INSERT IGNORE INTO cu (a, b, v) VALUES (1, 1, 0), (9, 9, 9)");
        REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM cu") == Rows{{"8"}});

        // ALTER TABLE ... ADD UNIQUE looks at the rows that are there
        ok(ex, "CREATE TABLE cd (id INT PRIMARY KEY, a INT, b INT)");
        ok(ex, "INSERT INTO cd VALUES (1, 1, 1), (2, 1, 1), (3, 2, 2)");
        fails(ex, "ALTER TABLE cd ADD UNIQUE (a, b)");
        ok(ex, "DELETE FROM cd WHERE id = 2");
        ok(ex, "ALTER TABLE cd ADD CONSTRAINT uq_ab UNIQUE (a, b)");
        fails(ex, "INSERT INTO cd VALUES (4, 1, 1)");
        ok(ex, "INSERT INTO cd VALUES (4, 1, 2)");
        ok(ex, "ALTER TABLE cd DROP INDEX uq_ab");
        ok(ex, "INSERT INTO cd VALUES (5, 1, 1)");

        // ... for one column too (it used to be added whatever the rows held); NULLs repeat nothing
        ok(ex, "CREATE TABLE cs (id INT PRIMARY KEY, c INT)");
        ok(ex, "INSERT INTO cs VALUES (1, 5), (2, 5), (3, NULL), (4, NULL)");
        fails(ex, "ALTER TABLE cs ADD UNIQUE (c)");
        ok(ex, "DELETE FROM cs WHERE id = 2");
        ok(ex, "ALTER TABLE cs ADD UNIQUE (c)");
        fails(ex, "INSERT INTO cs VALUES (5, 5)");
        ok(ex, "INSERT INTO cs VALUES (6, NULL)");
        ok(ex, "CREATE TABLE ct (id INT PRIMARY KEY, c INT)");
        ok(ex, "INSERT INTO ct VALUES (1, 7), (2, 7)");
        fails(ex, "CREATE UNIQUE INDEX uct ON ct (c)");
        fails(ex, "ALTER TABLE ct ADD CONSTRAINT uq_c UNIQUE (c)");

        // a key repeated inside one statement (INSERT of two rows, UPDATE of two rows) fails the statement as a whole
        ok(ex, "CREATE TABLE cw (id INT PRIMARY KEY, a INT, b INT, UNIQUE (a, b))");
        ok(ex, "INSERT INTO cw VALUES (1, 1, 1), (2, 1, 2), (3, 2, 1)");
        fails(ex, "INSERT INTO cw VALUES (4, 7, 7), (5, 7, 7)");
        REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM cw") == Rows{{"3"}});
        fails(ex, "UPDATE cw SET a = 5, b = 5 WHERE id IN (1, 2)");
        REQUIRE(rows_of(ex, "SELECT id, a, b FROM cw ORDER BY id") == Rows{{"1", "1", "1"}, {"2", "1", "2"}, {"3", "2", "1"}});
        // a NULL in the key repeats nothing, for REPLACE too: the rows with (1, NULL) stay
        ok(ex, "INSERT INTO cw VALUES (6, 1, NULL), (7, 1, NULL)");
        ok(ex, "REPLACE INTO cw VALUES (8, 1, NULL)");
        REQUIRE(rows_of(ex, "SELECT id FROM cw WHERE a = 1 AND b IS NULL ORDER BY id") == Rows{{"6"}, {"7"}, {"8"}});
        // rows that hold NULLs already do not stop ALTER TABLE ... ADD UNIQUE
        ok(ex, "CREATE TABLE cx (id INT PRIMARY KEY, a INT, b INT)");
        ok(ex, "INSERT INTO cx VALUES (1, 1, NULL), (2, 1, NULL), (3, NULL, NULL)");
        ok(ex, "ALTER TABLE cx ADD UNIQUE (a, b)");
        ok(ex, "INSERT INTO cx VALUES (4, 1, NULL)");
        ok(ex, "INSERT INTO cx VALUES (5, 2, 2)");
        fails(ex, "INSERT INTO cx VALUES (6, 2, 2)");

        // REPLACE deletes the row it repeats, so DELETE's rules apply to it: a parent row that a child refers to cannot go
        ok(ex, "CREATE TABLE pr (id INT PRIMARY KEY, a INT, b INT, UNIQUE (a, b))");
        ok(ex, "CREATE TABLE chd (id INT PRIMARY KEY, p INT, FOREIGN KEY (p) REFERENCES pr(id))");
        ok(ex, "INSERT INTO pr VALUES (1, 1, 1), (2, 2, 2)");
        ok(ex, "INSERT INTO chd VALUES (1, 1)");
        fails(ex, "REPLACE INTO pr VALUES (3, 1, 1)");
        REQUIRE(rows_of(ex, "SELECT id FROM pr ORDER BY id") == Rows{{"1"}, {"2"}});
        ok(ex, "REPLACE INTO pr VALUES (4, 2, 2)");
        REQUIRE(rows_of(ex, "SELECT id FROM pr ORDER BY id") == Rows{{"1"}, {"4"}});
        // ... and a REPLACE that fails leaves the table as it was, whichever of its rows is the one that cannot go
        ok(ex, "INSERT INTO pr VALUES (5, 3, 3)");
        fails(ex, "REPLACE INTO pr VALUES (6, 3, 3), (7, 1, 1)");
        REQUIRE(rows_of(ex, "SELECT id FROM pr ORDER BY id") == Rows{{"1"}, {"4"}, {"5"}});

        // CREATE UNIQUE INDEX
        ok(ex, "CREATE TABLE ce (id INT PRIMARY KEY, a INT, b INT)");
        ok(ex, "INSERT INTO ce VALUES (1, 1, 1), (2, 1, 1)");
        fails(ex, "CREATE UNIQUE INDEX ue ON ce (a, b)");
        ok(ex, "DELETE FROM ce WHERE id = 2");
        ok(ex, "CREATE UNIQUE INDEX ue ON ce (a, b)");
        fails(ex, "INSERT INTO ce VALUES (3, 1, 1)");
        ok(ex, "INSERT INTO ce VALUES (3, 1, 2)");

        // two keys on one table: either one rejects a row, and an UPDATE is checked against both
        ok(ex, "CREATE TABLE cf (a INT, b INT, c INT, UNIQUE (a, b), UNIQUE (c))");
        ok(ex, "INSERT INTO cf VALUES (1, 1, 1), (1, 2, 2)");
        fails(ex, "INSERT INTO cf VALUES (1, 1, 3)");
        fails(ex, "INSERT INTO cf VALUES (2, 1, 1)");
        ok(ex, "INSERT INTO cf VALUES (2, 2, 3)");
        fails(ex, "UPDATE cf SET a = 1, b = 1 WHERE c = 3");
        ok(ex, "UPDATE cf SET a = a + 1, b = b - 1 WHERE c = 3");
        REQUIRE(rows_of(ex, "SELECT a, b, c FROM cf ORDER BY c") == Rows{{"1", "1", "1"}, {"1", "2", "2"}, {"3", "1", "3"}});

        // a PRIMARY KEY of two columns
        ok(ex, "CREATE TABLE cg (a INT, b INT, PRIMARY KEY (a, b))");
        ok(ex, "INSERT INTO cg VALUES (1, 1), (1, 2)");
        fails(ex, "INSERT INTO cg VALUES (1, 1)");
        fails(ex, "UPDATE cg SET b = 1 WHERE b = 2");
        REQUIRE(rows_of(ex, "SELECT a, b FROM cg ORDER BY a, b") == Rows{{"1", "1"}, {"1", "2"}});

        // SHOW CREATE TABLE names the key with all its columns
        auto shown = ex.execute_sql("SHOW CREATE TABLE cu");
        REQUIRE(shown.is_ok());
        REQUIRE(shown.value().find("UNIQUE KEY `ab` (`a`,`b`)") != std::string::npos);
    }
    // the keys are there after the server has been restarted
    Executor again(dir.path);
    ok(again, "USE d");
    fails(again, "INSERT INTO cu (a, b, v) VALUES (1, 2, 0)");
    ok(again, "INSERT INTO cu (a, b, v) VALUES (1, 3, 0)");
    fails(again, "INSERT INTO ce VALUES (4, 1, 2)");
    fails(again, "INSERT INTO cf VALUES (1, 2, 9)");
    fails(again, "INSERT INTO cg VALUES (1, 2)");
}

namespace {
struct KRow {
    int id;
    std::optional<int> a, b, c;
};

std::string show(const std::optional<int>& v) { return v ? std::to_string(*v) : "NULL"; }
} // namespace

TEST_CASE("writes against UNIQUE (a, b), UNIQUE (c) and a primary key match a model", "[composite_unique][random][fuzz]") {
    unsigned seed_count = 4; // RUSQL_FUZZ_SEEDS=40 runs a much longer campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 0;
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    std::size_t accepted = 0, rejected = 0;
    for (unsigned k = seed_start; k < seed_start + seed_count; k++) {
        INFO("seed " << k);
        std::mt19937 rng(777 + k * 17);
        TempDataDir dir("composite_unique_random");
        std::vector<KRow> model;
        auto pick = [&](int top) -> std::optional<int> {
            if (rng() % 6 == 0) return std::nullopt;
            return static_cast<int>(rng() % static_cast<unsigned>(top));
        };
        // does row `r` repeat the key of another row (not `skip_id`)? the ids of the rows it repeats
        auto clashes = [&](const KRow& r, int skip_id) {
            std::vector<int> ids;
            for (const KRow& o : model) {
                if (o.id == skip_id) continue;
                const bool same_id = o.id == r.id;
                const bool same_ab = r.a && r.b && o.a == r.a && o.b == r.b;
                const bool same_c = r.c && o.c == r.c;
                if (same_id || same_ab || same_c) ids.push_back(o.id);
            }
            return ids;
        };
        auto compare = [&](Executor& ex, const std::string& after) {
            Rows got = rows_of(ex, "SELECT id, a, b, c FROM r ORDER BY id");
            std::vector<KRow> want = model;
            std::sort(want.begin(), want.end(), [](const KRow& x, const KRow& y) { return x.id < y.id; });
            INFO("after " << after);
            REQUIRE(got.size() == want.size());
            for (std::size_t i = 0; i < want.size(); i++) {
                REQUIRE(got[i] == Rows::value_type{std::to_string(want[i].id), show(want[i].a), show(want[i].b), show(want[i].c)});
            }
        };

        std::string history; // every statement so far
        auto ex = std::make_unique<Executor>(dir.path);
        open_db(*ex);
        ok(*ex, "CREATE TABLE r (id INT PRIMARY KEY, a INT, b INT, c INT, UNIQUE KEY uab (a, b), UNIQUE KEY uc (c))");
        for (int step = 0; step < 220; step++) {
            const int kind = static_cast<int>(rng() % 10);
            const int id = static_cast<int>(rng() % 14);
            KRow row{id, pick(3), pick(3), pick(14)};
            std::string sql;
            bool should_succeed = true;
            if (kind < 4 || kind == 9) { // INSERT
                sql = "INSERT INTO r VALUES (" + std::to_string(id) + ", " + show(row.a) + ", " + show(row.b) + ", " + show(row.c) + ")";
                should_succeed = clashes(row, -1).empty();
                if (should_succeed) model.push_back(row);
            } else if (kind == 4) { // INSERT IGNORE
                sql = "INSERT IGNORE INTO r VALUES (" + std::to_string(id) + ", " + show(row.a) + ", " + show(row.b) + ", " + show(row.c) + ")";
                if (clashes(row, -1).empty()) model.push_back(row);
            } else if (kind == 5) { // REPLACE: every row the new one repeats goes
                sql = "REPLACE INTO r VALUES (" + std::to_string(id) + ", " + show(row.a) + ", " + show(row.b) + ", " + show(row.c) + ")";
                for (int victim : clashes(row, -1)) model.erase(std::remove_if(model.begin(), model.end(), [&](const KRow& o) { return o.id == victim; }), model.end());
                model.push_back(row);
            } else if (kind == 6 || kind == 7) { // UPDATE one row
                sql = "UPDATE r SET a = " + show(row.a) + ", b = " + show(row.b) + ", c = " + show(row.c) + " WHERE id = " + std::to_string(id);
                auto it = std::find_if(model.begin(), model.end(), [&](const KRow& o) { return o.id == id; });
                if (it != model.end()) {
                    should_succeed = clashes(row, id).empty();
                    if (should_succeed) *it = row;
                }
            } else { // DELETE
                sql = "DELETE FROM r WHERE id = " + std::to_string(id);
                model.erase(std::remove_if(model.begin(), model.end(), [&](const KRow& o) { return o.id == id; }), model.end());
            }
            INFO(sql);
            std::string state;
            for (const KRow& r : model) state += "(" + std::to_string(r.id) + "," + show(r.a) + "," + show(r.b) + "," + show(r.c) + ") ";
            INFO("the rows before: " << state);
            INFO("history: " << history);
            history += sql + "; ";
            auto result = ex->execute_sql(sql);
            INFO("error: " << (result.is_err() ? result.error() : std::string()));
            REQUIRE(result.is_ok() == should_succeed);
            (should_succeed ? accepted : rejected)++;
            compare(*ex, sql);
            if (step == 110) { // restart in the middle: the keys come back with the table
                ex.reset();
                ex = std::make_unique<Executor>(dir.path);
                ok(*ex, "USE d");
                compare(*ex, "restart");
            }
        }
        compare(*ex, "the end");
    }
    INFO("accepted " << accepted << " rejected " << rejected);
    REQUIRE(accepted > 0);
    REQUIRE(rejected > 0);
}
