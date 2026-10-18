// The column types of an answer, for the MySQL protocol: a client that is told `INT` hands its application a number, one that is told nothing hands it
// the text. The types are read from the SELECT, not from the answer: a column of a table has the table's type, COUNT(*) is a BIGINT, SUM / AVG follow
// their argument; anything else (a CASE, a string function, a subquery) is left as text. The caller checks the cells against the type before it
// trusts it (a column typed INT whose cells are no integers goes out as text), and the whole answer is trusted only when the number of columns found
// is the number of columns answered.

#include <algorithm>
#include <cctype>
#include <unordered_set>

#include "engine/executor/executor.hpp"
#include "engine/parser/parser.hpp"
#include "engine/value_class.hpp"

namespace engine {

namespace {

struct NamedType {
    std::string name; // "" when the column is not called anything a query can name
    ResultColumnType type;
};

bool ieq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

std::string last_part(const std::string& qualified) {
    auto cut = qualified.rfind('.');
    return cut == std::string::npos ? qualified : qualified.substr(cut + 1);
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// `name`, `t.name`, `db.t.name`
bool plain_reference(const std::string& text) {
    if (text.empty() || std::isdigit(static_cast<unsigned char>(text[0]))) return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '.' || c == '$' || c >= 0x80; });
}

ResultColumnType made_up(DataType type, bool is_unsigned = false) {
    ColumnDef def;
    def.data_type = std::move(type);
    def.is_unsigned = is_unsigned;
    return ResultColumnType{def, false};
}

bool integer_type(const DataType& t) {
    return std::holds_alternative<DataType::Int>(t.data) || std::holds_alternative<DataType::BigInt>(t.data) ||
           std::holds_alternative<DataType::SmallInt>(t.data) || std::holds_alternative<DataType::TinyInt>(t.data);
}

// a function that gives a number (the list of the comparisons, and the ones that tell about the session or count something)
bool numeric_function(const std::string& name) {
    static const std::unordered_set<std::string> more = {"LAST_INSERT_ID", "ROW_COUNT", "FOUND_ROWS", "BIT_COUNT", "PERIOD_ADD", "PERIOD_DIFF", "YEARWEEK",
                                                         "TO_SECONDS", "REGEXP_INSTR", "SLEEP", "TIMESTAMPADD", "CONNECTION_ID", "DEGREES", "RADIANS", "ATAN",
                                                         "ASIN", "ACOS", "COT", "CRC32", "QUARTER", "TO_DAYS", "MICROSECOND", "LAST_INSERT_ROWID"};
    return function_result_class(name) == ValueClass::Number || more.count(upper(name)) > 0;
}

bool approximate_type(const DataType& t) { return std::holds_alternative<DataType::Float>(t.data) || std::holds_alternative<DataType::Double>(t.data); }

struct Source {
    std::string full, bare, alias;
    std::vector<NamedType> columns;
};

class Resolver {
public:
    Resolver(const SharedDatabase& db, std::string current_db) : db_(db), current_db_(std::move(current_db)) {}

    // The types of the columns `sel` gives; false when they cannot be told (a table that is not there, a USING join, a CTE ...)
    bool columns_of(const Statement& stmt, int depth, std::vector<NamedType>& out) const {
        const Statement* q = &stmt;
        while (q) { // (a set operation: the left side names the columns and types them)
            if (auto* u = std::get_if<Statement::Union>(&q->data)) q = u->left.get();
            else if (auto* i = std::get_if<Statement::Intersect>(&q->data)) q = i->left.get();
            else if (auto* e = std::get_if<Statement::Except>(&q->data)) q = e->left.get();
            else break;
        }
        if (!q || depth > 4) return false;
        auto* sel = std::get_if<Statement::Select>(&q->data);
        return sel && select_columns(*sel, depth, out);
    }

private:
    const SharedDatabase& db_;
    std::string current_db_; // the database an unqualified table name belongs to

    bool derived(const Statement& inner, const std::string& alias, int depth, std::vector<Source>& sources) const {
        Source src;
        src.full = src.bare = alias;
        if (!columns_of(inner, depth + 1, src.columns)) return false;
        sources.push_back(std::move(src));
        return true;
    }

    bool add_table(const std::string& name, const std::string& alias, int depth, std::vector<Source>& sources) const {
        const std::string table = name.find('.') != std::string::npos ? name : current_db_ + "." + name;
        Source src;
        src.full = table;
        src.bare = last_part(table);
        src.alias = alias;
        if (auto bare = Executor::info_schema_bare(table)) {
            const std::vector<InfoSchemaColumn>* cols = Executor::info_schema_columns(*bare);
            if (!cols) return false;
            for (const InfoSchemaColumn& c : *cols) {
                src.columns.push_back({c.name, made_up(c.numeric ? DataType(DataType::BigInt{}) : DataType(DataType::Text{}))});
            }
        } else if (const TableSchema* schema = db_.catalog.get_table(table)) {
            for (const ColumnDef& c : schema->columns) src.columns.push_back({c.name, ResultColumnType{c, false}});
        } else if (auto view = db_.views.find(table); view != db_.views.end()) {
            if (!columns_of(view->second, depth + 1, src.columns)) return false;
        } else {
            return false;
        }
        sources.push_back(std::move(src));
        return true;
    }

    const NamedType* find(const Statement::Select& sel, const std::vector<Source>& sources, const std::string& reference) const {
        const std::size_t dot = reference.rfind('.');
        std::string qualifier = dot == std::string::npos ? std::string() : reference.substr(0, dot);
        const std::string column = last_part(reference);
        auto in_source = [&](const Source& src) -> const NamedType* {
            for (const NamedType& c : src.columns) {
                if (ieq(c.name, column)) return &c;
            }
            return nullptr;
        };
        if (qualifier.empty()) {
            for (const Source& src : sources) {
                if (const NamedType* c = in_source(src)) return c;
            }
            return nullptr;
        }
        for (int pass = 0; pass < 2; pass++) {
            for (const Source& src : sources) {
                if (ieq(src.full, qualifier) || ieq(src.bare, qualifier) || (!src.alias.empty() && ieq(src.alias, qualifier))) return in_source(src);
            }
            std::string table;
            for (auto& [alias, t] : sel.table_aliases) {
                if (ieq(alias, qualifier)) table = t;
            }
            if (table.empty()) return nullptr;
            qualifier = table;
        }
        return nullptr;
    }

    ResultColumnType aggregate(const AggFunc& func, const std::string& argument, const Statement::Select& sel, const std::vector<Source>& sources) const {
        std::string arg = argument;
        if (arg.rfind("DISTINCT ", 0) == 0) arg.erase(0, 9);
        const NamedType* column = plain_reference(arg) ? find(sel, sources, arg) : nullptr;
        const std::optional<ColumnDef>& def = column ? column->type.column : std::optional<ColumnDef>();
        const bool by_value = !column; // (an expression, or something that is not a column: a number if the values are)
        auto of_argument = [&](bool average) -> ResultColumnType {
            if (!def) return ResultColumnType{std::nullopt, by_value};
            if (approximate_type(def->data_type)) return made_up(DataType(DataType::Double{}));
            if (auto* dec = std::get_if<DataType::Decimal>(&def->data_type.data)) {
                const unsigned scale = std::min<unsigned>(dec->scale + (average ? 4u : 0u), 30u);
                return made_up(DataType(DataType::Decimal{static_cast<std::uint8_t>(std::min<unsigned>(dec->precision + (average ? 4u : 22u), 65u)),
                                                           static_cast<std::uint8_t>(scale)}));
            }
            if (integer_type(def->data_type)) return made_up(DataType(DataType::Decimal{static_cast<std::uint8_t>(average ? 14 : 32), static_cast<std::uint8_t>(average ? 4 : 0)}));
            return ResultColumnType{}; // (text and dates: MySQL converts them to numbers; left as text here)
        };
        return std::visit(
            [&](const auto& f) -> ResultColumnType {
                using T = std::decay_t<decltype(f)>;
                if constexpr (std::is_same_v<T, AggFunc::Count> || std::is_same_v<T, AggFunc::CountDistinct> || std::is_same_v<T, AggFunc::CountCase>)
                    return made_up(DataType(DataType::BigInt{}));
                else if constexpr (std::is_same_v<T, AggFunc::Sum> || std::is_same_v<T, AggFunc::SumDistinct> || std::is_same_v<T, AggFunc::SumCase>)
                    return of_argument(false);
                else if constexpr (std::is_same_v<T, AggFunc::Avg> || std::is_same_v<T, AggFunc::AvgDistinct>)
                    return of_argument(true);
                else if constexpr (std::is_same_v<T, AggFunc::Min> || std::is_same_v<T, AggFunc::Max>)
                    return def ? ResultColumnType{*def, false} : ResultColumnType{};
                else if constexpr (std::is_same_v<T, AggFunc::Stddev> || std::is_same_v<T, AggFunc::Variance>)
                    return made_up(DataType(DataType::Double{}));
                else if constexpr (std::is_same_v<T, AggFunc::BitAnd> || std::is_same_v<T, AggFunc::BitOr>)
                    return made_up(DataType(DataType::BigInt{}), true);
                else if constexpr (std::is_same_v<T, AggFunc::Median>)
                    return ResultColumnType{std::nullopt, true};
                else
                    return ResultColumnType{}; // GROUP_CONCAT, JSON_AGG, ARRAY_AGG
            },
            func.data);
    }

    // Is the expression a number? Arithmetic, a comparison and the functions that give numbers are; a string function, a CASE, an IF, a COALESCE
    // (it gives what its arguments are) and a subquery are left as text.
    bool numeric_expression(const ArithExpr& e, const Statement::Select& sel, const std::vector<Source>& sources) const {
        if (std::holds_alternative<ArithExpr::Num>(e.data)) return true;
        if (std::holds_alternative<ArithExpr::Add>(e.data) || std::holds_alternative<ArithExpr::Sub>(e.data) || std::holds_alternative<ArithExpr::Mul>(e.data) ||
            std::holds_alternative<ArithExpr::Div>(e.data) || std::holds_alternative<ArithExpr::Cmp>(e.data) || std::holds_alternative<ArithExpr::Pred>(e.data))
            return true;
        if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) return numeric_function(f->name);
        if (auto* c = std::get_if<ArithExpr::Col>(&e.data)) {
            const NamedType* column = c->outer == 0 && plain_reference(c->name) ? find(sel, sources, c->name) : nullptr;
            if (!column || !column->type.column) return column && column->type.by_value;
            const DataType& t = column->type.column->data_type;
            return integer_type(t) || approximate_type(t) || std::holds_alternative<DataType::Decimal>(t.data) || std::holds_alternative<DataType::Boolean>(t.data);
        }
        return false; // a string, a subquery
    }

    ResultColumnType window(const SelectColumn::WinFunc& w, const Statement::Select& sel, const std::vector<Source>& sources) const {
        switch (w.func) {
            case WindowFunc::RowNumber:
            case WindowFunc::Rank:
            case WindowFunc::DenseRank:
            case WindowFunc::Ntile:
            case WindowFunc::Count:
                return made_up(DataType(DataType::BigInt{}));
            case WindowFunc::PercentRank:
            case WindowFunc::CumeDist:
                return made_up(DataType(DataType::Double{}));
            case WindowFunc::Sum:
            case WindowFunc::Avg:
                return w.col ? aggregate(w.func == WindowFunc::Sum ? AggFunc(AggFunc::Sum{}) : AggFunc(AggFunc::Avg{}), *w.col, sel, sources) : ResultColumnType{};
            default: { // MIN / MAX / LAG / LEAD / FIRST_VALUE / ...: the column they read
                const NamedType* column = w.col && plain_reference(*w.col) ? find(sel, sources, *w.col) : nullptr;
                return column ? column->type : ResultColumnType{};
            }
        }
    }

    bool select_columns(const Statement::Select& sel, int depth, std::vector<NamedType>& out) const {
        std::vector<Source> sources;
        if (sel.subquery) {
            if (!derived(*sel.subquery->first, sel.subquery->second, depth, sources)) return false;
        } else if (!sel.table.empty() && sel.table != "_dual_" && !(sel.table.size() > 7 && sel.table.compare(sel.table.size() - 7, 7, "._dual_") == 0)) {
            if (!add_table(sel.table, sel.table_alias, depth, sources)) return false;
        }
        for (const Join& j : sel.joins) {
            if (j.lateral || !j.using_cols.empty() || j.join_type == JoinType::Natural) return false; // (a USING join shows its common columns once)
            if (j.subquery) {
                if (!derived(*j.subquery->first, j.subquery->second, depth, sources)) return false;
            } else if (!add_table(j.table, j.alias, depth, sources)) {
                return false;
            }
        }
        for (const SelectColumn& item : sel.columns) {
            if (auto* all = std::get_if<SelectColumn::All>(&item.data)) {
                bool matched = all->table.empty();
                for (const Source& src : sources) {
                    if (!all->table.empty() && !(ieq(src.full, all->table) || ieq(src.bare, all->table) || (!src.alias.empty() && ieq(src.alias, all->table)))) continue;
                    matched = true;
                    out.insert(out.end(), src.columns.begin(), src.columns.end());
                }
                if (!matched) return false;
            } else if (auto* col = std::get_if<SelectColumn::Column>(&item.data)) {
                const NamedType* found = col->outer == 0 && plain_reference(col->name) ? find(sel, sources, col->name) : nullptr;
                out.push_back({last_part(col->name), found ? found->type : ResultColumnType{}});
            } else if (auto* ca = std::get_if<SelectColumn::ColumnAlias>(&item.data)) {
                const NamedType* found = ca->outer == 0 && plain_reference(ca->name) ? find(sel, sources, ca->name) : nullptr;
                out.push_back({ca->alias, found ? found->type : ResultColumnType{}});
            } else if (auto* agg = std::get_if<SelectColumn::Agg>(&item.data)) {
                out.push_back({std::string(), aggregate(agg->func, agg->source.empty() ? agg->col : agg->source, sel, sources)});
            } else if (auto* aa = std::get_if<SelectColumn::AggAlias>(&item.data)) {
                out.push_back({aa->alias, aggregate(aa->func, aa->source.empty() ? aa->col : aa->source, sel, sources)});
            } else if (auto* fn = std::get_if<SelectColumn::Func>(&item.data)) {
                out.push_back({fn->alias.value_or(std::string()), ResultColumnType{std::nullopt, numeric_function(fn->name)}});
            } else if (auto* e = std::get_if<SelectColumn::Expr>(&item.data)) {
                out.push_back({e->alias.value_or(std::string()), ResultColumnType{std::nullopt, numeric_expression(e->expr, sel, sources)}});
            } else if (auto* w = std::get_if<SelectColumn::WinFunc>(&item.data)) {
                out.push_back({w->alias.value_or(std::string()), window(*w, sel, sources)});
            } else if (auto* cw = std::get_if<SelectColumn::CaseWhen>(&item.data)) {
                out.push_back({cw->alias.value_or(std::string()), ResultColumnType{}});
            } else if (auto* sq = std::get_if<SelectColumn::Subquery>(&item.data)) {
                out.push_back({sq->alias.value_or(std::string()), ResultColumnType{}});
            } else {
                return false;
            }
        }
        return true;
    }
};

} // namespace

std::vector<ResultColumnType> Executor::result_columns(const std::string& sql) const {
    Parser parser(sql, pipes_as_concat());
    if (!parser.has_tokens()) return {};
    auto parsed = parser.parse();
    if (parsed.is_err()) return {};
    Statement stmt = std::move(parsed).value();
    auto s = shared->read();
    resolve_names(*s, stmt);
    std::vector<NamedType> found;
    if (!Resolver(*s, current_db).columns_of(stmt, 0, found)) return {};
    std::vector<ResultColumnType> out;
    out.reserve(found.size());
    for (NamedType& c : found) out.push_back(std::move(c.type));
    return out;
}

} // namespace engine
