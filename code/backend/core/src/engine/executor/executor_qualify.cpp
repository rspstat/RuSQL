// Faithful port of Executor::qualify_stmt / qualify_condexpr from
// rusql-core/src/engine/executor.rs (lines ~6959-7155): qualifies every bare table
// reference in a statement with the current database ("table" -> "{current_db}.table"),
// resolving synonyms where the Rust original does. Called once per top-level statement
// in execute_with_s before dispatch.

#include "engine/executor/executor.hpp"

namespace engine {

CondExpr Executor::qualify_condexpr(const SharedDatabase& s, CondExpr expr) const {
    if (auto* v = std::get_if<CondExpr::And>(&expr.data)) {
        return CondExpr(CondExpr::And{std::make_unique<CondExpr>(qualify_condexpr(s, std::move(*v->lhs))),
                                       std::make_unique<CondExpr>(qualify_condexpr(s, std::move(*v->rhs)))});
    }
    if (auto* v = std::get_if<CondExpr::Or>(&expr.data)) {
        return CondExpr(CondExpr::Or{std::make_unique<CondExpr>(qualify_condexpr(s, std::move(*v->lhs))),
                                      std::make_unique<CondExpr>(qualify_condexpr(s, std::move(*v->rhs)))});
    }
    if (auto* v = std::get_if<CondExpr::Not>(&expr.data)) {
        return CondExpr(CondExpr::Not{std::make_unique<CondExpr>(qualify_condexpr(s, std::move(*v->inner)))});
    }
    if (auto* v = std::get_if<CondExpr::Leaf>(&expr.data)) {
        if (auto* sub = std::get_if<ConditionValue::Subquery>(&v->condition.value.data)) {
            Condition cond = v->condition;
            cond.value = ConditionValue(ConditionValue::Subquery{std::make_unique<Statement>(qualify_stmt(s, std::move(*sub->query)))});
            return CondExpr(CondExpr::Leaf{std::move(cond)});
        }
        return expr;
    }
    return expr;
}

// Every call site qualifies a Join's table via qualify_name_with_synonyms and its
// on_expr via qualify_condexpr, so this private helper avoids repeating that pairing.
Join Executor::qualify_join_(const SharedDatabase& s, const Join& j) const {
    Join out;
    if (j.lateral && j.subquery) {
        // LATERAL JOIN -- Rust 원본에 없음. j.table은 서브쿼리의 별칭이지 실테이블이 아니므로
        // db-접두어를 붙이지 않는다(붙이면 "rusql.o" 같은 존재하지 않는 테이블명이 되어버림).
        // 서브쿼리 자신은 재귀적으로 qualify (다른 서브쿼리 필드들과 동일한 처리 방식).
        out.table = j.table;
        out.subquery = std::make_pair(std::make_unique<Statement>(qualify_stmt(s, Statement(*j.subquery->first))), j.subquery->second);
        out.lateral = true;
    } else {
        out.table = qualify_name_with_synonyms(s, j.table);
    }
    out.on_expr = qualify_condexpr(s, j.on_expr);
    out.join_type = j.join_type;
    out.using_cols = j.using_cols;
    return out;
}

Statement Executor::qualify_stmt(const SharedDatabase& s, Statement stmt) const {
    if (auto* v = std::get_if<Statement::Select>(&stmt.data)) {
        Statement::Select out;
        out.table = qualify_name_with_synonyms(s, v->table);
        if (v->subquery) {
            out.subquery = std::make_pair(std::make_unique<Statement>(qualify_stmt(s, std::move(*v->subquery->first))), v->subquery->second);
        }
        out.columns.reserve(v->columns.size());
        for (auto& c : v->columns) {
            if (auto* sub = std::get_if<SelectColumn::Subquery>(&c.data)) {
                out.columns.push_back(
                    SelectColumn(SelectColumn::Subquery{std::make_unique<Statement>(qualify_stmt(s, std::move(*sub->query))), sub->alias}));
            } else {
                out.columns.push_back(c);
            }
        }
        out.distinct = v->distinct;
        if (v->condition) out.condition = qualify_condexpr(s, std::move(*v->condition));
        out.joins.reserve(v->joins.size());
        for (auto& j : v->joins) out.joins.push_back(qualify_join_(s, j));
        out.order_by = v->order_by;
        out.group_by = v->group_by;
        if (v->having) out.having = qualify_condexpr(s, std::move(*v->having));
        out.limit = v->limit;
        out.offset = v->offset;
        out.for_update = v->for_update;
        out.for_share = v->for_share;
        return Statement(std::move(out));
    }
    if (auto* v = std::get_if<Statement::Insert>(&stmt.data)) {
        v->table = qualify_name_with_synonyms(s, v->table);
        return stmt;
    }
    if (auto* v = std::get_if<Statement::InsertSelect>(&stmt.data)) {
        v->table = qualify_name_with_synonyms(s, v->table);
        *v->query = qualify_stmt(s, std::move(*v->query));
        return stmt;
    }
    if (auto* v = std::get_if<Statement::Update>(&stmt.data)) {
        v->table = qualify_name_with_synonyms(s, v->table);
        if (v->condition) v->condition = qualify_condexpr(s, std::move(*v->condition));
        return stmt;
    }
    if (auto* v = std::get_if<Statement::Delete>(&stmt.data)) {
        v->table = qualify_name_with_synonyms(s, v->table);
        if (v->condition) v->condition = qualify_condexpr(s, std::move(*v->condition));
        return stmt;
    }
    if (auto* v = std::get_if<Statement::CreateTable>(&stmt.data)) {
        for (auto& col : v->columns) {
            if (col.foreign_key) col.foreign_key->ref_table = qualify_name(col.foreign_key->ref_table);
        }
        v->name = qualify_name(v->name);
        return stmt;
    }
    if (auto* v = std::get_if<Statement::DropTable>(&stmt.data)) {
        v->name = qualify_name(v->name);
        return stmt;
    }
    if (auto* v = std::get_if<Statement::TruncateTable>(&stmt.data)) {
        v->name = qualify_name(v->name);
        return stmt;
    }
    if (auto* v = std::get_if<Statement::AlterTable>(&stmt.data)) {
        if (auto* rn = std::get_if<AlterAction::RenameTable>(&v->action.data)) rn->to = qualify_name(rn->to);
        v->table = qualify_name(v->table);
        return stmt;
    }
    if (auto* v = std::get_if<Statement::CreateIndex>(&stmt.data)) {
        v->table = qualify_name(v->table);
        return stmt;
    }
    if (std::holds_alternative<Statement::DropIndex>(stmt.data)) {
        return stmt;
    }
    if (auto* v = std::get_if<Statement::CreateView>(&stmt.data)) {
        v->name = qualify_name(v->name);
        *v->query = qualify_stmt(s, std::move(*v->query));
        return stmt;
    }
    if (auto* v = std::get_if<Statement::DropView>(&stmt.data)) {
        v->name = qualify_name(v->name);
        return stmt;
    }
    if (auto* v = std::get_if<Statement::Describe>(&stmt.data)) {
        v->table = qualify_name(v->table);
        return stmt;
    }
    if (auto* v = std::get_if<Statement::Vacuum>(&stmt.data)) {
        if (v->table) v->table = qualify_name(*v->table);
        return stmt;
    }
    if (auto* v = std::get_if<Statement::AnalyzeTable>(&stmt.data)) {
        v->table = qualify_name(v->table);
        return stmt;
    }
    if (auto* v = std::get_if<Statement::Union>(&stmt.data)) {
        *v->left = qualify_stmt(s, std::move(*v->left));
        *v->right = qualify_stmt(s, std::move(*v->right));
        return stmt;
    }
    if (auto* v = std::get_if<Statement::Intersect>(&stmt.data)) {
        *v->left = qualify_stmt(s, std::move(*v->left));
        *v->right = qualify_stmt(s, std::move(*v->right));
        return stmt;
    }
    if (auto* v = std::get_if<Statement::Except>(&stmt.data)) {
        *v->left = qualify_stmt(s, std::move(*v->left));
        *v->right = qualify_stmt(s, std::move(*v->right));
        return stmt;
    }
    if (auto* v = std::get_if<Statement::ShowCreateTable>(&stmt.data)) {
        v->table = qualify_name(v->table);
        return stmt;
    }
    if (auto* v = std::get_if<Statement::ShowCreateView>(&stmt.data)) {
        v->view = qualify_name(v->view);
        return stmt;
    }
    if (auto* v = std::get_if<Statement::ShowIndex>(&stmt.data)) {
        v->table = qualify_name(v->table);
        return stmt;
    }
    if (auto* v = std::get_if<Statement::With>(&stmt.data)) {
        for (auto& [name, q] : v->ctes) {
            name = qualify_name(name);
            *q = qualify_stmt(s, std::move(*q));
        }
        *v->query = qualify_stmt(s, std::move(*v->query));
        return stmt;
    }
    if (auto* v = std::get_if<Statement::Explain>(&stmt.data)) {
        *v->inner = qualify_stmt(s, std::move(*v->inner));
        return stmt;
    }
    if (auto* v = std::get_if<Statement::ExplainAnalyze>(&stmt.data)) {
        *v->inner = qualify_stmt(s, std::move(*v->inner));
        return stmt;
    }
    if (auto* v = std::get_if<Statement::MultiUpdate>(&stmt.data)) {
        for (auto& t : v->tables) t = qualify_name(t);
        for (auto& j : v->joins) j = qualify_join_(s, j);
        if (v->condition) v->condition = qualify_condexpr(s, std::move(*v->condition));
        return stmt;
    }
    if (auto* v = std::get_if<Statement::MultiDelete>(&stmt.data)) {
        for (auto& t : v->delete_tables) t = qualify_name(t);
        v->from_table = qualify_name(v->from_table);
        for (auto& j : v->joins) j = qualify_join_(s, j);
        if (v->condition) v->condition = qualify_condexpr(s, std::move(*v->condition));
        return stmt;
    }
    if (auto* v = std::get_if<Statement::Merge>(&stmt.data)) {
        v->target = qualify_name(v->target);
        v->source = qualify_name(v->source);
        return stmt;
    }
    if (auto* v = std::get_if<Statement::CreateTrigger>(&stmt.data)) {
        v->table = qualify_name(v->table);
        return stmt;
    }
    return stmt;
}

} // namespace engine
