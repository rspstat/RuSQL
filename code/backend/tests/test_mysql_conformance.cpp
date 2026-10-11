#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// Statements whose answers were taken from a real MySQL 8.4 (code/test/diff/vs_mysql.py --emit): functions, operators and casts, queries over a table with
// NULLs, constraints and writes. Each case is {kind, statement, expected rows}: kind 0 is a setup statement, 1 a query with the rows MySQL gave, 2 a statement
// that succeeds, 3 one that fails, 4 one that is only run (RuSQL is known to differ from MySQL on it: the case-insensitive collation of MySQL's default, the
// difference between DOUBLE and DECIMAL arithmetic, ...; it is run so that the tables stay as they were in MySQL's run). Numbers compare by value, rows as a
// set unless the statement has an ORDER BY.

namespace {
struct Case {
    int kind;
    const char* sql;
    const char* expected; // rows end with \036, cells are separated by \037
};

const Case kFunctions[] = {
#include "mysql_conformance_functions.inc"
};
const Case kTables[] = {
#include "mysql_conformance_tables.inc"
};
const Case kConstraints[] = {
#include "mysql_conformance_constraints.inc"
};

struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

using Rows = std::vector<std::vector<std::string>>;

Rows decode(const char* text) {
    Rows rows;
    std::string cell;
    std::vector<std::string> row;
    for (const char* p = text; *p; p++) {
        if (*p == '\037') {
            row.push_back(cell);
            cell.clear();
        } else if (*p == '\036') {
            row.push_back(cell);
            cell.clear();
            rows.push_back(std::move(row));
            row.clear();
        } else {
            cell.push_back(*p);
        }
    }
    return rows;
}

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

bool same_cell(const std::string& a, const std::string& b) {
    if (a == b) return true;
    char *ea = nullptr, *eb = nullptr;
    const double x = std::strtod(a.c_str(), &ea), y = std::strtod(b.c_str(), &eb);
    if (a.empty() || b.empty() || *ea != '\0' || *eb != '\0') return false;
    return std::abs(x - y) <= 1e-9 * std::max(1.0, std::abs(x));
}

bool same_rows(Rows a, Rows b, bool ordered) {
    if (a.size() != b.size()) return false;
    if (!ordered) {
        std::sort(a.begin(), a.end());
        std::sort(b.begin(), b.end());
    }
    for (std::size_t i = 0; i < a.size(); i++) {
        if (a[i].size() != b[i].size()) return false;
        for (std::size_t k = 0; k < a[i].size(); k++) {
            if (!same_cell(a[i][k], b[i][k])) return false;
        }
    }
    return true;
}

// numbers that print differently (a number string for another) sort apart when unordered: sort by numeric value when every cell is a number
void normalize_for_set(Rows& rows) {
    std::sort(rows.begin(), rows.end(), [](const std::vector<std::string>& x, const std::vector<std::string>& y) {
        for (std::size_t k = 0; k < x.size() && k < y.size(); k++) {
            char *ex = nullptr, *ey = nullptr;
            const double a = std::strtod(x[k].c_str(), &ex), b = std::strtod(y[k].c_str(), &ey);
            const bool numbers = !x[k].empty() && !y[k].empty() && *ex == '\0' && *ey == '\0';
            if (numbers ? a != b : x[k] != y[k]) return numbers ? a < b : x[k] < y[k];
        }
        return x.size() < y.size();
    });
}

void run_cases(const char* dir, const Case* cases, std::size_t count) {
    TempDataDir data(dir);
    Executor ex(data.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE v").is_ok());
    REQUIRE(ex.execute_sql("USE v").is_ok());
    const char* setup[] = {
        "CREATE TABLE t (id INT PRIMARY KEY, i INT, d DOUBLE, s VARCHAR(20), dt DATE, g INT)",
        "INSERT INTO t VALUES (1, 10, 1.5, 'apple', '2024-01-31', 1), (2, NULL, NULL, 'Apple', '2024-02-29', 1), (3, 30, 2.5, 'BANANA', NULL, 2),"
        " (4, 0, 0, 'banana', '2023-12-25', 2), (5, 7, 3.25, '10', '2024-03-05', 3), (6, -3, -1.5, '9', '2024-03-05', 3), (7, 7, 3.25, 'abc', '2020-02-29', 3),"
        " (8, NULL, 8.5, '', '2024-12-31', 4), (9, 100, 100, NULL, '2024-06-15', 4), (10, 12, 0.5, ' x ', '2024-06-16', 4)",
        "CREATE TABLE u (id INT PRIMARY KEY, k INT, name VARCHAR(10))",
        "INSERT INTO u VALUES (1, 10, 'one'), (2, 20, 'Two'), (3, NULL, 'three'), (4, 10, NULL)",
    };
    for (const char* sql : setup) REQUIRE(ex.execute_sql(sql).is_ok());
    for (std::size_t n = 0; n < count; n++) {
        const Case& c = cases[n];
        INFO("case " << n << ": " << c.sql);
        auto result = ex.execute_sql(c.sql);
        switch (c.kind) {
            case 0:
            case 2: {
                INFO("error: " << (result.is_err() ? result.error() : std::string()));
                REQUIRE(result.is_ok());
                break;
            }
            case 3: {
                REQUIRE(result.is_err());
                break;
            }
            case 1: {
                INFO("error: " << (result.is_err() ? result.error() : std::string()));
                REQUIRE(result.is_ok());
                Rows got = cells(result.value()), want = decode(c.expected);
                const bool ordered = std::string(c.sql).find("ORDER BY") != std::string::npos;
                if (!ordered) {
                    normalize_for_set(got);
                    normalize_for_set(want);
                }
                std::ostringstream shown;
                for (auto& r : got) {
                    shown << "[";
                    for (auto& v : r) shown << v << "|";
                    shown << "] ";
                }
                INFO("got " << shown.str());
                REQUIRE(same_rows(got, want, true));
                break;
            }
            default: {
                break; // (run only)
            }
        }
    }
}
} // namespace

TEST_CASE("functions, operators and casts give MySQL's answers", "[mysql_conformance]") {
    run_cases("mc_functions", kFunctions, sizeof kFunctions / sizeof kFunctions[0]);
}

TEST_CASE("queries over a table with NULLs give MySQL's answers", "[mysql_conformance]") {
    run_cases("mc_tables", kTables, sizeof kTables / sizeof kTables[0]);
}

TEST_CASE("constraints, defaults, writes and transactions give MySQL's answers", "[mysql_conformance]") {
    run_cases("mc_constraints", kConstraints, sizeof kConstraints / sizeof kConstraints[0]);
}
