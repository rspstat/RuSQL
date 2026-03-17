// src/parser/parser.rs

use crate::parser::lexer::{Lexer, Token};
use crate::parser::ast::*;

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
            Some(Token::Ident(s)) if s == "ROLLBACK" => Ok(Statement::Rollback),
            Some(Token::Alter) => self.parse_alter(),
            other => Err(format!("Unknown statement: {:?}", other)),
        }
    }

    fn parse_condition(&mut self) -> Result<Condition, String> {
        let column = self.expect_ident()?;

        // IN (subquery) 처리
        if self.peek() == Some(&Token::In) {
            self.advance();
            match self.advance() {
                Some(Token::LParen) => {}
                other => return Err(format!("Expected '(', got {:?}", other)),
            }
            // 서브쿼리 파싱
            let sub_stmt = match self.advance() {
                Some(Token::Select) => self.parse_select()?,
                other => return Err(format!("Expected SELECT in subquery, got {:?}", other)),
            };
            match self.advance() {
                Some(Token::RParen) => {}
                other => return Err(format!("Expected ')', got {:?}", other)),
            }
            return Ok(Condition {
                column,
                operator: Operator::In,
                value: ConditionValue::Subquery(Box::new(sub_stmt)),
            });
        }

        let operator = match self.advance() {
            Some(Token::Eq)  => Operator::Eq,
            Some(Token::Ne)  => Operator::Ne,
            Some(Token::Gt)  => Operator::Gt,
            Some(Token::Lt)  => Operator::Lt,
            Some(Token::Gte) => Operator::Gte,
            Some(Token::Lte) => Operator::Lte,
            other => return Err(format!("Expected operator, got {:?}", other)),
        };

        let value = match self.advance() {
            Some(Token::Ident(s))     => ConditionValue::Literal(s.clone()),
            Some(Token::NumberLit(n)) => ConditionValue::Literal(n.clone()),
            Some(Token::StringLit(s)) => ConditionValue::Literal(s.clone()),
            other => return Err(format!("Expected value, got {:?}", other)),
        };

        Ok(Condition { column, operator, value })
    }

    fn parse_select(&mut self) -> Result<Statement, String> {
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
                        let col = match self.advance() {
                            Some(Token::Asterisk)  => "*".to_string(),
                            Some(Token::Ident(s))  => s.clone(),
                            other => return Err(format!("Expected column, got {:?}", other)),
                        };
                        match self.advance() {
                            Some(Token::RParen) => {}
                            other => return Err(format!("Expected ')', got {:?}", other)),
                        }
                        SelectColumn::Agg { func, col }
                    }
                    _ => SelectColumn::Column(self.expect_ident()?),
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
        let table = self.expect_ident()?;

        // JOIN
        let join = if self.peek() == Some(&Token::Join) {
            self.advance();
            let join_table = self.expect_ident()?;
            match self.advance() {
                Some(Token::On) => {}
                other => return Err(format!("Expected ON, got {:?}", other)),
            }
            let left_col  = self.expect_ident()?;
            match self.advance() {
                Some(Token::Eq) => {}
                other => return Err(format!("Expected =, got {:?}", other)),
            }
            let right_col = self.expect_ident()?;
            Some(Join { table: join_table, left_col, right_col })
        } else {
            None
        };

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
            let mut cols = vec![self.expect_ident()?];
            while self.peek() == Some(&Token::Comma) {
                self.advance();
                cols.push(self.expect_ident()?);
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

        // ORDER BY
        let order_by = if self.peek() == Some(&Token::Order) {
            self.advance();
            match self.advance() {
                Some(Token::By) => {}
                other => return Err(format!("Expected BY, got {:?}", other)),
            }
            let col = self.expect_ident()?;
            let ascending = match self.peek() {
                Some(Token::Desc) => { self.advance(); false }
                Some(Token::Asc)  => { self.advance(); true  }
                _ => true,
            };
            Some(OrderBy { column: col, ascending })
        } else {
            None
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

        Ok(Statement::Select { table, columns, condition, join, order_by, group_by, having, limit })
    }

    fn parse_insert(&mut self) -> Result<Statement, String> {
        // INSERT INTO table VALUES (v1, v2, ...)
        match self.advance() {
            Some(Token::Into) => {}
            other => return Err(format!("Expected INTO, got {:?}", other)),
        }
        let table = self.expect_ident()?;
        match self.advance() {
            Some(Token::Values) => {}
            other => return Err(format!("Expected VALUES, got {:?}", other)),
        }
        match self.advance() {
            Some(Token::LParen) => {}
            other => return Err(format!("Expected '(', got {:?}", other)),
        }

        let mut values = Vec::new();
        loop {
            let val = match self.advance() {
                Some(Token::StringLit(s)) => s.clone(),
                Some(Token::NumberLit(n)) => n.clone(),
                Some(Token::Ident(s))     => s.clone(),
                other => return Err(format!("Expected value, got {:?}", other)),
            };
            values.push(val);
            match self.peek() {
                Some(Token::Comma)  => { self.advance(); }
                Some(Token::RParen) => { self.advance(); break; }
                other => return Err(format!("Expected ',' or ')', got {:?}", other)),
            }
        }

        Ok(Statement::Insert { table, values })
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

    fn parse_create(&mut self) -> Result<Statement, String> {
        // CREATE TABLE name (col1 TYPE, col2 TYPE, ...)
        match self.advance() {
            Some(Token::Table) => {}
            other => return Err(format!("Expected TABLE, got {:?}", other)),
        }
        let name = self.expect_ident()?;
        match self.advance() {
            Some(Token::LParen) => {}
            other => return Err(format!("Expected '(', got {:?}", other)),
        }

        let mut columns = Vec::new();
        loop {
            let col_name = self.expect_ident()?;
            let data_type = match self.advance() {
                Some(Token::Int)     => DataType::Int,
                Some(Token::Text)    => DataType::Text,
                Some(Token::Float)   => DataType::Float,
                Some(Token::Boolean) => DataType::Boolean,
                other => return Err(format!("Expected data type, got {:?}", other)),
            };
            columns.push(ColumnDef { name: col_name, data_type });
            match self.peek() {
                Some(Token::Comma)  => { self.advance(); }
                Some(Token::RParen) => { self.advance(); break; }
                other => return Err(format!("Expected ',' or ')', got {:?}", other)),
            }
        }

        Ok(Statement::CreateTable { name, columns })
    }

    fn parse_drop(&mut self) -> Result<Statement, String> {
        // DROP TABLE name
        match self.advance() {
            Some(Token::Table) => {}
            other => return Err(format!("Expected TABLE, got {:?}", other)),
        }
        let name = self.expect_ident()?;
        Ok(Statement::DropTable { name })
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
                let data_type = match self.advance() {
                    Some(Token::Int)     => DataType::Int,
                    Some(Token::Text)    => DataType::Text,
                    Some(Token::Float)   => DataType::Float,
                    Some(Token::Boolean) => DataType::Boolean,
                    other => return Err(format!("Expected data type, got {:?}", other)),
                };
                Ok(Statement::AlterTable {
                    table,
                    action: AlterAction::AddColumn(ColumnDef { name: col_name, data_type }),
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
            other => Err(format!("Expected ADD, DROP, or RENAME, got {:?}", other)),
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
        let column = self.expect_ident()?;
        match self.advance() {
            Some(Token::RParen) => {}
            other => return Err(format!("Expected ')', got {:?}", other)),
        }
        Ok(Statement::CreateIndex { index_name, table, column })
    }

    fn parse_drop_index(&mut self) -> Result<Statement, String> {
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
        let name = self.expect_ident()?;
        Ok(Statement::DropView { name })
    }
}