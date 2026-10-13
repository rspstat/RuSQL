#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
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

// What a foreign-key action does to the child rows has to be part of the statement that caused it: a ROLLBACK (or ROLLBACK TO SAVEPOINT) gives back the child rows as well, a
// reader that began before the statement keeps seeing the old child rows, and the indexes of the child table agree with its rows. The cascades used to edit the rows in place
// without telling the transaction manager: after a ROLLBACK the parent was back and its children stayed deleted, nulled, or pointing at a key the parent no longer had.

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
} // namespace

// Every action, on a delete and on an update of the parent, with a ROLLBACK, a ROLLBACK TO SAVEPOINT and a COMMIT.
TEST_CASE("a ROLLBACK gives back the child rows a foreign-key action changed", "[cascade_rollback]") {
    TempDataDir dir("cascade_rollback");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    struct Case {
        const char* name;
        const char* action;   // the clause
        const char* statement;
        Rows after;           // the children after the statement (ids 10 -> parent 1, 11 -> parent 2, 12 -> parent 1)
    };
    const Rows untouched = {{"10", "1"}, {"11", "2"}, {"12", "1"}};
    const std::vector<Case> cases = {
        {"delete cascade", "ON DELETE CASCADE", "DELETE FROM p WHERE id = 1", {{"11", "2"}}},
        {"delete set null", "ON DELETE SET NULL", "DELETE FROM p WHERE id = 1", {{"10", "NULL"}, {"11", "2"}, {"12", "NULL"}}},
        {"delete set default", "ON DELETE SET DEFAULT", "DELETE FROM p WHERE id = 1", {{"10", "2"}, {"11", "2"}, {"12", "2"}}},
        {"update cascade", "ON UPDATE CASCADE", "UPDATE p SET id = 5 WHERE id = 1", {{"10", "5"}, {"11", "2"}, {"12", "5"}}},
        {"update set null", "ON UPDATE SET NULL", "UPDATE p SET id = 5 WHERE id = 1", {{"10", "NULL"}, {"11", "2"}, {"12", "NULL"}}},
        {"update set default", "ON UPDATE SET DEFAULT", "UPDATE p SET id = 5 WHERE id = 1", {{"10", "2"}, {"11", "2"}, {"12", "2"}}},
    };
    for (auto& c : cases) {
        for (int ending = 0; ending < 4; ending++) { // 0: ROLLBACK, 1: ROLLBACK TO SAVEPOINT, 2: COMMIT, 3: no transaction at all
            INFO(c.name << ", ending " << ending);
            ok(ex, "DROP TABLE IF EXISTS c");
            ok(ex, "DROP TABLE IF EXISTS p");
            ok(ex, "CREATE TABLE p (id INT PRIMARY KEY, n INT)");
            ok(ex, std::string("CREATE TABLE c (id INT PRIMARY KEY, pid INT DEFAULT 2, FOREIGN KEY (pid) REFERENCES p(id) ") + c.action + ")");
            ok(ex, "CREATE INDEX c_pid ON c (pid)");
            ok(ex, "CREATE INDEX c_hash ON c (pid) USING HASH");
            ok(ex, "CREATE INDEX c_both ON c (pid, id)");
            ok(ex, "INSERT INTO p VALUES (1, 0), (2, 0)");
            ok(ex, "INSERT INTO c VALUES (10, 1), (11, 2), (12, 1)");
            if (ending != 3) ok(ex, "BEGIN");
            if (ending == 1) ok(ex, "SAVEPOINT s1");
            ok(ex, c.statement);
            const Rows expected = c.after;
            // the rows, the primary key, the secondary index and the hash index all agree with what the cascade did
            REQUIRE(rows_of(ex, "SELECT id, pid FROM c ORDER BY id") == expected);
            for (auto& row : expected) {
                REQUIRE(rows_of(ex, "SELECT pid FROM c WHERE id = " + row[0]) == Rows{{row[1]}});
                if (row[1] != "NULL") REQUIRE(rows_of(ex, "SELECT id FROM c WHERE pid = " + row[1] + " AND id = " + row[0]) == Rows{{row[0]}}); // (the composite index)
                if (row[1] != "NULL") {
                    Rows ids;
                    for (auto& other : expected) {
                        if (other[1] == row[1]) ids.push_back({other[0]});
                    }
                    REQUIRE(rows_of(ex, "SELECT id FROM c WHERE pid = " + row[1] + " ORDER BY id") == ids);
                }
            }
            if (ending == 0) {
                ok(ex, "ROLLBACK");
            } else if (ending == 1) {
                ok(ex, "ROLLBACK TO s1");
                ok(ex, "COMMIT");
            } else if (ending == 2) {
                ok(ex, "COMMIT");
            }
            if (ending <= 1) {
                REQUIRE(rows_of(ex, "SELECT id, pid FROM c ORDER BY id") == untouched);
                REQUIRE(rows_of(ex, "SELECT id FROM p ORDER BY id") == Rows{{"1"}, {"2"}});
                REQUIRE(rows_of(ex, "SELECT id FROM c WHERE pid = 1 ORDER BY id") == Rows{{"10"}, {"12"}});
                REQUIRE(rows_of(ex, "SELECT pid FROM c WHERE id = 10") == Rows{{"1"}});
            } else {
                REQUIRE(rows_of(ex, "SELECT id, pid FROM c ORDER BY id") == expected);
            }
        }
    }
}

// A reader that began before the cascade keeps seeing the child rows as they were (a repeatable read), and one that begins after sees the new ones.
TEST_CASE("a snapshot taken before a cascade keeps the old child rows", "[cascade_rollback]") {
    TempDataDir dir("cascade_snapshot");
    Executor writer(dir.path);
    ok(writer, "CREATE DATABASE d");
    ok(writer, "USE d");
    auto shared = writer.get_shared();
    Executor reader = Executor::new_session(shared);
    ok(reader, "USE d");
    ok(writer, "CREATE TABLE p (id INT PRIMARY KEY)");
    ok(writer, "CREATE TABLE c (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p(id) ON DELETE SET NULL ON UPDATE CASCADE)");
    ok(writer, "INSERT INTO p VALUES (1), (2)");
    ok(writer, "INSERT INTO c VALUES (10, 1), (11, 2)");
    ok(reader, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ");
    ok(reader, "BEGIN");
    const Rows before = rows_of(reader, "SELECT id, pid FROM c ORDER BY id");
    REQUIRE(before == Rows{{"10", "1"}, {"11", "2"}});
    ok(writer, "UPDATE p SET id = 7 WHERE id = 1"); // ON UPDATE CASCADE: c 10 follows
    ok(writer, "DELETE FROM p WHERE id = 2");       // ON DELETE SET NULL: c 11 loses its parent
    REQUIRE(rows_of(writer, "SELECT id, pid FROM c ORDER BY id") == Rows{{"10", "7"}, {"11", "NULL"}});
    REQUIRE(rows_of(reader, "SELECT id, pid FROM c ORDER BY id") == before);
    ok(reader, "COMMIT");
    REQUIRE(rows_of(reader, "SELECT id, pid FROM c ORDER BY id") == Rows{{"10", "7"}, {"11", "NULL"}});
}

// Random statements on a parent and a child table in transactions, savepoints and rollbacks, against a model of the three actions (RESTRICT, CASCADE, SET NULL) on delete
// and on update.
TEST_CASE("foreign-key actions follow a model through transactions, savepoints and rollbacks", "[cascade_rollback][random]") {
    unsigned seed_count = 9; // RUSQL_FUZZ_SEEDS=40 makes it a long campaign
    if (const char* e = std::getenv("RUSQL_FUZZ_SEEDS")) seed_count = static_cast<unsigned>(std::strtoul(e, nullptr, 10));
    const char* actions[] = {"RESTRICT", "CASCADE", "SET NULL"};
    struct State {
        std::set<int> parents;
        std::map<int, std::optional<int>> kids;
        bool operator==(const State&) const = default;
    };
    for (unsigned seed = 1; seed <= seed_count; seed++) {
        const int on_delete = static_cast<int>(seed % 3), on_update = static_cast<int>((seed / 3) % 3);
        INFO("seed " << seed << " ON DELETE " << actions[on_delete] << " ON UPDATE " << actions[on_update]);
        TempDataDir dir("cascade_random");
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        ok(ex, "CREATE TABLE p (id INT PRIMARY KEY)");
        ok(ex, std::string("CREATE TABLE c (id INT PRIMARY KEY, pid INT, FOREIGN KEY (pid) REFERENCES p(id) ON DELETE ") + actions[on_delete] + " ON UPDATE " + actions[on_update] + ")");
        ok(ex, "CREATE INDEX c_pid ON c (pid)");
        ok(ex, "CREATE INDEX c_hash ON c (pid) USING HASH");
        std::mt19937 rng(seed * 2654435761u);
        auto pick = [&](std::size_t n) { return static_cast<std::size_t>(rng() % n); };
        State committed, now;
        std::vector<std::pair<std::string, State>> savepoints;
        bool in_txn = false;
        int savepoint_no = 0;
        auto read_state = [&]() {
            State st;
            for (auto& r : rows_of(ex, "SELECT id FROM p ORDER BY id")) {
                INFO("parent " << r[0] << " is there twice");
                REQUIRE(st.parents.insert(std::stoi(r[0])).second);
            }
            for (auto& r : rows_of(ex, "SELECT id, pid FROM c ORDER BY id")) {
                INFO("child " << r[0] << " is there twice");
                REQUIRE(st.kids.emplace(std::stoi(r[0]), r[1] == "NULL" ? std::nullopt : std::optional<int>(std::stoi(r[1]))).second);
            }
            return st;
        };
        for (int step = 0; step < 220; step++) {
            std::string sql;
            bool expect_ok = true;
            State next = now;
            const std::size_t kind = pick(100);
            auto some_parent = [&]() -> std::optional<int> {
                if (now.parents.empty()) return std::nullopt;
                auto it = now.parents.begin();
                std::advance(it, static_cast<long>(pick(now.parents.size())));
                return *it;
            };
            auto some_kid = [&]() -> std::optional<int> {
                if (now.kids.empty()) return std::nullopt;
                auto it = now.kids.begin();
                std::advance(it, static_cast<long>(pick(now.kids.size())));
                return it->first;
            };
            if (kind < 14) {
                int id = 1 + static_cast<int>(pick(30));
                if (now.parents.count(id)) continue;
                sql = "INSERT INTO p VALUES (" + std::to_string(id) + ")";
                next.parents.insert(id);
            } else if (kind < 34) {
                int id = 1 + static_cast<int>(pick(60));
                if (now.kids.count(id)) continue;
                auto parent = pick(6) == 0 ? std::nullopt : some_parent();
                sql = "INSERT INTO c VALUES (" + std::to_string(id) + ", " + (parent ? std::to_string(*parent) : "NULL") + ")";
                next.kids[id] = parent;
            } else if (kind < 44) {
                auto kid = some_kid();
                if (!kid) continue;
                auto parent = pick(5) == 0 ? std::nullopt : some_parent();
                sql = "UPDATE c SET pid = " + (parent ? std::to_string(*parent) : "NULL") + " WHERE id = " + std::to_string(*kid);
                next.kids[*kid] = parent;
            } else if (kind < 52) {
                auto kid = some_kid();
                if (!kid) continue;
                sql = "DELETE FROM c WHERE id = " + std::to_string(*kid);
                next.kids.erase(*kid);
            } else if (kind < 64) {
                auto parent = some_parent();
                if (!parent) continue;
                sql = "DELETE FROM p WHERE id = " + std::to_string(*parent);
                std::vector<int> children;
                for (auto& [id, pid] : now.kids) {
                    if (pid && *pid == *parent) children.push_back(id);
                }
                if (!children.empty() && on_delete == 0) {
                    expect_ok = false;
                } else {
                    next.parents.erase(*parent);
                    for (int id : children) {
                        if (on_delete == 1) next.kids.erase(id);
                        else next.kids[id] = std::nullopt;
                    }
                }
            } else if (kind < 76) {
                auto parent = some_parent();
                if (!parent) continue;
                int target = 31 + static_cast<int>(pick(40));
                if (now.parents.count(target)) continue;
                sql = "UPDATE p SET id = " + std::to_string(target) + " WHERE id = " + std::to_string(*parent);
                std::vector<int> children;
                for (auto& [id, pid] : now.kids) {
                    if (pid && *pid == *parent) children.push_back(id);
                }
                if (!children.empty() && on_update == 0) {
                    expect_ok = false;
                } else {
                    next.parents.erase(*parent);
                    next.parents.insert(target);
                    for (int id : children) next.kids[id] = on_update == 1 ? std::optional<int>(target) : std::nullopt;
                }
            } else if (kind < 82) {
                if (in_txn) {
                    sql = "COMMIT";
                    committed = now;
                    in_txn = false;
                    savepoints.clear();
                } else {
                    sql = "BEGIN";
                    in_txn = true;
                }
            } else if (kind < 90) {
                if (!in_txn) continue;
                sql = "ROLLBACK";
                now = committed;
                next = now;
                in_txn = false;
                savepoints.clear();
            } else if (kind < 95) {
                if (!in_txn) continue;
                sql = "SAVEPOINT sp" + std::to_string(++savepoint_no);
                savepoints.push_back({"sp" + std::to_string(savepoint_no), now});
            } else {
                if (!in_txn || savepoints.empty()) continue;
                auto& target = savepoints[pick(savepoints.size())];
                sql = "ROLLBACK TO " + target.first;
                now = target.second;
                next = now;
                // the savepoints made after the one rolled back to are gone
                while (savepoints.back().first != target.first) savepoints.pop_back();
            }
            INFO("step " << step << ": " << sql);
            auto r = ex.execute_sql(sql);
            INFO("answer: " << (r.is_err() ? r.error() : std::string("ok")));
            REQUIRE(r.is_ok() == expect_ok);
            if (expect_ok) {
                now = next;
                if (!in_txn) committed = now;
            }
            if (sql == "ROLLBACK" || sql.rfind("ROLLBACK TO", 0) == 0) { /* the model state was set above */ }
            {
                const State actual = read_state();
                auto show = [](const State& st) {
                    std::string out = "parents:";
                    for (int p : st.parents) out += " " + std::to_string(p);
                    out += " | kids:";
                    for (auto& [id, pid] : st.kids) out += " " + std::to_string(id) + "->" + (pid ? std::to_string(*pid) : std::string("NULL"));
                    return out;
                };
                INFO("engine: " << show(actual));
                INFO("model:  " << show(now));
                REQUIRE(actual == now);
            }
            if (step % 7 == 0 && !now.kids.empty()) { // the indexes of the child table say the same as its rows
                auto parent = some_parent();
                if (parent) {
                    Rows ids;
                    for (auto& [id, pid] : now.kids) {
                        if (pid && *pid == *parent) ids.push_back({std::to_string(id)});
                    }
                    REQUIRE(rows_of(ex, "SELECT id FROM c WHERE pid = " + std::to_string(*parent) + " ORDER BY id") == ids);
                }
                auto kid = some_kid();
                REQUIRE(rows_of(ex, "SELECT pid FROM c WHERE id = " + std::to_string(*kid)) ==
                        Rows{{now.kids[*kid] ? std::to_string(*now.kids[*kid]) : std::string("NULL")}});
            }
        }
        if (in_txn) ok(ex, "ROLLBACK");
    }
}

// Found by the model above, and not about foreign keys: ROLLBACK TO SAVEPOINT left the row twice after an UPDATE that changed the primary key (the new version was looked for
// under the old key) and after an UPDATE followed by a DELETE of the same row (the DELETE's rollback brought back the version the UPDATE had ended as well).
TEST_CASE("ROLLBACK TO SAVEPOINT undoes a key change and an UPDATE followed by a DELETE of the same row", "[cascade_rollback]") {
    TempDataDir dir("rollback_to_versions");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE t (id INT PRIMARY KEY, v INT)");
    ok(ex, "INSERT INTO t VALUES (1, 10), (2, 20)");
    ok(ex, "BEGIN");
    ok(ex, "SAVEPOINT s1");
    ok(ex, "UPDATE t SET id = 5 WHERE id = 1");
    ok(ex, "UPDATE t SET id = 9 WHERE id = 5"); // the key changes twice
    REQUIRE(rows_of(ex, "SELECT id, v FROM t ORDER BY id") == Rows{{"2", "20"}, {"9", "10"}});
    ok(ex, "ROLLBACK TO s1");
    REQUIRE(rows_of(ex, "SELECT id, v FROM t ORDER BY id") == Rows{{"1", "10"}, {"2", "20"}});
    ok(ex, "UPDATE t SET v = 11 WHERE id = 1");
    ok(ex, "SAVEPOINT s2");
    ok(ex, "DELETE FROM t WHERE id = 1");
    REQUIRE(rows_of(ex, "SELECT id, v FROM t ORDER BY id") == Rows{{"2", "20"}});
    ok(ex, "ROLLBACK TO s2");
    REQUIRE(rows_of(ex, "SELECT id, v FROM t ORDER BY id") == Rows{{"1", "11"}, {"2", "20"}});
    REQUIRE(rows_of(ex, "SELECT v FROM t WHERE id = 1") == Rows{{"11"}});
    ok(ex, "COMMIT");
    REQUIRE(rows_of(ex, "SELECT id, v FROM t ORDER BY id") == Rows{{"1", "11"}, {"2", "20"}});
}
