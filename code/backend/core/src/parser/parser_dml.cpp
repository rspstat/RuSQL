#include <unordered_map>

#include "engine/parser/parser.hpp"
#include "parser_detail.hpp"

namespace engine {

Statement Parser::parse_insert() {
    // INSERT [IGNORE] INTO table [(col1, col2, ...)] VALUES (...) [ON DUPLICATE KEY UPDATE ...]
    bool ignore = false;
    if (peek_is(TokenKind::Ignore)) { advance(); ignore = true; }
    if (!peek_is(TokenKind::Into)) throw ParseError("Expected INTO");
    advance();
    std::string table = expect_ident();

    std::optional<std::vector<std::string>> columns;
    if (peek_is(TokenKind::LParen)) {
        advance();
        std::vector<std::string> cols;
        cols.push_back(expect_ident());
        while (peek_is(TokenKind::Comma)) { advance(); cols.push_back(expect_ident()); }
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after column list");
        advance();
        columns = cols;
    }

    // INSERT INTO table [(cols)] SELECT ...
    if (peek_is(TokenKind::Select)) {
        advance(); // consume SELECT
        Statement query = parse_select();
        InsertConflict on_conflict = ignore ? InsertConflict(InsertConflict::Ignore{}) : InsertConflict(InsertConflict::Abort{});
        auto returning = parse_returning();
        return Statement(Statement::InsertSelect{table, columns, std::make_unique<Statement>(std::move(query)), on_conflict, returning});
    }

    if (!peek_is(TokenKind::Values)) throw ParseError("Expected VALUES or SELECT");
    advance();

    std::vector<std::vector<std::string>> all_values;
    for (;;) {
        if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '('");
        advance();
        std::vector<std::string> row_vals;
        for (;;) {
            std::string val;
            if (peek_is(TokenKind::Comma) || peek_is(TokenKind::RParen)) {
                val = "";
            } else {
                const Token* t = advance();
                if (!t) throw ParseError("Expected value");
                switch (t->kind) {
                    case TokenKind::StringLit: case TokenKind::NumberLit: case TokenKind::Ident: val = t->text; break;
                    case TokenKind::Null: val = "NULL"; break;
                    default: throw ParseError("Expected value");
                }
            }
            row_vals.push_back(val);
            if (peek_is(TokenKind::Comma)) { advance(); }
            else if (peek_is(TokenKind::RParen)) { advance(); break; }
            else throw ParseError("Expected ',' or ')'");
        }
        all_values.push_back(std::move(row_vals));
        if (peek_is(TokenKind::Comma)) advance(); else break;
    }

    InsertConflict on_conflict = [&]() -> InsertConflict {
        if (peek_is(TokenKind::On)) {
            advance(); // ON
            if (!peek_is(TokenKind::Duplicate)) throw ParseError("Expected DUPLICATE");
            advance();
            if (!peek_is(TokenKind::Key)) throw ParseError("Expected KEY");
            advance();
            if (!peek_is(TokenKind::Update)) throw ParseError("Expected UPDATE");
            advance();
            std::vector<std::pair<std::string, ArithExpr>> assignments;
            for (;;) {
                std::string col = expect_ident();
                if (!peek_is(TokenKind::Eq)) throw ParseError("Expected '=' in ON DUPLICATE KEY UPDATE");
                advance();
                ArithExpr expr = parse_arith_expr();
                assignments.emplace_back(col, std::move(expr));
                if (peek_is(TokenKind::Comma)) advance(); else break;
            }
            return InsertConflict(InsertConflict::Update{std::move(assignments)});
        }
        if (ignore) return InsertConflict(InsertConflict::Ignore{});
        return InsertConflict(InsertConflict::Abort{});
    }();

    auto returning = parse_returning();
    return Statement(Statement::Insert{table, columns, all_values, on_conflict, returning});
}

Statement Parser::parse_replace() {
    // REPLACE [INTO] table [(col1, col2, ...)] VALUES (...) | REPLACE [INTO] table [(cols)] SELECT ...
    // No Rust/MySQL-parity original -- new C++-native addition. Reuses the plain
    // Statement::Insert/InsertSelect shape with on_conflict = InsertConflict::Replace
    // rather than introducing a new Statement kind (see executor_dml.cpp's
    // replace_delete_conflicts for the actual delete-then-insert execution). Unlike
    // INSERT, REPLACE has no IGNORE keyword and no ON DUPLICATE KEY UPDATE clause.
    if (peek_is(TokenKind::Into)) advance(); // INTO is optional in real MySQL REPLACE syntax
    std::string table = expect_ident();

    std::optional<std::vector<std::string>> columns;
    if (peek_is(TokenKind::LParen)) {
        advance();
        std::vector<std::string> cols;
        cols.push_back(expect_ident());
        while (peek_is(TokenKind::Comma)) { advance(); cols.push_back(expect_ident()); }
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after column list");
        advance();
        columns = cols;
    }

    if (peek_is(TokenKind::Select)) {
        advance(); // consume SELECT
        Statement query = parse_select();
        auto returning = parse_returning();
        return Statement(Statement::InsertSelect{table, columns, std::make_unique<Statement>(std::move(query)),
                                                   InsertConflict(InsertConflict::Replace{}), returning});
    }

    if (!peek_is(TokenKind::Values)) throw ParseError("Expected VALUES or SELECT");
    advance();

    std::vector<std::vector<std::string>> all_values;
    for (;;) {
        if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '('");
        advance();
        std::vector<std::string> row_vals;
        for (;;) {
            std::string val;
            if (peek_is(TokenKind::Comma) || peek_is(TokenKind::RParen)) {
                val = "";
            } else {
                const Token* t = advance();
                if (!t) throw ParseError("Expected value");
                switch (t->kind) {
                    case TokenKind::StringLit: case TokenKind::NumberLit: case TokenKind::Ident: val = t->text; break;
                    case TokenKind::Null: val = "NULL"; break;
                    default: throw ParseError("Expected value");
                }
            }
            row_vals.push_back(val);
            if (peek_is(TokenKind::Comma)) { advance(); }
            else if (peek_is(TokenKind::RParen)) { advance(); break; }
            else throw ParseError("Expected ',' or ')'");
        }
        all_values.push_back(std::move(row_vals));
        if (peek_is(TokenKind::Comma)) advance(); else break;
    }

    auto returning = parse_returning();
    return Statement(Statement::Insert{table, columns, all_values, InsertConflict(InsertConflict::Replace{}), returning});
}

Statement Parser::parse_update() {
    // UPDATE [t1 [alias1]] [, t2 [alias2]] | [JOIN t2 ON ...] SET col = val [WHERE ...]
    std::string first_table = expect_ident();
    std::unordered_map<std::string, std::string> alias_map;

    if (peek_is(TokenKind::Ident)) {
        std::string a = expect_ident();
        alias_map[a] = first_table;
    }

    std::vector<std::string> tables = {first_table};
    std::vector<Join> joins;

    // 쉼표로 구분된 멀티 테이블: UPDATE t1, t2 SET ...
    while (peek_is(TokenKind::Comma)) {
        advance();
        std::string t = expect_ident();
        if (peek_is(TokenKind::Ident)) {
            std::string a = expect_ident();
            alias_map[a] = t;
        }
        tables.push_back(t);
    }

    // JOIN 절: UPDATE t1 JOIN t2 ON ...
    for (;;) {
        std::optional<JoinType> jt;
        if (peek_is(TokenKind::Join)) { advance(); jt = JoinType::Inner; }
        else if (peek_is(TokenKind::Inner)) {
            advance();
            if (!peek_is(TokenKind::Join)) throw ParseError("Expected JOIN after INNER");
            advance();
            jt = JoinType::Inner;
        } else if (peek_is(TokenKind::Left)) {
            advance();
            // Rust checks `Token::Ident("OUTER")` here (not the OUTER keyword token) — OUTER
            // always lexes as Token::Outer via the keyword table, so this check is
            // effectively dead in the original; faithfully not consuming anything extra.
            if (const Token* p = peek(); p && p->kind == TokenKind::Ident && p->text == "OUTER") advance();
            if (!peek_is(TokenKind::Join)) throw ParseError("Expected JOIN after LEFT");
            advance();
            jt = JoinType::Left;
        } else if (peek_is(TokenKind::Right)) {
            advance();
            if (!peek_is(TokenKind::Join)) throw ParseError("Expected JOIN after RIGHT");
            advance();
            jt = JoinType::Right;
        } else {
            break;
        }
        std::string join_table = expect_ident();
        if (peek_is(TokenKind::Ident)) {
            std::string a = expect_ident();
            alias_map[a] = join_table;
        }
        if (!peek_is(TokenKind::On)) throw ParseError("Expected ON");
        advance();
        CondExpr on_expr = detail::expand_condexpr(parse_condexpr(), alias_map);
        joins.push_back(Join{join_table, std::move(on_expr), *jt, {}});
    }

    if (!peek_is(TokenKind::Set)) throw ParseError("Expected SET");
    advance();

    std::vector<std::pair<std::string, ArithExpr>> assignments;
    for (;;) {
        std::string col = expect_col_ref();
        col = detail::expand_alias_str(col, alias_map);
        if (!peek_is(TokenKind::Eq)) throw ParseError("Expected =");
        advance();
        ArithExpr expr = parse_arith_expr();
        assignments.emplace_back(col, std::move(expr));
        if (peek_is(TokenKind::Comma)) advance(); else break;
    }

    std::optional<CondExpr> condition;
    if (peek_is(TokenKind::Where)) {
        advance();
        condition = detail::expand_condexpr(parse_condexpr(), alias_map);
    }

    auto returning = parse_returning();
    if (tables.size() > 1 || !joins.empty()) {
        return Statement(Statement::MultiUpdate{tables, joins, assignments, condition});
    }
    return Statement(Statement::Update{first_table, assignments, condition, returning});
}

Statement Parser::parse_delete() {
    // DELETE [t1 [, t2]] FROM table [JOIN ...] WHERE ...
    // or DELETE FROM table WHERE ...
    std::optional<std::vector<std::string>> delete_tables;
    if (!peek_is(TokenKind::From)) {
        std::vector<std::string> tbls;
        tbls.push_back(expect_ident());
        while (peek_is(TokenKind::Comma)) { advance(); tbls.push_back(expect_ident()); }
        if (!peek_is(TokenKind::From)) throw ParseError("Expected FROM");
        advance();
        delete_tables = tbls;
    } else {
        advance(); // FROM
    }

    std::string from_table = expect_ident();
    std::unordered_map<std::string, std::string> alias_map;
    if (peek_is(TokenKind::Ident)) {
        std::string a = expect_ident();
        alias_map[a] = from_table;
    }

    std::vector<Join> joins;
    for (;;) {
        std::optional<JoinType> jt;
        if (peek_is(TokenKind::Join)) { advance(); jt = JoinType::Inner; }
        else if (peek_is(TokenKind::Inner)) {
            advance();
            if (!peek_is(TokenKind::Join)) throw ParseError("Expected JOIN after INNER");
            advance();
            jt = JoinType::Inner;
        } else if (peek_is(TokenKind::Left)) {
            advance();
            if (!peek_is(TokenKind::Join)) throw ParseError("Expected JOIN after LEFT");
            advance();
            jt = JoinType::Left;
        } else if (peek_is(TokenKind::Right)) {
            advance();
            if (!peek_is(TokenKind::Join)) throw ParseError("Expected JOIN after RIGHT");
            advance();
            jt = JoinType::Right;
        } else {
            break;
        }
        std::string join_table = expect_ident();
        if (peek_is(TokenKind::Ident)) {
            std::string a = expect_ident();
            alias_map[a] = join_table;
        }
        if (!peek_is(TokenKind::On)) throw ParseError("Expected ON");
        advance();
        CondExpr on_expr = detail::expand_condexpr(parse_condexpr(), alias_map);
        joins.push_back(Join{join_table, std::move(on_expr), *jt, {}});
    }

    std::optional<CondExpr> condition;
    if (peek_is(TokenKind::Where)) {
        advance();
        condition = detail::expand_condexpr(parse_condexpr(), alias_map);
    }

    auto returning = parse_returning();
    if (delete_tables) {
        return Statement(Statement::MultiDelete{*delete_tables, from_table, joins, condition});
    }
    if (!joins.empty()) {
        return Statement(Statement::MultiDelete{{from_table}, from_table, joins, condition});
    }
    return Statement(Statement::Delete{from_table, condition, returning});
}

std::string Parser::parse_single_value() {
    const Token* t = advance();
    if (!t) throw ParseError("Expected value");
    switch (t->kind) {
        case TokenKind::StringLit: return "'" + t->text + "'";
        case TokenKind::NumberLit: return t->text;
        case TokenKind::Null: return "NULL";
        case TokenKind::Ident: {
            std::string s = t->text;
            if (peek_is(TokenKind::Dot)) {
                advance();
                std::string col = expect_ident();
                return s + "." + col;
            }
            return s;
        }
        default: throw ParseError("Expected value");
    }
}

// ── MERGE INTO ──────────────────────────────────────────────
Statement Parser::parse_merge() {
    if (!peek_is(TokenKind::Into)) throw ParseError("Expected INTO after MERGE");
    advance();
    std::string target = expect_ident();
    std::optional<std::string> target_alias;
    if (peek_is(TokenKind::As)) { advance(); target_alias = expect_alias_ident(); }
    else if (peek_is(TokenKind::Ident)) { target_alias = expect_ident(); }

    if (!peek_is(TokenKind::Using)) throw ParseError("Expected USING");
    advance();
    std::string source = expect_ident();
    std::optional<std::string> source_alias;
    if (peek_is(TokenKind::As)) { advance(); source_alias = expect_alias_ident(); }
    else if (peek_is(TokenKind::Ident)) { source_alias = expect_ident(); }

    if (!peek_is(TokenKind::On)) throw ParseError("Expected ON");
    advance();
    CondExpr on = parse_condexpr();

    std::optional<std::vector<std::pair<std::string, ArithExpr>>> when_matched_update;
    bool when_matched_delete = false;
    std::optional<CondExpr> when_matched_delete_cond;
    std::optional<std::vector<std::string>> when_not_matched_columns;
    std::vector<std::string> when_not_matched_values;

    for (int i = 0; i < 4; i++) {
        if (!peek_is(TokenKind::When)) break;
        advance(); // consume WHEN

        bool not_matched = false;
        if (peek_is(TokenKind::Not)) { advance(); not_matched = true; }

        if (!peek_is(TokenKind::Matched)) throw ParseError("Expected MATCHED");
        advance();

        std::optional<CondExpr> extra_cond;
        if (!not_matched && peek_is(TokenKind::And)) {
            advance();
            extra_cond = parse_condexpr();
        }
        if (!peek_is(TokenKind::Then)) throw ParseError("Expected THEN");
        advance();

        if (not_matched) {
            if (!peek_is(TokenKind::Insert)) throw ParseError("Expected INSERT after WHEN NOT MATCHED THEN");
            advance();
            if (peek_is(TokenKind::LParen)) {
                advance();
                std::vector<std::string> cols;
                for (;;) {
                    cols.push_back(expect_ident());
                    if (peek_is(TokenKind::Comma)) { advance(); }
                    else if (peek_is(TokenKind::RParen)) { advance(); break; }
                    else throw ParseError("Expected ',' or ')' in INSERT cols");
                }
                when_not_matched_columns = cols;
            }
            if (!peek_is(TokenKind::Values)) throw ParseError("Expected VALUES");
            advance();
            if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after VALUES");
            advance();
            for (;;) {
                when_not_matched_values.push_back(parse_single_value());
                if (peek_is(TokenKind::Comma)) { advance(); }
                else if (peek_is(TokenKind::RParen)) { advance(); break; }
                else throw ParseError("Expected ',' or ')' in VALUES");
            }
        } else if (peek_is(TokenKind::Update)) {
            advance();
            if (!peek_is(TokenKind::Set)) throw ParseError("Expected SET after UPDATE");
            advance();
            std::vector<std::pair<std::string, ArithExpr>> assignments;
            for (;;) {
                std::string col = expect_col_ref();
                if (!peek_is(TokenKind::Eq)) throw ParseError("Expected = in assignment");
                advance();
                ArithExpr expr = parse_arith_expr();
                assignments.emplace_back(col, std::move(expr));
                if (peek_is(TokenKind::Comma)) advance(); else break;
            }
            when_matched_update = std::move(assignments);
        } else if (peek_is(TokenKind::Delete)) {
            advance();
            when_matched_delete = true;
            when_matched_delete_cond = extra_cond;
        } else {
            throw ParseError("Expected UPDATE or DELETE after WHEN MATCHED THEN");
        }
    }

    return Statement(Statement::Merge{
        target, target_alias, source, source_alias, std::move(on),
        when_matched_update, when_matched_delete, when_matched_delete_cond,
        when_not_matched_columns, when_not_matched_values});
}

} // namespace engine
