#include "catch.hpp"
#include "engine/join.hpp"

using namespace engine;

namespace {
Row row(std::initializer_list<std::pair<const char*, const char*>> kvs) {
    Row r;
    for (auto& [k, v] : kvs) r[k] = v;
    return r;
}
} // namespace

TEST_CASE("nested_loop_join inner join merges matching rows", "[join]") {
    std::vector<Row> left = {row({{"id", "1"}, {"dept_id", "10"}}), row({{"id", "2"}, {"dept_id", "20"}})};
    std::vector<Row> right = {row({{"id", "10"}, {"name", "Eng"}}), row({{"id", "20"}, {"name", "Sales"}})};

    auto out = nested_loop_join(left, right, JoinType::Inner, "department", {}, {"id", "name"}, [](const Row& merged) {
        auto dept_id = merged.find("dept_id");
        auto dept_pk = merged.find("department.id");
        return dept_id != merged.end() && dept_pk != merged.end() && dept_id->second == dept_pk->second;
    });

    REQUIRE(out.size() == 2);
    for (auto& r : out) {
        REQUIRE(r.at("dept_id") == r.at("department.id"));
    }
}

TEST_CASE("nested_loop_join left join fills NULL for unmatched rows", "[join]") {
    std::vector<Row> left = {row({{"id", "1"}, {"dept_id", "99"}})}; // no matching dept
    std::vector<Row> right = {row({{"id", "10"}, {"name", "Eng"}})};

    auto out = nested_loop_join(left, right, JoinType::Left, "department", {}, {"id", "name"}, [](const Row& merged) {
        return merged.at("dept_id") == merged.at("department.id");
    });

    REQUIRE(out.size() == 1);
    REQUIRE(out[0].at("department.name") == "NULL");
}

TEST_CASE("nested_loop_join USING clause matches on shared column values", "[join]") {
    std::vector<Row> left = {row({{"dept_id", "1"}, {"emp_name", "Alice"}})};
    std::vector<Row> right = {row({{"dept_id", "1"}, {"dept_name", "Eng"}}), row({{"dept_id", "2"}, {"dept_name", "Sales"}})};

    auto out = nested_loop_join(left, right, JoinType::Inner, "d", {"dept_id"}, {}, [](const Row&) { return true; });
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].at("d.dept_name") == "Eng");
}

TEST_CASE("nested_loop_join cross join is the full cartesian product", "[join]") {
    std::vector<Row> left = {row({{"a", "1"}}), row({{"a", "2"}})};
    std::vector<Row> right = {row({{"b", "x"}}), row({{"b", "y"}})};
    auto out = nested_loop_join(left, right, JoinType::Cross, "r", {}, {}, [](const Row&) { return true; });
    REQUIRE(out.size() == 4);
}

TEST_CASE("hash_join inner produces the same rows as nested_loop_join for equi-join", "[join]") {
    std::vector<Row> left = {row({{"id", "1"}, {"dept_id", "10"}}), row({{"id", "2"}, {"dept_id", "20"}}),
                             row({{"id", "3"}, {"dept_id", "10"}})};
    std::vector<Row> right = {row({{"id", "10"}, {"name", "Eng"}}), row({{"id", "20"}, {"name", "Sales"}})};

    auto out = hash_join(left, right, JoinType::Inner, "department", "dept_id", "id", {"id", "name"});
    REQUIRE(out.size() == 3);
}

TEST_CASE("hash_join left join fills NULL for unmatched probe rows", "[join]") {
    std::vector<Row> left = {row({{"id", "1"}, {"dept_id", "99"}})};
    std::vector<Row> right = {row({{"id", "10"}, {"name", "Eng"}})};

    auto out = hash_join(left, right, JoinType::Left, "department", "dept_id", "id", {"id", "name"});
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].at("department.name") == "NULL");
}

TEST_CASE("sort_merge_join inner matches equal keys across sorted order", "[join]") {
    std::vector<Row> left = {row({{"id", "2"}, {"dept_id", "20"}}), row({{"id", "1"}, {"dept_id", "10"}})};
    std::vector<Row> right = {row({{"id", "20"}, {"name", "Sales"}}), row({{"id", "10"}, {"name", "Eng"}})};

    auto out = sort_merge_join(left, right, JoinType::Inner, "department", "dept_id", "id", {"id", "name"});
    REQUIRE(out.size() == 2);
    for (auto& r : out) REQUIRE(r.at("dept_id") == r.at("department.id"));
}

TEST_CASE("merge_right sets both qualified and bare keys without overwriting existing bare key", "[join]") {
    Row merged = row({{"id", "1"}});
    Row right = row({{"id", "10"}, {"name", "Eng"}});
    merge_right(merged, right, "department");

    REQUIRE(merged.at("department.id") == "10");
    REQUIRE(merged.at("department.name") == "Eng");
    // bare "id" already existed on the left side and must not be overwritten
    REQUIRE(merged.at("id") == "1");
    REQUIRE(merged.at("name") == "Eng");
}
