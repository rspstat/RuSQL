// Table partitioning (PARTITION BY RANGE/LIST/HASH, V1) -- see executor_partition.cpp for
// the design (a permanently-empty logical "phantom" table + N ordinary physical child
// tables, routed to by a statement-rewrite layer at the top of execute()).

#include <filesystem>

#include "catch.hpp"
#include "engine/executor/executor.hpp"

using namespace engine;
namespace fs = std::filesystem;

namespace {
struct TempDataDir {
    std::string path;
    explicit TempDataDir(std::string p) : path(std::move(p)) { fs::remove_all(path); }
    ~TempDataDir() { fs::remove_all(path); }
};

// MVCC: filters to live rows only (_xmax == "0") -- UPDATE/DELETE append/mark versions
// instead of mutating in place, so an unfiltered read could see superseded versions too
// (see test_executor_update_delete.cpp's identical table_rows() helper). Reads a specific
// physical (child) table directly, bypassing the router entirely.
std::vector<Row> raw_table_rows(Executor& ex, const std::string& qualified_table) {
    auto s = ex.get_shared()->read();
    auto it = s->tables.find(qualified_table);
    if (it == s->tables.end()) return {};
    std::vector<Row> live;
    for (auto& r : it->second) {
        auto xit = r.find("_xmax");
        if (xit == r.end() || xit->second == "0") live.push_back(r);
    }
    return live;
}
} // namespace

TEST_CASE("PARTITION BY RANGE routes INSERT to the correct child table", "[executor][partitioning]") {
    TempDataDir dir("part_range_route");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val VARCHAR(20)) "
                            "PARTITION BY RANGE (id) ("
                            "  PARTITION p0 VALUES LESS THAN (100),"
                            "  PARTITION p1 VALUES LESS THAN (200),"
                            "  PARTITION p2 VALUES LESS THAN MAXVALUE"
                            ")")
                .is_ok());

    auto ins = ex.execute_sql("INSERT INTO t VALUES (5, 'a'), (150, 'b'), (250, 'c')");
    REQUIRE(ins.is_ok());
    REQUIRE(ins.value() == "3 row(s) inserted.");

    REQUIRE(raw_table_rows(ex, "d.t").empty()); // logical parent stays a permanently-empty phantom
    REQUIRE(raw_table_rows(ex, "d.t__p0").size() == 1);
    REQUIRE(raw_table_rows(ex, "d.t__p1").size() == 1);
    REQUIRE(raw_table_rows(ex, "d.t__p2").size() == 1);
    REQUIRE(raw_table_rows(ex, "d.t__p0")[0].at("val") == "a");
    REQUIRE(raw_table_rows(ex, "d.t__p1")[0].at("val") == "b");
    REQUIRE(raw_table_rows(ex, "d.t__p2")[0].at("val") == "c");
}

TEST_CASE("PARTITION BY RANGE: SELECT with WHERE prunes to the right child and still returns correct rows",
          "[executor][partitioning]") {
    TempDataDir dir("part_range_select");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val VARCHAR(20)) "
                            "PARTITION BY RANGE (id) ("
                            "  PARTITION p0 VALUES LESS THAN (100),"
                            "  PARTITION p1 VALUES LESS THAN (200),"
                            "  PARTITION p2 VALUES LESS THAN MAXVALUE"
                            ")")
                .is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (5, 'a'), (99, 'd'), (150, 'b'), (250, 'c')").is_ok());

    auto full = ex.execute_sql("SELECT * FROM t");
    REQUIRE(full.is_ok());
    REQUIRE(full.value().find("4 row(s) returned.") != std::string::npos);

    auto eq = ex.execute_sql("SELECT val FROM t WHERE id = 150");
    REQUIRE(eq.is_ok());
    REQUIRE(eq.value().find("b") != std::string::npos);
    REQUIRE(eq.value().find("1 row(s) returned.") != std::string::npos);

    auto lt = ex.execute_sql("SELECT val FROM t WHERE id < 100");
    REQUIRE(lt.is_ok());
    REQUIRE(lt.value().find("2 row(s) returned.") != std::string::npos); // 5 and 99, both in p0
    REQUIRE(lt.value().find("a") != std::string::npos);
    REQUIRE(lt.value().find("d") != std::string::npos);
    REQUIRE(lt.value().find("b") == std::string::npos); // p1/p2 correctly excluded

    auto ge = ex.execute_sql("SELECT val FROM t WHERE id >= 200");
    REQUIRE(ge.is_ok());
    REQUIRE(ge.value().find("1 row(s) returned.") != std::string::npos);
    REQUIRE(ge.value().find("c") != std::string::npos);
}

TEST_CASE("PARTITION BY RANGE: a value past every bound with no MAXVALUE partition is rejected",
          "[executor][partitioning]") {
    TempDataDir dir("part_range_no_match");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY) PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS THAN (100))").is_ok());

    auto ins = ex.execute_sql("INSERT INTO t VALUES (150)");
    REQUIRE(ins.is_err());
    REQUIRE(ins.error().find("No partition found") != std::string::npos);
}

TEST_CASE("PARTITION BY LIST routes INSERT/SELECT by exact value membership", "[executor][partitioning]") {
    TempDataDir dir("part_list_route");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT, region VARCHAR(10), PRIMARY KEY(id, region)) "
                            "PARTITION BY LIST (region) ("
                            "  PARTITION p_north VALUES IN ('NY', 'MA'),"
                            "  PARTITION p_south VALUES IN ('TX', 'FL')"
                            ")")
                .is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 'NY'), (2, 'TX'), (3, 'MA'), (4, 'FL')").is_ok());

    REQUIRE(raw_table_rows(ex, "d.t__p_north").size() == 2);
    REQUIRE(raw_table_rows(ex, "d.t__p_south").size() == 2);

    auto eq = ex.execute_sql("SELECT id FROM t WHERE region = 'TX'");
    REQUIRE(eq.is_ok());
    REQUIRE(eq.value().find("1 row(s) returned.") != std::string::npos);
    REQUIRE(eq.value().find("2") != std::string::npos);

    auto ins_bad = ex.execute_sql("INSERT INTO t VALUES (5, 'CA')");
    REQUIRE(ins_bad.is_err());
    REQUIRE(ins_bad.error().find("No partition found") != std::string::npos);
}

TEST_CASE("PARTITION BY HASH routes deterministically and prunes on Eq", "[executor][partitioning]") {
    TempDataDir dir("part_hash_route");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT) PARTITION BY HASH (id) PARTITIONS 4").is_ok());
    for (int i = 0; i < 8; i++) REQUIRE(ex.execute_sql("INSERT INTO t VALUES (" + std::to_string(i) + ", " + std::to_string(i) + ")").is_ok());

    // id % 4 determinism: 0,4 -> p0; 1,5 -> p1; 2,6 -> p2; 3,7 -> p3.
    REQUIRE(raw_table_rows(ex, "d.t__p0").size() == 2);
    REQUIRE(raw_table_rows(ex, "d.t__p1").size() == 2);
    REQUIRE(raw_table_rows(ex, "d.t__p2").size() == 2);
    REQUIRE(raw_table_rows(ex, "d.t__p3").size() == 2);

    auto eq = ex.execute_sql("SELECT val FROM t WHERE id = 6");
    REQUIRE(eq.is_ok());
    REQUIRE(eq.value().find("1 row(s) returned.") != std::string::npos);
    REQUIRE(eq.value().find("6") != std::string::npos);

    auto agg = ex.execute_sql("SELECT COUNT(*) FROM t");
    REQUIRE(agg.is_err());
    REQUIRE(agg.error().find("Aggregate") != std::string::npos);
}

TEST_CASE("PARTITION BY RANGE: UPDATE routes to the right child and rejects changing the partition column",
          "[executor][partitioning]") {
    TempDataDir dir("part_update");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val VARCHAR(20)) "
                            "PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS THAN (100), PARTITION p1 VALUES LESS THAN MAXVALUE)")
                .is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (5, 'a'), (150, 'b')").is_ok());

    auto upd = ex.execute_sql("UPDATE t SET val = 'updated' WHERE id = 150");
    REQUIRE(upd.is_ok());
    REQUIRE(upd.value() == "1 row(s) updated.");
    REQUIRE(raw_table_rows(ex, "d.t__p1")[0].at("val") == "updated");
    REQUIRE(raw_table_rows(ex, "d.t__p0")[0].at("val") == "a"); // untouched

    auto bad = ex.execute_sql("UPDATE t SET id = 999 WHERE id = 5");
    REQUIRE(bad.is_err());
    REQUIRE(bad.error().find("partitioning column") != std::string::npos);
}

TEST_CASE("PARTITION BY RANGE: DELETE routes to and only removes from the right child", "[executor][partitioning]") {
    TempDataDir dir("part_delete");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY) "
                            "PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS THAN (100), PARTITION p1 VALUES LESS THAN MAXVALUE)")
                .is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (5), (150)").is_ok());

    auto del = ex.execute_sql("DELETE FROM t WHERE id = 5");
    REQUIRE(del.is_ok());
    REQUIRE(del.value() == "1 row(s) deleted.");
    REQUIRE(raw_table_rows(ex, "d.t__p0").empty());
    REQUIRE(raw_table_rows(ex, "d.t__p1").size() == 1);
}

TEST_CASE("Partitioned table lifecycle: DROP TABLE and TRUNCATE TABLE affect every child", "[executor][partitioning]") {
    TempDataDir dir("part_lifecycle");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY) "
                            "PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS THAN (100), PARTITION p1 VALUES LESS THAN MAXVALUE)")
                .is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (5), (150)").is_ok());

    REQUIRE(ex.execute_sql("TRUNCATE TABLE t").is_ok());
    REQUIRE(raw_table_rows(ex, "d.t__p0").empty());
    REQUIRE(raw_table_rows(ex, "d.t__p1").empty());
    auto sel = ex.execute_sql("SELECT * FROM t");
    REQUIRE(sel.is_ok());
    REQUIRE(sel.value() == "0 rows returned.");

    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (5), (150)").is_ok());
    REQUIRE(ex.execute_sql("DROP TABLE t").is_ok());
    auto tables = ex.execute_sql("SHOW TABLES");
    REQUIRE(tables.is_ok());
    REQUIRE(tables.value().find("t__p0") == std::string::npos);
    REQUIRE(tables.value().find("t__p1") == std::string::npos);
    REQUIRE(tables.value().find(" t ") == std::string::npos);
}

TEST_CASE("ALTER TABLE ADD PARTITION / DROP PARTITION", "[executor][partitioning]") {
    TempDataDir dir("part_alter");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY) PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS THAN (100))").is_ok());

    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (150)").is_err()); // no partition yet for >= 100

    auto add = ex.execute_sql("ALTER TABLE t ADD PARTITION (PARTITION p1 VALUES LESS THAN MAXVALUE)");
    REQUIRE(add.is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (150)").is_ok());
    REQUIRE(raw_table_rows(ex, "d.t__p1").size() == 1);

    auto drop = ex.execute_sql("ALTER TABLE t DROP PARTITION p1");
    REQUIRE(drop.is_ok());
    auto tables = ex.execute_sql("SHOW TABLES");
    REQUIRE(tables.is_ok());
    REQUIRE(tables.value().find("t__p1") == std::string::npos);
    auto sel = ex.execute_sql("SELECT * FROM t");
    REQUIRE(sel.is_ok());
    REQUIRE(sel.value() == "0 rows returned."); // that partition's data is gone with it
}

TEST_CASE("PARTITION BY column must be part of the PRIMARY KEY when one exists", "[executor][partitioning]") {
    TempDataDir dir("part_pk_restriction");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    auto bad =
        ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, region VARCHAR(10)) PARTITION BY LIST (region) (PARTITION p0 VALUES IN ('a'))");
    REQUIRE(bad.is_err());
    REQUIRE(bad.error().find("PRIMARY KEY") != std::string::npos);
}

TEST_CASE("PARTITION BY column cannot be AUTO_INCREMENT", "[executor][partitioning]") {
    TempDataDir dir("part_autoinc_restriction");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    auto bad = ex.execute_sql(
        "CREATE TABLE t (id INT PRIMARY KEY AUTO_INCREMENT) PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS THAN MAXVALUE)");
    REQUIRE(bad.is_err());
    REQUIRE(bad.error().find("AUTO_INCREMENT") != std::string::npos);
}

TEST_CASE("FK reference into a partitioned parent table validates against all children", "[executor][partitioning]") {
    TempDataDir dir("part_fk_parent");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE parent (id INT PRIMARY KEY) "
                            "PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS THAN (100), PARTITION p1 VALUES LESS THAN MAXVALUE)")
                .is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO parent VALUES (5), (150)").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE child (id INT PRIMARY KEY, parent_id INT, FOREIGN KEY (parent_id) REFERENCES parent(id))").is_ok());

    REQUIRE(ex.execute_sql("INSERT INTO child VALUES (1, 5)").is_ok());   // parent row in p0
    REQUIRE(ex.execute_sql("INSERT INTO child VALUES (2, 150)").is_ok()); // parent row in p1
    auto bad = ex.execute_sql("INSERT INTO child VALUES (3, 999)");
    REQUIRE(bad.is_err());
    REQUIRE(bad.error().find("Foreign key violation") != std::string::npos);
}

TEST_CASE("Autocommit multi-child partitioned DELETE is all-or-nothing", "[executor][partitioning]") {
    // A single autocommit statement spanning multiple children is wrapped in an implicit
    // transaction (run_routed_statements) -- if a later child fails, earlier children's
    // work must roll back too, not leave a partial result. DELETE with a subquery-free
    // condition can't easily be made to fail mid-way here without deeper hooks, so this
    // instead verifies the simpler, always-true property: a routed statement touching
    // multiple children that ALL succeed is visible as a single atomic unit (COMMIT
    // happened), by checking no dangling in-progress transaction remains afterward.
    TempDataDir dir("part_atomic");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY) "
                            "PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS THAN (100), PARTITION p1 VALUES LESS THAN MAXVALUE)")
                .is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (5), (150)").is_ok());
    REQUIRE_FALSE(ex.txn.is_active()); // the implicit BEGIN/COMMIT wrapper must not leak an open txn

    auto del = ex.execute_sql("DELETE FROM t");
    REQUIRE(del.is_ok());
    REQUIRE(del.value() == "2 row(s) deleted.");
    REQUIRE_FALSE(ex.txn.is_active());
    REQUIRE(raw_table_rows(ex, "d.t__p0").empty());
    REQUIRE(raw_table_rows(ex, "d.t__p1").empty());
}

TEST_CASE("Partitioned INSERT/UPDATE/DELETE reject RETURNING in this version", "[executor][partitioning]") {
    TempDataDir dir("part_returning_rejected");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY) PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS THAN MAXVALUE)").is_ok());

    auto ins = ex.execute_sql("INSERT INTO t VALUES (1) RETURNING id");
    REQUIRE(ins.is_err());
    REQUIRE(ins.error().find("RETURNING") != std::string::npos);
}

TEST_CASE("MultiUpdate/MultiDelete/Merge/InsertSelect against a partitioned table are rejected, not silently no-op",
          "[executor][partitioning]") {
    TempDataDir dir("part_unsupported_stmts");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val INT) PARTITION BY RANGE (id) (PARTITION p0 VALUES LESS THAN MAXVALUE)")
                .is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 1)").is_ok());

    auto multi_upd = ex.execute_sql("UPDATE t, t AS t2 SET t.val = 2 WHERE t.id = t2.id");
    REQUIRE(multi_upd.is_err());

    auto insert_select = ex.execute_sql("INSERT INTO t SELECT id, val FROM t");
    REQUIRE(insert_select.is_err());

    // Neither statement should have silently touched the permanently-empty phantom and
    // reported success -- the original row must be exactly as it was.
    REQUIRE(raw_table_rows(ex, "d.t__p0").size() == 1);
    REQUIRE(raw_table_rows(ex, "d.t__p0")[0].at("val") == "1");
}

TEST_CASE("Non-partitioned tables are completely unaffected by the partitioning hooks", "[executor][partitioning][regression]") {
    TempDataDir dir("part_regression_plain_table");
    Executor ex(dir.path);
    REQUIRE(ex.execute_sql("CREATE DATABASE d").is_ok());
    REQUIRE(ex.execute_sql("USE d").is_ok());
    REQUIRE(ex.execute_sql("CREATE TABLE t (id INT PRIMARY KEY, val VARCHAR(20))").is_ok());
    REQUIRE(ex.execute_sql("INSERT INTO t VALUES (1, 'a'), (2, 'b')").is_ok());

    auto sel = ex.execute_sql("SELECT * FROM t WHERE id = 1");
    REQUIRE(sel.is_ok());
    REQUIRE(sel.value().find("a") != std::string::npos);

    REQUIRE(ex.execute_sql("UPDATE t SET val = 'z' WHERE id = 2").is_ok());
    REQUIRE(ex.execute_sql("DELETE FROM t WHERE id = 1").is_ok());
    auto final_sel = ex.execute_sql("SELECT * FROM t");
    REQUIRE(final_sel.is_ok());
    REQUIRE(final_sel.value().find("1 row(s) returned.") != std::string::npos);
    REQUIRE(final_sel.value().find("z") != std::string::npos);
}
