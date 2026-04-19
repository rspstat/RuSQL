#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum IsolationLevel {
    ReadUncommitted,  // 더티 읽기 허용
    ReadCommitted,    // 커밋된 데이터만 읽기
    RepeatableRead,   // 트랜잭션 시작 시 스냅샷 고정
    Serializable,     // RepeatableRead + 팬텀 읽기 방지 검증
}

use serde::{Serialize, Deserialize};

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum DataType {
    Int,
    Text,
    Float,
    Boolean,
    Varchar(u32),        // VARCHAR(n)
    Date,                // DATE — "YYYY-MM-DD"
    DateTime,            // DATETIME — "YYYY-MM-DD HH:MM:SS"
    Timestamp,           // TIMESTAMP — "YYYY-MM-DD HH:MM:SS" (UTC 기준)
    Decimal(u8, u8),     // DECIMAL(precision, scale)
    #[serde(other)]
    Unknown,             // 구버전 스키마 호환용
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum FkAction {
    Restrict,   // 기본값 - 삭제 거부
    Cascade,    // 연쇄 삭제
    SetNull,    // NULL로 설정
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub struct ForeignKey {
    pub column: String,
    pub ref_table: String,
    pub ref_column: String,
    pub on_delete: FkAction,
    pub on_update: FkAction,  // ON UPDATE CASCADE / RESTRICT / SET NULL
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub struct ColumnDef {
    pub name: String,
    pub data_type: DataType,
    pub primary_key: bool,
    pub not_null: bool,
    pub unique: bool,
    pub unique_constraint_name: Option<String>,  // CONSTRAINT name UNIQUE
    pub auto_increment: bool,
    pub default: Option<String>,                 // DEFAULT value
    pub foreign_key: Option<ForeignKey>,
    pub check_expr: Option<String>,              // CHECK (expr) — raw SQL string
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub struct OrderBy {
    pub column: String,
    pub ascending: bool,
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub struct Join {
    pub table: String,
    pub left_col: String,
    pub right_col: String,
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
    In, NotIn,           // IN / NOT IN (서브쿼리)
    Like, Between,
    IsNull, IsNotNull,
    Exists, NotExists,   // EXISTS / NOT EXISTS (서브쿼리)
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum ConditionValue {
    Literal(String),
    Subquery(Box<Statement>),
    Between(String, String),  // BETWEEN a AND b
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub struct Condition {
    pub column: String,
    pub operator: Operator,
    pub value: ConditionValue,
    pub and: Option<Box<Condition>>,  // AND 연결
    pub or: Option<Box<Condition>>,   // OR 연결
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum AggFunc {
    Count, Sum, Avg, Min, Max,
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum SelectColumn {
    All,
    Column(String),
    ColumnAlias(String, String),               // col AS alias
    Agg { func: AggFunc, col: String },
    AggAlias { func: AggFunc, col: String, alias: String },
    /// 스칼라 함수: SELECT UPPER(name), NOW() AS ts, ...
    Func { name: String, args: Vec<String>, alias: Option<String> },
}

#[derive(Debug, PartialEq, Clone, Serialize, Deserialize)]
pub enum AlterAction {
    AddColumn(ColumnDef),
    DropColumn(String),
    RenameColumn { from: String, to: String },
    ModifyColumn(ColumnDef),  // ALTER TABLE t MODIFY COLUMN col TYPE [constraints]
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
        /// 테이블 레벨 복합 PK: PRIMARY KEY (col1, col2)
        primary_key_columns: Vec<String>,
        /// 테이블 레벨 CHECK 제약 목록: (name?, expr_string)
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
        /// 컬럼 지정 삽입: INSERT INTO t (col1, col2) VALUES (...)
        columns: Option<Vec<String>>,
        /// 멀티 row: VALUES (...), (...)
        values: Vec<Vec<String>>,
    },
    Select {
        table: String,
        /// FROM (SELECT ...) AS alias — Some일 때 table은 빈 문자열
        subquery: Option<(Box<Statement>, String)>,
        columns: Vec<SelectColumn>,
        distinct: bool,
        condition: Option<Condition>,
        joins: Vec<Join>,
        order_by: Vec<OrderBy>,
        group_by: Option<Vec<String>>,
        having: Option<Condition>,
        limit: Option<usize>,
        for_update: bool,
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
    /// VACUUM [table] — 논리 삭제된 행을 물리적으로 제거
    Vacuum {
        table: Option<String>,
    },
    ShowLocks,
    /// SAVEPOINT name
    Savepoint { name: String },
    /// RELEASE SAVEPOINT name
    ReleaseSavepoint { name: String },
    /// ROLLBACK TO SAVEPOINT name
    RollbackTo { name: String },
    /// EXPLAIN <SELECT>
    Explain(Box<Statement>),
}