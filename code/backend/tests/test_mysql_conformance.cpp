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
#include "engine/result_text.hpp"

using namespace engine;
namespace fs = std::filesystem;

// Statements whose answers were taken from a real MySQL 8.4 (code/test/diff/vs_mysql.py --emit): functions, operators and casts, queries over a table with
// NULLs, constraints and writes. Each case is {kind, statement, expected rows}: kind 0 is a setup statement, 1 a query with the rows MySQL gave, 2 a statement
// that succeeds, 3 one that fails, 4 one that is only run (RuSQL is known to differ from MySQL on it: DOUBLE aggregates, `||`, WITH ROLLUP, a text with a space at its
// end that goes through the text of a result (UNION, INSERT ... SELECT, IN (subquery)) ...; it is run so that the tables stay as they were in MySQL's run), 5 a query whose
// cells must also be the same TEXT (30.75 is not 30.7500: the DECIMAL corpus). Numbers compare by value (kind 1), rows as a set unless the statement has an ORDER BY.

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
const Case kCollation[] = {
#include "mysql_conformance_collation.inc"
};
const Case kDecimals[] = {
#include "mysql_conformance_decimals.inc"
};
const Case kTemporal[] = {
#include "mysql_conformance_temporal.inc"
};
const Case kRoutines[] = {
#include "mysql_conformance_routines.inc"
};
const Case kSchemaChange[] = {
#include "mysql_conformance_schema_change.inc"
};
const Case kAutoinc[] = {
#include "mysql_conformance_autoinc.inc"
};
const Case kShift[] = {
#include "mysql_conformance_shift.inc"
};
const Case kWhereErrors[] = {
#include "mysql_conformance_where_errors.inc"
};
const Case kArithTypes[] = {
#include "mysql_conformance_arith_types.inc"
};
const Case kFloat[] = {
#include "mysql_conformance_float.inc"
};
const Case kAnyValue[] = {
#include "mysql_conformance_any_value.inc"
};
const Case kViewOrder[] = {
#include "mysql_conformance_view_order.inc"
};
const Case kWindows[] = {
#include "mysql_conformance_windows.inc"
};
const Case kMaxExecutionTime[] = {
#include "mysql_conformance_max_execution_time.inc"
};
const Case kFkNames[] = {
#include "mysql_conformance_fk_names.inc"
};
const Case kWindowAgg[] = {
#include "mysql_conformance_window_agg.inc"
};
const Case kRangeFrames[] = {
#include "mysql_conformance_range_frames.inc"
};
const Case kTemporalValues[] = {
#include "mysql_conformance_temporal_values.inc"
};
const Case kFloatSingle[] = {
#include "mysql_conformance_float_single.inc"
};
const Case kTargetTable[] = {
#include "mysql_conformance_target_table.inc"
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
        rows.push_back(result_text::split_row(line)); // (the values as they are: a space at either end is written as an escape)
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
            case 1:
            case 5: {
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
                if (c.kind == 5) {
                    if (!ordered) {
                        std::sort(got.begin(), got.end());
                        std::sort(want.begin(), want.end());
                    }
                    REQUIRE(got == want); // (the same texts)
                } else {
                    REQUIRE(same_rows(got, want, true));
                }
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

TEST_CASE("DECIMAL and integer results are written with MySQL's places", "[mysql_conformance][decimals]") {
    run_cases("mc_decimals", kDecimals, sizeof kDecimals / sizeof kDecimals[0]);
}

TEST_CASE("fractional seconds, the forms of a date and a time, INTERVAL units and column types give MySQL's answers", "[mysql_conformance][temporal]") {
    run_cases("mc_temporal", kTemporal, sizeof kTemporal / sizeof kTemporal[0]);
}

TEST_CASE("texts compare, sort, group, join and stay unique by MySQL's default collation", "[mysql_conformance][collation]") {
    run_cases("mc_collation", kCollation, sizeof kCollation / sizeof kCollation[0]);
}

TEST_CASE("stored routines, prepared statements and division by zero give MySQL's answers", "[mysql_conformance][routines]") {
    run_cases("mc_routines", kRoutines, sizeof kRoutines / sizeof kRoutines[0]);
}

TEST_CASE("ALTER TABLE converts the values and keeps the indexes right as MySQL does", "[mysql_conformance][schema_change]") {
    run_cases("mc_schema_change", kSchemaChange, sizeof kSchemaChange / sizeof kSchemaChange[0]);
}

TEST_CASE("the AUTO_INCREMENT numbers a statement reserves, skips and loses are InnoDB's", "[mysql_conformance][autoinc]") {
    run_cases("mc_autoinc", kAutoinc, sizeof kAutoinc / sizeof kAutoinc[0]);
}

TEST_CASE("an UPDATE that shifts a UNIQUE or PRIMARY KEY value fits or fails as InnoDB's row by row writing does", "[mysql_conformance][shift]") {
    run_cases("mc_shift", kShift, sizeof kShift / sizeof kShift[0]);
}

TEST_CASE("the WHERE of an UPDATE or a DELETE fails on a division by zero or a text that is no number, as a write does", "[mysql_conformance][where_errors]") {
    run_cases("mc_where_errors", kWhereErrors, sizeof kWhereErrors / sizeof kWhereErrors[0]);
}

TEST_CASE("a date, a time, an ENUM and a SET are the numbers MySQL reads them as in arithmetic, comparisons and numeric functions", "[mysql_conformance][arith_types]") {
    run_cases("mc_arith_types", kArithTypes, sizeof kArithTypes / sizeof kArithTypes[0]);
}

TEST_CASE("a FLOAT column is shown with six significant digits", "[mysql_conformance][float]") {
    run_cases("mc_float", kFloat, sizeof kFloat / sizeof kFloat[0]);
}

TEST_CASE("ANY_VALUE is a function that lets ONLY_FULL_GROUP_BY pass a column the group does not fix", "[mysql_conformance][any_value]") {
    run_cases("mc_any_value", kAnyValue, sizeof kAnyValue / sizeof kAnyValue[0]);
}

TEST_CASE("UPDATE and DELETE through a view take ORDER BY and LIMIT", "[mysql_conformance][view_order]") {
    run_cases("mc_view_order", kViewOrder, sizeof kViewOrder / sizeof kViewOrder[0]);
}

TEST_CASE("window frames have a short form and a window can be named in WINDOW and used with OVER w", "[mysql_conformance][windows]") {
    run_cases("mc_windows", kWindows, sizeof kWindows / sizeof kWindows[0]);
}

TEST_CASE("a SELECT that runs longer than MAX_EXECUTION_TIME fails with error 3024", "[mysql_conformance][max_execution_time]") {
    run_cases("mc_max_execution_time", kMaxExecutionTime, sizeof kMaxExecutionTime / sizeof kMaxExecutionTime[0]);
}

TEST_CASE("a foreign key keeps its name, DROP FOREIGN KEY takes it, referential_constraints lists the rules", "[mysql_conformance][fk_names]") {
    run_cases("mc_fk_names", kFkNames, sizeof kFkNames / sizeof kFkNames[0]);
}

TEST_CASE("window functions run on the groups: over aggregates, inside expressions, with DISTINCT and ORDER BY", "[mysql_conformance][window_agg]") {
    run_cases("mc_window_agg", kWindowAgg, sizeof kWindowAgg / sizeof kWindowAgg[0]);
}

TEST_CASE("RANGE frames with an offset reach the rows within n (or an INTERVAL) of the current row's ORDER BY value; a frame can hold no row", "[mysql_conformance][range_frames]") {
    run_cases("mc_range_frames", kRangeFrames, sizeof kRangeFrames / sizeof kRangeFrames[0]);
}

TEST_CASE("the result of a date function and a SUM of dates are the numbers MySQL reads them as", "[mysql_conformance][temporal_values]") {
    run_cases("mc_temporal_values", kTemporalValues, sizeof kTemporalValues / sizeof kTemporalValues[0]);
}

TEST_CASE("a FLOAT column is the single-precision number in arithmetic and comparisons", "[mysql_conformance][float_single]") {
    run_cases("mc_float_single", kFloatSingle, sizeof kFloatSingle / sizeof kFloatSingle[0]);
}

TEST_CASE("an UPDATE, DELETE or INSERT cannot read the table it writes in a subquery (1093)", "[mysql_conformance][target_table]") {
    run_cases("mc_target_table", kTargetTable, sizeof kTargetTable / sizeof kTargetTable[0]);
}
