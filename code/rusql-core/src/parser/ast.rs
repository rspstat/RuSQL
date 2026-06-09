#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum IsolationLevel {
    ReadUncommitted,
    ReadCommitted,
    RepeatableRead,
    Serializable,
}

use serde::{Serialize, Deserialize};

/// Arithmetic expression tree (for SELECT columns and WHERE left-hand side)
#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum ArithExpr {
    Col(String),
    Num(String),
    Str(String),
    Add(Box<ArithExpr>, Box<ArithExpr>),
    Sub(Box<ArithExpr>, Box<ArithExpr>),
    Mul(Box<ArithExpr>, Box<ArithExpr>),
    Div(Box<ArithExpr>, Box<ArithExpr>),
    Func(String, Vec<ArithExpr>),
    Cmp(Box<ArithExpr>, String, Box<ArithExpr>),
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum DataType {
    Int,
    BigInt,
    SmallInt,
    TinyInt,
    Text,
    Float,
    Boolean,
    Varchar(u32),
    Date,
    DateTime,
    Timestamp,
    Decimal(u8, u8),
    Double,
    Time,
    Year,
    Enum(Vec<String>),
    Set(Vec<String>),
    Blob,
    Json,
    #[serde(other)]
    Unknown,
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum FkAction {
    Restrict,
    Cascade,
    SetNull,
    SetDefault,
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub struct ForeignKey {
    pub column: String,
    pub ref_table: String,
    pub ref_column: String,
    pub on_delete: FkAction,
    pub on_update: FkAction,
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub struct ColumnDef {
    pub name: String,
    pub data_type: DataType,
    pub primary_key: bool,
    pub not_null: bool,
    pub unique: bool,
    pub unique_constraint_name: Option<String>,
    pub auto_increment: bool,
    pub default: Option<String>,
    pub foreign_key: Option<ForeignKey>,
    pub check_expr: Option<String>,
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub struct OrderBy {
    pub column: String,
    pub ascending: bool,
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub struct Join {
    pub table: String,
    pub on_expr: CondExpr,   // full ON condition (merged row is evaluated)
    pub join_type: JoinType,
    pub using_cols: Vec<String>, // USING(col, ...) — empty if ON clause was used
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum JoinType {
    Inner,
    Left,
    Right,
    Cross,
    Natural,
    FullOuter,
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum Operator {
    Eq, Ne, Gt, Lt, Gte, Lte,
    In, NotIn,
    Like, Between,
    IsNull, IsNotNull,
    Exists, NotExists,
    Regexp,
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum ConditionValue {
    Literal(String),
    Subquery(Box<Statement>),
    Between(String, String),
    LiteralList(Vec<String>),
}

/// Leaf predicate (single comparison)
#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub struct Condition {
    pub left: ArithExpr,
    pub operator: Operator,
    pub value: ConditionValue,
}

/// Boolean expression tree with proper AND > OR precedence
#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum CondExpr {
    And(Box<CondExpr>, Box<CondExpr>),
    Or(Box<CondExpr>, Box<CondExpr>),
    Not(Box<CondExpr>),
    Leaf(Condition),
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum AggFunc {
    Count,
    CountDistinct,
    Sum, Avg, Min, Max,
    SumDistinct,
    AvgDistinct,
    Stddev,
    Variance,
    GroupConcat { separator: String },
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum FrameBound {
    UnboundedPreceding,
    Preceding(usize),
    CurrentRow,
    Following(usize),
    UnboundedFollowing,
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum FrameUnit { Rows, Range }

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub struct WindowFrame {
    pub unit: FrameUnit,
    pub start: FrameBound,
    pub end: FrameBound,
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum WindowFunc {
    RowNumber,
    Rank,
    DenseRank,
    Lag,
    Lead,
    FirstValue,
    LastValue,
    NthValue,
    Ntile,
    PercentRank,
    CumeDist,
    // 집계 윈도우 함수
    Sum,
    Avg,
    Count,
    Min,
    Max,
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum InsertConflict {
    Abort,
    Ignore,
    Update(Vec<(String, ArithExpr)>),
}

/// CASE WHEN branch: (condition_expression, then_value)
#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub struct CaseWhenBranch {
    pub condition: CondExpr,
    pub result: String,
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum SelectColumn {
    All,
    Column(String),
    ColumnAlias(String, String),
    Agg { func: AggFunc, col: String },
    AggAlias { func: AggFunc, col: String, alias: String },
    Func { name: String, args: Vec<String>, alias: Option<String> },
    Expr { expr: ArithExpr, alias: Option<String> },
    CaseWhen {
        branches: Vec<CaseWhenBranch>,
        else_val: Option<String>,
        alias: Option<String>,
    },
    WinFunc {
        func: WindowFunc,
        col: Option<String>,
        offset: i64,
        partition_by: Vec<String>,
        order_by: Vec<OrderBy>,
        alias: Option<String>,
        frame: Option<WindowFrame>,
    },
    Subquery {
        query: Box<Statement>,
        alias: Option<String>,
    },
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum AlterAction {
    AddColumn(ColumnDef),
    DropColumn(String),
    RenameColumn { from: String, to: String },
    ModifyColumn(ColumnDef),
    RenameTable { to: String },
    // 제약조건 추가/삭제
    AddForeignKey {
        name: Option<String>,
        column: String,
        ref_table: String,
        ref_column: String,
        on_delete: FkAction,
        on_update: FkAction,
    },
    DropForeignKey(String),
    AddUniqueConstraint { name: Option<String>, column: String },
    AddCheckConstraint { name: Option<String>, expr: String },
    DropConstraint(String),
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum Statement {
    Begin,
    Commit,
    Rollback,
    CreateTable {
        name: String,
        columns: Vec<ColumnDef>,
        if_not_exists: bool,
        primary_key_columns: Vec<String>,
        check_constraints: Vec<(Option<String>, String)>,
    },
    DropTable {
        name: String,
        if_exists: bool,
    },
    TruncateTable {
        name: String,
    },
    Insert {
        table: String,
        columns: Option<Vec<String>>,
        values: Vec<Vec<String>>,
        on_conflict: InsertConflict,
        returning: Option<Vec<SelectColumn>>,
    },
    InsertSelect {
        table: String,
        columns: Option<Vec<String>>,
        query: Box<Statement>,
        on_conflict: InsertConflict,
        returning: Option<Vec<SelectColumn>>,
    },
    Select {
        table: String,
        subquery: Option<(Box<Statement>, String)>,
        columns: Vec<SelectColumn>,
        distinct: bool,
        condition: Option<CondExpr>,
        joins: Vec<Join>,
        order_by: Vec<OrderBy>,
        group_by: Option<Vec<String>>,
        having: Option<CondExpr>,
        limit: Option<usize>,
        offset: Option<usize>,
        for_update: bool,
        for_share: bool,
    },
    Update {
        table: String,
        assignments: Vec<(String, ArithExpr)>,
        condition: Option<CondExpr>,
        returning: Option<Vec<SelectColumn>>,
    },
    Delete {
        table: String,
        condition: Option<CondExpr>,
        returning: Option<Vec<SelectColumn>>,
    },
    AlterTable {
        table: String,
        action: AlterAction,
    },
    CreateIndex {
        index_name: String,
        table: String,
        columns: Vec<String>,
        using_hash: bool,
    },
    DropIndex {
        index_name: String,
    },
    CreateView {
        name: String,
        query: Box<Statement>,
        #[serde(default)]
        raw_sql: String,
    },
    DropView {
        name: String,
    },
    ShowTables,
    Describe {
        table: String,
    },
    ShowBufferPool,
    ShowWal,
    Checkpoint,
    SetIsolationLevel(IsolationLevel),
    ShowIsolationLevel,
    Vacuum {
        table: Option<String>,
    },
    ShowLocks,
    Use { database: String },
    Savepoint { name: String },
    ReleaseSavepoint { name: String },
    RollbackTo { name: String },
    Explain(Box<Statement>),
    ExplainAnalyze(Box<Statement>),
    AnalyzeTable {
        table: String,
    },
    With {
        ctes: Vec<(String, Box<Statement>)>,
        query: Box<Statement>,
        recursive: bool,
    },
    Union {
        left: Box<Statement>,
        right: Box<Statement>,
        all: bool,
        order_by: Vec<OrderBy>,
        limit: Option<usize>,
        offset: Option<usize>,
    },
    Intersect {
        left: Box<Statement>,
        right: Box<Statement>,
        all: bool,
        order_by: Vec<OrderBy>,
        limit: Option<usize>,
        offset: Option<usize>,
    },
    Except {
        left: Box<Statement>,
        right: Box<Statement>,
        all: bool,
        order_by: Vec<OrderBy>,
        limit: Option<usize>,
        offset: Option<usize>,
    },
    CreateDatabase {
        name: String,
        if_not_exists: bool,
    },
    DropDatabase {
        name: String,
        if_exists: bool,
    },
    MultiUpdate {
        tables: Vec<String>,
        joins: Vec<Join>,
        assignments: Vec<(String, ArithExpr)>,
        condition: Option<CondExpr>,
    },
    MultiDelete {
        delete_tables: Vec<String>,
        from_table: String,
        joins: Vec<Join>,
        condition: Option<CondExpr>,
    },
    CreateUser {
        user: String,
        host: String,
        password: Option<String>,
        if_not_exists: bool,
    },
    DropUser {
        user: String,
        host: String,
        if_exists: bool,
    },
    Grant {
        privileges: Vec<String>,
        object_type: String,
        object: String,
        user: String,
        host: String,
        with_grant_option: bool,
    },
    Revoke {
        privileges: Vec<String>,
        object_type: String,
        object: String,
        user: String,
        host: String,
    },
    ShowGrants {
        user: Option<String>,
        host: Option<String>,
    },
    // ROLE
    CreateRole {
        name: String,
    },
    DropRole {
        name: String,
        if_exists: bool,
    },
    GrantRole {
        role: String,
        user: String,
        host: String,
        with_admin_option: bool,
    },
    RevokeRole {
        role: String,
        user: String,
        host: String,
    },
    ShowRoles,
    // SYNONYM
    CreateSynonym {
        name: String,
        target: String,
        or_replace: bool,
    },
    DropSynonym {
        name: String,
        if_exists: bool,
    },
    ShowSynonyms,
    ShowDatabases,
    ShowCreateTable {
        table: String,
    },
    ShowCreateView {
        view: String,
    },
    ShowIndex {
        table: String,
    },
    Merge {
        target: String,
        target_alias: Option<String>,
        source: String,
        source_alias: Option<String>,
        on: CondExpr,
        when_matched_update: Option<Vec<(String, ArithExpr)>>,
        when_matched_delete: bool,
        when_matched_delete_cond: Option<CondExpr>,
        when_not_matched_columns: Option<Vec<String>>,
        when_not_matched_values: Vec<String>,
    },
    CreateProcedure {
        name: String,
        params: Vec<(String, String, String)>,  // (IN/OUT/INOUT, name, type)
        body: Vec<Statement>,
    },
    CallProcedure {
        name: String,
        args: Vec<String>,
    },
    CreateTrigger {
        name: String,
        timing: TriggerTiming,
        event: TriggerEvent,
        table: String,
        body: Vec<Statement>,
    },
    DropTrigger {
        name: String,
        if_exists: bool,
    },
    DropProcedure {
        name: String,
        if_exists: bool,
    },
    Backup {
        database: Option<String>,
        output_file: Option<String>,
    },
    ShowProcessList,
    CreateFunction {
        name: String,
        params: Vec<String>,
        body: String,
    },
    DropFunction {
        name: String,
        if_exists: bool,
    },
    // 저장 프로시저 제어문
    ProcDeclare {
        name: String,
        typ: String,
        default: Option<String>,
    },
    ProcSet {
        name: String,
        expr: ArithExpr,
    },
    ProcIf {
        condition: CondExpr,
        then_body: Vec<Statement>,
        elseif_branches: Vec<(CondExpr, Vec<Statement>)>,
        else_body: Option<Vec<Statement>>,
    },
    ProcWhile {
        label: Option<String>,
        condition: CondExpr,
        body: Vec<Statement>,
    },
    ProcLoop {
        label: Option<String>,
        body: Vec<Statement>,
    },
    ProcRepeat {
        label: Option<String>,
        body: Vec<Statement>,
        until: CondExpr,
    },
    ProcLeave {
        label: Option<String>,
    },
    ProcIterate {
        label: Option<String>,
    },
    PrepareStmt {
        name: String,
        query: String,
    },
    ExecuteStmt {
        name: String,
        using_vars: Vec<String>,
    },
    DeallocatePrepare {
        name: String,
    },
    SetUserVar {
        name: String,
        expr: ArithExpr,
    },
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum TriggerTiming { Before, After }

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum TriggerEvent { Insert, Update, Delete }
