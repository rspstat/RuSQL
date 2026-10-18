#include "engine/parser/parser.hpp"

#include <cctype>
#include <unordered_map>

namespace engine {

const std::string NULL_DEFAULT = "__NULL_DEFAULT__";
const std::string INSERT_DEFAULT = "__INSERT_DEFAULT__";

namespace {
std::string to_upper(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}
} // namespace

// (SELECT ...) UNION (SELECT ...): the parentheses around an operand of a set operation (or around the whole query) change nothing unless the operand has its
// own ORDER BY / LIMIT, so they are taken out; a group that does have one is left, and is refused as it always was
static void drop_parentheses_around_operands(std::vector<Token>& tokens) {
    auto is_set_operator = [](TokenKind k) { return k == TokenKind::Union || k == TokenKind::Intersect || k == TokenKind::Except; };
    std::size_t i = 0;
    while (i < tokens.size()) {
        const bool after_operator = i == 0 || is_set_operator(tokens[i - 1].kind) || (tokens[i - 1].kind == TokenKind::All && i >= 2 && is_set_operator(tokens[i - 2].kind));
        if (tokens[i].kind != TokenKind::LParen || !after_operator || i + 1 >= tokens.size() || tokens[i + 1].kind != TokenKind::Select) {
            i++;
            continue;
        }
        // the matching ')', and whether the group has an ORDER BY / LIMIT / OFFSET of its own at its top level
        int depth = 0;
        std::size_t close = i;
        bool own_order = false;
        for (std::size_t k = i; k < tokens.size(); k++) {
            if (tokens[k].kind == TokenKind::LParen) depth++;
            else if (tokens[k].kind == TokenKind::RParen && --depth == 0) { close = k; break; }
            else if (depth == 1 && (tokens[k].kind == TokenKind::Order || tokens[k].kind == TokenKind::Limit || tokens[k].kind == TokenKind::Offset)) own_order = true;
        }
        const bool closed = close > i && tokens[close].kind == TokenKind::RParen;
        const bool followed_by_more = close + 1 >= tokens.size() || is_set_operator(tokens[close + 1].kind) || tokens[close + 1].kind == TokenKind::Order ||
                                      tokens[close + 1].kind == TokenKind::Limit || tokens[close + 1].kind == TokenKind::Semicolon || tokens[close + 1].kind == TokenKind::RParen;
        if (!closed || own_order || !followed_by_more) {
            i++;
            continue;
        }
        tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(close));
        tokens.erase(tokens.begin() + static_cast<std::ptrdiff_t>(i));
        // (stay at i: the token that is there now may open another group)
    }
}

std::string Parser::source_between(std::size_t from, std::size_t to) const {
    if (from >= to || to > tokens_.size()) return "";
    const std::size_t begin = tokens_[from].begin, end = tokens_[to - 1].end;
    if (end <= begin || end > source_.size()) return "";
    return source_.substr(begin, end - begin);
}

Parser::Parser(const std::string& input, bool pipes_as_concat) : source_(input) {
    Lexer lexer(input);
    tokens_ = lexer.tokenize();
    drop_parentheses_around_operands(tokens_);
    if (!pipes_as_concat) {
        for (Token& t : tokens_) {
            if (t.kind == TokenKind::PipePipe) t.kind = TokenKind::Or; // a || b is a OR b
        }
    }
}

const Token* Parser::peek() const {
    return pos_ < tokens_.size() ? &tokens_[pos_] : nullptr;
}

const Token* Parser::peek_at(std::size_t offset) const {
    std::size_t i = pos_ + offset;
    return i < tokens_.size() ? &tokens_[i] : nullptr;
}

const Token* Parser::advance() {
    const Token* t = peek();
    pos_++;
    return t;
}

bool Parser::peek_is(TokenKind k) const {
    const Token* t = peek();
    return t != nullptr && t->kind == k;
}

bool Parser::peek_at_is(std::size_t offset, TokenKind k) const {
    const Token* t = peek_at(offset);
    return t != nullptr && t->kind == k;
}

Result<Statement, std::string> Parser::parse() {
    try {
        Statement stmt = parse_stmt();
        // PLAN.md P0 fix: parse_stmt() used to return successfully even when tokens
        // remained after a syntactically complete statement (e.g. `SELECT * FROM t)))
        // garbage` silently ran as `SELECT * FROM t`), because nothing ever checked
        // that the whole input was consumed. Only the top-level entry point checks
        // this — parse_stmt() is also called recursively (EXPLAIN's inner statement,
        // stored-procedure body statements), where more tokens are expected to follow.
        if (peek() != nullptr) throw ParseError("Unexpected token(s) after end of statement");
        return Result<Statement, std::string>::Ok(std::move(stmt));
    } catch (const ParseError& e) {
        return Result<Statement, std::string>::Err(e.what());
    }
}

bool Parser::is_plain_word(TokenKind kind) {
    switch (kind) {
        // types
        case TokenKind::Text: case TokenKind::Date: case TokenKind::Datetime: case TokenKind::Timestamp: case TokenKind::Time: case TokenKind::Year:
        case TokenKind::Boolean: case TokenKind::Enum: case TokenKind::Json:
        // aggregate functions
        case TokenKind::Count: case TokenKind::Sum: case TokenKind::Avg: case TokenKind::Min: case TokenKind::Max: case TokenKind::Stddev:
        case TokenKind::Variance: case TokenKind::Median: case TokenKind::BitAnd: case TokenKind::BitOr: case TokenKind::JsonAgg:
        case TokenKind::ArrayAgg: case TokenKind::GroupConcat: case TokenKind::Filter:
        // scalar functions
        case TokenKind::Upper: case TokenKind::Lower: case TokenKind::Length: case TokenKind::Trim: case TokenKind::Concat: case TokenKind::Substr:
        case TokenKind::Substring: case TokenKind::Now: case TokenKind::Curdate: case TokenKind::DateFormat: case TokenKind::Coalesce:
        case TokenKind::Ifnull: case TokenKind::Round: case TokenKind::Abs: case TokenKind::Ceil: case TokenKind::Floor: case TokenKind::Nullif:
        case TokenKind::Lpad: case TokenKind::Rpad: case TokenKind::Cast: case TokenKind::DateAdd: case TokenKind::DateSub: case TokenKind::DateDiff:
        case TokenKind::Truncate:
        // other words
        case TokenKind::Level: case TokenKind::User: case TokenKind::Password: case TokenKind::Role: case TokenKind::Next: case TokenKind::Current:
        case TokenKind::View: case TokenKind::Offset: case TokenKind::Full: case TokenKind::Merge: case TokenKind::Matched: case TokenKind::Following:
        case TokenKind::Preceding: case TokenKind::Unbounded: case TokenKind::Savepoint: case TokenKind::Checkpoint: case TokenKind::Isolation:
        case TokenKind::Uncommitted: case TokenKind::Committed: case TokenKind::Repeatable: case TokenKind::Serializable: case TokenKind::Vacuum:
        case TokenKind::Locks: case TokenKind::Modify: case TokenKind::Duplicate: case TokenKind::Returning: case TokenKind::Prepare:
        case TokenKind::Execute: case TokenKind::Deallocate: case TokenKind::Synonym: case TokenKind::Share: case TokenKind::Only: case TokenKind::After:
        case TokenKind::Do: case TokenKind::Body: case TokenKind::Privileges: case TokenKind::Grants: case TokenKind::Identified: case TokenKind::End:
        case TokenKind::Until: case TokenKind::Handler: case TokenKind::Tables:
            return true;
        default:
            return false;
    }
}

bool Parser::is_bare_function(const Token& t) {
    return (t.kind == TokenKind::Now || t.kind == TokenKind::Curdate) && t.word != "now" && t.word != "curdate";
}

std::string Parser::expect_ident() {
    const Token* t = advance();
    if (t && t->kind == TokenKind::Ident) return t->text;
    if (t && is_plain_word(t->kind) && !t->word.empty()) return t->word;
    throw ParseError("Expected identifier");
}

// A table (or view) name, with the database in front of it when it has one: `db.t`
std::string Parser::expect_table_name() {
    std::string name = expect_ident();
    if (peek_is(TokenKind::Dot)) {
        advance();
        name += "." + expect_any_name();
    }
    return name;
}

std::optional<std::string> Parser::parse_table_alias() {
    if (peek_is(TokenKind::As)) {
        advance();
        return expect_alias_ident();
    }
    if (peek_is(TokenKind::Ident)) {
        // (`t LOCK IN SHARE MODE` is no alias called LOCK)
        if (to_upper(peek()->text) == "LOCK" && peek_at_is(1, TokenKind::In)) return std::nullopt;
        return expect_ident();
    }
    return std::nullopt;
}

std::optional<std::string> Parser::parse_column_alias() {
    if (peek_is(TokenKind::As)) {
        advance();
        return expect_alias_ident();
    }
    if (peek_is(TokenKind::Ident)) return expect_ident();
    return std::nullopt;
}

/// 키워드도 식별자로 허용 (AS alias 위치에서 사용)
std::string Parser::expect_alias_ident() {
    const Token* t = advance();
    if (!t) throw ParseError("Expected identifier (alias)");
    switch (t->kind) {
        case TokenKind::Ident:    return t->text;
        case TokenKind::Now:      return "now";
        case TokenKind::Date:     return "date";
        case TokenKind::Count:    return "count";
        case TokenKind::Sum:      return "sum";
        case TokenKind::Avg:      return "avg";
        case TokenKind::Min:      return "min";
        case TokenKind::Max:      return "max";
        case TokenKind::Key:      return "key";
        case TokenKind::Set:      return "set";
        case TokenKind::Select:   return "select";
        case TokenKind::From:     return "from";
        case TokenKind::Where:    return "where";
        case TokenKind::Table:    return "table";
        case TokenKind::Order:    return "order";
        case TokenKind::Group:    return "group";
        case TokenKind::Index:    return "index";
        case TokenKind::View:     return "view";
        case TokenKind::User:     return "user";
        case TokenKind::Role:     return "role";
        case TokenKind::Row:      return "row";
        case TokenKind::Left:     return "left";
        case TokenKind::Right:    return "right";
        case TokenKind::Time:     return "time";
        case TokenKind::Year:     return "year";
        case TokenKind::Rank:     return "rank";
        case TokenKind::Check:    return "check";
        case TokenKind::Replace:  return "replace";
        case TokenKind::Repeat:   return "repeat";
        case TokenKind::Truncate: return "truncate";
        case TokenKind::Interval: return "interval";
        case TokenKind::Database: return "database";
        case TokenKind::Tables:   return "tables";
        case TokenKind::Column:   return "column";
        case TokenKind::Null:     return "null";
        default:
            if (is_plain_word(t->kind) && !t->word.empty()) return t->word;
            throw ParseError("Expected identifier (alias)");
    }
}

// table.column 형태를 허용하며, 테이블 접두사는 무시하고 컬럼명만 반환
std::string Parser::expect_col_ref() {
    std::string first = expect_any_ident();
    if (peek_is(TokenKind::Dot)) {
        advance();
        std::string col = expect_any_name();
        std::string name = first + "." + col; // table.column 전체 보존
        // database.table.column
        if (peek_is(TokenKind::Dot) && peek_at(1) && peek_at(1)->kind != TokenKind::Asterisk && peek_at(1)->kind != TokenKind::LParen) {
            advance();
            name += "." + expect_any_name();
        }
        return name;
    }
    return first;
}

/// Any token that can serve as an identifier in dotted names (schema.table, table.column).
std::string Parser::expect_any_name() {
    const Token* t = advance();
    if (!t) throw ParseError("Expected identifier");
    switch (t->kind) {
        case TokenKind::Ident:    return t->text;
        case TokenKind::Tables:   return "tables";
        case TokenKind::Column:   return "column";
        case TokenKind::Database: return "schema";
        case TokenKind::Index:    return "index";
        case TokenKind::View:     return "view";
        case TokenKind::Key:      return "key";
        case TokenKind::Set:      return "set";
        case TokenKind::Count:    return "count";
        case TokenKind::Sum:      return "sum";
        case TokenKind::Avg:      return "avg";
        case TokenKind::Min:      return "min";
        case TokenKind::Max:      return "max";
        case TokenKind::User:     return "user";
        case TokenKind::Role:     return "role";
        case TokenKind::Row:      return "row";
        case TokenKind::Order:    return "order";
        case TokenKind::Group:    return "group";
        case TokenKind::Left:     return "left";
        case TokenKind::Right:    return "right";
        case TokenKind::Date:     return "date";
        case TokenKind::Time:     return "time";
        case TokenKind::Year:     return "year";
        case TokenKind::Repeat:   return "repeat";
        case TokenKind::Replace:  return "replace";
        case TokenKind::Truncate: return "truncate";
        case TokenKind::Now:      return "now";
        case TokenKind::Rank:     return "rank";
        case TokenKind::Check:    return "check";
        case TokenKind::Interval: return "interval";
        default:
            if (is_plain_word(t->kind) && !t->word.empty()) return t->word;
            throw ParseError("Expected identifier");
    }
}

/// 식별자 또는 alias로 쓰이는 키워드 모두 허용 (ORDER BY / GROUP BY 컬럼명 파싱용)
std::string Parser::expect_any_ident() {
    const Token* t = advance();
    if (!t) throw ParseError("Expected identifier");
    switch (t->kind) {
        case TokenKind::Ident:    return t->text;
        case TokenKind::Count:    return "count";
        case TokenKind::Sum:      return "sum";
        case TokenKind::Avg:      return "avg";
        case TokenKind::Min:      return "min";
        case TokenKind::Max:      return "max";
        case TokenKind::Now:      return "now";
        case TokenKind::Date:     return "date";
        case TokenKind::Key:      return "key";
        case TokenKind::Set:      return "set";
        case TokenKind::Index:    return "index";
        case TokenKind::View:     return "view";
        case TokenKind::User:     return "user";
        case TokenKind::Role:     return "role";
        case TokenKind::Row:      return "row";
        case TokenKind::Order:    return "order";
        case TokenKind::Group:    return "group";
        case TokenKind::Left:     return "left";
        case TokenKind::Right:    return "right";
        case TokenKind::Time:     return "time";
        case TokenKind::Year:     return "year";
        case TokenKind::Repeat:   return "repeat";
        case TokenKind::Replace:  return "replace";
        case TokenKind::Truncate: return "truncate";
        case TokenKind::Rank:     return "rank";
        case TokenKind::Check:    return "check";
        case TokenKind::Interval: return "interval";
        case TokenKind::Tables:   return "tables";
        case TokenKind::Column:   return "column";
        case TokenKind::Database: return "schema";
        default:
            if (is_plain_word(t->kind) && !t->word.empty()) return t->word;
            throw ParseError("Expected identifier");
    }
}

Statement Parser::parse_stmt() {
    const Token* t = advance();
    if (!t) throw ParseError("Unknown statement: end of input");

    switch (t->kind) {
        case TokenKind::Select: return parse_select();
        case TokenKind::Insert: return parse_insert();
        // At statement-start, REPLACE can only mean REPLACE INTO -- the REPLACE(str, from,
        // to) string function only ever appears inside an expression parse, never here.
        case TokenKind::Replace: return parse_replace();
        case TokenKind::Update: return parse_update();
        case TokenKind::Delete: return parse_delete();
        case TokenKind::Create: {
            skip_create_attributes();
            if (peek_is(TokenKind::Unique) && peek_at_is(1, TokenKind::Index)) { advance(); advance(); return parse_create_unique_index(); }
            if (peek_is(TokenKind::Index))    { advance(); return parse_create_index(); }
            if (peek_is(TokenKind::View))     { advance(); return parse_create_view(); }
            if (peek_is(TokenKind::Database)) { advance(); return parse_create_database(); }
            if (peek_is(TokenKind::User))     { advance(); return parse_create_user(); }
            if (peek_is(TokenKind::Procedure)){ advance(); return parse_create_procedure(); }
            if (peek_is(TokenKind::Trigger))  { advance(); return parse_create_trigger(); }
            if (const Token* p = peek(); p && p->kind == TokenKind::Ident && to_upper(p->text) == "FUNCTION") {
                advance();
                return parse_create_function();
            }
            if (peek_is(TokenKind::Role)) {
                advance();
                std::string name = expect_ident();
                return Statement(Statement::CreateRole{name});
            }
            if (peek_is(TokenKind::Synonym)) {
                advance();
                std::string name = expect_ident();
                if (!peek_is(TokenKind::For)) throw ParseError("Expected FOR after SYNONYM name");
                advance();
                std::string target = expect_ident();
                return Statement(Statement::CreateSynonym{name, target, false});
            }
            if (peek_is(TokenKind::Or)) {
                // CREATE OR REPLACE SYNONYM name FOR target
                advance(); // OR
                const Token* rep = advance();
                bool ok_replace = rep && (rep->kind == TokenKind::Replace ||
                                          (rep->kind == TokenKind::Ident && to_upper(rep->text) == "REPLACE"));
                if (!ok_replace) throw ParseError("Expected REPLACE");
                skip_create_attributes();
                if (peek_is(TokenKind::View)) { advance(); return parse_create_view(); } // (a view of that name is replaced, as CREATE VIEW does)
                if (!peek_is(TokenKind::Synonym)) throw ParseError("Expected SYNONYM or VIEW after CREATE OR REPLACE");
                advance();
                std::string name = expect_ident();
                if (!peek_is(TokenKind::For)) throw ParseError("Expected FOR");
                advance();
                std::string target = expect_ident();
                return Statement(Statement::CreateSynonym{name, target, true});
            }
            return parse_create();
        }
        case TokenKind::Drop: {
            if (peek_is(TokenKind::Index))    { advance(); return parse_drop_index(); }
            if (peek_is(TokenKind::View))     { advance(); return parse_drop_view(); }
            if (peek_is(TokenKind::Database)) { advance(); return parse_drop_database(); }
            if (peek_is(TokenKind::User))     { advance(); return parse_drop_user(); }
            if (peek_is(TokenKind::Trigger))  { advance(); return parse_drop_trigger(); }
            if (peek_is(TokenKind::Procedure)){ advance(); return parse_drop_procedure(); }
            if (const Token* p = peek(); p && p->kind == TokenKind::Ident && to_upper(p->text) == "FUNCTION") {
                advance();
                return parse_drop_function();
            }
            if (peek_is(TokenKind::Role)) {
                advance();
                bool if_exists = false;
                if (peek_is(TokenKind::If)) {
                    advance();
                    if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS after IF");
                    advance();
                    if_exists = true;
                }
                std::string name = expect_ident();
                return Statement(Statement::DropRole{name, if_exists});
            }
            if (peek_is(TokenKind::Synonym)) {
                advance();
                bool if_exists = false;
                if (peek_is(TokenKind::If)) {
                    advance();
                    if (!peek_is(TokenKind::Exists)) throw ParseError("Expected EXISTS after IF");
                    advance();
                    if_exists = true;
                }
                std::string name = expect_ident();
                return Statement(Statement::DropSynonym{name, if_exists});
            }
            return parse_drop();
        }
        case TokenKind::Grant:  return parse_grant();
        case TokenKind::Revoke: return parse_revoke();
        case TokenKind::Ident: {
            auto work_word = [&]() {
                if (const Token* w = peek(); w && w->kind == TokenKind::Ident && to_upper(w->text) == "WORK") advance();
            };
            if (to_upper(t->text) == "BEGIN") {
                work_word();
                return Statement(Statement::Begin{});
            }
            if (to_upper(t->text) == "START") { // START TRANSACTION [READ WRITE | READ ONLY | WITH CONSISTENT SNAPSHOT ...]
                const Token* w = peek();
                if (!w || w->kind != TokenKind::Ident || to_upper(w->text) != "TRANSACTION") throw ParseError("Expected TRANSACTION after START");
                advance();
                while (peek() && !peek_is(TokenKind::Semicolon)) advance();
                return Statement(Statement::Begin{});
            }
            if (to_upper(t->text) == "COMMIT") {
                work_word();
                return Statement(Statement::Commit{});
            }
            if (to_upper(t->text) == "ROLLBACK") {
                work_word();
                // ROLLBACK TO [SAVEPOINT] name
                if (peek_is(TokenKind::To)) {
                    advance();
                    if (peek_is(TokenKind::Savepoint)) advance();
                    std::string name = expect_ident();
                    return Statement(Statement::RollbackTo{name});
                }
                return Statement(Statement::Rollback{});
            }
            if (to_upper(t->text) == "BACKUP") return parse_backup();
            if (to_upper(t->text) == "RESTORE") return parse_restore();
            if (to_upper(t->text) == "LOCK") return parse_lock_tables();
            if (to_upper(t->text) == "UNLOCK") {
                if (!peek_is(TokenKind::Tables)) throw ParseError("Expected TABLES after UNLOCK");
                advance();
                return Statement(Statement::UnlockTables{});
            }
            throw ParseError("Unknown statement: " + t->text);
        }
        case TokenKind::Savepoint: {
            std::string name = expect_ident();
            return Statement(Statement::Savepoint{name});
        }
        case TokenKind::Release: {
            if (peek_is(TokenKind::Savepoint)) advance();
            std::string name = expect_ident();
            return Statement(Statement::ReleaseSavepoint{name});
        }
        case TokenKind::Analyze: {
            if (!peek_is(TokenKind::Table)) throw ParseError("Expected TABLE after ANALYZE");
            advance();
            std::string table = expect_ident();
            return Statement(Statement::AnalyzeTable{table});
        }
        case TokenKind::Explain: {
            if (peek_is(TokenKind::Analyze)) {
                advance();
                Statement inner = parse_stmt();
                return Statement(Statement::ExplainAnalyze{std::make_unique<Statement>(std::move(inner))});
            }
            Statement inner = parse_stmt();
            return Statement(Statement::Explain{std::make_unique<Statement>(std::move(inner))});
        }
        case TokenKind::Alter:
            if (peek_is(TokenKind::View)) { advance(); return parse_create_view(); } // ALTER VIEW v AS SELECT ... (a view is made again)
            return parse_alter();
        case TokenKind::Show:     return parse_show();
        case TokenKind::Describe: return parse_describe();
        case TokenKind::Truncate: return parse_truncate();
        case TokenKind::Rename: return parse_rename();
        case TokenKind::Checkpoint: return Statement(Statement::Checkpoint{});
        case TokenKind::Set:      return parse_set();
        case TokenKind::Vacuum:   return parse_vacuum();
        case TokenKind::With:     return parse_with();
        case TokenKind::Use:      return parse_use();
        case TokenKind::Merge:    return parse_merge();
        case TokenKind::Call:     return parse_call();
        case TokenKind::Prepare:  return parse_prepare();
        case TokenKind::Execute:  return parse_execute();
        case TokenKind::Deallocate: return parse_deallocate();
        default: throw ParseError("Unknown statement");
    }
}

namespace {
// WITH r (a, b) AS (SELECT ...): the list names the columns of the CTE, so the select list of its query (the first one of a UNION) is given those names
void name_select_columns(Statement& stmt, const std::vector<std::string>& names) {
    if (auto* u = std::get_if<Statement::Union>(&stmt.data)) return name_select_columns(*u->left, names);
    if (auto* i = std::get_if<Statement::Intersect>(&stmt.data)) return name_select_columns(*i->left, names);
    if (auto* x = std::get_if<Statement::Except>(&stmt.data)) return name_select_columns(*x->left, names);
    auto* sel = std::get_if<Statement::Select>(&stmt.data);
    if (!sel) throw ParseError("A column list needs a SELECT");
    for (auto& col : sel->columns) {
        if (std::holds_alternative<SelectColumn::All>(col.data)) throw ParseError("A column list on a query that selects * is not supported (name the columns)");
    }
    if (sel->columns.size() != names.size()) {
        throw ParseError("ERROR 1353 (HY000): In definition of view, derived table or common table expression, SELECT list and column list have different column counts");
    }
    for (std::size_t k = 0; k < names.size(); k++) {
        const std::string& n = names[k];
        std::optional<SelectColumn> renamed; // (an item that has no alias slot is replaced by its aliased twin once the visit is over)
        std::visit(
            [&](auto& c) {
                using T = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<T, SelectColumn::Column>) renamed = SelectColumn(SelectColumn::ColumnAlias{c.name, n, c.cls, c.outer});
                else if constexpr (std::is_same_v<T, SelectColumn::Agg>) renamed = SelectColumn(SelectColumn::AggAlias{c.func, c.col, n, c.filter, c.source, c.arg_class});
                else if constexpr (!std::is_same_v<T, SelectColumn::All>) c.alias = n; // (ColumnAlias, AggAlias, Func, Expr, CaseWhen, WinFunc, Subquery)
            },
            sel->columns[k].data);
        if (renamed) sel->columns[k] = std::move(*renamed);
    }
}
} // namespace

void Parser::skip_create_attributes() {
    auto word = [&](std::size_t at, const char* w) {
        const Token* t = peek_at(at);
        return t && t->kind == TokenKind::Ident && to_upper(t->text) == w;
    };
    for (;;) {
        if (word(0, "ALGORITHM")) { // ALGORITHM = UNDEFINED | MERGE | TEMPTABLE
            advance();
            if (peek_is(TokenKind::Eq)) advance();
            if (peek()) advance();
        } else if (word(0, "DEFINER")) { // DEFINER = user@host | CURRENT_USER[()]
            advance();
            if (peek_is(TokenKind::Eq)) advance();
            if (peek()) advance();
            if (peek_is(TokenKind::At)) {
                advance();
                if (peek()) advance();
            } else if (peek_is(TokenKind::LParen) && peek_at_is(1, TokenKind::RParen)) {
                advance();
                advance();
            }
        } else if (word(0, "SQL") && word(1, "SECURITY")) { // SQL SECURITY DEFINER | INVOKER
            advance();
            advance();
            if (peek()) advance();
        } else {
            return;
        }
    }
}

Statement Parser::parse_use() {
    // USE [DATABASE] name
    if (peek_is(TokenKind::Database)) advance();
    std::string database = expect_ident();
    return Statement(Statement::Use{database});
}

Statement Parser::parse_with() {
    // WITH [RECURSIVE] cte_name AS (query) [, cte_name AS (query)] ... SELECT ...
    bool recursive = false;
    if (peek_is(TokenKind::Recursive)) { advance(); recursive = true; }

    std::vector<std::pair<std::string, StatementPtr>> ctes;
    for (;;) {
        std::string name = expect_ident();
        std::vector<std::string> column_list; // WITH r (a, b) AS (...)
        if (peek_is(TokenKind::LParen)) {
            advance();
            for (;;) {
                column_list.push_back(expect_ident());
                if (peek_is(TokenKind::Comma)) { advance(); continue; }
                break;
            }
            if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after the CTE column list");
            advance();
        }
        if (!peek_is(TokenKind::As)) throw ParseError("Expected AS in CTE");
        advance();
        if (!peek_is(TokenKind::LParen)) throw ParseError("Expected '(' in CTE");
        advance();
        if (!peek_is(TokenKind::Select)) throw ParseError("Expected SELECT in CTE body");
        advance();
        Statement base = parse_select();
        if (!column_list.empty()) name_select_columns(base, column_list);
        // 재귀 CTE: body 내 UNION [ALL] 처리
        Statement body = [&]() -> Statement {
            if (peek_is(TokenKind::Union)) {
                advance(); // consume UNION
                bool all = false;
                if (peek_is(TokenKind::All)) { advance(); all = true; }
                if (!peek_is(TokenKind::Select)) throw ParseError("Expected SELECT after UNION in CTE");
                advance();
                Statement recursive_part = parse_select();
                return Statement(Statement::Union{
                    std::make_unique<Statement>(std::move(base)),
                    std::make_unique<Statement>(std::move(recursive_part)),
                    all, {}, std::nullopt, std::nullopt});
            }
            return base;
        }();
        if (!peek_is(TokenKind::RParen)) throw ParseError("Expected ')' after CTE body");
        advance();
        ctes.emplace_back(name, std::make_unique<Statement>(std::move(body)));
        if (peek_is(TokenKind::Comma)) advance(); else break;
    }

    // Main query (any DML: SELECT, INSERT ... SELECT, etc.)
    Statement query = parse_stmt();
    return Statement(Statement::With{std::move(ctes), std::make_unique<Statement>(std::move(query)), recursive});
}

} // namespace engine
