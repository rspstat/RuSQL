#include <algorithm>
#include <array>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

// SELECT paths that work on pointers to the table's rows instead of copies: aggregates, GROUP BY, HAVING, DISTINCT, ORDER BY /
// LIMIT, and the join shortcuts (WHERE conjuncts that read one table are applied before the join, LEFT/INNER joins whose ON
// contains an equality are hashed on it, IndexNL probes are cached per key). Each is checked against something that does
// not share its code: a reference computed here, or the very same query written so that the shortcut cannot apply.

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

std::string ok_text(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql);
    REQUIRE(r.is_ok());
    return r.value();
}

std::vector<std::string> sorted_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    std::sort(lines.begin(), lines.end());
    return lines;
}

// The cells of a result table, one vector per row (the header line is skipped).
std::vector<std::vector<std::string>> table_cells(const std::string& text) {
    std::vector<std::vector<std::string>> rows;
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

bool ref_number(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return end == s.c_str() + s.size() && s.find_first_of("xXnN") == std::string::npos;
}
int ref_cmp(const std::string& a, const std::string& b) {
    double x, y;
    if (ref_number(a, x) && ref_number(b, y)) return x < y ? -1 : (x > y ? 1 : 0);
    return a < b ? -1 : (a > b ? 1 : 0);
}

// A table of the reference model: every value is text, "NULL" for NULL, like the engine stores it.
struct RefRow {
    std::map<std::string, std::string> v;
    const std::string& at(const std::string& c) const { return v.at(c); }
};
} // namespace

TEST_CASE("GROUP BY and DISTINCT tell composite keys apart value by value", "[query_paths]") {
    TempDataDir dir("qp_keys");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE k (id INT PRIMARY KEY, a VARCHAR(10), b VARCHAR(10))").is_ok());
    // ("ab","c") and ("a","bc") concatenate to the same text; so do (NULL, "abc") and ("abc", NULL) -- the engine stores an
    // empty string as NULL
    REQUIRE(ex.execute_sql("INSERT INTO k VALUES (1,'ab','c'), (2,'a','bc'), (3,'ab','c'), (4,'a','bc'), (5,'','abc'), (6,'abc',''), "
                           "(7,'ab','c'), (8,NULL,'x'), (9,NULL,'x'), (10,'x',NULL)")
                .is_ok());
    std::map<std::string, std::string> groups; // "a|b" -> COUNT(*)
    for (auto& r : table_cells(ok_text(ex, "SELECT a, b, COUNT(*) FROM k GROUP BY a, b"))) groups[r[0] + "|" + r[1]] = r[2];
    REQUIRE(groups == std::map<std::string, std::string>{{"ab|c", "3"}, {"a|bc", "2"}, {"NULL|abc", "1"}, {"abc|NULL", "1"}, {"NULL|x", "2"}, {"x|NULL", "1"}});

    std::set<std::string> distinct;
    auto cells = table_cells(ok_text(ex, "SELECT DISTINCT a, b FROM k"));
    for (auto& r : cells) distinct.insert(r[0] + "|" + r[1]);
    REQUIRE(cells.size() == 6);
    REQUIRE(distinct.size() == 6);
    // groups come out in order of first appearance
    auto order = table_cells(ok_text(ex, "SELECT a, b FROM k GROUP BY a, b"));
    REQUIRE(order.size() == 6);
    REQUIRE(order[0][0] == "ab");
    REQUIRE(order[1][0] == "a");
    REQUIRE(order[2][0] == "NULL");
    REQUIRE(order[3][0] == "abc");
}

TEST_CASE("GROUP BY with aggregates, WHERE, HAVING, ORDER BY and LIMIT matches a reference", "[query_paths]") {
    TempDataDir dir("qp_group");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE g (id INT PRIMARY KEY, grp INT, val INT, tag VARCHAR(10))").is_ok());
    std::mt19937 rng(2468);
    static const char* tags[] = {"red", "green", "blue", "10", "9"};
    std::vector<RefRow> table;
    std::string values;
    for (int i = 1; i <= 300; i++) {
        RefRow r;
        r.v["id"] = std::to_string(i);
        r.v["grp"] = rng() % 12 == 0 ? "NULL" : std::to_string(rng() % 8);
        r.v["val"] = rng() % 5 == 0 ? "NULL" : std::to_string(rng() % 60);
        r.v["tag"] = rng() % 9 == 0 ? "NULL" : tags[rng() % 5];
        values += (i > 1 ? ", (" : "(") + r.at("id") + ", " + r.at("grp") + ", " + r.at("val") + ", " + (r.at("tag") == "NULL" ? "NULL" : "'" + r.at("tag") + "'") + ")";
        table.push_back(std::move(r));
    }
    REQUIRE(ex.execute_sql("INSERT INTO g VALUES " + values).is_ok());

    struct Leaf {
        std::string sql;
        std::function<bool(const RefRow&)> pred;
    };
    auto make_leaf = [&]() -> Leaf {
        int k = static_cast<int>(rng() % 6);
        int n = static_cast<int>(rng() % 60);
        switch (k) {
            case 0: return {"val > " + std::to_string(n), [n](const RefRow& r) { return r.at("val") != "NULL" && std::stoi(r.at("val")) > n; }};
            case 1: return {"val <= " + std::to_string(n), [n](const RefRow& r) { return r.at("val") != "NULL" && std::stoi(r.at("val")) <= n; }};
            case 2: return {"grp = " + std::to_string(n % 8), [n](const RefRow& r) { return r.at("grp") != "NULL" && std::stoi(r.at("grp")) == n % 8; }};
            case 3: return {"tag = '" + std::string(tags[n % 5]) + "'", [n](const RefRow& r) { return r.at("tag") == tags[n % 5]; }};
            case 4: return {"val IS NULL", [](const RefRow& r) { return r.at("val") == "NULL"; }};
            default: return {"tag IS NOT NULL", [](const RefRow& r) { return r.at("tag") != "NULL"; }};
        }
    };

    for (int iter = 0; iter < 250; iter++) {
        // GROUP BY <keys> with COUNT(*), COUNT(val), SUM(val), MIN(val), MAX(val), COUNT(DISTINCT tag), GROUP_CONCAT(id)
        std::vector<std::string> keys;
        switch (rng() % 3) {
            case 0: keys = {"grp"}; break;
            case 1: keys = {"tag"}; break;
            default: keys = {"grp", "tag"}; break;
        }
        std::string where_sql;
        std::vector<Leaf> leaves;
        for (std::size_t k = 0, n = rng() % 3; k < n; k++) leaves.push_back(make_leaf());
        for (std::size_t k = 0; k < leaves.size(); k++) where_sql += (k ? " AND " : " WHERE ") + leaves[k].sql;
        int having_min = static_cast<int>(rng() % 4);
        bool has_having = rng() % 2 == 0;
        std::string order_key = keys[rng() % keys.size()];
        bool order_desc = rng() % 2 == 0, has_order = rng() % 2 == 0;
        bool has_limit = rng() % 2 == 0, has_offset = has_limit && rng() % 2 == 0;
        std::size_t limit = rng() % 8, offset = rng() % 5;

        std::string key_sql;
        for (auto& k : keys) key_sql += (key_sql.empty() ? "" : ", ") + k;
        std::string sql = "SELECT " + key_sql + ", COUNT(*), COUNT(val), SUM(val), MIN(val), MAX(val), COUNT(DISTINCT tag), GROUP_CONCAT(id) FROM g" + where_sql + " GROUP BY " + key_sql;
        if (has_having) sql += " HAVING COUNT(*) >= " + std::to_string(having_min);
        if (has_order) sql += " ORDER BY " + order_key + (order_desc ? " DESC" : "");
        if (has_limit) sql += " LIMIT " + std::to_string(limit);
        if (has_offset) sql += " OFFSET " + std::to_string(offset);

        // reference: filter, order the rows (stable) by the ORDER BY column, group in order of first appearance, aggregate,
        // HAVING, order the groups (stable), offset/limit
        std::vector<const RefRow*> rows;
        for (auto& r : table) {
            bool keep = true;
            for (auto& l : leaves) keep = keep && l.pred(r);
            if (keep) rows.push_back(&r);
        }
        auto less_by_order = [&](const RefRow* a, const RefRow* b) {
            int c = ref_cmp(a->at(order_key), b->at(order_key));
            return order_desc ? c > 0 : c < 0;
        };
        if (has_order) std::stable_sort(rows.begin(), rows.end(), less_by_order);
        struct Group {
            std::vector<std::string> key;
            std::vector<const RefRow*> rows;
        };
        std::vector<Group> groups;
        for (auto* r : rows) {
            std::vector<std::string> key;
            for (auto& k : keys) key.push_back(r->at(k));
            auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.key == key; });
            if (it == groups.end()) {
                groups.push_back({key, {}});
                it = groups.end() - 1;
            }
            it->rows.push_back(r);
        }
        std::vector<std::vector<std::string>> expected;
        std::vector<std::vector<std::string>> kept_keys;
        for (auto& g : groups) {
            if (has_having && static_cast<int>(g.rows.size()) < having_min) continue;
            long count_val = 0, sum = 0;
            std::optional<int> mn, mx;
            std::set<std::string> distinct_tags;
            for (auto* r : g.rows) {
                if (r->at("val") != "NULL") {
                    int v = std::stoi(r->at("val"));
                    count_val++;
                    sum += v;
                    mn = mn ? std::min(*mn, v) : v;
                    mx = mx ? std::max(*mx, v) : v;
                }
                if (r->at("tag") != "NULL") distinct_tags.insert(r->at("tag"));
            }
            std::vector<std::string> row = g.key;
            row.push_back(std::to_string(g.rows.size()));
            row.push_back(std::to_string(count_val));
            row.push_back(std::to_string(sum));
            row.push_back(mn ? std::to_string(*mn) : "NULL");
            row.push_back(mx ? std::to_string(*mx) : "NULL");
            row.push_back(std::to_string(distinct_tags.size()));
            std::string ids; // a group keeps its rows in the order the statement saw them (sorted by ORDER BY, else table order)
            for (auto* r : g.rows) ids += (ids.empty() ? "" : ",") + r->at("id");
            row.push_back(ids);
            expected.push_back(std::move(row));
        }
        if (has_order) {
            std::size_t col = static_cast<std::size_t>(std::find(keys.begin(), keys.end(), order_key) - keys.begin());
            std::stable_sort(expected.begin(), expected.end(), [&](const std::vector<std::string>& a, const std::vector<std::string>& b) {
                int c = ref_cmp(a[col], b[col]);
                return order_desc ? c > 0 : c < 0;
            });
        }
        if (has_offset) expected.erase(expected.begin(), expected.begin() + static_cast<std::ptrdiff_t>(std::min(offset, expected.size())));
        if (has_limit && expected.size() > limit) expected.resize(limit);

        INFO(sql);
        auto actual = table_cells(ok_text(ex, sql));
        REQUIRE(actual == expected);
    }
}

TEST_CASE("DISTINCT with ORDER BY / LIMIT / OFFSET matches a reference", "[query_paths]") {
    TempDataDir dir("qp_distinct");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE dd (id INT PRIMARY KEY, a VARCHAR(10), b INT)").is_ok());
    std::mt19937 rng(97531);
    static const char* a_vals[] = {"x", "y", "10", "9", "7", "07", "abc"};
    std::vector<std::array<std::string, 3>> table;
    std::string values;
    for (int i = 1; i <= 120; i++) {
        std::array<std::string, 3> r = {std::to_string(i), a_vals[rng() % 7], std::to_string(rng() % 5)};
        values += (i > 1 ? ", (" : "(") + r[0] + ", '" + r[1] + "', " + r[2] + ")";
        table.push_back(r);
    }
    REQUIRE(ex.execute_sql("INSERT INTO dd VALUES " + values).is_ok());
    for (int iter = 0; iter < 150; iter++) {
        bool both = rng() % 2 == 0;
        std::string cols = both ? "a, b" : "b";
        bool has_order = rng() % 3 != 0, desc = rng() % 2 == 0;
        std::string order_col = rng() % 2 == 0 ? "a" : "b";
        bool has_limit = rng() % 2 == 0, has_offset = has_limit && rng() % 2 == 0;
        std::size_t limit = rng() % 10, offset = rng() % 6;
        std::string sql = "SELECT DISTINCT " + cols + " FROM dd";
        if (has_order) sql += " ORDER BY " + order_col + (desc ? " DESC" : "");
        if (has_limit) sql += " LIMIT " + std::to_string(limit);
        if (has_offset) sql += " OFFSET " + std::to_string(offset);

        // the engine's order of work: sort (stable), cut to the LIMIT/OFFSET window, THEN drop repeated rows
        std::vector<std::array<std::string, 3>> rows = table;
        if (has_order) {
            std::size_t col = order_col == "a" ? 1 : 2;
            std::stable_sort(rows.begin(), rows.end(), [&](const auto& x, const auto& y) {
                int c = ref_cmp(x[col], y[col]);
                return desc ? c > 0 : c < 0;
            });
        }
        if (has_offset) rows.erase(rows.begin(), rows.begin() + static_cast<std::ptrdiff_t>(std::min(offset, rows.size())));
        if (has_limit && rows.size() > limit) rows.resize(limit);
        std::vector<std::vector<std::string>> expected;
        std::set<std::vector<std::string>> seen;
        for (auto& r : rows) {
            std::vector<std::string> cells = both ? std::vector<std::string>{r[1], r[2]} : std::vector<std::string>{r[2]};
            if (seen.insert(cells).second) expected.push_back(cells);
        }
        INFO(sql);
        REQUIRE(table_cells(ok_text(ex, sql)) == expected);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Joins. The reference of every query is the same query with each ON and the WHERE wrapped as `(<cond>) OR 1 = 0`: an OR at the
// top has no equality part to hash on and no conjunct to push down, so it runs the nested loop and filters the joined rows --
// the way every join ran before the shortcuts existed.

namespace {
struct JoinFixture {
    TempDataDir dir;
    Executor ex;
    explicit JoinFixture(const std::string& path, unsigned seed) : dir(path), ex(path) {
        open_db(ex);
        REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, grp INT, val INT, code VARCHAR(10), tag VARCHAR(10))").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE u (id INT PRIMARY KEY, grp INT, name VARCHAR(10))").is_ok());
        REQUIRE(ex.execute_sql("CREATE TABLE w (id INT PRIMARY KEY, code VARCHAR(10), k INT)").is_ok());
        std::mt19937 rng(seed);
        static const char* codes[] = {"'7'", "'007'", "'7.0'", "'07'", "'C1'", "'C2'", "'C3'", "''", "NULL", "'10'", "'1e1'"};
        static const char* tags[] = {"'red'", "'green'", "'name'", "NULL"};
        std::string tv, uv, wv;
        for (int i = 1; i <= 90; i++) {
            tv += std::string(i > 1 ? ", (" : "(") + std::to_string(i) + ", " + (rng() % 10 == 0 ? "NULL" : std::to_string(rng() % 7)) + ", " +
                  (rng() % 8 == 0 ? "NULL" : std::to_string(rng() % 12)) + ", " + codes[rng() % 11] + ", " + tags[rng() % 4] + ")";
        }
        for (int i = 1; i <= 12; i++) uv += std::string(i > 1 ? ", (" : "(") + std::to_string(i) + ", " + std::to_string(rng() % 7) + ", 'N" + std::to_string(i) + "')";
        for (int i = 1; i <= 18; i++) wv += std::string(i > 1 ? ", (" : "(") + std::to_string(i) + ", " + codes[rng() % 11] + ", " + (rng() % 6 == 0 ? "NULL" : std::to_string(rng() % 7)) + ")";
        REQUIRE(ex.execute_sql("INSERT INTO t VALUES " + tv).is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO u VALUES " + uv).is_ok());
        REQUIRE(ex.execute_sql("INSERT INTO w VALUES " + wv).is_ok());
    }
};

// Both queries must succeed with the same set of lines, or fail with the same message.
void require_same_answer(Executor& ex, const std::string& sql, const std::string& reference_sql) {
    INFO(sql);
    INFO(reference_sql);
    auto got = ex.execute_sql(sql);
    auto want = ex.execute_sql(reference_sql);
    REQUIRE(got.is_ok() == want.is_ok());
    if (got.is_ok()) {
        auto a = sorted_lines(got.value()), b = sorted_lines(want.value());
        std::vector<std::string> only_a, only_b;
        std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(only_a));
        std::set_difference(b.begin(), b.end(), a.begin(), a.end(), std::back_inserter(only_b));
        std::string note = a.size() == b.size() ? "" : "lines " + std::to_string(a.size()) + " vs " + std::to_string(b.size()) + "; ";
        if (!only_a.empty()) note += "only in the query: " + only_a.front() + "; ";
        if (!only_b.empty()) note += "only in the reference: " + only_b.front();
        INFO(note);
        REQUIRE(a == b);
    } else {
        REQUIRE(got.error() == want.error());
    }
}
} // namespace

TEST_CASE("joins whose ON holds an equality (and WHERE conjuncts of one table) answer like the nested loop", "[query_paths][join]") {
    // RUSQL_FUZZ_SEEDS / RUSQL_FUZZ_START: more iterations / another stream of queries (the same knobs as the index fuzz tests)
    unsigned iterations = 400, start = 0;
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) iterations = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    JoinFixture fx("qp_join", 4242 + start);
    std::mt19937 rng(8086 + start);
    // every ON reads t or a table joined before it
    const std::vector<std::string> on_u = {"t.grp = u.id", "u.id = t.grp", "t.grp = u.grp AND u.id < 5", "t.val = u.id", "t.grp = u.id OR u.id = 3",
                                           "t.grp = u.id AND u.grp = t.grp", "u.grp = t.grp AND t.id = u.id", "t.grp = u.id AND u.name LIKE 'N1%'",
                                           "u.grp = t.grp AND (u.id < 4 OR t.val < 6)", "t.grp > u.id AND t.val = u.grp"};
    const std::vector<std::string> on_w = {"t.code = w.code", "w.code = t.code", "t.val = w.k", "w.k = t.grp", "t.code = w.code AND w.k = t.grp",
                                           "w.k = t.grp AND t.code = w.code", "t.code = w.code AND t.val > w.k", "t.val = w.k AND t.grp = w.k"};
    const std::vector<std::string> on_w_after_u = {"w.k = u.id", "w.k = u.grp AND t.val = w.k"};
    auto leaf = [&](bool has_u, bool has_w) {
        std::vector<std::string> pool = {"t.val > " + std::to_string(rng() % 12), "t.grp = " + std::to_string(rng() % 7), "t.tag = 'red'", "t.tag IS NULL",
                                          "val < " + std::to_string(rng() % 12), "id BETWEEN " + std::to_string(rng() % 60) + " AND 80", "grp IN (1, 2, 3)",
                                          "t.code LIKE 'C%'", "t.tag = 'name'", "t.code = 'grp'", "t.id % " + std::to_string(2 + rng() % 4) + " = 0"};
        if (has_u) {
            for (auto& s : std::vector<std::string>{"u.name LIKE 'N1%'", "u.id < " + std::to_string(1 + rng() % 12), "u.grp = " + std::to_string(rng() % 7),
                                                     "name = 'N3'", "u.name IS NULL", "t.grp = u.grp", "t.tag = 'name'"}) {
                pool.push_back(s);
            }
        }
        if (has_w) {
            for (auto& s : std::vector<std::string>{"w.code = '7'", "w.k > " + std::to_string(rng() % 7), "w.code LIKE 'C1%'", "k < 5", "t.val = w.k",
                                                     "t.grp = 'k'", "t.code = 'k'", "t.val = 'k'"}) {
                pool.push_back(s);
            }
        }
        return pool[rng() % pool.size()];
    };

    for (unsigned iter = 0; iter < iterations; iter++) {
        bool with_u = rng() % 3 != 0, with_w = !with_u || rng() % 2 == 0;
        std::string from = "FROM t", ref_from = "FROM t";
        auto add_join = [&](const std::string& table, const std::vector<std::string>& ons) {
            std::string on = ons[rng() % ons.size()];
            std::string jt = rng() % 2 == 0 ? "LEFT JOIN" : "JOIN";
            from += " " + jt + " " + table + " ON " + on;
            ref_from += " " + jt + " " + table + " ON (" + on + ") OR 1 = 0";
        };
        if (with_u) add_join("u", on_u);
        if (with_w) {
            std::vector<std::string> ons = on_w;
            if (with_u) ons.insert(ons.end(), on_w_after_u.begin(), on_w_after_u.end());
            add_join("w", ons);
        }
        std::string where, ref_where;
        if (rng() % 6 != 0) {
            std::string w = leaf(with_u, with_w);
            for (std::size_t k = 0, n = rng() % 3; k < n; k++) w += (rng() % 3 == 0 ? " OR " : " AND ") + leaf(with_u, with_w);
            if (rng() % 8 == 0) w = "NOT (" + w + ")";
            where = " WHERE " + w;
            ref_where = " WHERE (" + w + ") OR 1 = 0";
        }
        // (explicit columns: for `SELECT *` the planner's join order decides the column order, and it does not reorder a
        // join whose ON is an OR)
        std::string select = with_u && with_w ? "t.id, t.val, u.name, w.code" : (with_u ? "t.id, t.val, u.name" : "t.id, t.val, w.code");
        if (rng() % 5 == 0) select = "COUNT(*), MAX(t.val)";
        require_same_answer(fx.ex, "SELECT " + select + " " + from + where, "SELECT " + select + " " + ref_from + ref_where);
    }
}

TEST_CASE("LEFT JOIN on NULL, empty and numeric look-alike keys", "[query_paths][join]") {
    TempDataDir dir("qp_left_keys");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE a (id INT PRIMARY KEY, k VARCHAR(10))").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE b (id INT PRIMARY KEY, k VARCHAR(10), tag VARCHAR(10))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO a VALUES (1,'7'), (2,'7.0'), (3,NULL), (4,''), (5,'x'), (6,'07'), (7,'8')").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO b VALUES (1,'007','p'), (2,NULL,'q'), (3,'','r'), (4,'x','s'), (5,'7.00','t'), (6,'9','u')").is_ok());
    // 7, 7.0, 07 meet 007 and 7.00 (numbers); NULL meets nothing, not even a NULL (and the engine stores '' as NULL);
    // 'x' meets 'x'; 8 meets nothing
    auto rows = table_cells(ok_text(ex, "SELECT a.id, b.tag FROM a LEFT JOIN b ON a.k = b.k"));
    std::multiset<std::string> got;
    for (auto& r : rows) got.insert(r[0] + ":" + r[1]);
    REQUIRE(got == std::multiset<std::string>{"1:p", "1:t", "2:p", "2:t", "3:NULL", "4:NULL", "5:s", "6:p", "6:t", "7:NULL"});
    // left rows keep their order, a left row's matches keep the right table's order
    REQUIRE(rows.at(0)[0] == "1");
    REQUIRE(rows.at(0)[1] == "p");
    REQUIRE(rows.at(1)[1] == "t");
    // EXPLAIN says what runs: a LEFT JOIN is a hash join, never an index nested loop
    REQUIRE(ok_text(ex, "EXPLAIN SELECT a.id, b.tag FROM a LEFT JOIN b ON a.k = b.k").find("Index NL") == std::string::npos);
    // an inner join with the same condition drops the unmatched rows
    REQUIRE(table_cells(ok_text(ex, "SELECT a.id FROM a JOIN b ON a.k = b.k AND b.tag <> 'zzz'")).size() == 7);
}

TEST_CASE("IndexNL probes cached per key give the rows an uncached join would", "[query_paths][join]") {
    TempDataDir dir("qp_indexnl");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE small (id INT PRIMARY KEY, grp INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE big (id INT PRIMARY KEY, name VARCHAR(10))").is_ok());
    std::string sv, bv;
    for (int i = 1; i <= 150; i++) sv += std::string(i > 1 ? ", (" : "(") + std::to_string(i) + ", " + std::to_string(i % 10) + ")";
    for (int i = 0; i < 6000; i++) {
        bv += std::string(i % 500 ? ", (" : "(") + std::to_string(i) + ", 'n" + std::to_string(i) + "')";
        if (i % 500 == 499) {
            REQUIRE(ex.execute_sql("INSERT INTO big VALUES " + bv).is_ok());
            bv.clear();
        }
    }
    REQUIRE(ex.execute_sql("INSERT INTO small VALUES " + sv).is_ok());
    std::string plan = ok_text(ex, "EXPLAIN SELECT small.id, big.name FROM small JOIN big ON small.grp = big.id");
    INFO(plan);
    REQUIRE(plan.find("Index NL Join") != std::string::npos); // the probe this test is about
    require_same_answer(ex, "SELECT small.id, big.name FROM small JOIN big ON small.grp = big.id",
                        "SELECT small.id, big.name FROM small JOIN big ON (small.grp = big.id) OR 1 = 0");
    auto rows = table_cells(ok_text(ex, "SELECT small.id, big.name FROM small JOIN big ON small.grp = big.id"));
    REQUIRE(rows.size() == 150);
    for (auto& r : rows) REQUIRE(r[1] == "n" + std::to_string(std::stoi(r[0]) % 10));
}

// ---------------------------------------------------------------------------------------------------------------------
// `WHERE <indexed equality> AND <anything else>`: the planner starts from the point index and every row it finds is checked
// against the whole condition. The twin is the same condition with `OR id < 0` appended (never true; a top-level OR is
// always a scan).

TEST_CASE("an AND with one indexed equality uses the index and answers like a scan", "[query_paths][planner]") {
    TempDataDir dir("qp_and_point");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE g (id INT PRIMARY KEY, grp INT, tag VARCHAR(10), val INT, note VARCHAR(10))").is_ok());
    std::mt19937 rng(1357);
    static const char* grp_text[] = {"0", "1", "2", "3", "3.0", "03", "4", "5", "NULL"};
    static const char* tags[] = {"'red'", "'green'", "'blue'", "NULL"};
    static const char* notes[] = {"'abc'", "'axe'", "'bob'", "NULL"};
    std::string values;
    for (int i = 1; i <= 400; i++) {
        values += std::string(i > 1 ? ", (" : "(") + std::to_string(i) + ", " + grp_text[rng() % 9] + ", " + tags[rng() % 4] + ", " +
                  (rng() % 7 == 0 ? "NULL" : std::to_string(rng() % 100)) + ", " + notes[rng() % 4] + ")";
    }
    REQUIRE(ex.execute_sql("INSERT INTO g VALUES " + values).is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX g_grp ON g (grp)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX g_tag ON g (tag) USING HASH").is_ok());

    auto plan_uses_index = [&](const std::string& sql) {
        auto plan = ok_text(ex, "EXPLAIN " + sql);
        auto at = plan.find("Access: ");
        return at != std::string::npos && plan.compare(at + 8, 8, "Seq Scan") != 0;
    };
    REQUIRE(plan_uses_index("SELECT id FROM g WHERE id = 5 AND val > 3"));
    REQUIRE(plan_uses_index("SELECT id FROM g WHERE val > 3 AND grp = 2"));
    REQUIRE(plan_uses_index("SELECT id FROM g WHERE val > 3 AND tag = 'red'"));
    REQUIRE_FALSE(plan_uses_index("SELECT id FROM g WHERE val > 3 AND note = 'abc'")); // nothing indexed
    REQUIRE_FALSE(plan_uses_index("SELECT id FROM g WHERE id > 5 AND val > 3"));       // a range stays a scan

    std::vector<std::string> keyed = {"id = " + std::to_string(1 + rng() % 400), "id = 7.0", "id = 07", "id = 9999", "grp = 3", "grp = 3.0", "grp = 03",
                                      "grp = 0", "grp = 5", "tag = 'red'", "tag = 'blue'", "tag = 'none'"};
    std::vector<std::string> rest = {"val > 50", "val < 10", "val IS NULL", "note LIKE 'a%'", "grp <> 3", "id < 100", "(val < 5 OR val > 90)", "tag IS NOT NULL",
                                     "note = 'bob'", "NOT (val > 20)"};
    for (int iter = 0; iter < 300; iter++) {
        std::string cond = keyed[rng() % keyed.size()];
        for (std::size_t k = 0, n = 1 + rng() % 2; k < n; k++) cond = (rng() % 2 ? cond + " AND " + rest[rng() % rest.size()] : rest[rng() % rest.size()] + " AND " + cond);
        // selecting only the indexed column must not be answered from the index entry: the other predicates need more
        std::string select = std::vector<std::string>{"id", "*", "grp", "tag", "COUNT(*)", "id, val"}[rng() % 6];
        std::string sql = "SELECT " + select + " FROM g WHERE " + cond, twin = "SELECT " + select + " FROM g WHERE (" + cond + ") OR id < 0";
        INFO(sql);
        REQUIRE(sorted_lines(ok_text(ex, sql)) == sorted_lines(ok_text(ex, twin)));
    }
    // the same inside an open transaction by another session's absence/presence is covered by test_select_index; here the
    // statement-level result after DML keeps agreeing
    REQUIRE(ex.execute_sql("UPDATE g SET grp = 3 WHERE id % 11 = 0").is_ok());
    REQUIRE(ex.execute_sql("DELETE FROM g WHERE id % 13 = 0").is_ok());
    for (const char* cond : {"grp = 3 AND val > 10", "id = 22 AND grp = 3", "id = 26 AND val > 0", "tag = 'red' AND grp = 3.0"}) {
        std::string sql = std::string("SELECT id FROM g WHERE ") + cond, twin = std::string("SELECT id FROM g WHERE (") + cond + ") OR id < 0";
        INFO(sql);
        REQUIRE(sorted_lines(ok_text(ex, sql)) == sorted_lines(ok_text(ex, twin)));
    }
}
