#pragma once

// Faithful port of rusql-core/src/parser/ast.rs.
//
// Design ("cookbook" pattern, see migration plan): every Rust tagged-union enum becomes
// a struct holding a std::variant of small nested "variant" structs (one per Rust enum
// variant). Recursive fields that were `Box<T>` in Rust become std::unique_ptr<T>. Since
// unique_ptr isn't copyable, the handful of types that actually hold such pointers
// (ArithExpr's Add/Sub/Mul/Div/Cmp, CondExpr's And/Or/Not, and ~9 Statement variants)
// get an explicit deep-copy constructor (see ast.cpp); everything else uses the
// compiler-generated copy constructor.
//
// Two field names collide with C++ keywords and are renamed from the Rust original:
//   Condition::operator   -> Condition::op
//   ProcDeclare::default  -> ProcDeclare::default_value

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "engine/value_class.hpp"

namespace engine {

// ---------------------------------------------------------------------------
// IsolationLevel
// ---------------------------------------------------------------------------
enum class IsolationLevel { ReadUncommitted, ReadCommitted, RepeatableRead, Serializable };

// ---------------------------------------------------------------------------
// ArithExpr (recursive arithmetic expression tree)
// ---------------------------------------------------------------------------
struct CondExpr;
struct Statement;

struct ArithExpr {
    // `cls`: what kind of value the column holds (its declared type), set by Executor::bind_statement before a statement runs; a comparison
    // reads it (two text columns compare as text, a number column as numbers). Unknown for a column that has not been bound.
    // `outer`: 0 for a column of the query the expression is in, otherwise how many queries out the column lives (1 = the query around a
    // subquery); the binder sets it and rewrites `name` to the table-qualified name the outer row holds, and the subquery gets the outer row's
    // value for it before it runs (see Executor::substitute_outer).
    struct Col  { std::string name; ValueClass cls = ValueClass::Unknown; int outer = 0; };
    struct Num  { std::string value; };
    struct Str  { std::string value; };
    struct Add  { std::unique_ptr<ArithExpr> lhs, rhs; };
    struct Sub  { std::unique_ptr<ArithExpr> lhs, rhs; };
    struct Mul  { std::unique_ptr<ArithExpr> lhs, rhs; };
    struct Div  { std::unique_ptr<ArithExpr> lhs, rhs; };
    struct Func { std::string name; std::vector<ArithExpr> args; };
    struct Cmp  { std::unique_ptr<ArithExpr> lhs; std::string op; std::unique_ptr<ArithExpr> rhs; };
    // A condition used as a value: 1 when it is true, 0 when false, NULL when unknown (`v > 5`, `v IS NULL`, `a AND b`). A CASE is the
    // function CASE whose arguments are (a Pred, its result) pairs and, when the count is odd, the ELSE result last.
    struct Pred { std::unique_ptr<CondExpr> cond; };
    // A scalar subquery used as a value: `(SELECT MAX(k) FROM b) + 1`, `COALESCE((SELECT ...), 0)`, `SET v = (SELECT ...)`, `VALUES (1, (SELECT ...))`. No
    // row: NULL; several: an error (MySQL 1242). `cls`: what the one column it gives holds (set by the binder). The rest is the answer kept while it
    // cannot have changed (see Executor::scalar_subquery_value): a copy starts without it.
    struct Subquery {
        std::unique_ptr<Statement> query;
        ValueClass cls = ValueClass::Unknown;
        mutable std::uint64_t answered_in = 0;     // the statement the kept answer is of; 0: none
        mutable std::uint64_t answered_after = 0;  // ... and how many statements that write had run by then
        mutable std::string answer;
        mutable signed char correlated = -1;   // -1: not looked at yet
    };

    using Data = std::variant<Col, Num, Str, Add, Sub, Mul, Div, Func, Cmp, Pred, Subquery>;
    Data data;

    ArithExpr() : data(Col{}) {}
    template <typename Alt, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Alt>, ArithExpr>>>
    ArithExpr(Alt alt) : data(std::move(alt)) {}

    ArithExpr(const ArithExpr& other);
    ArithExpr& operator=(const ArithExpr& other);
    ArithExpr(ArithExpr&&) noexcept;
    ArithExpr& operator=(ArithExpr&&) noexcept;
    ~ArithExpr();
};

// ---------------------------------------------------------------------------
// DataType
// ---------------------------------------------------------------------------
struct DataType {
    struct Int {};
    struct BigInt {};
    struct SmallInt {};
    struct TinyInt {};
    struct Text {};
    struct Float {};
    struct Boolean {};
    struct Varchar { std::uint32_t length; };
    struct Date {};
    struct DateTime {};
    struct Timestamp {};
    struct Decimal { std::uint8_t precision; std::uint8_t scale; };
    struct Double {};
    struct Time {};
    struct Year {};
    struct Enum { std::vector<std::string> values; };
    struct Set { std::vector<std::string> values; };
    struct Blob {};
    struct Json {};
    struct Unknown {}; // mirrors Rust's #[serde(other)] fallback

    using Data = std::variant<Int, BigInt, SmallInt, TinyInt, Text, Float, Boolean, Varchar,
                               Date, DateTime, Timestamp, Decimal, Double, Time, Year, Enum,
                               Set, Blob, Json, Unknown>;
    Data data;

    DataType() : data(Int{}) {}
    template <typename Alt, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Alt>, DataType>>>
    DataType(Alt alt) : data(std::move(alt)) {}
};

// What a column of this declared type holds: numbers, or text (dates and times are kept as text in their canonical form, which compares right as text).
inline ValueClass class_of_type(const DataType& type) {
    return std::visit(
        [](const auto& alt) -> ValueClass {
            using T = std::decay_t<decltype(alt)>;
            if constexpr (std::is_same_v<T, DataType::Int> || std::is_same_v<T, DataType::BigInt> || std::is_same_v<T, DataType::SmallInt> ||
                          std::is_same_v<T, DataType::TinyInt> || std::is_same_v<T, DataType::Float> || std::is_same_v<T, DataType::Double> ||
                          std::is_same_v<T, DataType::Decimal> || std::is_same_v<T, DataType::Boolean> || std::is_same_v<T, DataType::Year>) {
                return ValueClass::Number;
            } else if constexpr (std::is_same_v<T, DataType::Unknown>) {
                return ValueClass::Unknown;
            } else {
                return ValueClass::Text;
            }
        },
        type.data);
}

// ---------------------------------------------------------------------------
// FkAction / ForeignKey / ColumnDef
// ---------------------------------------------------------------------------
enum class FkAction { Restrict, Cascade, SetNull, SetDefault };

struct ForeignKey {
    std::string column;
    std::string ref_table;
    std::string ref_column;
    FkAction on_delete = FkAction::Restrict;
    FkAction on_update = FkAction::Restrict;
};

struct ColumnDef {
    std::string name;
    DataType data_type;
    bool primary_key = false;
    bool not_null = false;
    bool unique = false;
    std::optional<std::string> unique_constraint_name;
    bool auto_increment = false;
    std::optional<std::string> default_value; // Rust field name: `default`
    std::optional<ForeignKey> foreign_key;
    std::optional<std::string> check_expr;
};

// ---------------------------------------------------------------------------
// PartitionBy (table partitioning -- no Rust original, new C++-native feature)
// ---------------------------------------------------------------------------
enum class PartitionKind { Range, List, Hash };

// One partition's bounds. RANGE uses range_upper_bound (or range_is_maxvalue for the
// catch-all last partition); LIST uses list_values. `child_table` is empty as parsed --
// the executor fills it in at CREATE TABLE time (see executor_ddl.cpp) once the physical
// hidden child table's name is decided, and it's what gets persisted in TableSchema
// (catalog/schema.hpp reuses this exact struct for both the parsed statement and the
// on-disk/in-catalog representation, matching this header's existing ColumnDef/ForeignKey
// convention).
struct PartitionDef {
    std::string name;
    std::optional<std::string> range_upper_bound;
    bool range_is_maxvalue = false;
    std::vector<std::string> list_values;
    std::string child_table;
};

struct PartitionBy {
    PartitionKind kind = PartitionKind::Range;
    std::string column;
    // RANGE/LIST: as written by the user. HASH: filled in by the executor at CREATE TABLE
    // time from `hash_partitions` (auto-named p0..pN-1) -- by the time this is stored in
    // TableSchema, `partitions` is always the complete, authoritative child list
    // regardless of kind, so routing code never needs to special-case HASH's naming.
    std::vector<PartitionDef> partitions;
    int hash_partitions = 0;
};

struct OrderBy {
    std::string column;
    bool ascending = true;
    ValueClass cls = ValueClass::Unknown; // what the column holds (set by Executor::bind_statement): text sorts as text, numbers as numbers
};

// ---------------------------------------------------------------------------
// Forward declaration of Statement — several expression/column types below
// hold a boxed (unique_ptr) reference to a subquery Statement.
// ---------------------------------------------------------------------------
struct Statement;
using StatementPtr = std::unique_ptr<Statement>;

// ---------------------------------------------------------------------------
// Operator / ConditionValue / Condition / CondExpr
// ---------------------------------------------------------------------------
enum class Operator {
    Eq, Ne, Gt, Lt, Gte, Lte,
    In, NotIn,
    Like, NotLike, Between, NotBetween,
    IsNull, IsNotNull,
    Exists, NotExists,
    Regexp, NotRegexp,
};

struct ConditionValue {
    // `quoted`: the value was written as a string ('abc'), so it is a string and never the name of a column or a number. (A value is kept
    // without its quotes, which made `name = 'city'` read the column city.) LiteralList::quoted is empty when no item is quoted.
    // `outer`: as ArithExpr::Col::outer, for a value that is the name of a column of an enclosing query.
    struct Literal { std::string value; bool quoted = false; int outer = 0; };
    struct Subquery { StatementPtr query; };
    struct Between { std::string lo, hi; bool lo_quoted = false, hi_quoted = false; };
    struct LiteralList { std::vector<std::string> values; std::vector<bool> quoted; };
    // PLAN.md P0 fix: the RHS of a comparison (`WHERE v > id + 100`) used to parse
    // as a single token, silently dropping the rest of the expression. Arith holds
    // a full ArithExpr so the RHS can be any arithmetic/function expression, not
    // just a bare literal or column reference.
    struct Arith { ArithExpr expr; };

    using Data = std::variant<Literal, Subquery, Between, LiteralList, Arith>;
    Data data;

    ConditionValue() : data(Literal{}) {}
    template <typename Alt, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Alt>, ConditionValue>>>
    ConditionValue(Alt alt) : data(std::move(alt)) {}

    ConditionValue(const ConditionValue& other);
    ConditionValue& operator=(const ConditionValue& other);
    ConditionValue(ConditionValue&&) noexcept = default;
    ConditionValue& operator=(ConditionValue&&) noexcept = default;
    ~ConditionValue() = default;
};

// Leaf predicate (single comparison). `operator` renamed to `op` (C++ keyword).
struct Condition {
    ArithExpr left;
    Operator op;
    ConditionValue value;
    // What each side of the comparison is (set by Executor::bind_statement): how the two compare (see value_class.hpp).
    ValueClass left_class = ValueClass::Unknown;
    ValueClass right_class = ValueClass::Unknown;
};

// Boolean expression tree with proper AND > OR precedence.
struct CondExpr {
    struct And { std::unique_ptr<CondExpr> lhs, rhs; };
    struct Or  { std::unique_ptr<CondExpr> lhs, rhs; };
    struct Not { std::unique_ptr<CondExpr> inner; };
    struct Leaf { Condition condition; };

    using Data = std::variant<And, Or, Not, Leaf>;
    Data data;

    CondExpr() : data(Leaf{}) {}
    template <typename Alt, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Alt>, CondExpr>>>
    CondExpr(Alt alt) : data(std::move(alt)) {}

    CondExpr(const CondExpr& other);
    CondExpr& operator=(const CondExpr& other);
    CondExpr(CondExpr&&) noexcept = default;
    CondExpr& operator=(CondExpr&&) noexcept = default;
    ~CondExpr() = default;
};

// ---------------------------------------------------------------------------
// JoinType / Join
// ---------------------------------------------------------------------------
enum class JoinType { Inner, Left, Right, Cross, Natural, FullOuter };

struct Join {
    std::string table; // LATERAL이면 서브쿼리의 별칭
    CondExpr on_expr;
    JoinType join_type;
    std::vector<std::string> using_cols;
    // LATERAL 서브쿼리 -- Rust 원본에 없음, Select::subquery와 동일한 모양(StatementPtr + 별칭).
    std::optional<std::pair<StatementPtr, std::string>> subquery;
    bool lateral = false;
    // Set only when `table` is used a second time in the same FROM list (a self-join, a lookup table joined twice): the parser
    // replaces every alias by its table name, which would make the two uses one, so this use keeps its alias as the name
    // its columns carry in the joined rows (`alias.column`).
    std::string alias;

    Join() = default;
    Join(std::string t, CondExpr on, JoinType jt, std::vector<std::string> uc,
         std::optional<std::pair<StatementPtr, std::string>> sq = std::nullopt, bool lat = false, std::string al = {})
        : table(std::move(t)), on_expr(std::move(on)), join_type(jt), using_cols(std::move(uc)),
          subquery(std::move(sq)), lateral(lat), alias(std::move(al)) {}
    // subquery가 unique_ptr를 담아 암시적 복사가 불가능해지므로, 명시적 깊은 복사 생성자가 필요
    // (SelectColumn과 동일한 패턴). Statement::Select의 기존 복사 생성자가 `joins` 벡터를
    // 그대로(변경 없이) 복사할 수 있도록 이 생성자가 std::vector<Join>의 복사를 가능하게 한다.
    Join(const Join&);
    Join& operator=(const Join&);
    Join(Join&&) noexcept = default;
    Join& operator=(Join&&) noexcept = default;
};

// ---------------------------------------------------------------------------
// CaseWhenBranch / AggFunc
// ---------------------------------------------------------------------------
struct CaseWhenBranch {
    CondExpr condition;
    std::string result;
};

struct AggFunc {
    struct Count {};
    struct CountDistinct {};
    struct Sum {};
    struct Avg {};
    struct Min {};
    struct Max {};
    struct SumDistinct {};
    struct AvgDistinct {};
    struct Stddev {};
    struct Variance {};
    struct GroupConcat { std::string separator; };
    struct CountCase { std::vector<CaseWhenBranch> branches; std::optional<std::string> else_val; };
    struct SumCase { std::vector<CaseWhenBranch> branches; std::optional<std::string> else_val; };
    // No Rust/MySQL original for these -- new C++-native additions.
    struct BitAnd {};
    struct BitOr {};
    struct JsonAgg {};
    // PostgreSQL-style ARRAY_AGG. This engine has no distinct array storage type, so it
    // is rendered identically to JSON_AGG (a JSON array text value) -- a separate AggFunc
    // alternative only so EXPLAIN/SHOW CREATE VIEW and the column label ("ARRAY_AGG(x)"
    // vs "JSON_AGG(x)") stay faithful to what the user actually wrote.
    struct ArrayAgg {};
    // MEDIAN(col): the 50th percentile (PERCENTILE_CONT(0.5)) -- linear interpolation between
    // the two middle values for an even count. NULLs are ignored.
    struct Median {};

    using Data = std::variant<Count, CountDistinct, Sum, Avg, Min, Max, SumDistinct, AvgDistinct,
                               Stddev, Variance, GroupConcat, CountCase, SumCase, BitAnd, BitOr, JsonAgg, ArrayAgg, Median>;
    Data data;

    AggFunc() : data(Count{}) {}
    template <typename Alt, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Alt>, AggFunc>>>
    AggFunc(Alt alt) : data(std::move(alt)) {}
};

// ---------------------------------------------------------------------------
// Window function frame
// ---------------------------------------------------------------------------
struct FrameBound {
    struct UnboundedPreceding {};
    struct Preceding { std::size_t n; };
    struct CurrentRow {};
    struct Following { std::size_t n; };
    struct UnboundedFollowing {};

    using Data = std::variant<UnboundedPreceding, Preceding, CurrentRow, Following, UnboundedFollowing>;
    Data data;

    FrameBound() : data(CurrentRow{}) {}
    template <typename Alt, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Alt>, FrameBound>>>
    FrameBound(Alt alt) : data(std::move(alt)) {}
};

enum class FrameUnit { Rows, Range };

struct WindowFrame {
    FrameUnit unit;
    FrameBound start;
    FrameBound end;
};

enum class WindowFunc {
    RowNumber, Rank, DenseRank, Lag, Lead, FirstValue, LastValue, NthValue, Ntile,
    PercentRank, CumeDist,
    // 집계 윈도우 함수
    Sum, Avg, Count, Min, Max,
};

// ---------------------------------------------------------------------------
// InsertConflict
// ---------------------------------------------------------------------------
struct InsertConflict {
    struct Abort {};
    struct Ignore {};
    struct Update { std::vector<std::pair<std::string, ArithExpr>> assignments; };
    // REPLACE INTO: on conflict, delete the existing row(s) then insert the new one --
    // no Rust/MySQL-parity original, new C++-native addition (parser_dml.cpp's
    // parse_replace() is the only place that produces this; it reuses the plain
    // Statement::Insert/InsertSelect shape rather than introducing a new Statement kind).
    struct Replace {};

    using Data = std::variant<Abort, Ignore, Update, Replace>;
    Data data;

    InsertConflict() : data(Abort{}) {}
    template <typename Alt, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Alt>, InsertConflict>>>
    InsertConflict(Alt alt) : data(std::move(alt)) {}
};

// ---------------------------------------------------------------------------
// SelectColumn
// ---------------------------------------------------------------------------
struct SelectColumn {
    struct All { std::string table; }; // `*`, or `table.*` (the table name or alias as typed)
    // `cls` / `arg_class` / `col_class`: what the column the select item reads holds (set by Executor::bind_statement)
    // `outer`: as ArithExpr::Col::outer, for a select item that is a column of an enclosing query.
    struct Column { std::string name; ValueClass cls = ValueClass::Unknown; int outer = 0; };
    struct ColumnAlias { std::string name, alias; ValueClass cls = ValueClass::Unknown; int outer = 0; };
    // `filter`: PostgreSQL's `FILTER (WHERE ...)` clause on an aggregate -- no Rust
    // original, new C++-native addition. Restricts which rows THIS aggregate considers,
    // independent of the query's own WHERE/HAVING (e.g. `COUNT(*) FILTER (WHERE
    // status='active')` alongside a plain unfiltered `COUNT(*)` in the same SELECT).
    // `col` is the argument as the query spells it (`b.id`, `o.amount`): it names the result column (`SUM(o.amount)`), like the
    // text the user typed. `source` is where a row holds that value when `col` goes through a table ALIAS: the same name with the
    // table in place of the alias (`orders.amount`); empty when `col` already names the table (or none).
    struct Agg { AggFunc func; std::string col; std::optional<CondExpr> filter; std::string source; ValueClass arg_class = ValueClass::Unknown; };
    struct AggAlias { AggFunc func; std::string col; std::string alias; std::optional<CondExpr> filter; std::string source; ValueClass arg_class = ValueClass::Unknown; };
    struct Func { std::string name; std::vector<std::string> args; std::optional<std::string> alias; };
    struct Expr { ArithExpr expr; std::optional<std::string> alias; };
    struct CaseWhen {
        std::vector<CaseWhenBranch> branches;
        std::optional<std::string> else_val;
        std::optional<std::string> alias;
    };
    struct WinFunc {
        WindowFunc func;
        std::optional<std::string> col;
        std::int64_t offset = 0;
        std::vector<std::string> partition_by;
        std::vector<OrderBy> order_by;
        std::optional<std::string> alias;
        std::optional<WindowFrame> frame;
        ValueClass col_class = ValueClass::Unknown;
    };
    struct Subquery {
        StatementPtr query;
        std::optional<std::string> alias;
    };

    using Data = std::variant<All, Column, ColumnAlias, Agg, AggAlias, Func, Expr, CaseWhen, WinFunc, Subquery>;
    Data data;

    SelectColumn() : data(All{}) {}
    template <typename Alt, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Alt>, SelectColumn>>>
    SelectColumn(Alt alt) : data(std::move(alt)) {}

    SelectColumn(const SelectColumn& other);
    SelectColumn& operator=(const SelectColumn& other);
    SelectColumn(SelectColumn&&) noexcept = default;
    SelectColumn& operator=(SelectColumn&&) noexcept = default;
    ~SelectColumn() = default;
};

// ---------------------------------------------------------------------------
// AlterAction
// ---------------------------------------------------------------------------
struct AlterAction {
    struct AddColumn { ColumnDef column; };
    struct DropColumn { std::string name; };
    struct RenameColumn { std::string from, to; };
    struct ModifyColumn { ColumnDef column; };
    struct RenameTable { std::string to; };
    struct AddForeignKey {
        std::optional<std::string> name;
        std::string column;
        std::string ref_table;
        std::string ref_column;
        FkAction on_delete;
        FkAction on_update;
    };
    struct DropForeignKey { std::string name; };
    struct AddUniqueConstraint { std::optional<std::string> name; std::string column; };
    struct AddCheckConstraint { std::optional<std::string> name; std::string expr; };
    struct DropConstraint { std::string name; };
    // Table partitioning (V1, no Rust original): RANGE/LIST only -- ADD PARTITION on a
    // HASH-partitioned table would need rehashing every existing row, out of scope.
    struct AddPartition { PartitionDef def; };
    struct DropPartition { std::string name; };

    using Data = std::variant<AddColumn, DropColumn, RenameColumn, ModifyColumn, RenameTable,
                               AddForeignKey, DropForeignKey, AddUniqueConstraint,
                               AddCheckConstraint, DropConstraint, AddPartition, DropPartition>;
    Data data;

    AlterAction() : data(DropColumn{}) {}
    template <typename Alt, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Alt>, AlterAction>>>
    AlterAction(Alt alt) : data(std::move(alt)) {}
};

enum class TriggerTiming { Before, After };
enum class TriggerEvent { Insert, Update, Delete };

// ---------------------------------------------------------------------------
// Statement — the root AST node (80 variants in the Rust original).
// ---------------------------------------------------------------------------
struct Statement {
    struct Begin {};
    struct Commit {};
    struct Rollback {};
    struct CreateTable {
        std::string name;
        std::vector<ColumnDef> columns;
        bool if_not_exists = false;
        std::vector<std::string> primary_key_columns;
        std::vector<std::pair<std::optional<std::string>, std::string>> check_constraints;
        std::optional<PartitionBy> partition_by;
    };
    struct DropTable { std::string name; bool if_exists = false; };
    struct TruncateTable { std::string name; };
    struct Insert {
        std::string table;
        std::optional<std::vector<std::string>> columns;
        std::vector<std::vector<std::string>> values;
        InsertConflict on_conflict;
        std::optional<std::vector<SelectColumn>> returning;
    };
    struct InsertSelect {
        std::string table;
        std::optional<std::vector<std::string>> columns;
        StatementPtr query;
        InsertConflict on_conflict;
        std::optional<std::vector<SelectColumn>> returning;
    };
    struct Select {
        std::string table;
        std::optional<std::pair<StatementPtr, std::string>> subquery;
        std::vector<SelectColumn> columns;
        bool distinct = false;
        std::optional<CondExpr> condition;
        std::vector<Join> joins;
        std::vector<OrderBy> order_by;
        std::optional<std::vector<std::string>> group_by;
        std::optional<CondExpr> having;
        std::optional<std::size_t> limit;
        std::optional<std::size_t> offset;
        bool for_update = false;
        bool for_share = false;
        // The binder replaced the positions and select-list names in ORDER BY / GROUP BY by what they stand for (done once per statement).
        bool sort_resolved = false;
        // The alias the FROM table keeps as its name when a query around this one uses the same table (a join keeps it in Join::alias): `a2.g`
        // must stay apart from the `a.g` of the outer query.
        std::string table_alias;
        // The aliases of the FROM list, as the parser expanded them (alias -> the table name the columns carry; a table used twice keeps its
        // alias): the parser does not expand an alias of this query inside a subquery of it, so the binder reads `x.id` there through these.
        std::vector<std::pair<std::string, std::string>> table_aliases;
    };
    struct Update {
        std::string table;
        std::vector<std::pair<std::string, ArithExpr>> assignments;
        std::optional<CondExpr> condition;
        std::optional<std::vector<SelectColumn>> returning;
    };
    struct Delete {
        std::string table;
        std::optional<CondExpr> condition;
        std::optional<std::vector<SelectColumn>> returning;
    };
    struct AlterTable { std::string table; AlterAction action; };
    struct CreateIndex {
        std::string index_name;
        std::string table;
        std::vector<std::string> columns;
        bool using_hash = false;
    };
    struct DropIndex { std::string index_name; };
    struct CreateView { std::string name; StatementPtr query; std::string raw_sql; };
    struct DropView { std::string name; };
    struct ShowTables {};
    struct Describe { std::string table; };
    struct ShowBufferPool {};
    struct ShowWal {};
    struct Checkpoint {};
    struct SetIsolationLevel { IsolationLevel level; };
    struct ShowIsolationLevel {};
    struct Vacuum { std::optional<std::string> table; };
    struct ShowLocks {};
    struct Use { std::string database; };
    struct Savepoint { std::string name; };
    struct ReleaseSavepoint { std::string name; };
    struct RollbackTo { std::string name; };
    struct Explain { StatementPtr inner; };
    struct ExplainAnalyze { StatementPtr inner; };
    struct AnalyzeTable { std::string table; };
    struct With {
        std::vector<std::pair<std::string, StatementPtr>> ctes;
        StatementPtr query;
        bool recursive = false;
    };
    struct Union {
        StatementPtr left, right;
        bool all = false;
        std::vector<OrderBy> order_by;
        std::optional<std::size_t> limit;
        std::optional<std::size_t> offset;
    };
    struct Intersect {
        StatementPtr left, right;
        bool all = false;
        std::vector<OrderBy> order_by;
        std::optional<std::size_t> limit;
        std::optional<std::size_t> offset;
    };
    struct Except {
        StatementPtr left, right;
        bool all = false;
        std::vector<OrderBy> order_by;
        std::optional<std::size_t> limit;
        std::optional<std::size_t> offset;
    };
    struct CreateDatabase { std::string name; bool if_not_exists = false; };
    struct DropDatabase { std::string name; bool if_exists = false; };
    struct MultiUpdate {
        std::vector<std::string> tables;
        std::vector<Join> joins;
        std::vector<std::pair<std::string, ArithExpr>> assignments;
        std::optional<CondExpr> condition;
    };
    struct MultiDelete {
        std::vector<std::string> delete_tables;
        std::string from_table;
        std::vector<Join> joins;
        std::optional<CondExpr> condition;
    };
    struct CreateUser {
        std::string user, host;
        std::optional<std::string> password;
        bool if_not_exists = false;
    };
    struct DropUser { std::string user, host; bool if_exists = false; };
    struct Grant {
        std::vector<std::string> privileges;
        std::string object_type, object, user, host;
        bool with_grant_option = false;
    };
    struct Revoke {
        std::vector<std::string> privileges;
        std::string object_type, object, user, host;
    };
    struct ShowGrants { std::optional<std::string> user, host; };
    struct CreateRole { std::string name; };
    struct DropRole { std::string name; bool if_exists = false; };
    struct GrantRole { std::string role, user, host; bool with_admin_option = false; };
    struct RevokeRole { std::string role, user, host; };
    struct ShowRoles {};
    struct CreateSynonym { std::string name, target; bool or_replace = false; };
    struct DropSynonym { std::string name; bool if_exists = false; };
    struct ShowSynonyms {};
    struct ShowDatabases {};
    struct ShowCreateTable { std::string table; };
    struct ShowCreateView { std::string view; };
    struct ShowIndex { std::string table; };
    struct Merge {
        std::string target;
        std::optional<std::string> target_alias;
        std::string source;
        std::optional<std::string> source_alias;
        CondExpr on;
        std::optional<std::vector<std::pair<std::string, ArithExpr>>> when_matched_update;
        bool when_matched_delete = false;
        std::optional<CondExpr> when_matched_delete_cond;
        std::optional<std::vector<std::string>> when_not_matched_columns;
        std::vector<std::string> when_not_matched_values;
        // WHEN MATCHED AND <condition> THEN UPDATE: the update is made only for the matched rows the condition holds for (it used to be read
        // and thrown away); and which of the two WHEN MATCHED clauses was written first, since the first whose condition holds is the one used.
        std::optional<CondExpr> when_matched_update_cond;
        bool when_matched_update_first = false;
    };
    struct CreateProcedure {
        std::string name;
        std::vector<std::tuple<std::string, std::string, std::string>> params; // (IN/OUT/INOUT, name, type)
        std::vector<Statement> body;
    };
    struct CallProcedure { std::string name; std::vector<std::string> args; };
    struct CreateTrigger {
        std::string name;
        TriggerTiming timing;
        TriggerEvent event;
        std::string table;
        std::vector<Statement> body;
    };
    struct DropTrigger { std::string name; bool if_exists = false; };
    struct DropProcedure { std::string name; bool if_exists = false; };
    struct Backup { std::optional<std::string> database; std::optional<std::string> output_file; };
    struct Restore { std::string source_file; std::optional<std::string> database; };
    struct ShowProcessList {};
    struct CreateFunction { std::string name; std::vector<std::string> params; std::string body; };
    struct DropFunction { std::string name; bool if_exists = false; };
    // 저장 프로시저 제어문
    struct ProcDeclare { std::string name, typ; std::optional<std::string> default_value; };
    struct ProcSet { std::string name; ArithExpr expr; };
    struct ProcIf {
        CondExpr condition;
        std::vector<Statement> then_body;
        std::vector<std::pair<CondExpr, std::vector<Statement>>> elseif_branches;
        std::optional<std::vector<Statement>> else_body;
    };
    struct ProcWhile { std::optional<std::string> label; CondExpr condition; std::vector<Statement> body; };
    struct ProcLoop { std::optional<std::string> label; std::vector<Statement> body; };
    struct ProcRepeat { std::optional<std::string> label; std::vector<Statement> body; CondExpr until; };
    struct ProcLeave { std::optional<std::string> label; };
    struct ProcIterate { std::optional<std::string> label; };
    struct PrepareStmt { std::string name, query; };
    struct ExecuteStmt { std::string name; std::vector<std::string> using_vars; };
    struct DeallocatePrepare { std::string name; };
    struct SetUserVar { std::string name; ArithExpr expr; };
    // LOCK TABLES / UNLOCK TABLES -- no Rust/MySQL-parity original, new C++-native
    // addition (V1 scope: session-to-session LOCK TABLES cooperation only -- see
    // executor_dcl.cpp's exec_lock_tables design note). `tables` pairs a table name with
    // whether it's WRITE (true, exclusive) or READ (false, shared).
    struct LockTables { std::vector<std::pair<std::string, bool>> tables; };
    struct UnlockTables {};
    // SELECT ... INTO var [, var]: the first row of the query goes into the variables (a procedure's, or an @user variable)
    struct SelectInto {
        StatementPtr query;
        std::vector<std::string> vars;
    };

    using Data = std::variant<
        Begin, Commit, Rollback, CreateTable, DropTable, TruncateTable, Insert, InsertSelect,
        Select, Update, Delete, AlterTable, CreateIndex, DropIndex, CreateView, DropView,
        ShowTables, Describe, ShowBufferPool, ShowWal, Checkpoint, SetIsolationLevel,
        ShowIsolationLevel, Vacuum, ShowLocks, Use, Savepoint, ReleaseSavepoint, RollbackTo,
        Explain, ExplainAnalyze, AnalyzeTable, With, Union, Intersect, Except, CreateDatabase,
        DropDatabase, MultiUpdate, MultiDelete, CreateUser, DropUser, Grant, Revoke, ShowGrants,
        CreateRole, DropRole, GrantRole, RevokeRole, ShowRoles, CreateSynonym, DropSynonym,
        ShowSynonyms, ShowDatabases, ShowCreateTable, ShowCreateView, ShowIndex, Merge,
        CreateProcedure, CallProcedure, CreateTrigger, DropTrigger, DropProcedure, Backup,
        Restore, ShowProcessList, CreateFunction, DropFunction, ProcDeclare, ProcSet, ProcIf,
        ProcWhile, ProcLoop, ProcRepeat, ProcLeave, ProcIterate, PrepareStmt, ExecuteStmt,
        DeallocatePrepare, SetUserVar, LockTables, UnlockTables, SelectInto>;
    Data data;

    Statement() : data(Begin{}) {}
    template <typename Alt, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Alt>, Statement>>>
    Statement(Alt alt) : data(std::move(alt)) {}

    Statement(const Statement& other);
    Statement& operator=(const Statement& other);
    Statement(Statement&&) noexcept = default;
    Statement& operator=(Statement&&) noexcept = default;
    ~Statement() = default;
};

} // namespace engine
