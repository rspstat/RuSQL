#include "engine/parser/lexer.hpp"

#include <cctype>
#include <unordered_map>

namespace engine {

namespace {

bool is_space(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }
bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }
bool is_hexdigit(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
bool is_alpha(char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }
bool is_alnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

char to_upper(char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); }

std::string to_upper_str(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out.push_back(to_upper(c));
    return out;
}

const std::unordered_map<std::string, TokenKind>& keyword_map() {
    static const std::unordered_map<std::string, TokenKind> kMap = {
        {"SELECT", TokenKind::Select}, {"FROM", TokenKind::From}, {"WHERE", TokenKind::Where},
        {"INSERT", TokenKind::Insert}, {"INTO", TokenKind::Into}, {"VALUES", TokenKind::Values},
        {"UPDATE", TokenKind::Update}, {"SET", TokenKind::Set}, {"DELETE", TokenKind::Delete},
        {"CREATE", TokenKind::Create}, {"TABLE", TokenKind::Table}, {"DROP", TokenKind::Drop},
        {"JOIN", TokenKind::Join}, {"LEFT", TokenKind::Left}, {"RIGHT", TokenKind::Right},
        {"CROSS", TokenKind::Cross}, {"NATURAL", TokenKind::Natural}, {"OUTER", TokenKind::Outer},
        {"FULL", TokenKind::Full}, {"RETURNING", TokenKind::Returning}, {"ON", TokenKind::On},
        {"AND", TokenKind::And}, {"OR", TokenKind::Or}, {"NOT", TokenKind::Not},
        {"ALTER", TokenKind::Alter}, {"ADD", TokenKind::Add}, {"COLUMN", TokenKind::Column},
        {"RENAME", TokenKind::Rename}, {"TO", TokenKind::To}, {"ORDER", TokenKind::Order},
        {"GROUP", TokenKind::Group}, {"BY", TokenKind::By}, {"ASC", TokenKind::Asc},
        {"DESC", TokenKind::Desc}, {"LIMIT", TokenKind::Limit}, {"COUNT", TokenKind::Count},
        {"SUM", TokenKind::Sum}, {"AVG", TokenKind::Avg}, {"MIN", TokenKind::Min},
        {"MAX", TokenKind::Max}, {"HAVING", TokenKind::Having}, {"IN", TokenKind::In},
        {"BETWEEN", TokenKind::Between}, {"LIKE", TokenKind::Like}, {"INDEX", TokenKind::Index},
        {"UNIQUE", TokenKind::Unique}, {"VIEW", TokenKind::View}, {"AS", TokenKind::As},
        {"PRIMARY", TokenKind::Primary}, {"KEY", TokenKind::Key}, {"NULL", TokenKind::Null},
        {"AUTO", TokenKind::Auto}, {"INCREMENT", TokenKind::Increment}, {"SHOW", TokenKind::Show},
        {"TABLES", TokenKind::Tables}, {"DESCRIBE", TokenKind::Describe}, {"TRUNCATE", TokenKind::Truncate},
        {"INT", TokenKind::Int}, {"TEXT", TokenKind::Text}, {"FLOAT", TokenKind::Float},
        {"BOOLEAN", TokenKind::Boolean}, {"REFERENCES", TokenKind::References},
        {"FOREIGN", TokenKind::Foreign}, {"CONSTRAINT", TokenKind::Constraint},
        {"CASCADE", TokenKind::Cascade}, {"RESTRICT", TokenKind::Restrict}, {"IS", TokenKind::Is},
        {"CHECKPOINT", TokenKind::Checkpoint}, {"ISOLATION", TokenKind::Isolation},
        {"LEVEL", TokenKind::Level}, {"UNCOMMITTED", TokenKind::Uncommitted},
        {"COMMITTED", TokenKind::Committed}, {"REPEATABLE", TokenKind::Repeatable},
        {"SERIALIZABLE", TokenKind::Serializable}, {"VACUUM", TokenKind::Vacuum},
        {"FOR", TokenKind::For}, {"LOCKS", TokenKind::Locks}, {"DISTINCT", TokenKind::Distinct},
        {"DEFAULT", TokenKind::Default}, {"MODIFY", TokenKind::Modify}, {"EXISTS", TokenKind::Exists},
        {"SAVEPOINT", TokenKind::Savepoint}, {"RELEASE", TokenKind::Release},
        {"EXPLAIN", TokenKind::Explain}, {"VARCHAR", TokenKind::Varchar}, {"DATE", TokenKind::Date},
        {"DATETIME", TokenKind::Datetime}, {"TIMESTAMP", TokenKind::Timestamp},
        {"DECIMAL", TokenKind::Decimal}, {"DOUBLE", TokenKind::Double}, {"TIME", TokenKind::Time},
        {"YEAR", TokenKind::Year}, {"BLOB", TokenKind::Blob}, {"ENUM", TokenKind::Enum},
        {"DATABASE", TokenKind::Database}, {"INNER", TokenKind::Inner}, {"SCHEMA", TokenKind::Database},
        {"CHECK", TokenKind::Check}, {"UPPER", TokenKind::Upper}, {"LOWER", TokenKind::Lower},
        {"LENGTH", TokenKind::Length}, {"TRIM", TokenKind::Trim}, {"CONCAT", TokenKind::Concat},
        {"SUBSTR", TokenKind::Substr}, {"SUBSTRING", TokenKind::Substring}, {"NOW", TokenKind::Now},
        {"CURDATE", TokenKind::Curdate}, {"DATE_FORMAT", TokenKind::DateFormat},
        {"COALESCE", TokenKind::Coalesce}, {"IFNULL", TokenKind::Ifnull}, {"REPLACE", TokenKind::Replace},
        {"ROUND", TokenKind::Round}, {"ABS", TokenKind::Abs}, {"CEIL", TokenKind::Ceil},
        {"FLOOR", TokenKind::Floor}, {"MOD", TokenKind::Mod}, {"INTERVAL", TokenKind::Interval},
        {"CASE", TokenKind::Case}, {"WHEN", TokenKind::When}, {"THEN", TokenKind::Then},
        {"ELSE", TokenKind::Else}, {"END", TokenKind::End}, {"UNION", TokenKind::Union},
        {"INTERSECT", TokenKind::Intersect}, {"EXCEPT", TokenKind::Except}, {"ALL", TokenKind::All},
        {"IF", TokenKind::If}, {"GROUP_CONCAT", TokenKind::GroupConcat}, {"IGNORE", TokenKind::Ignore},
        {"DUPLICATE", TokenKind::Duplicate}, {"NULLIF", TokenKind::Nullif}, {"LPAD", TokenKind::Lpad},
        {"RPAD", TokenKind::Rpad}, {"CAST", TokenKind::Cast}, {"DATE_ADD", TokenKind::DateAdd},
        {"DATE_SUB", TokenKind::DateSub}, {"DATEDIFF", TokenKind::DateDiff},
        {"SEPARATOR", TokenKind::Separator}, {"OFFSET", TokenKind::Offset}, {"WITH", TokenKind::With},
        {"RECURSIVE", TokenKind::Recursive}, {"USE", TokenKind::Use}, {"USER", TokenKind::User},
        {"IDENTIFIED", TokenKind::Identified}, {"GRANT", TokenKind::Grant}, {"REVOKE", TokenKind::Revoke},
        {"PRIVILEGES", TokenKind::Privileges}, {"GRANTS", TokenKind::Grants},
        {"OPTION", TokenKind::OptionKw}, {"PASSWORD", TokenKind::Password},
        {"DATABASES", TokenKind::Databases}, {"ANALYZE", TokenKind::Analyze},
        {"STDDEV", TokenKind::Stddev}, {"STD", TokenKind::Stddev}, {"STDDEV_POP", TokenKind::Stddev},
        {"VARIANCE", TokenKind::Variance}, {"VAR_POP", TokenKind::Variance},
        {"BIT_AND", TokenKind::BitAnd}, {"BIT_OR", TokenKind::BitOr}, {"JSON_AGG", TokenKind::JsonAgg},
        {"ARRAY_AGG", TokenKind::ArrayAgg}, {"FILTER", TokenKind::Filter},
        {"LATERAL", TokenKind::Lateral},
        {"NTILE", TokenKind::Ntile}, {"PERCENT_RANK", TokenKind::PercentRank},
        {"CUME_DIST", TokenKind::CumeDist}, {"ROW_NUMBER", TokenKind::RowNumber},
        {"REGEXP", TokenKind::Regexp}, {"RLIKE", TokenKind::Regexp}, {"MERGE", TokenKind::Merge},
        {"USING", TokenKind::Using}, {"MATCHED", TokenKind::Matched}, {"PREPARE", TokenKind::Prepare},
        {"EXECUTE", TokenKind::Execute}, {"DEALLOCATE", TokenKind::Deallocate},
        {"FETCH", TokenKind::Fetch}, {"NEXT", TokenKind::Next}, {"ONLY", TokenKind::Only},
        {"ROLE", TokenKind::Role}, {"SYNONYM", TokenKind::Synonym}, {"PROCEDURE", TokenKind::Procedure},
        {"CALL", TokenKind::Call}, {"TRIGGER", TokenKind::Trigger}, {"BEFORE", TokenKind::Before},
        {"AFTER", TokenKind::After}, {"EACH", TokenKind::Each}, {"ROW", TokenKind::Row},
        {"NEW", TokenKind::NewKw}, {"OLD", TokenKind::OldKw}, {"BODY", TokenKind::Body},
        {"SIGNAL", TokenKind::Signal}, {"DECLARE", TokenKind::Declare}, {"HANDLER", TokenKind::Handler},
        {"LEAVE", TokenKind::Leave}, {"ITERATE", TokenKind::Iterate}, {"LOOP", TokenKind::Loop},
        {"REPEAT", TokenKind::Repeat}, {"UNTIL", TokenKind::Until}, {"WHILE", TokenKind::While},
        {"DO", TokenKind::Do}, {"ELSEIF", TokenKind::ElseIfKw}, {"ROWS", TokenKind::Rows},
        {"RANGE", TokenKind::Range}, {"UNBOUNDED", TokenKind::Unbounded},
        {"PRECEDING", TokenKind::Preceding}, {"FOLLOWING", TokenKind::Following},
        {"CURRENT", TokenKind::Current}, {"FIRST_VALUE", TokenKind::FirstValue},
        {"LAST_VALUE", TokenKind::LastValue}, {"NTH_VALUE", TokenKind::NthValue},
        {"RANK", TokenKind::Rank}, {"DENSE_RANK", TokenKind::DenseRank}, {"LAG", TokenKind::Lag},
        {"LEAD", TokenKind::Lead}, {"OVER", TokenKind::Over}, {"PARTITION", TokenKind::Partition},
        {"BIGINT", TokenKind::BigInt}, {"SMALLINT", TokenKind::SmallInt}, {"TINYINT", TokenKind::TinyInt},
        {"JSON", TokenKind::Json}, {"JSON_EXTRACT", TokenKind::JsonExtract},
        {"JSON_UNQUOTE", TokenKind::JsonUnquote}, {"JSON_VALUE", TokenKind::JsonValue},
        {"SHARE", TokenKind::Share},
        // Type aliases
        {"INTEGER", TokenKind::Int}, {"INT4", TokenKind::Int}, {"INT8", TokenKind::BigInt},
        {"MEDIUMINT", TokenKind::Int}, {"INT2", TokenKind::Int}, {"INT3", TokenKind::Int},
        {"LONGTEXT", TokenKind::Text}, {"MEDIUMTEXT", TokenKind::Text}, {"TINYTEXT", TokenKind::Text},
        {"CLOB", TokenKind::Text}, {"LONGBLOB", TokenKind::Text}, {"MEDIUMBLOB", TokenKind::Text},
        {"TINYBLOB", TokenKind::Text},
        {"NVARCHAR", TokenKind::Varchar}, {"NCHAR", TokenKind::Varchar},
        {"CHARACTER", TokenKind::Varchar}, {"CHAR", TokenKind::Varchar},
        {"NUMERIC", TokenKind::Decimal}, {"REAL", TokenKind::Float}, {"BOOL", TokenKind::Boolean},
        {"UNKNOWN", TokenKind::Null},
        // Additional aliases
        {"CURRENT_DATE", TokenKind::Curdate}, {"CURRENT_TIME", TokenKind::Curdate},
        {"CURRENT_TIMESTAMP", TokenKind::Now}, {"SYSDATE", TokenKind::Now},
        {"LOCALTIME", TokenKind::Now}, {"LOCALTIMESTAMP", TokenKind::Now},
    };
    return kMap;
}

bool is_value_or_close_paren(const Token& t) {
    return t.kind == TokenKind::NumberLit || t.kind == TokenKind::Ident ||
           t.kind == TokenKind::StringLit || t.kind == TokenKind::RParen;
}

} // namespace

Lexer::Lexer(const std::string& input) : input_(input), pos_(0) {}

std::optional<char> Lexer::peek() const {
    if (pos_ < input_.size()) return input_[pos_];
    return std::nullopt;
}

std::optional<char> Lexer::advance() {
    auto ch = peek();
    pos_++;
    return ch;
}

void Lexer::skip_whitespace() {
    while (auto ch = peek()) {
        if (is_space(*ch)) advance(); else break;
    }
}

Token Lexer::read_string() {
    advance(); // consume opening quote
    std::string s;
    while (auto ch = peek()) {
        if (*ch == '\'') {
            advance();
            if (peek() == '\'') {
                advance();
                s.push_back('\'');
            } else {
                break;
            }
        } else if (*ch == '\\') {
            advance();
            auto esc = peek();
            if (!esc) break;
            switch (*esc) {
                case '\'': advance(); s.push_back('\''); break;
                case '\\': advance(); s.push_back('\\'); break;
                case 'n':  advance(); s.push_back('\n'); break;
                case 'r':  advance(); s.push_back('\r'); break;
                case 't':  advance(); s.push_back('\t'); break;
                case '0':  advance(); s.push_back('\0'); break;
                default:   advance(); s.push_back(*esc); break;
            }
        } else {
            s.push_back(*ch);
            advance();
        }
    }
    return Token(TokenKind::StringLit, std::move(s));
}

Token Lexer::read_quoted_ident(char closing) {
    advance(); // consume opening quote
    std::string s;
    while (auto ch = peek()) {
        if (*ch == closing) {
            advance();
            break;
        }
        s.push_back(*ch);
        advance();
    }
    // Always treat backtick/double-quote-quoted names as identifiers, never as keywords.
    return Token(TokenKind::Ident, std::move(s));
}

Token Lexer::read_number() {
    std::string s;
    while (auto ch = peek()) {
        if (is_digit(*ch) || *ch == '.') { s.push_back(*ch); advance(); } else break;
    }
    // 0x... hex literal → StringLit(hex_digits)
    if (s == "0") {
        if (auto x = peek()) {
            if (*x == 'x' || *x == 'X') {
                advance();
                std::string hex;
                while (auto ch = peek()) {
                    if (is_hexdigit(*ch)) { hex.push_back(*ch); advance(); } else break;
                }
                return Token(TokenKind::StringLit, std::move(hex));
            }
        }
    }
    return Token(TokenKind::NumberLit, std::move(s));
}

Token Lexer::read_ident() {
    std::string s;
    while (auto ch = peek()) {
        if (is_alnum(*ch) || *ch == '_') { s.push_back(*ch); advance(); } else break;
    }
    const std::string upper = to_upper_str(s);

    // Boolean literals → normalize to lowercase ident for consistent handling
    // (matches original char-for-char, not just semantically).
    if (upper == "TRUE") return Token(TokenKind::Ident, "true");
    if (upper == "FALSE") return Token(TokenKind::Ident, "false");

    const auto& map = keyword_map();
    auto it = map.find(upper);
    if (it != map.end()) return Token(it->second);
    return Token(TokenKind::Ident, std::move(s));
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> tokens;
    for (;;) {
        skip_whitespace();
        auto chOpt = peek();
        if (!chOpt) break;
        char ch = *chOpt;

        std::optional<Token> tok;

        switch (ch) {
            case '-': {
                advance();
                if (peek() == '-') {
                    // -- 한 줄 주석: 줄 끝까지 건너뜀
                    while (auto c = peek()) {
                        advance();
                        if (*c == '\n') break;
                    }
                    continue;
                } else if (peek() == '>') {
                    advance(); // consume '>'
                    if (peek() == '>') {
                        advance();
                        tok = Token(TokenKind::LongArrow);
                    } else {
                        tok = Token(TokenKind::Arrow);
                    }
                } else if (peek().has_value() && is_digit(*peek()) &&
                           !(!tokens.empty() && is_value_or_close_paren(tokens.back()))) {
                    // 음수 리터럴: -숫자 (단, 직전 토큰이 값/식별자/')'가 아닐 때만.
                    std::string s = "-";
                    while (auto c = peek()) {
                        if (is_digit(*c) || *c == '.') { s.push_back(*c); advance(); } else break;
                    }
                    tok = Token(TokenKind::NumberLit, std::move(s));
                } else {
                    tok = Token(TokenKind::Minus);
                }
                break;
            }
            case '+': advance(); tok = Token(TokenKind::Plus); break;
            case '@': advance(); tok = Token(TokenKind::At); break;
            case '#': {
                while (auto c = peek()) {
                    advance();
                    if (*c == '\n') break;
                }
                continue;
            }
            case '/': {
                advance();
                if (peek() == '*') {
                    advance(); // consume '*'
                    for (;;) {
                        auto c = advance();
                        if (!c) break;
                        if (*c == '*' && peek() == '/') { advance(); break; }
                    }
                    continue;
                } else {
                    tok = Token(TokenKind::Slash);
                }
                break;
            }
            case '*': advance(); tok = Token(TokenKind::Asterisk); break;
            case ',': advance(); tok = Token(TokenKind::Comma); break;
            case ';': advance(); tok = Token(TokenKind::Semicolon); break;
            case '(': advance(); tok = Token(TokenKind::LParen); break;
            case ')': advance(); tok = Token(TokenKind::RParen); break;
            case '.': advance(); tok = Token(TokenKind::Dot); break;
            case '=': advance(); tok = Token(TokenKind::Eq); break;
            case '>': {
                advance();
                if (peek() == '=') { advance(); tok = Token(TokenKind::Gte); }
                else tok = Token(TokenKind::Gt);
                break;
            }
            case '<': {
                advance();
                if (peek() == '=') { advance(); tok = Token(TokenKind::Lte); }
                else if (peek() == '>') { advance(); tok = Token(TokenKind::Ne); }
                else tok = Token(TokenKind::Lt);
                break;
            }
            case '!': {
                advance();
                if (peek() == '=') { advance(); tok = Token(TokenKind::Ne); }
                else continue;
                break;
            }
            case '%': advance(); tok = Token(TokenKind::Percent); break;
            case '|': {
                advance();
                if (peek() == '|') { advance(); tok = Token(TokenKind::PipePipe); }
                else continue;
                break;
            }
            case '`': tok = read_quoted_ident('`'); break;
            case '"': tok = read_quoted_ident('"'); break;
            case '\'': tok = read_string(); break;
            default:
                if (is_digit(ch)) { tok = read_number(); break; }
                if (is_alpha(ch) || ch == '_') { tok = read_ident(); break; }
                advance();
                continue;
        }

        tokens.push_back(std::move(*tok));
    }
    return tokens;
}

} // namespace engine
