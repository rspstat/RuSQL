#include "catch.hpp"
#include "engine/planner.hpp"

using namespace engine;

namespace {
Row row(std::initializer_list<std::pair<const char*, const char*>> kvs) {
    Row r;
    for (auto& [k, v] : kvs) r[k] = v;
    return r;
}

CondExpr eq_cond(const std::string& col, const std::string& val, bool quoted = false) {
    return CondExpr(CondExpr::Leaf{
        Condition{ArithExpr(ArithExpr::Col{col}), Operator::Eq, ConditionValue(ConditionValue::Literal{val, quoted})}});
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
    email_col.data_type = DataType(DataType::Varchar{30});
    catalog.create_table("employee", {email_col});

    std::unordered_map<std::string, TableStats> stats;
    Planner planner(tables, indexes, index_meta, composite_indexes, hash_indexes, hash_index_meta, catalog, stats);

    auto cond = eq_cond("email", "a@b.com", true);
    AccessPath access = planner.choose_access("employee", cond, std::nullopt);
    REQUIRE(std::holds_alternative<AccessPath::HashPoint>(access.data));
    // a text column compared with a number is read by the number each text starts with: no index of texts answers that
    AccessPath by_number = planner.choose_access("employee", eq_cond("email", "7"), std::nullopt);
    REQUIRE(std::holds_alternative<AccessPath::SeqScan>(by_number.data));
}

TEST_CASE("Planner uses an index of a column whose type it does not know, for a string and for a number", "[planner]") {
    std::unordered_map<std::string, std::vector<Row>> tables = {{"employee", {row({{"email", "a@b.com"}})}}};
    std::unordered_map<std::string, BPlusTree> indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> index_meta;
    std::unordered_map<std::string, CompositeIndex> composite_indexes;
    std::unordered_map<std::string, HashIndex> hash_indexes;
    std::unordered_map<std::string, std::pair<std::string, std::string>> hash_index_meta = {{"idx_email", {"employee", "email"}}};

    Catalog catalog;
    ColumnDef email_col;
    email_col.name = "email";
    email_col.data_type = DataType(DataType::Unknown{}); // (a schema written by a build that knew a type this one does not)
    catalog.create_table("employee", {email_col});

    std::unordered_map<std::string, TableStats> stats;
    Planner planner(tables, indexes, index_meta, composite_indexes, hash_indexes, hash_index_meta, catalog, stats);
    REQUIRE(std::holds_alternative<AccessPath::HashPoint>(planner.choose_access("employee", eq_cond("email", "a@b.com", true), std::nullopt).data));
    REQUIRE(std::holds_alternative<AccessPath::HashPoint>(planner.choose_access("employee", eq_cond("email", "7"), std::nullopt).data));
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

TEST_CASE("ReverseIndexNL counts the rows a probe of a non-unique index returns", "[planner]") {
    // `fact.dim_id` (a foreign key, indexed, not unique) joined with `dim`: iterating dim and probing the index returns EVERY
    // fact row of each key and copies each one -- measured 2.5x a hash join for 100,000 rows. The distinct count of the indexed
    // column (ANALYZE) says how many rows a probe returns; without statistics the planner assumes one, as everywhere else.
    auto algo_for = [](std::size_t fact_rows, std::size_t dim_rows, std::optional<std::size_t> ndv, bool probe_the_pk) {
        std::unordered_map<std::string, std::vector<Row>> tables = {{"fact", std::vector<Row>(fact_rows)}, {"dim", std::vector<Row>(dim_rows)}};
        std::unordered_map<std::string, BPlusTree> indexes;
        std::unordered_map<std::string, std::pair<std::string, std::string>> index_meta;
        if (!probe_the_pk) index_meta = {{"idx_fk", {"fact", "dim_id"}}};
        std::unordered_map<std::string, CompositeIndex> composite_indexes;
        std::unordered_map<std::string, HashIndex> hash_indexes;
        std::unordered_map<std::string, std::pair<std::string, std::string>> hash_index_meta;
        Catalog catalog;
        ColumnDef id;
        id.name = "id";
        id.primary_key = true;
        catalog.create_table("fact", {id});
        catalog.create_table("dim", {id});
        std::unordered_map<std::string, TableStats> stats;
        const std::string probe_col = probe_the_pk ? "fact.id" : "fact.dim_id";
        if (ndv) {
            stats["fact"].total_rows = fact_rows;
            stats["fact"].columns[probe_the_pk ? "id" : "dim_id"].distinct_count = *ndv;
        }
        Planner planner(tables, indexes, index_meta, composite_indexes, hash_indexes, hash_index_meta, catalog, stats);
        auto cond = CondExpr(CondExpr::Leaf{Condition{ArithExpr(ArithExpr::Col{probe_col}), Operator::Eq,
                                                      ConditionValue(ConditionValue::Literal{probe_the_pk ? "dim.fid" : "dim.id"})}});
        std::vector<Join> joins;
        joins.push_back(Join{"dim", cond, JoinType::Inner, {}});
        SelectPlan plan = planner.plan("fact", std::nullopt, joins);
        REQUIRE(plan.joins.size() == 1);
        return plan.joins[0].algo;
    };
    auto is_reverse = [](const JoinAlgo& a) { return std::holds_alternative<JoinAlgo::ReverseIndexNL>(a.data); };
    auto is_hash = [](const JoinAlgo& a) { return std::holds_alternative<JoinAlgo::Hash>(a.data); };

    // 20 fact rows per dim row: the hash join is cheaper
    REQUIRE(is_hash(algo_for(20000, 1000, 1000, false)));
    // about one fact row per key (statistics say the column is nearly unique), or a couple: the index probe stays
    REQUIRE(is_reverse(algo_for(20000, 1000, 20000, false)));
    REQUIRE(is_reverse(algo_for(20000, 1000, 10000, false)));
    // a few probes of a selective key beat scanning the big table whatever the table size
    REQUIRE(is_reverse(algo_for(20000, 10, 20000, false)));
    // no statistics: one match per probe is assumed, so the plan is what it always was
    REQUIRE(is_reverse(algo_for(20000, 1000, std::nullopt, false)));
    // a unique key (the primary key) returns one row per probe, whatever the statistics say
    REQUIRE(is_reverse(algo_for(20000, 1000, 1000, true)));
    REQUIRE(is_reverse(algo_for(20000, 1000, std::nullopt, true)));
}

TEST_CASE("ReverseIndexNL without ANALYZE statistics samples how many rows a probe returns", "[planner]") {
    // Tables are rarely analyzed (and the automatic ANALYZE counts statements, not rows), so the planner reads a few rows of the
    // left table and looks their keys up in the index: 20,000 fact rows over `keys` distinct dim_id values.
    auto algo_for = [](std::size_t keys, std::size_t dim_rows) {
        const std::size_t fact_rows = 20000;
        std::vector<Row> fact(fact_rows);
        std::unordered_map<std::string, std::string> bucket_json;
        for (std::size_t i = 0; i < fact_rows; i++) {
            std::string key = std::to_string(i % keys);
            fact[i] = row({{"id", std::to_string(i).c_str()}, {"dim_id", key.c_str()}, {"_xmin", "1"}, {"_xmax", "0"}});
            std::string& json = bucket_json[key];
            json += (json.empty() ? "[" : ",") + std::string("{\"id\":\"") + std::to_string(i) + "\",\"dim_id\":\"" + key + "\",\"_xmin\":\"1\",\"_xmax\":\"0\"}";
        }
        std::unordered_map<std::string, BPlusTree> indexes;
        for (auto& [key, json] : bucket_json) indexes["idx_fk"].insert(key, json + "]");
        std::unordered_map<std::string, std::vector<Row>> tables = {{"fact", fact}, {"dim", std::vector<Row>(dim_rows)}};
        std::unordered_map<std::string, std::pair<std::string, std::string>> index_meta = {{"idx_fk", {"fact", "dim_id"}}};
        std::unordered_map<std::string, CompositeIndex> composite_indexes;
        std::unordered_map<std::string, HashIndex> hash_indexes;
        std::unordered_map<std::string, std::pair<std::string, std::string>> hash_index_meta;
        Catalog catalog;
        ColumnDef id;
        id.name = "id";
        id.primary_key = true;
        catalog.create_table("fact", {id});
        catalog.create_table("dim", {id});
        std::unordered_map<std::string, TableStats> stats; // none: nobody ran ANALYZE
        Planner planner(tables, indexes, index_meta, composite_indexes, hash_indexes, hash_index_meta, catalog, stats);
        auto cond = CondExpr(CondExpr::Leaf{
            Condition{ArithExpr(ArithExpr::Col{"fact.dim_id"}), Operator::Eq, ConditionValue(ConditionValue::Literal{"dim.id"})}});
        std::vector<Join> joins;
        joins.push_back(Join{"dim", cond, JoinType::Inner, {}});
        SelectPlan plan = planner.plan("fact", std::nullopt, joins);
        REQUIRE(plan.joins.size() == 1);
        return plan.joins[0].algo;
    };
    auto is_reverse = [](const JoinAlgo& a) { return std::holds_alternative<JoinAlgo::ReverseIndexNL>(a.data); };
    auto is_hash = [](const JoinAlgo& a) { return std::holds_alternative<JoinAlgo::Hash>(a.data); };

    REQUIRE(is_hash(algo_for(1000, 1000)));      // 20 rows per key, every key probed: the hash join
    REQUIRE(is_reverse(algo_for(20000, 1000)));  // one row per key: the index probe stays
    REQUIRE(is_reverse(algo_for(10000, 1000)));  // two rows per key
    REQUIRE(is_reverse(algo_for(1000, 3)));      // 20 rows per key but only 3 probes: still far cheaper than reading 20,000 rows
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
