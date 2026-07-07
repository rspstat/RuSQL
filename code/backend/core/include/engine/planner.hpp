#pragma once

// Faithful port of rusql-core/src/engine/planner.rs — cost-based query planner.

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include "engine/parser/ast.hpp"
#include "engine/storage/btree.hpp"
#include "engine/storage/composite_index.hpp"
#include "engine/storage/hash_index.hpp"
#include "engine/row.hpp"
#include "engine/catalog/schema.hpp"
#include "engine/table_stats.hpp"

namespace engine {

enum class RangeOp { Gt, Gte, Lt, Lte };
bool range_op_inclusive(RangeOp op);
bool range_op_is_lower_bound(RangeOp op);
const char* range_op_label(RangeOp op);

struct AccessPath {
    struct SeqScan {};
    struct PkPoint { std::string key; };
    struct PkBetween { std::string start, end; };
    struct PkRange { RangeOp op; std::string key; };
    struct SecondaryPoint { std::string index_key, col, key; };
    struct SecondaryRange { std::string index_key, col; RangeOp op; std::string key; };
    struct SecondaryBetween { std::string index_key, col, start, end; };
    struct CompositeIndexPath { std::string index_name; };
    struct CompositeIndexPrefix { std::string index_name, prefix; };
    struct HashPoint { std::string index_key, col, key; };
    struct SecondaryLikePrefix { std::string index_key, col, prefix; };
    struct IndexIntersection { std::vector<AccessPath> paths; };

    using Data = std::variant<SeqScan, PkPoint, PkBetween, PkRange, SecondaryPoint, SecondaryRange,
                               SecondaryBetween, CompositeIndexPath, CompositeIndexPrefix, HashPoint,
                               SecondaryLikePrefix, IndexIntersection>;
    Data data;

    AccessPath() : data(SeqScan{}) {}
    template <typename Alt, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Alt>, AccessPath>>>
    AccessPath(Alt alt) : data(std::move(alt)) {}
};

struct JoinAlgo {
    struct NestedLoop {};
    struct Hash { std::string probe_col, build_col; };
    struct SortMerge { std::string probe_col, build_col; };
    struct IndexNL { std::string probe_col, right_pk_col; };

    using Data = std::variant<NestedLoop, Hash, SortMerge, IndexNL>;
    Data data;

    JoinAlgo() : data(NestedLoop{}) {}
    template <typename Alt, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Alt>, JoinAlgo>>>
    JoinAlgo(Alt alt) : data(std::move(alt)) {}
};

struct TablePlan {
    std::string table;
    AccessPath access;
    std::optional<CondExpr> filter;
    std::size_t est_rows = 0;
    double est_cost = 0.0;
    bool is_covering = false;
};

struct JoinPlan {
    std::string right_table;
    CondExpr on_expr;
    JoinType join_type;
    JoinAlgo algo;
    std::size_t est_rows = 0;
    double est_cost = 0.0;
};

struct SelectPlan {
    TablePlan base;
    std::vector<JoinPlan> joins;

    double total_cost() const {
        double sum = base.est_cost;
        for (auto& j : joins) sum += j.est_cost;
        return sum;
    }
};

class Planner {
public:
    Planner(const std::unordered_map<std::string, std::vector<Row>>& tables,
            const std::unordered_map<std::string, BPlusTree>& indexes,
            const std::unordered_map<std::string, std::pair<std::string, std::string>>& index_meta,
            const std::unordered_map<std::string, CompositeIndex>& composite_indexes,
            const std::unordered_map<std::string, HashIndex>& hash_indexes,
            const std::unordered_map<std::string, std::pair<std::string, std::string>>& hash_index_meta,
            const Catalog& catalog,
            const std::unordered_map<std::string, TableStats>& table_stats)
        : tables_(tables), indexes_(indexes), index_meta_(index_meta), composite_indexes_(composite_indexes),
          hash_indexes_(hash_indexes), hash_index_meta_(hash_index_meta), catalog_(catalog), table_stats_(table_stats) {}

    SelectPlan plan(const std::string& table, const std::optional<CondExpr>& condition, const std::vector<Join>& joins) const;
    SelectPlan plan_covering(const std::string& table, const std::optional<CondExpr>& condition,
                              const std::vector<Join>& joins, const std::vector<SelectColumn>& columns) const;

    AccessPath choose_access(const std::string& table, const std::optional<CondExpr>& condition, const std::optional<std::string>& pk) const;

    std::size_t table_size(const std::string& table) const;
    std::optional<std::string> pk_col(const std::string& table) const;
    std::size_t estimate_rows(std::size_t total, const AccessPath& access, const std::string& table) const;
    double estimate_cost(std::size_t total, const AccessPath& access) const;

    std::string explain(const SelectPlan& plan) const;

private:
    const std::unordered_map<std::string, std::vector<Row>>& tables_;
    const std::unordered_map<std::string, BPlusTree>& indexes_;
    const std::unordered_map<std::string, std::pair<std::string, std::string>>& index_meta_;
    const std::unordered_map<std::string, CompositeIndex>& composite_indexes_;
    const std::unordered_map<std::string, HashIndex>& hash_indexes_;
    const std::unordered_map<std::string, std::pair<std::string, std::string>>& hash_index_meta_;
    const Catalog& catalog_;
    const std::unordered_map<std::string, TableStats>& table_stats_;

    TablePlan plan_table(const std::string& table, const std::optional<CondExpr>& condition) const;
    bool is_covering_access(const AccessPath& access, const std::vector<SelectColumn>& columns, const std::string& table) const;
    std::optional<AccessPath> try_index_intersection(const std::string& table, const CondExpr& expr, const std::optional<std::string>& pk) const;
    std::optional<AccessPath> pk_access(const Condition& cond, const std::string& table) const;
    std::optional<AccessPath> secondary_access(const std::string& index_key, const std::string& col, const Condition& cond, const std::string& table) const;
    bool is_col_ref_in_context(const std::string& k, const std::string& table) const;
    std::optional<std::string> find_secondary_index(const std::string& table, const std::string& col) const;
    std::optional<std::string> find_hash_index(const std::string& table, const std::string& col) const;

    JoinPlan plan_join(const TablePlan& base, const Join& join) const;
    std::size_t estimate_join_output(const TablePlan& base, std::size_t right_size, const CondExpr& on_expr, const std::string& right_table) const;
    JoinAlgo choose_join_algo(std::size_t left_size, std::size_t right_size, const CondExpr& on_expr,
                              const std::string& left_table, const std::string& right_table) const;

    double histogram_sel_range(const std::string& table, const std::string& col, RangeOp op, const std::string& key) const;
    double histogram_sel_between(const std::string& table, const std::string& col, const std::string& lo, const std::string& hi) const;

    std::string describe_access(const AccessPath& access) const;
    std::string describe_join(const JoinPlan& jp) const;
};

// ── Public helpers (also used by executor) ──────────────────────────────────

std::optional<std::pair<std::string, std::string>> extract_equi_join_cols(const CondExpr& on_expr);
std::unordered_map<std::string, std::string> collect_eq_map(const CondExpr& expr);
std::vector<const Condition*> collect_and_leaves(const CondExpr& expr);
std::vector<Join> reorder_joins_dp(const std::string& base_table, std::vector<Join> joins,
                                    const std::unordered_map<std::string, std::vector<Row>>& tables);
std::vector<Join> reorder_joins_greedy(const std::string& base_table, std::vector<Join> joins,
                                       const std::unordered_map<std::string, std::vector<Row>>& tables);
void collect_table_refs_from_expr(const CondExpr& expr, std::unordered_set<std::string>& refs);

} // namespace engine
