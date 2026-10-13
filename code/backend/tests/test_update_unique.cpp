#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// An UPDATE that changes a UNIQUE column looks for each new value among the rows the table holds. With no index to ask, that was one scan of the table per
// changed row; now the values are looked for in one pass. The answer must not change: the statement is checked as a whole (a chain of values may shift, two rows
// may not end with one value), the message names the first row of the statement that clashes, and a failed statement leaves the table as it was. The random test
// runs the same statements on two tables, one whose column has a hash index (each value is asked of the index) and one that has none (one pass), against a model.

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

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}
std::string upper(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}
} // namespace

TEST_CASE("UPDATE of a UNIQUE column is checked as a whole, with an index and without", "[update_unique][random]") {
    TempDataDir dir("update_unique");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    unsigned seed_count = 3; // RUSQL_FUZZ_SEEDS=40 makes it a long campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));

    struct ModelRow {
        int id;
        std::optional<std::string> u;
        int v;
    };
    const std::vector<std::string> pool = {"a", "b", "c", "ab", "x", "y", "q", "r", "s", "t", "m", "n"};
    for (unsigned seed = 1; seed <= seed_count; seed++) {
        INFO("seed " << seed);
        std::mt19937 rng(seed * 104729u);
        auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
        auto spelled = [&](const std::string& w) { // the same word, spelled in either case
            return pick(2) ? upper(w) : w;
        };
        for (const char* suffix : {"a", "b"}) {
            ok(ex, std::string("DROP TABLE IF EXISTS uu_") + suffix);
            ok(ex, std::string("CREATE TABLE uu_") + suffix + " (id INT PRIMARY KEY, u VARCHAR(8) UNIQUE, v INT)");
        }
        ok(ex, "CREATE INDEX uu_b_u ON uu_b (u) USING HASH");
        std::vector<ModelRow> model;
        {
            std::vector<std::string> words = pool;
            std::shuffle(words.begin(), words.end(), rng);
            for (int id = 1; id <= 10; id++) {
                std::optional<std::string> u;
                if (pick(4) != 0) u = spelled(words[static_cast<std::size_t>(id)]);
                model.push_back({id, u, id * 10});
                for (const char* suffix : {"a", "b"}) {
                    ok(ex, std::string("INSERT INTO uu_") + suffix + " VALUES (" + std::to_string(id) + ", " + (u ? "'" + *u + "'" : "NULL") + ", " + std::to_string(id * 10) + ")");
                }
            }
        }
        for (int step = 0; step < 120; step++) {
            std::vector<ModelRow> next = model;
            std::string set_clause, where;
            const std::size_t kind = pick(6);
            if (kind == 0 || kind == 1) { // some rows take words of their own (or NULL)
                const std::size_t count = 2 + pick(3);
                std::vector<int> ids;
                while (ids.size() < count) {
                    int id = 1 + static_cast<int>(pick(10));
                    if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
                }
                set_clause = "u = CASE";
                where = "id IN (";
                for (std::size_t k = 0; k < ids.size(); k++) {
                    std::optional<std::string> w;
                    if (pick(6) != 0) w = spelled(pool[pick(pool.size())]);
                    set_clause += " WHEN id = " + std::to_string(ids[k]) + " THEN " + (w ? "'" + *w + "'" : "NULL");
                    where += (k ? ", " : "") + std::to_string(ids[k]);
                    next[static_cast<std::size_t>(ids[k] - 1)].u = w;
                }
                set_clause += " ELSE u END";
                where += ")";
            } else if (kind == 2) { // the case of every word changes (the same words, so no new clash)
                const int limit = 1 + static_cast<int>(pick(10));
                const bool up = pick(2) == 0;
                set_clause = std::string("u = ") + (up ? "UPPER(u)" : "LOWER(u)");
                where = "id <= " + std::to_string(limit);
                for (auto& row : next) {
                    if (row.id <= limit && row.u) row.u = up ? upper(*row.u) : lower(*row.u);
                }
            } else if (kind == 3) { // two words trade places: every value is held by a row the statement rewrites
                const std::string w1 = pool[pick(pool.size())], w2 = pool[pick(pool.size())];
                set_clause = "u = CASE WHEN u = '" + w1 + "' THEN '" + w2 + "' WHEN u = '" + w2 + "' THEN '" + w1 + "' ELSE u END";
                where = "id > 0";
                for (auto& row : next) {
                    if (!row.u) continue;
                    if (lower(*row.u) == w1) row.u = w2;
                    else if (lower(*row.u) == w2) row.u = w1;
                }
            } else if (kind == 4) { // a row alone takes a word (one value to look for)
                const int id = 1 + static_cast<int>(pick(10));
                const std::string w = spelled(pool[pick(pool.size())]);
                set_clause = "u = '" + w + "'";
                where = "id = " + std::to_string(id);
                next[static_cast<std::size_t>(id - 1)].u = w;
            } else { // the other column: the unique one is not touched
                set_clause = "v = v + 1";
                where = "id <= " + std::to_string(1 + static_cast<int>(pick(10)));
                for (auto& row : next) {
                    if (row.id <= std::stoi(where.substr(6))) row.v++;
                }
            }
            // the model: a statement is valid when no two rows end with one word (by case; NULLs repeat nothing)
            bool valid = true;
            for (std::size_t i = 0; i < next.size() && valid; i++) {
                for (std::size_t j = i + 1; j < next.size() && valid; j++) {
                    if (next[i].u && next[j].u && lower(*next[i].u) == lower(*next[j].u)) valid = false;
                }
            }
            const std::string statement_tail = " SET " + set_clause + " WHERE " + where;
            INFO("step " << step << ":" << statement_tail);
            auto ra = ex.execute_sql("UPDATE uu_a" + statement_tail);
            auto rb = ex.execute_sql("UPDATE uu_b" + statement_tail);
            INFO("a: " << (ra.is_err() ? ra.error() : "ok") << " | b: " << (rb.is_err() ? rb.error() : "ok"));
            REQUIRE(ra.is_ok() == valid);
            REQUIRE(rb.is_ok() == valid);
            if (valid) {
                model = next;
            } else {
                REQUIRE(ra.error().find("Duplicate") != std::string::npos);
                REQUIRE(ra.error() == rb.error()); // the index and the pass name the same clash
            }
            Rows expected;
            for (auto& row : model) expected.push_back({std::to_string(row.id), row.u ? *row.u : "NULL", std::to_string(row.v)});
            REQUIRE(rows_of(ex, "SELECT id, u, v FROM uu_a ORDER BY id") == expected);
            REQUIRE(rows_of(ex, "SELECT id, u, v FROM uu_b ORDER BY id") == expected);
        }
    }
}

TEST_CASE("UPDATE of a UNIQUE column over many rows names the first clashing row", "[update_unique]") {
    TempDataDir dir("update_unique_many");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE big (id INT PRIMARY KEY, n INT UNIQUE)");
    std::string values;
    for (int id = 1; id <= 600; id++) values += (id > 1 ? ", (" : "(") + std::to_string(id) + ", " + std::to_string(id * 2) + ")";
    ok(ex, "INSERT INTO big VALUES " + values);
    // the odd numbers are free: every one of 300 rows moves
    ok(ex, "UPDATE big SET n = n + 1 WHERE id <= 300");
    REQUIRE(rows_of(ex, "SELECT n FROM big WHERE id = 1 OR id = 300 OR id = 301 ORDER BY id") == Rows{{"3"}, {"601"}, {"602"}});
    // again: the rows that move end on 4, 6, ... 602, and 602 is held by row 301, which the statement does not rewrite; the other rows are free to move
    auto r = ex.execute_sql("UPDATE big SET n = n + 1 WHERE id <= 300");
    REQUIRE(r.is_err());
    REQUIRE(r.error() == "Duplicate value '602' for column 'n'");
    REQUIRE(rows_of(ex, "SELECT n FROM big WHERE id = 1 OR id = 300 OR id = 301 ORDER BY id") == Rows{{"3"}, {"601"}, {"602"}});
    // the first row of the statement that clashes is named, however many later rows clash too
    r = ex.execute_sql("UPDATE big SET n = n + 300 WHERE id > 100 AND id <= 400");
    REQUIRE(r.is_err());
    REQUIRE(r.error() == "Duplicate value '902' for column 'n'"); // (row 301 moves to 902, which row 451 holds; the odd values 503 ... 901 are free)
    REQUIRE(rows_of(ex, "SELECT COUNT(*), COUNT(DISTINCT n) FROM big") == Rows{{"600", "600"}});
    // a shift to values nobody holds is fine
    ok(ex, "UPDATE big SET n = n + 10000 WHERE id > 300");
    REQUIRE(rows_of(ex, "SELECT COUNT(*), COUNT(DISTINCT n) FROM big") == Rows{{"600", "600"}});
}
