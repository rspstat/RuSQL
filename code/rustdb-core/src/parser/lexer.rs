// src/parser/lexer.rs

#[derive(Debug, PartialEq, Clone)]
pub enum Token {
    // 키워드
    Select, From, Where, Insert, Into, Values,
    Update, Set, Delete, Create, Table, Drop,
    Join, Left, Right, Cross, Natural, Outer, On, And, Or, Not,
    Alter, Add, Column, Rename, To,
    Order, Group, By, Asc, Desc, Limit,
    Count, Sum, Avg, Min, Max,
    Having, In, Between, Like,
    Index, Unique, View, As,
    Primary, Key, Null, Auto, Increment,
    Show, Tables, Describe, Truncate,
    References, Foreign, Constraint,

    // 데이터 타입
    Int, Text, Float, Boolean,

    // 기호
    Asterisk, Comma, Semicolon, LParen, RParen, Dot,

    // 연산자
    Eq, Ne, Gt, Lt, Gte, Lte,

    // 값
    Ident(String),
    StringLit(String),
    NumberLit(String),

    // FK 제약조건
    Cascade, Restrict,

    // NOT NULL
    Is,

    // 체크포인트
    Checkpoint,

    // 격리 수준
    Isolation,
    Level,
    Uncommitted,
    Committed,
    Repeatable,
    Serializable,

    // MVCC
    Vacuum,

    // Row-level locking
    For,
    Locks,

    // DISTINCT
    Distinct,

    // DEFAULT 값
    Default,

    // ALTER TABLE MODIFY
    Modify,

    // EXISTS / NOT EXISTS
    Exists,

    // SAVEPOINT
    Savepoint,
    Release,

    // EXPLAIN
    Explain,

    // 새 데이터 타입
    Varchar,
    Date,
    Datetime,
    Timestamp,
    Decimal,
    Double,
    Time,
    Year,
    Enum,

    // DATABASE
    Database,

    // INNER JOIN
    Inner,

    // CHECK 제약
    Check,

    // 스칼라 함수 (SELECT / WHERE 용)
    Upper, Lower, Length, Trim, Concat, Substr, Substring,
    Now, Curdate, DateFormat, Coalesce, Ifnull, Replace,

    // 수학 함수
    Round, Abs, Ceil, Floor, Mod,

    // INTERVAL (DATE_ADD / DATE_SUB)
    Interval,

    // CASE WHEN
    Case,
    When,
    Then,
    Else,
    End,

    // UNION / UNION ALL
    Union,
    All,

    // IF()
    If,

    // 새 함수/키워드
    GroupConcat,
    Ignore,
    Duplicate,
    Nullif,
    Lpad,
    Rpad,
    Cast,
    DateAdd,
    DateDiff,
    Separator,

    // 산술 연산자
    Plus,
    Minus,
    Slash,

    // OFFSET
    Offset,

    // CTE
    With,
    Recursive,

    // USE DATABASE
    Use,

    // CREATE USER / GRANT / REVOKE
    User,
    Identified,
    Grant,
    Revoke,
    Privileges,
    Grants,
    OptionKw,
    At,
    Password,
    Databases,

    // EXPLAIN ANALYZE
    Analyze,

    // 윈도우 함수
    RowNumber,
    Rank,
    DenseRank,
    Lag,
    Lead,
    Over,
    Partition,
    FirstValue,
    LastValue,
    NthValue,
}

pub struct Lexer {
    input: Vec<char>,
    pos: usize,
}

impl Lexer {
    pub fn new(input: &str) -> Self {
        Lexer { input: input.chars().collect(), pos: 0 }
    }

    fn peek(&self) -> Option<char> {
        self.input.get(self.pos).copied()
    }

    fn advance(&mut self) -> Option<char> {
        let ch = self.input.get(self.pos).copied();
        self.pos += 1;
        ch
    }

    fn skip_whitespace(&mut self) {
        while let Some(ch) = self.peek() {
            if ch.is_whitespace() { self.advance(); } else { break; }
        }
    }

    fn read_string(&mut self) -> Token {
        self.advance();
        let mut s = String::new();
        while let Some(ch) = self.peek() {
            if ch == '\'' { self.advance(); break; }
            s.push(ch); self.advance();
        }
        Token::StringLit(s)
    }

    fn read_number(&mut self) -> Token {
        let mut s = String::new();
        while let Some(ch) = self.peek() {
            if ch.is_ascii_digit() || ch == '.' { s.push(ch); self.advance(); } else { break; }
        }
        Token::NumberLit(s)
    }

    fn read_ident(&mut self) -> Token {
        let mut s = String::new();
        while let Some(ch) = self.peek() {
            if ch.is_alphanumeric() || ch == '_' { s.push(ch); self.advance(); } else { break; }
        }
        match s.to_uppercase().as_str() {
            "SELECT"    => Token::Select,
            "FROM"      => Token::From,
            "WHERE"     => Token::Where,
            "INSERT"    => Token::Insert,
            "INTO"      => Token::Into,
            "VALUES"    => Token::Values,
            "UPDATE"    => Token::Update,
            "SET"       => Token::Set,
            "DELETE"    => Token::Delete,
            "CREATE"    => Token::Create,
            "TABLE"     => Token::Table,
            "DROP"      => Token::Drop,
            "JOIN"      => Token::Join,
            "LEFT"      => Token::Left,
            "RIGHT"     => Token::Right,
            "CROSS"     => Token::Cross,
            "NATURAL"   => Token::Natural,
            "OUTER"     => Token::Outer,
            "ON"        => Token::On,
            "AND"       => Token::And,
            "OR"        => Token::Or,
            "NOT"       => Token::Not,
            "ALTER"     => Token::Alter,
            "ADD"       => Token::Add,
            "COLUMN"    => Token::Column,
            "RENAME"    => Token::Rename,
            "TO"        => Token::To,
            "ORDER"     => Token::Order,
            "GROUP"     => Token::Group,
            "BY"        => Token::By,
            "ASC"       => Token::Asc,
            "DESC"      => Token::Desc,
            "LIMIT"     => Token::Limit,
            "COUNT"     => Token::Count,
            "SUM"       => Token::Sum,
            "AVG"       => Token::Avg,
            "MIN"       => Token::Min,
            "MAX"       => Token::Max,
            "HAVING"    => Token::Having,
            "IN"        => Token::In,
            "BETWEEN"   => Token::Between,
            "LIKE"      => Token::Like,
            "INDEX"     => Token::Index,
            "UNIQUE"    => Token::Unique,
            "VIEW"      => Token::View,
            "AS"        => Token::As,
            "PRIMARY"   => Token::Primary,
            "KEY"       => Token::Key,
            "NULL"      => Token::Null,
            "AUTO"      => Token::Auto,
            "INCREMENT" => Token::Increment,
            "SHOW"      => Token::Show,
            "TABLES"    => Token::Tables,
            "DESCRIBE"  => Token::Describe,
            "TRUNCATE"  => Token::Truncate,
            "INT"       => Token::Int,
            "TEXT"      => Token::Text,
            "FLOAT"     => Token::Float,
            "BOOLEAN"   => Token::Boolean,
            "REFERENCES" => Token::References,
            "FOREIGN"    => Token::Foreign,
            "CONSTRAINT" => Token::Constraint,
            "CASCADE"  => Token::Cascade,
            "RESTRICT" => Token::Restrict,
            "IS"           => Token::Is,
            "CHECKPOINT"   => Token::Checkpoint,
            "ISOLATION"    => Token::Isolation,
            "LEVEL"        => Token::Level,
            "UNCOMMITTED"  => Token::Uncommitted,
            "COMMITTED"    => Token::Committed,
            "REPEATABLE"   => Token::Repeatable,
            "SERIALIZABLE" => Token::Serializable,
            "VACUUM"       => Token::Vacuum,
            "FOR"          => Token::For,
            "LOCKS"        => Token::Locks,
            "DISTINCT"     => Token::Distinct,
            "DEFAULT"      => Token::Default,
            "MODIFY"       => Token::Modify,
            "EXISTS"       => Token::Exists,
            "SAVEPOINT"    => Token::Savepoint,
            "RELEASE"      => Token::Release,
            "EXPLAIN"      => Token::Explain,
            "VARCHAR"      => Token::Varchar,
            "DATE"         => Token::Date,
            "DATETIME"     => Token::Datetime,
            "TIMESTAMP"    => Token::Timestamp,
            "DECIMAL"      => Token::Decimal,
            "DOUBLE"       => Token::Double,
            "TIME"         => Token::Time,
            "YEAR"         => Token::Year,
            "ENUM"         => Token::Enum,
            "DATABASE"     => Token::Database,
            "INNER"        => Token::Inner,
            "SCHEMA"       => Token::Database,
            "CHECK"        => Token::Check,
            "UPPER"        => Token::Upper,
            "LOWER"        => Token::Lower,
            "LENGTH"       => Token::Length,
            "TRIM"         => Token::Trim,
            "CONCAT"       => Token::Concat,
            "SUBSTR"       => Token::Substr,
            "SUBSTRING"    => Token::Substring,
            "NOW"          => Token::Now,
            "CURDATE"      => Token::Curdate,
            "DATE_FORMAT"  => Token::DateFormat,
            "COALESCE"     => Token::Coalesce,
            "IFNULL"       => Token::Ifnull,
            "REPLACE"      => Token::Replace,
            "ROUND"        => Token::Round,
            "ABS"          => Token::Abs,
            "CEIL"         => Token::Ceil,
            "FLOOR"        => Token::Floor,
            "MOD"          => Token::Mod,
            "INTERVAL"     => Token::Interval,
            "CASE"         => Token::Case,
            "WHEN"         => Token::When,
            "THEN"         => Token::Then,
            "ELSE"         => Token::Else,
            "END"          => Token::End,
            "UNION"        => Token::Union,
            "ALL"          => Token::All,
            "IF"           => Token::If,
            "GROUP_CONCAT" => Token::GroupConcat,
            "IGNORE"       => Token::Ignore,
            "DUPLICATE"    => Token::Duplicate,
            "NULLIF"       => Token::Nullif,
            "LPAD"         => Token::Lpad,
            "RPAD"         => Token::Rpad,
            "CAST"         => Token::Cast,
            "DATE_ADD"     => Token::DateAdd,
            "DATEDIFF"     => Token::DateDiff,
            "SEPARATOR"    => Token::Separator,
            "OFFSET"       => Token::Offset,
            "WITH"         => Token::With,
            "RECURSIVE"    => Token::Recursive,
            "USE"          => Token::Use,
            "USER"         => Token::User,
            "IDENTIFIED"   => Token::Identified,
            "GRANT"        => Token::Grant,
            "REVOKE"       => Token::Revoke,
            "PRIVILEGES"   => Token::Privileges,
            "GRANTS"       => Token::Grants,
            "OPTION"       => Token::OptionKw,
            "PASSWORD"     => Token::Password,
            "DATABASES"    => Token::Databases,
            "ANALYZE"      => Token::Analyze,
            "ROW_NUMBER"   => Token::RowNumber,
            "FIRST_VALUE"  => Token::FirstValue,
            "LAST_VALUE"   => Token::LastValue,
            "NTH_VALUE"    => Token::NthValue,
            "RANK"         => Token::Rank,
            "DENSE_RANK"   => Token::DenseRank,
            "LAG"          => Token::Lag,
            "LEAD"         => Token::Lead,
            "OVER"         => Token::Over,
            "PARTITION"    => Token::Partition,
            _              => Token::Ident(s),
        }
    }

    pub fn tokenize(&mut self) -> Vec<Token> {
        let mut tokens = Vec::new();
        loop {
            self.skip_whitespace();
            match self.peek() {
                None => break,
                Some(ch) => {
                    let tok = match ch {
                        // ── 주석 처리 ──────────────────────────────
                        '-' => {
                            self.advance();
                            if self.peek() == Some('-') {
                                // -- 한 줄 주석: 줄 끝까지 건너뜀
                                while let Some(c) = self.peek() {
                                    self.advance();
                                    if c == '\n' { break; }
                                }
                                continue;
                            } else if self.peek().map(|c| c.is_ascii_digit()).unwrap_or(false) {
                                // 음수 리터럴: -숫자
                                let mut s = "-".to_string();
                                while let Some(ch) = self.peek() {
                                    if ch.is_ascii_digit() || ch == '.' { s.push(ch); self.advance(); } else { break; }
                                }
                                Token::NumberLit(s)
                            } else {
                                Token::Minus
                            }
                        }
                        '+' => { self.advance(); Token::Plus }
                        '@' => { self.advance(); Token::At }
                        '#' => {
                            // # 한 줄 주석: 줄 끝까지 건너뜀
                            while let Some(c) = self.peek() {
                                self.advance();
                                if c == '\n' { break; }
                            }
                            continue;
                        }
                        '/' => {
                            self.advance();
                            if self.peek() == Some('*') {
                                // /* */ 블록 주석: */ 가 나올 때까지 건너뜀
                                self.advance(); // consume '*'
                                loop {
                                    match self.advance() {
                                        None => break,
                                        Some('*') if self.peek() == Some('/') => {
                                            self.advance(); // consume '/'
                                            break;
                                        }
                                        _ => {}
                                    }
                                }
                                continue;
                            } else {
                                Token::Slash
                            }
                        }
                        // ── 기존 토큰 ──────────────────────────────
                        '*' => { self.advance(); Token::Asterisk }
                        ',' => { self.advance(); Token::Comma }
                        ';' => { self.advance(); Token::Semicolon }
                        '(' => { self.advance(); Token::LParen }
                        ')' => { self.advance(); Token::RParen }
                        '.' => { self.advance(); Token::Dot }
                        '=' => { self.advance(); Token::Eq }
                        '>' => {
                            self.advance();
                            if self.peek() == Some('=') { self.advance(); Token::Gte }
                            else { Token::Gt }
                        }
                        '<' => {
                            self.advance();
                            if self.peek() == Some('=') { self.advance(); Token::Lte }
                            else { Token::Lt }
                        }
                        '!' => {
                            self.advance();
                            if self.peek() == Some('=') { self.advance(); Token::Ne }
                            else { continue }
                        }
                        '\'' => self.read_string(),
                        c if c.is_ascii_digit() => self.read_number(),
                        c if c.is_alphabetic() || c == '_' => self.read_ident(),
                        _ => { self.advance(); continue }
                    };
                    tokens.push(tok);
                }
            }
        }
        tokens
    }
}