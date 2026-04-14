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
        other => other,
    }
}

fn expand_condition(cond: Condition, map: &HashMap<String, String>) -> Condition {
    Condition {
        column: expand_alias_str(&cond.column, map),
        operator: cond.operator,
        value: match cond.value {
            ConditionValue::Literal(s) =>
                ConditionValue::Literal(expand_alias_str(&s, map)),
            ConditionValue::Between(a, b) =>
                ConditionValue::Between(expand_alias_str(&a, map), expand_alias_str(&b, map)),
            other => other,
        },
        and: cond.and.map(|c| Box::new(expand_condition(*c, map))),
        or:  cond.or .map(|c| Box::new(expand_condition(*c, map))),
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
        let first = self.expect_ident()?;
        if self.peek() == Some(&Token::Dot) {
            self.advance(); // consume '.'
            let col = self.expect_ident()?;
            Ok(format!("{}.{}", first, col)) // table.column 전체 보존
        } else {
            Ok(first)
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
                    _ => self.parse_drop(),
                }
            }
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
            Some(Token::Explain) => {
                let inner = self.parse()?;
                Ok(Statement::Explain(Box::new(inner)))
            }
            Some(Token::Alter) => self.parse_alter(),
            Some(Token::Show)     => self.parse_show(),
            Some(Token::Describe) => self.parse_describe(),
            Some(Token::Truncate)    => self.parse_truncate(),
            Some(Token::Checkpoint)  => Ok(Statement::Checkpoint),
            Some(Token::Set)         => self.parse_set(),
            Some(Token::Vacuum)      => self.parse_vacuum(),
            other => Err(format!("Unknown statement: {:?}", other)),
        }
    }

    fn parse_condition(&mut self) -> Result<Condition, String> {
        // EXISTS (SELECT ...) — 컬럼 없이 시작
        if self.peek() == Some(&Token::Exists) {
            self.advance();
            let sub = self.parse_exists_subquery()?;
            let cond = Condition {
                column: String::new(), operator: Operator::Exists,
                value: ConditionValue::Subquery(Box::new(sub)),
                and: None, or: None,
            };
            return self.parse_condition_chain(cond);
        }

        // NOT EXISTS (SELECT ...) — NOT 토큰 다음 EXISTS
        if self.peek() == Some(&Token::Not) {
            // 한 칸 앞을 확인: NOT EXISTS 인 경우만 여기서 처리
            if self.tokens.get(self.pos + 1) == Some(&Token::Exists) {
                self.advance(); // NOT
                self.advance(); // EXISTS
                let sub = self.parse_exists_subquery()?;
                let cond = Condition {
                    column: String::new(), operator: Operator::NotExists,
                    value: ConditionValue::Subquery(Box::new(sub)),
                    and: None, or: None,
                };
                return self.parse_condition_chain(cond);
            }
            // 아닌 경우 (NOT NULL 등)는 아래 컬럼 파싱으로 진행
        }

        // HAVING 절에서 COUNT(*) > 1 같은 집계 함수 조건 처리
        let column = match self.peek() {
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
                format!("{}({})", label, inner)
            }
            _ => self.expect_col_ref()?,
        };

        // IN 처리
        if self.peek() == Some(&Token::In) {
            self.advance();
            match self.advance() {
                Some(Token::LParen) => {}
                other => return Err(format!("Expected '(', got {:?}", other)),
            }
            let sub_stmt = match self.advance() {
                Some(Token::Select) => self.parse_select()?,
                other => return Err(format!("Expected SELECT, got {:?}", other)),
            };
            match self.advance() {
                Some(Token::RParen) => {}
                other => return Err(format!("Expected ')', got {:?}", other)),
            }
            let cond = Condition {
                column, operator: Operator::In,
                value: ConditionValue::Subquery(Box::new(sub_stmt)),
                and: None, or: None,
            };
            return self.parse_condition_chain(cond);
        }

        // NOT IN (SELECT ...) 처리
        if self.peek() == Some(&Token::Not) {
            if self.tokens.get(self.pos + 1) == Some(&Token::In) {
                self.advance(); // NOT
                self.advance(); // IN
                match self.advance() {
                    Some(Token::LParen) => {}
                    other => return Err(format!("Expected '(' after NOT IN, got {:?}", other)),
                }
                let sub_stmt = match self.advance() {
                    Some(Token::Select) => self.parse_select()?,
                    other => return Err(format!("Expected SELECT in NOT IN subquery, got {:?}", other)),
                };
                match self.advance() {
                    Some(Token::RParen) => {}
                    other => return Err(format!("Expected ')' after NOT IN subquery, got {:?}", other)),
                }
                let cond = Condition {
                    column, operator: Operator::NotIn,
                    value: ConditionValue::Subquery(Box::new(sub_stmt)),
                    and: None, or: None,
                };
                return self.parse_condition_chain(cond);
            }
        }

        // BETWEEN 처리
        if self.peek() == Some(&Token::Between) {
            self.advance();
            let start = match self.advance() {
                Some(Token::NumberLit(n)) => n.clone(),
                Some(Token::Ident(s))     => s.clone(),
                other => return Err(format!("Expected value, got {:?}", other)),
            };
            match self.advance() {
                Some(Token::And) => {}
                other => return Err(format!("Expected AND, got {:?}", other)),
            }
            let end = match self.advance() {
                Some(Token::NumberLit(n)) => n.clone(),
                Some(Token::Ident(s))     => s.clone(),
                other => return Err(format!("Expected value, got {:?}", other)),
            };
            let cond = Condition {
                column, operator: Operator::Between,
                value: ConditionValue::Between(start, end),
                and: None, or: None,
            };
            return self.parse_condition_chain(cond);
        }

        // LIKE 처리
        if self.peek() == Some(&Token::Like) {
            self.advance();
            let pattern = match self.advance() {
                Some(Token::StringLit(s)) => s.clone(),
                Some(Token::Ident(s))     => s.clone(),
                other => return Err(format!("Expected pattern, got {:?}", other)),
            };
            let cond = Condition {
                column, operator: Operator::Like,
                value: ConditionValue::Literal(pattern),
                and: None, or: None,
            };
            return self.parse_condition_chain(cond);
        }

        let operator = match self.advance() {
            // IS NULL / IS NOT NULL
            Some(Token::Is) => {
                match self.peek() {
                    Some(Token::Not) => {
                        self.advance();
                        match self.advance() {
                            Some(Token::Null) => {
                                let cond = Condition {
                                    column,
                                    operator: Operator::IsNotNull,
                                    value: ConditionValue::Literal(String::new()),
                                    and: None,
                                    or: None,
                                };
                                return self.parse_condition_chain(cond);
                            }
                            other => return Err(format!("Expected NULL, got {:?}", other)),
                        }
                    }
                    Some(Token::Null) => {
                        self.advance();
                        let cond = Condition {
                            column,
                            operator: Operator::IsNull,
                            value: ConditionValue::Literal(String::new()),
                            and: None,
                            or: None,
                        };
                        return self.parse_condition_chain(cond);
                    }
                    other => return Err(format!("Expected NULL or NOT, got {:?}", other)),
                }
            }
            Some(Token::Eq)  => Operator::Eq,
            Some(Token::Ne)  => Operator::Ne,
            Some(Token::Gt)  => Operator::Gt,
            Some(Token::Lt)  => Operator::Lt,
            Some(Token::Gte) => Operator::Gte,
            Some(Token::Lte) => Operator::Lte,
            other => return Err(format!("Expected operator, got {:?}", other)),
        };

        let value = match self.peek() {
            Some(Token::LParen) => {
                // (SELECT ...) 형태의 서브쿼리
                self.advance(); // ( 소비
                match self.advance() {
                    Some(Token::Select) => {}
                    other => return Err(format!("Expected SELECT in subquery, got {:?}", other)),
                }
                let sub_stmt = self.parse_select()?;
                match self.advance() {
                    Some(Token::RParen) => {}
                    other => return Err(format!("Expected ')', got {:?}", other)),
                }
                ConditionValue::Subquery(Box::new(sub_stmt))
            }
            _ => match self.advance() {
                Some(Token::Ident(s)) => {
                    let s = s.clone();
                    // table.col 형태 지원 (상관 서브쿼리용)
                    if self.peek() == Some(&Token::Dot) {
                        self.advance(); // '.' 소비
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

        let cond = Condition { column, operator, value, and: None, or: None };
        self.parse_condition_chain(cond)
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

    fn parse_condition_chain(&mut self, mut cond: Condition) -> Result<Condition, String> {
        match self.peek() {
            Some(Token::And) => {
                self.advance();
                let next = self.parse_condition()?;
                cond.and = Some(Box::new(next));
            }
            Some(Token::Or) => {
                self.advance();
                let next = self.parse_condition()?;
                cond.or = Some(Box::new(next));
            }
            _ => {}
        }
        Ok(cond)
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
                Some(Token::Min)   | Some(Token::Max) => {
                    let func = match self.advance() {
                        Some(Token::Count) => AggFunc::Count,
                        Some(Token::Sum)   => AggFunc::Sum,
                        Some(Token::Avg)   => AggFunc::Avg,
                        Some(Token::Min)   => AggFunc::Min,
                        Some(Token::Max)   => AggFunc::Max,
                        _ => unreachable!(),
                    };
                    match self.advance() {
                        Some(Token::LParen) => {}
                        other => return Err(format!("Expected '(', got {:?}", other)),
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
                    // AS 별칭
                    if self.peek() == Some(&Token::As) {
                        self.advance();
                        let alias = self.expect_alias_ident()?;
                        SelectColumn::AggAlias { func, col: agg_col, alias }
                    } else {
                        SelectColumn::Agg { func, col: agg_col }
                    }
                }
                // SELECT 1, SELECT 'literal' — EXISTS 서브쿼리 등에서 사용
                Some(Token::NumberLit(_)) | Some(Token::StringLit(_)) => {
                    let val = match self.advance() {
                        Some(Token::NumberLit(n)) => n.clone(),
                        Some(Token::StringLit(s)) => s.clone(),
                        _ => unreachable!(),
                    };
                    SelectColumn::Column(val)
                }
                // 스칼라 함수: UPPER(col), NOW(), CONCAT(a, b), ...
                Some(Token::Upper) | Some(Token::Lower) | Some(Token::Length) |
                Some(Token::Trim)  | Some(Token::Concat) | Some(Token::Substr) |
                Some(Token::Substring) | Some(Token::Now) | Some(Token::Curdate) |
                Some(Token::DateFormat) | Some(Token::Coalesce) | Some(Token::Ifnull) |
                Some(Token::Replace) => {
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
                        _ => unreachable!(),
                    }.to_string();
                    let args = self.parse_func_args()?;
                    let alias = if self.peek() == Some(&Token::As) {
                        self.advance();
                        Some(self.expect_alias_ident()?)
                    } else { None };
                    SelectColumn::Func { name: fname, args, alias }
                }
                _ => {
                    let name = self.expect_col_ref()?;
                    if self.peek() == Some(&Token::As) {
                        self.advance();
                        let alias = self.expect_alias_ident()?;
                        SelectColumn::ColumnAlias(name, alias)
                    } else {
                        SelectColumn::Column(name)
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

        match self.advance() {
            Some(Token::From) => {}
            other => return Err(format!("Expected FROM, got {:?}", other)),
        }

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
            match self.advance() {
                Some(Token::As) => {}
                other => return Err(format!("Expected AS after subquery, got {:?}", other)),
            }
            let alias = self.expect_ident()?;
            (String::new(), Some((Box::new(inner), alias)))
        } else {
            let t = self.expect_ident()?;
            // 선택적 테이블 별칭: FROM employees e
            if matches!(self.peek(), Some(Token::Ident(_))) {
                let a = self.expect_ident()?;
                alias_map.insert(a, t.clone());
            }
            (t, None)
        };

        // JOIN / LEFT JOIN / RIGHT JOIN (다중 반복)
        let mut joins = Vec::new();
        loop {
            let join_type = match self.peek() {
                Some(Token::Join)  => { self.advance(); JoinType::Inner }
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
            // 선택적 JOIN 테이블 별칭: JOIN departments d ON ...
            if matches!(self.peek(), Some(Token::Ident(_))) {
                let a = self.expect_ident()?;
                alias_map.insert(a, join_table.clone());
            }
            match self.advance() {
                Some(Token::On) => {}
                other => return Err(format!("Expected ON, got {:?}", other)),
            }
            let left_col  = self.expect_col_ref()?;
            match self.advance() {
                Some(Token::Eq) => {}
                other => return Err(format!("Expected =, got {:?}", other)),
            }
            let right_col = self.expect_col_ref()?;
            joins.push(Join { table: join_table, left_col, right_col, join_type });
        }

        // WHERE
        let condition = if self.peek() == Some(&Token::Where) {
            self.advance();
            Some(self.parse_condition()?)
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
            Some(self.parse_condition()?)
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

        // LIMIT
        let limit = if self.peek() == Some(&Token::Limit) {
            self.advance();
            match self.advance() {
                Some(Token::NumberLit(n)) => Some(n.parse::<usize>().unwrap_or(0)),
                other => return Err(format!("Expected number, got {:?}", other)),
            }
        } else {
            None
        };

        // FOR UPDATE
        let for_update = if self.peek() == Some(&Token::For) {
            self.advance();
            match self.advance() {
                Some(Token::Update) => true,
                other => return Err(format!("Expected UPDATE after FOR, got {:?}", other)),
            }
        } else {
            false
        };

        // 별칭 확장 적용
        let columns: Vec<SelectColumn> = columns.into_iter()
            .map(|c| expand_select_column(c, &alias_map))
            .collect();
        let joins: Vec<Join> = joins.into_iter().map(|j| Join {
            table: j.table,
            left_col:  expand_alias_str(&j.left_col, &alias_map),
            right_col: expand_alias_str(&j.right_col, &alias_map),
            join_type: j.join_type,
        }).collect();
        let condition = condition.map(|c| expand_condition(c, &alias_map));
        let order_by: Vec<OrderBy> = order_by.into_iter().map(|o| OrderBy {
            column: expand_alias_str(&o.column, &alias_map),
            ascending: o.ascending,
        }).collect();
        let group_by = group_by.map(|cols| cols.into_iter()
            .map(|c| expand_alias_str(&c, &alias_map))
            .collect::<Vec<_>>());
        let having = having.map(|c| expand_condition(c, &alias_map));

        Ok(Statement::Select { table, subquery, distinct, columns, condition, joins, order_by, group_by, having, limit, for_update })
    }

    fn parse_insert(&mut self) -> Result<Statement, String> {
        // INSERT INTO table [(col1, col2, ...)] VALUES (v1, v2, ...) [, (v3, v4, ...) ...]
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

        match self.advance() {
            Some(Token::Values) => {}
            other => return Err(format!("Expected VALUES, got {:?}", other)),
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

        Ok(Statement::Insert { table, columns, values: all_values })
    }

    fn parse_update(&mut self) -> Result<Statement, String> {
        // UPDATE table SET col = val WHERE ...
        let table = self.expect_ident()?;
        match self.advance() {
            Some(Token::Set) => {}
            other => return Err(format!("Expected SET, got {:?}", other)),
        }

        let mut assignments = Vec::new();
        loop {
            let col = self.expect_ident()?;
            match self.advance() {
                Some(Token::Eq) => {}
                other => return Err(format!("Expected =, got {:?}", other)),
            }
            let val = match self.advance() {
                Some(Token::StringLit(s)) => s.clone(),
                Some(Token::NumberLit(n)) => n.clone(),
                Some(Token::Ident(s))     => s.clone(),
                other => return Err(format!("Expected value, got {:?}", other)),
            };
            assignments.push((col, val));
            if self.peek() == Some(&Token::Comma) { self.advance(); } else { break; }
        }

        let condition = if self.peek() == Some(&Token::Where) {
            self.advance();
            Some(self.parse_condition()?)
        } else {
            None
        };

        Ok(Statement::Update { table, assignments, condition })
    }

    fn parse_delete(&mut self) -> Result<Statement, String> {
        // DELETE FROM table WHERE ...
        match self.advance() {
            Some(Token::From) => {}
            other => return Err(format!("Expected FROM, got {:?}", other)),
        }
        let table = self.expect_ident()?;
        let condition = if self.peek() == Some(&Token::Where) {
            self.advance();
            Some(self.parse_condition()?)
        } else {
            None
        };
        Ok(Statement::Delete { table, condition })
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
            let arg = match self.peek() {
                Some(Token::RParen) => break,
                _ => match self.advance() {
                    Some(Token::StringLit(s)) => format!("'{}'", s),
                    Some(Token::NumberLit(n)) => n.clone(),
                    Some(Token::Asterisk)     => "*".to_string(),
                    Some(Token::Null)         => "NULL".to_string(),
                    Some(Token::Ident(s))     => {
                        let s = s.clone();
                        // table.col 형태 처리
                        if self.peek() == Some(&Token::Dot) {
                            self.advance();
                            let col = self.expect_ident()?;
                            format!("{}.{}", s, col)
                        } else {
                            s
                        }
                    }
                    other => return Err(format!("Expected function argument, got {:?}", other)),
                }
            };
            args.push(arg);
        }
        match self.advance() {
            Some(Token::RParen) => {}
            other => return Err(format!("Expected ')' after function args, got {:?}", other)),
        }
        Ok(args)
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

    /// 데이터 타입 파싱: INT, TEXT, FLOAT, BOOLEAN, VARCHAR(n), DATE, DECIMAL(p,s)
    fn parse_data_type(&mut self) -> Result<DataType, String> {
        match self.advance() {
            Some(Token::Int)     => Ok(DataType::Int),
            Some(Token::Text)    => Ok(DataType::Text),
            Some(Token::Float)   => Ok(DataType::Float),
            Some(Token::Boolean) => Ok(DataType::Boolean),
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
            Some(Token::Date) => Ok(DataType::Date),
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
                                Some(Token::Set) => {
                                    match p.advance() {
                                        Some(Token::Null) => Ok(FkAction::SetNull),
                                        other => Err(format!("Expected NULL, got {:?}", other)),
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

    fn parse_create(&mut self) -> Result<Statement, String> {
        match self.advance() {
            Some(Token::Table) => {}
            other => return Err(format!("Expected TABLE, got {:?}", other)),
        }

        // IF NOT EXISTS (IF, EXISTS 는 Ident로 들어옴)
        let if_not_exists = if matches!(self.peek(), Some(Token::Ident(s)) if s.eq_ignore_ascii_case("IF")) {
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
                    other => return Err(format!("Expected UNIQUE or CHECK after CONSTRAINT name, got {:?}", other)),
                }
            } else if self.peek() == Some(&Token::Foreign) {
                // 테이블 레벨: FOREIGN KEY (col) REFERENCES ref_table(ref_col) [ON DELETE ...] [ON UPDATE ...]
                self.advance(); // FOREIGN
                match self.advance() {
                    Some(Token::Key) => {}
                    other => return Err(format!("Expected KEY after FOREIGN, got {:?}", other)),
                }
                match self.advance() {
                    Some(Token::LParen) => {}
                    other => return Err(format!("Expected '(' after FOREIGN KEY, got {:?}", other)),
                }
                let fk_col = self.expect_ident()?;
                match self.advance() {
                    Some(Token::RParen) => {}
                    other => return Err(format!("Expected ')' after FK column, got {:?}", other)),
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
                    self.advance(); // ON
                    let parse_fk_action = |p: &mut Parser| -> Result<FkAction, String> {
                        match p.advance() {
                            Some(Token::Cascade)  => Ok(FkAction::Cascade),
                            Some(Token::Restrict) => Ok(FkAction::Restrict),
                            Some(Token::Set) => {
                                match p.advance() {
                                    Some(Token::Null) => Ok(FkAction::SetNull),
                                    other => Err(format!("Expected NULL, got {:?}", other)),
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
                // 해당 컬럼에 FK 설정
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
        // IF EXISTS 처리 (IF, EXISTS 는 Ident로 들어옴)
        let if_exists = if matches!(self.peek(), Some(Token::Ident(s)) if s.eq_ignore_ascii_case("IF")) {
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
                match self.advance() {
                    Some(Token::Column) => {}
                    other => return Err(format!("Expected COLUMN, got {:?}", other)),
                }
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
            Some(Token::Drop) => {
                match self.advance() {
                    Some(Token::Column) => {}
                    other => return Err(format!("Expected COLUMN, got {:?}", other)),
                }
                let col_name = self.expect_ident()?;
                Ok(Statement::AlterTable {
                    table,
                    action: AlterAction::DropColumn(col_name),
                })
            }
            Some(Token::Rename) => {
                match self.advance() {
                    Some(Token::Column) => {}
                    other => return Err(format!("Expected COLUMN, got {:?}", other)),
                }
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
        if matches!(self.peek(), Some(Token::Ident(s)) if s.eq_ignore_ascii_case("IF")) {
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
        if matches!(self.peek(), Some(Token::Ident(s)) if s.eq_ignore_ascii_case("IF")) {
            self.advance(); // IF
            match self.advance() {
                Some(Token::Exists) => {}
                other => return Err(format!("Expected EXISTS after IF, got {:?}", other)),
            }
        }
        let name = self.expect_ident()?;
        Ok(Statement::DropView { name })
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
            other => Err(format!("Expected TABLES, BUFFER, WAL, ISOLATION, or LOCKS, got {:?}", other)),
        }
    }

    fn parse_set(&mut self) -> Result<Statement, String> {
        match self.advance() {
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
}