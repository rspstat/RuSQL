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
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum DataType {
    Int,
    Text,
    Float,
    Boolean,
    Varchar(u32),
    Date,
    DateTime,
    Timestamp,
    Decimal(u8, u8),
    #[serde(other)]
    Unknown,
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum FkAction {
    Restrict,
    Cascade,
    SetNull,
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
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum JoinType {
    Inner,
    Left,
    Right,
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum Operator {
    Eq, Ne, Gt, Lt, Gte, Lte,
    In, NotIn,
    Like, Between,
    IsNull, IsNotNull,
    Exists, NotExists,
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
    Count, Sum, Avg, Min, Max,
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
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum AlterAction {
    AddColumn(ColumnDef),
    DropColumn(String),
    RenameColumn { from: String, to: String },
    ModifyColumn(ColumnDef),
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
    },
    InsertSelect {
        table: String,
        columns: Option<Vec<String>>,
        query: Box<Statement>,
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
    },
    Update {
        table: String,
        assignments: Vec<(String, ArithExpr)>,
        condition: Option<CondExpr>,
    },
    Delete {
        table: String,
        condition: Option<CondExpr>,
    },
    AlterTable {
        table: String,
        action: AlterAction,
    },
    CreateIndex {
        index_name: String,
        table: String,
        columns: Vec<String>,
    },
    DropIndex {
        index_name: String,
    },
    CreateView {
        name: String,
        query: Box<Statement>,
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
    Savepoint { name: String },
    ReleaseSavepoint { name: String },
    RollbackTo { name: String },
    Explain(Box<Statement>),
    With {
        ctes: Vec<(String, Box<Statement>)>,
        query: Box<Statement>,
    },
    Union {
        left: Box<Statement>,
        right: Box<Statement>,
        all: bool,
        order_by: Vec<OrderBy>,
        limit: Option<usize>,
        offset: Option<usize>,
    },
}
