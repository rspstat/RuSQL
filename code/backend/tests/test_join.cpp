#include "catch.hpp"
#include <set>

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

TEST_CASE("the join algorithms keep integers beyond 2^53 apart", "[join][typed_comparison]") {
    // as doubles 2^53 + 1 is 2^53: a hash or a sort that goes through doubles joins rows whose keys are different numbers
    std::vector<Row> left = {row({{"id", "1"}, {"k", "9007199254740992"}}), row({{"id", "2"}, {"k", "9007199254740993"}}),
                             row({{"id", "3"}, {"k", "9007199254740994"}}), row({{"id", "4"}, {"k", "5"}})};
    std::vector<Row> right = {row({{"rid", "9007199254740993"}, {"name", "c"}}), row({{"rid", "9007199254740992"}, {"name", "b"}}), row({{"rid", "5"}, {"name", "e"}})};
    auto pairs = [](const std::vector<Row>& out) {
        std::multiset<std::string> p;
        for (auto& r : out) p.insert(r.at("id") + ":" + r.at("name"));
        return p;
    };
    const std::multiset<std::string> expected = {"1:b", "2:c", "4:e"};
    REQUIRE(pairs(hash_join(left, right, JoinType::Inner, "r", "k", "rid", {"rid", "name"})) == expected);
    REQUIRE(pairs(sort_merge_join(left, right, JoinType::Inner, "r", "k", "rid", {"rid", "name"})) == expected);
}

TEST_CASE("hashed_join_verified hashes text keys by their text and number keys by value", "[join][typed_comparison]") {
    std::vector<Row> left = {row({{"id", "1"}, {"k", "007"}}), row({{"id", "2"}, {"k", "7"}}), row({{"id", "3"}, {"k", "9007199254740993"}}),
                             row({{"id", "4"}, {"k", "NULL"}})};
    std::vector<Row> right = {row({{"rid", "a"}, {"k", "7"}}), row({{"rid", "b"}, {"k", "9007199254740992"}}), row({{"rid", "c"}, {"k", "7.0"}})};
    auto key = [](const Row& r) -> const std::string* { return &r.at("k"); };
    auto joined = [&](bool exact) {
        auto out = hashed_join_verified(left, right, "r", JoinType::Inner, key, key, {"rid", "k"}, [](const Row&) { return true; }, nullptr, exact);
        REQUIRE(out.has_value());
        std::multiset<std::string> p;
        for (auto& r : *out) p.insert(r.at("id") + ":" + r.at("rid"));
        return p;
    };
    // numbers: 007 = 7 = 7.0, 2^53 + 1 is not 2^53, NULL matches nothing
    REQUIRE(joined(false) == std::multiset<std::string>{"1:a", "1:c", "2:a", "2:c"});
    // texts: the texts are equal or they are not
    REQUIRE(joined(true) == std::multiset<std::string>{"2:a"});
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
