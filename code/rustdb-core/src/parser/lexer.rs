// src/parser/lexer.rs

#[derive(Debug, PartialEq, Clone)]
pub enum Token {
    // 키워드
    Select, From, Where, Insert, Into, Values,
    Update, Set, Delete, Create, Table, Drop,
    Join, Left, Right, On, And, Or, Not,
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
            _           => Token::Ident(s),
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