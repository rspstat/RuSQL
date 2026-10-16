#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// INSERT INTO t SET a = 1, b = 'x' is INSERT INTO t (a, b) VALUES (1, 'x'): the same row, the same conflict handling (IGNORE, ON DUPLICATE KEY UPDATE, REPLACE).

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

std::string rows_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_ok());
    std::string out, line;
    std::istringstream in(r.value());
    while (std::getline(in, line)) {
        if (!line.empty() && (line[0] == '|' || line[0] == '+')) out += line + "\n";
    }
    return out;
}
} // namespace

TEST_CASE("INSERT ... SET and REPLACE ... SET give the row the VALUES form gives", "[parser][insert_set]") {
    TempDataDir dir("insert_set");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    for (const char* t : {"a", "b"}) {
        REQUIRE(ex.execute_sql(std::string("CREATE TABLE ") + t + " (id INT PRIMARY KEY AUTO_INCREMENT, n INT DEFAULT 7, s VARCHAR(20), v INT UNIQUE)").is_ok());
    }
    REQUIRE(ex.execute_sql("SET @x = 41").is_ok());

    // {SET form on table a, the same statement as VALUES on table b}
    const std::vector<std::pair<std::string, std::string>> pairs = {
        {"INSERT INTO a SET id = 1, n = 5, s = 'one', v = 10", "INSERT INTO b (id, n, s, v) VALUES (1, 5, 'one', 10)"},
        {"INSERT INTO a SET s = 'auto'", "INSERT INTO b (s) VALUES ('auto')"},
        {"INSERT INTO a SET id = 5, n = DEFAULT, s = NULL, v = 50", "INSERT INTO b (id, n, s, v) VALUES (5, DEFAULT, NULL, 50)"},
        {"INSERT INTO a SET id = 6, n = 1 + 2 * 3, s = CONCAT('a', 'b'), v = @x + 1", "INSERT INTO b (id, n, s, v) VALUES (6, 1 + 2 * 3, CONCAT('a', 'b'), @x + 1)"},
        {"INSERT INTO a SET id = 7, s = 'it''s', v = -3", "INSERT INTO b (id, s, v) VALUES (7, 'it''s', -3)"},
        {"INSERT INTO a SET id = 8, v = 80", "INSERT INTO b (id, v) VALUES (8, 80)"},
        {"INSERT IGNORE INTO a SET id = 1, s = 'dup'", "INSERT IGNORE INTO b (id, s) VALUES (1, 'dup')"},
        {"INSERT INTO a SET id = 1, s = 'upd', v = 10 ON DUPLICATE KEY UPDATE s = 'changed', n = n + 1", "INSERT INTO b (id, s, v) VALUES (1, 'upd', 10) ON DUPLICATE KEY UPDATE s = 'changed', n = n + 1"},
        {"INSERT INTO a SET id = 9, s = 'nine' ON DUPLICATE KEY UPDATE n = 0", "INSERT INTO b (id, s) VALUES (9, 'nine') ON DUPLICATE KEY UPDATE n = 0"},
        {"REPLACE INTO a SET id = 5, s = 'replaced', v = 55", "REPLACE INTO b (id, s, v) VALUES (5, 'replaced', 55)"},
        {"REPLACE a SET id = 12, s = 'twelve'", "REPLACE b (id, s) VALUES (12, 'twelve')"},
    };
    for (auto& [with_set, with_values] : pairs) {
        INFO("SET form: " << with_set);
        auto ra = ex.execute_sql(with_set);
        auto rb = ex.execute_sql(with_values);
        REQUIRE(ra.is_ok() == rb.is_ok());
        REQUIRE(ra.is_ok());
        REQUIRE(rows_of(ex, "SELECT id, n, s, v FROM a ORDER BY id") == rows_of(ex, "SELECT id, n, s, v FROM b ORDER BY id"));
    }

    // the statements that must not work still do not
    for (const char* bad : {"INSERT INTO a (s) SET id = 1", "INSERT INTO a SET", "INSERT INTO a SET id 1", "INSERT INTO a SET id = 1,", "INSERT INTO a SET = 1",
                            "REPLACE INTO a (s) SET id = 1",
                            // a column named twice (MySQL error 1110), in every form
                            "INSERT INTO a SET id = 21, id = 22", "INSERT INTO a (id, id) VALUES (30, 31)", "REPLACE INTO a (s, s) VALUES ('x', 'y')",
                            "INSERT INTO a (s, s) SELECT s, s FROM b"}) {
        INFO(bad);
        REQUIRE(ex.execute_sql(bad).is_err());
    }
    // an error inside the row (a duplicate primary key) is the same error as with VALUES
    auto dup_set = ex.execute_sql("INSERT INTO a SET id = 1, v = 999");
    auto dup_values = ex.execute_sql("INSERT INTO a (id, v) VALUES (1, 999)");
    REQUIRE(dup_set.is_err());
    REQUIRE(dup_values.is_err());
    REQUIRE(dup_set.error() == dup_values.error());
}
