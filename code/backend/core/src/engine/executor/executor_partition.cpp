// Table partitioning (PARTITION BY RANGE/LIST/HASH, V1): a partitioned table is a
// permanently-empty logical "phantom" table (see exec_create, executor_ddl.cpp) plus N
// ordinary physical child tables, each with its own full set of the 8 per-table
// SharedDatabase maps. This file is the routing layer: it rewrites a statement targeting
// the logical table into one statement per relevant child and runs each through
// Executor::execute() -- reusing 100% of the existing locking/MVCC/index/undo machinery
// unchanged for every child; nothing below this layer (exec_insert_inner,
// exec_update_inner, exec_delete_inner, exec_select, table_lock_set_for, ...) needs to
// know partitioning exists at all.
//
// Design note -- why routing can ONLY recurse through the outer execute(), never
// execute_with_s(): execute() acquires the structural/per-table locks for the CURRENT
// statement and then calls execute_with_s() while still holding them. If routing were
// hooked inside execute_with_s() (e.g. to also cover views/subqueries/CTEs/triggers that
// reach a partitioned table without going through execute()'s own top-level entry), a
// recursive execute() call for a child would try to re-acquire the SAME structural
// RwLock/std::shared_mutex on the SAME thread while the outer call's guard is still
// alive -- undefined behavior, the exact "same mutex twice on one thread" hazard this
// codebase's own comments repeatedly flag and avoid (see e.g. is_select_family's note in
// executor_core.cpp). So routing only ever hooks at the very top of execute(), BEFORE any
// lock for the current statement is taken -- covering the common case (a partitioned
// table as the direct target of a client's own INSERT/UPDATE/DELETE/SELECT). Anything
// that reaches a partitioned table indirectly (views, subqueries, CTEs, triggers, JOINs,
// InsertSelect/MultiUpdate/MultiDelete/Merge) is explicitly rejected -- see the
// execute_with_s safety-net check right after qualify_stmt -- rather than silently
// touching the empty phantom and returning wrong (empty) results.

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

#include "engine/planner.hpp"

namespace engine {

namespace {

// Duplicated from gap_lock.cpp's cmp_val (different translation unit, same reasoning as
// that file's own comment for why it doesn't share code with executor_eval.cpp) --
// numeric-priority compare so RANGE partition bounds compare the same way a WHERE clause
// would evaluate the same values.
std::optional<double> parse_f64(const std::string& s) {
    if (s.empty()) return std::nullopt;
    double val;
    auto res = std::from_chars(s.data(), s.data() + s.size(), val);
    if (res.ec != std::errc() || res.ptr != s.data() + s.size()) return std::nullopt;
    return val;
}

int cmp_val(const std::string& a, const std::string& b) {
    auto da = parse_f64(a);
    auto db = parse_f64(b);
    if (da && db) return (*da < *db) ? -1 : (*da > *db ? 1 : 0);
    return (a < b) ? -1 : (a > b ? 1 : 0);
}

// Duplicated from executor_core.cpp's anonymous-namespace qualify_local (different TU).
std::string qualify_table_name(const std::string& name, const std::string& current_db) {
    if (name.find('.') != std::string::npos) return name;
    return current_db + "." + name;
}

// First Eq or In match for `column` among `where`'s AND-joined leaves (LIST/HASH
// pruning). Deliberately doesn't try to intersect multiple matching leaves -- the first
// one found is already a safe (possibly loose) candidate set, and pruning is always
// allowed to over-return.
std::optional<std::vector<std::string>> find_eq_or_in_values(const std::optional<CondExpr>& where, const std::string& column) {
    if (!where) return std::nullopt;
    for (auto* cond : collect_and_leaves(*where)) {
        auto* col = std::get_if<ArithExpr::Col>(&cond->left.data);
        if (!col || col->name != column) continue;
        if (cond->op == Operator::Eq) {
            if (auto* lit = std::get_if<ConditionValue::Literal>(&cond->value.data)) return std::vector<std::string>{lit->value};
        } else if (cond->op == Operator::In) {
            if (auto* list = std::get_if<ConditionValue::LiteralList>(&cond->value.data)) return list->values;
        }
    }
    return std::nullopt;
}

// Leading integer of a "N row(s) <verb>." success message (nullopt if the message
// doesn't start with digits -- e.g. an unexpected format, treated as 0 by callers).
std::optional<std::size_t> leading_count(const std::string& msg) {
    std::size_t i = 0;
    while (i < msg.size() && std::isdigit(static_cast<unsigned char>(msg[i]))) i++;
    if (i == 0) return std::nullopt;
    try {
        return static_cast<std::size_t>(std::stoull(msg.substr(0, i)));
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

std::optional<PartitionBy> Executor::partition_info_for(const SharedDatabase& s, const std::string& table) {
    auto* schema = s.catalog.get_table(table);
    if (!schema || !schema->partition_info) return std::nullopt;
    return schema->partition_info;
}

std::string Executor::partition_child_for_value(const PartitionBy& info, const std::string& value) {
    switch (info.kind) {
        case PartitionKind::Range:
            for (auto& def : info.partitions) {
                if (def.range_is_maxvalue) return def.child_table;
                if (def.range_upper_bound && cmp_val(value, *def.range_upper_bound) < 0) return def.child_table;
            }
            return ""; // exceeds every bound, no MAXVALUE catch-all -- caller must error
        case PartitionKind::List:
            for (auto& def : info.partitions) {
                if (std::find(def.list_values.begin(), def.list_values.end(), value) != def.list_values.end()) return def.child_table;
            }
            return "";
        case PartitionKind::Hash: {
            if (info.partitions.empty()) return "";
            // MySQL's own HASH partitioning requires an integer-valued expression; V1
            // matches that restriction (enforced at CREATE TABLE time isn't done, but a
            // non-numeric value here just falls into bucket 0 -- documented limitation,
            // not a silent-wrong-answer risk since routing is still deterministic).
            long long n = 0;
            try {
                n = std::stoll(value);
            } catch (...) {
                n = 0;
            }
            long long count = static_cast<long long>(info.partitions.size());
            long long idx = ((n % count) + count) % count;
            return info.partitions[static_cast<std::size_t>(idx)].child_table;
        }
    }
    return "";
}

std::vector<std::string> Executor::prune_partition_children(const PartitionBy& info, const std::optional<CondExpr>& where) {
    std::vector<std::string> all;
    all.reserve(info.partitions.size());
    for (auto& def : info.partitions) all.push_back(def.child_table);
    if (!where) return all;

    switch (info.kind) {
        case PartitionKind::Range: {
            // extract_pk_gap_range is already generic on column name (not just PK) --
            // reused as-is for RANGE pruning on the partition column.
            GapRange qr = extract_pk_gap_range(where, info.column);
            if (!qr.lo && !qr.hi) return all; // fully unbounded -- can't prune anything
            std::vector<std::string> pruned;
            std::optional<std::string> prev_bound; // inclusive lower edge of the NEXT partition
            for (auto& def : info.partitions) {
                std::optional<std::string> part_lo = prev_bound; // inclusive; nullopt == -inf
                std::optional<std::string> part_hi =
                    def.range_is_maxvalue ? std::nullopt : def.range_upper_bound; // exclusive; nullopt == +inf
                bool overlaps = true;
                // No overlap if the partition ends at-or-before the query's lower bound --
                // part_hi is always exclusive, so this holds regardless of qr.lo_inclusive.
                if (part_hi && qr.lo && cmp_val(*part_hi, *qr.lo) <= 0) overlaps = false;
                // No overlap if the partition starts strictly after the query's upper
                // bound (or exactly at it while the query excludes that boundary) --
                // part_lo is always inclusive, so qr.hi_inclusive matters here.
                if (overlaps && part_lo && qr.hi) {
                    int c = cmp_val(*part_lo, *qr.hi);
                    if (c > 0 || (c == 0 && !qr.hi_inclusive)) overlaps = false;
                }
                if (overlaps) pruned.push_back(def.child_table);
                prev_bound = def.range_upper_bound;
            }
            return pruned.empty() ? all : pruned;
        }
        case PartitionKind::List: {
            auto values = find_eq_or_in_values(where, info.column);
            if (!values) return all;
            std::vector<std::string> pruned;
            for (auto& def : info.partitions) {
                bool any = std::any_of(values->begin(), values->end(), [&](const std::string& v) {
                    return std::find(def.list_values.begin(), def.list_values.end(), v) != def.list_values.end();
                });
                if (any) pruned.push_back(def.child_table);
            }
            return pruned.empty() ? all : pruned;
        }
        case PartitionKind::Hash: {
            // Only a single Eq value is prunable to one bucket -- anything else (In with
            // multiple values, ranges, no match) can't be narrowed below "all buckets".
            auto values = find_eq_or_in_values(where, info.column);
            if (!values || values->size() != 1) return all;
            std::string child = partition_child_for_value(info, (*values)[0]);
            return child.empty() ? all : std::vector<std::string>{child};
        }
    }
    return all;
}

Executor::RoutedRun Executor::run_routed_statements(std::vector<Statement> child_stmts) {
    RoutedRun run;
    if (child_stmts.empty()) return run;

    // Wrap in an implicit transaction when the caller isn't already in one, so a
    // multi-child autocommit statement is all-or-nothing (matching a single-table
    // statement's atomicity) instead of leaving earlier children's writes in place if a
    // later child fails. Already-explicit transactions rely on the caller's own eventual
    // COMMIT/ROLLBACK, exactly like FK cascades already do.
    bool own_txn = !txn.is_active();
    if (own_txn) {
        auto begin_result = execute(Statement(Statement::Begin{}));
        if (begin_result.is_err()) {
            run.ok = false;
            run.error = begin_result;
            return run;
        }
    }

    for (auto& child_stmt : child_stmts) {
        auto result = execute(std::move(child_stmt));
        if (result.is_err()) {
            if (own_txn) execute(Statement(Statement::Rollback{}));
            run.ok = false;
            run.error = result;
            return run;
        }
        run.child_results.push_back(std::move(result));
    }

    if (own_txn) {
        auto commit_result = execute(Statement(Statement::Commit{}));
        if (commit_result.is_err()) {
            run.ok = false;
            run.error = commit_result;
            return run;
        }
    }
    return run;
}

StringResult Executor::route_partitioned_insert(const std::string& table, const PartitionBy& info, Statement::Insert ins) {
    if (ins.returning) return StringResult::Err("RETURNING is not yet supported on INSERT into a partitioned table in this version");

    std::size_t part_idx;
    if (ins.columns) {
        auto it = std::find(ins.columns->begin(), ins.columns->end(), info.column);
        if (it == ins.columns->end())
            return StringResult::Err("INSERT into a partitioned table must include the partition column '" + info.column + "'");
        part_idx = static_cast<std::size_t>(std::distance(ins.columns->begin(), it));
    } else {
        // No explicit column list -- values are positional against the full schema, so
        // the partition column's index has to come from the catalog.
        auto s = shared->read();
        auto* schema = s->catalog.get_table(table);
        if (!schema) return StringResult::Err("Table '" + table + "' not found");
        auto it = std::find_if(schema->columns.begin(), schema->columns.end(), [&](const ColumnDef& c) { return c.name == info.column; });
        if (it == schema->columns.end()) return StringResult::Err("PARTITION BY column '" + info.column + "' not defined");
        part_idx = static_cast<std::size_t>(std::distance(schema->columns.begin(), it));
    }

    std::vector<std::string> child_order;
    std::unordered_map<std::string, std::vector<std::vector<std::string>>> grouped;
    for (auto& row : ins.values) {
        if (part_idx >= row.size()) return StringResult::Err("INSERT value list is shorter than the partition column's position");
        std::string child = partition_child_for_value(info, row[part_idx]);
        if (child.empty())
            return StringResult::Err("No partition found for value '" + row[part_idx] + "' in column '" + info.column + "'");
        if (!grouped.count(child)) child_order.push_back(child);
        grouped[child].push_back(row);
    }

    std::vector<Statement> child_stmts;
    child_stmts.reserve(child_order.size());
    for (auto& child : child_order) {
        Statement::Insert child_ins;
        child_ins.table = child;
        child_ins.columns = ins.columns;
        child_ins.values = std::move(grouped[child]);
        child_ins.on_conflict = ins.on_conflict;
        child_stmts.push_back(Statement(std::move(child_ins)));
    }

    auto run = run_routed_statements(std::move(child_stmts));
    if (!run.ok) return run.error;
    std::size_t total = 0;
    for (auto& r : run.child_results) {
        if (auto c = leading_count(r.value())) total += *c;
    }
    return StringResult::Ok(std::to_string(total) + " row(s) inserted.");
}

StringResult Executor::route_partitioned_update(const std::string& table, const PartitionBy& info, Statement::Update upd) {
    (void)table;
    if (upd.returning) return StringResult::Err("RETURNING is not yet supported on UPDATE against a partitioned table in this version");
    for (auto& [col, expr] : upd.assignments) {
        (void)expr;
        if (col == info.column)
            return StringResult::Err("Cannot UPDATE the partitioning column '" + info.column + "' -- use DELETE+INSERT instead");
    }

    auto children = prune_partition_children(info, upd.condition);
    std::vector<Statement> child_stmts;
    child_stmts.reserve(children.size());
    for (auto& child : children) {
        Statement::Update child_upd = upd;
        child_upd.table = child;
        child_stmts.push_back(Statement(std::move(child_upd)));
    }

    auto run = run_routed_statements(std::move(child_stmts));
    if (!run.ok) return run.error;
    std::size_t total = 0;
    for (auto& r : run.child_results) {
        if (auto c = leading_count(r.value())) total += *c;
    }
    return StringResult::Ok(std::to_string(total) + " row(s) updated.");
}

StringResult Executor::route_partitioned_delete(const std::string& table, const PartitionBy& info, Statement::Delete del) {
    (void)table;
    if (del.returning) return StringResult::Err("RETURNING is not yet supported on DELETE against a partitioned table in this version");

    auto children = prune_partition_children(info, del.condition);
    std::vector<Statement> child_stmts;
    child_stmts.reserve(children.size());
    for (auto& child : children) {
        Statement::Delete child_del = del;
        child_del.table = child;
        child_stmts.push_back(Statement(std::move(child_del)));
    }

    auto run = run_routed_statements(std::move(child_stmts));
    if (!run.ok) return run.error;
    std::size_t total = 0;
    for (auto& r : run.child_results) {
        if (auto c = leading_count(r.value())) total += *c;
    }
    return StringResult::Ok(std::to_string(total) + " row(s) deleted.");
}

StringResult Executor::route_partitioned_select(const PartitionBy& info, Statement stmt) {
    auto* sel = std::get_if<Statement::Select>(&stmt.data);
    if (!sel) return StringResult::Err("This statement is not supported on partitioned tables in this version");
    // V1 scope: only a plain per-row scan (projection + WHERE, optionally ORDER BY/LIMIT/
    // OFFSET/FOR UPDATE/FOR SHARE, all applied AFTER merging children) is routed --
    // GROUP BY/aggregates/DISTINCT/JOIN need cross-child reconciliation this version
    // doesn't implement, so they're rejected explicitly rather than silently merged wrong.
    if (sel->subquery) return StringResult::Err("FROM-subquery is not supported together with a partitioned table in this version");
    if (!sel->joins.empty()) return StringResult::Err("JOIN is not yet supported on partitioned tables in this version");
    if (sel->group_by) return StringResult::Err("GROUP BY is not yet supported on partitioned tables in this version");
    if (sel->distinct) return StringResult::Err("SELECT DISTINCT is not yet supported on partitioned tables in this version");
    bool has_agg = std::any_of(sel->columns.begin(), sel->columns.end(), [](const SelectColumn& c) {
        return std::holds_alternative<SelectColumn::Agg>(c.data) || std::holds_alternative<SelectColumn::AggAlias>(c.data);
    });
    if (has_agg) return StringResult::Err("Aggregate functions are not yet supported on partitioned tables in this version");

    auto children = prune_partition_children(info, sel->condition);
    std::vector<std::string> order_by_cols;
    std::vector<OrderBy> order_by = sel->order_by;
    std::optional<std::size_t> limit = sel->limit;
    std::optional<std::size_t> offset = sel->offset;

    std::vector<Statement> child_stmts;
    child_stmts.reserve(children.size());
    for (auto& child : children) {
        // Statement's own explicit deep-copy constructor is required here (not a direct
        // Statement::Select copy -- its `subquery` member holds a unique_ptr<Statement>,
        // so Select has no implicit copy constructor of its own).
        Statement child_stmt = stmt;
        auto* child_sel = std::get_if<Statement::Select>(&child_stmt.data);
        child_sel->table = child;
        // Applied once on the merged result below instead -- a per-child LIMIT/OFFSET
        // would truncate before merging, and per-child ORDER BY is wasted work.
        child_sel->order_by.clear();
        child_sel->limit = std::nullopt;
        child_sel->offset = std::nullopt;
        child_stmts.push_back(std::move(child_stmt));
    }

    auto run = run_routed_statements(std::move(child_stmts));
    if (!run.ok) return run.error;

    std::vector<std::string> cols;
    std::vector<Row> merged;
    for (auto& r : run.child_results) {
        auto [c, rows] = parse_table_output(r.value());
        if (cols.empty()) cols = std::move(c);
        for (auto& row : rows) merged.push_back(std::move(row));
    }
    (void)order_by_cols;
    apply_set_postprocess(merged, cols, order_by, limit, offset);
    return StringResult::Ok(format_set_result(cols, merged));
}

std::optional<StringResult> Executor::try_route_partitioned(Statement& stmt) {
    // Insert/Update/Delete/Select: the only kinds V1 actually routes.
    std::string qtable;
    if (auto* v = std::get_if<Statement::Insert>(&stmt.data)) qtable = qualify_table_name(v->table, current_db);
    else if (auto* v = std::get_if<Statement::Update>(&stmt.data)) qtable = qualify_table_name(v->table, current_db);
    else if (auto* v = std::get_if<Statement::Delete>(&stmt.data)) qtable = qualify_table_name(v->table, current_db);
    else if (auto* v = std::get_if<Statement::Select>(&stmt.data)) qtable = qualify_table_name(v->table, current_db);
    else if (std::holds_alternative<Statement::InsertSelect>(stmt.data) || std::holds_alternative<Statement::MultiUpdate>(stmt.data) ||
             std::holds_alternative<Statement::MultiDelete>(stmt.data) || std::holds_alternative<Statement::Merge>(stmt.data)) {
        // Can touch a partitioned table but V1 doesn't route these -- reject explicitly
        // (with a specific message) rather than let them silently fall through to the
        // permanently-empty logical parent.
        std::vector<std::string> candidates;
        if (auto* v = std::get_if<Statement::InsertSelect>(&stmt.data)) candidates.push_back(qualify_table_name(v->table, current_db));
        else if (auto* v = std::get_if<Statement::MultiUpdate>(&stmt.data)) {
            for (auto& t : v->tables) candidates.push_back(qualify_table_name(t, current_db));
        } else if (auto* v = std::get_if<Statement::MultiDelete>(&stmt.data)) {
            candidates.push_back(qualify_table_name(v->from_table, current_db));
            for (auto& t : v->delete_tables) candidates.push_back(qualify_table_name(t, current_db));
        } else if (auto* v = std::get_if<Statement::Merge>(&stmt.data)) {
            candidates.push_back(qualify_table_name(v->target, current_db));
            candidates.push_back(qualify_table_name(v->source, current_db));
        }
        auto s = shared->read();
        for (auto& c : candidates) {
            if (partition_info_for(*s, c)) {
                return StringResult::Err("Partitioned tables don't yet support this statement type in this version "
                                          "(INSERT/UPDATE/DELETE/SELECT directly on the table are supported)");
            }
        }
        return std::nullopt;
    } else {
        return std::nullopt;
    }

    std::optional<PartitionBy> info;
    {
        auto s = shared->read();
        info = partition_info_for(*s, qtable);
    }
    if (!info) return std::nullopt;

    if (auto* v = std::get_if<Statement::Insert>(&stmt.data)) return route_partitioned_insert(qtable, *info, std::move(*v));
    if (auto* v = std::get_if<Statement::Update>(&stmt.data)) return route_partitioned_update(qtable, *info, std::move(*v));
    if (auto* v = std::get_if<Statement::Delete>(&stmt.data)) return route_partitioned_delete(qtable, *info, std::move(*v));
    if (std::holds_alternative<Statement::Select>(stmt.data)) return route_partitioned_select(*info, std::move(stmt));
    return std::nullopt; // unreachable: qtable is only set for the 4 kinds checked above
}

} // namespace engine
