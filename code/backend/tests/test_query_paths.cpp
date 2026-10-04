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
        bool only_outer = true; // no INNER join among them: the planner does not reorder or re-algorithm them, the row order is the loop's
        auto add_join = [&](const std::string& table, const std::vector<std::string>& ons) {
            std::string on = ons[rng() % ons.size()];
            static const char* kinds[] = {"LEFT JOIN", "JOIN", "LEFT JOIN", "RIGHT JOIN", "FULL OUTER JOIN", "JOIN"};
            std::string jt = kinds[rng() % 6];
            if (jt == "JOIN") only_outer = false;
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
        if (only_outer && select.rfind("COUNT", 0) != 0) { // the same rows in the same ORDER
            std::string a = ok_text(fx.ex, "SELECT " + select + " " + from + where), b = ok_text(fx.ex, "SELECT " + select + " " + ref_from + ref_where);
            INFO("SELECT " << select << " " << from << where);
            REQUIRE(a == b);
        }
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

// ---------------------------------------------------------------------------------------------------------------------
// A pure-read statement that asks one table for `<column> = <constant>` over and over (a correlated subquery, once per outer
// row) builds a hash index on that column for itself. The twin hides the equality from it with `(<cond>) OR 1 = 0`.

TEST_CASE("correlated subqueries over an unindexed column answer like a scan", "[query_paths][point_index]") {
    TempDataDir dir("qp_point_index");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE cust (id INT PRIMARY KEY, name VARCHAR(10), tier INT, code VARCHAR(10))").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE ord (id INT PRIMARY KEY, cust_id INT, code VARCHAR(10), amount INT, status VARCHAR(10))").is_ok());
    std::mt19937 rng(9753);
    static const char* codes[] = {"'7'", "'007'", "'7.0'", "'07'", "'C1'", "'C2'", "NULL", "'10'", "'1e1'"};
    static const char* status[] = {"'open'", "'done'", "'void'", "NULL"};
    std::string cv;
    for (int i = 1; i <= 300; i++) {
        cv += std::string(i > 1 ? ", (" : "(") + std::to_string(i) + ", 'C" + std::to_string(i) + "', " + std::to_string(rng() % 4) + ", " + codes[rng() % 9] + ")";
    }
    REQUIRE(ex.execute_sql("INSERT INTO cust VALUES " + cv).is_ok());
    for (int start = 1; start <= 800; start += 200) {
        std::string ov;
        for (int i = start; i < start + 200; i++) {
            ov += std::string(i > start ? ", (" : "(") + std::to_string(i) + ", " + (rng() % 12 == 0 ? "NULL" : std::to_string(1 + rng() % 400)) + ", " + codes[rng() % 9] + ", " +
                  std::to_string(rng() % 50) + ", " + status[rng() % 4] + ")";
        }
        REQUIRE(ex.execute_sql("INSERT INTO ord VALUES " + ov).is_ok());
    }
    // by key (cust_id), by a numeric-lookalike code, by a residual status
    std::vector<std::string> inner = {"ord.cust_id = cust.id", "ord.code = cust.code", "ord.cust_id = cust.id AND ord.status = 'done'",
                                      "ord.cust_id = cust.id AND ord.amount > 25", "ord.status = 'open' AND ord.cust_id = cust.id", "ord.code = cust.code AND ord.cust_id = cust.id"};
    for (int iter = 0; iter < 120; iter++) {
        std::string cond = inner[rng() % inner.size()];
        std::string twin_cond = "(" + cond + ") OR 1 = 0";
        int shape = static_cast<int>(rng() % 6);
        auto make = [&](const std::string& c) {
            switch (shape) {
                case 0: return "SELECT COUNT(*) FROM cust WHERE EXISTS (SELECT 1 FROM ord WHERE " + c + ")";
                case 1: return "SELECT id FROM cust WHERE NOT EXISTS (SELECT 1 FROM ord WHERE " + c + ")";
                case 2: return "SELECT id FROM cust WHERE tier IN (SELECT amount FROM ord WHERE " + c + ")";
                case 3: return "SELECT id FROM cust WHERE 30 < (SELECT MAX(amount) FROM ord WHERE " + c + ")";
                case 4: return "SELECT cust.id, (SELECT COUNT(*) FROM ord WHERE " + c + ") AS n FROM cust WHERE cust.id < 120";
                default: return "SELECT id FROM cust WHERE tier = (SELECT MIN(amount) FROM ord WHERE " + c + ")";
            }
        };
        require_same_answer(ex, make(cond), make(twin_cond));
    }
    // a plain statement that repeats the lookup: the same rows in the same order as the scan gives
    for (int id : {1, 7, 42, 77, 300}) {
        std::string q = "SELECT id FROM ord WHERE cust_id = " + std::to_string(id);
        REQUIRE(ok_text(ex, q) == ok_text(ex, "SELECT id FROM ord WHERE (cust_id = " + std::to_string(id) + ") OR 1 = 0"));
    }
}

TEST_CASE("statement-scoped point indexes never outlive their statement", "[query_paths][point_index]") {
    TempDataDir dir("qp_point_index_scope");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE a (id INT PRIMARY KEY, k INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE b (id INT PRIMARY KEY, k INT)").is_ok());
    std::string av, bv;
    for (int i = 1; i <= 600; i++) {
        av += std::string(i > 1 ? ", (" : "(") + std::to_string(i) + ", " + std::to_string(i % 50) + ")";
        bv += std::string(i > 1 ? ", (" : "(") + std::to_string(i) + ", " + std::to_string(i % 50) + ")";
    }
    REQUIRE(ex.execute_sql("INSERT INTO a VALUES " + av).is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO b VALUES " + bv).is_ok());
    const std::string probe = "SELECT COUNT(*) FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.k = a.k AND b.id > 590)"; // asks b.k = <value> 600 times
    auto before = ok_text(ex, probe);
    // rows change between statements: the next statement must see them
    REQUIRE(ex.execute_sql("DELETE FROM b WHERE id > 590").is_ok());
    auto after_delete = ok_text(ex, probe);
    REQUIRE(after_delete != before);
    REQUIRE(after_delete.find("| 0 ") != std::string::npos);
    REQUIRE(ex.execute_sql("INSERT INTO b VALUES (9001, 7)").is_ok());
    REQUIRE(ok_text(ex, "SELECT COUNT(*) FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.k = a.k AND b.id > 9000)").find("| 12 ") != std::string::npos);
    // a write that consults the table while it changes: the subquery is not a pure read, so it scans
    REQUIRE(ex.execute_sql("UPDATE b SET k = k + 100 WHERE EXISTS (SELECT 1 FROM a WHERE a.k = b.k AND a.id < 100)").is_ok());
    REQUIRE(ok_text(ex, "SELECT COUNT(*) FROM b WHERE k >= 100").find("| 5") != std::string::npos);
    // inside an open transaction, with the transaction's own writes
    REQUIRE(ex.execute_sql("BEGIN").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO b VALUES (9002, 7)").is_ok());
    REQUIRE(ok_text(ex, "SELECT COUNT(*) FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.k = a.k AND b.id > 9001)").find("| 12 ") != std::string::npos);
    REQUIRE(ex.execute_sql("ROLLBACK").is_ok());
    REQUIRE(ok_text(ex, "SELECT COUNT(*) FROM a WHERE EXISTS (SELECT 1 FROM b WHERE b.k = a.k AND b.id > 9001)").find("| 0 ") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------------------------------
// UPDATE builds the old and new row image of every row it touches only when a secondary, hash or composite index has to be
// rewritten; the PK B+Tree takes the new version's JSON text. After every statement, whatever indexes a table has must answer
// like a scan -- for plain changes, for changes of the indexed columns and for changes of the primary key itself.

TEST_CASE("UPDATE keeps every index equal to the table (with and without secondary indexes, primary key changes)", "[query_paths][update]") {
    TempDataDir dir("qp_update_indexes");
    {
        Executor ex(dir.path);
        open_db(ex);
        REQUIRE(ex.execute_sql("CREATE TABLE p0 (id INT PRIMARY KEY, a INT, b VARCHAR(10), c INT)").is_ok());            // primary key only
        REQUIRE(ex.execute_sql("CREATE TABLE p1 (id INT PRIMARY KEY, a INT, b VARCHAR(10), c INT)").is_ok());            // + B+Tree index
        REQUIRE(ex.execute_sql("CREATE TABLE p2 (id INT PRIMARY KEY, a INT, b VARCHAR(10), c INT)").is_ok());            // + hash index
        REQUIRE(ex.execute_sql("CREATE TABLE p3 (id INT PRIMARY KEY, a INT, b VARCHAR(10), c INT)").is_ok());            // + composite index
        REQUIRE(ex.execute_sql("CREATE INDEX p1_a ON p1 (a)").is_ok());
        REQUIRE(ex.execute_sql("CREATE INDEX p2_b ON p2 (b) USING HASH").is_ok());
        REQUIRE(ex.execute_sql("CREATE INDEX p3_ac ON p3 (a, c)").is_ok());
        std::mt19937 rng(246);
        std::string values;
        for (int i = 1; i <= 200; i++) {
            values += std::string(i > 1 ? ", (" : "(") + std::to_string(i) + ", " + std::to_string(rng() % 10) + ", 'k" + std::to_string(rng() % 8) + "', " + std::to_string(rng() % 6) + ")";
        }
        for (const char* t : {"p0", "p1", "p2", "p3"}) REQUIRE(ex.execute_sql(std::string("INSERT INTO ") + t + " VALUES " + values).is_ok());

        auto check_all = [&](const std::string& t) {
            const std::vector<std::string> preds = {"id = 7", "id = " + std::to_string(1 + rng() % 400), "a = 3", "a = 3.0", "b = 'k2'", "a = 4 AND c = 2", "id = 1007",
                                                    "id = 1" + std::to_string(rng() % 50)};
            for (const std::string& pred : preds) {
                std::string sql = "SELECT id, a, b, c FROM " + t + " WHERE " + pred, twin = "SELECT id, a, b, c FROM " + t + " WHERE (" + pred + ") OR id < 0";
                INFO(sql);
                REQUIRE(sorted_lines(ok_text(ex, sql)) == sorted_lines(ok_text(ex, twin)));
            }
        };
        for (int iter = 0; iter < 60; iter++) {
            for (const std::string t : std::vector<std::string>{"p0", "p1", "p2", "p3"}) {
                std::string sql;
                switch (rng() % 5) {
                    case 0: sql = "UPDATE " + t + " SET c = c + 1 WHERE a = " + std::to_string(rng() % 10); break;                       // no indexed column
                    case 1: sql = "UPDATE " + t + " SET a = a + 1 WHERE id % 7 = " + std::to_string(rng() % 7); break;                 // the B+Tree / composite column
                    case 2: sql = "UPDATE " + t + " SET b = 'k" + std::to_string(rng() % 8) + "' WHERE c = " + std::to_string(rng() % 6); break; // the hash column
                    case 3: sql = "UPDATE " + t + " SET id = id + 1000 WHERE id = " + std::to_string(1 + rng() % 200); break;          // the primary key (may find no row)
                    default: sql = "UPDATE " + t + " SET id = id + 1 WHERE id > 1000 AND id < " + std::to_string(1000 + rng() % 60); break; // a chain of key changes
                }
                auto r = ex.execute_sql(sql);
                (void)r; // a duplicate key is a legitimate refusal; the indexes must be right either way
                if (iter % 6 == 0) check_all(t);
            }
        }
        for (const char* t : {"p0", "p1", "p2", "p3"}) check_all(t);
    }
    // and after a restart: the indexes are rebuilt from the rows, the redo log replayed
    Executor again(dir.path);
    REQUIRE(again.execute_sql("USE d").is_ok());
    for (const char* t : {"p0", "p1", "p2", "p3"}) {
        for (const std::string pred : std::vector<std::string>{"id = 1007", "id = 7", "a = 3", "b = 'k2'"}) {
            std::string sql = std::string("SELECT id, a, b, c FROM ") + t + " WHERE " + pred, twin = std::string("SELECT id, a, b, c FROM ") + t + " WHERE (" + pred + ") OR id < 0";
            INFO(sql);
            REQUIRE(sorted_lines(ok_text(again, sql)) == sorted_lines(ok_text(again, twin)));
        }
    }
}

// A view is run by materializing its result as a temporary table under the view's name and erasing it afterwards. A
// statement-scoped point index built on that name would be left pointing at erased rows (found by reading the code, not by
// a failure): a correlated subquery over a view re-materializes it for every outer row.
TEST_CASE("statement-scoped point indexes are never built on a view's temporary table", "[query_paths][point_index]") {
    TempDataDir dir("qp_point_index_view");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE cust (id INT PRIMARY KEY, tier INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE ord (id INT PRIMARY KEY, cust_id INT, amount INT)").is_ok());
    std::string cv, ov;
    for (int i = 1; i <= 40; i++) cv += std::string(i > 1 ? ", (" : "(") + std::to_string(i) + ", " + std::to_string(i % 3) + ")";
    REQUIRE(ex.execute_sql("INSERT INTO cust VALUES " + cv).is_ok());
    for (int start = 1; start <= 700; start += 175) {
        ov.clear();
        for (int i = start; i < start + 175; i++) ov += std::string(i > start ? ", (" : "(") + std::to_string(i) + ", " + std::to_string(1 + (i * 7) % 60) + ", " + std::to_string(i % 40) + ")";
        REQUIRE(ex.execute_sql("INSERT INTO ord VALUES " + ov).is_ok());
    }
    REQUIRE(ex.execute_sql("CREATE VIEW ordv AS SELECT id, cust_id, amount FROM ord").is_ok());
    struct Shape {
        const char* before; // text up to the inner WHERE condition
        const char* cond;
        const char* after;
    };
    for (const Shape& sh : {Shape{"SELECT COUNT(*) FROM cust WHERE EXISTS (SELECT 1 FROM ordv WHERE ", "ordv.cust_id = cust.id", ")"},
                            Shape{"SELECT id FROM cust WHERE NOT EXISTS (SELECT 1 FROM ordv WHERE ", "ordv.cust_id = cust.id AND ordv.amount > 30", ")"},
                            Shape{"SELECT id FROM cust WHERE tier IN (SELECT amount FROM ordv WHERE ", "ordv.cust_id = cust.id", ")"}}) {
        require_same_answer(ex, std::string(sh.before) + sh.cond + sh.after, std::string(sh.before) + "(" + sh.cond + ") OR 1 = 0" + sh.after);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// A column written with its table -- `t.id`, or `x.id` through an alias -- in ORDER BY and DISTINCT. A row keeps its columns
// under their bare names, and both read the column as the row's own key, so `ORDER BY t.id` read nothing (every row compared
// equal, nothing was sorted) and `SELECT DISTINCT t.col` gave every row the same empty key (a single row survived). Both
// now resolve the name the way WHERE and the select list do. When two joined tables share a column name the qualifier is
// what tells them apart, so the reference below is computed here, from the data, never from the engine's other spelling.

namespace {
struct QT {
    int id, grp, val;
    std::string tag;
};
struct QU {
    int id;
    std::string name;
    int grp;
};

// The value of one `t.*` / `u.*` column of a joined row; u is null for a LEFT JOIN row without a partner.
std::string q_value(const QT& t, const QU* u, const std::string& col) {
    if (col == "t.id") return std::to_string(t.id);
    if (col == "t.grp") return std::to_string(t.grp);
    if (col == "t.val") return std::to_string(t.val);
    if (col == "t.tag") return t.tag;
    if (!u) return "NULL";
    if (col == "u.id") return std::to_string(u->id);
    if (col == "u.name") return u->name;
    return std::to_string(u->grp);
}
} // namespace

TEST_CASE("ORDER BY and DISTINCT on table.column match a reference when both tables have the same column names",
          "[query_paths][qualified]") {
    unsigned seed_count = 2; // RUSQL_FUZZ_SEEDS=30 runs a much longer campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    unsigned seed_start = 0;
    if (const char* e = std::getenv("RUSQL_FUZZ_START")) seed_start = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    struct Config {
        int t_rows, u_rows;
        bool index;     // a secondary index on t.grp: lets the planner pick the index-driven join algorithms
        int grp_values; // distinct values of t.grp (0: u_rows + 2, so a few rows have no partner)
    };
    std::set<std::string> algorithms; // the join algorithms the queries ran through
    std::size_t checked = 0;
    // sizes chosen for the planner to pick Index NL (small left), Hash, Nested Loop (one right row) and Reverse Index NL (a few
    // right rows probing an index on a big left table whose keys are nearly unique)
    for (const Config& cfg : {Config{90, 5, false, 0}, Config{90, 5, true, 0}, Config{400, 40, false, 0}, Config{400, 40, true, 0},
                              Config{10, 1, false, 0}, Config{400, 10, true, 200}}) {
        for (unsigned k = seed_start; k < seed_start + seed_count; k++) {
            INFO("rows " << cfg.t_rows << "/" << cfg.u_rows << " index " << cfg.index << " seed " << k);
            std::mt19937 rng(777 + k * 13 + static_cast<unsigned>(cfg.t_rows) + (cfg.index ? 1u : 0u));
            TempDataDir dir("qp_qualified_names");
            Executor ex(dir.path);
            open_db(ex);
            REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, grp INT, val INT, tag VARCHAR(8))").is_ok());
            REQUIRE(ex.execute_sql("CREATE TABLE u (id INT PRIMARY KEY, name VARCHAR(8), grp INT)").is_ok());
            static const char* tags[] = {"a", "b", "c", "d"};
            static const char* names[] = {"nx", "ny", "nz", "nw", "nv"};
            std::vector<QT> T;
            std::vector<QU> U;
            for (int i = 1; i <= cfg.u_rows; i++) U.push_back({i, names[rng() % 5], static_cast<int>(rng() % 4)});
            for (int i = 1; i <= cfg.t_rows; i++) {
                // grp reaches values with no partner in u (0 and above u_rows)
                const unsigned grp_values = static_cast<unsigned>(cfg.grp_values ? cfg.grp_values : cfg.u_rows + 2);
                T.push_back({i, static_cast<int>(rng() % grp_values), static_cast<int>(rng() % 25), tags[rng() % 4]});
            }
            std::string tv, uv;
            for (auto& t : T) tv += std::string(tv.empty() ? "(" : ", (") + std::to_string(t.id) + ", " + std::to_string(t.grp) + ", " + std::to_string(t.val) + ", '" + t.tag + "')";
            for (auto& u : U) uv += std::string(uv.empty() ? "(" : ", (") + std::to_string(u.id) + ", '" + u.name + "', " + std::to_string(u.grp) + ")";
            REQUIRE(ex.execute_sql("INSERT INTO t VALUES " + tv).is_ok());
            REQUIRE(ex.execute_sql("INSERT INTO u VALUES " + uv).is_ok());
            if (cfg.index) REQUIRE(ex.execute_sql("CREATE INDEX t_grp ON t (grp)").is_ok());

            const std::vector<std::string> all_cols = {"t.id", "t.grp", "t.val", "t.tag", "u.id", "u.name", "u.grp"};
            struct JR {
                const QT* t;
                const QU* u;
            };
            struct Key {
                std::string col;
                bool asc;
            };
            for (int iter = 0; iter < 60; iter++) {
                const bool left = rng() % 3 == 0, alias = rng() % 2 == 0;
                const std::string tq = alias ? "x" : "t", uq = alias ? "y" : "u";
                auto spell = [&](const std::string& col) { return (col[0] == 't' ? tq : uq) + col.substr(1); };
                const std::string from = std::string("FROM t") + (alias ? " x" : "") + (left ? " LEFT JOIN u" : " JOIN u") + (alias ? " y" : "") + " ON " + uq + ".id = " + tq + ".grp";
                std::vector<JR> joined;
                for (auto& t : T) {
                    const QU* match = nullptr;
                    for (auto& u : U) {
                        if (u.id == t.grp) match = &u;
                    }
                    if (match || left) joined.push_back({&t, match});
                }
                // a LEFT JOIN row without a partner has NULL in every u column: those never take part in an order (how the engine
                // places a NULL is not what is being tested here)
                std::vector<std::string> order_pool;
                for (auto& c : all_cols) {
                    if (!left || c[0] == 't') order_pool.push_back(c);
                }
                auto pick = [&](const std::vector<std::string>& pool, std::size_t n) {
                    std::vector<std::string> p = pool;
                    std::shuffle(p.begin(), p.end(), rng);
                    p.resize(std::min(n, p.size()));
                    return p;
                };
                auto sort_by = [&](std::vector<JR>& rows, const std::vector<Key>& keys) {
                    std::stable_sort(rows.begin(), rows.end(), [&](const JR& a, const JR& b) {
                        for (auto& key : keys) {
                            int c = ref_cmp(q_value(*a.t, a.u, key.col), q_value(*b.t, b.u, key.col));
                            if (c != 0) return key.asc ? c < 0 : c > 0;
                        }
                        return false;
                    });
                };
                auto join_names = [&](const std::vector<std::string>& cols) {
                    std::string s;
                    for (auto& c : cols) s += (s.empty() ? "" : ", ") + spell(c);
                    return s;
                };
                auto order_text = [&](const std::vector<Key>& keys) {
                    std::string s;
                    for (auto& key : keys) s += (s.empty() ? "" : ", ") + spell(key.col) + (key.asc ? "" : " DESC");
                    return s;
                };

                if (iter % 3 != 2) {
                    // ORDER BY (with a unique last key, so the answer does not depend on the join algorithm) [LIMIT n OFFSET m]
                    std::vector<std::string> select = pick(all_cols, 1 + rng() % 4);
                    std::vector<Key> keys;
                    for (auto& c : pick(order_pool, rng() % 3)) keys.push_back({c, rng() % 2 == 0});
                    keys.push_back({"t.id", rng() % 2 == 0});
                    std::string sql = "SELECT " + join_names(select) + " " + from + " ORDER BY " + order_text(keys);
                    std::size_t offset = 0, limit = joined.size();
                    if (rng() % 3 == 0) {
                        limit = 1 + rng() % 15;
                        offset = rng() % 4 == 0 ? rng() % 10 : 0;
                        sql += " LIMIT " + std::to_string(limit) + (offset ? " OFFSET " + std::to_string(offset) : "");
                    }
                    sort_by(joined, keys);
                    std::vector<std::vector<std::string>> expected;
                    for (std::size_t i = offset; i < joined.size() && expected.size() < limit; i++) {
                        std::vector<std::string> row;
                        for (auto& c : select) row.push_back(q_value(*joined[i].t, joined[i].u, c));
                        expected.push_back(std::move(row));
                    }
                    INFO(sql);
                    REQUIRE(table_cells(ok_text(ex, sql)) == expected);
                    auto plan = ok_text(ex, "EXPLAIN " + sql);
                    for (const char* name : {"Nested Loop", "Hash Join", "Reverse Index NL", "Index NL", "Sort Merge"}) {
                        if (plan.find(std::string("Join: ") + name) != std::string::npos) algorithms.insert(name);
                    }
                } else if (iter % 2 == 0) {
                    // DISTINCT, ordered by every column it selects (a total order on the distinct rows)
                    std::vector<std::string> select = pick(order_pool, 1 + rng() % 3);
                    std::vector<Key> keys;
                    for (auto& c : select) keys.push_back({c, rng() % 2 == 0});
                    std::string sql = "SELECT DISTINCT " + join_names(select) + " " + from + " ORDER BY " + order_text(keys);
                    std::vector<std::vector<std::string>> distinct_rows;
                    {
                        std::set<std::vector<std::string>> seen;
                        for (auto& r : joined) {
                            std::vector<std::string> row;
                            for (auto& c : select) row.push_back(q_value(*r.t, r.u, c));
                            if (seen.insert(row).second) distinct_rows.push_back(std::move(row));
                        }
                    }
                    std::stable_sort(distinct_rows.begin(), distinct_rows.end(), [&](const std::vector<std::string>& a, const std::vector<std::string>& b) {
                        for (std::size_t i = 0; i < keys.size(); i++) {
                            int c = ref_cmp(a[i], b[i]);
                            if (c != 0) return keys[i].asc ? c < 0 : c > 0;
                        }
                        return false;
                    });
                    INFO(sql);
                    REQUIRE(table_cells(ok_text(ex, sql)) == distinct_rows);
                } else {
                    // DISTINCT over any columns, a LEFT JOIN's NULLs included: the same set of rows
                    std::vector<std::string> select = pick(all_cols, 1 + rng() % 3);
                    std::string sql = "SELECT DISTINCT " + join_names(select) + " " + from;
                    std::set<std::vector<std::string>> expected;
                    for (auto& r : joined) {
                        std::vector<std::string> row;
                        for (auto& c : select) row.push_back(q_value(*r.t, r.u, c));
                        expected.insert(std::move(row));
                    }
                    auto got = table_cells(ok_text(ex, sql));
                    INFO(sql);
                    REQUIRE(got.size() == expected.size());
                    REQUIRE(std::set<std::vector<std::string>>(got.begin(), got.end()) == expected);
                }
                checked++;
            }

            // GROUP BY on a qualified key, ordered by it spelled qualified and bare
            std::map<std::string, std::pair<int, int>> groups; // name -> (rows, sum of val)
            for (auto& t : T) {
                for (auto& u : U) {
                    if (u.id == t.grp) {
                        groups[u.name].first++;
                        groups[u.name].second += t.val;
                    }
                }
            }
            for (const char* order : {"y.name DESC", "name DESC", "y.name"}) {
                std::vector<std::vector<std::string>> expected;
                for (auto& [name, agg] : groups) expected.push_back({name, std::to_string(agg.first), std::to_string(agg.second)});
                if (std::string(order).find("DESC") != std::string::npos) std::reverse(expected.begin(), expected.end());
                std::string sql = std::string("SELECT y.name, COUNT(*), SUM(x.val) FROM t x JOIN u y ON y.id = x.grp GROUP BY y.name ORDER BY ") + order;
                INFO(sql);
                REQUIRE(table_cells(ok_text(ex, sql)) == expected);
            }
        }
    }
    std::string seen_algorithms;
    for (auto& a : algorithms) seen_algorithms += a + "; ";
    INFO("join algorithms seen: " << seen_algorithms);
    REQUIRE(algorithms.size() >= 4); // the answers held through different join algorithms, not through one of them
    REQUIRE(checked >= 600);
}

// The same statement spelled with and without a table qualifier, on tables whose column names do not collide (so the bare
// spelling is unambiguous): the rows have to be the same, wherever the statement reads its columns from -- a plain scan, a
// join, a group, a view, a CTE, a derived table, an index path.
TEST_CASE("table.column spellings answer like the bare spellings in ORDER BY, DISTINCT and everywhere around them",
          "[query_paths][qualified]") {
    TempDataDir dir("qp_qualified_twins");
    Executor ex(dir.path);
    open_db(ex);
    REQUIRE(ex.execute_sql("CREATE TABLE a (aid INT PRIMARY KEY, g INT, v INT)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE b (bid INT PRIMARY KEY, nm VARCHAR(5), w INT)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO a VALUES (1,2,50),(2,1,40),(3,2,30),(4,1,20),(5,3,10),(6,2,NULL),(7,3,60),(8,1,35)").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO b VALUES (1,'x',7),(2,'y',8),(3,'z',9)").is_ok());
    REQUIRE(ex.execute_sql("CREATE INDEX a_v ON a (v)").is_ok());
    REQUIRE(ex.execute_sql("CREATE VIEW vv AS SELECT aid, v FROM a").is_ok());
    const std::string J = "FROM a JOIN b ON b.bid = a.g";
    const std::vector<std::pair<std::string, std::string>> twins = {
        {"SELECT aid FROM a ORDER BY a.v DESC", "SELECT aid FROM a ORDER BY v DESC"},
        {"SELECT x.aid FROM a x ORDER BY x.v DESC, x.aid", "SELECT aid FROM a ORDER BY v DESC, aid"},
        {"SELECT a.aid, b.nm " + J + " ORDER BY a.v DESC, a.aid", "SELECT aid, nm " + J + " ORDER BY v DESC, aid"},
        {"SELECT a.aid, b.nm " + J + " ORDER BY b.nm, a.aid DESC", "SELECT aid, nm " + J + " ORDER BY nm, aid DESC"},
        {"SELECT b.nm, COUNT(*) " + J + " GROUP BY b.nm ORDER BY b.nm DESC", "SELECT nm, COUNT(*) " + J + " GROUP BY nm ORDER BY nm DESC"},
        {"SELECT b.nm, COUNT(*) " + J + " GROUP BY b.nm ORDER BY nm DESC", "SELECT nm, COUNT(*) " + J + " GROUP BY nm ORDER BY nm DESC"},
        {"SELECT nm, COUNT(*) " + J + " GROUP BY nm ORDER BY b.nm DESC", "SELECT nm, COUNT(*) " + J + " GROUP BY nm ORDER BY nm DESC"},
        {"SELECT aid FROM a ORDER BY a.v DESC, a.aid LIMIT 3 OFFSET 1", "SELECT aid FROM a ORDER BY v DESC, aid LIMIT 3 OFFSET 1"},
        {"SELECT aid FROM a WHERE a.v > 15 ORDER BY a.v DESC LIMIT 2", "SELECT aid FROM a WHERE v > 15 ORDER BY v DESC LIMIT 2"}, // the index's top-K path
        {"SELECT DISTINCT a.g FROM a ORDER BY g", "SELECT DISTINCT g FROM a ORDER BY g"},
        {"SELECT DISTINCT x.g FROM a x ORDER BY x.g DESC", "SELECT DISTINCT g FROM a ORDER BY g DESC"},
        {"SELECT DISTINCT b.nm " + J + " ORDER BY nm", "SELECT DISTINCT nm " + J + " ORDER BY nm"},
        {"SELECT DISTINCT a.g, b.nm " + J + " ORDER BY a.g", "SELECT DISTINCT g, nm " + J + " ORDER BY g"},
        {"SELECT DISTINCT a.g AS gg FROM a ORDER BY gg", "SELECT DISTINCT g AS gg FROM a ORDER BY gg"},
        {"SELECT DISTINCT a.g FROM a ORDER BY a.g DESC LIMIT 2", "SELECT DISTINCT g FROM a ORDER BY g DESC LIMIT 2"},
        {"SELECT aid, ROW_NUMBER() OVER (ORDER BY a.v DESC) AS rn FROM a ORDER BY aid", "SELECT aid, ROW_NUMBER() OVER (ORDER BY v DESC) AS rn FROM a ORDER BY aid"},
        {"WITH c AS (SELECT a.aid, a.v FROM a) SELECT c.aid FROM c ORDER BY c.v DESC", "WITH c AS (SELECT aid, v FROM a) SELECT aid FROM c ORDER BY v DESC"},
        {"SELECT vv.aid FROM vv ORDER BY vv.v DESC", "SELECT aid FROM vv ORDER BY v DESC"},
        {"SELECT d.aid FROM (SELECT aid, v FROM a) d ORDER BY d.v DESC", "SELECT aid FROM (SELECT aid, v FROM a) d ORDER BY v DESC"},
        {"SELECT a.aid, b.nm FROM a LEFT JOIN b ON b.bid = a.g ORDER BY a.v DESC, a.aid", "SELECT aid, nm FROM a LEFT JOIN b ON b.bid = a.g ORDER BY v DESC, aid"},
        {"SELECT a.aid FROM a WHERE a.g IN (SELECT b.bid FROM b WHERE b.w > 7) ORDER BY a.v, a.aid", "SELECT aid FROM a WHERE g IN (SELECT bid FROM b WHERE w > 7) ORDER BY v, aid"},
    };
    for (auto& [qualified, bare] : twins) {
        INFO(qualified << "   vs   " << bare);
        auto q = table_cells(ok_text(ex, qualified)), b = table_cells(ok_text(ex, bare));
        REQUIRE(q == b);
        REQUIRE_FALSE(q.empty());
    }
    // they are not trivially equal: the sorted answers differ from the table order, and DISTINCT really drops rows
    REQUIRE(table_cells(ok_text(ex, "SELECT aid FROM a ORDER BY a.v DESC, a.aid"))[0][0] != "1");
    REQUIRE(table_cells(ok_text(ex, "SELECT DISTINCT a.g FROM a")).size() == 3);
}
