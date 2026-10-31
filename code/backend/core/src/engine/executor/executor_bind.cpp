// Binding: what the names of a statement refer to. It runs once for every statement before it is executed (execute_with_s, after the names are
// qualified) and does two things.
//
// Unknown columns. A column name that none of the statement's tables has used to be accepted: SELECT gave an empty column, WHERE found no
// row, ORDER BY sorted by nothing and a typo in a query looked like "no data". MySQL answers error 1054, "Unknown column 'x' in
// 'where clause'", and so does this check, for each top-level statement.
//
// What is checked is what is certain to be a column: a plain `name`, `table.name` or `db.table.name` in the select list, ON, WHERE, GROUP BY,
// HAVING and ORDER BY of a SELECT (through subqueries, with the columns of the enclosing queries in sight), and in the WHERE and the SET
// expressions of an UPDATE / DELETE. What cannot be told from a literal (the right-hand side of a comparison that is not quoted) or is not a
// column at all (function calls, a JSON path, `@variable`, a number) is not. A table the catalog does not know -- an information_schema
// table, a derived table whose columns cannot be named -- accepts any column name.
//
// Value classes. Values are stored as text, but MySQL compares by type: two strings as strings ('10' < '9', '007' and '7' differ), a number
// and anything else as numbers. So every column reference, every comparison, every ORDER BY column and every aggregate argument gets the
// class its declared type (or its quotes, or its operators) gives it -- the evaluators read it (see value_class.hpp). A column the binder
// cannot place (a function result of no fixed type, a derived column of a mixed expression) stays Unknown and compares the old way.

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

#include "engine/column_text.hpp"
#include "engine/executor/executor.hpp"
#include "engine/parser/parser.hpp"

namespace engine {

namespace {

struct BindTable {
    std::string full; // as the engine names it ("db.t"), or the alias of a derived table / a table used twice
    std::string bare; // after the last dot
    bool open = false; // any column name is accepted
    const TableSchema* schema = nullptr;
    std::vector<std::string> own_columns; // a derived table's (a view's, a CTE's)
    std::vector<ValueClass> own_classes;  // what each of them holds
};

struct BindScope {
    std::vector<BindTable> tables;
    std::unordered_set<std::string> aliases; // the names the select list gives, which GROUP BY / HAVING / ORDER BY may use
    std::unordered_map<std::string, ValueClass> alias_classes;
    std::vector<std::pair<std::string, std::string>> table_aliases; // alias -> table name, as the parser expanded them (Select::table_aliases)
    // an UPDATE / DELETE keeps the aliases of its tables in the SET expressions (the parser expands them in the WHERE only), so a
    // qualifier that names no table may still be one of them
    bool lenient_qualifier = false;
    std::unordered_set<std::string> merged;   // the columns a USING / NATURAL join makes one: a name two tables have, and not ambiguous
    std::unordered_set<std::string> selected; // the names the select list gives its plain columns: ORDER BY / HAVING read them before the tables'
};

std::string last_part(const std::string& qualified) {
    auto cut = qualified.rfind('.');
    return cut == std::string::npos ? qualified : qualified.substr(cut + 1);
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

bool has_column(const BindTable& t, const std::string& column) {
    if (t.open) return true;
    if (t.schema) return std::any_of(t.schema->columns.begin(), t.schema->columns.end(), [&](const ColumnDef& c) { return c.name == column; });
    return std::find(t.own_columns.begin(), t.own_columns.end(), column) != t.own_columns.end();
}

bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

// The spelling the table has for a column that is called `column` apart from case; nothing when it has none or two that differ.
std::optional<std::string> column_ci(const BindTable& t, const std::string& column) {
    if (t.open) return std::nullopt;
    std::optional<std::string> found;
    auto consider = [&](const std::string& name) {
        if (!ieq(name, column)) return true;
        if (found && *found != name) return false;
        found = name;
        return true;
    };
    if (t.schema) {
        for (const ColumnDef& c : t.schema->columns) {
            if (!consider(c.name)) return std::nullopt;
        }
    } else {
        for (const std::string& name : t.own_columns) {
            if (!consider(name)) return std::nullopt;
        }
    }
    return found;
}

// what a column of a table is besides text or number: a date, a time, an ENUM or a SET (see ArithExpr::Col)
void describe_column(ArithExpr::Col& col, const ColumnDef* def) {
    col.kind = def ? kind_of_type(def->data_type) : ColKind::Plain;
    col.members.reset();
    col.fsp = 0;
    col.single = def && std::holds_alternative<DataType::Float>(def->data_type.data);
    if (!def) return;
    if (auto* e = std::get_if<DataType::Enum>(&def->data_type.data)) col.members = std::make_shared<const std::vector<std::string>>(e->values);
    else if (auto* s = std::get_if<DataType::Set>(&def->data_type.data)) col.members = std::make_shared<const std::vector<std::string>>(s->values);
    else if (auto* dt = std::get_if<DataType::DateTime>(&def->data_type.data)) col.fsp = dt->fsp;
    else if (auto* ts = std::get_if<DataType::Timestamp>(&def->data_type.data)) col.fsp = ts->fsp;
    else if (auto* tm = std::get_if<DataType::Time>(&def->data_type.data)) col.fsp = tm->fsp;
}

ValueClass column_class(const BindTable& t, const std::string& column, bool* approx = nullptr, const ColumnDef** def = nullptr) {
    if (t.schema) {
        for (auto& c : t.schema->columns) {
            if (c.name != column) continue;
            if (approx) *approx = std::holds_alternative<DataType::Float>(c.data_type.data) || std::holds_alternative<DataType::Double>(c.data_type.data);
            if (def) *def = &c;
            return class_of_type(c.data_type);
        }
        return ValueClass::Unknown;
    }
    for (std::size_t i = 0; i < t.own_columns.size() && i < t.own_classes.size(); i++) {
        if (t.own_columns[i] == column) return t.own_classes[i];
    }
    return ValueClass::Unknown;
}

ValueClass aggregate_function_class(const AggFunc& f) {
    if (std::holds_alternative<AggFunc::GroupConcat>(f.data) || std::holds_alternative<AggFunc::JsonAgg>(f.data) ||
        std::holds_alternative<AggFunc::ArrayAgg>(f.data)) {
        return ValueClass::Text;
    }
    if (std::holds_alternative<AggFunc::Min>(f.data) || std::holds_alternative<AggFunc::Max>(f.data)) return ValueClass::Unknown; // the argument's
    return ValueClass::Number;
}

// What a window function gives: the rank / count / sum kinds are numbers, the others (FIRST_VALUE, LAG, MIN ...) the class of their column.
ValueClass window_function_class(const SelectColumn::WinFunc& w) {
    switch (w.func) {
        case WindowFunc::RowNumber: case WindowFunc::Rank: case WindowFunc::DenseRank: case WindowFunc::Ntile: case WindowFunc::PercentRank:
        case WindowFunc::CumeDist: case WindowFunc::Sum: case WindowFunc::Avg: case WindowFunc::Count:
            return ValueClass::Number;
        default:
            return w.col_class;
    }
}

} // namespace

std::optional<std::string> Executor::bind_statement(SharedDatabase& s, Statement& stmt, bool check) {
    struct Binder {
        Executor& ex;
        SharedDatabase& s;
        bool check;
        std::vector<const BindScope*> chain; // the scopes of the enclosing queries, outermost first, then this one
        std::optional<std::string> error;
        std::unordered_map<std::string, BindTable> ctes;
        std::vector<std::pair<std::string, ValueClass>> outputs; // what the last query bound gives, column by column

        // `qualifier_is_table`: false for the text of a comparison's right-hand side, where `x.y` is a column only if x is a table of the query
        bool known(const std::string& name, bool qualifier_is_table = true) const {
            if (!plain_reference(name)) return true;
            const std::size_t dot = name.rfind('.');
            if (dot != std::string::npos) {
                const std::string qualifier = name.substr(0, dot), column = name.substr(dot + 1);
                for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                    for (const BindTable& t : (*it)->tables) {
                        if (t.full == qualifier || t.bare == qualifier) return has_column(t, column);
                    }
                }
                // no table of that name: in a subquery it may be the alias of a table of the query around it (the parser does not
                // expand an outer alias inside a subquery), in a write statement one of its own
                for (const BindScope* sc : chain) {
                    for (auto& [alias, table] : sc->table_aliases) {
                        if (alias != qualifier) continue;
                        for (const BindScope* owner : chain) {
                            for (const BindTable& t : owner->tables) {
                                if (t.full == table || t.bare == table) return has_column(t, column);
                            }
                        }
                        return true;
                    }
                }
                if (!qualifier_is_table) return true;
                return std::any_of(chain.begin(), chain.end(), [](const BindScope* sc) { return sc->lenient_qualifier; });
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                if ((*it)->aliases.count(name)) return true;
                for (const BindTable& t : (*it)->tables) {
                    if (has_column(t, name)) return true;
                }
            }
            return false;
        }

        // A column reference spelled in another case than the table has it (`NAME` for `name`, `E.Id` for `e.id`, a select-list alias in capitals): the
        // spelling the tables and the select list give, or `n` itself when it is right as it is or cannot be told (no column of that name, or two that differ
        // only in case). MySQL does not tell the cases apart for a column, an alias or a table alias.
        // (is `n` a column of a table in sight, a table alias's column or a name the select list gives, exactly as it is spelled)
        bool exact(const std::string& n) const {
            const std::size_t dot = n.rfind('.');
            if (dot != std::string::npos) {
                const std::string qualifier = n.substr(0, dot), column = n.substr(dot + 1);
                for (const BindScope* sc : chain) {
                    for (const BindTable& t : sc->tables) {
                        if ((t.full == qualifier || t.bare == qualifier) && has_column(t, column)) return true;
                    }
                    for (auto& [alias, table] : sc->table_aliases) {
                        if (alias != qualifier) continue;
                        for (const BindScope* owner : chain) {
                            for (const BindTable& t : owner->tables) {
                                if ((t.full == table || t.bare == table) && has_column(t, column)) return true;
                            }
                        }
                    }
                }
                return false;
            }
            for (const BindScope* sc : chain) {
                if (sc->aliases.count(n)) return true;
                for (const BindTable& t : sc->tables) {
                    if (has_column(t, n)) return true;
                }
            }
            return false;
        }

        std::string canonical(const std::string& n) const {
            if (!plain_reference(n) || exact(n)) return n;
            const std::size_t dot = n.rfind('.');
            if (dot != std::string::npos) {
                const std::string qualifier = n.substr(0, dot), column = n.substr(dot + 1);
                for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                    for (const BindTable& t : (*it)->tables) {
                        std::string spelled;
                        if (ieq(t.full, qualifier)) spelled = t.full;
                        else if (ieq(t.bare, qualifier)) spelled = t.bare;
                        else continue;
                        if (auto c = column_ci(t, column)) return spelled + "." + *c;
                    }
                    for (auto& [alias, table] : (*it)->table_aliases) {
                        if (!ieq(alias, qualifier)) continue;
                        for (const BindScope* owner : chain) {
                            for (const BindTable& t : owner->tables) {
                                if (!ieq(t.full, table) && !ieq(t.bare, table)) continue;
                                if (auto c = column_ci(t, column)) return alias + "." + *c;
                            }
                        }
                    }
                }
                return n;
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                std::optional<std::string> found;
                bool two = false;
                for (const BindTable& t : (*it)->tables) {
                    if (auto c = column_ci(t, n)) {
                        if (found && *found != *c) two = true;
                        found = c;
                    }
                }
                if (found && !two) return *found;
                for (const std::string& alias : (*it)->aliases) {
                    if (ieq(alias, n)) return alias;
                }
            }
            return n;
        }

        // Does an expression give a DOUBLE? A FLOAT / DOUBLE column does, a text (MySQL reads it as a double), a function that works in floating point;
        // arithmetic gives one when an operand does.
        bool expression_is_approx(const ArithExpr& e) const {
            if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) {
                if (col->outer != 0 || !plain_reference(col->name)) return false;
                bool approx = false;
                const ValueClass k = class_of_name(col->name, &approx);
                return approx || k == ValueClass::Text;
            }
            if (auto* str = std::get_if<ArithExpr::Str>(&e.data)) return str->value != "NULL";
            if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) return expression_is_approx(*v->lhs) || expression_is_approx(*v->rhs);
            if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) return expression_is_approx(*v->lhs) || expression_is_approx(*v->rhs);
            if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) return expression_is_approx(*v->lhs) || expression_is_approx(*v->rhs);
            if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) return expression_is_approx(*v->lhs) || expression_is_approx(*v->rhs);
            if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) {
                static const std::unordered_set<std::string> floating = {"SQRT", "POW", "POWER", "EXP", "LN", "LOG", "LOG2", "LOG10", "SIN", "COS", "TAN", "ASIN",
                                                                         "ACOS", "ATAN", "ATAN2", "COT", "DEGREES", "RADIANS", "RAND", "PI"};
                if (floating.count(upper(f->name))) return true;
                for (const ArithExpr& a : f->args) {
                    if (expression_is_approx(a)) return true;
                }
            }
            return false;
        }

        // The argument of an aggregate as the query spells it (`d`, `t.d`, `DISTINCT d`, `price * qty`): is it a DOUBLE? Then SUM and AVG of it are doubles.
        bool argument_is_approx(const std::string& argument) const {
            std::string arg = argument;
            if (arg.rfind("DISTINCT ", 0) == 0) arg.erase(0, 9);
            if (arg.empty() || arg == "*") return false;
            try {
                return expression_is_approx(Parser::str_to_arith(arg));
            } catch (const ParseError&) {
                return false;
            }
        }

        // The argument of an aggregate as the query spells it: is it one FLOAT column (single precision)? MIN and MAX of it are shown as a FLOAT is, with six digits.
        bool argument_is_single(const std::string& argument) const {
            std::string arg = argument;
            if (arg.rfind("DISTINCT ", 0) == 0) arg.erase(0, 9);
            if (arg.empty() || arg == "*" || !plain_reference(arg)) return false;
            const ColumnDef* def = nullptr;
            bool approx = false;
            class_of_name(arg, &approx, &def);
            return def && std::holds_alternative<DataType::Float>(def->data_type.data);
        }

        // What `AGG(arg)` -- the text a HAVING or a select-list expression uses for an aggregate -- gives; nullopt when `name` is not one.
        // `approx`, when given, is told whether the aggregate's value is a DOUBLE (its argument is, or it is STDDEV / VARIANCE).
        std::optional<ValueClass> aggregate_reference_class(const std::string& name, bool* approx = nullptr) const {
            const std::size_t lp = name.find('('), rp = name.rfind(')');
            if (lp == std::string::npos || rp == std::string::npos || rp != name.size() - 1 || lp == 0) return std::nullopt;
            const std::string function = upper(name.substr(0, lp));
            if (approx && (function == "MIN" || function == "MAX" || function == "SUM" || function == "AVG")) *approx = argument_is_approx(name.substr(lp + 1, rp - lp - 1));
            if (approx && (function == "STDDEV" || function == "VARIANCE")) *approx = true;
            if (function == "MIN" || function == "MAX") {
                std::string arg = name.substr(lp + 1, rp - lp - 1);
                if (arg.rfind("DISTINCT ", 0) == 0) arg.erase(0, 9);
                return class_of_name(arg);
            }
            if (function == "GROUP_CONCAT" || function == "JSON_AGG" || function == "ARRAY_AGG") return ValueClass::Text;
            if (function == "COUNT" || function == "SUM" || function == "AVG" || function == "STDDEV" || function == "VARIANCE" ||
                function == "MEDIAN" || function == "BIT_AND" || function == "BIT_OR") {
                return ValueClass::Number;
            }
            return std::nullopt;
        }

        // What the column `name` (`name`, `t.name`, `db.t.name`), a name the select list gives, or an aggregate reference holds.
        // `approx`: told whether it is a FLOAT / DOUBLE column
        ValueClass class_of_name(const std::string& name, bool* approx = nullptr, const ColumnDef** def = nullptr) const {
            if (approx) *approx = false;
            if (def) *def = nullptr;
            if (auto agg = aggregate_reference_class(name, approx)) return *agg;
            if (!plain_reference(name)) return ValueClass::Unknown;
            const std::size_t dot = name.rfind('.');
            if (dot != std::string::npos) {
                const std::string qualifier = name.substr(0, dot), column = name.substr(dot + 1);
                for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                    for (const BindTable& t : (*it)->tables) {
                        if (t.full == qualifier || t.bare == qualifier) return column_class(t, column, approx, def);
                    }
                }
                return ValueClass::Unknown;
            }
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                if (auto alias = (*it)->alias_classes.find(name); alias != (*it)->alias_classes.end()) return alias->second;
                for (const BindTable& t : (*it)->tables) {
                    if (!t.open && has_column(t, name)) return column_class(t, name, approx, def);
                }
            }
            return ValueClass::Unknown;
        }

        // `SUM(SAL)` / `COUNT(DISTINCT dept)` as a HAVING or an expression names an aggregate: a plain column in it spelled as the tables have it (the
        // aggregate of an expression keeps the spelling it was typed in)
        std::string canonical_aggregate(const std::string& ref) const {
            const std::size_t lp = ref.find('('), rp = ref.rfind(')');
            if (lp == std::string::npos || lp == 0 || rp != ref.size() - 1) return ref;
            std::string arg = ref.substr(lp + 1, rp - lp - 1), distinct;
            if (arg.rfind("DISTINCT ", 0) == 0) { distinct = "DISTINCT "; arg.erase(0, 9); }
            if (arg.empty() || !plain_reference(arg)) return ref;
            const std::string spelled = canonical(arg);
            return spelled == arg ? ref : ref.substr(0, lp + 1) + distinct + spelled + ")";
        }

        void name(std::string& n, const char* clause) {
            n = canonical(n);
            if (check && !error && !known(n)) error = "Unknown column '" + n + "' in '" + clause + "'";
            else if (check && !error && ambiguous(n, clause)) error = "Column '" + n + "' in " + clause + " is ambiguous";
        }

        // An unqualified column that two tables of the innermost query that has it both have (MySQL 1052: it used to be the first table's). The
        // columns a USING / NATURAL join makes one are not; ORDER BY and HAVING read the names the select list gives first.
        bool ambiguous(const std::string& n, const char* clause) const {
            if (!plain_reference(n) || n.find('.') != std::string::npos) return false;
            const std::string where = clause;
            const bool reads_select_list = where == "order clause" || where == "having clause";
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                const BindScope& scope = **it;
                std::size_t count = 0;
                for (const BindTable& t : scope.tables) {
                    if (!t.open && has_column(t, n)) count++;
                }
                if (count == 0) continue;
                if (count < 2 || scope.merged.count(n)) return false;
                if (reads_select_list && (scope.selected.count(n) || scope.aliases.count(n) ||
                                          std::any_of(scope.selected.begin(), scope.selected.end(), [&](const std::string& x) { return ieq(x, n); }))) return false;
                return true;
            }
            return false;
        }

        // Where a column of an enclosing query lives: how many queries out, and the name that query's rows hold it under (the table it
        // belongs to, then the column). Nothing for a column of this query's own tables -- they come first, as in MySQL -- or of no table.
        // The alias of a table of an enclosing query is read through that query's table_aliases (the parser expands it only in the query
        // that declares it).
        std::optional<std::pair<int, std::string>> outer_column(const std::string& name) const {
            if (chain.size() < 2 || !plain_reference(name)) return std::nullopt;
            const std::size_t own = chain.size() - 1;
            const std::size_t dot = name.rfind('.');
            for (std::size_t i = chain.size(); i-- > 0;) {
                const BindScope& scope = *chain[i];
                if (dot != std::string::npos) {
                    std::string qualifier = name.substr(0, dot);
                    for (auto& [alias, table] : scope.table_aliases) {
                        if (alias == qualifier) { qualifier = table; break; }
                    }
                    for (const BindTable& t : scope.tables) {
                        if (t.full != qualifier && t.bare != qualifier) continue;
                        if (i == own) return std::nullopt;
                        return std::make_pair(static_cast<int>(own - i), t.full + "." + name.substr(dot + 1));
                    }
                } else {
                    if (i == own && scope.aliases.count(name)) return std::nullopt;
                    for (const BindTable& t : scope.tables) {
                        if (!has_column(t, name)) continue;
                        if (i == own || t.open) return std::nullopt; // (a table that takes any name cannot be told from a column of the query)
                        return std::make_pair(static_cast<int>(own - i), t.full + "." + name);
                    }
                }
            }
            return std::nullopt;
        }

        // `name` is a column of an enclosing query: it is renamed as that query's rows have it and `outer` says how far out the query is.
        void mark_outer(std::string& name, int& outer) const {
            outer = 0;
            if (auto o = outer_column(name)) {
                outer = o->first;
                name = o->second;
            }
        }

        // The places that keep a column as text -- an aggregate's argument, a sort or group item -- have no way to hold the value of a column
        // of an enclosing query, so a subquery that names one there is refused rather than answered wrongly.
        void no_outer(const std::string& n, const char* clause) {
            if (check && !error && outer_column(n)) error = "Outer reference '" + n + "' is not supported in '" + clause + "'";
        }

        void arith(ArithExpr& e, const char* clause) {
            if (error) return;
            if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) {
                if (col->name.find('(') != std::string::npos) col->name = canonical_aggregate(col->name);
                name(col->name, clause);
                mark_outer(col->name, col->outer);
                const ColumnDef* def = nullptr;
                col->cls = class_of_name(col->name, &col->approx, &def);
                describe_column(*col, def);
                if (col->kind == ColKind::Plain && col->name.find('(') != std::string::npos) {
                    if (const ColKind k = aggregate_reference_kind(col->name); k != ColKind::Plain) { col->kind = k; col->fsp = 255; }
                }
            }
            else if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) { arith(*v->lhs, clause); arith(*v->rhs, clause); }
            else if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) { arith(*v->lhs, clause); arith(*v->rhs, clause); }
            else if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) { arith(*v->lhs, clause); arith(*v->rhs, clause); }
            else if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) { arith(*v->lhs, clause); arith(*v->rhs, clause); }
            else if (auto* v = std::get_if<ArithExpr::Cmp>(&e.data)) { arith(*v->lhs, clause); arith(*v->rhs, clause); }
            else if (auto* v = std::get_if<ArithExpr::Pred>(&e.data)) cond(*v->cond, clause);
            else if (auto* v = std::get_if<ArithExpr::Subquery>(&e.data)) subquery_value(*v);
            else if (auto* v = std::get_if<ArithExpr::Func>(&e.data); v && v->name == "CASE") {
                for (auto& a : v->args) arith(a, clause); // (its conditions and results are expressions)
            } else if (std::holds_alternative<ArithExpr::Func>(e.data)) {
                // the arguments of any other function are not checked (they may name a unit or a type as well as a column), but a column in them
                // still holds what its type says: IFNULL(y, w) over two number columns is a number
                annotate(e);
            }
        }

        // what the columns in `e` hold, without asking for them to exist
        void annotate(ArithExpr& e) {
            if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) {
                col->name = canonical(col->name);
                mark_outer(col->name, col->outer);
                const ColumnDef* def = nullptr;
                col->cls = class_of_name(col->name, &col->approx, &def);
                describe_column(*col, def);
            }
            else if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) { annotate(*v->lhs); annotate(*v->rhs); }
            else if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) { annotate(*v->lhs); annotate(*v->rhs); }
            else if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) { annotate(*v->lhs); annotate(*v->rhs); }
            else if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) { annotate(*v->lhs); annotate(*v->rhs); }
            else if (auto* v = std::get_if<ArithExpr::Subquery>(&e.data)) subquery_value(*v);
            else if (auto* v = std::get_if<ArithExpr::Pred>(&e.data)) cond(*v->cond, "field list");
            else if (auto* v = std::get_if<ArithExpr::Func>(&e.data)) {
                // (the unit of DATE_ADD(d, INTERVAL n DAY) is a word, not a column)
                const bool unit_last = v->name == "DATE_ADD" || v->name == "DATE_SUB";
                for (std::size_t i = 0; i < v->args.size(); i++) {
                    if (unit_last && i + 1 == v->args.size()) continue;
                    annotate(v->args[i]);
                }
            }
        }

        // A scalar subquery used as a value: bound as a statement nested in this one (its references to this query are marked); it gives one column
        // (MySQL 1241), which is what the value holds.
        void subquery_value(ArithExpr::Subquery& sq) {
            if (!sq.query) return;
            auto out = nested(*sq.query);
            if (check && !error && out.size() > 1) error = "Operand should contain 1 column(s)";
            sq.cls = out.empty() ? ValueClass::Unknown : out.front().second;
        }

        static bool has_outer(const ArithExpr& e) {
            if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) return col->outer != 0;
            if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) return has_outer(*v->lhs) || has_outer(*v->rhs);
            if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) return has_outer(*v->lhs) || has_outer(*v->rhs);
            if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) return has_outer(*v->lhs) || has_outer(*v->rhs);
            if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) return has_outer(*v->lhs) || has_outer(*v->rhs);
            if (auto* v = std::get_if<ArithExpr::Cmp>(&e.data)) return has_outer(*v->lhs) || has_outer(*v->rhs);
            if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) {
                for (auto& a : f->args) {
                    if (has_outer(a)) return true;
                }
            }
            return false; // (a condition inside the expression is not looked into: the executor says so when it reads the text)
        }

        // Binds a statement nested in this one (a subquery, a derived table): the columns of this query are in sight of it. Its output
        // columns are returned.
        std::vector<std::pair<std::string, ValueClass>> nested(Statement& inner, bool sees_outer = true) {
            Binder sub{ex, s, check, sees_outer ? chain : std::vector<const BindScope*>{}, std::nullopt, ctes, {}};
            if (sees_outer) sub.write_targets = write_targets; // (a derived table is made before the statement writes: it may read the table)
            sub.statement(inner);
            if (sub.error && !error) error = sub.error;
            return std::move(sub.outputs);
        }

        void cond(CondExpr& e, const char* clause) {
            if (error) return;
            if (auto* a = std::get_if<CondExpr::And>(&e.data)) { cond(*a->lhs, clause); cond(*a->rhs, clause); }
            else if (auto* o = std::get_if<CondExpr::Or>(&e.data)) { cond(*o->lhs, clause); cond(*o->rhs, clause); }
            else if (auto* n = std::get_if<CondExpr::Not>(&e.data)) cond(*n->inner, clause);
            else if (auto* leaf = std::get_if<CondExpr::Leaf>(&e.data)) {
                Condition& c = leaf->condition;
                arith(c.left, clause);
                c.left_class = class_of_expr(c.left);
                c.right_class = ValueClass::Unknown;
                // `a.x = b.y`: the parser keeps the right side as text; a `table.column` of a table of the query is a column
                if (auto* lit = std::get_if<ConditionValue::Literal>(&c.value.data)) {
                    lit->outer = 0;
                    if (lit->quoted) {
                        c.right_class = ValueClass::Text;
                    } else if (parse_number(lit->value)) {
                        c.right_class = ValueClass::Number;
                    } else {
                        lit->value = canonical(lit->value);
                        if (check && !error && lit->value.find('.') != std::string::npos && !known(lit->value, false)) {
                            error = "Unknown column '" + lit->value + "' in '" + clause + "'";
                        } else if (check && !error && lit->value.find('.') == std::string::npos && table_column(lit->value) && ambiguous(lit->value, clause)) {
                            error = "Column '" + lit->value + "' in " + clause + " is ambiguous"; // (a bare word is a column only when a table has it)
                        }
                        mark_outer(lit->value, lit->outer);
                        bool right_approx = false;
                        const ColumnDef* right_def = nullptr;
                        c.right_class = class_of_name(lit->value, &right_approx, &right_def);
                        // a FLOAT column on the right is compared as the single-precision number: it is kept as an expression, which knows that (a name in the
                        // text of the condition does not)
                        if (lit->outer == 0 && right_def && std::holds_alternative<DataType::Float>(right_def->data_type.data)) {
                            ArithExpr::Col right{lit->value};
                            right.cls = c.right_class;
                            right.approx = right_approx;
                            describe_column(right, right_def);
                            c.value = ConditionValue(ConditionValue::Arith{ArithExpr(std::move(right))});
                        }
                    }
                } else if (auto* value = std::get_if<ConditionValue::Arith>(&c.value.data)) {
                    arith(value->expr, clause);
                    c.right_class = class_of_expr(value->expr);
                } else if (auto* sub = std::get_if<ConditionValue::Subquery>(&c.value.data)) {
                    if (sub->query) {
                        auto out = nested(*sub->query);
                        // a subquery compared with a value or listed after IN gives one column (EXISTS does not care), or as many as the row `(a, b)` it is
                        // compared with has; MySQL 1241
                        const auto* row_left = std::get_if<ArithExpr::Func>(&c.left.data);
                        const std::size_t wanted = row_left && row_left->name == "ROW" ? row_left->args.size() : 1;
                        if (check && !error && (wanted == 1 ? out.size() > 1 : out.size() != wanted) && c.op != Operator::Exists && c.op != Operator::NotExists) {
                            error = "Operand should contain " + std::to_string(wanted) + " column(s)";
                        }
                        if (!out.empty()) c.right_class = out.front().second;
                    }
                }
            }
        }

        BindTable table_of(const std::string& qualified_name, const std::string& as) {
            BindTable t;
            t.full = as.empty() ? qualified_name : as;
            t.bare = last_part(t.full);
            if (auto cte = ctes.find(qualified_name); cte != ctes.end()) {
                t.own_columns = cte->second.own_columns;
                t.own_classes = cte->second.own_classes;
                t.open = t.own_columns.empty();
            } else if (auto view = s.views.find(qualified_name); view != s.views.end()) {
                Statement body = view->second; // bound as a copy: the stored view stays as it is (and is not checked again here)
                Binder sub{ex, s, false, {}, std::nullopt, ctes, {}, {}};
                sub.statement(body);
                t.own_columns = ex.derived_column_names(s, body);
                for (auto& o : sub.outputs) t.own_classes.push_back(o.second);
                t.open = t.own_columns.empty() || t.own_columns.size() != t.own_classes.size();
            } else if (const TableSchema* schema = s.catalog.get_table(qualified_name)) {
                t.schema = schema;
            } else if (auto info = Executor::info_schema_bare(qualified_name)) {
                for (const InfoSchemaColumn& c : *Executor::info_schema_columns(*info)) { // (an information_schema table: its columns are known)
                    t.own_columns.push_back(c.name);
                    t.own_classes.push_back(c.numeric ? ValueClass::Number : ValueClass::Text);
                }
            } else {
                t.open = true; // a temporary table, ...
            }
            return t;
        }

        BindTable derived_of(const Statement& inner, const std::string& alias, const std::vector<std::pair<std::string, ValueClass>>& out) {
            BindTable t;
            t.full = t.bare = alias;
            t.own_columns = ex.derived_column_names(s, inner);
            for (auto& o : out) t.own_classes.push_back(o.second);
            t.open = t.own_columns.empty() || t.own_columns.size() != t.own_classes.size();
            return t;
        }

        // The argument of an aggregate that is an expression (`price * qty`): the columns in it have to exist; what it holds is what its operators say
        // (the executor reads the text again to compute it, so the expression bound here is a copy)
        // `canonical_text`, when given, is told the text of the expression with its columns spelled as the tables have them (what the executor reads).
        ValueClass expression_argument(const std::string& text, const char* clause, std::string* canonical_text = nullptr) {
            ArithExpr e = Parser::str_to_arith(text);
            if (auto* col = std::get_if<ArithExpr::Col>(&e.data); col && col->name == text) return ValueClass::Unknown; // (not readable: the executor says so)
            std::string before;
            try { before = Parser::arith_to_string(e); } catch (const ParseError&) {}
            arith(e, clause);
            if (check && !error && has_outer(e)) error = std::string("An outer reference inside '") + text + "' is not supported in '" + clause + "'";
            if (canonical_text && !before.empty()) {
                try {
                    const std::string after = Parser::arith_to_string(e);
                    if (after != before) *canonical_text = after;
                } catch (const ParseError&) {}
            }
            return class_of_expr(e);
        }

        // `MIN(d)` / `MAX(DATE(ts))` in an expression (the parser keeps it as a column of that name) is the date it gives: MAX(d) + 0 is 20240305
        ColKind aggregate_reference_kind(const std::string& name) {
            const std::size_t lp = name.find('('), rp = name.rfind(')');
            if (lp == std::string::npos || lp == 0 || rp != name.size() - 1) return ColKind::Plain;
            const std::string function = upper(name.substr(0, lp));
            if (function != "MIN" && function != "MAX") return ColKind::Plain;
            std::string arg = name.substr(lp + 1, rp - lp - 1);
            if (arg.rfind("DISTINCT ", 0) == 0) arg.erase(0, 9);
            if (arg.empty() || arg == "*") return ColKind::Plain;
            try {
                ArithExpr e = Parser::str_to_arith(arg);
                annotate(e);
                return kind_of_expr(e);
            } catch (const ParseError&) {
                return ColKind::Plain;
            }
        }

        // SUM and AVG of a date, a time, an ENUM, a SET or a date function add up the numbers they stand for (MySQL: SUM of the DATEs 2024-03-05 and 2024-01-01 is
        // 40480406), and of a FLOAT column the single-precision numbers widened to DOUBLEs. True when the argument `text` is one of those.
        // `approx`: whether the number they stand for is a DOUBLE (an ENUM, a SET, a FLOAT); a date or a time is a whole number (or a decimal with a fraction of a second)
        bool reads_number_view(const std::string& text, bool* approx) {
            if (text.empty() || text == "*" || text.rfind("DISTINCT ", 0) == 0 || text.rfind("__", 0) == 0) return false;
            try {
                ArithExpr e = Parser::str_to_arith(text);
                annotate(e);
                const ColKind kind = kind_of_expr(e);
                if (kind != ColKind::Plain) {
                    *approx = kind == ColKind::Enum || kind == ColKind::Set;
                    return true;
                }
                const auto* col = std::get_if<ArithExpr::Col>(&e.data);
                *approx = true;
                return col && col->single;
            } catch (const ParseError&) {
                return false;
            }
        }

        // The argument of an aggregate: a column, `*`, an expression, or for SUM / COUNT of a CASE (or of a condition: SUM(v > 1)) the conditions of
        // the CASE -- the parser leaves the placeholder "__case__" in the column then.
        // `label` is the argument as the query spells it (it names the result column and stays as typed); `source` is where a row holds it, which is
        // set here when that is another spelling.
        ValueClass aggregate(AggFunc& func, const std::string& label, std::string& source, std::optional<CondExpr>& filter, bool* approx = nullptr, bool* single = nullptr) {
            std::string column = source.empty() ? label : source;
            std::vector<CaseWhenBranch>* branches = nullptr;
            if (auto* counted = std::get_if<AggFunc::CountCase>(&func.data)) branches = &counted->branches;
            else if (auto* summed = std::get_if<AggFunc::SumCase>(&func.data)) branches = &summed->branches;
            ValueClass arg = ValueClass::Unknown;
            if (branches) {
                for (auto& b : *branches) cond(b.condition, "field list");
            } else if (is_expression_argument(column)) {
                std::string spelled;
                arg = expression_argument(column, "field list", &spelled);
                if (!spelled.empty()) source = spelled;
            } else {
                const std::string typed = column;
                name(column, "field list");
                no_outer(column, "field list");
                arg = class_of_name(column);
                if (column != typed) source = column;
            }
            if (filter) cond(*filter, "field list");
            if (approx) *approx = !branches && argument_is_approx(column);
            if (single) *single = !branches && (std::holds_alternative<AggFunc::Min>(func.data) || std::holds_alternative<AggFunc::Max>(func.data)) && argument_is_single(column);
            const bool adds = std::holds_alternative<AggFunc::Sum>(func.data) || std::holds_alternative<AggFunc::Avg>(func.data) ||
                              std::holds_alternative<AggFunc::SumDistinct>(func.data) || std::holds_alternative<AggFunc::AvgDistinct>(func.data);
            if (!branches && adds) {
                const std::string text = source.empty() ? label : source;
                bool number_is_double = false;
                if (reads_number_view(text, &number_is_double)) {
                    source = "(" + text + ") + 0"; // (the rows hold `arg + 0` under that text; the result column keeps the name as typed)
                    if (approx) *approx = number_is_double;
                }
            }
            return arg;
        }

        // What the select item gives (after it has been bound).
        ValueClass class_of_item(SelectColumn& c) {
            if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) return col->cls;
            if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) return ca->cls;
            if (auto* agg = std::get_if<SelectColumn::Agg>(&c.data)) {
                ValueClass k = aggregate_function_class(agg->func);
                return k == ValueClass::Unknown ? agg->arg_class : k;
            }
            if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) {
                ValueClass k = aggregate_function_class(aa->func);
                return k == ValueClass::Unknown ? aa->arg_class : k;
            }
            if (auto* fn = std::get_if<SelectColumn::Func>(&c.data)) return function_result_class(fn->name);
            if (auto* e = std::get_if<SelectColumn::Expr>(&c.data)) return class_of_expr(e->expr);
            if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data)) return window_function_class(*wf);
            return ValueClass::Unknown; // a CASE, a subquery
        }

        void select_column(SelectColumn& c) {
            if (error) return;
            if (auto* all = std::get_if<SelectColumn::All>(&c.data)) {
                // `T.*`: the table or alias apart from case
                if (!all->table.empty()) {
                    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                        for (const BindTable& t : (*it)->tables) {
                            if (t.full == all->table || t.bare == all->table) return;
                        }
                    }
                    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                        for (const BindTable& t : (*it)->tables) {
                            if (ieq(t.full, all->table)) { all->table = t.full; return; }
                            if (ieq(t.bare, all->table)) { all->table = t.bare; return; }
                        }
                    }
                }
            } else if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) {
                const std::string typed = col->name;
                name(col->name, "field list");
                mark_outer(col->name, col->outer);
                col->cls = class_of_name(col->name);
                if (col->name != typed && col->outer == 0 && plain_reference(typed)) {
                    // (the result column keeps the name as it was typed, as in MySQL)
                    SelectColumn::ColumnAlias renamed{col->name, last_part(typed), col->cls, col->outer};
                    c.data = std::move(renamed);
                }
            } else if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) {
                name(ca->name, "field list");
                mark_outer(ca->name, ca->outer);
                ca->cls = class_of_name(ca->name);
            }
            else if (auto* agg = std::get_if<SelectColumn::Agg>(&c.data)) agg->arg_class = aggregate(agg->func, agg->col, agg->source, agg->filter, &agg->arg_approx, &agg->arg_single);
            else if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) aa->arg_class = aggregate(aa->func, aa->col, aa->source, aa->filter, &aa->arg_approx, &aa->arg_single);
            else if (auto* ex_col = std::get_if<SelectColumn::Expr>(&c.data)) {
                // (the result column is named by the expression as it was typed, unless it has an alias)
                std::string typed;
                if (!ex_col->alias) {
                    try { typed = Parser::arith_to_string(ex_col->expr); } catch (const ParseError&) {}
                }
                arith(ex_col->expr, "field list");
                if (!typed.empty()) {
                    try {
                        if (Parser::arith_to_string(ex_col->expr) != typed) ex_col->alias = typed;
                    } catch (const ParseError&) {}
                }
            }
            else if (auto* cw = std::get_if<SelectColumn::CaseWhen>(&c.data)) {
                for (auto& b : cw->branches) cond(b.condition, "field list");
            } else if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data)) {
                if (wf->col) {
                    if (is_expression_argument(*wf->col)) {
                        std::string spelled;
                        wf->col_class = expression_argument(*wf->col, "field list", &spelled);
                        if (!spelled.empty()) *wf->col = spelled;
                    } else {
                        name(*wf->col, "field list");
                        no_outer(*wf->col, "field list");
                        wf->col_class = class_of_name(*wf->col);
                    }
                    wf->col_approx = argument_is_approx(*wf->col);
                    wf->col_single = (wf->func == WindowFunc::Min || wf->func == WindowFunc::Max || wf->func == WindowFunc::Lag || wf->func == WindowFunc::Lead ||
                                      wf->func == WindowFunc::FirstValue || wf->func == WindowFunc::LastValue || wf->func == WindowFunc::NthValue) && argument_is_single(*wf->col);
                    bool number_is_double = false;
                    if ((wf->func == WindowFunc::Sum || wf->func == WindowFunc::Avg) && reads_number_view(*wf->col, &number_is_double)) {
                        *wf->col = "(" + *wf->col + ") + 0";
                        wf->col_approx = number_is_double;
                    }
                }
                // (the items are columns, or expressions and aggregates of a query that groups: `OVER (ORDER BY SUM(v) DESC)`)
                for (auto& p : wf->partition_by) {
                    if (plain_reference(p)) { name(p, "field list"); no_outer(p, "field list"); }
                    else {
                        std::string spelled;
                        expression_argument(p, "field list", &spelled);
                        if (!spelled.empty()) p = spelled;
                    }
                }
                for (auto& o : wf->order_by) {
                    if (plain_reference(o.column)) { name(o.column, "field list"); no_outer(o.column, "field list"); o.cls = class_of_name(o.column); }
                    else {
                        std::string spelled;
                        o.cls = expression_argument(o.column, "field list", &spelled);
                        if (!spelled.empty()) o.column = spelled;
                    }
                }
            } else if (auto* sq = std::get_if<SelectColumn::Subquery>(&c.data)) {
                if (sq->query) {
                    auto out = nested(*sq->query);
                    if (check && !error && out.size() > 1) error = "Operand should contain 1 column(s)"; // a scalar subquery is one column (MySQL 1241)
                }
            }
        }

        // The columns `select * from <the scope>` gives, in the order derived_column_names lists them.
        void star_columns(const BindScope& scope, const std::string& only_table, std::vector<std::pair<std::string, ValueClass>>& out) const {
            for (const BindTable& t : scope.tables) {
                if (!only_table.empty() && t.full != only_table && t.bare != only_table) continue;
                if (t.schema) {
                    for (auto& c : t.schema->columns) out.emplace_back(c.name, class_of_type(c.data_type));
                } else {
                    for (std::size_t i = 0; i < t.own_columns.size(); i++) out.emplace_back(t.own_columns[i], i < t.own_classes.size() ? t.own_classes[i] : ValueClass::Unknown);
                }
            }
        }

        // HAVING reads ANY_VALUE(x) as it reads a plain `x`: a column of the select list (MySQL 1054 for one that is only in the table or in GROUP BY)
        void any_value_columns(const ArithExpr& e, std::vector<std::string>& out) const {
            if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) { any_value_columns(*v->lhs, out); any_value_columns(*v->rhs, out); }
            else if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) { any_value_columns(*v->lhs, out); any_value_columns(*v->rhs, out); }
            else if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) { any_value_columns(*v->lhs, out); any_value_columns(*v->rhs, out); }
            else if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) { any_value_columns(*v->lhs, out); any_value_columns(*v->rhs, out); }
            else if (auto* v = std::get_if<ArithExpr::Cmp>(&e.data)) { any_value_columns(*v->lhs, out); any_value_columns(*v->rhs, out); }
            else if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) {
                if (f->name == "ANY_VALUE" && f->args.size() == 1) {
                    if (auto* col = std::get_if<ArithExpr::Col>(&f->args[0].data); col && col->outer == 0 && plain_reference(col->name)) out.push_back(col->name);
                } else {
                    for (auto& a : f->args) any_value_columns(a, out);
                }
            }
        }

        void any_value_columns(const CondExpr& e, std::vector<std::string>& out) const {
            if (auto* a = std::get_if<CondExpr::And>(&e.data)) { any_value_columns(*a->lhs, out); any_value_columns(*a->rhs, out); }
            else if (auto* o = std::get_if<CondExpr::Or>(&e.data)) { any_value_columns(*o->lhs, out); any_value_columns(*o->rhs, out); }
            else if (auto* n = std::get_if<CondExpr::Not>(&e.data)) any_value_columns(*n->inner, out);
            else if (auto* leaf = std::get_if<CondExpr::Leaf>(&e.data)) {
                any_value_columns(leaf->condition.left, out);
                if (auto* value = std::get_if<ConditionValue::Arith>(&leaf->condition.value.data)) any_value_columns(value->expr, out);
            }
        }

        void having_any_value(const Statement::Select& sel, const CondExpr& having) {
            std::vector<std::string> names;
            any_value_columns(having, names);
            for (const std::string& name : names) {
                bool selected = false;
                for (const SelectColumn& c : sel.columns) {
                    const std::string* selected_name = nullptr;
                    if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) selected_name = &col->name;
                    else if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) selected_name = &ca->name;
                    else if (std::holds_alternative<SelectColumn::All>(c.data)) selected = true; // (`SELECT *` lists every column)
                    if (selected_name && ieq(last_part(*selected_name), last_part(name))) selected = true;
                }
                if (!selected) {
                    error = "Unknown column '" + name + "' in 'having clause'";
                    return;
                }
            }
        }

        void forbid_target(const BindTable& t) {
            if (!check || error || t.open) return;
            for (const auto& [target, shown] : write_targets) {
                if (ieq(target, t.full)) error = "You can't specify target table '" + shown + "' for update in FROM clause";
            }
        }

        void select(Statement::Select& sel) {
            BindScope scope;
            scope.table_aliases = sel.table_aliases;
            // the FROM list
            if (sel.subquery) {
                auto out = nested(*sel.subquery->first, false); // a derived table sees none of the columns around it
                if (error) return;
                scope.tables.push_back(derived_of(*sel.subquery->first, sel.subquery->second, out));
            } else if (sel.table == "_dual_" || (sel.table.size() > 7 && sel.table.compare(sel.table.size() - 7, 7, "._dual_") == 0)) {
                // no table
            } else {
                scope.tables.push_back(table_of(sel.table, sel.table_alias));
                forbid_target(scope.tables.back());
            }
            for (auto& j : sel.joins) {
                if (j.subquery && j.lateral) {
                    // (it sees the tables before it, as a subquery sees the query around it)
                    chain.push_back(&scope);
                    nested(*j.subquery->first);
                    chain.pop_back();
                    if (error) return;
                    BindTable open;
                    open.full = open.bare = j.table;
                    open.open = true;
                    scope.tables.push_back(open);
                } else if (j.subquery) {
                    auto out = nested(*j.subquery->first, false);
                    if (error) return;
                    scope.tables.push_back(derived_of(*j.subquery->first, j.subquery->second, out));
                } else {
                    scope.tables.push_back(table_of(j.table, j.alias));
                    forbid_target(scope.tables.back());
                }
            }
            // the columns a USING / NATURAL join makes one
            const std::size_t base = scope.tables.size() - sel.joins.size(); // (a SELECT without FROM has no first table)
            for (std::size_t ji = 0; ji < sel.joins.size(); ji++) {
                const Join& j = sel.joins[ji];
                for (auto& c : j.using_cols) scope.merged.insert(c);
                if (j.join_type != JoinType::Natural) continue;
                const BindTable& joined = scope.tables[base + ji];
                std::vector<std::string> names;
                if (joined.schema) {
                    for (auto& c : joined.schema->columns) names.push_back(c.name);
                } else {
                    names = joined.own_columns;
                }
                for (auto& n : names) {
                    for (std::size_t ti = 0; ti < base + ji; ti++) {
                        if (!scope.tables[ti].open && has_column(scope.tables[ti], n)) scope.merged.insert(n);
                    }
                }
            }
            // the names the select list gives
            for (auto& c : sel.columns) {
                if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) scope.selected.insert(last_part(col->name));
                if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) scope.aliases.insert(ca->alias);
                else if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) scope.aliases.insert(aa->alias);
                else if (auto* fn = std::get_if<SelectColumn::Func>(&c.data); fn && fn->alias) scope.aliases.insert(*fn->alias);
                else if (auto* e = std::get_if<SelectColumn::Expr>(&c.data); e && e->alias) scope.aliases.insert(*e->alias);
                else if (auto* cw = std::get_if<SelectColumn::CaseWhen>(&c.data); cw && cw->alias) scope.aliases.insert(*cw->alias);
                else if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data); wf && wf->alias) scope.aliases.insert(*wf->alias);
                else if (auto* sq = std::get_if<SelectColumn::Subquery>(&c.data); sq && sq->alias) scope.aliases.insert(*sq->alias);
            }
            chain.push_back(&scope);
            std::vector<std::pair<std::string, ValueClass>> out;
            std::vector<std::string> positions; // what each output column is called as an ORDER BY / GROUP BY item ("" when it cannot be one)
            std::unordered_map<std::string, std::string> given; // the names the select list gives, and what each stands for
            std::vector<std::string> ambiguous; // the names two different items give
            std::size_t subquery_items = 0;     // the scalar subqueries of the select list seen so far
            // the window functions inside expressions (`SUM(v) / SUM(SUM(v)) OVER ()`) are columns of their own that no result shows: bound first, the
            // expressions that read them need their types
            auto hidden_window = [](const SelectColumn& c) {
                auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data);
                return wf && wf->alias && hidden_window_name(*wf->alias);
            };
            for (auto& c : sel.columns) {
                if (!hidden_window(c)) continue;
                select_column(c);
                if (error) break;
                scope.alias_classes[*std::get<SelectColumn::WinFunc>(c.data).alias] = class_of_item(c);
            }
            for (auto& c : sel.columns) {
                if (hidden_window(c)) continue;
                select_column(c);
                if (error) break;
                const ValueClass k = class_of_item(c);
                if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) scope.alias_classes[ca->alias] = k;
                else if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) scope.alias_classes[aa->alias] = k;
                else if (auto* fn = std::get_if<SelectColumn::Func>(&c.data); fn && fn->alias) scope.alias_classes[*fn->alias] = k;
                else if (auto* e = std::get_if<SelectColumn::Expr>(&c.data); e && e->alias) scope.alias_classes[*e->alias] = k;
                else if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data); wf && wf->alias) scope.alias_classes[*wf->alias] = k;
                if (auto* all = std::get_if<SelectColumn::All>(&c.data)) {
                    const std::size_t before = out.size();
                    star_columns(scope, all->table, out);
                    for (std::size_t i = before; i < out.size(); i++) positions.push_back(out[i].first);
                } else {
                    out.emplace_back(std::string(), k);
                    std::string key = sort_key(c);
                    if (key.empty() && std::holds_alternative<SelectColumn::Subquery>(c.data)) key = "__sq_" + std::to_string(subquery_items) + "__"; // (the executor sorts by its value)
                    if (std::holds_alternative<SelectColumn::Subquery>(c.data)) subquery_items++;
                    positions.push_back(key);
                    if (const std::string alias = alias_of(c); !alias.empty() && !positions.back().empty()) {
                        // (two items that stand for different things under one name -- apart from case -- make an ORDER BY / GROUP BY by that name ambiguous)
                        for (const auto& [other, target] : given) {
                            if (ieq(other, alias) && target != positions.back()) ambiguous.push_back(alias);
                        }
                        given.emplace(alias, positions.back());
                    }
                }
            }
            for (auto& j : sel.joins) cond(j.on_expr, "on clause");
            if (sel.condition) cond(*sel.condition, "where clause");
            if (!sel.sort_resolved && !error) {
                for (auto& o : sel.order_by) resolve_sort_item(o.column, "order clause", positions, given, ambiguous);
                if (sel.group_by) {
                    for (auto& g : *sel.group_by) resolve_sort_item(g, "group statement", positions, given, ambiguous);
                }
                sel.sort_resolved = true;
            }
            if (sel.group_by) {
                for (auto& g : *sel.group_by) {
                    if (plain_reference(g)) { name(g, "group statement"); no_outer(g, "group statement"); }
                    else {
                        std::string spelled;
                        expression_argument(g, "group statement", &spelled);
                        if (!spelled.empty()) g = spelled;
                    }
                }
            }
            if (sel.having) cond(*sel.having, "having clause");
            if (sel.having && check && !error) having_any_value(sel, *sel.having);
            for (auto& o : sel.order_by) {
                if (subquery_item_name(o.column)) continue; // (a scalar subquery of the select list, by its alias or position)
                if (plain_reference(o.column)) {
                    name(o.column, "order clause");
                    no_outer(o.column, "order clause");
                    o.cls = class_of_name(o.column);
                } else {
                    std::string spelled;
                    o.cls = expression_argument(o.column, "order clause", &spelled);
                    if (!spelled.empty()) o.column = spelled;
                }
            }
            if (check && !error) only_full_group_by(sel, scope);
            chain.pop_back();
            outputs = std::move(out);
        }

        // The columns an expression names itself -- not the ones inside an aggregate (the parser keeps those as the text `SUM(v)`), a column of a query
        // around it, or the unit of a date function.
        void own_columns(const ArithExpr& e, std::vector<std::string>& out) const {
            if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) {
                if (col->outer == 0 && plain_reference(col->name) && !hidden_window_name(col->name)) out.push_back(col->name);
            } else if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) { own_columns(*v->lhs, out); own_columns(*v->rhs, out); }
            else if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) { own_columns(*v->lhs, out); own_columns(*v->rhs, out); }
            else if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) { own_columns(*v->lhs, out); own_columns(*v->rhs, out); }
            else if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) { own_columns(*v->lhs, out); own_columns(*v->rhs, out); }
            else if (auto* v = std::get_if<ArithExpr::Cmp>(&e.data)) { own_columns(*v->lhs, out); own_columns(*v->rhs, out); }
            else if (auto* v = std::get_if<ArithExpr::Pred>(&e.data)) own_columns(*v->cond, out);
            else if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) {
                if (f->name == "ANY_VALUE") return; // (the group is not asked to fix what is inside)
                const bool unit_last = f->name == "DATE_ADD" || f->name == "DATE_SUB";
                for (std::size_t i = 0; i < f->args.size(); i++) {
                    if (!(unit_last && i + 1 == f->args.size())) own_columns(f->args[i], out);
                }
            }
        }

        void own_columns(const CondExpr& e, std::vector<std::string>& out) const {
            if (auto* a = std::get_if<CondExpr::And>(&e.data)) { own_columns(*a->lhs, out); own_columns(*a->rhs, out); }
            else if (auto* o = std::get_if<CondExpr::Or>(&e.data)) { own_columns(*o->lhs, out); own_columns(*o->rhs, out); }
            else if (auto* n = std::get_if<CondExpr::Not>(&e.data)) own_columns(*n->inner, out);
            else if (auto* leaf = std::get_if<CondExpr::Leaf>(&e.data)) {
                own_columns(leaf->condition.left, out);
                if (auto* lit = std::get_if<ConditionValue::Literal>(&leaf->condition.value.data)) {
                    // (an unquoted word on the right is a column only when a table of the query has it)
                    if (!lit->quoted && lit->outer == 0 && plain_reference(lit->value) && !parse_number(lit->value) && table_column(lit->value)) out.push_back(lit->value);
                } else if (auto* value = std::get_if<ConditionValue::Arith>(&leaf->condition.value.data)) {
                    own_columns(value->expr, out);
                }
            }
        }

        // The table of this query that a column name (`v`, `t.v`) belongs to, and the column; nothing for a name no table of the query has.
        std::optional<std::pair<const BindTable*, std::string>> table_column(const std::string& name) const {
            if (chain.empty()) return std::nullopt;
            const BindScope& scope = *chain.back();
            const std::size_t dot = name.rfind('.');
            for (const BindTable& t : scope.tables) {
                if (dot != std::string::npos) {
                    const std::string qualifier = name.substr(0, dot);
                    if (t.full == qualifier || t.bare == qualifier) return std::make_pair(&t, name.substr(dot + 1));
                } else if (!t.open && has_column(t, name)) {
                    return std::make_pair(&t, name);
                }
            }
            return std::nullopt;
        }

        // ONLY_FULL_GROUP_BY, MySQL's default: in a query that groups or aggregates, what is selected without an aggregate may name only the columns grouped
        // by, columns that depend on them (the primary key of their table is grouped by) or none at all. The rest used to be left out of the answer (no
        // GROUP BY) or come out empty (with one): a column of some row of the group is no answer.
        void only_full_group_by(Statement::Select& sel, const BindScope& scope) {
            const bool aggregated = sel.group_by.has_value() || sel.having.has_value() || Executor::columns_have_aggregate(sel.columns);
            if (!aggregated) return;
            // what is grouped by: the columns (table, column) and the texts of the expressions
            std::vector<std::pair<const BindTable*, std::string>> grouped;
            std::vector<std::string> grouped_texts;
            if (sel.group_by) {
                for (auto& item : *sel.group_by) {
                    grouped_texts.push_back(item);
                    if (auto tc = table_column(item)) grouped.push_back(*tc);
                }
            }
            auto is_grouped = [&](const BindTable* t, const std::string& column) {
                for (auto& [gt, gc] : grouped) {
                    if (gt == t && gc == column) return true;
                }
                // (the primary key of the table is grouped by: every column of the table follows from it -- not under WITH ROLLUP: the key is NULL in a rolled-up row)
                if (sel.with_rollup || !t->schema) return false;
                bool has_key = false;
                for (auto& c : t->schema->columns) {
                    if (!c.primary_key) continue;
                    has_key = true;
                    if (!std::any_of(grouped.begin(), grouped.end(), [&](auto& g) { return g.first == t && g.second == c.name; })) return false;
                }
                return has_key;
            };
            std::size_t position = 0;
            for (auto& c : sel.columns) {
                position++;
                std::vector<std::string> names;
                if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) {
                    if (col->outer == 0 && plain_reference(col->name)) names.push_back(col->name);
                } else if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) {
                    if (ca->outer == 0 && plain_reference(ca->name)) names.push_back(ca->name);
                } else if (auto* e = std::get_if<SelectColumn::Expr>(&c.data)) {
                    // an expression that is itself what is grouped by (`id % 2`) names no column of its own
                    try {
                        const std::string text = Parser::aggregate_argument_text(e->expr);
                        if (std::find(grouped_texts.begin(), grouped_texts.end(), text) != grouped_texts.end()) continue;
                    } catch (const ParseError&) {
                    }
                    own_columns(e->expr, names);
                } else if (auto* all = std::get_if<SelectColumn::All>(&c.data)) {
                    for (const BindTable& t : scope.tables) {
                        if (t.open || !t.schema || (!all->table.empty() && t.full != all->table && t.bare != all->table)) continue;
                        for (auto& column : t.schema->columns) names.push_back(t.full + "." + column.name);
                    }
                } else if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data)) {
                    // a window function reads the groups: the columns in its argument and its PARTITION BY / ORDER BY items have to be grouped by too
                    auto note = [&](const std::string& text) {
                        if (plain_reference(text)) { names.push_back(text); return; }
                        if (!is_expression_argument(text)) return;
                        try {
                            own_columns(Parser::str_to_arith(text), names);
                        } catch (const ParseError&) {
                        }
                    };
                    if (wf->col) note(*wf->col);
                    for (auto& p : wf->partition_by) note(p);
                    for (auto& o : wf->order_by) note(o.column);
                }
                for (auto& name : names) {
                    auto tc = table_column(name);
                    if (!tc) continue; // (a name of no table of the query: an unknown column is reported, or it is the name of something else)
                    if (is_grouped(tc->first, tc->second)) continue;
                    error = sel.group_by
                                ? "Expression #" + std::to_string(position) + " of SELECT list is not in GROUP BY clause and contains nonaggregated column '" + name +
                                      "' which is not functionally dependent on columns in GROUP BY clause; this is incompatible with sql_mode=only_full_group_by"
                                : "In aggregated query without GROUP BY, expression #" + std::to_string(position) + " of SELECT list contains nonaggregated column '" + name +
                                      "'; this is incompatible with sql_mode=only_full_group_by";
                    return;
                }
            }
            // HAVING and ORDER BY read the columns of the group the same way (a name the select list gives is the select list's)
            if (sel.having) {
                std::vector<std::string> names;
                own_columns(*sel.having, names);
                for (auto& name : names) {
                    if (scope.aliases.count(name)) continue;
                    auto tc = table_column(name);
                    if (!tc || is_grouped(tc->first, tc->second)) continue;
                    error = "Non-grouping field '" + name + "' is used in HAVING clause";
                    return;
                }
            }
            std::size_t item = 0;
            for (auto& o : sel.order_by) {
                item++;
                if (!plain_reference(o.column) || scope.aliases.count(o.column)) continue;
                auto tc = table_column(o.column);
                if (!tc || is_grouped(tc->first, tc->second)) continue;
                error = sel.group_by
                            ? "Expression #" + std::to_string(item) + " of ORDER BY clause is not in GROUP BY clause and contains nonaggregated column '" + o.column +
                                  "' which is not functionally dependent on columns in GROUP BY clause; this is incompatible with sql_mode=only_full_group_by"
                            : "In aggregated query without GROUP BY, expression #" + std::to_string(item) + " of ORDER BY contains nonaggregated column '" + o.column +
                                  "'; this is incompatible with sql_mode=only_full_group_by";
                return;
            }
        }

        // The name an ORDER BY / GROUP BY item uses for a select-list column: a column by its name, an aggregate by its label, an expression by
        // its text; "" for what cannot be sorted by (a legacy function column, a subquery).
        std::string sort_key(SelectColumn& c) const {
            if (auto* col = std::get_if<SelectColumn::Column>(&c.data)) return col->name;
            if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) return ca->name;
            if (auto* agg = std::get_if<SelectColumn::Agg>(&c.data)) return Executor::agg_label(agg->func, agg->col);
            if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) return aa->alias;
            if (auto* e = std::get_if<SelectColumn::Expr>(&c.data)) {
                try {
                    const std::string printed = Parser::arith_to_string(e->expr);
                    // (a number alone would read as a position: the parentheses make it the constant it is)
                    return printed.find_first_not_of("0123456789") == std::string::npos ? "(" + printed + ")" : printed;
                } catch (const ParseError&) {
                    return std::string();
                }
            }
            if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data)) return wf->alias.value_or(Executor::window_func_default_label(wf->func));
            return std::string();
        }

        static std::string alias_of(const SelectColumn& c) {
            if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&c.data)) return ca->alias;
            if (auto* aa = std::get_if<SelectColumn::AggAlias>(&c.data)) return aa->alias;
            if (auto* e = std::get_if<SelectColumn::Expr>(&c.data)) return e->alias.value_or(std::string());
            if (auto* wf = std::get_if<SelectColumn::WinFunc>(&c.data)) return wf->alias.value_or(std::string());
            if (auto* sq = std::get_if<SelectColumn::Subquery>(&c.data)) return sq->alias.value_or(std::string());
            return std::string();
        }

        // `ORDER BY 2` is the second column of the select list, `ORDER BY total` the column the select list calls total (a name the select list
        // gives wins over a column of the same name, as in MySQL): either becomes what it stands for, the name of a column or the text of an
        // expression, which is all the executor has to know how to sort / group by.
        void resolve_sort_item(std::string& item, const char* clause, const std::vector<std::string>& positions,
                               const std::unordered_map<std::string, std::string>& given, const std::vector<std::string>& ambiguous) {
            if (item.empty()) return;
            if (item.find_first_not_of("0123456789") == std::string::npos) {
                const std::size_t n = item.size() > 9 ? 0 : static_cast<std::size_t>(std::stoul(item));
                if (n < 1 || n > positions.size()) {
                    if (check && !error) error = "Unknown column '" + item + "' in '" + clause + "'";
                    return;
                }
                if (positions[n - 1].empty()) {
                    if (check && !error) error = std::string("Cannot use column ") + item + " of the select list in '" + clause + "'";
                    return;
                }
                item = positions[n - 1];
                return;
            }
            if (item.find('.') == std::string::npos) {
                for (const std::string& name : ambiguous) {
                    if (ieq(name, item)) {
                        if (check && !error) error = "Column '" + item + "' in " + clause + " is ambiguous";
                        return;
                    }
                }
                if (auto it = given.find(item); it != given.end()) {
                    item = it->second;
                } else {
                    for (const auto& [alias, target] : given) {
                        if (ieq(alias, item)) { item = target; break; }
                    }
                }
            }
        }

        // an UPDATE / DELETE: the table (and the joined ones) as the one scope of its WHERE and SET expressions
        void write(const std::vector<std::string>& tables, std::vector<Join>& joins, std::optional<CondExpr>& where,
                   std::vector<std::pair<std::string, ArithExpr>>* assignments, const std::vector<std::pair<std::string, std::string>>& aliases) {
            BindScope scope;
            scope.table_aliases = aliases;
            for (auto& t : tables) scope.tables.push_back(table_of(t, ""));
            for (auto& j : joins) scope.tables.push_back(table_of(j.table, j.alias));
            scope.lenient_qualifier = true;
            for (std::size_t i = 0; i < tables.size() && i < scope.tables.size(); i++) {
                std::string shown = scope.tables[i].bare;
                for (const auto& [alias, table] : aliases) {
                    if (ieq(table, tables[i]) || ieq(table, scope.tables[i].bare) || ieq(table, scope.tables[i].full)) { shown = alias; break; }
                }
                write_targets.emplace_back(scope.tables[i].full, shown);
            }
            chain.push_back(&scope);
            if (assignments) {
                for (auto& [target, value] : *assignments) {
                    if (tables.size() + joins.size() > 1) name(target, "field list"); // (a one-table UPDATE checks it as it runs)
                    arith(value, "field list");
                }
            }
            for (auto& j : joins) cond(j.on_expr, "on clause");
            if (where) cond(*where, "where clause");
            chain.pop_back();
            write_targets.clear();
        }

        // MERGE: the target and the source are the one scope of its conditions and assignments, known by their aliases (else by their own names,
        // which is how the merged row names their columns)
        void merge(Statement::Merge& m) {
            BindScope scope;
            scope.tables.push_back(table_of(m.target, m.target_alias.value_or(last_part(m.target))));
            scope.tables.push_back(table_of(m.source, m.source_alias.value_or(last_part(m.source))));
            chain.push_back(&scope);
            cond(m.on, "on clause");
            if (m.when_matched_update_cond) cond(*m.when_matched_update_cond, "where clause");
            if (m.when_matched_delete_cond) cond(*m.when_matched_delete_cond, "where clause");
            if (m.when_matched_update) {
                for (auto& [target, value] : *m.when_matched_update) arith(value, "field list");
            }
            chain.pop_back();
        }

        void statement(Statement& st) {
            if (error) return;
            outputs.clear();
            if (auto* sel = std::get_if<Statement::Select>(&st.data)) select(*sel);
            else if (auto* u = std::get_if<Statement::Union>(&st.data)) {
                statement(*u->left);
                auto first = std::move(outputs); // (the columns of a UNION are named and typed by its first query)
                statement(*u->right);
                outputs = std::move(first);
            } else if (auto* i = std::get_if<Statement::Intersect>(&st.data)) {
                statement(*i->left);
                auto first = std::move(outputs);
                statement(*i->right);
                outputs = std::move(first);
            } else if (auto* x = std::get_if<Statement::Except>(&st.data)) {
                statement(*x->left);
                auto first = std::move(outputs);
                statement(*x->right);
                outputs = std::move(first);
            } else if (auto* w = std::get_if<Statement::With>(&st.data)) {
                for (auto& cte : w->ctes) {
                    statement(*cte.second);
                    if (error) return;
                    BindTable t;
                    t.full = t.bare = cte.first;
                    t.own_columns = ex.derived_column_names(s, *cte.second);
                    for (auto& o : outputs) t.own_classes.push_back(o.second);
                    t.open = t.own_columns.empty() || t.own_columns.size() != t.own_classes.size();
                    ctes[cte.first] = std::move(t);
                }
                if (w->query) statement(*w->query);
            } else if (auto* up = std::get_if<Statement::Update>(&st.data)) write({up->table}, no_joins, up->condition, &up->assignments, up->table_aliases);
            else if (auto* del = std::get_if<Statement::Delete>(&st.data)) write({del->table}, no_joins, del->condition, nullptr, del->table_aliases);
            else if (auto* mu = std::get_if<Statement::MultiUpdate>(&st.data)) write(mu->tables, mu->joins, mu->condition, &mu->assignments, mu->table_aliases);
            else if (auto* md = std::get_if<Statement::MultiDelete>(&st.data)) write({md->from_table}, md->joins, md->condition, nullptr, md->table_aliases);
            else if (auto* mg = std::get_if<Statement::Merge>(&st.data)) merge(*mg);
            else if (auto* is = std::get_if<Statement::InsertSelect>(&st.data)) {
                if (is->query) statement(*is->query);
            } else if (auto* view = std::get_if<Statement::CreateView>(&st.data)) {
                if (view->query) statement(*view->query); // a view over a column that is not there is refused when it is made
            }
        }

        std::vector<Join> no_joins;
        // the tables an UPDATE / DELETE / INSERT is changing -- as the tables of a query name them, and as MySQL calls them in its error (the alias the statement gave
        // the table, else its name): a subquery of it (not a derived table) that reads one is refused (MySQL 1093)
        std::vector<std::pair<std::string, std::string>> write_targets;
    };

    Binder binder{*this, s, check, {}, std::nullopt, {}, {}, {}};
    binder.statement(stmt);
    return binder.error;
}

ArithExpr Executor::parse_bound_expression(SharedDatabase& s, const std::string& text, const std::string& table, const std::vector<Join>& joins) {
    ArithExpr expr = Parser::str_to_arith(text);
    // (bound as the one select item of a query over the same tables)
    Statement::Select sel;
    sel.table = table;
    sel.joins = joins;
    sel.columns.push_back(SelectColumn(SelectColumn::Expr{std::move(expr), std::nullopt}));
    Statement stmt(std::move(sel));
    bind_statement(s, stmt, false);
    return std::move(std::get<SelectColumn::Expr>(std::get<Statement::Select>(stmt.data).columns[0].data).expr);
}

} // namespace engine
