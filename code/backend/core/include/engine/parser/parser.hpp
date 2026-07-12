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
#include <utility>
#include <vector>

#include "engine/parser/ast.hpp"
#include "engine/parser/lexer.hpp"
#include "engine/result.hpp"

namespace engine {

// Mirrors parser.rs's `pub const NULL_DEFAULT: &str = "__NULL_DEFAULT__";`
extern const std::string NULL_DEFAULT;

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
    static ArithExpr str_to_arith(const std::string& s);

private:
    std::vector<Token> tokens_;
    std::size_t pos_ = 0;

    const Token* peek() const;
    const Token* peek_at(std::size_t offset) const;
    const Token* advance();

    bool peek_is(TokenKind k) const;
    bool peek_at_is(std::size_t offset, TokenKind k) const;

    // The internal (exception-throwing) statement dispatcher. `parse()` wraps this.
    Statement parse_stmt();

    std::string expect_ident();
    std::string expect_alias_ident();
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
    Condition parse_single_pred();
    Condition parse_pred_tail(ArithExpr left);
    Statement parse_exists_subquery();

    ArithExpr parse_arith_factor();
    ArithExpr parse_arith_term();

    std::optional<WindowFrame> parse_window_frame();
    std::pair<std::vector<CaseWhenBranch>, std::optional<std::string>> parse_case_when_inner();
    SelectColumn parse_case_when();

    Statement parse_select();
    Statement parse_insert();
    Statement parse_update();
    Statement parse_delete();

    std::vector<std::string> parse_func_args();
    std::vector<std::string> parse_cast_args();
    std::vector<std::string> parse_date_add_args();
    std::string read_parenthesized_expr();

    DataType parse_data_type();
    ColConstraints parse_col_constraints(const std::string& col_name);
    void parse_fk_table_level(std::vector<ColumnDef>& columns);
    FkAction parse_fk_action();

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
