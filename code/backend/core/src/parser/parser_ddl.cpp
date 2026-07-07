#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>

#include "engine/parser/parser.hpp"

namespace engine {

namespace {

// 토큰 슬라이스 → SQL 문자열 재구성 (SHOW CREATE VIEW용)
std::string token_debug(const Token& tok) {
    switch (tok.kind) {
        case TokenKind::Ident: return "Ident(" + tok.text + ")";
        case TokenKind::StringLit: return "StringLit(" + tok.text + ")";
        case TokenKind::NumberLit: return "NumberLit(" + tok.text + ")";
        default: return "Token";
    }
}

std::string tokens_to_sql(const std::vector<Token>& tokens) {
    std::string out;
    static const std::unordered_map<TokenKind, std::string> kSimple = {
        {TokenKind::Select, "SELECT"}, {TokenKind::From, "FROM"}, {TokenKind::Where, "WHERE"},
        {TokenKind::Insert, "INSERT"}, {TokenKind::Into, "INTO"}, {TokenKind::Values, "VALUES"},
        {TokenKind::Update, "UPDATE"}, {TokenKind::Set, "SET"}, {TokenKind::Delete, "DELETE"},
        {TokenKind::Create, "CREATE"}, {TokenKind::Table, "TABLE"}, {TokenKind::Drop, "DROP"},
        {TokenKind::Join, "JOIN"}, {TokenKind::Left, "LEFT"}, {TokenKind::Right, "RIGHT"}, {TokenKind::Cross, "CROSS"},
        {TokenKind::Natural, "NATURAL"}, {TokenKind::Outer, "OUTER"}, {TokenKind::On, "ON"},
        {TokenKind::And, "AND"}, {TokenKind::Or, "OR"}, {TokenKind::Not, "NOT"},
        {TokenKind::Alter, "ALTER"}, {TokenKind::Add, "ADD"}, {TokenKind::Column, "COLUMN"},
        {TokenKind::Rename, "RENAME"}, {TokenKind::To, "TO"},
        {TokenKind::Order, "ORDER"}, {TokenKind::Group, "GROUP"}, {TokenKind::By, "BY"},
        {TokenKind::Asc, "ASC"}, {TokenKind::Desc, "DESC"}, {TokenKind::Limit, "LIMIT"},
        {TokenKind::Count, "COUNT"}, {TokenKind::Sum, "SUM"}, {TokenKind::Avg, "AVG"}, {TokenKind::Min, "MIN"}, {TokenKind::Max, "MAX"},
        {TokenKind::Having, "HAVING"}, {TokenKind::In, "IN"}, {TokenKind::Between, "BETWEEN"}, {TokenKind::Like, "LIKE"},
        {TokenKind::Index, "INDEX"}, {TokenKind::Unique, "UNIQUE"}, {TokenKind::View, "VIEW"}, {TokenKind::As, "AS"},
        {TokenKind::Primary, "PRIMARY"}, {TokenKind::Key, "KEY"}, {TokenKind::Null, "NULL"},
        {TokenKind::Auto, "AUTO"}, {TokenKind::Increment, "INCREMENT"},
        {TokenKind::Show, "SHOW"}, {TokenKind::Tables, "TABLES"}, {TokenKind::Describe, "DESCRIBE"}, {TokenKind::Truncate, "TRUNCATE"},
        {TokenKind::References, "REFERENCES"}, {TokenKind::Foreign, "FOREIGN"}, {TokenKind::Constraint, "CONSTRAINT"},
        {TokenKind::Int, "INT"}, {TokenKind::Text, "TEXT"}, {TokenKind::Float, "FLOAT"}, {TokenKind::Boolean, "BOOLEAN"},
        {TokenKind::Asterisk, "*"}, {TokenKind::Comma, ","}, {TokenKind::Semicolon, ";"},
        {TokenKind::LParen, "("}, {TokenKind::RParen, ")"},
        {TokenKind::Dot, "."}, {TokenKind::Eq, "="}, {TokenKind::Ne, "<>"}, {TokenKind::Gt, ">"}, {TokenKind::Lt, "<"},
        {TokenKind::Gte, ">="}, {TokenKind::Lte, "<="}, {TokenKind::Plus, "+"}, {TokenKind::Minus, "-"}, {TokenKind::Slash, "/"},
        {TokenKind::Cascade, "CASCADE"}, {TokenKind::Restrict, "RESTRICT"}, {TokenKind::Is, "IS"},
        {TokenKind::Isolation, "ISOLATION"}, {TokenKind::Level, "LEVEL"},
        {TokenKind::Uncommitted, "UNCOMMITTED"}, {TokenKind::Committed, "COMMITTED"},
        {TokenKind::Repeatable, "REPEATABLE"}, {TokenKind::Serializable, "SERIALIZABLE"},
        {TokenKind::For, "FOR"}, {TokenKind::Locks, "LOCKS"}, {TokenKind::Distinct, "DISTINCT"},
        {TokenKind::Default, "DEFAULT"}, {TokenKind::Exists, "EXISTS"},
        {TokenKind::Varchar, "VARCHAR"}, {TokenKind::Date, "DATE"}, {TokenKind::Datetime, "DATETIME"},
        {TokenKind::Timestamp, "TIMESTAMP"}, {TokenKind::Decimal, "DECIMAL"}, {TokenKind::Double, "DOUBLE"},
        {TokenKind::Time, "TIME"}, {TokenKind::Year, "YEAR"}, {TokenKind::Blob, "BLOB"}, {TokenKind::Enum, "ENUM"},
        {TokenKind::Database, "DATABASE"}, {TokenKind::Inner, "INNER"}, {TokenKind::Check, "CHECK"},
        {TokenKind::Upper, "UPPER"}, {TokenKind::Lower, "LOWER"}, {TokenKind::Length, "LENGTH"},
        {TokenKind::Trim, "TRIM"}, {TokenKind::Concat, "CONCAT"}, {TokenKind::Substr, "SUBSTR"},
        {TokenKind::Substring, "SUBSTRING"}, {TokenKind::Now, "NOW"}, {TokenKind::Curdate, "CURDATE"},
        {TokenKind::DateFormat, "DATE_FORMAT"}, {TokenKind::Coalesce, "COALESCE"},
        {TokenKind::Ifnull, "IFNULL"}, {TokenKind::Replace, "REPLACE"},
        {TokenKind::Round, "ROUND"}, {TokenKind::Abs, "ABS"}, {TokenKind::Ceil, "CEIL"}, {TokenKind::Floor, "FLOOR"}, {TokenKind::Mod, "MOD"},
        {TokenKind::Case, "CASE"}, {TokenKind::When, "WHEN"}, {TokenKind::Then, "THEN"}, {TokenKind::Else, "ELSE"}, {TokenKind::End, "END"},
        {TokenKind::Union, "UNION"}, {TokenKind::Intersect, "INTERSECT"}, {TokenKind::Except, "EXCEPT"}, {TokenKind::All, "ALL"},
        {TokenKind::If, "IF"}, {TokenKind::Offset, "OFFSET"}, {TokenKind::With, "WITH"}, {TokenKind::Recursive, "RECURSIVE"},
        {TokenKind::Full, "FULL"}, {TokenKind::Returning, "RETURNING"}, {TokenKind::Ignore, "IGNORE"},
    };

    for (std::size_t i = 0; i < tokens.size(); i++) {
        const Token& tok = tokens[i];
        if (tok.kind == TokenKind::Ident) {
            bool needs_space = i > 0 && !(tokens[i - 1].kind == TokenKind::LParen ||
                                          tokens[i - 1].kind == TokenKind::Dot || tokens[i - 1].kind == TokenKind::Comma);
            if (needs_space) out.push_back(' ');
            out += tok.text;
            continue;
        }
        if (tok.kind == TokenKind::StringLit) {
            bool needs_space = i > 0 && !(tokens[i - 1].kind == TokenKind::LParen || tokens[i - 1].kind == TokenKind::Comma);
            if (needs_space) out.push_back(' ');
            out.push_back('\'');
            std::string escaped;
            for (char c : tok.text) { if (c == '\'') escaped += "\\'"; else escaped.push_back(c); }
            out += escaped;
            out.push_back('\'');
            continue;
        }
        if (tok.kind == TokenKind::NumberLit) {
            bool needs_space = i > 0 && !(tokens[i - 1].kind == TokenKind::LParen || tokens[i - 1].kind == TokenKind::Comma ||
                                          tokens[i - 1].kind == TokenKind::Dot);
            if (needs_space) out.push_back(' ');
            out += tok.text;
            continue;
        }

        std::string s;
        auto it = kSimple.find(tok.kind);
        if (it != kSimple.end()) {
            s = it->second;
        } else {
            bool needs_space = i > 0;
            if (needs_space) out.push_back(' ');
            out += token_debug(tok);
            continue;
        }

        bool prev_no_space = i > 0 && (tokens[i - 1].kind == TokenKind::LParen || tokens[i - 1].kind == TokenKind::Dot);
        bool curr_no_space_before = tok.kind == TokenKind::Comma || tok.kind == TokenKind::RParen ||
                                    tok.kind == TokenKind::Dot || tok.kind == TokenKind::Semicolon || tok.kind == TokenKind::Asterisk;
        if (i > 0 && !prev_no_space && !curr_no_space_before) out.push_back(' ');
        out += s;
    }

    // trim
    std::size_t start = out.find_first_not_of(" \t\r\n");
    std::size_t end = out.find_last_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return out.substr(start, end - start + 1);
}

std::string to_upper(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::toupper(c); });
    return out;
}

} // namespace

/// 데이터 타입 파싱
DataType Parser::parse_data_type() {
    auto skip_parenthesized = [this]() {
        if (peek_is(TokenKind::LParen)) {
            advance();
            while (!peek_is(TokenKind::RParen) && peek() != nullptr) advance();
            advance();
        }
    };
    auto skip_unsigned = [this]() {
        if (const Token* p = peek(); p && p->kind == TokenKind::Ident && to_upper(p->text) == "UNSIGNED") advance();
    };

    const Token* t = advance();
    if (!t) throw ParseError("Expected data type");
    switch (t->kind) {
        case TokenKind::Int: skip_parenthesized(); skip_unsigned(); return DataType(DataType::Int{});
        case TokenKind::BigInt: skip_parenthesized(); skip_unsigned(); return DataType(DataType::BigInt{});
        case TokenKind::SmallInt: skip_parenthesized(); skip_unsigned(); return DataType(DataType::SmallInt{});
        case TokenKind::TinyInt: skip_parenthesized(); skip_unsigned(); return DataType(DataType::TinyInt{});
        case TokenKind::Text: return DataType(DataType::Text{});
        case TokenKind::Float: skip_parenthesized(); return DataType(DataType::Float{});
        case TokenKind::Boolean: return DataType(DataType::Boolean{});
        case TokenKind::Double: skip_parenthesized(); return DataType(DataType::Double{});
        case TokenKind::Time: return DataType(DataType::Time{});
        case TokenKind::Year: return DataType(DataType::Year{});
        case TokenKind::Blob: return DataType(DataType::Blob{});
        case TokenKind::Json: return DataType(DataType::Json{});
        case TokenKind::Enum: {
            if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after ENUM");
            advance();
            std::vector<std::string> values;
            for (;;) {
                const Token* v = advance();
                if (v && v->kind == TokenKind::StringLit) values.push_back(v->text);
                else if (v && v->kind == TokenKind::RParen) break;
                else throw ParseError("Expected string value in ENUM");
                if (peek_is(TokenKind::Comma)) advance();
                else if (peek_is(TokenKind::RParen)) { advance(); break; }
                else break;
            }
            return DataType(DataType::Enum{values});
        }
        case TokenKind::Set: {
            if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after SET type");
            advance();
            std::vector<std::string> values;
            for (;;) {
                const Token* v = advance();
                if (v && v->kind == TokenKind::StringLit) values.push_back(v->text);
                else if (v && v->kind == TokenKind::RParen) break;
                else throw ParseError("Expected string value in SET");
                if (peek_is(TokenKind::Comma)) advance();
                else if (peek_is(TokenKind::RParen)) { advance(); break; }
                else break;
            }
            return DataType(DataType::Set{values});
        }
        case TokenKind::Varchar: {
            std::uint32_t n = 255;
            if (peek_is(TokenKind::LParen)) {
                advance();
                const Token* sz = advance();
                if (sz && sz->kind == TokenKind::NumberLit) { try { n = static_cast<std::uint32_t>(std::stoul(sz->text)); } catch (...) { n = 255; } }
                else n = 255;
                while (!peek_is(TokenKind::RParen) && peek() != nullptr) advance();
                if (peek_is(TokenKind::RParen)) advance();
            }
            return DataType(DataType::Varchar{n});
        }
        case TokenKind::Date: return DataType(DataType::Date{});
        case TokenKind::Datetime: return DataType(DataType::DateTime{});
        case TokenKind::Timestamp: return DataType(DataType::Timestamp{});
        case TokenKind::Decimal: {
            if (!peek_is(TokenKind::LParen)) return DataType(DataType::Decimal{10, 2});
            advance();
            const Token* pt = advance();
            std::uint8_t p = 10;
            if (pt && pt->kind == TokenKind::NumberLit) { try { p = static_cast<std::uint8_t>(std::stoul(pt->text)); } catch (...) { p = 10; } }
            else return DataType(DataType::Decimal{10, 2});
            std::uint8_t s = 0;
            if (peek_is(TokenKind::Comma)) {
                advance();
                const Token* st = advance();
                if (st && st->kind == TokenKind::NumberLit) { try { s = static_cast<std::uint8_t>(std::stoul(st->text)); } catch (...) { s = 2; } }
                else s = 2;
            }
            if (peek_is(TokenKind::RParen)) advance();
            return DataType(DataType::Decimal{p, s});
        }
        default: throw ParseError("Expected data type");
    }
}

FkAction Parser::parse_fk_action() {
    const Token* t = advance();
    if (!t) throw ParseError("Expected CASCADE/RESTRICT/SET");
    if (t->kind == TokenKind::Cascade) return FkAction::Cascade;
    if (t->kind == TokenKind::Restrict) return FkAction::Restrict;
    if (t->kind == TokenKind::Ident && to_upper(t->text) == "NO") {
        advance(); // ACTION
        return FkAction::Restrict;
    }
    if (t->kind == TokenKind::Set) {
        const Token* n = advance();
        if (n && n->kind == TokenKind::Null) return FkAction::SetNull;
        if (n && n->kind == TokenKind::Default) return FkAction::SetDefault;
        throw ParseError("Expected NULL or DEFAULT after SET");
    }
    throw ParseError("Expected CASCADE/RESTRICT/SET");
}

/// 괄호 안의 원시 SQL 표현식을 문자열로 캡처 (CHECK 제약 저장용)
std::string Parser::read_parenthesized_expr() {
    if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' for expression");
    advance();
    std::vector<std::string> parts;
    int depth = 1;
    for (;;) {
        const Token* t = advance();
        if (!t) throw ParseError("Unexpected end in expression");
        switch (t->kind) {
            case TokenKind::LParen: depth++; parts.push_back("("); break;
            case TokenKind::RParen:
                depth--;
                if (depth == 0) goto done;
                parts.push_back(")");
                break;
            case TokenKind::Ident: parts.push_back(t->text); break;
            case TokenKind::StringLit: parts.push_back("'" + t->text + "'"); break;
            case TokenKind::NumberLit: parts.push_back(t->text); break;
            case TokenKind::And: parts.push_back("AND"); break;
            case TokenKind::Or: parts.push_back("OR"); break;
            case TokenKind::Not: parts.push_back("NOT"); break;
            case TokenKind::Eq: parts.push_back("="); break;
            case TokenKind::Ne: parts.push_back("!="); break;
            case TokenKind::Gt: parts.push_back(">"); break;
            case TokenKind::Lt: parts.push_back("<"); break;
            case TokenKind::Gte: parts.push_back(">="); break;
            case TokenKind::Lte: parts.push_back("<="); break;
            case TokenKind::Null: parts.push_back("NULL"); break;
            case TokenKind::Is: parts.push_back("IS"); break;
            case TokenKind::In: parts.push_back("IN"); break;
            case TokenKind::Between: parts.push_back("BETWEEN"); break;
            case TokenKind::Like: parts.push_back("LIKE"); break;
            case TokenKind::Comma: parts.push_back(","); break;
            default: parts.push_back(token_debug(*t)); break;
        }
    }
done:
    std::string out;
    for (std::size_t i = 0; i < parts.size(); i++) {
        if (i) out.push_back(' ');
        out += parts[i];
    }
    return out;
}

/// 컬럼 제약 공통 파서
ColConstraints Parser::parse_col_constraints(const std::string& col_name) {
    ColConstraints c;
    for (;;) {
        const Token* p = peek();
        if (!p) break;
        if (p->kind == TokenKind::Check) {
            advance();
            c.check_expr = read_parenthesized_expr();
        } else if (p->kind == TokenKind::Primary) {
            advance();
            if (!peek_is(TokenKind::Key)) throw ParseError("Expected KEY");
            advance();
            c.primary_key = true;
            c.not_null = true;
        } else if (p->kind == TokenKind::Not) {
            advance();
            if (!peek_is(TokenKind::Null)) throw ParseError("Expected NULL");
            advance();
            c.not_null = true;
        } else if (p->kind == TokenKind::Unique) {
            advance();
            c.unique = true;
        } else if (p->kind == TokenKind::Ident && to_upper(p->text) == "AUTO_INCREMENT") {
            advance();
            c.auto_increment = true;
        } else if (p->kind == TokenKind::Auto) {
            advance();
            if (!peek_is(TokenKind::Increment)) throw ParseError("Expected INCREMENT");
            advance();
            c.auto_increment = true;
        } else if (p->kind == TokenKind::Default) {
            advance();
            const Token* t = advance();
            if (!t) throw ParseError("Expected default value");
            switch (t->kind) {
                case TokenKind::StringLit: case TokenKind::NumberLit: case TokenKind::Ident: c.default_value = t->text; break;
                case TokenKind::Null: c.default_value = NULL_DEFAULT; break;
                default: throw ParseError("Expected default value");
            }
        } else if (p->kind == TokenKind::References) {
            advance();
            std::string ref_table = expect_ident();
            if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '('");
            advance();
            std::string ref_column = expect_ident();
            if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')'");
            advance();
            FkAction on_delete = FkAction::Restrict, on_update = FkAction::Restrict;
            while (peek_is(TokenKind::On)) {
                advance(); // ON
                const Token* dt = advance();
                if (dt && dt->kind == TokenKind::Delete) on_delete = parse_fk_action();
                else if (dt && dt->kind == TokenKind::Update) on_update = parse_fk_action();
                else throw ParseError("Expected DELETE or UPDATE after ON");
            }
            c.foreign_key = ForeignKey{col_name, ref_table, ref_column, on_delete, on_update};
        } else {
            break;
        }
    }
    return c;
}

/// FOREIGN KEY (col[, col2...]) REFERENCES ref_table(ref_col) [ON DELETE ...] [ON UPDATE ...]
void Parser::parse_fk_table_level(std::vector<ColumnDef>& columns) {
    if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after FOREIGN KEY");
    advance();
    std::string fk_col = expect_ident();
    while (peek_is(TokenKind::Comma)) { advance(); expect_ident(); }
    if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after FK columns");
    advance();
    if (!peek_is(TokenKind::References)) throw ParseError("Expected REFERENCES");
    advance();
    std::string ref_table = expect_ident();
    if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after ref table");
    advance();
    std::string ref_column = expect_ident();
    while (peek_is(TokenKind::Comma)) { advance(); expect_ident(); }
    if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after ref column");
    advance();
    FkAction on_delete = FkAction::Restrict, on_update = FkAction::Restrict;
    while (peek_is(TokenKind::On)) {
        advance();
        const Token* dt = advance();
        if (dt && dt->kind == TokenKind::Delete) on_delete = parse_fk_action();
        else if (dt && dt->kind == TokenKind::Update) on_update = parse_fk_action();
        else throw ParseError("Expected DELETE or UPDATE after ON");
    }
    auto it = std::find_if(columns.begin(), columns.end(), [&](const ColumnDef& c) { return c.name == fk_col; });
    if (it == columns.end()) throw ParseError("FOREIGN KEY: column '" + fk_col + "' not defined");
    it->foreign_key = ForeignKey{fk_col, ref_table, ref_column, on_delete, on_update};
}

Statement Parser::parse_create() {
    if (!peek_is(TokenKind::Table)) throw ParseError("Expected TABLE");
    advance();

    bool if_not_exists = false;
    if (peek_is(TokenKind::If)) {
        advance();
        if (!peek_is(TokenKind::Not)) throw ParseError("Expected NOT after IF");
        advance();
        if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS");
        advance();
        if_not_exists = true;
    }

    std::string name = expect_ident();
    if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '('");
    advance();

    std::vector<ColumnDef> columns;
    std::vector<std::string> primary_key_columns;
    std::vector<std::pair<std::optional<std::string>, std::string>> check_constraints;

    for (;;) {
        if (peek_is(TokenKind::Primary)) {
            advance();
            if (!peek_is(TokenKind::Key)) throw ParseError("Expected KEY after PRIMARY");
            advance();
            if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after PRIMARY KEY");
            advance();
            std::vector<std::string> pk_cols;
            pk_cols.push_back(expect_ident());
            while (peek_is(TokenKind::Comma)) { advance(); pk_cols.push_back(expect_ident()); }
            if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after PK columns");
            advance();
            for (auto& pk_col : pk_cols) {
                auto it = std::find_if(columns.begin(), columns.end(), [&](const ColumnDef& c) { return c.name == pk_col; });
                if (it != columns.end()) { it->primary_key = true; it->not_null = true; }
            }
            primary_key_columns = pk_cols;
        } else if (peek_is(TokenKind::Check)) {
            advance();
            std::string expr = read_parenthesized_expr();
            check_constraints.emplace_back(std::nullopt, expr);
        } else if (peek_is(TokenKind::Constraint)) {
            advance();
            std::string constraint_name = expect_ident();
            if (peek_is(TokenKind::Unique)) {
                advance();
                if (peek_is(TokenKind::Key)) advance();
                if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after UNIQUE");
                advance();
                std::string col = expect_ident();
                if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after column");
                advance();
                auto it = std::find_if(columns.begin(), columns.end(), [&](const ColumnDef& c) { return c.name == col; });
                if (it == columns.end()) throw ParseError("CONSTRAINT: column '" + col + "' not defined");
                it->unique = true;
                it->unique_constraint_name = constraint_name;
            } else if (peek_is(TokenKind::Check)) {
                advance();
                std::string expr = read_parenthesized_expr();
                check_constraints.emplace_back(constraint_name, expr);
            } else if (peek_is(TokenKind::Primary)) {
                advance();
                if (!peek_is(TokenKind::Key)) throw ParseError("Expected KEY after PRIMARY");
                advance();
                if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after PRIMARY KEY");
                advance();
                std::vector<std::string> pk_cols;
                pk_cols.push_back(expect_ident());
                while (peek_is(TokenKind::Comma)) { advance(); pk_cols.push_back(expect_ident()); }
                if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after PK columns");
                advance();
                for (auto& pk_col : pk_cols) {
                    auto it = std::find_if(columns.begin(), columns.end(), [&](const ColumnDef& c) { return c.name == pk_col; });
                    if (it != columns.end()) { it->primary_key = true; it->not_null = true; }
                }
                primary_key_columns = pk_cols;
            } else if (peek_is(TokenKind::Foreign)) {
                advance();
                if (!peek_is(TokenKind::Key)) throw ParseError("Expected KEY");
                advance();
                parse_fk_table_level(columns);
            } else {
                throw ParseError("Expected PRIMARY KEY, UNIQUE, CHECK, or FOREIGN KEY after CONSTRAINT name");
            }
        } else if (peek_is(TokenKind::Unique)) {
            advance();
            if (peek_is(TokenKind::Key)) advance();
            if (peek_is(TokenKind::Ident)) expect_ident();
            if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after UNIQUE KEY name");
            advance();
            std::string col = expect_ident();
            while (peek_is(TokenKind::Comma)) { advance(); expect_ident(); }
            if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after UNIQUE KEY columns");
            advance();
            auto it = std::find_if(columns.begin(), columns.end(), [&](const ColumnDef& c) { return c.name == col; });
            if (it != columns.end()) it->unique = true;
        } else if (peek_is(TokenKind::Index) || peek_is(TokenKind::Key)) {
            advance(); // INDEX or KEY
            if (peek_is(TokenKind::Ident)) expect_ident();
            if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after INDEX/KEY name");
            advance();
            expect_ident();
            while (peek_is(TokenKind::Comma)) { advance(); expect_ident(); }
            if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after INDEX columns");
            advance();
        } else if (peek_is(TokenKind::Foreign)) {
            advance();
            if (!peek_is(TokenKind::Key)) throw ParseError("Expected KEY after FOREIGN");
            advance();
            parse_fk_table_level(columns);
        } else {
            std::string col_name = expect_ident();
            DataType data_type = parse_data_type();
            ColConstraints cc = parse_col_constraints(col_name);
            ColumnDef col;
            col.name = col_name;
            col.data_type = data_type;
            col.primary_key = cc.primary_key;
            col.not_null = cc.not_null;
            col.unique = cc.unique;
            col.unique_constraint_name = cc.unique_constraint_name;
            col.auto_increment = cc.auto_increment;
            col.default_value = cc.default_value;
            col.foreign_key = cc.foreign_key;
            col.check_expr = cc.check_expr;
            columns.push_back(std::move(col));
        }

        if (peek_is(TokenKind::Comma)) { advance(); }
        else if (peek_is(TokenKind::RParen)) { advance(); break; }
        else throw ParseError("Expected ',' or ')'");
    }

    return Statement(Statement::CreateTable{name, columns, if_not_exists, primary_key_columns, check_constraints});
}

Statement Parser::parse_drop() {
    if (!peek_is(TokenKind::Table)) throw ParseError("Expected TABLE");
    advance();
    bool if_exists = false;
    if (peek_is(TokenKind::If)) {
        advance();
        if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS after IF");
        advance();
        if_exists = true;
    }
    std::string name = expect_ident();
    return Statement(Statement::DropTable{name, if_exists});
}

Statement Parser::parse_alter() {
    if (!peek_is(TokenKind::Table)) throw ParseError("Expected TABLE");
    advance();
    std::string table = expect_ident();

    const Token* t = advance();
    if (!t) throw ParseError("Expected ADD, DROP, RENAME, or MODIFY");

    if (t->kind == TokenKind::Add) {
        if (peek_is(TokenKind::Constraint) || peek_is(TokenKind::Foreign) || peek_is(TokenKind::Unique) || peek_is(TokenKind::Check)) {
            std::optional<std::string> constraint_name;
            if (peek_is(TokenKind::Constraint)) {
                advance();
                if (peek_is(TokenKind::Ident)) constraint_name = expect_ident();
            }
            const Token* kind = advance();
            if (kind && kind->kind == TokenKind::Foreign) {
                if (!peek_is(TokenKind::Key)) throw ParseError("Expected KEY after FOREIGN");
                advance();
                if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after FOREIGN KEY");
                advance();
                std::string column = expect_ident();
                if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')'");
                advance();
                if (!peek_is(TokenKind::References)) throw ParseError("Expected REFERENCES");
                advance();
                std::string ref_table = expect_ident();
                if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after ref table");
                advance();
                std::string ref_column = expect_ident();
                if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after ref column");
                advance();
                FkAction on_delete = FkAction::Restrict, on_update = FkAction::Restrict;
                while (peek_is(TokenKind::On)) {
                    advance();
                    const Token* dt = advance();
                    if (dt && dt->kind == TokenKind::Delete) on_delete = parse_fk_action();
                    else if (dt && dt->kind == TokenKind::Update) on_update = parse_fk_action();
                    else throw ParseError("Expected DELETE or UPDATE after ON");
                }
                return Statement(Statement::AlterTable{table, AlterAction(AlterAction::AddForeignKey{
                    constraint_name, column, ref_table, ref_column, on_delete, on_update})});
            }
            if (kind && kind->kind == TokenKind::Unique) {
                if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after UNIQUE");
                advance();
                std::string column = expect_ident();
                if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after column");
                advance();
                return Statement(Statement::AlterTable{table, AlterAction(AlterAction::AddUniqueConstraint{constraint_name, column})});
            }
            if (kind && kind->kind == TokenKind::Check) {
                if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' after CHECK");
                advance();
                int depth = 1;
                std::string expr;
                auto app = [&](const std::string& piece) {
                    if (!expr.empty()) expr.push_back(' ');
                    expr += piece;
                };
                for (;;) {
                    const Token* tk = advance();
                    if (!tk) break;
                    if (tk->kind == TokenKind::LParen) { depth++; if (!expr.empty()) expr.push_back(' '); expr.push_back('('); }
                    else if (tk->kind == TokenKind::RParen) { depth--; if (depth == 0) break; if (!expr.empty()) expr.push_back(' '); expr.push_back(')'); }
                    else if (tk->kind == TokenKind::Ident) app(tk->text);
                    else if (tk->kind == TokenKind::NumberLit) app(tk->text);
                    else if (tk->kind == TokenKind::StringLit) app("'" + tk->text + "'");
                    else if (tk->kind == TokenKind::Gt) app(">");
                    else if (tk->kind == TokenKind::Lt) app("<");
                    else if (tk->kind == TokenKind::Gte) app(">=");
                    else if (tk->kind == TokenKind::Lte) app("<=");
                    else if (tk->kind == TokenKind::Eq) app("=");
                    else if (tk->kind == TokenKind::Ne) app("!=");
                    else if (tk->kind == TokenKind::And) app("AND");
                    else if (tk->kind == TokenKind::Or) app("OR");
                }
                return Statement(Statement::AlterTable{table, AlterAction(AlterAction::AddCheckConstraint{constraint_name, expr})});
            }
            throw ParseError("Expected FOREIGN, UNIQUE, or CHECK after CONSTRAINT");
        }
        if (peek_is(TokenKind::Column)) advance();
        std::string col_name = expect_ident();
        DataType data_type = parse_data_type();
        ColumnDef col;
        col.name = col_name;
        col.data_type = data_type;
        return Statement(Statement::AlterTable{table, AlterAction(AlterAction::AddColumn{col})});
    }

    if (t->kind == TokenKind::Drop) {
        if (peek_is(TokenKind::Constraint)) {
            advance();
            std::string name = expect_ident();
            return Statement(Statement::AlterTable{table, AlterAction(AlterAction::DropConstraint{name})});
        }
        if (peek_is(TokenKind::Foreign)) {
            advance();
            if (!peek_is(TokenKind::Key)) throw ParseError("Expected KEY after FOREIGN");
            advance();
            std::string name = expect_ident();
            return Statement(Statement::AlterTable{table, AlterAction(AlterAction::DropForeignKey{name})});
        }
        if (peek_is(TokenKind::Column)) advance();
        std::string col_name = expect_ident();
        return Statement(Statement::AlterTable{table, AlterAction(AlterAction::DropColumn{col_name})});
    }

    if (t->kind == TokenKind::Rename) {
        if (peek_is(TokenKind::Column)) {
            advance();
            std::string from = expect_ident();
            if (!peek_is(TokenKind::To)) throw ParseError("Expected TO");
            advance();
            std::string to = expect_ident();
            return Statement(Statement::AlterTable{table, AlterAction(AlterAction::RenameColumn{from, to})});
        }
        if (peek_is(TokenKind::To)) {
            advance();
            std::string to = expect_ident();
            return Statement(Statement::AlterTable{table, AlterAction(AlterAction::RenameTable{to})});
        }
        if (peek_is(TokenKind::Ident)) {
            std::string to = expect_ident();
            return Statement(Statement::AlterTable{table, AlterAction(AlterAction::RenameTable{to})});
        }
        throw ParseError("Expected COLUMN, TO, or table name after RENAME");
    }

    if (t->kind == TokenKind::Modify) {
        if (peek_is(TokenKind::Column)) advance();
        std::string col_name = expect_ident();
        DataType data_type = parse_data_type();
        ColConstraints cc = parse_col_constraints(col_name);
        ColumnDef col;
        col.name = col_name;
        col.data_type = data_type;
        col.primary_key = cc.primary_key;
        col.not_null = cc.not_null;
        col.unique = cc.unique;
        col.unique_constraint_name = cc.unique_constraint_name;
        col.auto_increment = cc.auto_increment;
        col.default_value = cc.default_value;
        col.foreign_key = cc.foreign_key;
        col.check_expr = cc.check_expr;
        return Statement(Statement::AlterTable{table, AlterAction(AlterAction::ModifyColumn{col})});
    }

    throw ParseError("Expected ADD, DROP, RENAME, or MODIFY");
}

Statement Parser::parse_create_index() {
    std::string index_name = expect_ident();
    if (!peek_is(TokenKind::On)) throw ParseError("Expected ON");
    advance();
    std::string table = expect_ident();
    if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '('");
    advance();
    std::vector<std::string> columns;
    columns.push_back(expect_ident());
    while (peek_is(TokenKind::Comma)) { advance(); columns.push_back(expect_ident()); }
    if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')'");
    advance();
    bool using_hash = false;
    if (peek_is(TokenKind::Using)) {
        advance();
        const Token* t = advance();
        using_hash = t && t->kind == TokenKind::Ident && to_upper(t->text) == "HASH";
    }
    return Statement(Statement::CreateIndex{index_name, table, columns, using_hash});
}

Statement Parser::parse_drop_index() {
    if (peek_is(TokenKind::If)) {
        advance();
        if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS after IF");
        advance();
    }
    std::string index_name = expect_ident();
    return Statement(Statement::DropIndex{index_name});
}

Statement Parser::parse_create_view() {
    std::string name = expect_ident();
    if (!peek_is(TokenKind::As)) throw ParseError("Expected AS");
    advance();
    std::size_t pos_before_select = pos_;
    if (!peek_is(TokenKind::Select)) throw ParseError("Expected SELECT");
    advance();
    Statement query = parse_select();
    std::size_t pos_end = pos_;
    std::vector<Token> slice(tokens_.begin() + static_cast<std::ptrdiff_t>(pos_before_select),
                              tokens_.begin() + static_cast<std::ptrdiff_t>(pos_end));
    std::string raw_sql = tokens_to_sql(slice);
    return Statement(Statement::CreateView{name, std::make_unique<Statement>(std::move(query)), raw_sql});
}

Statement Parser::parse_drop_view() {
    if (peek_is(TokenKind::If)) {
        advance();
        if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS after IF");
        advance();
    }
    std::string name = expect_ident();
    return Statement(Statement::DropView{name});
}

Statement Parser::parse_create_database() {
    bool if_not_exists = false;
    if (peek_is(TokenKind::If)) {
        advance();
        if (!peek_is(TokenKind::Not)) throw ParseError("Expected NOT after IF");
        advance();
        if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS");
        advance();
        if_not_exists = true;
    }
    std::string name = expect_ident();
    return Statement(Statement::CreateDatabase{name, if_not_exists});
}

Statement Parser::parse_drop_database() {
    bool if_exists = false;
    if (peek_is(TokenKind::If)) {
        advance();
        if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS after IF");
        advance();
        if_exists = true;
    }
    std::string name = expect_ident();
    return Statement(Statement::DropDatabase{name, if_exists});
}

} // namespace engine
