// src/parser/parser.rs

use std::collections::HashMap;
use crate::parser::lexer::{Lexer, Token};
use crate::parser::ast::*;

// ─── 테이블 별칭(alias) 확장 헬퍼 ────────────────────────────────────
/// "alias.col" → "real_table.col" 로 확장. 알 수 없는 접두사는 유지.
fn expand_alias_str(s: &str, map: &HashMap<String, String>) -> String {
    if let Some(dot) = s.find('.') {
        let prefix = &s[..dot];
        if let Some(real) = map.get(prefix) {
            return format!("{}.{}", real, &s[dot + 1..]);
        }
    }
    s.to_string()
}

fn expand_arith(expr: ArithExpr, map: &HashMap<String, String>) -> ArithExpr {
    match expr {
        ArithExpr::Col(name) => ArithExpr::Col(expand_alias_str(&name, map)),
        ArithExpr::Num(n)    => ArithExpr::Num(n),
        ArithExpr::Str(s)    => ArithExpr::Str(s),
        ArithExpr::Add(l, r) => ArithExpr::Add(Box::new(expand_arith(*l, map)), Box::new(expand_arith(*r, map))),
        ArithExpr::Sub(l, r) => ArithExpr::Sub(Box::new(expand_arith(*l, map)), Box::new(expand_arith(*r, map))),
        ArithExpr::Mul(l, r) => ArithExpr::Mul(Box::new(expand_arith(*l, map)), Box::new(expand_arith(*r, map))),
        ArithExpr::Div(l, r) => ArithExpr::Div(Box::new(expand_arith(*l, map)), Box::new(expand_arith(*r, map))),
        ArithExpr::Func(name, args) => ArithExpr::Func(name, args.into_iter().map(|a| expand_arith(a, map)).collect()),
        ArithExpr::Cmp(l, op, r) => ArithExpr::Cmp(Box::new(expand_arith(*l, map)), op, Box::new(expand_arith(*r, map))),
    }
}

fn expand_select_column(col: SelectColumn, map: &HashMap<String, String>) -> SelectColumn {
    match col {
        SelectColumn::Column(name) =>
            SelectColumn::Column(expand_alias_str(&name, map)),
        SelectColumn::ColumnAlias(name, alias) =>
            SelectColumn::ColumnAlias(expand_alias_str(&name, map), alias),
        SelectColumn::Func { name, args, alias } =>
            SelectColumn::Func {
                name,
                args: args.iter().map(|a| expand_alias_str(a, map)).collect(),
                alias,
            },
        SelectColumn::Expr { expr, alias } =>
            SelectColumn::Expr { expr: expand_arith(expr, map), alias },
        SelectColumn::CaseWhen { branches, else_val, alias } =>
            SelectColumn::CaseWhen {
                branches: branches.into_iter().map(|b| CaseWhenBranch {
                    condition: expand_condexpr(b.condition, map),
                    result: b.result,
                }).collect(),
                else_val,
                alias,
            },
        other => other,
    }
}

fn expand_leaf(cond: Condition, map: &HashMap<String, String>) -> Condition {
    Condition {
        left: expand_arith(cond.left, map),
        operator: cond.operator,
        value: match cond.value {
            ConditionValue::Literal(s) =>
                ConditionValue::Literal(expand_alias_str(&s, map)),
            ConditionValue::Between(a, b) =>
                ConditionValue::Between(expand_alias_str(&a, map), expand_alias_str(&b, map)),
            other => other,
        },
    }
}

fn expand_condexpr(expr: CondExpr, map: &HashMap<String, String>) -> CondExpr {
    match expr {
        CondExpr::And(l, r) =>
            CondExpr::And(Box::new(expand_condexpr(*l, map)), Box::new(expand_condexpr(*r, map))),
        CondExpr::Or(l, r) =>
            CondExpr::Or(Box::new(expand_condexpr(*l, map)), Box::new(expand_condexpr(*r, map))),
        CondExpr::Not(inner) =>
            CondExpr::Not(Box::new(expand_condexpr(*inner, map))),
        CondExpr::Leaf(cond) =>
            CondExpr::Leaf(expand_leaf(cond, map)),
    }
}

/// DEFAULT NULL 을 나타내는 내부 마커 (executor와 공유)
pub const NULL_DEFAULT: &str = "__NULL_DEFAULT__";

pub struct Parser {
    tokens: Vec<Token>,
    pos: usize,
}

impl Parser {
    pub fn new(input: &str) -> Self {
        let mut lexer = Lexer::new(input);
        Parser {
            tokens: lexer.tokenize(),
            pos: 0,
        }
    }

    fn peek(&self) -> Option<&Token> {
        self.tokens.get(self.pos)
    }

    fn advance(&mut self) -> Option<&Token> {
        let tok = self.tokens.get(self.pos);
        self.pos += 1;
        tok
    }

    fn expect_ident(&mut self) -> Result<String, String> {
        match self.advance() {
            Some(Token::Ident(s)) => Ok(s.clone()),
            other => Err(format!("Expected identifier, got {:?}", other)),
        }
    }

    /// 키워드도 식별자로 허용 (AS alias 위치에서 사용)
    fn expect_alias_ident(&mut self) -> Result<String, String> {
        match self.advance() {
            Some(Token::Ident(s)) => Ok(s.clone()),
            // 자주 alias로 쓰이는 키워드들
            Some(Token::Now)       => Ok("now".to_string()),
            Some(Token::Date)      => Ok("date".to_string()),
            Some(Token::Count)     => Ok("count".to_string()),
            Some(Token::Sum)       => Ok("sum".to_string()),
            Some(Token::Avg)       => Ok("avg".to_string()),
            Some(Token::Min)       => Ok("min".to_string()),
            Some(Token::Max)       => Ok("max".to_string()),
            Some(Token::Key)       => Ok("key".to_string()),
            Some(Token::Set)       => Ok("set".to_string()),
            Some(Token::Select)    => Ok("select".to_string()),
            Some(Token::From)      => Ok("from".to_string()),
            Some(Token::Where)     => Ok("where".to_string()),
            Some(Token::Table)     => Ok("table".to_string()),
            Some(Token::Order)     => Ok("order".to_string()),
            Some(Token::Group)     => Ok("group".to_string()),
            Some(Token::Index)     => Ok("index".to_string()),
            Some(Token::View)      => Ok("view".to_string()),
            other => Err(format!("Expected identifier (alias), got {:?}", other)),
        }
    }

    // table.column 형태를 허용하며, 테이블 접두사는 무시하고 컬럼명만 반환
    fn expect_col_ref(&mut self) -> Result<String, String> {
        let first = self.expect_any_ident()?;
        if self.peek() == Some(&Token::Dot) {
            self.advance(); // consume '.'
            let col = self.expect_any_name()?;
            Ok(format!("{}.{}", first, col)) // table.column 전체 보존
        } else {
            Ok(first)
        }
    }

    /// Any token that can serve as an identifier in dotted names (schema.table, table.column).
    /// Accepts Token::Ident plus keyword tokens that appear as table/column names.
    fn expect_any_name(&mut self) -> Result<String, String> {
        match self.advance() {
            Some(Token::Ident(s))   => Ok(s.clone()),
            Some(Token::Tables)     => Ok("tables".to_string()),
            Some(Token::Column)     => Ok("column".to_string()),
            Some(Token::Database)   => Ok("schema".to_string()),
            Some(Token::Index)      => Ok("index".to_string()),
            Some(Token::View)       => Ok("view".to_string()),
            Some(Token::Key)        => Ok("key".to_string()),
            Some(Token::Set)        => Ok("set".to_string()),
            Some(Token::Count)      => Ok("count".to_string()),
            Some(Token::Sum)        => Ok("sum".to_string()),
            Some(Token::Avg)        => Ok("avg".to_string()),
            Some(Token::Min)        => Ok("min".to_string()),
            Some(Token::Max)        => Ok("max".to_string()),
            other => Err(format!("Expected identifier, got {:?}", other)),
        }
    }

    /// 식별자 또는 alias로 쓰이는 키워드 모두 허용 (ORDER BY / GROUP BY 컬럼명 파싱용)
    fn expect_any_ident(&mut self) -> Result<String, String> {
        match self.advance() {
            Some(Token::Ident(s))  => Ok(s.clone()),
            Some(Token::Count)     => Ok("count".to_string()),
            Some(Token::Sum)       => Ok("sum".to_string()),
            Some(Token::Avg)       => Ok("avg".to_string()),
            Some(Token::Min)       => Ok("min".to_string()),
            Some(Token::Max)       => Ok("max".to_string()),
            Some(Token::Now)       => Ok("now".to_string()),
            Some(Token::Date)      => Ok("date".to_string()),
            Some(Token::Key)       => Ok("key".to_string()),
            Some(Token::Set)       => Ok("set".to_string()),
            Some(Token::Index)     => Ok("index".to_string()),
            Some(Token::View)      => Ok("view".to_string()),
            other => Err(format!("Expected identifier, got {:?}", other)),
        }
    }

    pub fn parse(&mut self) -> Result<Statement, String> {
        match self.advance() {
            Some(Token::Select) => self.parse_select(),
            Some(Token::Insert) => self.parse_insert(),
            Some(Token::Update) => self.parse_update(),
            Some(Token::Delete) => self.parse_delete(),
            // 기존 parse_create 대신 아래로 교체
            Some(Token::Create) => {
                match self.peek() {
                    Some(Token::Index) => {
                        self.advance();
                        self.parse_create_index()
                    }
                    Some(Token::View) => {
                        self.advance();
                        self.parse_create_view()
                    }
                    Some(Token::Database) => {
                        self.advance();
                        self.parse_create_database()
                    }
                    Some(Token::User) => {
                        self.advance();
                        self.parse_create_user()
                    }
                    Some(Token::Procedure) => {
                        self.advance();
                        self.parse_create_procedure()
                    }
                    Some(Token::Trigger) => {
                        self.advance();
                        self.parse_create_trigger()
                    }
                    Some(Token::Ident(s)) if s.to_uppercase() == "FUNCTION" => {
                        self.advance();
                        self.parse_create_function()
                    }
                    _ => self.parse_create(),
                }
            }
            Some(Token::Drop) => {
                match self.peek() {
                    Some(Token::Index) => {
                        self.advance();
                        self.parse_drop_index()
                    }
                    Some(Token::View) => {
                        self.advance();
                        self.parse_drop_view()
                    }
                    Some(Token::Database) => {
                        self.advance();
                        self.parse_drop_database()
                    }
                    Some(Token::User) => {
                        self.advance();
                        self.parse_drop_user()
                    }
                    Some(Token::Trigger) => {
                        self.advance();
                        self.parse_drop_trigger()
                    }
                    Some(Token::Procedure) => {
                        self.advance();
                        self.parse_drop_procedure()
                    }
                    Some(Token::Ident(s)) if s.to_uppercase() == "FUNCTION" => {
                        self.advance();
                        self.parse_drop_function()
                    }
                    _ => self.parse_drop(),
                }
            }
            Some(Token::Grant)  => self.parse_grant(),
            Some(Token::Revoke) => self.parse_revoke(),
            Some(Token::Ident(s)) if s == "BEGIN"    => Ok(Statement::Begin),
            Some(Token::Ident(s)) if s == "COMMIT"   => Ok(Statement::Commit),
            Some(Token::Ident(s)) if s == "ROLLBACK" => {
                // ROLLBACK TO [SAVEPOINT] name
                if self.peek() == Some(&Token::To) {
                    self.advance(); // TO
                    if self.peek() == Some(&Token::Savepoint) { self.advance(); } // optional SAVEPOINT
                    let name = self.expect_ident()?;
                    Ok(Statement::RollbackTo { name })
                } else {
                    Ok(Statement::Rollback)
                }
            }
            Some(Token::Savepoint) => {
                let name = self.expect_ident()?;
                Ok(Statement::Savepoint { name })
            }
            Some(Token::Release) => {
                if self.peek() == Some(&Token::Savepoint) { self.advance(); }
                let name = self.expect_ident()?;
                Ok(Statement::ReleaseSavepoint { name })
            }
            Some(Token::Analyze) => {
                match self.advance() {
                    Some(Token::Table) => {}
                    other => return Err(format!("Expected TABLE after ANALYZE, got {:?}", other)),
                }
                let table = self.expect_ident()?;
                Ok(Statement::AnalyzeTable { table })
            }
            Some(Token::Explain) => {
                if self.peek() == Some(&Token::Analyze) {
                    self.advance();
                    let inner = self.parse()?;
                    Ok(Statement::ExplainAnalyze(Box::new(inner)))
                } else {
                    let inner = self.parse()?;
                    Ok(Statement::Explain(Box::new(inner)))
                }
            }
            Some(Token::Alter) => self.parse_alter(),
            Some(Token::Show)     => self.parse_show(),
            Some(Token::Describe) => self.parse_describe(),
            Some(Token::Truncate)    => self.parse_truncate(),
            Some(Token::Checkpoint)  => Ok(Statement::Checkpoint),
            Some(Token::Set)         => self.parse_set(),
            Some(Token::Vacuum)      => self.parse_vacuum(),
            Some(Token::With)        => self.parse_with(),
            Some(Token::Use)         => self.parse_use(),
            Some(Token::Merge)       => self.parse_merge(),
            Some(Token::Call)        => self.parse_call(),
            Some(Token::Prepare)     => self.parse_prepare(),
            Some(Token::Execute)     => self.parse_execute(),
            Some(Token::Deallocate)  => self.parse_deallocate(),
            Some(Token::Ident(s)) if s.to_uppercase() == "BACKUP" => self.parse_backup(),
            other => Err(format!("Unknown statement: {:?}", other)),
        }
    }

    fn parse_use(&mut self) -> Result<Statement, String> {
        // USE [DATABASE] name
        if self.peek() == Some(&Token::Database) { self.advance(); }
        let database = self.expect_ident()?;
        Ok(Statement::Use { database })
    }

    fn parse_with(&mut self) -> Result<Statement, String> {
        // WITH [RECURSIVE] cte_name AS (query) [, cte_name AS (query)] ... SELECT ...
        let recursive = if self.peek() == Some(&Token::Recursive) {
            self.advance();
            true
        } else {
            false
        };

        let mut ctes: Vec<(String, Box<Statement>)> = Vec::new();
        loop {
            let name = self.expect_ident()?;
            match self.advance() {
                Some(Token::As) => {}
                other => return Err(format!("Expected AS in CTE, got {:?}", other)),
            }
            match self.advance() {
                Some(Token::LParen) => {}
                other => return Err(format!("Expected '(' in CTE, got {:?}", other)),
            }
            match self.advance() {
                Some(Token::Select) => {}
                other => return Err(format!("Expected SELECT in CTE body, got {:?}", other)),
            }
            let base = self.parse_select()?;
            // 재귀 CTE: body 내 UNION [ALL] 처리
            let body = if self.peek() == Some(&Token::Union) {
                self.advance(); // consume UNION
                let all = if self.peek() == Some(&Token::All) { self.advance(); true } else { false };
                match self.advance() {
                    Some(Token::Select) => {}
                    other => return Err(format!("Expected SELECT after UNION in CTE, got {:?}", other)),
                }
                let recursive_part = self.parse_select()?;
                Statement::Union {
                    left: Box::new(base),
                    right: Box::new(recursive_part),
                    all,
                    order_by: vec![],
                    limit: None,
                    offset: None,
                }
            } else {
                base
            };
            match self.advance() {
                Some(Token::RParen) => {}
                other => return Err(format!("Expected ')' after CTE body, got {:?}", other)),
            }
            ctes.push((name, Box::new(body)));
            if self.peek() == Some(&Token::Comma) {
                self.advance();
            } else {
                break;
            }
        }

        // Main query (any DML: SELECT, INSERT ... SELECT, etc.)
        let query = self.parse()?;
        Ok(Statement::With { ctes, query: Box::new(query), recursive })
    }

    /// Top-level condition expression parser (entry point for WHERE/HAVING/ON)
    fn parse_condexpr(&mut self) -> Result<CondExpr, String> {
        self.parse_or_expr()
    }

    /// OR has lower precedence than AND
    fn parse_or_expr(&mut self) -> Result<CondExpr, String> {
        let mut left = self.parse_and_expr()?;
        while self.peek() == Some(&Token::Or) {
            self.advance();
            let right = self.parse_and_expr()?;
            left = CondExpr::Or(Box::new(left), Box::new(right));
        }
        Ok(left)
    }

    /// AND has higher precedence than OR
    fn parse_and_expr(&mut self) -> Result<CondExpr, String> {
        let mut left = self.parse_not_expr()?;
        while self.peek() == Some(&Token::And) {
            self.advance();
            let right = self.parse_not_expr()?;
            left = CondExpr::And(Box::new(left), Box::new(right));
        }
        Ok(left)
    }

    /// NOT has higher precedence than AND
    fn parse_not_expr(&mut self) -> Result<CondExpr, String> {
        if self.peek() == Some(&Token::Not) {
            // NOT IN and NOT EXISTS are handled inside parse_single_pred
            let next = self.tokens.get(self.pos + 1);
            let is_not_in_or_exists = next == Some(&Token::In) || next == Some(&Token::Exists);
            if !is_not_in_or_exists {
                self.advance(); // consume NOT
                let inner = self.parse_not_expr()?;
                return Ok(CondExpr::Not(Box::new(inner)));
            }
        }
        self.parse_primary_cond()
    }

    /// Handles parenthesized sub-expressions or single predicates
    fn parse_primary_cond(&mut self) -> Result<CondExpr, String> {
        if self.peek() == Some(&Token::LParen) {
            // Could be (SELECT ...) for EXISTS, or grouped condition (a AND b)
            // Peek further: if next-next is SELECT it's a subquery value, handled in parse_single_pred
            // Otherwise it's a grouped condition
            let is_subquery = self.tokens.get(self.pos + 1) == Some(&Token::Select);
            if !is_subquery {
                self.advance(); // consume '('
                let inner = self.parse_or_expr()?;
                match self.advance() {
                    Some(Token::RParen) => {}
                    other => return Err(format!("Expected ')', got {:?}", other)),
                }
                return Ok(inner);
            }
        }
        let cond = self.parse_single_pred()?;
        Ok(CondExpr::Leaf(cond))
    }

    /// Parses a single predicate (leaf node): col OP val, IS NULL, BETWEEN, LIKE, IN, EXISTS, etc.
    fn parse_single_pred(&mut self) -> Result<Condition, String> {
        // EXISTS (SELECT ...)
        if self.peek() == Some(&Token::Exists) {
            self.advance();
            let sub = self.parse_exists_subquery()?;
            return Ok(Condition {
                left: ArithExpr::Col(String::new()), operator: Operator::Exists,
                value: ConditionValue::Subquery(Box::new(sub)),
            });
        }

        // NOT EXISTS (SELECT ...)
        if self.peek() == Some(&Token::Not) && self.tokens.get(self.pos + 1) == Some(&Token::Exists) {
            self.advance(); // NOT
            self.advance(); // EXISTS
            let sub = self.parse_exists_subquery()?;
            return Ok(Condition {
                left: ArithExpr::Col(String::new()), operator: Operator::NotExists,
                value: ConditionValue::Subquery(Box::new(sub)),
            });
        }

        // Left side: arithmetic expression (handles columns, aggregates, arithmetic)
        let left = self.parse_arith_expr()?;

        // IN (subquery or literal list)
        if self.peek() == Some(&Token::In) {
            self.advance();
            match self.advance() {
                Some(Token::LParen) => {}
                other => return Err(format!("Expected '(' after IN, got {:?}", other)),
            }
            if self.peek() == Some(&Token::Select) {
                let sub_stmt = match self.advance() {
                    Some(Token::Select) => self.parse_select()?,
                    _ => unreachable!(),
                };
                match self.advance() {
                    Some(Token::RParen) => {}
                    other => return Err(format!("Expected ')', got {:?}", other)),
                }
                return Ok(Condition { left, operator: Operator::In, value: ConditionValue::Subquery(Box::new(sub_stmt)) });
            } else {
                let mut values = Vec::new();
                loop {
                    let val = match self.advance() {
                        Some(Token::StringLit(s)) => s.clone(),
                        Some(Token::NumberLit(n)) => n.clone(),
                        Some(Token::Ident(s))     => s.clone(),
                        Some(Token::Null)          => "NULL".to_string(),
                        other => return Err(format!("Expected value in IN list, got {:?}", other)),
                    };
                    values.push(val);
                    match self.peek() {
                        Some(Token::Comma)  => { self.advance(); }
                        Some(Token::RParen) => break,
                        other => return Err(format!("Expected ',' or ')' in IN list, got {:?}", other)),
                    }
                }
                self.advance(); // consume ')'
                return Ok(Condition { left, operator: Operator::In, value: ConditionValue::LiteralList(values) });
            }
        }

        // NOT IN (subquery or literal list)
        if self.peek() == Some(&Token::Not) && self.tokens.get(self.pos + 1) == Some(&Token::In) {
            self.advance(); // NOT
            self.advance(); // IN
            match self.advance() {
                Some(Token::LParen) => {}
                other => return Err(format!("Expected '(' after NOT IN, got {:?}", other)),
            }
            if self.peek() == Some(&Token::Select) {
                let sub_stmt = match self.advance() {
                    Some(Token::Select) => self.parse_select()?,
                    _ => unreachable!(),
                };
                match self.advance() {
                    Some(Token::RParen) => {}
                    other => return Err(format!("Expected ')', got {:?}", other)),
                }
                return Ok(Condition { left, operator: Operator::NotIn, value: ConditionValue::Subquery(Box::new(sub_stmt)) });
            } else {
                let mut values = Vec::new();
                loop {
                    let val = match self.advance() {
                        Some(Token::StringLit(s)) => s.clone(),
                        Some(Token::NumberLit(n)) => n.clone(),
                        Some(Token::Ident(s))     => s.clone(),
                        Some(Token::Null)          => "NULL".to_string(),
                        other => return Err(format!("Expected value in NOT IN list, got {:?}", other)),
                    };
                    values.push(val);
                    match self.peek() {
                        Some(Token::Comma)  => { self.advance(); }
                        Some(Token::RParen) => break,
                        other => return Err(format!("Expected ',' or ')' in NOT IN list, got {:?}", other)),
                    }
                }
                self.advance(); // consume ')'
                return Ok(Condition { left, operator: Operator::NotIn, value: ConditionValue::LiteralList(values) });
            }
        }

        // BETWEEN val AND val
        if self.peek() == Some(&Token::Between) {
            self.advance();
            let start = match self.advance() {
                Some(Token::NumberLit(n)) => n.clone(),
                Some(Token::StringLit(s)) => s.clone(),
                Some(Token::Ident(s))     => s.clone(),
                other => return Err(format!("Expected value after BETWEEN, got {:?}", other)),
            };
            match self.advance() {
                Some(Token::And) => {}
                other => return Err(format!("Expected AND in BETWEEN, got {:?}", other)),
            }
            let end = match self.advance() {
                Some(Token::NumberLit(n)) => n.clone(),
                Some(Token::StringLit(s)) => s.clone(),
                Some(Token::Ident(s))     => s.clone(),
                other => return Err(format!("Expected value after BETWEEN ... AND, got {:?}", other)),
            };
            return Ok(Condition { left, operator: Operator::Between, value: ConditionValue::Between(start, end) });
        }

        // LIKE pattern
        if self.peek() == Some(&Token::Like) {
            self.advance();
            let pattern = match self.advance() {
                Some(Token::StringLit(s)) => s.clone(),
                Some(Token::Ident(s))     => s.clone(),
                other => return Err(format!("Expected pattern after LIKE, got {:?}", other)),
            };
            return Ok(Condition { left, operator: Operator::Like, value: ConditionValue::Literal(pattern) });
        }

        // REGEXP / RLIKE pattern
        if self.peek() == Some(&Token::Regexp) {
            self.advance();
            let pattern = match self.advance() {
                Some(Token::StringLit(s)) => s.clone(),
                Some(Token::Ident(s))     => s.clone(),
                other => return Err(format!("Expected pattern after REGEXP, got {:?}", other)),
            };
            return Ok(Condition { left, operator: Operator::Regexp, value: ConditionValue::Literal(pattern) });
        }

        // IS NULL / IS NOT NULL
        if self.peek() == Some(&Token::Is) {
            self.advance();
            return match self.peek() {
                Some(Token::Not) => {
                    self.advance();
                    match self.advance() {
                        Some(Token::Null) => Ok(Condition { left, operator: Operator::IsNotNull, value: ConditionValue::Literal(String::new()) }),
                        other => Err(format!("Expected NULL after IS NOT, got {:?}", other)),
                    }
                }
                Some(Token::Null) => {
                    self.advance();
                    Ok(Condition { left, operator: Operator::IsNull, value: ConditionValue::Literal(String::new()) })
                }
                other => Err(format!("Expected NULL or NOT after IS, got {:?}", other)),
            };
        }

        let operator = match self.advance() {
            Some(Token::Eq)  => Operator::Eq,
            Some(Token::Ne)  => Operator::Ne,
            Some(Token::Gt)  => Operator::Gt,
            Some(Token::Lt)  => Operator::Lt,
            Some(Token::Gte) => Operator::Gte,
            Some(Token::Lte) => Operator::Lte,
            other => return Err(format!("Expected comparison operator, got {:?}", other)),
        };

        let value = match self.peek() {
            Some(Token::LParen) => {
                self.advance();
                match self.advance() {
                    Some(Token::Select) => {}
                    other => return Err(format!("Expected SELECT in subquery, got {:?}", other)),
                }
                let sub_stmt = self.parse_select()?;
                match self.advance() {
                    Some(Token::RParen) => {}
                    other => return Err(format!("Expected ')' after subquery, got {:?}", other)),
                }
                ConditionValue::Subquery(Box::new(sub_stmt))
            }
            _ => match self.advance() {
                Some(Token::Ident(s)) => {
                    let s = s.clone();
                    if self.peek() == Some(&Token::Dot) {
                        self.advance();
                        let col = self.expect_ident()?;
                        ConditionValue::Literal(format!("{}.{}", s, col))
                    } else {
                        ConditionValue::Literal(s)
                    }
                }
                Some(Token::NumberLit(n)) => ConditionValue::Literal(n.clone()),
                Some(Token::StringLit(s)) => ConditionValue::Literal(s.clone()),
                Some(Token::Null)         => ConditionValue::Literal("__NULL__".to_string()),
                other => return Err(format!("Expected value, got {:?}", other)),
            }
        };

        Ok(Condition { left, operator, value })
    }

    /// EXISTS / NOT EXISTS 뒤의 (SELECT ...) 파싱
    fn parse_exists_subquery(&mut self) -> Result<Statement, String> {
        match self.advance() {
            Some(Token::LParen) => {}
            other => return Err(format!("Expected '(' after EXISTS, got {:?}", other)),
        }
        let sub = match self.advance() {
            Some(Token::Select) => self.parse_select()?,
            other => return Err(format!("Expected SELECT inside EXISTS, got {:?}", other)),
        };
        match self.advance() {
            Some(Token::RParen) => {}
            other => return Err(format!("Expected ')' after EXISTS subquery, got {:?}", other)),
        }
        Ok(sub)
    }

    /// Arithmetic factor: number | string | column | agg_func | '(' expr ')'
    fn parse_arith_factor(&mut self) -> Result<ArithExpr, String> {
        match self.peek() {
            // Aggregate functions → stored as Col("COUNT(*)")
            Some(Token::Count) | Some(Token::Sum) | Some(Token::Avg) |
            Some(Token::Min)   | Some(Token::Max) => {
                let label = match self.advance() {
                    Some(Token::Count) => "COUNT",
                    Some(Token::Sum)   => "SUM",
                    Some(Token::Avg)   => "AVG",
                    Some(Token::Min)   => "MIN",
                    Some(Token::Max)   => "MAX",
                    _ => unreachable!(),
                };
                match self.advance() {
                    Some(Token::LParen) => {}
                    other => return Err(format!("Expected '(' after aggregate, got {:?}", other)),
                }
                let inner = match self.advance() {
                    Some(Token::Asterisk) => "*".to_string(),
                    Some(Token::Ident(s)) => s.clone(),
                    other => return Err(format!("Expected column in aggregate, got {:?}", other)),
                };
                match self.advance() {
                    Some(Token::RParen) => {}
                    other => return Err(format!("Expected ')' after aggregate, got {:?}", other)),
                }
                Ok(ArithExpr::Col(format!("{}({})", label, inner)))
            }
            Some(Token::NumberLit(_)) => {
                let n = match self.advance() { Some(Token::NumberLit(n)) => n.clone(), _ => unreachable!() };
                Ok(ArithExpr::Num(n))
            }
            Some(Token::StringLit(_)) => {
                let s = match self.advance() { Some(Token::StringLit(s)) => s.clone(), _ => unreachable!() };
                Ok(ArithExpr::Str(s))
            }
            Some(Token::Null) => {
                self.advance();
                Ok(ArithExpr::Str("NULL".to_string()))
            }
            Some(Token::LParen) => {
                self.advance();
                let inner = self.parse_arith_expr()?;
                match self.advance() {
                    Some(Token::RParen) => {}
                    other => return Err(format!("Expected ')' in expression, got {:?}", other)),
                }
                Ok(inner)
            }
            // Scalar functions usable in arithmetic / UPDATE SET context
            Some(Token::Concat) | Some(Token::Upper) | Some(Token::Lower) |
            Some(Token::Length) | Some(Token::Trim)  | Some(Token::Substr) |
            Some(Token::Substring) | Some(Token::Replace) |
            Some(Token::Round)  | Some(Token::Abs)   | Some(Token::Ceil) |
            Some(Token::Floor)  | Some(Token::Mod)   |
            Some(Token::Coalesce) | Some(Token::Ifnull) | Some(Token::Nullif) |
            Some(Token::Lpad)   | Some(Token::Rpad)  | Some(Token::If) |
            Some(Token::DateAdd) | Some(Token::DateDiff) |
            Some(Token::Left)   | Some(Token::Right)  |
            Some(Token::Truncate) | Some(Token::Repeat) |
            Some(Token::JsonExtract) | Some(Token::JsonUnquote) | Some(Token::JsonValue) => {
                let fname = match self.advance() {
                    Some(Token::Concat)    => "CONCAT",
                    Some(Token::Upper)     => "UPPER",
                    Some(Token::Lower)     => "LOWER",
                    Some(Token::Length)    => "LENGTH",
                    Some(Token::Trim)      => "TRIM",
                    Some(Token::Substr) | Some(Token::Substring) => "SUBSTR",
                    Some(Token::Replace)   => "REPLACE",
                    Some(Token::Round)     => "ROUND",
                    Some(Token::Abs)       => "ABS",
                    Some(Token::Ceil)      => "CEIL",
                    Some(Token::Floor)     => "FLOOR",
                    Some(Token::Mod)       => "MOD",
                    Some(Token::Coalesce)  => "COALESCE",
                    Some(Token::Ifnull)    => "IFNULL",
                    Some(Token::Nullif)    => "NULLIF",
                    Some(Token::Lpad)      => "LPAD",
                    Some(Token::Rpad)      => "RPAD",
                    Some(Token::If)        => "IF",
                    Some(Token::DateAdd)   => "DATE_ADD",
                    Some(Token::DateDiff)  => "DATEDIFF",
                    Some(Token::Left)      => "LEFT",
                    Some(Token::Right)     => "RIGHT",
                    Some(Token::Truncate)  => "TRUNCATE",
                    Some(Token::Repeat)    => "REPEAT",
                    Some(Token::JsonExtract) => "JSON_EXTRACT",
                    Some(Token::JsonUnquote) => "JSON_UNQUOTE",
                    Some(Token::JsonValue)   => "JSON_VALUE",
                    _ => unreachable!(),
                };
                match self.advance() {
                    Some(Token::LParen) => {}
                    other => return Err(format!("Expected '(' after {}, got {:?}", fname, other)),
                }
                let mut args = Vec::new();
                while self.peek() != Some(&Token::RParen) {
                    if !args.is_empty() {
                        match self.advance() {
                            Some(Token::Comma) => {}
                            other => return Err(format!("Expected ',' in {} args, got {:?}", fname, other)),
                        }
                    }
                    if self.peek() == Some(&Token::RParen) { break; }
                    args.push(self.parse_arith_expr()?);
                }
                match self.advance() {
                    Some(Token::RParen) => {}
                    other => return Err(format!("Expected ')' after {} args, got {:?}", fname, other)),
                }
                Ok(ArithExpr::Func(fname.to_string(), args))
            }
            Some(Token::Ident(_)) => {
                // Check for generic function call: IDENT(...)
                if self.tokens.get(self.pos + 1) == Some(&Token::LParen) {
                    let fname = match self.advance() { Some(Token::Ident(s)) => s.clone(), _ => unreachable!() };
                    self.advance(); // consume (
                    let mut args = Vec::new();
                    while self.peek() != Some(&Token::RParen) && self.peek().is_some() {
                        if !args.is_empty() {
                            if self.peek() == Some(&Token::Comma) { self.advance(); } else { break; }
                        }
                        if self.peek() == Some(&Token::RParen) { break; }
                        args.push(self.parse_arith_expr()?);
                    }
                    match self.advance() {
                        Some(Token::RParen) => {}
                        other => return Err(format!("Expected ')' after {} args, got {:?}", fname, other)),
                    }
                    Ok(ArithExpr::Func(fname.to_string(), args))
                } else {
                    let s = self.expect_col_ref()?;
                    Ok(ArithExpr::Col(s))
                }
            }
            // YEAR: function call if followed by '(', else unit string literal
            Some(Token::Year) => {
                if self.tokens.get(self.pos + 1) == Some(&Token::LParen) {
                    self.advance(); // consume YEAR
                    self.advance(); // consume (
                    let mut args = Vec::new();
                    while self.peek() != Some(&Token::RParen) && self.peek().is_some() {
                        if !args.is_empty() {
                            if self.peek() == Some(&Token::Comma) { self.advance(); } else { break; }
                        }
                        if self.peek() == Some(&Token::RParen) { break; }
                        args.push(self.parse_arith_expr()?);
                    }
                    match self.advance() {
                        Some(Token::RParen) => {}
                        other => return Err(format!("Expected ')' after YEAR args, got {:?}", other)),
                    }
                    Ok(ArithExpr::Func("YEAR".to_string(), args))
                } else {
                    self.advance();
                    Ok(ArithExpr::Str("YEAR".to_string()))
                }
            }
            // DATE_SUB in expression context: parse INTERVAL-aware args
            Some(Token::DateSub) => {
                self.advance(); // consume DATE_SUB
                let str_args = self.parse_date_add_args()?;
                let arith_args: Vec<ArithExpr> = str_args.into_iter().map(|s| {
                    if s.starts_with('\'') && s.ends_with('\'') {
                        ArithExpr::Str(s[1..s.len()-1].to_string())
                    } else if s.parse::<f64>().is_ok() {
                        ArithExpr::Num(s)
                    } else {
                        ArithExpr::Col(s)
                    }
                }).collect();
                Ok(ArithExpr::Func("DATE_SUB".to_string(), arith_args))
            }
            Some(Token::At) => {
                self.advance();
                let name = self.expect_ident()?;
                Ok(ArithExpr::Col(format!("@{}", name)))
            }
            other => Err(format!("Expected expression term, got {:?}", other)),
        }
    }

    /// Arithmetic term: factor ('*' | '/' factor)*
    fn parse_arith_term(&mut self) -> Result<ArithExpr, String> {
        let mut left = self.parse_arith_factor()?;
        loop {
            match self.peek() {
                Some(Token::Asterisk) => {
                    self.advance();
                    let right = self.parse_arith_factor()?;
                    left = ArithExpr::Mul(Box::new(left), Box::new(right));
                }
                Some(Token::Slash) => {
                    self.advance();
                    let right = self.parse_arith_factor()?;
                    left = ArithExpr::Div(Box::new(left), Box::new(right));
                }
                _ => break,
            }
        }
        Ok(left)
    }

    /// Arithmetic expression: term (('+' | '-') term)*
    pub(crate) fn parse_arith_expr(&mut self) -> Result<ArithExpr, String> {
        let mut left = self.parse_arith_term()?;
        loop {
            match self.peek() {
                Some(Token::Plus) => {
                    self.advance();
                    let right = self.parse_arith_term()?;
                    left = ArithExpr::Add(Box::new(left), Box::new(right));
                }
                Some(Token::Minus) => {
                    self.advance();
                    let right = self.parse_arith_term()?;
                    left = ArithExpr::Sub(Box::new(left), Box::new(right));
                }
                // col->'$.key'  →  JSON_EXTRACT(col, '$.key')
                Some(Token::Arrow) => {
                    self.advance();
                    let path = match self.advance() {
                        Some(Token::StringLit(s)) => ArithExpr::Str(s.clone()),
                        other => return Err(format!("Expected path string after ->, got {:?}", other)),
                    };
                    left = ArithExpr::Func("JSON_EXTRACT".to_string(), vec![left, path]);
                }
                // col->>'$.key'  →  JSON_UNQUOTE(JSON_EXTRACT(col, '$.key'))
                Some(Token::LongArrow) => {
                    self.advance();
                    let path = match self.advance() {
                        Some(Token::StringLit(s)) => ArithExpr::Str(s.clone()),
                        other => return Err(format!("Expected path string after ->>, got {:?}", other)),
                    };
                    let extract = ArithExpr::Func("JSON_EXTRACT".to_string(), vec![left, path]);
                    left = ArithExpr::Func("JSON_UNQUOTE".to_string(), vec![extract]);
                }
                _ => break,
            }
        }
        Ok(left)
    }

    /// CASE WHEN cond THEN val ... [ELSE val] END
    pub(crate) fn arith_to_string(expr: &ArithExpr) -> String {
        match expr {
            ArithExpr::Col(s) | ArithExpr::Num(s) => s.clone(),
            ArithExpr::Str(s) => format!("'{}'", s),
            ArithExpr::Add(l, r) => format!("{} + {}", Self::arith_to_string(l), Self::arith_to_string(r)),
            ArithExpr::Sub(l, r) => format!("{} - {}", Self::arith_to_string(l), Self::arith_to_string(r)),
            ArithExpr::Mul(l, r) => format!("{} * {}", Self::arith_to_string(l), Self::arith_to_string(r)),
            ArithExpr::Div(l, r) => format!("{} / {}", Self::arith_to_string(l), Self::arith_to_string(r)),
            ArithExpr::Func(name, args) => {
                let arg_strs: Vec<String> = args.iter().map(|a| Self::arith_to_string(a)).collect();
                format!("{}({})", name, arg_strs.join(", "))
            }
            ArithExpr::Cmp(l, op, r) => format!("{} {} {}", Self::arith_to_string(l), op, Self::arith_to_string(r)),
        }
    }

    fn str_to_arith(s: &str) -> ArithExpr {
        Parser::new(s).parse_arith_expr().unwrap_or_else(|_| ArithExpr::Col(s.to_string()))
    }

    fn parse_window_frame(&mut self) -> Result<Option<crate::parser::ast::WindowFrame>, String> {
        use crate::parser::ast::{WindowFrame, FrameUnit, FrameBound};
        let unit = match self.peek() {
            Some(Token::Rows)  => { self.advance(); FrameUnit::Rows }
            Some(Token::Range) => { self.advance(); FrameUnit::Range }
            _ => return Ok(None),
        };
        match self.advance() {
            Some(Token::Between) => {}
            other => return Err(format!("Expected BETWEEN after ROWS/RANGE, got {:?}", other)),
        }
        let parse_bound = |p: &mut Parser| -> Result<FrameBound, String> {
            match p.peek() {
                Some(Token::Unbounded) => {
                    p.advance();
                    match p.advance() {
                        Some(Token::Preceding) => Ok(FrameBound::UnboundedPreceding),
                        Some(Token::Following) => Ok(FrameBound::UnboundedFollowing),
                        other => Err(format!("Expected PRECEDING/FOLLOWING after UNBOUNDED, got {:?}", other)),
                    }
                }
                Some(Token::Current) => {
                    p.advance();
                    match p.advance() {
                        Some(Token::Row) => Ok(FrameBound::CurrentRow),
                        Some(Token::Ident(s)) if s.to_uppercase() == "ROW" => Ok(FrameBound::CurrentRow),
                        other => Err(format!("Expected ROW after CURRENT, got {:?}", other)),
                    }
                }
                Some(Token::NumberLit(_)) => {
                    let n: usize = if let Some(Token::NumberLit(s)) = p.advance() {
                        s.parse().unwrap_or(0)
                    } else { 0 };
                    match p.advance() {
                        Some(Token::Preceding) => Ok(FrameBound::Preceding(n)),
                        Some(Token::Following) => Ok(FrameBound::Following(n)),
                        other => Err(format!("Expected PRECEDING/FOLLOWING after N, got {:?}", other)),
                    }
                }
                other => Err(format!("Expected frame bound, got {:?}", other)),
            }
        };
        let start = parse_bound(self)?;
        match self.advance() {
            Some(Token::And) => {}
            other => return Err(format!("Expected AND in frame, got {:?}", other)),
        }
        let end = parse_bound(self)?;
        Ok(Some(WindowFrame { unit, start, end }))
    }

    fn parse_case_when(&mut self) -> Result<SelectColumn, String> {
        let mut branches = Vec::new();
        loop {
            match self.peek() {
                Some(Token::When) => {
                    self.advance(); // WHEN
                    let cond = self.parse_condexpr()?;
                    match self.advance() {
                        Some(Token::Then) => {}
                        other => return Err(format!("Expected THEN, got {:?}", other)),
                    }
                    let result = match self.advance() {
                        Some(Token::StringLit(s)) => s.clone(),
                        Some(Token::NumberLit(n)) => n.clone(),
                        Some(Token::Null)         => "NULL".to_string(),
                        Some(Token::Ident(s))     => s.clone(),
                        other => return Err(format!("Expected THEN value, got {:?}", other)),
                    };
                    branches.push(CaseWhenBranch { condition: cond, result });
                }
                _ => break,
            }
        }
        let else_val = if self.peek() == Some(&Token::Else) {
            self.advance();
            Some(match self.advance() {
                Some(Token::StringLit(s)) => s.clone(),
                Some(Token::NumberLit(n)) => n.clone(),
                Some(Token::Null)         => "NULL".to_string(),
                Some(Token::Ident(s))     => s.clone(),
                other => return Err(format!("Expected ELSE value, got {:?}", other)),
            })
        } else {
            None
        };
        match self.advance() {
            Some(Token::End) => {}
            other => return Err(format!("Expected END after CASE, got {:?}", other)),
        }
        let alias = if self.peek() == Some(&Token::As) {
            self.advance();
            Some(self.expect_alias_ident()?)
        } else {
            None
        };
        Ok(SelectColumn::CaseWhen { branches, else_val, alias })
    }

    fn parse_select(&mut self) -> Result<Statement, String> {
        // DISTINCT
        let distinct = if self.peek() == Some(&Token::Distinct) {
            self.advance();
            true
        } else {
            false
        };

        // 컬럼 목록 (AS 별칭 포함)
        let mut columns = Vec::new();
        loop {
            let col = match self.peek() {
                Some(Token::Asterisk) => { self.advance(); SelectColumn::All }
                Some(Token::Count) | Some(Token::Sum) | Some(Token::Avg) |
                Some(Token::Min)   | Some(Token::Max) |
                Some(Token::Stddev) | Some(Token::Variance) => {
                    let mut func = match self.advance() {
                        Some(Token::Count)    => AggFunc::Count,
                        Some(Token::Sum)      => AggFunc::Sum,
                        Some(Token::Avg)      => AggFunc::Avg,
                        Some(Token::Min)      => AggFunc::Min,
                        Some(Token::Max)      => AggFunc::Max,
                        Some(Token::Stddev)   => AggFunc::Stddev,
                        Some(Token::Variance) => AggFunc::Variance,
                        _ => unreachable!(),
                    };
                    match self.advance() {
                        Some(Token::LParen) => {}
                        other => return Err(format!("Expected '(', got {:?}", other)),
                    }
                    if self.peek() == Some(&Token::Distinct) {
                        self.advance();
                        func = match func {
                            AggFunc::Count => AggFunc::CountDistinct,
                            AggFunc::Sum   => AggFunc::SumDistinct,
                            AggFunc::Avg   => AggFunc::AvgDistinct,
                            other          => other,
                        };
                    }
                    let agg_col = match self.advance() {
                        Some(Token::Asterisk)  => "*".to_string(),
                        Some(Token::Ident(s))  => {
                            let first = s.clone();
                            if self.peek() == Some(&Token::Dot) {
                                self.advance();
                                self.expect_ident()?
                            } else {
                                first
                            }
                        }
                        other => return Err(format!("Expected column, got {:?}", other)),
                    };
                    match self.advance() {
                        Some(Token::RParen) => {}
                        other => return Err(format!("Expected ')', got {:?}", other)),
                    }
                    // 집계함수 + OVER → aggregate window function
                    if self.peek() == Some(&Token::Over) {
                        self.advance(); // consume OVER
                        match self.advance() {
                            Some(Token::LParen) => {}
                            other => return Err(format!("Expected '(' after OVER, got {:?}", other)),
                        }
                        let partition_by = if self.peek() == Some(&Token::Partition) {
                            self.advance();
                            match self.advance() {
                                Some(Token::By) => {}
                                other => return Err(format!("Expected BY after PARTITION, got {:?}", other)),
                            }
                            let mut cols = vec![self.expect_col_ref()?];
                            while self.peek() == Some(&Token::Comma) {
                                self.advance();
                                cols.push(self.expect_col_ref()?);
                            }
                            cols
                        } else { vec![] };
                        let win_order_by = if self.peek() == Some(&Token::Order) {
                            self.advance();
                            match self.advance() {
                                Some(Token::By) => {}
                                other => return Err(format!("Expected BY after ORDER, got {:?}", other)),
                            }
                            let mut keys = Vec::new();
                            loop {
                                let col = self.expect_col_ref()?;
                                let ascending = match self.peek() {
                                    Some(Token::Desc) => { self.advance(); false }
                                    Some(Token::Asc)  => { self.advance(); true }
                                    _ => true,
                                };
                                keys.push(OrderBy { column: col, ascending });
                                if self.peek() == Some(&Token::Comma) { self.advance(); } else { break; }
                            }
                            keys
                        } else { vec![] };
                        let frame = self.parse_window_frame()?;
                        match self.advance() {
                            Some(Token::RParen) => {}
                            other => return Err(format!("Expected ')' after OVER clause, got {:?}", other)),
                        }
                        let win_func = match func {
                            AggFunc::Sum | AggFunc::SumDistinct => WindowFunc::Sum,
                            AggFunc::Avg | AggFunc::AvgDistinct => WindowFunc::Avg,
                            AggFunc::Count | AggFunc::CountDistinct => WindowFunc::Count,
                            AggFunc::Min => WindowFunc::Min,
                            AggFunc::Max => WindowFunc::Max,
                            _ => WindowFunc::Sum,
                        };
                        let alias = if self.peek() == Some(&Token::As) {
                            self.advance();
                            Some(self.expect_alias_ident()?)
                        } else { None };
                        SelectColumn::WinFunc {
                            func: win_func,
                            col: Some(agg_col),
                            offset: 0,
                            partition_by,
                            order_by: win_order_by,
                            alias,
                            frame,
                        }
                    } else {
                        // AS 별칭
                        if self.peek() == Some(&Token::As) {
                            self.advance();
                            let alias = self.expect_alias_ident()?;
                            SelectColumn::AggAlias { func, col: agg_col, alias }
                        } else {
                            SelectColumn::Agg { func, col: agg_col }
                        }
                    }
                }
                // GROUP_CONCAT(col [SEPARATOR 'sep'])
                Some(Token::GroupConcat) => {
                    self.advance();
                    match self.advance() {
                        Some(Token::LParen) => {}
                        other => return Err(format!("Expected '(' after GROUP_CONCAT, got {:?}", other)),
                    }
                    let agg_col = match self.advance() {
                        Some(Token::Ident(s)) => {
                            let first = s.clone();
                            if self.peek() == Some(&Token::Dot) {
                                self.advance();
                                self.expect_ident()?
                            } else { first }
                        }
                        other => return Err(format!("Expected column in GROUP_CONCAT, got {:?}", other)),
                    };
                    // Optional SEPARATOR 'sep'
                    let separator = if self.peek() == Some(&Token::Separator) {
                        self.advance();
                        match self.advance() {
                            Some(Token::StringLit(s)) => s.clone(),
                            other => return Err(format!("Expected string after SEPARATOR, got {:?}", other)),
                        }
                    } else {
                        ",".to_string()
                    };
                    match self.advance() {
                        Some(Token::RParen) => {}
                        other => return Err(format!("Expected ')' after GROUP_CONCAT, got {:?}", other)),
                    }
                    let func = AggFunc::GroupConcat { separator };
                    if self.peek() == Some(&Token::As) {
                        self.advance();
                        let alias = self.expect_alias_ident()?;
                        SelectColumn::AggAlias { func, col: agg_col, alias }
                    } else {
                        SelectColumn::Agg { func, col: agg_col }
                    }
                }
                // CASE WHEN ... THEN ... [ELSE ...] END
                Some(Token::Case) => {
                    self.advance(); // consume CASE
                    self.parse_case_when()?
                }
                // IF(cond, true_val, false_val) — parsed as CaseWhen to support any condition
                Some(Token::If) => {
                    self.advance(); // consume IF
                    match self.advance() {
                        Some(Token::LParen) => {}
                        other => return Err(format!("Expected '(' after IF, got {:?}", other)),
                    }
                    let cond = self.parse_condexpr()?;
                    match self.advance() {
                        Some(Token::Comma) => {}
                        other => return Err(format!("Expected ',' in IF(), got {:?}", other)),
                    }
                    let true_val = match self.advance() {
                        Some(Token::StringLit(s)) => s.clone(),
                        Some(Token::NumberLit(n)) => n.clone(),
                        Some(Token::Null)         => "NULL".to_string(),
                        Some(Token::Ident(s))     => s.clone(),
                        other => return Err(format!("Expected true value in IF(), got {:?}", other)),
                    };
                    match self.advance() {
                        Some(Token::Comma) => {}
                        other => return Err(format!("Expected ',' in IF(), got {:?}", other)),
                    }
                    let false_val = match self.advance() {
                        Some(Token::StringLit(s)) => s.clone(),
                        Some(Token::NumberLit(n)) => n.clone(),
                        Some(Token::Null)         => "NULL".to_string(),
                        Some(Token::Ident(s))     => s.clone(),
                        other => return Err(format!("Expected false value in IF(), got {:?}", other)),
                    };
                    match self.advance() {
                        Some(Token::RParen) => {}
                        other => return Err(format!("Expected ')' after IF(), got {:?}", other)),
                    }
                    let alias = if self.peek() == Some(&Token::As) {
                        self.advance();
                        Some(self.expect_alias_ident()?)
                    } else { None };
                    SelectColumn::CaseWhen {
                        branches: vec![CaseWhenBranch { condition: cond, result: true_val }],
                        else_val: Some(false_val),
                        alias,
                    }
                }
                // CAST(expr AS type) — 특수 문법
                Some(Token::Cast) => {
                    self.advance();
                    let args = self.parse_cast_args()?;
                    let alias = if self.peek() == Some(&Token::As) {
                        self.advance();
                        Some(self.expect_alias_ident()?)
                    } else { None };
                    SelectColumn::Func { name: "CAST".to_string(), args, alias }
                }
                // DATE_ADD / DATE_SUB (date, INTERVAL n unit) — 특수 문법
                Some(Token::DateAdd) | Some(Token::DateSub) => {
                    let fname = if self.peek() == Some(&Token::DateAdd) { "DATE_ADD" } else { "DATE_SUB" };
                    self.advance();
                    let args = self.parse_date_add_args()?;
                    let alias = if self.peek() == Some(&Token::As) {
                        self.advance();
                        Some(self.expect_alias_ident()?)
                    } else { None };
                    SelectColumn::Func { name: fname.to_string(), args, alias }
                }
                // 윈도우 함수
                Some(Token::RowNumber) | Some(Token::Rank) | Some(Token::DenseRank) |
                Some(Token::Lag) | Some(Token::Lead) |
                Some(Token::FirstValue) | Some(Token::LastValue) | Some(Token::NthValue) |
                Some(Token::Ntile) | Some(Token::PercentRank) | Some(Token::CumeDist) => {
                    let func = match self.advance() {
                        Some(Token::RowNumber)   => WindowFunc::RowNumber,
                        Some(Token::Rank)        => WindowFunc::Rank,
                        Some(Token::DenseRank)   => WindowFunc::DenseRank,
                        Some(Token::Lag)         => WindowFunc::Lag,
                        Some(Token::Lead)        => WindowFunc::Lead,
                        Some(Token::FirstValue)  => WindowFunc::FirstValue,
                        Some(Token::LastValue)   => WindowFunc::LastValue,
                        Some(Token::NthValue)    => WindowFunc::NthValue,
                        Some(Token::Ntile)       => WindowFunc::Ntile,
                        Some(Token::PercentRank) => WindowFunc::PercentRank,
                        Some(Token::CumeDist)    => WindowFunc::CumeDist,
                        _ => unreachable!(),
                    };
                    match self.advance() {
                        Some(Token::LParen) => {}
                        other => return Err(format!("Expected '(' after window function, got {:?}", other)),
                    }
                    let (wf_col, wf_offset) = match func {
                        WindowFunc::Lag | WindowFunc::Lead => {
                            let col = self.expect_col_ref()?;
                            let off = if self.peek() == Some(&Token::Comma) {
                                self.advance();
                                match self.advance() {
                                    Some(Token::NumberLit(n)) => n.parse::<i64>().unwrap_or(1),
                                    other => return Err(format!("Expected offset number in LAG/LEAD, got {:?}", other)),
                                }
                            } else { 1 };
                            (Some(col), off)
                        }
                        WindowFunc::FirstValue | WindowFunc::LastValue => {
                            let col = self.expect_col_ref()?;
                            (Some(col), 0i64)
                        }
                        WindowFunc::NthValue => {
                            let col = self.expect_col_ref()?;
                            match self.advance() {
                                Some(Token::Comma) => {}
                                other => return Err(format!("Expected ',' in NTH_VALUE, got {:?}", other)),
                            }
                            let n = match self.advance() {
                                Some(Token::NumberLit(n)) => n.parse::<i64>().unwrap_or(1),
                                other => return Err(format!("Expected N in NTH_VALUE, got {:?}", other)),
                            };
                            (Some(col), n)
                        }
                        WindowFunc::Ntile => {
                            let n = match self.advance() {
                                Some(Token::NumberLit(n)) => n.parse::<i64>().unwrap_or(1),
                                other => return Err(format!("Expected N in NTILE, got {:?}", other)),
                            };
                            (None, n)
                        }
                        _ => (None, 0i64),
                    };
                    match self.advance() {
                        Some(Token::RParen) => {}
                        other => return Err(format!("Expected ')' after window function args, got {:?}", other)),
                    }
                    match self.advance() {
                        Some(Token::Over) => {}
                        other => return Err(format!("Expected OVER, got {:?}", other)),
                    }
                    match self.advance() {
                        Some(Token::LParen) => {}
                        other => return Err(format!("Expected '(' after OVER, got {:?}", other)),
                    }
                    let partition_by = if self.peek() == Some(&Token::Partition) {
                        self.advance();
                        match self.advance() {
                            Some(Token::By) => {}
                            other => return Err(format!("Expected BY after PARTITION, got {:?}", other)),
                        }
                        let mut cols = vec![self.expect_col_ref()?];
                        while self.peek() == Some(&Token::Comma) {
                            self.advance();
                            cols.push(self.expect_col_ref()?);
                        }
                        cols
                    } else { vec![] };
                    let win_order_by = if self.peek() == Some(&Token::Order) {
                        self.advance();
                        match self.advance() {
                            Some(Token::By) => {}
                            other => return Err(format!("Expected BY after ORDER, got {:?}", other)),
                        }
                        let mut keys = Vec::new();
                        loop {
                            let col = self.expect_col_ref()?;
                            let ascending = match self.peek() {
                                Some(Token::Desc) => { self.advance(); false }
                                Some(Token::Asc)  => { self.advance(); true }
                                _ => true,
                            };
                            keys.push(OrderBy { column: col, ascending });
                            if self.peek() == Some(&Token::Comma) { self.advance(); } else { break; }
                        }
                        keys
                    } else { vec![] };
                    let frame = self.parse_window_frame()?;
                    match self.advance() {
                        Some(Token::RParen) => {}
                        other => return Err(format!("Expected ')' after OVER clause, got {:?}", other)),
                    }
                    let alias = if self.peek() == Some(&Token::As) {
                        self.advance();
                        Some(self.expect_alias_ident()?)
                    } else { None };
                    SelectColumn::WinFunc {
                        func,
                        col: wf_col,
                        offset: wf_offset,
                        partition_by,
                        order_by: win_order_by,
                        alias,
                        frame,
                    }
                }
                // 스칼라 함수: UPPER(col), NOW(), CONCAT(a, b), ...
                Some(Token::Upper) | Some(Token::Lower) | Some(Token::Length) |
                Some(Token::Trim)  | Some(Token::Concat) | Some(Token::Substr) |
                Some(Token::Substring) | Some(Token::Now) | Some(Token::Curdate) |
                Some(Token::DateFormat) | Some(Token::Coalesce) | Some(Token::Ifnull) |
                Some(Token::Replace) |
                Some(Token::Round) | Some(Token::Abs) | Some(Token::Ceil) |
                Some(Token::Floor) | Some(Token::Mod) |
                Some(Token::Nullif) | Some(Token::Lpad) | Some(Token::Rpad) |
                Some(Token::DateDiff) => {
                    let fname = match self.advance() {
                        Some(Token::Upper)      => "UPPER",
                        Some(Token::Lower)      => "LOWER",
                        Some(Token::Length)     => "LENGTH",
                        Some(Token::Trim)       => "TRIM",
                        Some(Token::Concat)     => "CONCAT",
                        Some(Token::Substr)     => "SUBSTR",
                        Some(Token::Substring)  => "SUBSTRING",
                        Some(Token::Now)        => "NOW",
                        Some(Token::Curdate)    => "CURDATE",
                        Some(Token::DateFormat) => "DATE_FORMAT",
                        Some(Token::Coalesce)   => "COALESCE",
                        Some(Token::Ifnull)     => "IFNULL",
                        Some(Token::Replace)    => "REPLACE",
                        Some(Token::Round)      => "ROUND",
                        Some(Token::Abs)        => "ABS",
                        Some(Token::Ceil)       => "CEIL",
                        Some(Token::Floor)      => "FLOOR",
                        Some(Token::Mod)        => "MOD",
                        Some(Token::Nullif)     => "NULLIF",
                        Some(Token::Lpad)       => "LPAD",
                        Some(Token::Rpad)       => "RPAD",
                        Some(Token::DateDiff)   => "DATEDIFF",
                        _ => unreachable!(),
                    }.to_string();
                    let args = self.parse_func_args()?;
                    // detect comparison after scalar func: LENGTH(x) > 0 AS alias
                    let cmp_op = match self.peek() {
                        Some(Token::Gt)  => { self.advance(); Some(">") }
                        Some(Token::Lt)  => { self.advance(); Some("<") }
                        Some(Token::Gte) => { self.advance(); Some(">=") }
                        Some(Token::Lte) => { self.advance(); Some("<=") }
                        Some(Token::Eq)  => { self.advance(); Some("=") }
                        Some(Token::Ne)  => { self.advance(); Some("!=") }
                        _ => None,
                    };
                    if let Some(op) = cmp_op {
                        let rhs = self.parse_arith_expr()?;
                        let alias = if self.peek() == Some(&Token::As) {
                            self.advance();
                            Some(self.expect_alias_ident()?)
                        } else { None };
                        let lhs = ArithExpr::Func(fname, args.iter().map(|s| Self::str_to_arith(s)).collect());
                        SelectColumn::Expr { expr: ArithExpr::Cmp(Box::new(lhs), op.to_string(), Box::new(rhs)), alias }
                    } else {
                        let alias = if self.peek() == Some(&Token::As) {
                            self.advance();
                            Some(self.expect_alias_ident()?)
                        } else { None };
                        SelectColumn::Func { name: fname, args, alias }
                    }
                }
                // 스칼라 서브쿼리: (SELECT ...) [AS alias]
                Some(Token::LParen) if self.tokens.get(self.pos + 1) == Some(&Token::Select) => {
                    self.advance(); // consume (
                    self.advance(); // consume SELECT
                    let inner = self.parse_select()?;
                    match self.advance() {
                        Some(Token::RParen) => {}
                        other => return Err(format!("Expected ')' after scalar subquery, got {:?}", other)),
                    }
                    let alias = if self.peek() == Some(&Token::As) {
                        self.advance();
                        Some(self.expect_alias_ident()?)
                    } else { None };
                    SelectColumn::Subquery { query: Box::new(inner), alias }
                }
                _ => {
                    let expr = self.parse_arith_expr()?;
                    let cmp_op = match self.peek() {
                        Some(Token::Gt)  => { self.advance(); Some(">") }
                        Some(Token::Lt)  => { self.advance(); Some("<") }
                        Some(Token::Gte) => { self.advance(); Some(">=") }
                        Some(Token::Lte) => { self.advance(); Some("<=") }
                        Some(Token::Eq)  => { self.advance(); Some("=") }
                        Some(Token::Ne)  => { self.advance(); Some("!=") }
                        _ => None,
                    };
                    if let Some(op) = cmp_op {
                        let rhs = self.parse_arith_expr()?;
                        let alias = if self.peek() == Some(&Token::As) {
                            self.advance();
                            Some(self.expect_alias_ident()?)
                        } else { None };
                        SelectColumn::Expr { expr: ArithExpr::Cmp(Box::new(expr), op.to_string(), Box::new(rhs)), alias }
                    } else {
                        let alias = if self.peek() == Some(&Token::As) {
                            self.advance();
                            Some(self.expect_alias_ident()?)
                        } else {
                            None
                        };
                        match expr {
                            ArithExpr::Col(name) => {
                                if let Some(a) = alias {
                                    SelectColumn::ColumnAlias(name, a)
                                } else {
                                    SelectColumn::Column(name)
                                }
                            }
                            other => SelectColumn::Expr { expr: other, alias },
                        }
                    }
                }
            };
            columns.push(col);
            if self.peek() == Some(&Token::Comma) {
                self.advance();
            } else {
                break;
            }
        }

        // FROM is optional: scalar SELECT (no FROM) is supported
        if self.peek() != Some(&Token::From) {
            return Ok(Statement::Select {
                table: "_dual_".to_string(),
                subquery: None,
                columns,
                distinct,
                condition: None,
                joins: vec![],
                order_by: vec![],
                group_by: None,
                having: None,
                limit: None,
                offset: None,
                for_update: false,
                for_share: false,
            });
        }
        self.advance(); // consume FROM

        // FROM (SELECT ...) AS alias  OR  FROM table_name [alias]
        let mut alias_map: HashMap<String, String> = HashMap::new();

        let (table, subquery) = if self.peek() == Some(&Token::LParen) {
            self.advance();
            match self.advance() {
                Some(Token::Select) => {}
                other => return Err(format!("Expected SELECT in subquery, got {:?}", other)),
            }
            let inner = self.parse_select()?;
            match self.advance() {
                Some(Token::RParen) => {}
                other => return Err(format!("Expected ')' after subquery, got {:?}", other)),
            }
            // AS alias — AS는 선택적
            if self.peek() == Some(&Token::As) { self.advance(); }
            let alias = self.expect_ident()?;
            (String::new(), Some((Box::new(inner), alias)))
        } else {
            let t = self.expect_col_ref()?;
            // 선택적 테이블 별칭: FROM employees e
            if matches!(self.peek(), Some(Token::Ident(_))) {
                let a = self.expect_ident()?;
                alias_map.insert(a, t.clone());
            }
            (t, None)
        };

        // JOIN / LEFT JOIN / RIGHT JOIN / CROSS JOIN / NATURAL JOIN (다중 반복)
        let mut joins = Vec::new();
        loop {
            let join_type = match self.peek() {
                Some(Token::Join)  => { self.advance(); JoinType::Inner }
                Some(Token::Inner) => {
                    self.advance();
                    match self.advance() {
                        Some(Token::Join) => {}
                        other => return Err(format!("Expected JOIN after INNER, got {:?}", other)),
                    }
                    JoinType::Inner
                }
                Some(Token::Left)  => {
                    self.advance();
                    if self.peek() == Some(&Token::Outer) { self.advance(); }
                    match self.advance() {
                        Some(Token::Join) => {}
                        other => return Err(format!("Expected JOIN after LEFT, got {:?}", other)),
                    }
                    JoinType::Left
                }
                Some(Token::Right) => {
                    self.advance();
                    if self.peek() == Some(&Token::Outer) { self.advance(); }
                    match self.advance() {
                        Some(Token::Join) => {}
                        other => return Err(format!("Expected JOIN after RIGHT, got {:?}", other)),
                    }
                    JoinType::Right
                }
                Some(Token::Cross) => {
                    self.advance();
                    match self.advance() {
                        Some(Token::Join) => {}
                        other => return Err(format!("Expected JOIN after CROSS, got {:?}", other)),
                    }
                    JoinType::Cross
                }
                Some(Token::Natural) => {
                    self.advance();
                    match self.advance() {
                        Some(Token::Join) => {}
                        other => return Err(format!("Expected JOIN after NATURAL, got {:?}", other)),
                    }
                    JoinType::Natural
                }
                Some(Token::Full) => {
                    self.advance();
                    if self.peek() == Some(&Token::Outer) { self.advance(); }
                    match self.advance() {
                        Some(Token::Join) => {}
                        other => return Err(format!("Expected JOIN after FULL, got {:?}", other)),
                    }
                    JoinType::FullOuter
                }
                _ => break,
            };
            let join_table = self.expect_ident()?;
            // 선택적 JOIN 테이블 별칭: JOIN departments d ON ...
            if matches!(self.peek(), Some(Token::Ident(_))) {
                let a = self.expect_ident()?;
                alias_map.insert(a, join_table.clone());
            }
            let on_expr = if matches!(join_type, JoinType::Cross | JoinType::Natural) {
                // No ON clause — dummy always-true condition
                CondExpr::Leaf(Condition {
                    left: ArithExpr::Num("1".to_string()),
                    operator: Operator::Eq,
                    value: ConditionValue::Literal("1".to_string()),
                })
            } else {
                match self.advance() {
                    Some(Token::On) => {}
                    other => return Err(format!("Expected ON, got {:?}", other)),
                }
                self.parse_condexpr()?
            };
            joins.push(Join { table: join_table, on_expr, join_type });
        }

        // WHERE
        let condition = if self.peek() == Some(&Token::Where) {
            self.advance();
            Some(self.parse_condexpr()?)
        } else {
            None
        };

        // GROUP BY
        let group_by = if self.peek() == Some(&Token::Group) {
            self.advance();
            match self.advance() {
                Some(Token::By) => {}
                other => return Err(format!("Expected BY, got {:?}", other)),
            }
            let mut cols = vec![self.expect_col_ref()?];
            while self.peek() == Some(&Token::Comma) {
                self.advance();
                cols.push(self.expect_col_ref()?);
            }
            Some(cols)
        } else {
            None
        };

        // HAVING
        let having = if self.peek() == Some(&Token::Having) {
            self.advance();
            Some(self.parse_condexpr()?)
        } else {
            None
        };

        // ORDER BY col1 [ASC|DESC], col2 [ASC|DESC], ...
        let order_by = if self.peek() == Some(&Token::Order) {
            self.advance();
            match self.advance() {
                Some(Token::By) => {}
                other => return Err(format!("Expected BY, got {:?}", other)),
            }
            let mut keys = Vec::new();
            loop {
                let col = self.expect_col_ref()?;
                let ascending = match self.peek() {
                    Some(Token::Desc) => { self.advance(); false }
                    Some(Token::Asc)  => { self.advance(); true  }
                    _ => true,
                };
                keys.push(OrderBy { column: col, ascending });
                if self.peek() == Some(&Token::Comma) {
                    self.advance();
                } else {
                    break;
                }
            }
            keys
        } else {
            Vec::new()
        };

        // LIMIT [OFFSET]
        let (limit, offset) = if self.peek() == Some(&Token::Limit) {
            self.advance();
            let lim = match self.advance() {
                Some(Token::NumberLit(n)) => Some(n.parse::<usize>().unwrap_or(0)),
                other => return Err(format!("Expected number after LIMIT, got {:?}", other)),
            };
            let off = if self.peek() == Some(&Token::Offset) {
                self.advance();
                match self.advance() {
                    Some(Token::NumberLit(n)) => Some(n.parse::<usize>().unwrap_or(0)),
                    other => return Err(format!("Expected number after OFFSET, got {:?}", other)),
                }
            } else {
                None
            };
            (lim, off)
        } else {
            (None, None)
        };

        // FOR UPDATE / FOR SHARE
        let (for_update, for_share) = if self.peek() == Some(&Token::For) {
            self.advance();
            match self.advance() {
                Some(Token::Update) => (true, false),
                Some(Token::Share)  => (false, true),
                other => return Err(format!("Expected UPDATE or SHARE after FOR, got {:?}", other)),
            }
        } else {
            (false, false)
        };

        // 별칭 확장 적용
        let columns: Vec<SelectColumn> = columns.into_iter()
            .map(|c| expand_select_column(c, &alias_map))
            .collect();
        let joins: Vec<Join> = joins.into_iter().map(|j| Join {
            table: j.table,
            on_expr: expand_condexpr(j.on_expr, &alias_map),
            join_type: j.join_type,
        }).collect();
        let condition = condition.map(|c| expand_condexpr(c, &alias_map));
        let order_by: Vec<OrderBy> = order_by.into_iter().map(|o| OrderBy {
            column: expand_alias_str(&o.column, &alias_map),
            ascending: o.ascending,
        }).collect();
        let group_by = group_by.map(|cols| cols.into_iter()
            .map(|c| expand_alias_str(&c, &alias_map))
            .collect::<Vec<_>>());
        let having = having.map(|c| expand_condexpr(c, &alias_map));

        let select_stmt = Statement::Select { table, subquery, distinct, columns, condition, joins, order_by, group_by, having, limit, offset, for_update, for_share };

        // UNION / INTERSECT / EXCEPT [ALL]
        let set_op = match self.peek() {
            Some(Token::Union)     => { self.advance(); 1 }
            Some(Token::Intersect) => { self.advance(); 2 }
            Some(Token::Except)    => { self.advance(); 3 }
            _ => 0,
        };
        if set_op > 0 {
            let all = if self.peek() == Some(&Token::All) { self.advance(); true } else { false };
            match self.advance() {
                Some(Token::Select) => {}
                other => return Err(format!("Expected SELECT after set operator, got {:?}", other)),
            }
            let right = self.parse_select()?;
            let (right_clean, op_order_by, op_limit, op_offset) = match right {
                Statement::Select { table, subquery, columns, distinct, condition, joins,
                                    order_by, group_by, having, limit, offset, for_update, for_share } => {
                    let clean = Statement::Select {
                        table, subquery, columns, distinct, condition, joins,
                        order_by: vec![], group_by, having,
                        limit: None, offset: None, for_update, for_share,
                    };
                    (clean, order_by, limit, offset)
                }
                other => (other, vec![], None, None),
            };
            return Ok(match set_op {
                1 => Statement::Union {
                    left: Box::new(select_stmt), right: Box::new(right_clean),
                    all, order_by: op_order_by, limit: op_limit, offset: op_offset,
                },
                2 => Statement::Intersect {
                    left: Box::new(select_stmt), right: Box::new(right_clean),
                    all, order_by: op_order_by, limit: op_limit, offset: op_offset,
                },
                _ => Statement::Except {
                    left: Box::new(select_stmt), right: Box::new(right_clean),
                    all, order_by: op_order_by, limit: op_limit, offset: op_offset,
                },
            });
        }

        Ok(select_stmt)
    }

    fn parse_insert(&mut self) -> Result<Statement, String> {
        // INSERT [IGNORE] INTO table [(col1, col2, ...)] VALUES (...) [ON DUPLICATE KEY UPDATE ...]
        let ignore = if self.peek() == Some(&Token::Ignore) {
            self.advance();
            true
        } else {
            false
        };
        match self.advance() {
            Some(Token::Into) => {}
            other => return Err(format!("Expected INTO, got {:?}", other)),
        }
        let table = self.expect_ident()?;

        // 선택적 컬럼 목록: (col1, col2, ...)
        let columns = if self.peek() == Some(&Token::LParen) {
            self.advance(); // '(' 소비
            let mut cols = vec![self.expect_ident()?];
            while self.peek() == Some(&Token::Comma) {
                self.advance();
                cols.push(self.expect_ident()?);
            }
            match self.advance() {
                Some(Token::RParen) => {}
                other => return Err(format!("Expected ')' after column list, got {:?}", other)),
            }
            Some(cols)
        } else {
            None
        };

        // INSERT INTO table [(cols)] SELECT ...
        if self.peek() == Some(&Token::Select) {
            self.advance(); // consume SELECT
            let query = self.parse_select()?;
            let on_conflict = if ignore { InsertConflict::Ignore } else { InsertConflict::Abort };
            let returning = self.parse_returning()?;
            return Ok(Statement::InsertSelect { table, columns, query: Box::new(query), on_conflict, returning });
        }

        match self.advance() {
            Some(Token::Values) => {}
            other => return Err(format!("Expected VALUES or SELECT, got {:?}", other)),
        }

        // 하나 이상의 값 그룹: VALUES (...), (...)
        let mut all_values: Vec<Vec<String>> = Vec::new();
        loop {
            match self.advance() {
                Some(Token::LParen) => {}
                other => return Err(format!("Expected '(', got {:?}", other)),
            }
            let mut row_vals = Vec::new();
            loop {
                let val = match self.peek() {
                    Some(Token::Comma) | Some(Token::RParen) => String::new(),
                    _ => match self.advance() {
                        Some(Token::StringLit(s)) => s.clone(),
                        Some(Token::NumberLit(n)) => n.clone(),
                        Some(Token::Null)         => "NULL".to_string(),
                        Some(Token::Ident(s))     => s.clone(),
                        other => return Err(format!("Expected value, got {:?}", other)),
                    }
                };
                row_vals.push(val);
                match self.peek() {
                    Some(Token::Comma)  => { self.advance(); }
                    Some(Token::RParen) => { self.advance(); break; }
                    other => return Err(format!("Expected ',' or ')', got {:?}", other)),
                }
            }
            all_values.push(row_vals);

            // 다음 값 그룹이 있으면 계속
            if self.peek() == Some(&Token::Comma) {
                self.advance();
            } else {
                break;
            }
        }

        // ON DUPLICATE KEY UPDATE col=val, ...
        let on_conflict = if self.peek() == Some(&Token::On) {
            self.advance(); // ON
            match self.advance() {
                Some(Token::Duplicate) => {}
                other => return Err(format!("Expected DUPLICATE, got {:?}", other)),
            }
            match self.advance() {
                Some(Token::Key) => {}
                other => return Err(format!("Expected KEY, got {:?}", other)),
            }
            match self.advance() {
                Some(Token::Update) => {}
                other => return Err(format!("Expected UPDATE, got {:?}", other)),
            }
            let mut assignments = Vec::new();
            loop {
                let col = self.expect_ident()?;
                match self.advance() {
                    Some(Token::Eq) => {}
                    other => return Err(format!("Expected '=' in ON DUPLICATE KEY UPDATE, got {:?}", other)),
                }
                let expr = self.parse_arith_expr()?;
                assignments.push((col, expr));
                if self.peek() == Some(&Token::Comma) {
                    self.advance();
                } else {
                    break;
                }
            }
            InsertConflict::Update(assignments)
        } else if ignore {
            InsertConflict::Ignore
        } else {
            InsertConflict::Abort
        };

        let returning = self.parse_returning()?;
        Ok(Statement::Insert { table, columns, values: all_values, on_conflict, returning })
    }

    fn parse_update(&mut self) -> Result<Statement, String> {
        // UPDATE [t1 [alias1]] [, t2 [alias2]] | [JOIN t2 ON ...] SET col = val [WHERE ...]
        let first_table = self.expect_ident()?;
        let mut alias_map: HashMap<String, String> = HashMap::new();

        // 선택적 첫 번째 테이블 별칭
        if matches!(self.peek(), Some(Token::Ident(_))) {
            let a = self.expect_ident()?;
            alias_map.insert(a, first_table.clone());
        }

        let mut tables = vec![first_table.clone()];
        let mut joins: Vec<Join> = Vec::new();

        // 쉼표로 구분된 멀티 테이블: UPDATE t1, t2 SET ...
        while self.peek() == Some(&Token::Comma) {
            self.advance();
            let t = self.expect_ident()?;
            if matches!(self.peek(), Some(Token::Ident(_))) {
                let a = self.expect_ident()?;
                alias_map.insert(a, t.clone());
            }
            tables.push(t);
        }

        // JOIN 절: UPDATE t1 JOIN t2 ON ...
        loop {
            let join_type = match self.peek() {
                Some(Token::Join)  => { self.advance(); JoinType::Inner }
                Some(Token::Inner) => {
                    self.advance();
                    match self.advance() {
                        Some(Token::Join) => {}
                        other => return Err(format!("Expected JOIN after INNER, got {:?}", other)),
                    }
                    JoinType::Inner
                }
                Some(Token::Left)  => {
                    self.advance();
                    if self.peek() == Some(&Token::Ident("OUTER".to_string())) { self.advance(); }
                    match self.advance() {
                        Some(Token::Join) => {}
                        other => return Err(format!("Expected JOIN after LEFT, got {:?}", other)),
                    }
                    JoinType::Left
                }
                Some(Token::Right) => {
                    self.advance();
                    match self.advance() {
                        Some(Token::Join) => {}
                        other => return Err(format!("Expected JOIN after RIGHT, got {:?}", other)),
                    }
                    JoinType::Right
                }
                _ => break,
            };
            let join_table = self.expect_ident()?;
            if matches!(self.peek(), Some(Token::Ident(_))) {
                let a = self.expect_ident()?;
                alias_map.insert(a, join_table.clone());
            }
            match self.advance() {
                Some(Token::On) => {}
                other => return Err(format!("Expected ON, got {:?}", other)),
            }
            let on_expr = expand_condexpr(self.parse_condexpr()?, &alias_map);
            joins.push(Join { table: join_table, on_expr, join_type });
        }

        match self.advance() {
            Some(Token::Set) => {}
            other => return Err(format!("Expected SET, got {:?}", other)),
        }

        let mut assignments = Vec::new();
        loop {
            // col 또는 table.col
            let col = self.expect_col_ref()?;
            let col = expand_alias_str(&col, &alias_map);
            match self.advance() {
                Some(Token::Eq) => {}
                other => return Err(format!("Expected =, got {:?}", other)),
            }
            let expr = self.parse_arith_expr()?;
            assignments.push((col, expr));
            if self.peek() == Some(&Token::Comma) { self.advance(); } else { break; }
        }

        let condition = if self.peek() == Some(&Token::Where) {
            self.advance();
            Some(expand_condexpr(self.parse_condexpr()?, &alias_map))
        } else {
            None
        };

        let returning = self.parse_returning()?;
        if tables.len() > 1 || !joins.is_empty() {
            Ok(Statement::MultiUpdate { tables, joins, assignments, condition })
        } else {
            Ok(Statement::Update { table: first_table, assignments, condition, returning })
        }
    }

    fn parse_delete(&mut self) -> Result<Statement, String> {
        // DELETE [t1 [, t2]] FROM table [JOIN ...] WHERE ...
        // or DELETE FROM table WHERE ...

        // FROM이 아닌 식별자가 오면 → 삭제 대상 테이블 목록
        let delete_tables: Option<Vec<String>> = if self.peek() != Some(&Token::From) {
            let mut tbls = vec![self.expect_ident()?];
            while self.peek() == Some(&Token::Comma) {
                self.advance();
                tbls.push(self.expect_ident()?);
            }
            match self.advance() {
                Some(Token::From) => {}
                other => return Err(format!("Expected FROM, got {:?}", other)),
            }
            Some(tbls)
        } else {
            self.advance(); // FROM
            None
        };

        let from_table = self.expect_ident()?;
        let mut alias_map: HashMap<String, String> = HashMap::new();
        if matches!(self.peek(), Some(Token::Ident(_))) {
            let a = self.expect_ident()?;
            alias_map.insert(a, from_table.clone());
        }

        // JOIN 절 파싱
        let mut joins: Vec<Join> = Vec::new();
        loop {
            let join_type = match self.peek() {
                Some(Token::Join)  => { self.advance(); JoinType::Inner }
                Some(Token::Inner) => {
                    self.advance();
                    match self.advance() {
                        Some(Token::Join) => {}
                        other => return Err(format!("Expected JOIN after INNER, got {:?}", other)),
                    }
                    JoinType::Inner
                }
                Some(Token::Left)  => {
                    self.advance();
                    match self.advance() {
                        Some(Token::Join) => {}
                        other => return Err(format!("Expected JOIN after LEFT, got {:?}", other)),
                    }
                    JoinType::Left
                }
                Some(Token::Right) => {
                    self.advance();
                    match self.advance() {
                        Some(Token::Join) => {}
                        other => return Err(format!("Expected JOIN after RIGHT, got {:?}", other)),
                    }
                    JoinType::Right
                }
                _ => break,
            };
            let join_table = self.expect_ident()?;
            if matches!(self.peek(), Some(Token::Ident(_))) {
                let a = self.expect_ident()?;
                alias_map.insert(a, join_table.clone());
            }
            match self.advance() {
                Some(Token::On) => {}
                other => return Err(format!("Expected ON, got {:?}", other)),
            }
            let on_expr = expand_condexpr(self.parse_condexpr()?, &alias_map);
            joins.push(Join { table: join_table, on_expr, join_type });
        }

        let condition = if self.peek() == Some(&Token::Where) {
            self.advance();
            Some(expand_condexpr(self.parse_condexpr()?, &alias_map))
        } else {
            None
        };

        let returning = self.parse_returning()?;
        if let Some(del_tbls) = delete_tables {
            Ok(Statement::MultiDelete { delete_tables: del_tbls, from_table, joins, condition })
        } else if !joins.is_empty() {
            Ok(Statement::MultiDelete {
                delete_tables: vec![from_table.clone()],
                from_table,
                joins,
                condition,
            })
        } else {
            Ok(Statement::Delete { table: from_table, condition, returning })
        }
    }

    /// 함수 호출 인수 파싱: (arg1, arg2, ...) → Vec<String>
    /// 각 인수는 컬럼명, 문자열 리터럴('...'), 숫자 리터럴, 또는 '*'
    fn parse_func_args(&mut self) -> Result<Vec<String>, String> {
        match self.advance() {
            Some(Token::LParen) => {}
            other => return Err(format!("Expected '(' after function name, got {:?}", other)),
        }
        let mut args = Vec::new();
        while self.peek() != Some(&Token::RParen) {
            if !args.is_empty() {
                match self.advance() {
                    Some(Token::Comma) => {}
                    other => return Err(format!("Expected ',' in function args, got {:?}", other)),
                }
            }
            if self.peek() == Some(&Token::RParen) { break; }
            // Parse each arg as a full arithmetic expression to support ROUND(salary / 1000000, 2)
            let expr = self.parse_arith_expr()?;
            args.push(Self::arith_to_string(&expr));
        }
        match self.advance() {
            Some(Token::RParen) => {}
            other => return Err(format!("Expected ')' after function args, got {:?}", other)),
        }
        Ok(args)
    }

    /// CAST(expr AS type) → ["expr", "TYPE"]
    fn parse_cast_args(&mut self) -> Result<Vec<String>, String> {
        match self.advance() {
            Some(Token::LParen) => {}
            other => return Err(format!("Expected '(' after CAST, got {:?}", other)),
        }
        // expr: identifier or literal
        let expr = match self.advance() {
            Some(Token::StringLit(s)) => format!("'{}'", s),
            Some(Token::NumberLit(n)) => n.clone(),
            Some(Token::Null)         => "NULL".to_string(),
            Some(Token::Ident(s))     => {
                let s = s.clone();
                if self.peek() == Some(&Token::Dot) {
                    self.advance();
                    let col = self.expect_ident()?;
                    format!("{}.{}", s, col)
                } else { s }
            }
            other => return Err(format!("Expected expression in CAST, got {:?}", other)),
        };
        match self.advance() {
            Some(Token::As) => {}
            other => return Err(format!("Expected AS in CAST, got {:?}", other)),
        }
        // type: ident (INT, VARCHAR, DATE, etc.)
        let type_str = match self.advance() {
            Some(Token::Ident(s)) => s.clone().to_uppercase(),
            Some(Token::Int)      => "INT".to_string(),
            Some(Token::Float)    => "FLOAT".to_string(),
            Some(Token::Double)   => "DOUBLE".to_string(),
            Some(Token::Text)     => "TEXT".to_string(),
            Some(Token::Varchar)  => "VARCHAR".to_string(),
            Some(Token::Date)     => "DATE".to_string(),
            Some(Token::Datetime) => "DATETIME".to_string(),
            Some(Token::Decimal)  => "DECIMAL".to_string(),
            Some(Token::Boolean)  => "BOOLEAN".to_string(),
            other => return Err(format!("Expected type in CAST, got {:?}", other)),
        };
        // optional (n) for VARCHAR(n)
        if self.peek() == Some(&Token::LParen) {
            self.advance();
            while self.peek() != Some(&Token::RParen) && self.peek().is_some() {
                self.advance();
            }
            self.advance(); // consume ')'
        }
        match self.advance() {
            Some(Token::RParen) => {}
            other => return Err(format!("Expected ')' after CAST, got {:?}", other)),
        }
        Ok(vec![expr, type_str])
    }

    /// DATE_ADD(date, INTERVAL n unit) → ["date_expr", "n", "UNIT"]
    fn parse_date_add_args(&mut self) -> Result<Vec<String>, String> {
        match self.advance() {
            Some(Token::LParen) => {}
            other => return Err(format!("Expected '(' after DATE_ADD, got {:?}", other)),
        }
        let date_expr = match self.advance() {
            Some(Token::StringLit(s)) => format!("'{}'", s),
            Some(Token::Ident(s))     => {
                let s = s.clone();
                if self.peek() == Some(&Token::Dot) {
                    self.advance();
                    let col = self.expect_ident()?;
                    format!("{}.{}", s, col)
                } else { s }
            }
            other => return Err(format!("Expected date expr in DATE_ADD, got {:?}", other)),
        };
        match self.advance() {
            Some(Token::Comma) => {}
            other => return Err(format!("Expected ',' in DATE_ADD, got {:?}", other)),
        }
        match self.advance() {
            Some(Token::Interval) => {}
            other => return Err(format!("Expected INTERVAL in DATE_ADD, got {:?}", other)),
        }
        let amount = match self.advance() {
            Some(Token::NumberLit(n)) => n.clone(),
            Some(Token::Minus) => {
                let n = match self.advance() {
                    Some(Token::NumberLit(n)) => n.clone(),
                    other => return Err(format!("Expected number after - in INTERVAL, got {:?}", other)),
                };
                format!("-{}", n)
            }
            other => return Err(format!("Expected number in INTERVAL, got {:?}", other)),
        };
        // unit: DAY/MONTH/YEAR/HOUR/MINUTE/SECOND — 대부분 Ident로 파싱됨, Year은 별도 토큰
        let unit = match self.advance() {
            Some(Token::Ident(s)) => s.clone().to_uppercase(),
            Some(Token::Year)     => "YEAR".to_string(),
            other => return Err(format!("Expected INTERVAL unit in DATE_ADD, got {:?}", other)),
        };
        match self.advance() {
            Some(Token::RParen) => {}
            other => return Err(format!("Expected ')' after DATE_ADD, got {:?}", other)),
        }
        Ok(vec![date_expr, amount, unit])
    }

    /// 괄호 안의 원시 SQL 표현식을 문자열로 캡처 (CHECK 제약 저장용)
    fn read_parenthesized_expr(&mut self) -> Result<String, String> {
        match self.advance() {
            Some(Token::LParen) => {}
            other => return Err(format!("Expected '(' for expression, got {:?}", other)),
        }
        let mut parts = Vec::new();
        let mut depth = 1usize;
        loop {
            match self.advance() {
                None => return Err("Unexpected end in expression".to_string()),
                Some(tok) => {
                    match tok {
                        Token::LParen => { depth += 1; parts.push("(".to_string()); }
                        Token::RParen => {
                            depth -= 1;
                            if depth == 0 { break; }
                            parts.push(")".to_string());
                        }
                        Token::Ident(s) => parts.push(s.clone()),
                        Token::StringLit(s) => parts.push(format!("'{}'", s)),
                        Token::NumberLit(n) => parts.push(n.clone()),
                        Token::And => parts.push("AND".to_string()),
                        Token::Or  => parts.push("OR".to_string()),
                        Token::Not => parts.push("NOT".to_string()),
                        Token::Eq  => parts.push("=".to_string()),
                        Token::Ne  => parts.push("!=".to_string()),
                        Token::Gt  => parts.push(">".to_string()),
                        Token::Lt  => parts.push("<".to_string()),
                        Token::Gte => parts.push(">=".to_string()),
                        Token::Lte => parts.push("<=".to_string()),
                        Token::Null => parts.push("NULL".to_string()),
                        Token::Is   => parts.push("IS".to_string()),
                        Token::In   => parts.push("IN".to_string()),
                        Token::Between => parts.push("BETWEEN".to_string()),
                        Token::Like    => parts.push("LIKE".to_string()),
                        Token::Comma   => parts.push(",".to_string()),
                        other => parts.push(format!("{:?}", other)),
                    }
                }
            }
        }
        Ok(parts.join(" "))
    }

    /// 데이터 타입 파싱: INT, BIGINT, SMALLINT, TINYINT, TEXT, FLOAT, BOOLEAN, VARCHAR(n), DATE, DECIMAL(p,s), DOUBLE, TIME, YEAR, ENUM, SET, JSON
    fn parse_data_type(&mut self) -> Result<DataType, String> {
        match self.advance() {
            Some(Token::Int)     => {
                // INT [(n)] [UNSIGNED] [ZEROFILL] — 선택적 크기·수정자 무시
                if self.peek() == Some(&Token::LParen) {
                    self.advance();
                    while self.peek() != Some(&Token::RParen) { self.advance(); }
                    self.advance();
                }
                if matches!(self.peek(), Some(Token::Ident(s)) if s.to_uppercase() == "UNSIGNED") { self.advance(); }
                Ok(DataType::Int)
            }
            Some(Token::BigInt) => {
                if self.peek() == Some(&Token::LParen) {
                    self.advance();
                    while self.peek() != Some(&Token::RParen) { self.advance(); }
                    self.advance();
                }
                if matches!(self.peek(), Some(Token::Ident(s)) if s.to_uppercase() == "UNSIGNED") { self.advance(); }
                Ok(DataType::BigInt)
            }
            Some(Token::SmallInt) => {
                if self.peek() == Some(&Token::LParen) {
                    self.advance();
                    while self.peek() != Some(&Token::RParen) { self.advance(); }
                    self.advance();
                }
                if matches!(self.peek(), Some(Token::Ident(s)) if s.to_uppercase() == "UNSIGNED") { self.advance(); }
                Ok(DataType::SmallInt)
            }
            Some(Token::TinyInt) => {
                if self.peek() == Some(&Token::LParen) {
                    self.advance();
                    while self.peek() != Some(&Token::RParen) { self.advance(); }
                    self.advance();
                }
                if matches!(self.peek(), Some(Token::Ident(s)) if s.to_uppercase() == "UNSIGNED") { self.advance(); }
                Ok(DataType::TinyInt)
            }
            Some(Token::Text)    => Ok(DataType::Text),
            Some(Token::Float)   => {
                if self.peek() == Some(&Token::LParen) {
                    self.advance();
                    while self.peek() != Some(&Token::RParen) { self.advance(); }
                    self.advance();
                }
                Ok(DataType::Float)
            }
            Some(Token::Boolean) => Ok(DataType::Boolean),
            Some(Token::Double)  => {
                // DOUBLE or DOUBLE(p,s) — 선택적 정밀도 무시
                if self.peek() == Some(&Token::LParen) {
                    self.advance();
                    while self.peek() != Some(&Token::RParen) { self.advance(); }
                    self.advance();
                }
                Ok(DataType::Double)
            }
            Some(Token::Time)    => Ok(DataType::Time),
            Some(Token::Year)    => Ok(DataType::Year),
            Some(Token::Blob)    => Ok(DataType::Blob),
            Some(Token::Json)    => Ok(DataType::Json),
            Some(Token::Enum) => {
                match self.advance() {
                    Some(Token::LParen) => {}
                    other => return Err(format!("Expected '(' after ENUM, got {:?}", other)),
                }
                let mut values = Vec::new();
                loop {
                    match self.advance() {
                        Some(Token::StringLit(s)) => values.push(s.clone()),
                        Some(Token::RParen) => break,
                        other => return Err(format!("Expected string value in ENUM, got {:?}", other)),
                    }
                    match self.peek() {
                        Some(Token::Comma)  => { self.advance(); }
                        Some(Token::RParen) => { self.advance(); break; }
                        _ => break,
                    }
                }
                Ok(DataType::Enum(values))
            }
            Some(Token::Set) => {
                // SET('val1','val2',...) — 데이터 타입으로서의 SET
                match self.advance() {
                    Some(Token::LParen) => {}
                    other => return Err(format!("Expected '(' after SET type, got {:?}", other)),
                }
                let mut values = Vec::new();
                loop {
                    match self.advance() {
                        Some(Token::StringLit(s)) => values.push(s.clone()),
                        Some(Token::RParen) => break,
                        other => return Err(format!("Expected string value in SET, got {:?}", other)),
                    }
                    match self.peek() {
                        Some(Token::Comma)  => { self.advance(); }
                        Some(Token::RParen) => { self.advance(); break; }
                        _ => break,
                    }
                }
                Ok(DataType::Set(values))
            }
            Some(Token::Varchar) => {
                match self.advance() {
                    Some(Token::LParen) => {}
                    other => return Err(format!("Expected '(' after VARCHAR, got {:?}", other)),
                }
                let n = match self.advance() {
                    Some(Token::NumberLit(n)) => n.parse::<u32>().unwrap_or(255),
                    other => return Err(format!("Expected number in VARCHAR(n), got {:?}", other)),
                };
                match self.advance() {
                    Some(Token::RParen) => {}
                    other => return Err(format!("Expected ')' after VARCHAR size, got {:?}", other)),
                }
                Ok(DataType::Varchar(n))
            }
            Some(Token::Date)      => Ok(DataType::Date),
            Some(Token::Datetime)  => Ok(DataType::DateTime),
            Some(Token::Timestamp) => Ok(DataType::Timestamp),
            Some(Token::Decimal) => {
                match self.advance() {
                    Some(Token::LParen) => {}
                    other => return Err(format!("Expected '(' after DECIMAL, got {:?}", other)),
                }
                let p = match self.advance() {
                    Some(Token::NumberLit(n)) => n.parse::<u8>().unwrap_or(10),
                    other => return Err(format!("Expected precision in DECIMAL(p,s), got {:?}", other)),
                };
                match self.advance() {
                    Some(Token::Comma) => {}
                    other => return Err(format!("Expected ',' in DECIMAL(p,s), got {:?}", other)),
                }
                let s = match self.advance() {
                    Some(Token::NumberLit(n)) => n.parse::<u8>().unwrap_or(2),
                    other => return Err(format!("Expected scale in DECIMAL(p,s), got {:?}", other)),
                };
                match self.advance() {
                    Some(Token::RParen) => {}
                    other => return Err(format!("Expected ')' after DECIMAL scale, got {:?}", other)),
                }
                Ok(DataType::Decimal(p, s))
            }
            other => Err(format!("Expected data type, got {:?}", other)),
        }
    }

    /// 컬럼 제약 공통 파서: PRIMARY KEY, NOT NULL, UNIQUE, AUTO INCREMENT, DEFAULT, REFERENCES, CHECK
    fn parse_col_constraints(
        &mut self,
        col_name: &str,
        primary_key: &mut bool,
        not_null: &mut bool,
        unique: &mut bool,
        _unique_constraint_name: &mut Option<String>,
        auto_increment: &mut bool,
        default: &mut Option<String>,
        foreign_key: &mut Option<ForeignKey>,
        check_expr: &mut Option<String>,
    ) -> Result<(), String> {
        loop {
            match self.peek() {
                Some(Token::Check) => {
                    self.advance();
                    let expr = self.read_parenthesized_expr()?;
                    *check_expr = Some(expr);
                }
                Some(Token::Primary) => {
                    self.advance();
                    match self.advance() {
                        Some(Token::Key) => { *primary_key = true; *not_null = true; }
                        other => return Err(format!("Expected KEY, got {:?}", other)),
                    }
                }
                Some(Token::Not) => {
                    self.advance();
                    match self.advance() {
                        Some(Token::Null) => { *not_null = true; }
                        other => return Err(format!("Expected NULL, got {:?}", other)),
                    }
                }
                Some(Token::Unique) => {
                    self.advance();
                    *unique = true;
                }
                Some(Token::Auto) => {
                    self.advance();
                    match self.advance() {
                        Some(Token::Increment) => { *auto_increment = true; }
                        other => return Err(format!("Expected INCREMENT, got {:?}", other)),
                    }
                }
                Some(Token::Default) => {
                    self.advance();
                    let val = match self.advance() {
                        Some(Token::StringLit(s)) => s.clone(),
                        Some(Token::NumberLit(n)) => n.clone(),
                        Some(Token::Null)         => NULL_DEFAULT.to_string(),
                        Some(Token::Ident(s))     => s.clone(),
                        other => return Err(format!("Expected default value, got {:?}", other)),
                    };
                    *default = Some(val);
                }
                Some(Token::References) => {
                    self.advance();
                    let ref_table = self.expect_ident()?;
                    match self.advance() {
                        Some(Token::LParen) => {}
                        other => return Err(format!("Expected '(', got {:?}", other)),
                    }
                    let ref_column = self.expect_ident()?;
                    match self.advance() {
                        Some(Token::RParen) => {}
                        other => return Err(format!("Expected ')', got {:?}", other)),
                    }

                    // ON DELETE / ON UPDATE (순서 무관, 최대 2회)
                    let mut on_delete = FkAction::Restrict;
                    let mut on_update = FkAction::Restrict;
                    while self.peek() == Some(&Token::On) {
                        self.advance(); // ON
                        let parse_fk_action = |p: &mut Parser| -> Result<FkAction, String> {
                            match p.advance() {
                                Some(Token::Cascade)  => Ok(FkAction::Cascade),
                                Some(Token::Restrict) => Ok(FkAction::Restrict),
                                Some(Token::Ident(s)) if s.to_uppercase() == "NO" => {
                                    // NO ACTION = RESTRICT
                                    p.advance(); // ACTION
                                    Ok(FkAction::Restrict)
                                }
                                Some(Token::Set) => {
                                    match p.advance() {
                                        Some(Token::Null) => Ok(FkAction::SetNull),
                                        Some(Token::Default) => Ok(FkAction::SetDefault),
                                        other => Err(format!("Expected NULL or DEFAULT after SET, got {:?}", other)),
                                    }
                                }
                                other => Err(format!("Expected CASCADE/RESTRICT/SET, got {:?}", other)),
                            }
                        };
                        match self.advance() {
                            Some(Token::Delete) => { on_delete = parse_fk_action(self)?; }
                            Some(Token::Update) => { on_update = parse_fk_action(self)?; }
                            other => return Err(format!("Expected DELETE or UPDATE after ON, got {:?}", other)),
                        }
                    }

                    *foreign_key = Some(ForeignKey {
                        column: col_name.to_string(),
                        ref_table,
                        ref_column,
                        on_delete,
                        on_update,
                    });
                }
                _ => break,
            }
        }
        Ok(())
    }

    /// FOREIGN KEY (col[, col2...]) REFERENCES ref_table(ref_col) [ON DELETE ...] [ON UPDATE ...]
    /// 다중 컬럼의 경우 첫 번째 컬럼에만 FK 제약 적용 (단일 컬럼 ForeignKey 구조 유지)
    fn parse_fk_table_level(&mut self, columns: &mut Vec<ColumnDef>) -> Result<(), String> {
        match self.advance() {
            Some(Token::LParen) => {}
            other => return Err(format!("Expected '(' after FOREIGN KEY, got {:?}", other)),
        }
        let fk_col = self.expect_ident()?;
        // 추가 컬럼은 파싱만 하고 무시
        while self.peek() == Some(&Token::Comma) { self.advance(); self.expect_ident()?; }
        match self.advance() {
            Some(Token::RParen) => {}
            other => return Err(format!("Expected ')' after FK columns, got {:?}", other)),
        }
        match self.advance() {
            Some(Token::References) => {}
            other => return Err(format!("Expected REFERENCES, got {:?}", other)),
        }
        let ref_table = self.expect_ident()?;
        match self.advance() {
            Some(Token::LParen) => {}
            other => return Err(format!("Expected '(' after ref table, got {:?}", other)),
        }
        let ref_column = self.expect_ident()?;
        while self.peek() == Some(&Token::Comma) { self.advance(); self.expect_ident()?; }
        match self.advance() {
            Some(Token::RParen) => {}
            other => return Err(format!("Expected ')' after ref column, got {:?}", other)),
        }
        let mut on_delete = FkAction::Restrict;
        let mut on_update = FkAction::Restrict;
        while self.peek() == Some(&Token::On) {
            self.advance();
            let parse_fk_action = |p: &mut Parser| -> Result<FkAction, String> {
                match p.advance() {
                    Some(Token::Cascade)  => Ok(FkAction::Cascade),
                    Some(Token::Restrict) => Ok(FkAction::Restrict),
                    Some(Token::Ident(s)) if s.to_uppercase() == "NO" => { p.advance(); Ok(FkAction::Restrict) }
                    Some(Token::Set) => match p.advance() {
                        Some(Token::Null)    => Ok(FkAction::SetNull),
                        Some(Token::Default) => Ok(FkAction::SetDefault),
                        other => Err(format!("Expected NULL or DEFAULT after SET, got {:?}", other)),
                    },
                    other => Err(format!("Expected CASCADE/RESTRICT/SET, got {:?}", other)),
                }
            };
            match self.advance() {
                Some(Token::Delete) => { on_delete = parse_fk_action(self)?; }
                Some(Token::Update) => { on_update = parse_fk_action(self)?; }
                other => return Err(format!("Expected DELETE or UPDATE after ON, got {:?}", other)),
            }
        }
        if let Some(c) = columns.iter_mut().find(|c| c.name == fk_col) {
            c.foreign_key = Some(ForeignKey {
                column: fk_col.clone(),
                ref_table,
                ref_column,
                on_delete,
                on_update,
            });
        } else {
            return Err(format!("FOREIGN KEY: column '{}' not defined", fk_col));
        }
        Ok(())
    }

    fn parse_create(&mut self) -> Result<Statement, String> {
        match self.advance() {
            Some(Token::Table) => {}
            other => return Err(format!("Expected TABLE, got {:?}", other)),
        }

        // IF NOT EXISTS
        let if_not_exists = if self.peek() == Some(&Token::If) {
            self.advance(); // IF
            match self.advance() {
                Some(Token::Not) => {}
                other => return Err(format!("Expected NOT after IF, got {:?}", other)),
            }
            match self.advance() {
                Some(Token::Exists) => {}
                other => return Err(format!("Expected EXISTS, got {:?}", other)),
            }
            true
        } else {
            false
        };

        let name = self.expect_ident()?;
        match self.advance() {
            Some(Token::LParen) => {}
            other => return Err(format!("Expected '(', got {:?}", other)),
        }

        let mut columns: Vec<ColumnDef> = Vec::new();
        let mut primary_key_columns: Vec<String> = Vec::new();
        let mut check_constraints: Vec<(Option<String>, String)> = Vec::new();
        loop {
            // PRIMARY KEY (col1, col2) — 테이블 레벨 복합 PK
            if self.peek() == Some(&Token::Primary) {
                self.advance(); // PRIMARY
                match self.advance() {
                    Some(Token::Key) => {}
                    other => return Err(format!("Expected KEY after PRIMARY, got {:?}", other)),
                }
                match self.advance() {
                    Some(Token::LParen) => {}
                    other => return Err(format!("Expected '(' after PRIMARY KEY, got {:?}", other)),
                }
                let mut pk_cols = vec![self.expect_ident()?];
                while self.peek() == Some(&Token::Comma) {
                    self.advance();
                    pk_cols.push(self.expect_ident()?);
                }
                match self.advance() {
                    Some(Token::RParen) => {}
                    other => return Err(format!("Expected ')' after PK columns, got {:?}", other)),
                }
                // 해당 컬럼들에 primary_key / not_null 설정
                for pk_col in &pk_cols {
                    if let Some(c) = columns.iter_mut().find(|c| &c.name == pk_col) {
                        c.primary_key = true;
                        c.not_null = true;
                    }
                }
                primary_key_columns = pk_cols;
            // CHECK (expr) — 테이블 레벨 CHECK 제약
            } else if self.peek() == Some(&Token::Check) {
                self.advance(); // CHECK
                let expr = self.read_parenthesized_expr()?;
                check_constraints.push((None, expr));
            // CONSTRAINT name ... — 테이블 레벨 named 제약
            } else if self.peek() == Some(&Token::Constraint) {
                self.advance();
                let constraint_name = self.expect_ident()?;
                match self.peek() {
                    Some(Token::Unique) => {
                        self.advance();
                        // UNIQUE [KEY] (col)
                        if self.peek() == Some(&Token::Key) { self.advance(); }
                        match self.advance() {
                            Some(Token::LParen) => {}
                            other => return Err(format!("Expected '(' after UNIQUE, got {:?}", other)),
                        }
                        let col = self.expect_ident()?;
                        match self.advance() {
                            Some(Token::RParen) => {}
                            other => return Err(format!("Expected ')' after column, got {:?}", other)),
                        }
                        if let Some(c) = columns.iter_mut().find(|c| c.name == col) {
                            c.unique = true;
                            c.unique_constraint_name = Some(constraint_name);
                        } else {
                            return Err(format!("CONSTRAINT: column '{}' not defined", col));
                        }
                    }
                    Some(Token::Check) => {
                        self.advance();
                        let expr = self.read_parenthesized_expr()?;
                        check_constraints.push((Some(constraint_name), expr));
                    }
                    Some(Token::Primary) => {
                        // CONSTRAINT name PRIMARY KEY (col, ...)
                        self.advance(); // PRIMARY
                        match self.advance() {
                            Some(Token::Key) => {}
                            other => return Err(format!("Expected KEY after PRIMARY, got {:?}", other)),
                        }
                        match self.advance() {
                            Some(Token::LParen) => {}
                            other => return Err(format!("Expected '(' after PRIMARY KEY, got {:?}", other)),
                        }
                        let mut pk_cols = vec![self.expect_ident()?];
                        while self.peek() == Some(&Token::Comma) {
                            self.advance();
                            pk_cols.push(self.expect_ident()?);
                        }
                        match self.advance() {
                            Some(Token::RParen) => {}
                            other => return Err(format!("Expected ')' after PK columns, got {:?}", other)),
                        }
                        for pk_col in &pk_cols {
                            if let Some(c) = columns.iter_mut().find(|c| &c.name == pk_col) {
                                c.primary_key = true;
                                c.not_null = true;
                            }
                        }
                        primary_key_columns = pk_cols;
                    }
                    Some(Token::Foreign) => {
                        // CONSTRAINT name FOREIGN KEY (col) REFERENCES ...
                        self.advance(); // FOREIGN
                        match self.advance() {
                            Some(Token::Key) => {}
                            other => return Err(format!("Expected KEY, got {:?}", other)),
                        }
                        self.parse_fk_table_level(&mut columns)?;
                    }
                    other => return Err(format!("Expected PRIMARY KEY, UNIQUE, CHECK, or FOREIGN KEY after CONSTRAINT name, got {:?}", other)),
                }
            // UNIQUE KEY name (col) — MySQL 인라인 UNIQUE 인덱스
            } else if self.peek() == Some(&Token::Unique) {
                self.advance(); // UNIQUE
                if self.peek() == Some(&Token::Key) { self.advance(); } // KEY optional
                // 이름은 있을 수도 없을 수도 있음
                let _idx_name = if matches!(self.peek(), Some(Token::Ident(_))) {
                    Some(self.expect_ident()?)
                } else { None };
                match self.advance() {
                    Some(Token::LParen) => {}
                    other => return Err(format!("Expected '(' after UNIQUE KEY name, got {:?}", other)),
                }
                let col = self.expect_ident()?;
                // 추가 컬럼은 무시 (단일 컬럼 UNIQUE만 제약에 반영)
                while self.peek() == Some(&Token::Comma) { self.advance(); self.expect_ident()?; }
                match self.advance() {
                    Some(Token::RParen) => {}
                    other => return Err(format!("Expected ')' after UNIQUE KEY columns, got {:?}", other)),
                }
                if let Some(c) = columns.iter_mut().find(|c| c.name == col) {
                    c.unique = true;
                }
            // INDEX / KEY name (col) — MySQL 인라인 인덱스 (파싱만, CREATE INDEX로 처리하지 않음)
            } else if matches!(self.peek(), Some(Token::Index) | Some(Token::Key)) {
                self.advance(); // INDEX or KEY
                // 인덱스 이름 (optional)
                if matches!(self.peek(), Some(Token::Ident(_))) { self.expect_ident()?; }
                match self.advance() {
                    Some(Token::LParen) => {}
                    other => return Err(format!("Expected '(' after INDEX/KEY name, got {:?}", other)),
                }
                // 컬럼 목록 소비
                self.expect_ident()?;
                while self.peek() == Some(&Token::Comma) { self.advance(); self.expect_ident()?; }
                match self.advance() {
                    Some(Token::RParen) => {}
                    other => return Err(format!("Expected ')' after INDEX columns, got {:?}", other)),
                }
            } else if self.peek() == Some(&Token::Foreign) {
                // 테이블 레벨: FOREIGN KEY (col[, col2...]) REFERENCES ref_table(ref_col) [ON DELETE ...] [ON UPDATE ...]
                self.advance(); // FOREIGN
                match self.advance() {
                    Some(Token::Key) => {}
                    other => return Err(format!("Expected KEY after FOREIGN, got {:?}", other)),
                }
                self.parse_fk_table_level(&mut columns)?;
            } else {
                // 일반 컬럼 정의
                let col_name = self.expect_ident()?;
                let data_type = self.parse_data_type()?;
                let mut primary_key = false;
                let mut not_null = false;
                let mut unique = false;
                let mut unique_constraint_name = None;
                let mut auto_increment = false;
                let mut default = None;
                let mut foreign_key = None;
                let mut check_expr = None;

                self.parse_col_constraints(
                    &col_name,
                    &mut primary_key, &mut not_null, &mut unique,
                    &mut unique_constraint_name, &mut auto_increment,
                    &mut default, &mut foreign_key, &mut check_expr,
                )?;

                columns.push(ColumnDef {
                    name: col_name,
                    data_type,
                    primary_key,
                    not_null,
                    unique,
                    unique_constraint_name,
                    auto_increment,
                    default,
                    foreign_key,
                    check_expr,
                });
            }

            match self.peek() {
                Some(Token::Comma)  => { self.advance(); }
                Some(Token::RParen) => { self.advance(); break; }
                other => return Err(format!("Expected ',' or ')', got {:?}", other)),
            }
        }

        Ok(Statement::CreateTable { name, columns, if_not_exists, primary_key_columns, check_constraints })
    }

    fn parse_drop(&mut self) -> Result<Statement, String> {
        // DROP TABLE [IF EXISTS] name
        match self.advance() {
            Some(Token::Table) => {}
            other => return Err(format!("Expected TABLE, got {:?}", other)),
        }
        // IF EXISTS 처리
        let if_exists = if self.peek() == Some(&Token::If) {
            self.advance(); // IF
            match self.advance() {
                Some(Token::Exists) => {}
                other => return Err(format!("Expected EXISTS after IF, got {:?}", other)),
            }
            true
        } else {
            false
        };
        let name = self.expect_ident()?;
        Ok(Statement::DropTable { name, if_exists })
    }

    fn parse_alter(&mut self) -> Result<Statement, String> {
        // ALTER TABLE name ADD COLUMN col TYPE
        // ALTER TABLE name DROP COLUMN col
        // ALTER TABLE name RENAME COLUMN col TO new_col
        match self.advance() {
            Some(Token::Table) => {}
            other => return Err(format!("Expected TABLE, got {:?}", other)),
        }
        let table = self.expect_ident()?;

        match self.advance() {
            Some(Token::Add) => {
                match self.peek() {
                    Some(Token::Constraint) | Some(Token::Foreign) | Some(Token::Unique) | Some(Token::Check) => {
                        let constraint_name = if self.peek() == Some(&Token::Constraint) {
                            self.advance();
                            if matches!(self.peek(), Some(Token::Ident(_))) {
                                Some(self.expect_ident()?)
                            } else { None }
                        } else { None };
                        match self.advance() {
                            Some(Token::Foreign) => {
                                match self.advance() {
                                    Some(Token::Key) => {}
                                    other => return Err(format!("Expected KEY after FOREIGN, got {:?}", other)),
                                }
                                match self.advance() {
                                    Some(Token::LParen) => {}
                                    other => return Err(format!("Expected '(' after FOREIGN KEY, got {:?}", other)),
                                }
                                let column = self.expect_ident()?;
                                match self.advance() {
                                    Some(Token::RParen) => {}
                                    other => return Err(format!("Expected ')', got {:?}", other)),
                                }
                                match self.advance() {
                                    Some(Token::References) => {}
                                    other => return Err(format!("Expected REFERENCES, got {:?}", other)),
                                }
                                let ref_table = self.expect_ident()?;
                                match self.advance() {
                                    Some(Token::LParen) => {}
                                    other => return Err(format!("Expected '(' after ref table, got {:?}", other)),
                                }
                                let ref_column = self.expect_ident()?;
                                match self.advance() {
                                    Some(Token::RParen) => {}
                                    other => return Err(format!("Expected ')' after ref column, got {:?}", other)),
                                }
                                let mut on_delete = FkAction::Restrict;
                                let mut on_update = FkAction::Restrict;
                                while self.peek() == Some(&Token::On) {
                                    self.advance();
                                    let parse_fk_action = |p: &mut Parser| -> Result<FkAction, String> {
                                        match p.advance() {
                                            Some(Token::Cascade)  => Ok(FkAction::Cascade),
                                            Some(Token::Restrict) => Ok(FkAction::Restrict),
                                            Some(Token::Ident(s)) if s.to_uppercase() == "NO" => { p.advance(); Ok(FkAction::Restrict) }
                                            Some(Token::Set) => match p.advance() {
                                                Some(Token::Null)    => Ok(FkAction::SetNull),
                                                Some(Token::Default) => Ok(FkAction::SetDefault),
                                                other => Err(format!("Expected NULL or DEFAULT, got {:?}", other)),
                                            },
                                            other => Err(format!("Expected FK action, got {:?}", other)),
                                        }
                                    };
                                    match self.advance() {
                                        Some(Token::Delete) => { on_delete = parse_fk_action(self)?; }
                                        Some(Token::Update) => { on_update = parse_fk_action(self)?; }
                                        other => return Err(format!("Expected DELETE or UPDATE after ON, got {:?}", other)),
                                    }
                                }
                                Ok(Statement::AlterTable { table, action: AlterAction::AddForeignKey { name: constraint_name, column, ref_table, ref_column, on_delete, on_update } })
                            }
                            Some(Token::Unique) => {
                                match self.advance() {
                                    Some(Token::LParen) => {}
                                    other => return Err(format!("Expected '(' after UNIQUE, got {:?}", other)),
                                }
                                let column = self.expect_ident()?;
                                match self.advance() {
                                    Some(Token::RParen) => {}
                                    other => return Err(format!("Expected ')' after column, got {:?}", other)),
                                }
                                Ok(Statement::AlterTable { table, action: AlterAction::AddUniqueConstraint { name: constraint_name, column } })
                            }
                            Some(Token::Check) => {
                                match self.advance() {
                                    Some(Token::LParen) => {}
                                    other => return Err(format!("Expected '(' after CHECK, got {:?}", other)),
                                }
                                let mut depth = 1usize;
                                let mut expr = String::new();
                                while let Some(tok) = self.advance() {
                                    match tok {
                                        Token::LParen => { depth += 1; expr.push('('); }
                                        Token::RParen => {
                                            depth -= 1;
                                            if depth == 0 { break; }
                                            expr.push(')');
                                        }
                                        Token::Ident(s) => { if !expr.is_empty() { expr.push(' '); } expr.push_str(s); }
                                        Token::NumberLit(s) => { if !expr.is_empty() { expr.push(' '); } expr.push_str(s); }
                                        Token::StringLit(s) => { if !expr.is_empty() { expr.push(' '); } expr.push('\''); expr.push_str(s); expr.push('\''); }
                                        Token::Gt  => { if !expr.is_empty() { expr.push(' '); } expr.push('>'); }
                                        Token::Lt  => { if !expr.is_empty() { expr.push(' '); } expr.push('<'); }
                                        Token::Gte => { if !expr.is_empty() { expr.push(' '); } expr.push_str(">="); }
                                        Token::Lte => { if !expr.is_empty() { expr.push(' '); } expr.push_str("<="); }
                                        Token::Eq  => { if !expr.is_empty() { expr.push(' '); } expr.push('='); }
                                        Token::Ne  => { if !expr.is_empty() { expr.push(' '); } expr.push_str("!="); }
                                        Token::And => { if !expr.is_empty() { expr.push(' '); } expr.push_str("AND"); }
                                        Token::Or  => { if !expr.is_empty() { expr.push(' '); } expr.push_str("OR"); }
                                        _ => {}
                                    }
                                }
                                Ok(Statement::AlterTable { table, action: AlterAction::AddCheckConstraint { name: constraint_name, expr } })
                            }
                            other => Err(format!("Expected FOREIGN, UNIQUE, or CHECK after CONSTRAINT, got {:?}", other)),
                        }
                    }
                    _ => {
                        if self.peek() == Some(&Token::Column) { self.advance(); }
                        let col_name = self.expect_ident()?;
                        let data_type = self.parse_data_type()?;
                        Ok(Statement::AlterTable {
                            table,
                            action: AlterAction::AddColumn(ColumnDef {
                                name: col_name,
                                data_type,
                                primary_key: false,
                                not_null: false,
                                unique: false,
                                unique_constraint_name: None,
                                auto_increment: false,
                                default: None,
                                foreign_key: None,
                                check_expr: None,
                            }),
                        })
                    }
                }
            }
            Some(Token::Drop) => {
                match self.peek() {
                    Some(Token::Constraint) => {
                        self.advance();
                        let name = self.expect_ident()?;
                        Ok(Statement::AlterTable { table, action: AlterAction::DropConstraint(name) })
                    }
                    Some(Token::Foreign) => {
                        self.advance(); // FOREIGN
                        match self.advance() {
                            Some(Token::Key) => {}
                            other => return Err(format!("Expected KEY after FOREIGN, got {:?}", other)),
                        }
                        let name = self.expect_ident()?;
                        Ok(Statement::AlterTable { table, action: AlterAction::DropForeignKey(name) })
                    }
                    _ => {
                        if self.peek() == Some(&Token::Column) { self.advance(); }
                        let col_name = self.expect_ident()?;
                        Ok(Statement::AlterTable { table, action: AlterAction::DropColumn(col_name) })
                    }
                }
            }
            Some(Token::Rename) => {
                match self.peek() {
                    Some(Token::Column) => {
                        self.advance(); // COLUMN
                        let from = self.expect_ident()?;
                        match self.advance() {
                            Some(Token::To) => {}
                            other => return Err(format!("Expected TO, got {:?}", other)),
                        }
                        let to = self.expect_ident()?;
                        Ok(Statement::AlterTable {
                            table,
                            action: AlterAction::RenameColumn { from, to },
                        })
                    }
                    Some(Token::To) => {
                        self.advance(); // TO
                        let to = self.expect_ident()?;
                        Ok(Statement::AlterTable { table, action: AlterAction::RenameTable { to } })
                    }
                    Some(Token::Ident(_)) => {
                        // RENAME new_name (TO 생략)
                        let to = self.expect_ident()?;
                        Ok(Statement::AlterTable { table, action: AlterAction::RenameTable { to } })
                    }
                    other => Err(format!("Expected COLUMN, TO, or table name after RENAME, got {:?}", other)),
                }
            }
            Some(Token::Modify) => {
                // MODIFY [COLUMN] col TYPE [constraints]
                if self.peek() == Some(&Token::Column) { self.advance(); }
                let col_name = self.expect_ident()?;
                let data_type = self.parse_data_type()?;
                let mut primary_key = false;
                let mut not_null = false;
                let mut unique = false;
                let mut unique_constraint_name = None;
                let mut auto_increment = false;
                let mut default = None;
                let mut foreign_key = None;
                let mut check_expr = None;
                self.parse_col_constraints(
                    &col_name,
                    &mut primary_key, &mut not_null, &mut unique,
                    &mut unique_constraint_name, &mut auto_increment,
                    &mut default, &mut foreign_key, &mut check_expr,
                )?;
                Ok(Statement::AlterTable {
                    table,
                    action: AlterAction::ModifyColumn(ColumnDef {
                        name: col_name,
                        data_type,
                        primary_key,
                        not_null,
                        unique,
                        unique_constraint_name,
                        auto_increment,
                        default,
                        foreign_key,
                        check_expr,
                    }),
                })
            }
            other => Err(format!("Expected ADD, DROP, RENAME, or MODIFY, got {:?}", other)),
        }
    }

    fn parse_create_index(&mut self) -> Result<Statement, String> {
        let index_name = self.expect_ident()?;
        match self.advance() {
            Some(Token::On) => {}
            other => return Err(format!("Expected ON, got {:?}", other)),
        }
        let table = self.expect_ident()?;
        match self.advance() {
            Some(Token::LParen) => {}
            other => return Err(format!("Expected '(', got {:?}", other)),
        }
        // 컬럼 목록 파싱 (단일 또는 복합)
        let mut columns = vec![self.expect_ident()?];
        while self.peek() == Some(&Token::Comma) {
            self.advance();
            columns.push(self.expect_ident()?);
        }
        match self.advance() {
            Some(Token::RParen) => {}
            other => return Err(format!("Expected ')', got {:?}", other)),
        }
        Ok(Statement::CreateIndex { index_name, table, columns })
    }

    fn parse_drop_index(&mut self) -> Result<Statement, String> {
        // IF EXISTS 처리
        if self.peek() == Some(&Token::If) {
            self.advance(); // IF
            match self.advance() {
                Some(Token::Exists) => {}
                other => return Err(format!("Expected EXISTS after IF, got {:?}", other)),
            }
        }
        let index_name = self.expect_ident()?;
        Ok(Statement::DropIndex { index_name })
    }

    fn parse_create_view(&mut self) -> Result<Statement, String> {
        let name = self.expect_ident()?;
        match self.advance() {
            Some(Token::As) => {}
            other => return Err(format!("Expected AS, got {:?}", other)),
        }
        match self.advance() {
            Some(Token::Select) => {}
            other => return Err(format!("Expected SELECT, got {:?}", other)),
        }
        let query = self.parse_select()?;
        Ok(Statement::CreateView {
            name,
            query: Box::new(query),
        })
    }

    fn parse_drop_view(&mut self) -> Result<Statement, String> {
        // IF EXISTS 처리
        if self.peek() == Some(&Token::If) {
            self.advance(); // IF
            match self.advance() {
                Some(Token::Exists) => {}
                other => return Err(format!("Expected EXISTS after IF, got {:?}", other)),
            }
        }
        let name = self.expect_ident()?;
        Ok(Statement::DropView { name })
    }

    fn parse_create_database(&mut self) -> Result<Statement, String> {
        let if_not_exists = if self.peek() == Some(&Token::If) {
            self.advance();
            match self.advance() {
                Some(Token::Not) => {}
                other => return Err(format!("Expected NOT after IF, got {:?}", other)),
            }
            match self.advance() {
                Some(Token::Exists) => {}
                other => return Err(format!("Expected EXISTS, got {:?}", other)),
            }
            true
        } else {
            false
        };
        let name = self.expect_ident()?;
        Ok(Statement::CreateDatabase { name, if_not_exists })
    }

    fn parse_drop_database(&mut self) -> Result<Statement, String> {
        let if_exists = if self.peek() == Some(&Token::If) {
            self.advance();
            match self.advance() {
                Some(Token::Exists) => {}
                other => return Err(format!("Expected EXISTS after IF, got {:?}", other)),
            }
            true
        } else {
            false
        };
        let name = self.expect_ident()?;
        Ok(Statement::DropDatabase { name, if_exists })
    }

    fn parse_backup(&mut self) -> Result<Statement, String> {
        // BACKUP [DATABASE db] [INTO 'file']
        // BACKUP INTO 'file'
        // BACKUP DATABASE db INTO 'file'
        let mut database = None;
        let mut output_file = None;

        if let Some(Token::Database) = self.peek().cloned().as_ref() {
            self.advance();
            database = Some(self.expect_ident()?);
        }
        if let Some(Token::Ident(s)) = self.peek().cloned().as_ref() {
            if s.to_uppercase() == "INTO" {
                self.advance();
                match self.advance() {
                    Some(Token::StringLit(f)) => output_file = Some(f.clone()),
                    other => return Err(format!("Expected filename string, got {:?}", other)),
                }
            }
        }
        Ok(Statement::Backup { database, output_file })
    }

    fn parse_show(&mut self) -> Result<Statement, String> {
        match self.advance() {
            Some(Token::Tables)  => Ok(Statement::ShowTables),
            Some(Token::Ident(s)) if s == "BUFFER" => {
                match self.advance() {
                    Some(Token::Ident(s)) if s == "POOL" => Ok(Statement::ShowBufferPool),
                    other => Err(format!("Expected POOL, got {:?}", other)),
                }
            }
            Some(Token::Ident(s)) if s == "WAL" => Ok(Statement::ShowWal),
            Some(Token::Isolation) => {
                match self.advance() {
                    Some(Token::Level) => Ok(Statement::ShowIsolationLevel),
                    other => Err(format!("Expected LEVEL, got {:?}", other)),
                }
            }
            Some(Token::Locks) => Ok(Statement::ShowLocks),
            Some(Token::Databases) => Ok(Statement::ShowDatabases),
            Some(Token::Grants) => {
                // SHOW GRANTS [FOR 'user'@'host']
                let (user, host) = if self.peek() == Some(&Token::For) {
                    self.advance(); // FOR
                    let (u, h) = self.parse_user_spec()?;
                    (Some(u), Some(h))
                } else {
                    (None, None)
                };
                Ok(Statement::ShowGrants { user, host })
            }
            Some(Token::Create) => {
                // SHOW CREATE TABLE <name>
                match self.advance() {
                    Some(Token::Table) => {}
                    other => return Err(format!("Expected TABLE after SHOW CREATE, got {:?}", other)),
                }
                let table = self.expect_ident()?;
                Ok(Statement::ShowCreateTable { table })
            }
            Some(Token::Ident(s)) if s.to_uppercase() == "PROCESSLIST" => Ok(Statement::ShowProcessList),
            other => Err(format!("Expected TABLES, BUFFER, WAL, ISOLATION, LOCKS, DATABASES, GRANTS, CREATE, or PROCESSLIST, got {:?}", other)),
        }
    }

    fn parse_set(&mut self) -> Result<Statement, String> {
        match self.advance() {
            Some(Token::At) => {
                let name = self.expect_ident()?;
                match self.advance() {
                    Some(Token::Eq) => {}
                    other => return Err(format!("Expected '=' after @{}, got {:?}", name, other)),
                }
                let expr = self.parse_arith_expr()?;
                Ok(Statement::SetUserVar { name, expr })
            }
            Some(Token::Isolation) => {
                match self.advance() {
                    Some(Token::Level) => {}
                    other => return Err(format!("Expected LEVEL, got {:?}", other)),
                }
                let level = match self.advance() {
                    // READ UNCOMMITTED / READ COMMITTED
                    Some(Token::Ident(s)) if s.to_uppercase() == "READ" => {
                        match self.advance() {
                            Some(Token::Uncommitted) => IsolationLevel::ReadUncommitted,
                            Some(Token::Committed)   => IsolationLevel::ReadCommitted,
                            other => return Err(format!("Expected UNCOMMITTED or COMMITTED after READ, got {:?}", other)),
                        }
                    }
                    // REPEATABLE READ
                    Some(Token::Repeatable) => {
                        self.advance(); // consume trailing "READ"
                        IsolationLevel::RepeatableRead
                    }
                    // SERIALIZABLE
                    Some(Token::Serializable) => IsolationLevel::Serializable,
                    other => return Err(format!("Expected isolation level name, got {:?}", other)),
                };
                Ok(Statement::SetIsolationLevel(level))
            }
            other => Err(format!("Expected ISOLATION, got {:?}", other)),
        }
    }

    fn parse_prepare(&mut self) -> Result<Statement, String> {
        // PREPARE stmt_name FROM 'query'
        let name = self.expect_ident()?;
        match self.advance() {
            Some(Token::From) | Some(Token::Ident(..)) => {}
            other => return Err(format!("Expected FROM in PREPARE, got {:?}", other)),
        }
        let query = match self.advance() {
            Some(Token::StringLit(s)) => s.clone(),
            other => return Err(format!("Expected query string in PREPARE, got {:?}", other)),
        };
        Ok(Statement::PrepareStmt { name, query })
    }

    fn parse_execute(&mut self) -> Result<Statement, String> {
        // EXECUTE stmt_name [USING @var1, @var2, ...]
        let name = self.expect_ident()?;
        let mut using_vars = Vec::new();
        if self.peek() == Some(&Token::Using) {
            self.advance();
            loop {
                match self.advance() {
                    Some(Token::At) => {
                        let var = self.expect_ident()?;
                        using_vars.push(var);
                    }
                    other => return Err(format!("Expected @var in EXECUTE USING, got {:?}", other)),
                }
                if self.peek() == Some(&Token::Comma) {
                    self.advance();
                } else {
                    break;
                }
            }
        }
        Ok(Statement::ExecuteStmt { name, using_vars })
    }

    fn parse_deallocate(&mut self) -> Result<Statement, String> {
        // DEALLOCATE PREPARE stmt_name
        match self.advance() {
            Some(Token::Prepare) => {}
            other => return Err(format!("Expected PREPARE after DEALLOCATE, got {:?}", other)),
        }
        let name = self.expect_ident()?;
        Ok(Statement::DeallocatePrepare { name })
    }

    fn parse_describe(&mut self) -> Result<Statement, String> {
        let table = self.expect_ident()?;
        Ok(Statement::Describe { table })
    }

    fn parse_truncate(&mut self) -> Result<Statement, String> {
        match self.advance() {
            Some(Token::Table) => {}
            other => return Err(format!("Expected TABLE, got {:?}", other)),
        }
        let name = self.expect_ident()?;
        Ok(Statement::TruncateTable { name })
    }

    fn parse_vacuum(&mut self) -> Result<Statement, String> {
        // VACUUM           — 모든 테이블 정리
        // VACUUM table     — 특정 테이블만 정리
        let table = match self.peek() {
            Some(Token::Ident(_)) => {
                if let Some(Token::Ident(s)) = self.advance() {
                    Some(s.clone())
                } else {
                    None
                }
            }
            _ => None,
        };
        Ok(Statement::Vacuum { table })
    }

    // ── 사용자 스펙 파싱: 'user'@'host' 또는 user@host 또는 user ──────────
    fn parse_user_spec(&mut self) -> Result<(String, String), String> {
        let user = match self.advance() {
            Some(Token::StringLit(s)) => s.clone(),
            Some(Token::Ident(s))     => s.clone(),
            other => return Err(format!("Expected username, got {:?}", other)),
        };
        let host = if self.peek() == Some(&Token::At) {
            self.advance(); // @
            match self.advance() {
                Some(Token::StringLit(s)) => s.clone(),
                Some(Token::Ident(s))     => s.clone(),
                Some(Token::NumberLit(s)) => s.clone(),
                Some(Token::Mod)          => "mod".to_string(),
                other => return Err(format!("Expected hostname after @, got {:?}", other)),
            }
        } else {
            "%".to_string()
        };
        Ok((user, host))
    }

    // CREATE USER ['user'@'host'] [IDENTIFIED BY 'password']
    fn parse_create_user(&mut self) -> Result<Statement, String> {
        let if_not_exists = if self.peek() == Some(&Token::If) {
            self.advance(); // IF
            match self.advance() { Some(Token::Not) => {} other => return Err(format!("Expected NOT, got {:?}", other)) }
            match self.advance() { Some(Token::Exists) => {} other => return Err(format!("Expected EXISTS, got {:?}", other)) }
            true
        } else { false };
        let (user, host) = self.parse_user_spec()?;
        let password = if self.peek() == Some(&Token::Identified) {
            self.advance(); // IDENTIFIED
            match self.advance() { Some(Token::By) | Some(Token::Ident(_)) => {} other => return Err(format!("Expected BY, got {:?}", other)) }
            match self.advance() {
                Some(Token::StringLit(s)) => Some(s.clone()),
                other => return Err(format!("Expected password string, got {:?}", other)),
            }
        } else { None };
        Ok(Statement::CreateUser { user, host, password, if_not_exists })
    }

    // DROP USER [IF EXISTS] 'user'@'host'
    fn parse_drop_user(&mut self) -> Result<Statement, String> {
        let if_exists = if self.peek() == Some(&Token::If) {
            self.advance();
            match self.advance() { Some(Token::Exists) => {} other => return Err(format!("Expected EXISTS, got {:?}", other)) }
            true
        } else { false };
        let (user, host) = self.parse_user_spec()?;
        Ok(Statement::DropUser { user, host, if_exists })
    }

    // GRANT priv [, priv ...] ON [TABLE|DATABASE|*] object TO 'user'@'host' [WITH GRANT OPTION]
    fn parse_grant(&mut self) -> Result<Statement, String> {
        let mut privileges: Vec<String> = Vec::new();
        loop {
            let priv_name = match self.advance() {
                Some(Token::Privileges) => "ALL PRIVILEGES".to_string(),
                Some(Token::All) => {
                    if self.peek() == Some(&Token::Privileges) { self.advance(); }
                    "ALL PRIVILEGES".to_string()
                }
                Some(Token::Ident(s)) if s.to_uppercase() == "ALL" => {
                    if self.peek() == Some(&Token::Privileges) { self.advance(); }
                    "ALL PRIVILEGES".to_string()
                }
                Some(Token::Select)  => "SELECT".to_string(),
                Some(Token::Insert)  => "INSERT".to_string(),
                Some(Token::Update)  => "UPDATE".to_string(),
                Some(Token::Delete)  => "DELETE".to_string(),
                Some(Token::Create)  => "CREATE".to_string(),
                Some(Token::Drop)    => "DROP".to_string(),
                Some(Token::Alter)   => "ALTER".to_string(),
                Some(Token::Index)   => "INDEX".to_string(),
                Some(Token::Grant)   => "GRANT OPTION".to_string(),
                Some(Token::Ident(s)) => s.to_uppercase().clone(),
                other => return Err(format!("Expected privilege name, got {:?}", other)),
            };
            privileges.push(priv_name);
            if self.peek() == Some(&Token::Comma) { self.advance(); } else { break; }
        }
        match self.advance() {
            Some(Token::On) => {}
            other => return Err(format!("Expected ON, got {:?}", other)),
        }
        // optional object type keyword
        let object_type = match self.peek() {
            Some(Token::Table)     => { self.advance(); "TABLE".to_string() }
            Some(Token::Database)  => { self.advance(); "DATABASE".to_string() }
            Some(Token::Databases) => { self.advance(); "DATABASES".to_string() }
            _ => "TABLE".to_string(),
        };
        // object: *.* or db.* or tablename or *
        let object = self.parse_grant_object()?;
        match self.advance() {
            Some(Token::To) => {}
            other => return Err(format!("Expected TO, got {:?}", other)),
        }
        let (user, host) = self.parse_user_spec()?;
        let with_grant_option = if self.peek() == Some(&Token::With) {
            self.advance(); // WITH
            if self.peek() == Some(&Token::Grant) { self.advance(); } // GRANT
            if self.peek() == Some(&Token::OptionKw) { self.advance(); } // OPTION
            true
        } else { false };
        Ok(Statement::Grant { privileges, object_type, object, user, host, with_grant_option })
    }

    fn parse_grant_object(&mut self) -> Result<String, String> {
        let first = match self.advance() {
            Some(Token::Asterisk) => "*".to_string(),
            Some(Token::Ident(s)) => s.clone(),
            Some(Token::StringLit(s)) => s.clone(),
            other => return Err(format!("Expected grant object, got {:?}", other)),
        };
        if self.peek() == Some(&Token::Dot) {
            self.advance(); // '.'
            let second = match self.advance() {
                Some(Token::Asterisk) => "*".to_string(),
                Some(Token::Ident(s)) => s.clone(),
                other => return Err(format!("Expected identifier or * after dot, got {:?}", other)),
            };
            Ok(format!("{}.{}", first, second))
        } else {
            Ok(first)
        }
    }

    // REVOKE priv [, priv ...] ON object FROM 'user'@'host'
    fn parse_revoke(&mut self) -> Result<Statement, String> {
        let mut privileges: Vec<String> = Vec::new();
        loop {
            let priv_name = match self.advance() {
                Some(Token::Privileges) => "ALL PRIVILEGES".to_string(),
                Some(Token::All) => {
                    if self.peek() == Some(&Token::Privileges) { self.advance(); }
                    "ALL PRIVILEGES".to_string()
                }
                Some(Token::Ident(s)) if s.to_uppercase() == "ALL" => {
                    if self.peek() == Some(&Token::Privileges) { self.advance(); }
                    "ALL PRIVILEGES".to_string()
                }
                Some(Token::Select)  => "SELECT".to_string(),
                Some(Token::Insert)  => "INSERT".to_string(),
                Some(Token::Update)  => "UPDATE".to_string(),
                Some(Token::Delete)  => "DELETE".to_string(),
                Some(Token::Create)  => "CREATE".to_string(),
                Some(Token::Drop)    => "DROP".to_string(),
                Some(Token::Alter)   => "ALTER".to_string(),
                Some(Token::Index)   => "INDEX".to_string(),
                Some(Token::Grant)   => "GRANT OPTION".to_string(),
                Some(Token::Ident(s)) => s.to_uppercase().clone(),
                other => return Err(format!("Expected privilege name, got {:?}", other)),
            };
            privileges.push(priv_name);
            if self.peek() == Some(&Token::Comma) { self.advance(); } else { break; }
        }
        match self.advance() {
            Some(Token::On) => {}
            other => return Err(format!("Expected ON, got {:?}", other)),
        }
        let object_type = match self.peek() {
            Some(Token::Table)     => { self.advance(); "TABLE".to_string() }
            Some(Token::Database)  => { self.advance(); "DATABASE".to_string() }
            Some(Token::Databases) => { self.advance(); "DATABASES".to_string() }
            _ => "TABLE".to_string(),
        };
        let object = self.parse_grant_object()?;
        match self.advance() {
            Some(Token::From) => {}
            other => return Err(format!("Expected FROM, got {:?}", other)),
        }
        let (user, host) = self.parse_user_spec()?;
        Ok(Statement::Revoke { privileges, object_type, object, user, host })
    }

    fn parse_returning(&mut self) -> Result<Option<Vec<SelectColumn>>, String> {
        if self.peek() != Some(&Token::Returning) {
            return Ok(None);
        }
        self.advance(); // consume RETURNING
        let mut cols = Vec::new();
        loop {
            if self.peek() == Some(&Token::Asterisk) {
                self.advance();
                cols.push(SelectColumn::All);
            } else {
                let name = self.expect_ident()?;
                if self.peek() == Some(&Token::As) {
                    self.advance();
                    let alias = self.expect_alias_ident()?;
                    cols.push(SelectColumn::ColumnAlias(name, alias));
                } else {
                    cols.push(SelectColumn::Column(name));
                }
            }
            if self.peek() == Some(&Token::Comma) { self.advance(); } else { break; }
        }
        Ok(Some(cols))
    }

    // ── MERGE INTO ──────────────────────────────────────────────
    fn parse_merge(&mut self) -> Result<Statement, String> {
        // MERGE INTO target [AS alias] USING source [AS alias] ON cond
        // WHEN MATCHED THEN UPDATE SET ...
        // WHEN NOT MATCHED THEN INSERT (cols) VALUES (vals)
        match self.advance() {
            Some(Token::Into) => {}
            other => return Err(format!("Expected INTO after MERGE, got {:?}", other)),
        }
        let target = self.expect_ident()?;
        let target_alias = if self.peek() == Some(&Token::As) {
            self.advance();
            Some(self.expect_alias_ident()?)
        } else if matches!(self.peek(), Some(Token::Ident(_))) {
            Some(self.expect_ident()?)
        } else { None };

        match self.advance() {
            Some(Token::Using) => {}
            other => return Err(format!("Expected USING, got {:?}", other)),
        }
        let source = self.expect_ident()?;
        let source_alias = if self.peek() == Some(&Token::As) {
            self.advance();
            Some(self.expect_alias_ident()?)
        } else if matches!(self.peek(), Some(Token::Ident(_))) {
            Some(self.expect_ident()?)
        } else { None };

        match self.advance() {
            Some(Token::On) => {}
            other => return Err(format!("Expected ON, got {:?}", other)),
        }
        let on = self.parse_condexpr()?;

        let mut when_matched_update: Option<Vec<(String, ArithExpr)>> = None;
        let mut when_matched_delete = false;
        let mut when_not_matched_columns: Option<Vec<String>> = None;
        let mut when_not_matched_values: Vec<String> = Vec::new();

        // Parse WHEN clauses (any order, up to 2)
        for _ in 0..4 {
            if !matches!(self.peek(), Some(Token::When)) {
                break;
            }
            self.advance(); // consume WHEN

            let not_matched = if self.peek() == Some(&Token::Not) {
                self.advance();
                true
            } else { false };

            match self.advance() {
                Some(Token::Matched) => {}
                other => return Err(format!("Expected MATCHED, got {:?}", other)),
            }
            match self.advance() {
                Some(Token::Then) => {}
                other => return Err(format!("Expected THEN, got {:?}", other)),
            }

            if not_matched {
                // WHEN NOT MATCHED THEN INSERT
                match self.advance() {
                    Some(Token::Insert) => {}
                    other => return Err(format!("Expected INSERT after WHEN NOT MATCHED THEN, got {:?}", other)),
                }
                when_not_matched_columns = if self.peek() == Some(&Token::LParen) {
                    self.advance();
                    let mut cols = Vec::new();
                    loop {
                        cols.push(self.expect_ident()?);
                        match self.peek() {
                            Some(Token::Comma)  => { self.advance(); }
                            Some(Token::RParen) => { self.advance(); break; }
                            other => return Err(format!("Expected ',' or ')' in INSERT cols, got {:?}", other)),
                        }
                    }
                    Some(cols)
                } else { None };
                match self.advance() {
                    Some(Token::Values) => {}
                    other => return Err(format!("Expected VALUES, got {:?}", other)),
                }
                match self.advance() {
                    Some(Token::LParen) => {}
                    other => return Err(format!("Expected '(' after VALUES, got {:?}", other)),
                }
                loop {
                    let val = self.parse_single_value()?;
                    when_not_matched_values.push(val);
                    match self.peek() {
                        Some(Token::Comma)  => { self.advance(); }
                        Some(Token::RParen) => { self.advance(); break; }
                        other => return Err(format!("Expected ',' or ')' in VALUES, got {:?}", other)),
                    }
                }
            } else {
                // WHEN MATCHED THEN UPDATE SET ... | DELETE
                match self.peek() {
                    Some(Token::Update) => {
                        self.advance();
                        match self.advance() {
                            Some(Token::Set) => {}
                            other => return Err(format!("Expected SET after UPDATE, got {:?}", other)),
                        }
                        let mut assignments = Vec::new();
                        loop {
                            let col = self.expect_col_ref()?;
                            match self.advance() {
                                Some(Token::Eq) => {}
                                other => return Err(format!("Expected = in assignment, got {:?}", other)),
                            }
                            let expr = self.parse_arith_expr()?;
                            assignments.push((col, expr));
                            if self.peek() == Some(&Token::Comma) { self.advance(); } else { break; }
                        }
                        when_matched_update = Some(assignments);
                    }
                    Some(Token::Delete) => {
                        self.advance();
                        when_matched_delete = true;
                    }
                    other => return Err(format!("Expected UPDATE or DELETE after WHEN MATCHED THEN, got {:?}", other)),
                }
            }
        }

        Ok(Statement::Merge {
            target, target_alias, source, source_alias, on,
            when_matched_update, when_matched_delete,
            when_not_matched_columns, when_not_matched_values,
        })
    }

    fn parse_single_value(&mut self) -> Result<String, String> {
        match self.advance() {
            Some(Token::StringLit(s)) => Ok(format!("'{}'", s)),
            Some(Token::NumberLit(n)) => Ok(n.clone()),
            Some(Token::Null)         => Ok("NULL".to_string()),
            Some(Token::Ident(s))     => {
                // could be alias.col
                let s = s.clone();
                if self.peek() == Some(&Token::Dot) {
                    self.advance();
                    let col = self.expect_ident()?;
                    Ok(format!("{}.{}", s, col))
                } else {
                    Ok(s)
                }
            }
            other => Err(format!("Expected value, got {:?}", other)),
        }
    }

    // ── CALL ────────────────────────────────────────────────────
    fn parse_call(&mut self) -> Result<Statement, String> {
        let name = self.expect_ident()?;
        let mut args = Vec::new();
        if self.peek() == Some(&Token::LParen) {
            self.advance();
            if self.peek() != Some(&Token::RParen) {
                loop {
                    args.push(self.parse_single_value()?);
                    if self.peek() == Some(&Token::Comma) { self.advance(); } else { break; }
                }
            }
            match self.advance() {
                Some(Token::RParen) => {}
                other => return Err(format!("Expected ')' after CALL args, got {:?}", other)),
            }
        }
        Ok(Statement::CallProcedure { name, args })
    }

    // ── CREATE PROCEDURE ────────────────────────────────────────
    fn parse_create_procedure(&mut self) -> Result<Statement, String> {
        let name = self.expect_ident()?;
        let mut params = Vec::new();
        if self.peek() == Some(&Token::LParen) {
            self.advance();
            while self.peek() != Some(&Token::RParen) {
                let dir = if matches!(self.peek(), Some(Token::In)) {
                    self.advance();
                    "IN".to_string()
                } else if let Some(Token::Ident(s)) = self.peek() {
                    let upper = s.to_uppercase();
                    if upper == "OUT" || upper == "INOUT" {
                        let result = upper.clone();
                        self.advance();
                        result
                    } else {
                        "IN".to_string()
                    }
                } else {
                    "IN".to_string()
                };
                let pname = self.expect_ident()?;
                let ptype = match self.advance() {
                    Some(Token::Int)      => "INT".to_string(),
                    Some(Token::Varchar)  => { if self.peek() == Some(&Token::LParen) { self.advance(); while self.peek() != Some(&Token::RParen) && self.peek().is_some() { self.advance(); } self.advance(); } "VARCHAR".to_string() }
                    Some(Token::Text)     => "TEXT".to_string(),
                    Some(Token::Float)    => "FLOAT".to_string(),
                    Some(Token::Date)     => "DATE".to_string(),
                    Some(Token::Datetime) => "DATETIME".to_string(),
                    Some(Token::Boolean)  => "BOOLEAN".to_string(),
                    Some(Token::Ident(s)) => s.clone(),
                    other => return Err(format!("Expected type in procedure param, got {:?}", other)),
                };
                params.push((dir, pname, ptype));
                if self.peek() == Some(&Token::Comma) { self.advance(); } else { break; }
            }
            match self.advance() {
                Some(Token::RParen) => {}
                other => return Err(format!("Expected ')' after params, got {:?}", other)),
            }
        }
        let body = self.parse_proc_body()?;
        Ok(Statement::CreateProcedure { name, params, body })
    }

    // ── CREATE FUNCTION ─────────────────────────────────────────
    fn parse_create_function(&mut self) -> Result<Statement, String> {
        // CREATE FUNCTION name(p1, p2, ...) RETURNS type RETURN <expr>
        let name = self.expect_ident()?.to_lowercase();
        let mut params = Vec::new();
        if self.peek() == Some(&Token::LParen) {
            self.advance();
            while self.peek() != Some(&Token::RParen) && self.peek().is_some() {
                params.push(self.expect_ident()?);
                if self.peek() == Some(&Token::Comma) { self.advance(); } else { break; }
            }
            match self.advance() {
                Some(Token::RParen) => {}
                other => return Err(format!("Expected ')' after params, got {:?}", other)),
            }
        }
        // skip optional RETURNS type (e.g. RETURNS VARCHAR(50))
        if let Some(Token::Ident(s)) = self.peek().cloned().as_ref() {
            if s.to_uppercase() == "RETURNS" {
                self.advance(); // RETURNS
                self.advance(); // type token (INT, VARCHAR, ...)
                // consume optional (N) — e.g. VARCHAR(50)
                if self.peek() == Some(&Token::LParen) {
                    self.advance();
                    while self.peek() != Some(&Token::RParen) && self.peek().is_some() {
                        self.advance();
                    }
                    if self.peek() == Some(&Token::RParen) { self.advance(); }
                }
            }
        }
        // RETURN <expr> — parse as ArithExpr and serialize to JSON
        if let Some(Token::Ident(s)) = self.peek().cloned().as_ref() {
            if s.to_uppercase() == "RETURN" {
                self.advance();
            }
        }
        let expr = self.parse_arith_expr()?;
        let body = serde_json::to_string(&expr).unwrap_or_default();
        Ok(Statement::CreateFunction { name, params, body })
    }

    fn parse_drop_function(&mut self) -> Result<Statement, String> {
        let if_exists = if self.peek() == Some(&Token::If) {
            self.advance();
            match self.advance() { Some(Token::Exists) => true, other => return Err(format!("Expected EXISTS, got {:?}", other)) }
        } else { false };
        let name = self.expect_ident()?.to_lowercase();
        Ok(Statement::DropFunction { name, if_exists })
    }

    // ── CREATE TRIGGER ──────────────────────────────────────────
    fn parse_create_trigger(&mut self) -> Result<Statement, String> {
        let name = self.expect_ident()?;
        let timing = match self.advance() {
            Some(Token::Before) => TriggerTiming::Before,
            Some(Token::After)  => TriggerTiming::After,
            other => return Err(format!("Expected BEFORE/AFTER, got {:?}", other)),
        };
        let event = match self.advance() {
            Some(Token::Insert) => TriggerEvent::Insert,
            Some(Token::Update) => TriggerEvent::Update,
            Some(Token::Delete) => TriggerEvent::Delete,
            other => return Err(format!("Expected INSERT/UPDATE/DELETE, got {:?}", other)),
        };
        match self.advance() {
            Some(Token::On) => {}
            other => return Err(format!("Expected ON, got {:?}", other)),
        }
        let table = self.expect_ident()?;
        // FOR EACH ROW
        if matches!(self.peek(), Some(Token::For)) { self.advance(); }
        if matches!(self.peek(), Some(Token::Each)) { self.advance(); }
        if matches!(self.peek(), Some(Token::Row))  { self.advance(); }
        let body = self.parse_proc_body()?;
        Ok(Statement::CreateTrigger { name, timing, event, table, body })
    }

    /// Parse BEGIN ... END block or a single statement as a procedure/trigger body
    fn parse_proc_body(&mut self) -> Result<Vec<Statement>, String> {
        match self.peek() {
            Some(Token::Ident(s)) if s.to_uppercase() == "BEGIN" => {
                self.advance();
                let stmts = self.parse_proc_stmts_until_end()?;
                Ok(stmts)
            }
            _ => {
                let s = self.parse_proc_stmt()?;
                Ok(vec![s])
            }
        }
    }

    /// Parse statements until END (consumes END token)
    fn parse_proc_stmts_until_end(&mut self) -> Result<Vec<Statement>, String> {
        let mut stmts = Vec::new();
        loop {
            // skip bare semicolons
            while self.peek() == Some(&Token::Semicolon) { self.advance(); }
            match self.peek() {
                Some(Token::End) => { self.advance(); break; }
                None => break,
                _ => {
                    let s = self.parse_proc_stmt()?;
                    stmts.push(s);
                    if self.peek() == Some(&Token::Semicolon) { self.advance(); }
                }
            }
        }
        Ok(stmts)
    }

    /// Parse one statement inside a procedure/trigger body (handles control flow)
    fn parse_proc_stmt(&mut self) -> Result<Statement, String> {
        // check for label: `label_name:`
        let label = self.try_parse_label();

        match self.peek() {
            Some(Token::Declare) => {
                self.advance();
                self.parse_proc_declare()
            }
            Some(Token::If) => {
                self.advance();
                self.parse_proc_if()
            }
            Some(Token::While) => {
                self.advance();
                self.parse_proc_while(label)
            }
            Some(Token::Loop) => {
                self.advance();
                self.parse_proc_loop(label)
            }
            Some(Token::Repeat) => {
                self.advance();
                self.parse_proc_repeat(label)
            }
            Some(Token::Leave) => {
                self.advance();
                let lbl = self.try_expect_ident();
                Ok(Statement::ProcLeave { label: lbl })
            }
            Some(Token::Iterate) => {
                self.advance();
                let lbl = self.try_expect_ident();
                Ok(Statement::ProcIterate { label: lbl })
            }
            Some(Token::Set) => {
                self.advance();
                // SET ISOLATION LEVEL or SET @var → fall to regular parse_set
                if matches!(self.peek(), Some(Token::Isolation) | Some(Token::At)) {
                    self.parse_set()
                } else {
                    self.parse_proc_set_var()
                }
            }
            _ => self.parse(),
        }
    }

    /// If next tokens are `Ident` followed by WHILE/LOOP/REPEAT, consume the ident as a label.
    /// (Lexer skips ':', so `label: WHILE` appears as Ident WHILE in token stream.)
    fn try_parse_label(&mut self) -> Option<String> {
        if let Some(Token::Ident(_)) = self.tokens.get(self.pos) {
            let next = self.tokens.get(self.pos + 1);
            if matches!(next, Some(Token::While) | Some(Token::Loop) | Some(Token::Repeat)) {
                if let Some(Token::Ident(s)) = self.advance() {
                    return Some(s.to_string());
                }
            }
        }
        None
    }

    /// Returns Some(ident) if next token is an identifier, None otherwise (no consume on None)
    fn try_expect_ident(&mut self) -> Option<String> {
        if let Some(Token::Ident(_)) = self.peek() {
            if let Some(Token::Ident(s)) = self.advance() {
                return Some(s.to_string());
            }
        }
        None
    }

    fn parse_proc_declare(&mut self) -> Result<Statement, String> {
        let name = self.expect_ident()?;
        // consume type (one or two tokens)
        let typ = match self.advance() {
            Some(Token::Int)      => "INT".to_string(),
            Some(Token::BigInt)   => "BIGINT".to_string(),
            Some(Token::TinyInt)  => "TINYINT".to_string(),
            Some(Token::SmallInt) => "SMALLINT".to_string(),
            Some(Token::Varchar)  => {
                if self.peek() == Some(&Token::LParen) {
                    self.advance();
                    while self.peek() != Some(&Token::RParen) && self.peek().is_some() { self.advance(); }
                    if self.peek() == Some(&Token::RParen) { self.advance(); }
                }
                "VARCHAR".to_string()
            }
            Some(Token::Text)     => "TEXT".to_string(),
            Some(Token::Float)    => "FLOAT".to_string(),
            Some(Token::Double)   => "DOUBLE".to_string(),
            Some(Token::Decimal)  => {
                if self.peek() == Some(&Token::LParen) {
                    self.advance();
                    while self.peek() != Some(&Token::RParen) && self.peek().is_some() { self.advance(); }
                    if self.peek() == Some(&Token::RParen) { self.advance(); }
                }
                "DECIMAL".to_string()
            }
            Some(Token::Boolean)  => "BOOLEAN".to_string(),
            Some(Token::Date)     => "DATE".to_string(),
            Some(Token::Datetime) => "DATETIME".to_string(),
            Some(Token::Ident(s)) => s.to_string(),
            other => return Err(format!("Expected type in DECLARE, got {:?}", other)),
        };
        // optional DEFAULT value (stored without quotes so proc_vars holds raw value)
        let default = if let Some(Token::Default) = self.peek() {
            self.advance();
            match self.advance() {
                Some(Token::StringLit(s)) => Some(s.clone()),
                Some(Token::NumberLit(n)) => Some(n.clone()),
                Some(Token::Null)         => Some("NULL".to_string()),
                Some(Token::Ident(s))     => Some(s.clone()),
                other => return Err(format!("Expected default value in DECLARE, got {:?}", other)),
            }
        } else {
            None
        };
        Ok(Statement::ProcDeclare { name, typ, default })
    }

    fn parse_proc_set_var(&mut self) -> Result<Statement, String> {
        let name = self.expect_ident()?;
        match self.advance() {
            Some(Token::Eq) => {}
            other => return Err(format!("Expected '=' in SET, got {:?}", other)),
        }
        let expr = self.parse_arith_expr()?;
        Ok(Statement::ProcSet { name, expr })
    }

    fn parse_proc_if(&mut self) -> Result<Statement, String> {
        // IF <cond> THEN <body> [ELSEIF <cond> THEN <body>]* [ELSE <body>] END IF
        let condition = self.parse_condexpr()?;
        match self.advance() {
            Some(Token::Then) => {}
            other => return Err(format!("Expected THEN after IF condition, got {:?}", other)),
        }
        let then_body = self.parse_proc_stmts_until_elseif_or_else_or_end()?;

        let mut elseif_branches = Vec::new();
        let mut else_body = None;

        loop {
            match self.peek() {
                Some(Token::ElseIfKw) => {
                    self.advance();
                    let cond = self.parse_condexpr()?;
                    match self.advance() {
                        Some(Token::Then) => {}
                        other => return Err(format!("Expected THEN after ELSEIF, got {:?}", other)),
                    }
                    let body = self.parse_proc_stmts_until_elseif_or_else_or_end()?;
                    elseif_branches.push((cond, body));
                }
                Some(Token::Else) => {
                    self.advance();
                    // parse until END, then consume END IF
                    let body = self.parse_proc_stmts_until_elseif_or_else_or_end()?;
                    else_body = Some(body);
                    // consume END IF
                    if let Some(Token::End) = self.peek() { self.advance(); }
                    if let Some(Token::If) = self.peek() { self.advance(); }
                    break;
                }
                Some(Token::End) => {
                    self.advance(); // consume END
                    if let Some(Token::If) = self.peek() { self.advance(); }
                    break;
                }
                _ => break,
            }
        }

        Ok(Statement::ProcIf { condition, then_body, elseif_branches, else_body })
    }

    /// Parse statements until ELSEIF / ELSE / END (does NOT consume that token)
    fn parse_proc_stmts_until_elseif_or_else_or_end(&mut self) -> Result<Vec<Statement>, String> {
        let mut stmts = Vec::new();
        loop {
            while self.peek() == Some(&Token::Semicolon) { self.advance(); }
            match self.peek() {
                Some(Token::ElseIfKw) | Some(Token::Else) | Some(Token::End) | None => break,
                _ => {
                    let s = self.parse_proc_stmt()?;
                    stmts.push(s);
                    if self.peek() == Some(&Token::Semicolon) { self.advance(); }
                }
            }
        }
        Ok(stmts)
    }

    fn parse_proc_while(&mut self, label: Option<String>) -> Result<Statement, String> {
        // WHILE <cond> DO <body> END WHILE
        let condition = self.parse_condexpr()?;
        match self.advance() {
            Some(Token::Do) => {}
            other => return Err(format!("Expected DO after WHILE condition, got {:?}", other)),
        }
        let body = self.parse_proc_stmts_until_end_while()?;
        Ok(Statement::ProcWhile { label, condition, body })
    }

    /// Parse statements until END WHILE (consumes both tokens)
    fn parse_proc_stmts_until_end_while(&mut self) -> Result<Vec<Statement>, String> {
        let mut stmts = Vec::new();
        loop {
            while self.peek() == Some(&Token::Semicolon) { self.advance(); }
            match self.peek() {
                Some(Token::End) => {
                    self.advance(); // consume END
                    if let Some(Token::While) = self.peek() { self.advance(); } // consume optional WHILE
                    break;
                }
                None => break,
                _ => {
                    let s = self.parse_proc_stmt()?;
                    stmts.push(s);
                    if self.peek() == Some(&Token::Semicolon) { self.advance(); }
                }
            }
        }
        Ok(stmts)
    }

    fn parse_proc_loop(&mut self, label: Option<String>) -> Result<Statement, String> {
        // LOOP <body> END LOOP
        let body = self.parse_proc_stmts_until_end_loop()?;
        Ok(Statement::ProcLoop { label, body })
    }

    fn parse_proc_stmts_until_end_loop(&mut self) -> Result<Vec<Statement>, String> {
        let mut stmts = Vec::new();
        loop {
            while self.peek() == Some(&Token::Semicolon) { self.advance(); }
            match self.peek() {
                Some(Token::End) => {
                    self.advance();
                    if let Some(Token::Loop) = self.peek() { self.advance(); }
                    break;
                }
                None => break,
                _ => {
                    let s = self.parse_proc_stmt()?;
                    stmts.push(s);
                    if self.peek() == Some(&Token::Semicolon) { self.advance(); }
                }
            }
        }
        Ok(stmts)
    }

    fn parse_proc_repeat(&mut self, label: Option<String>) -> Result<Statement, String> {
        // REPEAT <body> UNTIL <cond> END REPEAT
        let body = self.parse_proc_stmts_until_until()?;
        let until = self.parse_condexpr()?;
        // END REPEAT
        if let Some(Token::End) = self.peek() { self.advance(); }
        if let Some(Token::Repeat) = self.peek() { self.advance(); }
        Ok(Statement::ProcRepeat { label, body, until })
    }

    fn parse_proc_stmts_until_until(&mut self) -> Result<Vec<Statement>, String> {
        let mut stmts = Vec::new();
        loop {
            while self.peek() == Some(&Token::Semicolon) { self.advance(); }
            match self.peek() {
                Some(Token::Until) => { self.advance(); break; }
                None => break,
                _ => {
                    let s = self.parse_proc_stmt()?;
                    stmts.push(s);
                    if self.peek() == Some(&Token::Semicolon) { self.advance(); }
                }
            }
        }
        Ok(stmts)
    }

    // ── DROP TRIGGER / PROCEDURE ────────────────────────────────
    fn parse_drop_trigger(&mut self) -> Result<Statement, String> {
        let if_exists = if matches!(self.peek(), Some(Token::If)) {
            self.advance();
            match self.advance() {
                Some(Token::Exists) => {}
                other => return Err(format!("Expected EXISTS after IF, got {:?}", other)),
            }
            true
        } else { false };
        let name = self.expect_ident()?;
        Ok(Statement::DropTrigger { name, if_exists })
    }

    fn parse_drop_procedure(&mut self) -> Result<Statement, String> {
        let if_exists = if matches!(self.peek(), Some(Token::If)) {
            self.advance();
            match self.advance() {
                Some(Token::Exists) => {}
                other => return Err(format!("Expected EXISTS, got {:?}", other)),
            }
            true
        } else { false };
        let name = self.expect_ident()?;
        Ok(Statement::DropProcedure { name, if_exists })
    }
}