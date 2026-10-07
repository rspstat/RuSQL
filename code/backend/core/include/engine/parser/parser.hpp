#pragma once

// Faithful port of rusql-core/src/parser/parser.rs (91 functions, hand-written
// recursive-descent parser).
//
// Design deviation from the general cookbook: internally, parsing functions return
// plain values and throw ParseError on failure, rather than threading
// engine::Result<T,std::string> through every call site. Rust's `?` operator
// propagates ~500+ error sites in this file for free; hand-rolling that propagation
// with Result<T> at every site would balloon the code several-fold and make it easy
// to silently drop an error check. Only the public entry point (Parser::parse())
// catches ParseError and converts it to a Result<Statement,std::string>, matching the
// Rust function's public signature `pub fn parse(&mut self) -> Result<Statement, String>`.

#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include "engine/parser/ast.hpp"
#include "engine/parser/lexer.hpp"
#include "engine/result.hpp"

namespace engine {

// Mirrors parser.rs's `pub const NULL_DEFAULT: &str = "__NULL_DEFAULT__";`
extern const std::string NULL_DEFAULT;

// An INSERT value that was left out -- `(1, , 3)`, the DEFAULT keyword, a column missing from the column list -- takes the column's
// default. It is not the empty string '', which is a value like any other (it used to be stored as NULL).
extern const std::string INSERT_DEFAULT;

class ParseError : public std::runtime_error {
public:
    explicit ParseError(const std::string& message) : std::runtime_error(message) {}
};

// Bundles parse_col_constraints' output (Rust used 8 out-params by mutable reference;
// a struct is the more idiomatic C++ analogue of the same shape).
struct ColConstraints {
    bool primary_key = false;
    bool not_null = false;
    bool unique = false;
    std::optional<std::string> unique_constraint_name;
    bool auto_increment = false;
    std::optional<std::string> default_value;
    std::optional<ForeignKey> foreign_key;
    std::optional<std::string> check_expr;
};

class Parser {
public:
    explicit Parser(const std::string& input);

    // Public API: mirrors `pub fn parse(&mut self) -> Result<Statement, String>`.
    Result<Statement, std::string> parse();

    // pub(crate) in the Rust original; used by other modules (e.g. the executor) to
    // parse a standalone arithmetic expression / stringify one.
    ArithExpr parse_arith_expr();
    static std::string arith_to_string(const ArithExpr& expr);
    /// The text of a condition, written so that it parses back to the same condition (throws ParseError for one with a subquery).
    static std::string cond_to_string(const CondExpr& cond);
    static std::string aggregate_argument_text(const ArithExpr& arg);
    static ArithExpr str_to_arith(const std::string& s);

private:
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    // The table names and aliases of the FROM list of each SELECT being parsed (the innermost last): a subquery that uses one of those tables
    // under an alias keeps the alias, which would otherwise be replaced by the table name -- also what the query around it calls its table.
    std::vector<std::unordered_set<std::string>> enclosing_from_;
    /// The table names and aliases of the FROM list that follows (read ahead, from the position after SELECT)
    std::unordered_set<std::string> scan_from_names() const;

    /// A keyword that MySQL does not reserve (DATE, LEVEL, COUNT, USER, YEAR, TEXT ...): where a name is expected it is one (a column called `date`).
    /// Its name is the word as typed, in lower case (Token::word).
    static bool is_plain_word(TokenKind kind);
    /// CURRENT_TIMESTAMP, SYSDATE, LOCALTIME, CURRENT_DATE ... are values without parentheses; `now` and `curdate` need them (without, they are columns)
    static bool is_bare_function(const Token& t);

    const Token* peek() const;
    const Token* peek_at(std::size_t offset) const;
    const Token* advance();

    bool peek_is(TokenKind k) const;
    bool peek_at_is(std::size_t offset, TokenKind k) const;

    // The internal (exception-throwing) statement dispatcher. `parse()` wraps this.
    Statement parse_stmt();

    std::string expect_ident();
    std::string expect_alias_ident();
    /// `[AS] alias` after a table name, if there is one
    std::optional<std::string> parse_table_alias();
    std::string expect_col_ref();
    std::string expect_any_name();
    std::string expect_any_ident();

    Statement parse_use();
    Statement parse_with();

    CondExpr parse_condexpr();
    CondExpr parse_or_expr();
    CondExpr parse_and_expr();
    CondExpr parse_not_expr();
    CondExpr parse_primary_cond();
    /// one predicate: EXISTS, `expr OP value`, `expr IS NULL`, ..., or (no operator) an expression that is true when it is not NULL and not 0
    CondExpr parse_pred_expr();
    /// the operator and right side after an expression: `> 5`, `IS [NOT] NULL|TRUE|FALSE`, `[NOT] BETWEEN`, `[NOT] IN`, `[NOT] LIKE`, REGEXP
    CondExpr parse_pred_cond(ArithExpr left);
    Condition parse_pred_tail(ArithExpr left);
    Statement parse_exists_subquery();
    /// the next token starts a predicate operator (`>`, `IS`, `NOT IN`, `BETWEEN`, ...) / an operator that goes on with an expression (also `+`, `||`)
    bool at_pred_operator() const;
    bool at_value_continuation() const;
    /// the token `offset` ahead goes on with an arithmetic expression (+ - * / % || -> ->>)
    bool arith_continues_at(std::size_t offset) const;
    /// the IN list that starts at token `from` holds only numbers, strings, NULL and @variables (one condition); anything else is an expression
    bool in_list_is_literals(std::size_t from) const;
    bool select_item_continues() const;
    /// An item of ORDER BY / GROUP BY: a column (its name), a position (`2`), or an expression (the text of it: `a + b`, `COUNT(*)`, `YEAR(d)`)
    std::string parse_sort_item();

    /// A value expression: arithmetic, functions, CASE, and conditions as values (`v > 5`, `a AND b`, `v IS NULL`; 1, 0 or NULL).
    ArithExpr parse_value_expr();
    ArithExpr parse_value_and();
    ArithExpr parse_value_not();
    ArithExpr parse_value_pred();
    ArithExpr parse_case_expr();
    ArithExpr parse_if_expr();
    ArithExpr parse_cast_expr();
    std::string parse_cast_type();

    static std::string condition_text(const ArithExpr& when);

    ArithExpr parse_arith_factor();
    ArithExpr parse_arith_term();

    std::optional<WindowFrame> parse_window_frame();

    Statement parse_select();
    Statement parse_insert();
    std::vector<std::vector<std::string>> parse_insert_values();
    Statement parse_replace();
    Statement parse_update();
    Statement parse_delete();

    std::string read_parenthesized_expr();

    DataType parse_data_type();
    ColConstraints parse_col_constraints(const std::string& col_name);
    void parse_fk_table_level(std::vector<ColumnDef>& columns);
    FkAction parse_fk_action();
    std::optional<PartitionBy> parse_partition_by();
    // PostgreSQL's `FILTER (WHERE cond)` clause on an aggregate -- no Rust original.
    // Returns nullopt (consuming nothing) if the next token isn't FILTER. Shared by the
    // main aggregate dispatch and GROUP_CONCAT's separate branch (parse_select).
    std::optional<CondExpr> parse_optional_filter_clause();
    // Parses the "LESS THAN (bound) / LESS THAN MAXVALUE" (RANGE) or "IN (v1, v2, ...)"
    // (LIST) tail of a single partition definition into `def` -- callers have already
    // consumed "PARTITION name VALUES". Shared by parse_partition_by's own list and by
    // ALTER TABLE ... ADD PARTITION (parse_alter).
    void parse_partition_values(PartitionKind kind, PartitionDef& def);

    Statement parse_create();
    Statement parse_drop();
    Statement parse_alter();
    Statement parse_create_index();
    Statement parse_drop_index();
    Statement parse_create_view();
    Statement parse_drop_view();
    Statement parse_create_database();
    Statement parse_drop_database();
    Statement parse_backup();
    Statement parse_restore();
    Statement parse_show();
    Statement parse_set();
    Statement parse_lock_tables();
    Statement parse_prepare();
    Statement parse_execute();
    Statement parse_deallocate();
    Statement parse_describe();
    Statement parse_truncate();
    Statement parse_vacuum();

    std::pair<std::string, std::string> parse_user_spec();
    Statement parse_create_user();
    Statement parse_drop_user();
    Statement parse_grant();
    std::string parse_grant_object();
    Statement parse_revoke();

    std::optional<std::vector<SelectColumn>> parse_returning();
    Statement parse_merge();
    std::string parse_single_value();
    Statement parse_call();

    Statement parse_create_procedure();
    Statement parse_create_function();
    Statement parse_drop_function();
    Statement parse_create_trigger();

    std::vector<Statement> parse_proc_body();
    std::vector<Statement> parse_proc_stmts_until_end();
    Statement parse_proc_stmt();
    std::optional<std::string> try_parse_label();
    std::optional<std::string> try_expect_ident();
    Statement parse_proc_declare();
    Statement parse_proc_set_var();
    Statement parse_proc_if();
    std::vector<Statement> parse_proc_stmts_until_elseif_or_else_or_end();
    Statement parse_proc_while(std::optional<std::string> label);
    std::vector<Statement> parse_proc_stmts_until_end_while();
    Statement parse_proc_loop(std::optional<std::string> label);
    std::vector<Statement> parse_proc_stmts_until_end_loop();
    Statement parse_proc_repeat(std::optional<std::string> label);
    std::vector<Statement> parse_proc_stmts_until_until();

    Statement parse_drop_trigger();
    Statement parse_drop_procedure();
};

} // namespace engine
