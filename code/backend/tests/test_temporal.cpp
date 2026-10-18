#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "catch.hpp"
#include "engine/executor/executor.hpp"
#include "engine/result_text.hpp"

using namespace engine;
namespace fs = std::filesystem;

// DATETIME(n) / TIMESTAMP(n) / TIME(n) keep n places of the fraction of a second; they are part of the table when it is saved, and a restarted server has them
// again. AUTO_INCREMENT = n moves the counter of the table (also across a restart). The answers themselves are checked against MySQL in the
// conformance cases (corpus/temporal.sql); here are the things only a restart or a second statement can show.

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

void ok(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_ok());
}

void fails(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_err());
}

using Rows = std::vector<std::vector<std::string>>;

Rows rows_of(Executor& ex, const std::string& sql) {
    auto r = ex.execute_sql(sql);
    INFO(sql << " -> " << (r.is_err() ? r.error() : "ok"));
    REQUIRE(r.is_ok());
    Rows rows;
    std::istringstream in(r.value());
    std::string line;
    int bars = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] != '|') continue;
        if (++bars == 1) continue;
        rows.push_back(result_text::split_row(line));
    }
    return rows;
}
} // namespace

TEST_CASE("the places of the fraction of a second are kept by the table", "[temporal]") {
    TempDataDir dir("temporal_restart");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        ok(ex, "CREATE TABLE f (id INT PRIMARY KEY, a DATETIME(3), b TIMESTAMP(6) NULL, c TIME(2), d DATETIME, e TIME)");
        ok(ex, "INSERT INTO f VALUES (1, '2024-02-29 13:45:10.6789', '2024-02-29 13:45:10.1', '13:45:10.678', '2024-02-29 13:45:10.678', '13:45:10.5')");
        REQUIRE(rows_of(ex, "SELECT a, b, c, d, e FROM f") == Rows{{"2024-02-29 13:45:10.679", "2024-02-29 13:45:10.100000", "13:45:10.68", "2024-02-29 13:45:11", "13:45:11"}});
        REQUIRE(rows_of(ex, "SELECT column_name, column_type, datetime_precision FROM information_schema.columns WHERE table_name = 'f' ORDER BY ordinal_position") ==
                Rows{{"id", "int", "NULL"}, {"a", "datetime(3)", "3"}, {"b", "timestamp(6)", "6"}, {"c", "time(2)", "2"}, {"d", "datetime", "0"}, {"e", "time", "0"}});
        auto shown = ex.execute_sql("SHOW CREATE TABLE f");
        REQUIRE(shown.is_ok());
        REQUIRE(shown.value().find("`a` DATETIME(3)") != std::string::npos);
        REQUIRE(shown.value().find("`c` TIME(2)") != std::string::npos);
    }
    // a restarted server: the places are still there, so a new value is rounded to them and a default has them
    Executor again(dir.path);
    ok(again, "USE d");
    ok(again, "INSERT INTO f (id, a, c) VALUES (2, '2024-12-31 23:59:59.9996', '23:59:59.996')");
    REQUIRE(rows_of(again, "SELECT a, c FROM f WHERE id = 2") == Rows{{"2025-01-01 00:00:00.000", "24:00:00.00"}});
    ok(again, "CREATE TABLE g (id INT PRIMARY KEY, t DATETIME(3) DEFAULT CURRENT_TIMESTAMP(3), u TIMESTAMP DEFAULT CURRENT_TIMESTAMP)");
    ok(again, "INSERT INTO g (id) VALUES (1)");
    REQUIRE(rows_of(again, "SELECT LENGTH(t), LENGTH(u) FROM g") == Rows{{"23", "19"}});
    ok(again, "ALTER TABLE g MODIFY t DATETIME(6) DEFAULT CURRENT_TIMESTAMP(6)");
    REQUIRE(rows_of(again, "SELECT column_type FROM information_schema.columns WHERE table_name = 'g' AND column_name = 't'") == Rows{{"datetime(6)"}});
}

TEST_CASE("a fraction of a second rounds up into the next second, minute, day and year", "[temporal]") {
    TempDataDir dir("temporal_carry");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE r (id INT PRIMARY KEY, dt DATETIME, t TIME)");
    ok(ex, "INSERT INTO r VALUES (1, '2024-02-28 23:59:59.5', '00:59:59.5')");
    ok(ex, "INSERT INTO r VALUES (2, '2024-12-31 23:59:59.5', '-00:00:00.5')");
    ok(ex, "INSERT INTO r VALUES (3, '2024-02-29 00:00:00.4999999', '838:59:58.5')");
    REQUIRE(rows_of(ex, "SELECT dt, t FROM r ORDER BY id") ==
            Rows{{"2024-02-29 00:00:00", "01:00:00"}, {"2025-01-01 00:00:00", "-00:00:01"}, {"2024-02-29 00:00:01", "838:59:59"}}); // (the seventh place rounds first: .4999999 is .500000)
    fails(ex, "INSERT INTO r VALUES (4, '9999-12-31 23:59:59.5', '00:00:00')");
    fails(ex, "INSERT INTO r VALUES (5, '2024-01-01', '838:59:59.5')");
    fails(ex, "CREATE TABLE tooprecise (a DATETIME(7))");
}

TEST_CASE("ALTER TABLE ... AUTO_INCREMENT = n moves the counter, never below the largest number held", "[temporal]") {
    TempDataDir dir("temporal_auto_increment");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        ok(ex, "CREATE TABLE a (id INT PRIMARY KEY AUTO_INCREMENT, v INT)");
        ok(ex, "INSERT INTO a (v) VALUES (1), (2)");
        ok(ex, "ALTER TABLE a AUTO_INCREMENT = 1000");
        ok(ex, "INSERT INTO a (v) VALUES (3)");
        REQUIRE(rows_of(ex, "SELECT id FROM a ORDER BY id") == Rows{{"1"}, {"2"}, {"1000"}});
        REQUIRE(rows_of(ex, "SELECT auto_increment FROM information_schema.tables WHERE table_schema = 'd' AND table_name = 'a'") == Rows{{"1001"}});
        ok(ex, "ALTER TABLE a AUTO_INCREMENT = 5");
        ok(ex, "INSERT INTO a (v) VALUES (4)");
        REQUIRE(rows_of(ex, "SELECT MAX(id) FROM a") == Rows{{"1001"}});
        ok(ex, "ALTER TABLE a AUTO_INCREMENT = 5000, ADD COLUMN w INT");
        fails(ex, "ALTER TABLE nowhere AUTO_INCREMENT = 5");
    }
    Executor again(dir.path);
    ok(again, "USE d");
    ok(again, "INSERT INTO a (v) VALUES (5)");
    REQUIRE(rows_of(again, "SELECT MAX(id) FROM a") == Rows{{"5000"}});
}

TEST_CASE("DECIMAL, FLOAT and DOUBLE can be UNSIGNED, and REAL is a DOUBLE", "[temporal]") {
    TempDataDir dir("temporal_unsigned");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE u (id INT PRIMARY KEY, c DECIMAL(5,2) UNSIGNED, f FLOAT UNSIGNED, d DOUBLE UNSIGNED, r REAL)");
    ok(ex, "INSERT INTO u VALUES (1, 2.5, 3.5, 4.5, 0.1)");
    fails(ex, "INSERT INTO u (id, c) VALUES (2, -1)");
    fails(ex, "INSERT INTO u (id, f) VALUES (2, -0.5)");
    fails(ex, "INSERT INTO u (id, d) VALUES (2, -3)");
    ok(ex, "INSERT INTO u (id, c) VALUES (3, -0.001)"); // (rounds to 0.00)
    REQUIRE(rows_of(ex, "SELECT c, r * 3 FROM u ORDER BY id") == Rows{{"2.50", "0.30000000000000004"}, {"0.00", "NULL"}});
    REQUIRE(rows_of(ex, "SELECT column_type FROM information_schema.columns WHERE table_name = 'u' ORDER BY ordinal_position") ==
            Rows{{"int"}, {"decimal(5,2) unsigned"}, {"float unsigned"}, {"double unsigned"}, {"double"}});
}

TEST_CASE("a default of CURRENT_TIMESTAMP(n) and ON UPDATE CURRENT_TIMESTAMP(n) carry the fraction of the moment", "[temporal]") {
    TempDataDir dir("temporal_default_fraction");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE g (id INT PRIMARY KEY, t DATETIME(3) DEFAULT CURRENT_TIMESTAMP(3), u TIMESTAMP(6) DEFAULT CURRENT_TIMESTAMP(6) ON UPDATE CURRENT_TIMESTAMP(6), v DATETIME DEFAULT CURRENT_TIMESTAMP, w INT)");
    for (int i = 1; i <= 40; i++) ok(ex, "INSERT INTO g (id) VALUES (" + std::to_string(i) + ")");
    // (a thousandth, a millionth: forty rows that all fall on a whole millisecond are not a matter of luck)
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM g WHERE MICROSECOND(t) <> 0")[0][0] != "0");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM g WHERE MICROSECOND(t) % 1000 <> 0")[0][0] == "0");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM g WHERE MICROSECOND(u) <> 0")[0][0] != "0");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM g WHERE MICROSECOND(v) <> 0")[0][0] == "0");
    ok(ex, "UPDATE g SET u = '2020-01-01 00:00:00'");
    ok(ex, "UPDATE g SET w = id"); // (the row changes: ON UPDATE sets u to the moment, with its six places)
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM g WHERE MICROSECOND(u) <> 0")[0][0] != "0");
    REQUIRE(rows_of(ex, "SELECT COUNT(*) FROM g WHERE u > '2021-01-01'")[0][0] == "40");
}

TEST_CASE("UTC_TIMESTAMP is the time in Greenwich, whatever the clock of the machine says", "[temporal]") {
    TempDataDir dir("temporal_utc");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    const std::time_t now = std::time(nullptr);
    std::tm local{}, utc{};
    localtime_s(&local, &now);
    gmtime_s(&utc, &now);
    const long long offset_minutes = (static_cast<long long>(_mkgmtime(&local)) - static_cast<long long>(_mkgmtime(&utc))) / 60;
    REQUIRE(rows_of(ex, "SELECT TIMESTAMPDIFF(MINUTE, UTC_TIMESTAMP(), NOW())") == Rows{{std::to_string(offset_minutes)}});
    REQUIRE(rows_of(ex, "SELECT LENGTH(UTC_TIME()), LENGTH(UTC_TIME(2))") == Rows{{"8", "11"}});
    fails(ex, "SELECT NOW(7)");
    fails(ex, "SELECT UTC_TIMESTAMP(9)");
}

TEST_CASE("ALTER TABLE ... AUTO_INCREMENT does not count the rows that were deleted, and it is saved by itself", "[temporal]") {
    TempDataDir dir("temporal_auto_increment_deleted");
    {
        Executor ex(dir.path);
        ok(ex, "CREATE DATABASE d");
        ok(ex, "USE d");
        ok(ex, "CREATE TABLE a (id INT PRIMARY KEY AUTO_INCREMENT, v INT)");
        ok(ex, "INSERT INTO a (v) VALUES (1), (2), (3)");
        ok(ex, "DELETE FROM a WHERE id = 3");
        ok(ex, "ALTER TABLE a AUTO_INCREMENT = 2"); // (the rows left hold 1 and 2: the next number is 3, as the deleted row is gone)
        ok(ex, "INSERT INTO a (v) VALUES (4)");
        REQUIRE(rows_of(ex, "SELECT MAX(id) FROM a") == Rows{{"3"}});
        ok(ex, "ALTER TABLE a AUTO_INCREMENT = 700"); // (nothing else is altered: only this saves the counter)
    }
    Executor again(dir.path);
    ok(again, "USE d");
    ok(again, "INSERT INTO a (v) VALUES (5)");
    REQUIRE(rows_of(again, "SELECT MAX(id) FROM a") == Rows{{"700"}});
}

TEST_CASE("an index declared USING HASH is a hash index only when the statement that makes it says so", "[temporal]") {
    TempDataDir dir("temporal_index_options");
    Executor ex(dir.path);
    ok(ex, "CREATE DATABASE d");
    ok(ex, "USE d");
    ok(ex, "CREATE TABLE x (a INT, b INT, c INT, d INT)");
    ok(ex, "CREATE INDEX ia ON x (a) USING BTREE COMMENT 'one'");
    ok(ex, "CREATE INDEX ib USING HASH ON x (b)");
    ok(ex, "CREATE INDEX ic ON x (c) USING HASH");
    ok(ex, "ALTER TABLE x ADD INDEX id_ (d) USING BTREE COMMENT 'four' INVISIBLE KEY_BLOCK_SIZE = 4");
    auto s = ex.get_shared()->read();
    REQUIRE(s->hash_index_meta.size() == 2); // (ib and ic; the type may come before ON or after the columns)
}
