#include <algorithm>
#include <array>
#include <filesystem>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/storage/buffer_pool.hpp"
#include "engine/transaction/redo_log.hpp"

using namespace engine;
namespace fs = std::filesystem;

// Multi-row INSERT / bulk write paths: duplicate detection inside one statement, batched index maintenance, and the
// storage helpers a checkpoint relies on. (The row JSON and key-comparison rewrites have their own differential tests:
// test_row_json.cpp and test_btree.cpp.)

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

void open_db(Executor& ex) {
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
}

// the answer of `WHERE pred` through whatever index the planner picks versus the same predicate forced onto a scan
void check_same(Executor& ex, const std::string& table, const std::string& pred) {
    INFO(table << ": " << pred);
    REQUIRE(sorted_lines(ok_text(ex, "SELECT id FROM " + table + " WHERE " + pred)) ==
            sorted_lines(ok_text(ex, "SELECT id FROM " + table + " WHERE (" + pred + ") OR id < 0")));
}
} // namespace

TEST_CASE("bulk INSERT: duplicates inside one statement are reported exactly as a pairwise scan would", "[bulk_write]") {
    // Reference = the check this replaced: for each row, scan every earlier row of the statement, the earliest match wins
    // and within that row the lowest column. Values collide often (small ranges) so most batches contain duplicates.
    TempDataDir dir("bulk_dup");
    Executor ex(dir.path);
    open_db(ex);
    std::mt19937 rng(77);
    for (int iter = 0; iter < 400; iter++) {
        REQUIRE(ex.execute_sql("DROP TABLE IF EXISTS u").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE u (id INT PRIMARY KEY, a INT UNIQUE, b INT UNIQUE, c INT)").is_ok());
        int n = 1 + static_cast<int>(rng() % 12);
        std::vector<std::array<int, 3>> rows;
        std::string values;
        for (int i = 0; i < n; i++) {
            int id = 1 + static_cast<int>(rng() % 30), a = 1 + static_cast<int>(rng() % 20), b = 1 + static_cast<int>(rng() % 20);
            rows.push_back({id, a, b});
            values += (i ? ", (" : "(") + std::to_string(id) + ", " + std::to_string(a) + ", " + std::to_string(b) + ", " + std::to_string(i) + ")";
        }
        std::string expected_error;
        const char* cols[] = {"id", "a", "b"};
        for (int k = 1; k < n && expected_error.empty(); k++) {
            for (int r = 0; r < k && expected_error.empty(); r++) {
                for (int c = 0; c < 3; c++) {
                    if (rows[r][c] == rows[k][c]) {
                        expected_error = "Duplicate value '" + std::to_string(rows[k][c]) + "' for column '" + cols[c] + "'";
                        break;
                    }
                }
            }
        }
        auto res = ex.execute_sql("INSERT INTO u VALUES " + values);
        INFO("batch: " << values);
        if (expected_error.empty()) {
            REQUIRE(res.is_ok());
            REQUIRE(ok_text(ex, "SELECT COUNT(*) FROM u").find(std::to_string(n)) != std::string::npos);
        } else {
            REQUIRE_FALSE(res.is_ok());
            REQUIRE(res.error() == expected_error);
            REQUIRE(ok_text(ex, "SELECT * FROM u").find("0 rows") != std::string::npos); // nothing of a failed statement is kept
        }
    }
}

TEST_CASE("bulk INSERT: a composite primary key repeated inside one statement is rejected", "[bulk_write]") {
    TempDataDir dir("bulk_dup_comp");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE p (a INT, b INT, v INT, PRIMARY KEY (a, b))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO p VALUES (1, 1, 0), (1, 2, 0), (2, 1, 0), (2, 2, 0)").is_ok());
    auto dup = ex.execute_sql("INSERT INTO p VALUES (3, 1, 0), (3, 2, 0), (3, 1, 9)");
    REQUIRE_FALSE(dup.is_ok());
    REQUIRE(dup.error().find("Duplicate composite primary key") != std::string::npos);
    // (1, 11) and (11, 1) must not be mistaken for one tuple
    REQUIRE(ex.execute_sql("INSERT INTO p VALUES (1, 11, 0), (11, 1, 0)").is_ok());
    REQUIRE(ok_text(ex, "SELECT COUNT(*) FROM p").find("6") != std::string::npos);
}

TEST_CASE("bulk INSERT: secondary, hash and composite indexes stay equal to a scan after big batches", "[bulk_write]") {
    TempDataDir dir("bulk_idx");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, grp INT, tag VARCHAR(10), c DECIMAL(10,2), pad INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX t_grp ON t (grp)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX t_tag ON t (tag) USING HASH").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX t_gc ON t (grp, c)").is_ok());
    // one index created before the data, one after
    int next = 1;
    for (int batch = 0; batch < 6; batch++) {
        std::string values;
        for (int i = 0; i < 700; i++, next++) {
            values += (i ? ", (" : "(") + std::to_string(next) + ", " + std::to_string(next % 40) + ", 't" + std::to_string(next % 17) + "', " +
                      std::to_string(next % 9) + (next % 3 ? ".50" : ".5") + ", " + std::to_string(next) + ")";
        }
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES " + values).is_ok());
        if (batch == 2) REQUIRE(ex.execute_sql("CREATE INDEX t_c ON t (c)").is_ok());
    }
    REQUIRE(ok_text(ex, "SELECT COUNT(*) FROM t").find("4200") != std::string::npos);
    for (int g = 0; g < 40; g += 3) check_same(ex, "t", "grp = " + std::to_string(g));
    for (int k = 0; k < 17; k += 2) check_same(ex, "t", "tag = 't" + std::to_string(k) + "'");
    check_same(ex, "t", "grp = 7 AND c = 3.5");
    check_same(ex, "t", "grp = 7.0 AND c = 3.50");
    check_same(ex, "t", "grp = 12");
    check_same(ex, "t", "c = 4.5");
    check_same(ex, "t", "c >= 7.5");
    check_same(ex, "t", "id BETWEEN 100 AND 130");
    check_same(ex, "t", "id = 4200");
    // the bucket of one value really holds every row of it (4200 / 40 = 105)
    REQUIRE(ok_text(ex, "SELECT id FROM t WHERE grp = 5").find("105 row(s)") != std::string::npos);
    // and later single-row maintenance still works on batch-built buckets
    REQUIRE(ex.execute_sql("DELETE FROM t WHERE id = 45").is_ok());
    REQUIRE(ex.execute_sql("UPDATE t SET grp = 5 WHERE id = 46").is_ok());
    REQUIRE(ok_text(ex, "SELECT id FROM t WHERE grp = 5").find("105 row(s)") != std::string::npos); // -1 (id 45) +1 (id 46)
    check_same(ex, "t", "grp = 5");
    check_same(ex, "t", "grp = 6");
}

TEST_CASE("bulk INSERT: rows survive a restart and keep their pk positions", "[bulk_write]") {
    TempDataDir dir("bulk_restart");
    {
        Executor ex(dir.path);
        open_db(ex);
        REQUIRE(ex.execute_sql("CREATE TABLE r (id INT PRIMARY KEY, v INT)").is_ok());
        for (int batch = 0; batch < 5; batch++) {
            std::string values;
            for (int i = 0; i < 500; i++) values += (i ? ", (" : "(") + std::to_string(batch * 500 + i) + ", " + std::to_string(i) + ")";
            REQUIRE(ex.execute_sql("INSERT INTO r VALUES " + values).is_ok());
        }
        REQUIRE(ex.execute_sql("DELETE FROM r WHERE id BETWEEN 100 AND 199").is_ok());
        REQUIRE(ex.execute_sql("UPDATE r SET v = -1 WHERE id = 1234").is_ok());
    }
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ok_text(ex, "SELECT COUNT(*) FROM r").find("2400") != std::string::npos);
    REQUIRE(ok_text(ex, "SELECT v FROM r WHERE id = 1234").find("-1") != std::string::npos);
    check_same(ex, "r", "id BETWEEN 90 AND 210");
    check_same(ex, "r", "id = 150");
}

TEST_CASE("BufferPool::write_through saves the rows and leaves no stale cached copy", "[bulk_write][storage]") {
    fs::remove_all("bulk_wt_data");
    {
        DiskManager disk("bulk_wt_data");
        BufferPool pool(4);
        std::vector<Row> v1{{{"id", "1"}}, {{"id", "2"}}};
        std::vector<Row> v2{{{"id", "1"}}, {{"id", "2"}}, {{"id", "3"}}};
        pool.write_page("d.t", v1);
        REQUIRE(pool.usage() == 1);
        pool.write_through("d.t", v2, disk);
        REQUIRE(pool.usage() == 0); // the stale page is gone; nothing was copied into the pool
        REQUIRE(disk.load_table("d.t").size() == 3);
        REQUIRE(pool.get_page("d.t", disk).size() == 3); // a later read sees the new rows
    }
    fs::remove_all("bulk_wt_data");
}

TEST_CASE("RedoLog: the checkpoint trigger can be raised and a clear resets it", "[bulk_write][redo]") {
    fs::remove_all("bulk_redo_data");
    fs::create_directories("bulk_redo_data");
    {
        RedoLog log("bulk_redo_data/rusql.redo");
        REQUIRE(log.checkpoint_at() == 0);
        log.set_checkpoint_at(123456);
        REQUIRE(log.checkpoint_at() == 123456);
        RedoOp op;
        op.kind = RedoOp::Kind::InsertVersion;
        op.table = "d.t";
        op.row_json = "{\"id\":\"1\"}";
        log.append_batch({op});
        REQUIRE(log.bytes() > 0);
        log.clear();
        REQUIRE(log.bytes() == 0);
        REQUIRE(log.checkpoint_at() == 0);
    }
    fs::remove_all("bulk_redo_data");
}

// ---------------------------------------------------------------------------------------------------------------------
// ORDER BY [LIMIT/OFFSET] of a single-table scan sorts row pointers by pre-parsed keys (order_rows in executor_select.cpp).
// The reference below re-implements the comparison the engine has always used (numbers compare as numbers when both
// sides parse, otherwise as text; ties keep table order) and checks every id of every window.

namespace {
std::vector<std::vector<std::string>> table_cells(const std::string& text) {
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

bool ref_number(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size() && s.find_first_of("xXnN") == std::string::npos; // from_chars grammar, not strtod's hex/inf/nan
}
// `text`: the values are those of a text column, which sort as text whatever they look like ('10' < '9'); the others of a number column
int ref_cmp(const std::string& a, const std::string& b, bool text) {
    double x, y;
    if (!text && ref_number(a, x) && ref_number(b, y)) return x < y ? -1 : (x > y ? 1 : 0);
    return a < b ? -1 : (a > b ? 1 : 0);
}
} // namespace

TEST_CASE("ORDER BY / LIMIT / OFFSET of a scan returns exactly the reference order", "[bulk_write][order_by]") {
    TempDataDir dir("order_by_ref");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE o (id INT PRIMARY KEY, a VARCHAR(10), b INT, c VARCHAR(10))").is_ok());
    std::mt19937 rng(5150);
    static const char* a_vals[] = {"x", "y", "10", "9", "9.5", "07", "7", "abc", "ABC", "b", "100", "-3", "z9"};
    std::string values;
    for (int i = 1; i <= 150; i++) {
        values += (i > 1 ? ", (" : "(") + std::to_string(i) + ", '" + a_vals[rng() % 13] + "', " + std::to_string(static_cast<int>(rng() % 12) - 3) + ", '" +
                  a_vals[rng() % 13] + "')";
    }
    REQUIRE(ex.execute_sql("INSERT INTO o VALUES " + values).is_ok());
    auto all = table_cells(ok_text(ex, "SELECT id, a, b, c FROM o")); // table order
    REQUIRE(all.size() == 150);
    const char* col_names[] = {"id", "a", "b", "c"};
    for (int iter = 0; iter < 400; iter++) {
        std::vector<std::pair<int, bool>> keys; // (column index, ascending)
        std::string order_sql;
        for (std::size_t k = 0, n = 1 + rng() % 3; k < n; k++) {
            int col = static_cast<int>(rng() % 4);
            bool asc = rng() % 2 == 0;
            keys.emplace_back(col, asc);
            order_sql += (k ? ", " : "") + std::string(col_names[col]) + (asc ? "" : " DESC");
        }
        std::size_t limits[] = {0, 1, 5, 20, 200};
        std::size_t offsets[] = {0, 3, 50, 300};
        bool use_limit = rng() % 3 != 0, use_offset = use_limit && rng() % 2 == 0;
        std::size_t lim = limits[rng() % 5], off = offsets[rng() % 4];
        std::string sql = "SELECT id FROM o ORDER BY " + order_sql;
        if (use_limit) sql += " LIMIT " + std::to_string(lim);
        if (use_offset) sql += " OFFSET " + std::to_string(off);

        std::vector<std::size_t> order(all.size());
        for (std::size_t i = 0; i < order.size(); i++) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) {
            for (auto& [col, asc] : keys) {
                int c = ref_cmp(all[x][static_cast<std::size_t>(col)], all[y][static_cast<std::size_t>(col)], col == 1 || col == 3); // (a and c are text)
                if (!asc) c = -c;
                if (c != 0) return c < 0;
            }
            return false;
        });
        std::size_t from = use_offset ? std::min(off, order.size()) : 0, to = order.size();
        if (use_limit && to - from > lim) to = from + lim;
        std::vector<std::string> expected;
        for (std::size_t i = from; i < to; i++) expected.push_back(all[order[i]][0]);

        INFO(sql);
        auto got = table_cells(ok_text(ex, sql));
        std::vector<std::string> actual;
        for (auto& r : got) actual.push_back(r.at(0));
        REQUIRE(actual == expected);
    }
}
