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

namespace engine {

// ---------------------------------------------------------------------------
// IsolationLevel
// ---------------------------------------------------------------------------
enum class IsolationLevel { ReadUncommitted, ReadCommitted, RepeatableRead, Serializable };

// ---------------------------------------------------------------------------
// ArithExpr (recursive arithmetic expression tree)
// ---------------------------------------------------------------------------
struct ArithExpr {
    struct Col  { std::string name; };
    struct Num  { std::string value; };
    struct Str  { std::string value; };
    struct Add  { std::unique_ptr<ArithExpr> lhs, rhs; };
    struct Sub  { std::unique_ptr<ArithExpr> lhs, rhs; };
    struct Mul  { std::unique_ptr<ArithExpr> lhs, rhs; };
    struct Div  { std::unique_ptr<ArithExpr> lhs, rhs; };
    struct Func { std::string name; std::vector<ArithExpr> args; };
    struct Cmp  { std::unique_ptr<ArithExpr> lhs; std::string op; std::unique_ptr<ArithExpr> rhs; };

    using Data = std::variant<Col, Num, Str, Add, Sub, Mul, Div, Func, Cmp>;
    Data data;

    ArithExpr() : data(Col{}) {}
    template <typename Alt, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Alt>, ArithExpr>>>
    ArithExpr(Alt alt) : data(std::move(alt)) {}

    ArithExpr(const ArithExpr& other);
    ArithExpr& operator=(const ArithExpr& other);
    ArithExpr(ArithExpr&&) noexcept = default;
    ArithExpr& operator=(ArithExpr&&) noexcept = default;
    ~ArithExpr() = default;
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

struct OrderBy {
    std::string column;
    bool ascending = true;
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
    struct Literal { std::string value; };
    struct Subquery { StatementPtr query; };
    struct Between { std::string lo, hi; };
    struct LiteralList { std::vector<std::string> values; };

    using Data = std::variant<Literal, Subquery, Between, LiteralList>;
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
    std::string table;
    CondExpr on_expr;
    JoinType join_type;
    std::vector<std::string> using_cols;
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

    using Data = std::variant<Count, CountDistinct, Sum, Avg, Min, Max, SumDistinct, AvgDistinct,
                               Stddev, Variance, GroupConcat, CountCase, SumCase>;
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

    using Data = std::variant<Abort, Ignore, Update>;
    Data data;

    InsertConflict() : data(Abort{}) {}
    template <typename Alt, typename = std::enable_if_t<!std::is_same_v<std::decay_t<Alt>, InsertConflict>>>
    InsertConflict(Alt alt) : data(std::move(alt)) {}
};

// ---------------------------------------------------------------------------
// SelectColumn
// ---------------------------------------------------------------------------
struct SelectColumn {
    struct All {};
    struct Column { std::string name; };
    struct ColumnAlias { std::string name, alias; };
    struct Agg { AggFunc func; std::string col; };
    struct AggAlias { AggFunc func; std::string col; std::string alias; };
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

    using Data = std::variant<AddColumn, DropColumn, RenameColumn, ModifyColumn, RenameTable,
                               AddForeignKey, DropForeignKey, AddUniqueConstraint,
                               AddCheckConstraint, DropConstraint>;
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
        DeallocatePrepare, SetUserVar>;
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
