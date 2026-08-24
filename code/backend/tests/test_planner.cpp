#include "catch.hpp"
#include "engine/planner.hpp"

using namespace engine;

namespace {
Row row(std::initializer_list<std::pair<const char*, const char*>> kvs) {
    Row r;
    for (auto& [k, v] : kvs) r[k] = v;
    return r;
}

CondExpr eq_cond(const std::string& col, const std::string& val) {
    return CondExpr(CondExpr::Leaf{
        Condition{ArithExpr(ArithExpr::Col{col}), Operator::Eq, ConditionValue(ConditionValue::Literal{val})}});
}
} // namespace

TEST_CASE("Planner chooses SeqScan when there is no condition", "[planner]") {
    std::unordered_map<std::string, std::vector<Row>> tables = {{"employee", {row({{"id", "1"}})}}};
    std::unordered_map<std::string, BPlusTree> indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> index_meta;
    std::unordered_map<std::string, CompositeIndex> composite_indexes;
    std::unordered_map<std::string, HashIndex> hash_indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> hash_index_meta;
    Catalog catalog;
    std::unordered_map<std::string, TableStats> stats;

    Planner planner(tables, indexes, index_meta, composite_indexes, hash_indexes, hash_index_meta, catalog, stats);
    AccessPath access = planner.choose_access("employee", std::nullopt, std::nullopt);
    REQUIRE(std::holds_alternative<AccessPath::SeqScan>(access.data));
}

TEST_CASE("Planner chooses PkPoint for an equality condition on the primary key", "[planner]") {
    std::unordered_map<std::string, std::vector<Row>> tables = {{"employee", {row({{"id", "1"}})}}};
    std::unordered_map<std::string, BPlusTree> indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> index_meta;
    std::unordered_map<std::string, CompositeIndex> composite_indexes;
    std::unordered_map<std::string, HashIndex> hash_indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> hash_index_meta;

    Catalog catalog;
    ColumnDef id_col;
    id_col.name = "id";
    id_col.primary_key = true;
    catalog.create_table("employee", {id_col});

    std::unordered_map<std::string, TableStats> stats;
    Planner planner(tables, indexes, index_meta, composite_indexes, hash_indexes, hash_index_meta, catalog, stats);

    auto cond = eq_cond("id", "1");
    AccessPath access = planner.choose_access("employee", cond, std::string("id"));
    REQUIRE(std::holds_alternative<AccessPath::PkPoint>(access.data));
    REQUIRE(std::get<AccessPath::PkPoint>(access.data).key == "1");
}

TEST_CASE("Planner chooses HashPoint when a hash index exists on the column", "[planner]") {
    std::unordered_map<std::string, std::vector<Row>> tables = {{"employee", {row({{"email", "a@b.com"}})}}};
    std::unordered_map<std::string, BPlusTree> indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> index_meta;
    std::unordered_map<std::string, CompositeIndex> composite_indexes;
    std::unordered_map<std::string, HashIndex> hash_indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> hash_index_meta = {
        {"idx_email", {"employee", "email"}}};

    Catalog catalog;
    ColumnDef email_col;
    email_col.name = "email";
    catalog.create_table("employee", {email_col});

    std::unordered_map<std::string, TableStats> stats;
    Planner planner(tables, indexes, index_meta, composite_indexes, hash_indexes, hash_index_meta, catalog, stats);

    auto cond = eq_cond("email", "a@b.com");
    AccessPath access = planner.choose_access("employee", cond, std::nullopt);
    REQUIRE(std::holds_alternative<AccessPath::HashPoint>(access.data));
}

TEST_CASE("Planner estimate_cost: SeqScan scales with table size, PkPoint is near-constant", "[planner]") {
    std::unordered_map<std::string, std::vector<Row>> tables;
    std::unordered_map<std::string, BPlusTree> indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> index_meta;
    std::unordered_map<std::string, CompositeIndex> composite_indexes;
    std::unordered_map<std::string, HashIndex> hash_indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> hash_index_meta;
    Catalog catalog;
    std::unordered_map<std::string, TableStats> stats;
    Planner planner(tables, indexes, index_meta, composite_indexes, hash_indexes, hash_index_meta, catalog, stats);

    double seq_cost_small = planner.estimate_cost(10, AccessPath(AccessPath::SeqScan{}));
    double seq_cost_large = planner.estimate_cost(10000, AccessPath(AccessPath::SeqScan{}));
    REQUIRE(seq_cost_large > seq_cost_small);

    double pk_cost_large = planner.estimate_cost(10000, AccessPath(AccessPath::PkPoint{"5"}));
    REQUIRE(pk_cost_large < seq_cost_large);
}

TEST_CASE("Planner explain() produces a non-empty formatted plan", "[planner]") {
    std::unordered_map<std::string, std::vector<Row>> tables = {
        {"employee", {row({{"id", "1"}, {"_xmax", "0"}}), row({{"id", "2"}, {"_xmax", "0"}})}}};
    std::unordered_map<std::string, BPlusTree> indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> index_meta;
    std::unordered_map<std::string, CompositeIndex> composite_indexes;
    std::unordered_map<std::string, HashIndex> hash_indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> hash_index_meta;
    Catalog catalog;
    std::unordered_map<std::string, TableStats> stats;
    Planner planner(tables, indexes, index_meta, composite_indexes, hash_indexes, hash_index_meta, catalog, stats);

    SelectPlan plan = planner.plan("employee", std::nullopt, {});
    std::string explained = planner.explain(plan);
    REQUIRE(explained.find("QUERY PLAN") != std::string::npos);
    REQUIRE(explained.find("Seq Scan") != std::string::npos);
}

TEST_CASE("collect_eq_map extracts equality literals from an AND chain", "[planner]") {
    CondExpr expr = CondExpr(CondExpr::And{
        std::make_unique<CondExpr>(eq_cond("department_id", "1")),
        std::make_unique<CondExpr>(eq_cond("status", "active"))});
    auto map = collect_eq_map(expr);
    REQUIRE(map.at("department_id") == "1");
    REQUIRE(map.at("status") == "active");
}

TEST_CASE("Multi-join algorithm selection reflects accumulated cardinality, not the base table's row count",
          "[planner]") {
    // a has 1000 rows, but the first join (a JOIN b) is very selective (b has only 10 rows;
    // with no NDV stats available, the equi-join row estimate falls back to
    // min(left_rows, right_rows) = 10) -- so by the time the SECOND join (against c, 4 rows)
    // picks its algorithm, the true left-hand cardinality flowing into it is ~10, not a's
    // original 1000. If the planner still used a's base cardinality here, nl_cost=1000*4=4000
    // would exceed hash_cost=(1000+4)*3=3012 and NestedLoop would NOT be chosen; using the
    // correct accumulated ~10, nl_cost=10*4=40 stays under hash_cost=(10+4)*3=42, so
    // NestedLoop IS chosen. Regression guard for PLAN.md's "다중 조인 알고리즘 선택이 누적
    // 카디널리티 미반영".
    std::vector<Row> a_rows, b_rows, c_rows;
    for (int i = 0; i < 1000; i++) a_rows.push_back(row({{"id", "1"}}));
    for (int i = 0; i < 10; i++) b_rows.push_back(row({{"id", "1"}}));
    for (int i = 0; i < 4; i++) c_rows.push_back(row({{"id", "1"}}));
    std::unordered_map<std::string, std::vector<Row>> tables = {{"a", a_rows}, {"b", b_rows}, {"c", c_rows}};
    std::unordered_map<std::string, BPlusTree> indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> index_meta;
    std::unordered_map<std::string, CompositeIndex> composite_indexes;
    std::unordered_map<std::string, HashIndex> hash_indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> hash_index_meta;
    Catalog catalog;
    std::unordered_map<std::string, TableStats> stats; // NDV 통계 없음 -> min(left,right) 폴백 사용
    Planner planner(tables, indexes, index_meta, composite_indexes, hash_indexes, hash_index_meta, catalog, stats);

    auto ab_cond = CondExpr(CondExpr::Leaf{
        Condition{ArithExpr(ArithExpr::Col{"a.x"}), Operator::Eq, ConditionValue(ConditionValue::Literal{"b.x"})}});
    auto bc_cond = CondExpr(CondExpr::Leaf{
        Condition{ArithExpr(ArithExpr::Col{"b.y"}), Operator::Eq, ConditionValue(ConditionValue::Literal{"c.y"})}});
    std::vector<Join> joins;
    joins.push_back(Join{"b", ab_cond, JoinType::Inner, {}});
    joins.push_back(Join{"c", bc_cond, JoinType::Inner, {}});

    SelectPlan plan = planner.plan("a", std::nullopt, joins);
    REQUIRE(plan.joins.size() == 2);
    REQUIRE(plan.joins[0].est_rows == 10); // min(1000, 10)
    REQUIRE(std::holds_alternative<JoinAlgo::NestedLoop>(plan.joins[1].algo.data));
}

TEST_CASE("reorder_joins_greedy puts the smallest joinable table first", "[planner]") {
    std::unordered_map<std::string, std::vector<Row>> tables = {
        {"big", std::vector<Row>(100)},
        {"small", std::vector<Row>(2)},
    };
    Join j_big{"big", eq_cond("employee.dept_id", "big.id"), JoinType::Inner, {}};
    Join j_small{"small", eq_cond("employee.id", "small.emp_id"), JoinType::Inner, {}};

    auto reordered = reorder_joins_greedy("employee", {j_big, j_small}, tables);
    REQUIRE(reordered.size() == 2);
    REQUIRE(reordered[0].table == "small");
}
