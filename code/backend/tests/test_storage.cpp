#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "catch.hpp"
#include "engine/storage/buffer_pool.hpp"
#include "engine/storage/composite_index.hpp"
#include "engine/storage/disk.hpp"
#include "engine/storage/hash_index.hpp"

using namespace engine;
namespace fs = std::filesystem;

namespace {
Row make_row(std::initializer_list<std::pair<const char*, const char*>> kvs) {
    Row r;
    for (auto& [k, v] : kvs) r[k] = v;
    return r;
}
} // namespace

TEST_CASE("HashIndex rebuild/get/insert/remove", "[storage][hash_index]") {
    HashIndex idx("employee", "department_id");
    std::vector<Row> rows = {
        make_row({{"id", "1"}, {"department_id", "10"}}),
        make_row({{"id", "2"}, {"department_id", "10"}}),
        make_row({{"id", "3"}, {"department_id", "20"}}),
    };
    idx.rebuild(rows);
    REQUIRE(idx.get("10").size() == 2);
    REQUIRE(idx.get("20").size() == 1);
    REQUIRE(idx.get("30").empty());
    REQUIRE(idx.bucket_count() == 2);
    REQUIRE(idx.row_count() == 3);

    idx.insert_row(make_row({{"id", "4"}, {"department_id", "20"}}));
    REQUIRE(idx.get("20").size() == 2);

    idx.remove_row("10", "id", "1");
    REQUIRE(idx.get("10").size() == 1);
    idx.remove_row("10", "id", "2");
    REQUIRE(idx.get("10").empty()); // bucket removed once empty
    REQUIRE(idx.bucket_count() == 1);
}

// Row-level-concurrency Stage 2: HashIndex gained its own internal mutex_ (same
// reasoning/pattern as BPlusTree) and get() now returns by value instead of a reference
// into data_ (a reference would dangle the instant the lock inside get() is released).
// Each thread here owns a disjoint bucket key, so any wrong/missing/crashed result here
// is necessarily a real data race with another thread's concurrent bucket.
TEST_CASE("HashIndex is safe under real concurrent insert_row/remove_row/get on disjoint buckets", "[storage][hash_index][concurrency]") {
    HashIndex idx("t", "bucket_col");
    constexpr int kThreads = 8;
    constexpr int kPerThread = 300;

    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; t++) {
        threads.emplace_back([&idx, t] {
            std::string bucket_key = "b" + std::to_string(t);
            for (int i = 0; i < kPerThread; i++) {
                idx.insert_row(make_row({{"bucket_col", bucket_key.c_str()}, {"id", std::to_string(i).c_str()}}));
            }
            for (int i = 0; i < kPerThread; i += 2) {
                idx.remove_row(bucket_key, "id", std::to_string(i));
            }
            auto bucket = idx.get(bucket_key);
            if (bucket.size() != static_cast<std::size_t>(kPerThread / 2)) {
                throw std::runtime_error("bucket " + bucket_key + " has wrong size after concurrent ops");
            }
        });
    }
    for (auto& th : threads) th.join();

    REQUIRE(idx.bucket_count() == static_cast<std::size_t>(kThreads));
    REQUIRE(idx.row_count() == static_cast<std::size_t>(kThreads * kPerThread / 2));
}

TEST_CASE("CompositeIndex key construction and search", "[storage][composite_index]") {
    CompositeIndex idx("employee", {"department_id", "salary"});
    Row r1 = make_row({{"department_id", "1"}, {"salary", "50000"}, {"name", "Alice"}});
    Row r2 = make_row({{"department_id", "1"}, {"salary", "60000"}, {"name", "Bob"}});
    idx.insert_row(r1);
    idx.insert_row(r2);

    auto found = idx.search_exact({"1", "50000"});
    REQUIRE(found.has_value());
    REQUIRE(found->find("Alice") != std::string::npos);

    std::unordered_map<std::string, std::string> eq_map = {{"department_id", "1"}, {"salary", "60000"}};
    REQUIRE(idx.matches_conditions(eq_map));
    auto found2 = idx.search_from_eq_map(eq_map);
    REQUIRE(found2.has_value());
    REQUIRE(found2->find("Bob") != std::string::npos);

    std::unordered_map<std::string, std::string> prefix_map = {{"department_id", "1"}};
    auto prefix_key = idx.prefix_key_from_eq_map(prefix_map);
    REQUIRE(prefix_key.has_value());
    auto matches = idx.prefix_scan(*prefix_key);
    REQUIRE(matches.size() == 2);
}

TEST_CASE("BufferPool LRU hit/miss/eviction", "[storage][buffer_pool]") {
    fs::remove_all("bp_test_data");
    DiskManager disk("bp_test_data");
    BufferPool pool(2); // capacity 2, to force eviction

    disk.save_table("t1", {make_row({{"id", "1"}})});
    disk.save_table("t2", {make_row({{"id", "2"}})});
    disk.save_table("t3", {make_row({{"id", "3"}})});

    pool.get_page("t1", disk); // miss
    pool.get_page("t2", disk); // miss
    REQUIRE(pool.miss_count == 2);
    REQUIRE(pool.usage() == 2);

    pool.get_page("t1", disk); // hit, t1 becomes more recently used than t2
    REQUIRE(pool.hit_count == 1);

    pool.get_page("t3", disk); // miss, evicts t2 (least recently used)
    REQUIRE(pool.miss_count == 3);
    REQUIRE(pool.usage() == 2);

    pool.write_page("t3", {make_row({{"id", "3"}, {"extra", "x"}})});
    pool.flush_all(disk);
    auto reloaded = disk.load_table("t3");
    REQUIRE(reloaded.size() == 1);
    REQUIRE(reloaded[0].at("extra") == "x");

    fs::remove_all("bp_test_data");
}

TEST_CASE("DiskManager schema/table/index round trip", "[storage][disk]") {
    fs::remove_all("disk_test_data");
    DiskManager disk("disk_test_data");

    ColumnDef id_col;
    id_col.name = "id";
    id_col.data_type = DataType(DataType::Int{});
    id_col.primary_key = true;

    TableSchema schema;
    schema.name = "employee";
    schema.columns = {id_col};
    schema.primary_key_columns = {"id"};

    disk.save_schema("company.employee", schema);
    auto loaded_schema = disk.load_schema("company.employee");
    REQUIRE(loaded_schema.has_value());
    REQUIRE(loaded_schema->name == "employee");
    REQUIRE(loaded_schema->columns.size() == 1);

    std::vector<Row> rows = {
        make_row({{"id", "1"}, {"name", "Alice"}}),
        make_row({{"id", "2"}, {"name", "Bob;with;semicolons"}}),
    };
    disk.save_table("company.employee", rows);
    auto loaded_rows = disk.load_table("company.employee");
    REQUIRE(loaded_rows.size() == 2);

    auto tables = disk.list_tables();
    REQUIRE(std::find(tables.begin(), tables.end(), "company.employee") != tables.end());

    auto dbs = disk.list_databases();
    REQUIRE(std::find(dbs.begin(), dbs.end(), "company") != dbs.end());

    BPlusTree tree;
    tree.insert("1", "{\"id\":\"1\"}");
    tree.insert("2", "{\"id\":\"2\"}");
    disk.save_btree_index("company.employee", tree);
    auto loaded_tree = disk.load_btree_index("company.employee");
    REQUIRE(loaded_tree.has_value());
    REQUIRE(loaded_tree->search("1").has_value());

    std::vector<IndexMeta> metas = {IndexMeta{"idx_name", "employee", {"name"}, "btree"}};
    disk.save_index_meta("company", metas);
    auto loaded_metas = disk.load_index_meta("company");
    REQUIRE(loaded_metas.size() == 1);
    REQUIRE(loaded_metas[0].name == "idx_name");

    disk.delete_table("company.employee");
    auto after_delete = disk.load_table("company.employee");
    REQUIRE(after_delete.empty());

    fs::remove_all("disk_test_data");
}

TEST_CASE("DiskManager save_table replaces an existing file atomically, leaving no .tmp behind", "[storage][disk][regression]") {
    // Regression: save_table used to open its destination with truncate(true) and write
    // in place, with no fsync -- a crash between the truncate and the write completing
    // left the file corrupt with the old contents already gone, and even a clean write
    // was never durably synced to disk (PLAN.md, section C). The fix writes to a sibling
    // .tmp file, fsyncs it, then renames it over the destination; this checks the
    // happy-path result of that (overwrite still works, no leftover .tmp file) since a
    // real crash mid-write isn't something a unit test can inject.
    fs::remove_all("disk_test_data_atomic");
    DiskManager disk("disk_test_data_atomic");

    disk.save_table("company.t", {make_row({{"id", "1"}, {"val", "first"}})});
    auto first = disk.load_table("company.t");
    REQUIRE(first.size() == 1);
    REQUIRE(first[0].at("val") == "first");

    // Overwrite: exercises the rename-over-an-existing-file path, not just create-new.
    disk.save_table("company.t", {make_row({{"id", "1"}, {"val", "second"}}), make_row({{"id", "2"}, {"val", "third"}})});
    auto second = disk.load_table("company.t");
    REQUIRE(second.size() == 2);

    bool found_tmp = false;
    for (auto& entry : fs::recursive_directory_iterator("disk_test_data_atomic")) {
        if (entry.path().extension() == ".tmp") found_tmp = true;
    }
    REQUIRE_FALSE(found_tmp);

    fs::remove_all("disk_test_data_atomic");
}

TEST_CASE("DiskManager views/view_raw_sql round trip", "[storage][disk]") {
    fs::remove_all("disk_test_data2");
    DiskManager disk("disk_test_data2");

    std::unordered_map<std::string, Statement> views;
    views["company.v_emp"] = Statement::Select{
        "company.employee", std::nullopt, {SelectColumn(SelectColumn::All{})}, false, std::nullopt, {}, {},
        std::nullopt, std::nullopt, std::nullopt, std::nullopt, false, false};
    disk.save_views("company", views);
    auto loaded_views = disk.load_views("company");
    REQUIRE(loaded_views.size() == 1);
    REQUIRE(std::holds_alternative<Statement::Select>(loaded_views.at("company.v_emp").data));

    std::unordered_map<std::string, std::string> view_sql = {{"company.v_emp", "CREATE VIEW v_emp AS SELECT * FROM employee"}};
    disk.save_view_raw_sql("company", view_sql);
    auto loaded_sql = disk.load_view_raw_sql("company");
    REQUIRE(loaded_sql.at("company.v_emp") == "CREATE VIEW v_emp AS SELECT * FROM employee");

    fs::remove_all("disk_test_data2");
}

namespace {
struct DummySysRecord {
    std::string name;
    int value = 0;
};
void to_json(nlohmann::json& j, const DummySysRecord& r) { j = nlohmann::json{{"name", r.name}, {"value", r.value}}; }
void from_json(const nlohmann::json& j, DummySysRecord& r) { j.at("name").get_to(r.name); j.at("value").get_to(r.value); }
} // namespace

TEST_CASE("DiskManager generic sys-json save/load round trip and legacy-path migration", "[storage][disk]") {
    fs::remove_all("disk_test_data3");
    DiskManager disk("disk_test_data3");

    std::vector<DummySysRecord> users = {{"root", 1}, {"alice", 2}};
    disk.save_users(users);
    auto loaded = disk.load_users<std::vector<DummySysRecord>>();
    REQUIRE(loaded.size() == 2);
    REQUIRE(loaded[0].name == "root");
    REQUIRE(loaded[1].value == 2);

    // Missing file -> default-constructed T (empty vector), matching serde's #[derive(Default)] fallback.
    auto missing = disk.load_grants<std::vector<DummySysRecord>>();
    REQUIRE(missing.empty());

    // Legacy root-level file migrates into _system/ on first load.
    std::ofstream legacy("disk_test_data3/_roles_legacy.json");
    legacy << nlohmann::json(std::vector<DummySysRecord>{{"legacy", 9}}).dump();
    legacy.close();
    fs::rename("disk_test_data3/_roles_legacy.json", "disk_test_data3/_roles.json");
    auto migrated = disk.load_roles<std::vector<DummySysRecord>>();
    REQUIRE(migrated.size() == 1);
    REQUIRE(migrated[0].name == "legacy");
    REQUIRE(fs::exists("disk_test_data3/_system/_roles.json"));
    REQUIRE_FALSE(fs::exists("disk_test_data3/_roles.json"));

    fs::remove_all("disk_test_data3");
}
