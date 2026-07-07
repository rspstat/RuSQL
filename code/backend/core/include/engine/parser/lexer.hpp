#pragma once

// Faithful port of rusql-core/src/parser/lexer.rs.
//
// Token is represented as a {kind, text} pair rather than a variant-of-structs: unlike
// the AST (heterogeneous payload shapes per variant), almost every Token variant here
// carries no payload at all — only Ident/StringLit/NumberLit do, and all three share
// the same "one string" shape — so a single TokenKind enum + one optional text field
// is the more direct (and more efficient) analogue of Rust's Token enum here.
//
// Known divergence: Rust's Lexer operates on `char` (Unicode scalar values); this port
// operates on bytes (ASCII semantics via <cctype>). This only matters for non-ASCII
// identifiers/string contents, which the project's test suite (test_full.sql) does not
// exercise.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace engine {

enum class TokenKind {
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
    Ident, StringLit, NumberLit,

    // FK 제약조건
    Cascade, Restrict,

    // NOT NULL
    Is,

    // 체크포인트
    Checkpoint,

    // 격리 수준
    Isolation, Level, Uncommitted, Committed, Repeatable, Serializable,

    // MVCC
    Vacuum,

    // Row-level locking
    For, Locks,

    // DISTINCT
    Distinct,

    // DEFAULT 값
    Default,

    // ALTER TABLE MODIFY
    Modify,

    // EXISTS / NOT EXISTS
    Exists,

    // SAVEPOINT
    Savepoint, Release,

    // EXPLAIN
    Explain,

    // 새 데이터 타입
    Varchar, Date, Datetime, Timestamp, Decimal, Double, Time, Year, Blob, Enum,

    // DATABASE
    Database,

    // INNER JOIN
    Inner,

    // CHECK 제약
    Check,

    // 스칼라 함수
    Upper, Lower, Length, Trim, Concat, Substr, Substring,
    Now, Curdate, DateFormat, Coalesce, Ifnull, Replace,

    // 수학 함수
    Round, Abs, Ceil, Floor, Mod,

    // INTERVAL
    Interval,

    // CASE WHEN
    Case, When, Then, Else, End,

    // UNION / INTERSECT / EXCEPT
    Union, Intersect, Except, All,

    // IF()
    If,

    // 새 함수/키워드
    GroupConcat, Ignore, Duplicate, Nullif, Lpad, Rpad, Cast, DateAdd, DateSub, DateDiff, Separator,

    // 산술 연산자
    Plus, Minus, Slash,

    // OFFSET
    Offset,

    // CTE
    With, Recursive,

    // USE DATABASE
    Use,

    // CREATE USER / GRANT / REVOKE
    User, Identified, Grant, Revoke, Privileges, Grants, OptionKw, At, Password, Databases,

    // EXPLAIN ANALYZE
    Analyze,

    // 윈도우 함수
    RowNumber, Rank, DenseRank, Lag, Lead, Over, Partition, FirstValue, LastValue, NthValue,

    // FULL OUTER JOIN
    Full,

    // RETURNING
    Returning,

    // 통계 집계 함수
    Stddev, Variance,

    // 윈도우 함수 (추가)
    Ntile, PercentRank, CumeDist,

    // 정규식
    Regexp,

    // 윈도우 프레임
    Rows, Range, Unbounded, Preceding, Following, Current,

    // MERGE INTO
    Merge, Using, Matched, Procedure, Call, Trigger, Before, After, Each, Row, NewKw, OldKw,
    Body, Signal, Declare, Handler, Leave, Iterate, Loop, Repeat, Until, While, Do, IfKw, ElseIfKw,

    // 정수 타입 확장
    BigInt, SmallInt, TinyInt,

    // JSON
    Json, JsonExtract, JsonUnquote, JsonValue,

    // FOR SHARE
    Share,

    // JSON 연산자
    Arrow, LongArrow,

    // PREPARE / EXECUTE / DEALLOCATE
    Prepare, Execute, Deallocate,

    // FETCH FIRST n ROWS ONLY
    Fetch, Next, Only,

    // ROLE
    Role,

    // SYNONYM
    Synonym,

    // Modulo operator
    Percent,

    // String concatenation (||)
    PipePipe,
};

struct Token {
    TokenKind kind;
    std::string text; // populated only for Ident / StringLit / NumberLit

    Token(TokenKind k) : kind(k) {}
    Token(TokenKind k, std::string t) : kind(k), text(std::move(t)) {}

    bool operator==(const Token& other) const { return kind == other.kind && text == other.text; }
    bool operator!=(const Token& other) const { return !(*this == other); }
};

class Lexer {
public:
    explicit Lexer(const std::string& input);

    std::vector<Token> tokenize();

private:
    std::optional<char> peek() const;
    std::optional<char> advance();
    void skip_whitespace();

    Token read_string();
    Token read_quoted_ident(char closing);
    Token read_number();
    Token read_ident();

    std::string input_;
    std::size_t pos_ = 0;
};

} // namespace engine
