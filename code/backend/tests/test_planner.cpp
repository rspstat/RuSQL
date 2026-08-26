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

TEST_CASE("Planner chooses ReverseIndexNL via a secondary index when the base table is far larger than the joined table",
          "[planner]") {
    // Mirrors the recursive-CTE shape that motivated this: a large static table (`chain`)
    // joined against a tiny table (`delta`) on chain's own non-PK, secondary-indexed
    // column. Forward IndexNL (probe delta's PK) is also technically available here since
    // delta.id is delta's PK, but iterating a large left/base table and doing a cheap
    // probe into the tiny right table is still O(left_size) -- ReverseIndexNL (iterate the
    // tiny right table, probe the large left table's secondary index) should win instead.
    std::vector<Row> chain_rows(900), delta_rows(2);
    std::unordered_map<std::string, std::vector<Row>> tables = {{"chain", chain_rows}, {"delta", delta_rows}};
    std::unordered_map<std::string, BPlusTree> indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> index_meta = {{"idx_parent", {"chain", "parent_id"}}};
    std::unordered_map<std::string, CompositeIndex> composite_indexes;
    std::unordered_map<std::string, HashIndex> hash_indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> hash_index_meta;

    Catalog catalog;
    ColumnDef chain_id;
    chain_id.name = "id";
    chain_id.primary_key = true;
    catalog.create_table("chain", {chain_id});
    ColumnDef delta_id;
    delta_id.name = "id";
    delta_id.primary_key = true;
    catalog.create_table("delta", {delta_id});

    std::unordered_map<std::string, TableStats> stats;
    Planner planner(tables, indexes, index_meta, composite_indexes, hash_indexes, hash_index_meta, catalog, stats);

    auto cond = CondExpr(CondExpr::Leaf{
        Condition{ArithExpr(ArithExpr::Col{"chain.parent_id"}), Operator::Eq, ConditionValue(ConditionValue::Literal{"delta.id"})}});
    std::vector<Join> joins;
    joins.push_back(Join{"delta", cond, JoinType::Inner, {}});

    SelectPlan plan = planner.plan("chain", std::nullopt, joins);
    REQUIRE(plan.joins.size() == 1);
    REQUIRE(std::holds_alternative<JoinAlgo::ReverseIndexNL>(plan.joins[0].algo.data));
    auto& rev = std::get<JoinAlgo::ReverseIndexNL>(plan.joins[0].algo.data);
    REQUIRE(rev.right_extract_col == "id");
    REQUIRE(rev.left_index_key == "idx_parent");
    REQUIRE(rev.left_is_secondary_btree);
    REQUIRE_FALSE(rev.left_is_hash);
}

TEST_CASE("ReverseIndexNL is never chosen for the 2nd+ join in a chain, even when the size shape would favor it",
          "[planner]") {
    std::vector<Row> a_rows(3), chain_rows(900), delta_rows(2);
    std::unordered_map<std::string, std::vector<Row>> tables = {{"a", a_rows}, {"chain", chain_rows}, {"delta", delta_rows}};
    std::unordered_map<std::string, BPlusTree> indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> index_meta = {{"idx_parent", {"chain", "parent_id"}}};
    std::unordered_map<std::string, CompositeIndex> composite_indexes;
    std::unordered_map<std::string, HashIndex> hash_indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> hash_index_meta;

    Catalog catalog;
    ColumnDef chain_id;
    chain_id.name = "id";
    chain_id.primary_key = true;
    catalog.create_table("chain", {chain_id});
    ColumnDef delta_id;
    delta_id.name = "id";
    delta_id.primary_key = true;
    catalog.create_table("delta", {delta_id});

    std::unordered_map<std::string, TableStats> stats;
    Planner planner(tables, indexes, index_meta, composite_indexes, hash_indexes, hash_index_meta, catalog, stats);

    auto a_chain_cond = CondExpr(CondExpr::Leaf{
        Condition{ArithExpr(ArithExpr::Col{"a.chain_id"}), Operator::Eq, ConditionValue(ConditionValue::Literal{"chain.id"})}});
    auto chain_delta_cond = CondExpr(CondExpr::Leaf{
        Condition{ArithExpr(ArithExpr::Col{"chain.parent_id"}), Operator::Eq, ConditionValue(ConditionValue::Literal{"delta.id"})}});
    std::vector<Join> joins;
    joins.push_back(Join{"chain", a_chain_cond, JoinType::Inner, {}});
    joins.push_back(Join{"delta", chain_delta_cond, JoinType::Inner, {}});

    SelectPlan plan = planner.plan("a", std::nullopt, joins);
    REQUIRE(plan.joins.size() == 2);
    REQUIRE_FALSE(std::holds_alternative<JoinAlgo::ReverseIndexNL>(plan.joins[1].algo.data));
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

TEST_CASE("estimate_rows uses the exact MCV count for a skewed equality lookup instead of the plain NDV average",
          "[planner]") {
    std::unordered_map<std::string, std::vector<Row>> tables;
    std::unordered_map<std::string, BPlusTree> indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> index_meta;
    std::unordered_map<std::string, CompositeIndex> composite_indexes;
    std::unordered_map<std::string, HashIndex> hash_indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> hash_index_meta;
    Catalog catalog;

    ColumnStats status_stats;
    status_stats.distinct_count = 5;
    status_stats.mcv = {{"active", 900}};
    TableStats emp_stats;
    emp_stats.total_rows = 1000;
    emp_stats.columns["status"] = status_stats;
    std::unordered_map<std::string, TableStats> stats = {{"employee", emp_stats}};

    Planner planner(tables, indexes, index_meta, composite_indexes, hash_indexes, hash_index_meta, catalog, stats);

    AccessPath mcv_hit(AccessPath::SecondaryPoint{"employee", "status", "active"});
    REQUIRE(planner.estimate_rows(1000, mcv_hit, "employee") == 900);

    // Non-MCV value: (total - mcv_rows) / (distinct_count - mcv.size()) = (1000-900)/(5-1) = 25,
    // not the plain NDV average (1000/5 = 200) which would ignore that "active" already
    // accounts for 900 of the 1000 rows.
    AccessPath mcv_miss(AccessPath::SecondaryPoint{"employee", "status", "inactive"});
    REQUIRE(planner.estimate_rows(1000, mcv_miss, "employee") == 25);
}
