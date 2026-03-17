// src/parser/ast.rs

#[derive(Debug, PartialEq, Clone)]
pub enum DataType {
    Int,
    Text,
    Float,
    Boolean,
}

#[derive(Debug, PartialEq, Clone)]
pub struct ColumnDef {
    pub name: String,
    pub data_type: DataType,
}

#[derive(Debug, PartialEq, Clone)]
pub struct OrderBy {
    pub column: String,
    pub ascending: bool,
}

#[derive(Debug, PartialEq, Clone)]
pub struct Join {
    pub table: String,
    pub left_col: String,
    pub right_col: String,
}

#[derive(Debug, PartialEq, Clone)]
pub enum Operator {
    Eq, Ne, Gt, Lt, Gte, Lte, In,
}

#[derive(Debug, PartialEq, Clone)]
pub enum ConditionValue {
    Literal(String),
    Subquery(Box<Statement>),
}

#[derive(Debug, PartialEq, Clone)]
pub struct Condition {
    pub column: String,
    pub operator: Operator,
    pub value: ConditionValue,
}

#[derive(Debug, PartialEq, Clone)]
pub enum AggFunc {
    Count, Sum, Avg, Min, Max,
}

#[derive(Debug, PartialEq, Clone)]
pub enum SelectColumn {
    All,
    Column(String),
    Agg { func: AggFunc, col: String },
}

#[derive(Debug, PartialEq, Clone)]
pub enum AlterAction {
    AddColumn(ColumnDef),
    DropColumn(String),
    RenameColumn { from: String, to: String },
}

#[derive(Debug, PartialEq, Clone)]
pub enum Statement {
    Begin,
    Commit,
    Rollback,
    CreateTable {
        name: String,
        columns: Vec<ColumnDef>,
    },
    DropTable {
        name: String,
    },
    Insert {
        table: String,
        values: Vec<String>,
    },
    Select {
        table: String,
        columns: Vec<SelectColumn>,
        condition: Option<Condition>,
        join: Option<Join>,
        order_by: Option<OrderBy>,
        group_by: Option<Vec<String>>,
        having: Option<Condition>,
        limit: Option<usize>,
    },
    Update {
        table: String,
        assignments: Vec<(String, String)>,
        condition: Option<Condition>,
    },
    Delete {
        table: String,
        condition: Option<Condition>,
    },
    AlterTable {
        table: String,
        action: AlterAction,
    },
    CreateIndex {
    index_name: String,
    table: String,
    column: String,
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
}