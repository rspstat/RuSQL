// Names without regard to case. MySQL does not tell `NAME` from `name` for a column, an alias or an index, nor `T` from `t` for a table on Windows
// (lower_case_table_names = 1): `SELECT NAME FROM Users` finds the column `name` of the table `users`. Every table, database and column name below
// is kept in the spelling it was created with, so the statement is rewritten to that spelling before anything else looks at it -- the locks it takes, the
// query cache, the privilege check, the binder and the executor then all agree on one spelling.
//
// This file does the names of tables and databases everywhere in a statement and the lists of columns of the statements that write (INSERT / REPLACE / UPDATE
// / DELETE / CREATE INDEX / ALTER TABLE ...), whose table is known as soon as it is named. The columns inside the expressions of a query are the binder's
// (executor_bind.cpp): it knows which table each of them belongs to.

#include <algorithm>
#include <cctype>

#include "engine/column_text.hpp"
#include "engine/executor/executor.hpp"

namespace engine {

namespace {

bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

// the spelling of `want` among `names`: itself when it is one of them, else the one name that is it apart from case; itself when none or several are
template <typename Range>
std::string pick(const std::string& want, const Range& names) {
    const std::string* found = nullptr;
    for (const std::string& n : names) {
        if (n == want) return want;
        if (ieq(n, want)) found = found ? nullptr : &n;
    }
    return found ? *found : want;
}

struct NameResolver {
    const SharedDatabase& s;
    const std::string& current_db;
    std::vector<std::string> ctes; // the names the WITH clauses around the statement give

    std::string qualified(const std::string& name) const { return name.find('.') != std::string::npos ? name : current_db + "." + name; }

    std::string database(const std::string& db) const {
        if (s.databases.count(db)) return db;
        return pick(db, s.databases);
    }

    // a table or a view, in the form it was written (`t` stays `t`, `db.t` stays `db.t`)
    std::string table(const std::string& name) const {
        if (name.empty()) return name;
        if (std::find(ctes.begin(), ctes.end(), name) != ctes.end()) return name;
        for (const std::string& cte : ctes) {
            if (ieq(cte, name)) return cte;
        }
        const std::size_t dot = name.find('.');
        const std::string db = dot == std::string::npos ? current_db : database(name.substr(0, dot));
        const std::string bare = dot == std::string::npos ? name : name.substr(dot + 1);
        const std::string key = db + "." + bare;
        std::string spelled = bare;
        if (!s.catalog.tables.count(key) && !s.views.count(key)) {
            const std::string prefix = db + ".";
            std::vector<std::string> candidates;
            for (const auto& entry : s.catalog.tables) {
                if (entry.first.size() > prefix.size() && entry.first.compare(0, prefix.size(), prefix) == 0) candidates.push_back(entry.first.substr(prefix.size()));
            }
            for (const auto& entry : s.views) {
                if (entry.first.size() > prefix.size() && entry.first.compare(0, prefix.size(), prefix) == 0) candidates.push_back(entry.first.substr(prefix.size()));
            }
            spelled = pick(bare, candidates);
        }
        return dot == std::string::npos ? spelled : db + "." + spelled;
    }

    void table_in_place(std::string& name) const { name = table(name); }

    const TableSchema* schema_of(const std::string& name) const { return s.catalog.get_table(qualified(name)); }

    // a column of `schema`, in the spelling the table has it
    static std::string column(const TableSchema* schema, const std::string& name) {
        if (!schema) return name;
        std::vector<std::string> names;
        names.reserve(schema->columns.size());
        for (const ColumnDef& c : schema->columns) names.push_back(c.name);
        return pick(name, names);
    }

    static void column_in_place(const TableSchema* schema, std::string& name) {
        if (!schema) return;
        // (`t.c`: the column is what follows the last dot)
        const std::size_t dot = name.rfind('.');
        if (dot == std::string::npos) name = column(schema, name);
        else name = name.substr(0, dot + 1) + column(schema, name.substr(dot + 1));
    }

    static void columns_in_place(const TableSchema* schema, std::vector<std::string>& names) {
        for (std::string& n : names) column_in_place(schema, n);
    }

    // The columns an expression of a statement that writes to one table names (the values of ON DUPLICATE KEY UPDATE, VALUES(col)): a plain name is a
    // column of that table.
    void arith_columns(const TableSchema* schema, ArithExpr& e) const {
        if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) {
            if (col->name.find('.') == std::string::npos && col->name.find('(') == std::string::npos) col->name = column(schema, col->name);
        } else if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) { arith_columns(schema, *v->lhs); arith_columns(schema, *v->rhs); }
        else if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) { arith_columns(schema, *v->lhs); arith_columns(schema, *v->rhs); }
        else if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) { arith_columns(schema, *v->lhs); arith_columns(schema, *v->rhs); }
        else if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) { arith_columns(schema, *v->lhs); arith_columns(schema, *v->rhs); }
        else if (auto* v = std::get_if<ArithExpr::Func>(&e.data)) {
            for (auto& a : v->args) arith_columns(schema, a);
        }
    }

    // --- the statements nested in expressions ---------------------------------------------------------------------------------------------------
    void arith(ArithExpr& e) {
        if (auto* sq = std::get_if<ArithExpr::Subquery>(&e.data)) {
            if (sq->query) statement(*sq->query);
        } else if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) { arith(*v->lhs); arith(*v->rhs); }
        else if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) { arith(*v->lhs); arith(*v->rhs); }
        else if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) { arith(*v->lhs); arith(*v->rhs); }
        else if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) { arith(*v->lhs); arith(*v->rhs); }
        else if (auto* v = std::get_if<ArithExpr::Cmp>(&e.data)) { arith(*v->lhs); arith(*v->rhs); }
        else if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) {
            for (auto& a : f->args) arith(a);
        } else if (auto* p = std::get_if<ArithExpr::Pred>(&e.data)) {
            if (p->cond) cond(*p->cond);
        }
    }

    void cond(CondExpr& e) {
        if (auto* a = std::get_if<CondExpr::And>(&e.data)) { cond(*a->lhs); cond(*a->rhs); }
        else if (auto* o = std::get_if<CondExpr::Or>(&e.data)) { cond(*o->lhs); cond(*o->rhs); }
        else if (auto* n = std::get_if<CondExpr::Not>(&e.data)) cond(*n->inner);
        else if (auto* leaf = std::get_if<CondExpr::Leaf>(&e.data)) {
            Condition& c = leaf->condition;
            arith(c.left);
            if (auto* sq = std::get_if<ConditionValue::Subquery>(&c.value.data)) {
                if (sq->query) statement(*sq->query);
            } else if (auto* value = std::get_if<ConditionValue::Arith>(&c.value.data)) {
                arith(value->expr);
            }
        }
    }

    void select_columns(std::vector<SelectColumn>& columns) {
        for (SelectColumn& c : columns) {
            if (auto* ex = std::get_if<SelectColumn::Expr>(&c.data)) arith(ex->expr);
            else if (auto* cw = std::get_if<SelectColumn::CaseWhen>(&c.data)) {
                for (auto& b : cw->branches) cond(b.condition);
            } else if (auto* agg = std::get_if<SelectColumn::Agg>(&c.data)) {
                if (agg->filter) cond(*agg->filter);
            } else if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) {
                if (aa->filter) cond(*aa->filter);
            } else if (auto* sq = std::get_if<SelectColumn::Subquery>(&c.data)) {
                if (sq->query) statement(*sq->query);
            }
        }
    }

    void join(Join& j) {
        if (j.subquery) statement(*j.subquery->first);
        else table_in_place(j.table);
        cond(j.on_expr);
    }

    void aliases(std::vector<std::pair<std::string, std::string>>& list) const {
        for (auto& entry : list) entry.second = table(entry.second);
    }

    // ORDER BY items of an UPDATE / DELETE: columns of its table
    static void order_by(const TableSchema* schema, std::vector<OrderBy>& items) {
        for (OrderBy& o : items) {
            if (plain_reference(o.column)) column_in_place(schema, o.column);
        }
    }

    void conflict(const TableSchema* schema, InsertConflict& on) {
        if (auto* update = std::get_if<InsertConflict::Update>(&on.data)) {
            for (auto& [target, value] : update->assignments) {
                column_in_place(schema, target);
                arith_columns(schema, value);
                arith(value);
            }
        }
    }

    void alter(const std::string& table_name, AlterAction& action) {
        const TableSchema* schema = schema_of(table_name);
        if (auto* v = std::get_if<AlterAction::DropColumn>(&action.data)) column_in_place(schema, v->name);
        else if (auto* v = std::get_if<AlterAction::RenameColumn>(&action.data)) column_in_place(schema, v->from);
        else if (auto* v = std::get_if<AlterAction::ModifyColumn>(&action.data)) column_in_place(schema, v->column.name);
        else if (auto* v = std::get_if<AlterAction::RenameTable>(&action.data)) v->to = table(v->to);
        else if (auto* v = std::get_if<AlterAction::AddForeignKey>(&action.data)) {
            column_in_place(schema, v->column);
            table_in_place(v->ref_table);
            column_in_place(schema_of(v->ref_table), v->ref_column);
        } else if (auto* v = std::get_if<AlterAction::AddUniqueConstraint>(&action.data)) {
            column_in_place(schema, v->column);
            columns_in_place(schema, v->columns);
        }
    }

    void create_table(Statement::CreateTable& ct) {
        table_in_place(ct.name);
        // the columns the statement declares are what its own lists (PRIMARY KEY (...), UNIQUE (...), KEY (...)) name
        TableSchema own;
        for (const ColumnDef& c : ct.columns) {
            ColumnDef copy;
            copy.name = c.name;
            own.columns.push_back(std::move(copy));
        }
        columns_in_place(&own, ct.primary_key_columns);
        for (UniqueKey& k : ct.unique_keys) columns_in_place(&own, k.columns);
        for (UniqueKey& k : ct.indexes) columns_in_place(&own, k.columns);
        if (ct.partition_by) column_in_place(&own, ct.partition_by->column);
        for (ColumnDef& c : ct.columns) {
            if (!c.foreign_key) continue;
            column_in_place(&own, c.foreign_key->column);
            table_in_place(c.foreign_key->ref_table);
            if (ieq(strip(c.foreign_key->ref_table), strip(ct.name))) column_in_place(&own, c.foreign_key->ref_column);
            else column_in_place(schema_of(c.foreign_key->ref_table), c.foreign_key->ref_column);
        }
    }

    static std::string strip(const std::string& name) {
        const auto dot = name.rfind('.');
        return dot == std::string::npos ? name : name.substr(dot + 1);
    }

    // --- the statements -------------------------------------------------------------------------------------------------------------------------
    void statement(Statement& st) {
        if (auto* v = std::get_if<Statement::Select>(&st.data)) {
            if (v->subquery) statement(*v->subquery->first);
            else if (v->table != "_dual_" && !(v->table.size() > 7 && v->table.compare(v->table.size() - 7, 7, "._dual_") == 0)) table_in_place(v->table);
            aliases(v->table_aliases);
            for (Join& j : v->joins) join(j);
            select_columns(v->columns);
            if (v->condition) cond(*v->condition);
            if (v->having) cond(*v->having);
        } else if (auto* v = std::get_if<Statement::Union>(&st.data)) { statement(*v->left); statement(*v->right); }
        else if (auto* v = std::get_if<Statement::Intersect>(&st.data)) { statement(*v->left); statement(*v->right); }
        else if (auto* v = std::get_if<Statement::Except>(&st.data)) { statement(*v->left); statement(*v->right); }
        else if (auto* v = std::get_if<Statement::With>(&st.data)) {
            const std::size_t before = ctes.size();
            for (auto& cte : v->ctes) {
                ctes.push_back(cte.first);
                statement(*cte.second);
            }
            if (v->query) statement(*v->query);
            ctes.resize(before);
        } else if (auto* v = std::get_if<Statement::Insert>(&st.data)) {
            table_in_place(v->table);
            const TableSchema* schema = schema_of(v->table);
            if (v->columns) columns_in_place(schema, *v->columns);
            conflict(schema, v->on_conflict);
        } else if (auto* v = std::get_if<Statement::InsertSelect>(&st.data)) {
            table_in_place(v->table);
            const TableSchema* schema = schema_of(v->table);
            if (v->columns) columns_in_place(schema, *v->columns);
            conflict(schema, v->on_conflict);
            if (v->query) statement(*v->query);
        } else if (auto* v = std::get_if<Statement::Update>(&st.data)) {
            table_in_place(v->table);
            const TableSchema* schema = schema_of(v->table);
            aliases(v->table_aliases);
            for (auto& [target, value] : v->assignments) {
                column_in_place(schema, target);
                arith(value);
            }
            if (v->condition) cond(*v->condition);
            order_by(schema, v->order_by);
        } else if (auto* v = std::get_if<Statement::Delete>(&st.data)) {
            table_in_place(v->table);
            const TableSchema* schema = schema_of(v->table);
            aliases(v->table_aliases);
            if (v->condition) cond(*v->condition);
            order_by(schema, v->order_by);
        } else if (auto* v = std::get_if<Statement::MultiUpdate>(&st.data)) {
            for (std::string& t : v->tables) table_in_place(t);
            for (Join& j : v->joins) join(j);
            aliases(v->table_aliases);
            for (auto& entry : v->assignments) arith(entry.second);
            if (v->condition) cond(*v->condition);
        } else if (auto* v = std::get_if<Statement::MultiDelete>(&st.data)) {
            for (std::string& t : v->delete_tables) table_in_place(t);
            table_in_place(v->from_table);
            for (Join& j : v->joins) join(j);
            aliases(v->table_aliases);
            if (v->condition) cond(*v->condition);
        } else if (auto* v = std::get_if<Statement::Merge>(&st.data)) {
            table_in_place(v->target);
            table_in_place(v->source);
            cond(v->on);
        } else if (auto* v = std::get_if<Statement::CreateTable>(&st.data)) create_table(*v);
        else if (auto* v = std::get_if<Statement::CreateIndex>(&st.data)) {
            table_in_place(v->table);
            columns_in_place(schema_of(v->table), v->columns);
        } else if (auto* v = std::get_if<Statement::AlterTable>(&st.data)) {
            table_in_place(v->table);
            alter(v->table, v->action);
        } else if (auto* v = std::get_if<Statement::CreateView>(&st.data)) {
            table_in_place(v->name);
            if (v->query) statement(*v->query);
        } else if (auto* v = std::get_if<Statement::DropTable>(&st.data)) table_in_place(v->name);
        else if (auto* v = std::get_if<Statement::TruncateTable>(&st.data)) table_in_place(v->name);
        else if (auto* v = std::get_if<Statement::DropView>(&st.data)) table_in_place(v->name);
        else if (auto* v = std::get_if<Statement::DropIndex>(&st.data)) { if (v->table) table_in_place(*v->table); }
        else if (auto* v = std::get_if<Statement::Describe>(&st.data)) table_in_place(v->table);
        else if (auto* v = std::get_if<Statement::ShowCreateTable>(&st.data)) table_in_place(v->table);
        else if (auto* v = std::get_if<Statement::ShowCreateView>(&st.data)) table_in_place(v->view);
        else if (auto* v = std::get_if<Statement::ShowIndex>(&st.data)) table_in_place(v->table);
        else if (auto* v = std::get_if<Statement::AnalyzeTable>(&st.data)) table_in_place(v->table);
        else if (auto* v = std::get_if<Statement::Vacuum>(&st.data)) { if (v->table) table_in_place(*v->table); }
        else if (auto* v = std::get_if<Statement::LockTables>(&st.data)) {
            for (auto& entry : v->tables) table_in_place(entry.first);
        } else if (auto* v = std::get_if<Statement::CreateTrigger>(&st.data)) table_in_place(v->table);
        else if (auto* v = std::get_if<Statement::Use>(&st.data)) v->database = database(v->database);
        else if (auto* v = std::get_if<Statement::DropDatabase>(&st.data)) v->name = database(v->name);
        else if (auto* v = std::get_if<Statement::CreateDatabase>(&st.data)) v->name = database(v->name);
        else if (auto* v = std::get_if<Statement::Explain>(&st.data)) { if (v->inner) statement(*v->inner); }
        else if (auto* v = std::get_if<Statement::ExplainAnalyze>(&st.data)) { if (v->inner) statement(*v->inner); }
        else if (auto* v = std::get_if<Statement::SelectInto>(&st.data)) { if (v->query) statement(*v->query); }
    }
};

} // namespace

void Executor::resolve_names(Statement& stmt) const {
    auto s = shared->read();
    resolve_names(*s, stmt);
}

void Executor::resolve_names(const SharedDatabase& s, Statement& stmt) const {
    NameResolver resolver{s, current_db, {}};
    resolver.statement(stmt);
}

} // namespace engine
