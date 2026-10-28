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

#include <map>
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
    std::vector<ForeignKey> foreign_keys;
    std::optional<std::string> check_expr;
    bool on_update_now = false;
};

class Parser {
public:
    // `||` is the logical OR in MySQL's default mode and the concatenation only when sql_mode has PIPES_AS_CONCAT (or ANSI): `pipes_as_concat`.
    explicit Parser(const std::string& input, bool pipes_as_concat = false);

    // Public API: mirrors `pub fn parse(&mut self) -> Result<Statement, String>`.
    Result<Statement, std::string> parse();

    /// The text had no token at all (it was empty, or only comments): there is no statement to run.
    bool has_tokens() const { return !tokens_.empty(); }

    // pub(crate) in the Rust original; used by other modules (e.g. the executor) to
    // parse a standalone arithmetic expression / stringify one.
    ArithExpr parse_arith_expr();
    static std::string arith_to_string(const ArithExpr& expr);
    /// The text of a condition, written so that it parses back to the same condition (throws ParseError for one with a subquery).
    static std::string cond_to_string(const CondExpr& cond);
    static std::string aggregate_argument_text(const ArithExpr& arg);
    static ArithExpr str_to_arith(const std::string& s);
    /// The column that the type a routine declares ("DECIMAL(8,2)", "INT UNSIGNED") stands for, to make the values of a parameter or a variable fit it
    /// (nullopt for a type that is not read)
    static std::optional<ColumnDef> declared_column(const std::string& name, const std::string& type_text);

private:
    std::vector<Token> tokens_;
    std::string source_; // the text the tokens were read from
    // the text of the tokens [from, to) as it was typed (a result column that is an expression is called by it)
    std::string source_between(std::size_t from, std::size_t to) const;
    std::size_t pos_ = 0;
    bool last_type_unsigned_ = false; // whether the integer type parse_data_type() read last had UNSIGNED after it
    // CREATE TABLE names the foreign keys it is not given a name for <table>_ibfk_1, _2 ... in the order they are written: the table (without its database) while the
    // columns are parsed, and how many such keys there are so far (empty: not inside CREATE TABLE)
    std::string fk_table_;
    int fk_unnamed_ = 0;
    std::string unnamed_fk_name(); // the next <table>_ibfk_<n> ("" outside CREATE TABLE: ALTER TABLE chooses its own)
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
    /// `=` or `:=`, where a value is assigned (SET, UPDATE ... SET, INSERT ... SET, ON DUPLICATE KEY UPDATE)
    bool peek_is_assign() const { return peek_is(TokenKind::Eq) || peek_is(TokenKind::ColonEq); }
    bool peek_at_is(std::size_t offset, TokenKind k) const;

    // The internal (exception-throwing) statement dispatcher. `parse()` wraps this.
    Statement parse_stmt();

    std::string expect_ident();
    std::string expect_table_name(); // name or db.name
    std::string expect_alias_ident();
    /// `[AS] alias` after a table name, if there is one
    std::optional<std::string> parse_table_alias();
    /// `[AS] alias` after a select item (`COUNT(*) c`, `a x`, `UPPER(s) AS u`), if there is one: without AS only a plain identifier is an alias
    std::optional<std::string> parse_column_alias();
    std::string expect_col_ref();
    std::string expect_any_name();
    std::string expect_any_ident();

    Statement parse_use();
    Statement parse_with();
    /// DETERMINISTIC, NOT DETERMINISTIC, NO SQL, CONTAINS SQL, READS SQL DATA, MODIFIES SQL DATA, LANGUAGE SQL, COMMENT 'x', SQL SECURITY x between a
    /// routine's parameters (or return type) and its body: no effect here
    void skip_routine_characteristics();
    /// `ALGORITHM = x`, `DEFINER = user@host`, `SQL SECURITY DEFINER|INVOKER` between CREATE and VIEW / PROCEDURE / FUNCTION / TRIGGER (a dump writes them; no effect here)
    void skip_create_attributes();
    /// `[DEFAULT] CHARACTER SET|CHARSET [=] x` / `[DEFAULT] COLLATE [=] x` after a table, a database or a column: accepted when x is a character set / collation
    /// this engine's own comparison stands for (utf8mb4_0900_ai_ci), refused for the others, which would compare differently
    bool parse_charset_option();
    /// the table options after CREATE TABLE (...): ENGINE, AUTO_INCREMENT, charset, collation, COMMENT, ROW_FORMAT ...; the AUTO_INCREMENT start, if there is one
    std::optional<std::int64_t> parse_table_options();
    /// `USING BTREE|HASH`, `COMMENT 'text'`, `VISIBLE|INVISIBLE`, `KEY_BLOCK_SIZE [=] n` before or after the columns of an index; true when the index type was HASH
    bool skip_index_options();
    /// `[NOT] ENFORCED` after a CHECK; false for NOT ENFORCED (the check is written in the definition but never applied)
    bool check_is_enforced();

    CondExpr parse_condexpr();
    CondExpr parse_or_expr();
    CondExpr parse_and_expr();
    CondExpr parse_not_expr();
    CondExpr parse_primary_cond();
    /// `(a, b) = (1, 2)`, `(a, b) IN ((1, 2), (3, 4))`, `(a, b) NOT IN (SELECT x, y ...)`: a parenthesised list of two or more values followed by a comparison or IN
    bool row_constructor_ahead() const;
    std::vector<ArithExpr> parse_row();
    CondExpr parse_row_condition();
    /// one predicate: EXISTS, `expr OP value`, `expr IS NULL`, ..., or (no operator) an expression that is true when it is not NULL and not 0
    CondExpr parse_pred_expr();
    /// the operator and right side after an expression: `> 5`, `IS [NOT] NULL|TRUE|FALSE`, `[NOT] BETWEEN`, `[NOT] IN`, `[NOT] LIKE`, REGEXP
    CondExpr parse_pred_cond(ArithExpr left);
    Condition parse_pred_tail(ArithExpr left);
    Statement parse_exists_subquery();
    /// the next token starts a predicate operator (`>`, `IS`, `NOT IN`, `BETWEEN`, ...) / an operator that goes on with an expression (also `+`, `||`)
    bool at_pred_operator() const;
    bool at_value_continuation() const;
    ArithExpr parse_additive_expr();
    Statement parse_create_unique_index();
    void parse_order_limit(std::vector<OrderBy>& order_by, std::optional<std::size_t>& limit);
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
    // WINDOW w AS (...): the definitions (their tokens, by lower-case name) of the named windows of the SELECT being parsed, taken out of the token stream when
    // that SELECT starts; `OVER w` and `OVER (w ...)` then have the name replaced by the tokens it stands for (expand_window_name)
    std::vector<std::map<std::string, std::vector<Token>>> window_defs_;
    std::map<std::string, std::vector<Token>> take_window_clause();
    void expand_window_name();
    // A window function inside an expression (`SUM(v) / SUM(SUM(v)) OVER ()`, `RANK() OVER (...) + 1`, `ORDER BY RANK() OVER (...)`): in the select list and in ORDER BY
    // the call becomes a hidden window-function column of the query (`__win<n>`, see hidden_window_name) and the expression reads that as a column.
    bool allow_window_expr_ = false;
    std::vector<SelectColumn> hidden_windows_;
    std::size_t next_hidden_window_ = 0;
    ArithExpr hide_window(SelectColumn::WinFunc wf);
    void parse_over_clause(SelectColumn::WinFunc& wf);   // OVER (...) | OVER w, after the call
    SelectColumn::WinFunc parse_ranking_window();        // ROW_NUMBER() / RANK() / LAG(c) / NTILE(n) ... OVER (...)
    static bool is_window_function_token(TokenKind kind);

    Statement parse_select();
    Statement parse_insert();
    std::vector<std::vector<std::string>> parse_insert_values();
    /// one value of VALUES (...) or of INSERT ... SET col = value (`set_form`: the value also ends at ON, RETURNING, `;` or the end of the statement)
    std::string parse_insert_value(bool set_form);
    /// `SET col = value, ...` of INSERT / REPLACE: the same as `(cols) VALUES (values)`
    void parse_insert_set(std::optional<std::vector<std::string>>& columns, std::vector<std::vector<std::string>>& all_values);
    Statement parse_replace();
    Statement parse_update();
    Statement parse_delete();

    std::string read_parenthesized_expr();

    DataType parse_data_type();
    ColConstraints parse_col_constraints(const std::string& col_name);
    void parse_fk_table_level(std::vector<ColumnDef>& columns, const std::string& constraint_name = "");
    FkAction parse_fk_action(bool* restrict_written = nullptr); // `restrict_written`: set when the action was spelled RESTRICT (NO ACTION is the same here, but MySQL reports it apart)
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
    Statement parse_alter_action(const std::string& table); // one action of ALTER TABLE (the actions are separated by commas)
    Statement parse_rename();
    std::optional<ColumnPosition> parse_column_position(); // [FIRST | AFTER column]
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
    /// a data type of a parameter or a DECLARE as it is written ("DECIMAL(8,2)", "VARCHAR(20)"); empty (and nothing read) when what follows is no type
    std::string read_declared_type();
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
