// A CHECK condition the way MySQL writes it (R89): SHOW CREATE TABLE's `CHECK ((`qty` >= 0))` and information_schema.check_constraints.CHECK_CLAUSE. MySQL does not keep the
// text that was typed: it prints the condition it parsed -- columns back-quoted as the table spells them, every comparison and every AND / OR in its own brackets (a chain of the same
// AND / OR in one pair), the words in lower case, a string with its character set (`_utf8mb4'a'`), a number as typed (an integer without its leading zeros, a negative one as
// `-(5)`), a function by its own name (`lcase` is `lower`, `ceil` is `ceiling`, `substring` is `substr`, `round(x)` is `round(x,0)`, `mod(a, b)` is `(a % b)`), and a NOT
// pushed in (`NOT (a > 3)` is `(a <= 3)`, `NOT (a AND b)` is `((not a) or (not b))` written with the opposite comparisons). What is not written here is left to the caller, which shows it as
// it was typed.

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <vector>

#include "engine/executor/executor.hpp"
#include "engine/parser/parser.hpp"

namespace engine {

namespace {

std::string lower_text(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string upper_text(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

class CheckWriter {
public:
    explicit CheckWriter(const TableSchema& schema) : schema_(schema) {}

    bool ok() const { return ok_; }

    std::string condition(const CondExpr& cond) { return write(cond, false); }

private:
    void fail() { ok_ = false; }

    // the column as the condition spells it (MySQL keeps the case that was typed); a name the table does not have is not written here
    std::string column(const std::string& written) {
        const auto dot = written.rfind('.');
        const std::string bare = dot == std::string::npos ? written : written.substr(dot + 1);
        for (const ColumnDef& c : schema_.columns) {
            if (lower_text(c.name) == lower_text(bare)) return "`" + bare + "`";
        }
        fail();
        return std::string();
    }

    bool has_column(const std::string& written) const {
        const auto dot = written.rfind('.');
        const std::string bare = dot == std::string::npos ? written : written.substr(dot + 1);
        return std::any_of(schema_.columns.begin(), schema_.columns.end(), [&](const ColumnDef& c) { return lower_text(c.name) == lower_text(bare); });
    }

    static bool all_digits(const std::string& text) {
        return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    }

    // a number the way MySQL prints it: as typed, an integer without leading zeros, a negative one in brackets after a minus sign
    std::string number(const std::string& value) {
        const bool negative = !value.empty() && value[0] == '-';
        std::string body = negative ? value.substr(1) : value;
        std::size_t i = 0;
        while (i < body.size() && std::isdigit(static_cast<unsigned char>(body[i]))) i++;
        bool valid = i > 0;
        if (valid && i < body.size() && body[i] == '.') {
            i++;
            while (i < body.size() && std::isdigit(static_cast<unsigned char>(body[i]))) i++;
        }
        if (valid && i < body.size() && (body[i] == 'e' || body[i] == 'E')) {
            i++;
            if (i < body.size() && (body[i] == '+' || body[i] == '-')) i++;
            const std::size_t digits = i;
            while (i < body.size() && std::isdigit(static_cast<unsigned char>(body[i]))) i++;
            valid = i > digits;
        }
        if (!valid || i != body.size()) {
            fail();
            return std::string();
        }
        if (all_digits(body)) {
            const std::size_t first = body.find_first_not_of('0');
            body = first == std::string::npos ? std::string("0") : body.substr(first);
        }
        return negative ? "-(" + body + ")" : body;
    }

    // a string with its character set; a quote and a backslash in it are written with a backslash
    std::string text(const std::string& value) {
        std::string out = "_utf8mb4'";
        for (char c : value) {
            if (static_cast<unsigned char>(c) < 0x20) {
                fail();
                return std::string();
            }
            if (c == '\'' || c == '\\') out += '\\';
            out += c;
        }
        return out + "'";
    }

    // a value of a condition that the parser kept as a word: a number, NULL, TRUE / FALSE, a column
    std::string literal(const std::string& value, bool quoted) {
        if (quoted) return text(value);
        const std::string up = upper_text(value);
        if (up == "NULL") return "NULL";
        if (up == "TRUE") return "true";
        if (up == "FALSE") return "false";
        if (!value.empty() && (std::isdigit(static_cast<unsigned char>(value[0])) || value[0] == '.' || value[0] == '-')) return number(value);
        return column(value);
    }

    std::string arith(const ArithExpr& e) {
        if (auto* c = std::get_if<ArithExpr::Col>(&e.data)) {
            if (c->outer != 0) fail();
            if (!has_column(c->name)) { // (NULL, TRUE and FALSE come as names)
                const std::string up = upper_text(c->name);
                if (up == "NULL") return "NULL";
                if (up == "TRUE") return "true";
                if (up == "FALSE") return "false";
            }
            return column(c->name);
        }
        if (auto* n = std::get_if<ArithExpr::Num>(&e.data)) return number(n->value);
        if (auto* s = std::get_if<ArithExpr::Str>(&e.data)) {
            if (s->hex) fail();
            return text(s->value);
        }
        auto binary = [&](const std::unique_ptr<ArithExpr>& l, const char* op, const std::unique_ptr<ArithExpr>& r) {
            const std::string left = arith(*l);
            return "(" + left + " " + op + " " + arith(*r) + ")";
        };
        if (auto* a = std::get_if<ArithExpr::Add>(&e.data)) return binary(a->lhs, "+", a->rhs);
        if (auto* a = std::get_if<ArithExpr::Sub>(&e.data)) {
            // (a minus sign in front, `-qty`, is read as `0 - qty`: MySQL writes the first as `-(`qty`)` and the second as `(0 - `qty`)`, and which was typed is not known)
            if (auto* zero = std::get_if<ArithExpr::Num>(&a->lhs->data); zero && zero->value == "0") fail();
            return binary(a->lhs, "-", a->rhs);
        }
        if (auto* a = std::get_if<ArithExpr::Mul>(&e.data)) return binary(a->lhs, "*", a->rhs);
        if (auto* a = std::get_if<ArithExpr::Div>(&e.data)) return binary(a->lhs, "/", a->rhs);
        if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) return function(*f);
        fail(); // (a comparison or a subquery used as a value, CASE)
        return std::string();
    }

    std::string function(const ArithExpr::Func& f) {
        const std::string name = upper_text(f.name);
        if (name == "CASE") { // CASE: (a condition, its result) pairs and, when the count is odd, the ELSE result last
            std::string out = "(case";
            std::size_t i = 0;
            for (; i + 1 < f.args.size(); i += 2) {
                auto* when = std::get_if<ArithExpr::Pred>(&f.args[i].data);
                if (!when) {
                    fail();
                    return std::string();
                }
                const std::string condition_text = write(*when->cond, false);
                out += " when " + condition_text + " then " + arith(f.args[i + 1]);
            }
            if (i < f.args.size()) out += " else " + arith(f.args[i]);
            return out + " end)";
        }
        std::vector<std::string> args;
        for (const ArithExpr& a : f.args) args.push_back(arith(a));
        if (name == "MOD" && args.size() == 2) return "(" + args[0] + " % " + args[1] + ")";
        std::string written;
        static const std::pair<const char*, const char*> kNames[] = {
            {"UPPER", "upper"}, {"LOWER", "lower"}, {"LCASE", "lower"}, {"UCASE", "upper"}, {"LENGTH", "length"}, {"CHAR_LENGTH", "char_length"}, {"CHARACTER_LENGTH", "char_length"},
            {"ABS", "abs"}, {"COALESCE", "coalesce"}, {"IFNULL", "ifnull"}, {"CONCAT", "concat"}, {"CEIL", "ceiling"}, {"CEILING", "ceiling"}, {"FLOOR", "floor"},
            {"SUBSTR", "substr"}, {"SUBSTRING", "substr"}, {"LEFT", "left"}, {"TRIM", "trim"}, {"YEAR", "year"}, {"MONTH", "month"}, {"NULLIF", "nullif"},
            {"GREATEST", "greatest"}, {"LEAST", "least"}, {"ROUND", "round"}, {"REGEXP_LIKE", "regexp_like"}};
        for (const auto& [from, to] : kNames) {
            if (name == from) written = to;
        }
        if (written.empty() || args.empty()) {
            fail();
            return std::string();
        }
        if (written == "round" && args.size() == 1) args.push_back("0");
        std::string out = written + "(";
        for (std::size_t i = 0; i < args.size(); i++) out += (i ? "," : "") + args[i];
        return out + ")";
    }

    // the pieces of a chain of ANDs (or ORs) that is one pair of brackets in MySQL: the node, and the nodes below it that are the same kind of AND / OR once the NOTs are pushed in
    void chain(const CondExpr& node, bool and_chain, bool negate, std::vector<std::string>& parts) {
        const CondExpr* lhs = nullptr;
        const CondExpr* rhs = nullptr;
        bool is_and = false;
        bool is_and_or = false;
        if (auto* a = std::get_if<CondExpr::And>(&node.data)) {
            lhs = a->lhs.get();
            rhs = a->rhs.get();
            is_and = true;
            is_and_or = true;
        } else if (auto* o = std::get_if<CondExpr::Or>(&node.data)) {
            lhs = o->lhs.get();
            rhs = o->rhs.get();
            is_and_or = true;
        }
        if (is_and_or && (is_and != negate) == and_chain) { // (an AND under a NOT is an OR)
            chain(*lhs, and_chain, negate, parts);
            chain(*rhs, and_chain, negate, parts);
            return;
        }
        parts.push_back(write(node, negate));
    }

    static const char* operator_text(Operator op) {
        switch (op) {
            case Operator::Eq: return "=";
            case Operator::Ne: return "<>";
            case Operator::Gt: return ">";
            case Operator::Lt: return "<";
            case Operator::Gte: return ">=";
            case Operator::Lte: return "<=";
            default: return nullptr;
        }
    }

    static Operator opposite(Operator op) {
        switch (op) {
            case Operator::Eq: return Operator::Ne;
            case Operator::Ne: return Operator::Eq;
            case Operator::Gt: return Operator::Lte;
            case Operator::Lte: return Operator::Gt;
            case Operator::Lt: return Operator::Gte;
            case Operator::Gte: return Operator::Lt;
            default: return op;
        }
    }

    std::string write(const CondExpr& cond, bool negate) {
        if (auto* n = std::get_if<CondExpr::Not>(&cond.data)) return write(*n->inner, !negate);
        const bool is_and = std::holds_alternative<CondExpr::And>(cond.data);
        if (is_and || std::holds_alternative<CondExpr::Or>(cond.data)) {
            const bool and_chain = is_and != negate;
            std::vector<std::string> parts;
            chain(cond, and_chain, negate, parts);
            std::string out = "(";
            for (std::size_t i = 0; i < parts.size(); i++) out += (i ? (and_chain ? " and " : " or ") : "") + parts[i];
            return out + ")";
        }
        return leaf(std::get<CondExpr::Leaf>(cond.data).condition, negate);
    }

    std::string leaf(const Condition& c, bool negate) {
        if (c.quantifier != Quantifier::None) {
            fail();
            return std::string();
        }
        const std::string left = arith(c.left);
        auto right_value = [&]() -> std::string {
            if (auto* lit = std::get_if<ConditionValue::Literal>(&c.value.data)) {
                if (lit->outer != 0) fail();
                return literal(lit->value, lit->quoted);
            }
            if (auto* a = std::get_if<ConditionValue::Arith>(&c.value.data)) return arith(a->expr);
            fail();
            return std::string();
        };
        switch (c.op) {
            case Operator::Eq:
            case Operator::Ne:
            case Operator::Gt:
            case Operator::Lt:
            case Operator::Gte:
            case Operator::Lte: {
                // `CHECK (REGEXP_LIKE(a, 'x'))`: a value used as a condition is `value <> 0` to the parser; a function that answers true / false is written
                // by MySQL as itself (`<> 0` typed after it is not told apart, and is rarer)
                if (auto* call = std::get_if<ArithExpr::Func>(&c.left.data); call && c.op == Operator::Ne && upper_text(call->name) == "REGEXP_LIKE") {
                    if (auto* zero = std::get_if<ConditionValue::Literal>(&c.value.data); zero && !zero->quoted && zero->value == "0") return negate ? "(not(" + left + "))" : left;
                }
                const Operator op = negate ? opposite(c.op) : c.op;
                const std::string right = right_value();
                return "(" + left + " " + operator_text(op) + " " + right + ")";
            }
            case Operator::IsNull:
            case Operator::IsNotNull: {
                const bool is_null = (c.op == Operator::IsNull) != negate;
                return "(" + left + (is_null ? " is null)" : " is not null)");
            }
            case Operator::In:
            case Operator::NotIn: {
                auto* list = std::get_if<ConditionValue::LiteralList>(&c.value.data);
                if (!list) {
                    fail();
                    return std::string();
                }
                const bool is_in = (c.op == Operator::In) != negate;
                std::string out = "(" + left + (is_in ? " in (" : " not in (");
                for (std::size_t i = 0; i < list->values.size(); i++) out += (i ? "," : "") + literal(list->values[i], i < list->quoted.size() && list->quoted[i]);
                return out + "))";
            }
            case Operator::Between:
            case Operator::NotBetween: {
                auto* range = std::get_if<ConditionValue::Between>(&c.value.data);
                if (!range) {
                    fail();
                    return std::string();
                }
                const bool is_between = (c.op == Operator::Between) != negate;
                const std::string lo = literal(range->lo, range->lo_quoted);
                return "(" + left + (is_between ? " between " : " not between ") + lo + " and " + literal(range->hi, range->hi_quoted) + ")";
            }
            case Operator::Like:
            case Operator::NotLike: {
                std::string pattern;
                if (auto* lit = std::get_if<ConditionValue::Literal>(&c.value.data)) {
                    // (the pattern is a string even where it was kept without quotes -- unless it names a column)
                    pattern = !lit->quoted && lit->outer == 0 && has_column(lit->value) ? column(lit->value) : literal(lit->value, true);
                } else {
                    pattern = right_value();
                }
                const std::string like = "(" + left + " like " + pattern + ")";
                return (c.op == Operator::Like) != negate ? like : "(not(" + like + "))";
            }
            case Operator::Regexp:
            case Operator::NotRegexp: {
                std::string pattern;
                if (auto* lit = std::get_if<ConditionValue::Literal>(&c.value.data)) pattern = literal(lit->value, true);
                else pattern = right_value();
                const std::string regexp = "regexp_like(" + left + "," + pattern + ")";
                return (c.op == Operator::Regexp) != negate ? regexp : "(not(" + regexp + "))"; // (MySQL: `(not(regexp_like(`a`,_utf8mb4'x')))`)
            }
            default:
                fail(); // (EXISTS, a subquery)
                return std::string();
        }
    }

    const TableSchema& schema_;
    bool ok_ = true;
};

} // namespace

std::optional<std::string> Executor::check_clause_text(const std::string& raw, const TableSchema& schema, bool information_schema) {
    Parser parser("SELECT 1 FROM __check__ WHERE " + raw);
    auto parsed = parser.parse();
    if (!parsed.is_ok()) return std::nullopt;
    Statement stmt = std::move(parsed).value();
    auto* select = std::get_if<Statement::Select>(&stmt.data);
    if (!select || !select->condition) return std::nullopt;
    CheckWriter writer(schema);
    std::string clause = writer.condition(*select->condition);
    if (!writer.ok()) return std::nullopt;
    if (!information_schema) return clause;
    std::string escaped; // (CHECK_CLAUSE holds the text escaped once more, so that it reads back as a string)
    for (char c : clause) {
        if (c == '\'' || c == '\\') escaped += '\\';
        escaped += c;
    }
    return escaped;
}

} // namespace engine
