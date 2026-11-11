// Writes through a view over a join of tables (R80).
//
// MySQL writes through `CREATE VIEW v AS SELECT ... FROM a JOIN b ON ...` when the statement changes the columns of ONE base table: UPDATE v SET x = 1 WHERE y = 5 changes the rows of
// the table x belongs to that the join (and the WHERE of the view) pairs with a row y is 5 in -- each row once, as a multi-table UPDATE does, so it is run as one: the view's
// FROM list and joins, the view's WHERE with the one of the statement, the view's column names replaced by `table.column`. INSERT INTO v (cols) VALUES ... puts the row in the
// base table the columns belong to. The errors are MySQL's: 1393 (columns of two tables), 1394 (INSERT without a list of columns), 1348 (a column that is an expression),
// 1288 / 1471 (a view with an outer join), 1395 (REPLACE, DELETE), 1423 (a column of the table that has no default).
// Not here: WITH CHECK OPTION on a join view (refused with a message of its own), a join with USING or NATURAL, a derived table, a table used twice with the same name.

#include "engine/executor/executor.hpp"
#include "engine/view_rename.hpp"

namespace engine {

namespace {
std::string bare_of(const std::string& qualified) {
    const auto dot = qualified.rfind('.');
    return dot == std::string::npos ? qualified : qualified.substr(dot + 1);
}

const ColumnDef* column_named(const TableSchema& schema, const std::string& name) {
    for (const ColumnDef& c : schema.columns) {
        if (view_names_equal(c.name, name)) return &c;
    }
    return nullptr;
}

// Whether a condition / an expression names a column of the view that is an expression (nothing of a table below it to be read as: it would come out NULL)
bool names_expression_column(const CondExpr& e, const ViewColumnMap& map);

bool names_expression_column(const ArithExpr& e, const ViewColumnMap& map) {
    if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) {
        if (col->outer != 0) return false;
        const auto* entry = view_column_entry(map, col->name);
        return entry && entry->second.empty();
    }
    if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) return names_expression_column(*v->lhs, map) || names_expression_column(*v->rhs, map);
    if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) return names_expression_column(*v->lhs, map) || names_expression_column(*v->rhs, map);
    if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) return names_expression_column(*v->lhs, map) || names_expression_column(*v->rhs, map);
    if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) return names_expression_column(*v->lhs, map) || names_expression_column(*v->rhs, map);
    if (auto* v = std::get_if<ArithExpr::Cmp>(&e.data)) return names_expression_column(*v->lhs, map) || names_expression_column(*v->rhs, map);
    if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) {
        return std::any_of(f->args.begin(), f->args.end(), [&](const ArithExpr& a) { return names_expression_column(a, map); });
    }
    if (auto* p = std::get_if<ArithExpr::Pred>(&e.data)) return names_expression_column(*p->cond, map);
    return false;
}

bool names_expression_column(const CondExpr& e, const ViewColumnMap& map) {
    if (auto* a = std::get_if<CondExpr::And>(&e.data)) return names_expression_column(*a->lhs, map) || names_expression_column(*a->rhs, map);
    if (auto* o = std::get_if<CondExpr::Or>(&e.data)) return names_expression_column(*o->lhs, map) || names_expression_column(*o->rhs, map);
    if (auto* n = std::get_if<CondExpr::Not>(&e.data)) return names_expression_column(*n->inner, map);
    if (auto* leaf = std::get_if<CondExpr::Leaf>(&e.data)) {
        if (names_expression_column(leaf->condition.left, map)) return true;
        if (auto* value = std::get_if<ConditionValue::Arith>(&leaf->condition.value.data)) return names_expression_column(value->expr, map);
        if (auto* literal = std::get_if<ConditionValue::Literal>(&leaf->condition.value.data)) {
            if (literal->quoted || literal->outer != 0) return false;
            const auto* entry = view_column_entry(map, literal->value);
            return entry && entry->second.empty();
        }
    }
    return false;
}

std::string not_supported(const char* what) {
    return std::string("Writing through a join view ") + what + " is not supported in this version";
}
} // namespace

std::optional<Executor::JoinViewTarget> Executor::resolve_join_view_target(const SharedDatabase& s, const std::string& name) {
    auto it = s.views.find(name);
    if (it == s.views.end()) return std::nullopt;
    const auto* sel = std::get_if<Statement::Select>(&it->second.data);
    if (!sel) return std::nullopt;
    // DISTINCT, GROUP BY, HAVING, LIMIT, a derived table: no row of the view is a row of a table
    const bool group_by_empty = !sel->group_by || sel->group_by->empty();
    if (sel->distinct || !group_by_empty || sel->subquery || sel->having || sel->limit || sel->offset) return std::nullopt;

    JoinViewTarget target;
    ViewColumnMap below; // what the select list may name: (a column of the view below, or the table's column, "name.column")
    auto raw = s.view_raw_sql.find(name);
    target.check_option = raw != s.view_raw_sql.end() && raw->second.find("CHECK OPTION") != std::string::npos;

    const bool nested = s.views.count(sel->table) > 0;
    if (nested) { // a view over a join view: the tables, joins and columns of the one below, with this view's WHERE added
        if (!sel->joins.empty()) return std::nullopt;
        auto inner = resolve_join_view_target(s, sel->table);
        if (!inner) return std::nullopt;
        const bool check = target.check_option;
        target = *inner;
        target.check_option = check || inner->check_option;
        below = inner->columns;
        std::optional<CondExpr> mine = sel->condition;
        if (mine) rename_cond_columns(*mine, below);
        target.condition = merge_conditions(target.condition, mine);
    } else {
        if (sel->joins.empty()) return std::nullopt; // (one table: resolve_view_target)
        auto add = [&](const std::string& table, const std::string& alias) {
            if (!s.catalog.get_table(table)) return false; // (a table that is not there: a view, a temporary table)
            const std::string key = alias.empty() ? bare_of(table) : alias;
            for (auto& entry : target.tables) {
                if (view_names_equal(entry.first, key)) return false; // (the same table twice under one name)
            }
            target.tables.emplace_back(key, table);
            return true;
        };
        if (!add(sel->table, "")) return std::nullopt;
        for (const Join& j : sel->joins) {
            if (j.subquery || j.lateral || !j.using_cols.empty() || j.join_type == JoinType::Natural || j.join_type == JoinType::FullOuter) return std::nullopt;
            if (j.join_type == JoinType::Left || j.join_type == JoinType::Right) target.outer = true;
            if (!add(j.table, j.alias)) return std::nullopt;
        }
        target.joins = sel->joins;
        target.condition = sel->condition;
    }

    // "name.column" of the table a column written in the select list is (empty when none is, or when two have it)
    auto base_ref = [&](const std::string& written) -> std::string {
        const auto dot = written.rfind('.');
        const std::string column = dot == std::string::npos ? written : written.substr(dot + 1);
        std::string found;
        int hits = 0;
        for (auto& [key, table] : target.tables) {
            if (dot != std::string::npos) {
                const std::string qualifier = written.substr(0, dot);
                if (!view_names_equal(qualifier, key) && !view_names_equal(qualifier, table) && !view_names_equal(qualifier, bare_of(table))) continue;
            }
            const TableSchema* schema = s.catalog.get_table(table);
            const ColumnDef* def = schema ? column_named(*schema, column) : nullptr;
            if (!def) continue;
            found = key + "." + def->name;
            hits++;
        }
        return hits == 1 ? found : std::string();
    };
    auto column_below = [&](const std::string& written) -> std::string {
        if (nested) {
            const auto* entry = view_column_entry(below, written);
            return entry ? entry->second : std::string();
        }
        return base_ref(written);
    };

    for (const SelectColumn& c : sel->columns) {
        if (auto* all = std::get_if<SelectColumn::All>(&c.data)) { // `*`, or `t.*`
            if (nested) {
                for (auto& entry : below) target.columns.push_back(entry);
                continue;
            }
            for (auto& [key, table] : target.tables) {
                if (!all->table.empty() && !view_names_equal(all->table, key) && !view_names_equal(all->table, table) && !view_names_equal(all->table, bare_of(table))) continue;
                if (const TableSchema* schema = s.catalog.get_table(table)) {
                    for (const ColumnDef& def : schema->columns) target.columns.emplace_back(def.name, key + "." + def.name);
                }
            }
        } else if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) {
            target.columns.emplace_back(bare_of(col->name), column_below(col->name));
        } else if (auto* aliased = std::get_if<SelectColumn::ColumnAlias>(&c.data)) {
            target.columns.emplace_back(aliased->alias, column_below(aliased->name));
        } else if (auto* expr = std::get_if<SelectColumn::Expr>(&c.data)) {
            target.columns.emplace_back(expr->alias.value_or(""), "");
        } else if (auto* func = std::get_if<SelectColumn::Func>(&c.data)) {
            target.columns.emplace_back(func->alias.value_or(""), "");
        } else if (auto* when = std::get_if<SelectColumn::CaseWhen>(&c.data)) {
            target.columns.emplace_back(when->alias.value_or(""), "");
        } else {
            return std::nullopt; // (an aggregate, a window function, a subquery in the select list)
        }
    }
    return target;
}

StringResult Executor::exec_join_view_update(SharedDatabase& s, const std::string& view, const JoinViewTarget& target,
                                              std::vector<std::pair<std::string, ArithExpr>> assignments, std::optional<CondExpr> condition) {
    if (target.outer) return StringResult::Err("ERROR 1288 (HY000): The target table " + strip_db_prefix(view) + " of the UPDATE is not updatable");
    if (target.check_option) return StringResult::Err(not_supported("with CHECK OPTION"));
    std::string base_key;
    for (auto& [column, expr] : assignments) {
        const auto* entry = view_column_entry(target.columns, column);
        if (!entry) return StringResult::Err("Unknown column '" + column + "' in 'field list'");
        if (entry->second.empty()) return StringResult::Err("ERROR 1348 (HY000): Column '" + column + "' is not updatable");
        const std::string key = entry->second.substr(0, entry->second.find('.'));
        if (!base_key.empty() && !view_names_equal(base_key, key)) {
            return StringResult::Err("ERROR 1393 (HY000): Can not modify more than one base table through a join view '" + view + "'");
        }
        base_key = key;
        if (names_expression_column(expr, target.columns)) return StringResult::Err(not_supported("with a column that is an expression read in the new value"));
        column = entry->second;
        rename_arith_columns(expr, target.columns);
    }
    if (condition) {
        if (names_expression_column(*condition, target.columns)) return StringResult::Err(not_supported("with a column that is an expression read in the WHERE"));
        rename_cond_columns(*condition, target.columns);
    }
    return exec_multi_update(s, {target.tables.front().second}, target.joins, std::move(assignments), merge_conditions(target.condition, condition));
}

StringResult Executor::exec_join_view_insert(SharedDatabase& s, const std::string& view, const JoinViewTarget& target, std::optional<std::vector<std::string>> col_list,
                                              std::vector<std::vector<std::string>> all_values, InsertConflict on_conflict,
                                              std::optional<std::vector<SelectColumn>> returning, bool from_select) {
    const std::string bare = strip_db_prefix(view);
    if (std::holds_alternative<InsertConflict::Replace>(on_conflict.data)) return StringResult::Err("ERROR 1395 (HY000): Can not delete from join view '" + view + "'");
    if (target.outer) return StringResult::Err("ERROR 1471 (HY000): The target table " + bare + " of the INSERT is not insertable-into");
    if (!col_list) return StringResult::Err("ERROR 1394 (HY000): Can not insert into join view '" + view + "' without fields list");

    std::string base_key;
    std::vector<std::string> below;
    for (const std::string& written : *col_list) {
        const auto* entry = view_column_entry(target.columns, written);
        if (!entry) return StringResult::Err("Unknown column '" + written + "' in 'field list'");
        if (entry->second.empty()) return StringResult::Err("ERROR 1348 (HY000): Column '" + written + "' is not updatable");
        const auto dot = entry->second.find('.');
        const std::string key = entry->second.substr(0, dot);
        if (!base_key.empty() && !view_names_equal(base_key, key)) {
            return StringResult::Err("ERROR 1393 (HY000): Can not modify more than one base table through a join view '" + view + "'");
        }
        base_key = key;
        below.push_back(entry->second.substr(dot + 1));
    }
    // a view with a column of an expression takes no INSERT at all (MySQL 1471), however it is written
    for (auto& entry : target.columns) {
        if (entry.second.empty()) return StringResult::Err("ERROR 1471 (HY000): The target table " + bare + " of the INSERT is not insertable-into");
    }
    if (target.check_option) return StringResult::Err(not_supported("with CHECK OPTION"));

    std::string base_table;
    for (auto& [key, table] : target.tables) {
        if (view_names_equal(key, base_key)) base_table = table;
    }
    if (base_table.empty()) return StringResult::Err("ERROR 1471 (HY000): The target table " + bare + " of the INSERT is not insertable-into");

    // ON DUPLICATE KEY UPDATE names the columns of the view: they are the table's
    if (auto* update = std::get_if<InsertConflict::Update>(&on_conflict.data)) {
        ViewColumnMap bare_map;
        for (auto& entry : target.columns) {
            const auto dot = entry.second.find('.');
            if (!entry.second.empty() && view_names_equal(entry.second.substr(0, dot), base_key)) bare_map.emplace_back(entry.first, entry.second.substr(dot + 1));
        }
        for (auto& [column, expr] : update->assignments) {
            if (const auto* entry = view_column_entry(bare_map, column)) column = entry->second;
            rename_arith_columns(expr, bare_map);
        }
    }

    insert_from_select_ = from_select;
    auto done = exec_insert(s, base_table, below, std::move(all_values), std::move(on_conflict), std::move(returning));
    if (done.is_err() && done.error().find("doesn't have a default value") != std::string::npos) {
        return StringResult::Err("ERROR 1423 (HY000): Field of view '" + view + "' underlying table doesn't have a default value");
    }
    return done;
}

} // namespace engine
