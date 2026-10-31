// Faithful port of rusql-core/src/engine/executor.rs's apply_scalar_func (the built-in
// scalar SQL function dispatcher) plus its json_extract/json_take_value/json_array_items
// helpers and the from-scratch MD5 implementation.
//
// User-defined functions (CREATE FUNCTION) and DATABASE()/SCHEMA() need session context
// that this otherwise-static function doesn't have. The Rust original solves this with
// thread_local USER_FUNCTIONS/CURRENT_DB_CTX, synced at the top of execute_with_s
// (Statement dispatch) — sync_udf_context() below mirrors that exactly.

#include "engine/executor/executor.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <regex>
#include <unordered_map>

#include "engine/collation.hpp"
#include "engine/executor/builtin_functions.hpp"
#include "engine/now_text.hpp"
#include "engine/numeric_text.hpp"
#include "engine/parser/parser.hpp"
#include "engine/sql_value_util.hpp"

namespace engine {

namespace {

using namespace std::chrono;

thread_local std::unordered_map<std::string, UserFunctionDef> g_user_functions;
thread_local std::string g_current_db_ctx;
thread_local std::string g_current_user_ctx;

std::string to_lower_str(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::optional<long long> parse_i64(const std::string& s) {
    if (s.empty()) return std::nullopt;
    try {
        std::size_t pos;
        long long v = std::stoll(s, &pos);
        if (pos != s.size()) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
}

std::string to_upper(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::toupper(c); });
    return out;
}

std::string fmt_prec(double v, int prec) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
    return buf;
}

bool date_ok(int y, int mo, int d) {
    if (mo < 1 || mo > 12 || d < 1) return false;
    year_month_day ymd{year{y}, month{static_cast<unsigned>(mo)}, day{static_cast<unsigned>(d)}};
    return ymd.ok();
}

struct SimpleDate {
    int y = 1970, mo = 1, d = 1;
};

std::optional<SimpleDate> parse_date(const std::string& v) {
    int y, mo, d;
    if (std::sscanf(v.c_str(), "%d-%d-%d", &y, &mo, &d) != 3) return std::nullopt;
    if (!date_ok(y, mo, d)) return std::nullopt;
    return SimpleDate{y, mo, d};
}

struct SimpleDateTime {
    int y = 1970, mo = 1, d = 1, h = 0, mi = 0, s = 0;
};

std::optional<SimpleDateTime> parse_datetime(const std::string& v) {
    int y, mo, d, h, mi, s;
    if (std::sscanf(v.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6 && date_ok(y, mo, d)) {
        return SimpleDateTime{y, mo, d, h, mi, s};
    }
    if (auto sd = parse_date(v)) return SimpleDateTime{sd->y, sd->mo, sd->d, 0, 0, 0};
    return std::nullopt;
}

sys_days to_sys_days(const SimpleDate& sd) {
    return sys_days{year{sd.y} / month{static_cast<unsigned>(sd.mo)} / day{static_cast<unsigned>(sd.d)}};
}
sys_seconds to_sys_seconds(const SimpleDateTime& dt) {
    return sys_seconds{to_sys_days(SimpleDate{dt.y, dt.mo, dt.d}).time_since_epoch() + hours{dt.h} + minutes{dt.mi} + seconds{dt.s}};
}

// --- MD5 (from-scratch, matches the Rust original's own from-scratch implementation) ---
std::array<std::uint8_t, 16> md5_hash(const std::string& input) {
    static const std::uint32_t S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 5,  9,  14, 20, 5,  9,
                                         14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11,
                                         16, 23, 4,  11, 16, 23, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10,
                                         15, 21};
    static const std::uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
        0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
        0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

    std::uint64_t orig_bits = static_cast<std::uint64_t>(input.size()) * 8;
    std::vector<std::uint8_t> msg(input.begin(), input.end());
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 0; i < 8; i++) msg.push_back(static_cast<std::uint8_t>((orig_bits >> (8 * i)) & 0xFF));

    std::uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    auto rotl = [](std::uint32_t x, std::uint32_t n) { return (x << n) | (x >> (32 - n)); };

    for (std::size_t chunk_start = 0; chunk_start < msg.size(); chunk_start += 64) {
        std::uint32_t m[16];
        for (int i = 0; i < 16; i++) {
            const std::uint8_t* p = &msg[chunk_start + static_cast<std::size_t>(i) * 4];
            m[i] = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
                   (static_cast<std::uint32_t>(p[3]) << 24);
        }
        std::uint32_t a = a0, b = b0, c = c0, d = d0;
        for (std::uint32_t i = 0; i < 64; i++) {
            std::uint32_t f, g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            std::uint32_t tmp = d;
            d = c;
            c = b;
            b = b + rotl(a + f + K[i] + m[g], S[i]);
            a = tmp;
        }
        a0 += a;
        b0 += b;
        c0 += c;
        d0 += d;
    }

    std::array<std::uint8_t, 16> r{};
    auto store = [&](std::uint32_t v, int off) {
        for (int i = 0; i < 4; i++) r[static_cast<std::size_t>(off + i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    };
    store(a0, 0);
    store(b0, 4);
    store(c0, 8);
    store(d0, 12);
    return r;
}

// --- Minimal JSON path extraction on the raw text (matches the Rust original's
// hand-rolled, non-parsing-library approach exactly, byte quirks included) ---

std::string_view json_take_value(std::string_view s) {
    std::size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos) return s;
    s = s.substr(start);
    if (s.empty()) return s;
    if (s.front() == '"') {
        std::size_t i = 1;
        while (i < s.size()) {
            if (s[i] == '\\') {
                i += 2;
                continue;
            }
            if (s[i] == '"') return s.substr(0, i + 1);
            i++;
        }
        return s;
    }
    if (s.front() == '{' || s.front() == '[') {
        char open = s.front();
        char close = open == '{' ? '}' : ']';
        int depth = 0;
        bool in_str = false;
        for (std::size_t i = 0; i < s.size(); i++) {
            char c = s[i];
            if (in_str) {
                if (c == '\\') {
                    i++;
                    continue;
                }
                if (c == '"') in_str = false;
            } else {
                if (c == '"') in_str = true;
                else if (c == open) depth++;
                else if (c == close) {
                    depth--;
                    if (depth == 0) return s.substr(0, i + 1);
                }
            }
        }
        return s;
    }
    std::size_t end = s.find_first_of(",}]");
    std::string_view piece = end == std::string_view::npos ? s : s.substr(0, end);
    std::size_t last = piece.find_last_not_of(" \t\r\n");
    return last == std::string_view::npos ? piece.substr(0, 0) : piece.substr(0, last + 1);
}

std::vector<std::string_view> json_array_items(std::string_view inner) {
    std::vector<std::string_view> items;
    std::size_t start = inner.find_first_not_of(" \t\r\n");
    std::string_view s = start == std::string_view::npos ? std::string_view{} : inner.substr(start);
    while (!s.empty()) {
        std::string_view item = json_take_value(s);
        items.push_back(item);
        s = s.substr(item.size());
        std::size_t ws = s.find_first_not_of(" \t\r\n");
        s = ws == std::string_view::npos ? std::string_view{} : s.substr(ws);
        if (!s.empty() && s.front() == ',') {
            s = s.substr(1);
            std::size_t ws2 = s.find_first_not_of(" \t\r\n");
            s = ws2 == std::string_view::npos ? std::string_view{} : s.substr(ws2);
        }
    }
    return items;
}

std::string json_extract(const std::string& json_str, const std::string& path_in) {
    std::string path = path_in;
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);

    if (path != "$" && path.rfind("$.", 0) != 0) return EXECUTOR_NULL_VALUE;

    std::vector<std::string> parts;
    if (path != "$") {
        std::string rest = path.substr(2);
        std::size_t pos = 0;
        while (true) {
            auto dot = rest.find('.', pos);
            parts.push_back(rest.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos));
            if (dot == std::string::npos) break;
            pos = dot + 1;
        }
    }

    std::string_view current = json_str;
    std::size_t cs = current.find_first_not_of(" \t\r\n");
    std::size_t ce = current.find_last_not_of(" \t\r\n");
    current = (cs == std::string_view::npos) ? std::string_view{} : current.substr(cs, ce - cs + 1);

    for (auto& part : parts) {
        std::string key = part;
        std::optional<std::size_t> idx;
        if (auto br = part.find('['); br != std::string::npos) {
            key = part.substr(0, br);
            try {
                idx = static_cast<std::size_t>(std::stoul(part.substr(br + 1, part.size() - br - 2)));
            } catch (...) {
                idx = std::nullopt;
            }
        }

        if (!current.empty() && current.front() == '{') {
            std::string search = "\"" + key + "\":";
            auto pos = current.find(search);
            if (pos == std::string_view::npos) return EXECUTOR_NULL_VALUE;
            std::string_view rest = current.substr(pos + search.size());
            std::size_t ws = rest.find_first_not_of(" \t\r\n");
            rest = ws == std::string_view::npos ? std::string_view{} : rest.substr(ws);
            current = json_take_value(rest);
        } else {
            return EXECUTOR_NULL_VALUE;
        }

        if (idx) {
            if (!current.empty() && current.front() == '[') {
                auto items = json_array_items(current.substr(1, current.size() - 2));
                if (*idx < items.size()) current = items[*idx];
                else return EXECUTOR_NULL_VALUE;
            } else {
                return EXECUTOR_NULL_VALUE;
            }
        }
    }
    return std::string(current);
}

// Functions of the older dispatcher whose result is NULL when one of their first N arguments is NULL. The functions
// of builtin_functions.cpp deal with NULL themselves, so does COALESCE / IFNULL / NULLIF / ISNULL / IF, and the ones without arguments are not listed.
const std::unordered_map<std::string, std::size_t>& null_propagating_functions() {
    static const std::unordered_map<std::string, std::size_t> table = {
        {"MD5", 1}, {"FROM_UNIXTIME", 1}, {"UNIX_TIMESTAMP", 1}, {"JSON_UNQUOTE", 1}, {"JSON_EXTRACT", 2}, {"JSON_VALUE", 2},
        {"REGEXP_LIKE", 2}, {"REGEXP", 2}, {"REGEXP_MATCH", 2}, {"REGEXP_SUBSTR", 2}, {"REGEXP_INSTR", 2}, {"REGEXP_REPLACE", 3},
    };
    return table;
}

} // namespace

namespace {
thread_local std::uint64_t g_last_insert_id = 0;
thread_local std::int64_t g_row_count = -1;
thread_local std::uint64_t g_found_rows = 0;
} // namespace

void Executor::sync_session_counters(std::uint64_t last_insert_id, std::int64_t row_count, std::uint64_t found_rows) {
    g_last_insert_id = last_insert_id;
    g_row_count = row_count;
    g_found_rows = found_rows;
}

void Executor::sync_udf_context(const std::unordered_map<std::string, UserFunctionDef>& user_functions, const std::string& current_db,
                                 const std::string& current_user) {
    g_user_functions.clear();
    for (const auto& [name, def] : user_functions) g_user_functions[to_lower_str(name)] = def; // (a function is called by its name apart from case)
    g_current_db_ctx = current_db;
    g_current_user_ctx = current_user;
}

std::string Executor::call_builtin_with_values(const std::string& func_name, const std::vector<std::string>& raw, const std::vector<std::string>& values,
                                               const std::vector<char>& kinds) {
    return *call_builtin_function(BuiltinCall{func_name, raw, [&](std::size_t i) { return values[i]; }, g_current_db_ctx, kinds});
}

std::string Executor::apply_scalar_func(const std::string& typed_name, const std::vector<std::string>& args, const Row& row) {
    const std::string func_name = to_upper(typed_name); // (the names below are upper case; a user function is found by its lower case name)
    // (a stored function with the name of a built-in function is not called by it: MySQL calls the built-in one)
    if (auto uf_it = g_user_functions.find(to_lower_str(typed_name)); uf_it != g_user_functions.end() && !is_known_function_name(func_name)) {
        auto& [params, body_json] = uf_it->second;
        try {
            nlohmann::json body = nlohmann::json::parse(body_json);
            std::vector<std::string> param_types; // (a function made with its types: the values given and the result are made to fit them)
            std::string return_type;
            if (body.is_object() && body.contains("routine_expr")) {
                param_types = body.at("param_types").get<std::vector<std::string>>();
                return_type = body.at("return_type").get<std::string>();
                const nlohmann::json inner = body.at("routine_expr");
                body = inner;
            }
            ArithExpr expr = body.get<ArithExpr>();
            if (StatementContext* c = statement_context_) c->ex->bind_expression(*c->s, expr, false); // (a subquery in the body)
            Row bound_row = row;
            auto resolve_arg = [&](const std::string& arg) -> std::string {
                if (arg.size() >= 2 && arg.front() == '\'' && arg.back() == '\'') return arg.substr(1, arg.size() - 2);
                if (auto it = row.find(arg); it != row.end()) return it->second;
                return arg;
            };
            Row parameters;
            for (std::size_t i = 0; i < params.size(); i++) {
                std::string value = i < args.size() ? resolve_arg(args[i]) : std::string();
                if (i < param_types.size()) {
                    if (auto error = coerce_declared(params[i], param_types[i], value)) throw StatementError(*error);
                }
                bound_row[params[i]] = value;
                parameters[params[i]] = std::move(value);
            }
            substitute_parameters(expr, parameters);
            const sqlval::StrictWrite strict;
            std::string result = eval_arith(bound_row, expr);
            if (auto refused = strict.error()) throw StatementError(*refused); // (a function refuses a division by zero and a text that is not a number, where a SELECT shows NULL / the number it starts with)
            if (auto error = coerce_declared(typed_name, return_type, result)) throw StatementError(*error);
            return result;
        } catch (const StatementError&) {
            throw; // (a subquery of the body that fails is the statement's error)
        } catch (...) {
        }
    }

    std::vector<std::string> checked; // the leading arguments the NULL check below has resolved (an expression is parsed on every resolve)
    auto resolve = [&](const std::string& arg) -> std::string {
        for (std::size_t i = 0; i < checked.size(); i++) {
            if (args[i] == arg) return checked[i];
        }
        if (arg.size() >= 2 && arg.front() == '\'' && arg.back() == '\'') return arg.substr(1, arg.size() - 2);
        // an argument such as `a.x * a.y` is an expression, not a column: get_col would read what follows its last dot ("y") as the column
        if (arg.find_first_of(" +-*/%()<>=!,") == std::string::npos) {
            if (const std::string* v = get_col(row, arg)) return *v;
            if (auto dot = arg.rfind('.'); dot != std::string::npos) {
                auto it = row.find(arg.substr(dot + 1));
                if (it != row.end()) return it->second;
            }
        }
        try {
            Parser p(arg);
            ArithExpr expr = p.parse_arith_expr();
            return eval_arith(row, expr);
        } catch (...) {
        }
        return arg;
    };
    auto arg_at = [&](std::size_t i) -> std::string { return i < args.size() ? resolve(args[i]) : std::string(); };

    // the functions written to MySQL's rules come first
    if (auto result = call_builtin_function(BuiltinCall{func_name, args, [&](std::size_t i) { return resolve(args[i]); }, g_current_db_ctx, {}})) return *result;

    if (auto it = null_propagating_functions().find(func_name); it != null_propagating_functions().end()) {
        const std::size_t value_args = std::min(it->second, args.size());
        checked.reserve(value_args);
        for (std::size_t i = 0; i < value_args; i++) {
            checked.push_back(resolve(args[i]));
            if (checked.back() == EXECUTOR_NULL_VALUE) return EXECUTOR_NULL_VALUE;
        }
    }

    // NOW(3), CURTIME(6), UTC_TIMESTAMP(2): the digits of the fraction of a second (none: 0)
    auto fraction_arg = [&]() -> int {
        if (args.empty()) return 0;
        const auto digits = parse_i64(arg_at(0));
        if (digits && *digits > 6) {
            std::string name = func_name;
            for (char& ch : name) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            throw StatementError("Too-big precision " + std::to_string(*digits) + " specified for '" + name + "'. Maximum is 6.");
        }
        return digits ? static_cast<int>(std::max<long long>(*digits, 0)) : 0;
    };
    if (func_name == "NOW") return time_now_text(fraction_arg(), false, true);
    if (func_name == "CURDATE") {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
        localtime_s(&tm_buf, &t);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
        return buf;
    }
    if (func_name == "COALESCE") {
        for (auto& a : args) {
            std::string v = resolve(a);
            if (v != EXECUTOR_NULL_VALUE) return v;
        }
        return EXECUTOR_NULL_VALUE;
    }
    if (func_name == "IFNULL") {
        std::string v = arg_at(0);
        if (v == EXECUTOR_NULL_VALUE) return arg_at(1);
        return v;
    }
    if (func_name == "IF") {
        std::string cond_val = arg_at(0);
        std::string true_val = arg_at(1);
        std::string false_val = arg_at(2);
        bool is_true = !cond_val.empty() && cond_val != "0" && cond_val != "false" && cond_val != EXECUTOR_NULL_VALUE;
        return is_true ? true_val : false_val;
    }
    if (func_name == "NULLIF") {
        // NULL when the two are equal as `=` says: numbers by value (5 and 5.00 are equal), anything else as text
        std::string a = arg_at(0), b = arg_at(1);
        const bool both_numbers = parse_number(a) && parse_number(b);
        const bool equal = both_numbers ? compare_numbers(a, b) == 0 : collation::equal(a, b);
        return equal ? EXECUTOR_NULL_VALUE : a;
    }
    if (func_name == "REGEXP_LIKE" || func_name == "REGEXP") {
        std::string val = arg_at(0), pat = arg_at(1);
        try {
            return std::regex_search(val, std::regex(pat, std::regex::ECMAScript | std::regex::icase)) ? "1" : "0";
        } catch (...) {
            return "0";
        }
    }
    if (func_name == "REGEXP_REPLACE") {
        std::string val = arg_at(0), pat = arg_at(1), rep = arg_at(2);
        try {
            return std::regex_replace(val, std::regex(pat, std::regex::ECMAScript | std::regex::icase), rep);
        } catch (...) {
            return val;
        }
    }
    if (func_name == "REGEXP_MATCH" || func_name == "REGEXP_SUBSTR") {
        std::string val = arg_at(0), pat = arg_at(1);
        try {
            std::smatch m;
            std::regex re(pat, std::regex::ECMAScript | std::regex::icase);
            if (std::regex_search(val, m, re)) return m.str();
        } catch (...) {
        }
        return EXECUTOR_NULL_VALUE;
    }
    if (func_name == "REGEXP_INSTR") { // the position (1 for the first character) where the pattern matches, 0 when it does not
        std::string val = arg_at(0), pat = arg_at(1);
        try {
            std::smatch m;
            std::regex re(pat, std::regex::ECMAScript | std::regex::icase);
            if (std::regex_search(val, m, re)) return std::to_string(m.position(0) + 1);
        } catch (...) {
        }
        return "0";
    }
    if (func_name == "LAST_INSERT_ID") {
        if (args.empty()) return std::to_string(g_last_insert_id);
        // LAST_INSERT_ID(expr): the value of expr, which LAST_INSERT_ID() then says
        const std::string value = arg_at(0);
        if (value == EXECUTOR_NULL_VALUE) return value;
        if (StatementContext* c = statement_context_) {
            if (auto number = parse_i64(value)) c->ex->last_insert_id_ = static_cast<std::uint64_t>(*number);
        }
        return value;
    }
    if (func_name == "ROW_COUNT") return std::to_string(g_row_count);
    if (func_name == "FOUND_ROWS") return std::to_string(g_found_rows);
    if (func_name == "SLEEP") return "0"; // (it does not wait: the server answers every connection from one lock)
    if (func_name == "UTC_DATE" || func_name == "UTC_TIME" || func_name == "UTC_TIMESTAMP") {
        if (func_name == "UTC_TIME") return time_now_text(fraction_arg(), true, false);
        if (func_name == "UTC_TIMESTAMP") return time_now_text(fraction_arg(), true, true);
        std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm_buf{};
        gmtime_s(&tm_buf, &t);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
        return buf;
    }
    if (func_name == "RAND") {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1000000000;
        return fmt_prec(static_cast<double>(ns) / 1e9, 6);
    }
    if (func_name == "CURTIME" || func_name == "CURRENT_TIME") return time_now_text(fraction_arg(), false, false);
    if (func_name == "CURRENT_TIMESTAMP" || func_name == "LOCALTIME" || func_name == "LOCALTIMESTAMP") return time_now_text(fraction_arg(), false, true);
    if (func_name == "UNIX_TIMESTAMP") {
        if (args.empty()) {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now).count());
        }
        auto dt = parse_datetime(arg_at(0));
        if (!dt) return EXECUTOR_NULL_VALUE;
        // the session's time zone is the system's (@@time_zone = SYSTEM): the date is local time
        std::tm local{};
        local.tm_year = dt->y - 1900;
        local.tm_mon = dt->mo - 1;
        local.tm_mday = dt->d;
        local.tm_hour = dt->h;
        local.tm_min = dt->mi;
        local.tm_sec = dt->s;
        local.tm_isdst = -1;
        const std::time_t t = std::mktime(&local);
        if (t == static_cast<std::time_t>(-1)) return std::to_string(to_sys_seconds(*dt).time_since_epoch().count());
        return std::to_string(static_cast<long long>(t));
    }
    if (func_name == "FROM_UNIXTIME") {
        long long ts = args.empty() ? 0 : parse_i64(resolve(args[0])).value_or(0);
        std::time_t t = static_cast<std::time_t>(ts); // (shown in the system's time zone, the session's)
        std::tm local{};
        if (localtime_s(&local, &t) != 0) return EXECUTOR_NULL_VALUE;
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
        return buf;
    }
    if (func_name == "ISNULL") {
        std::string v = arg_at(0);
        return v == EXECUTOR_NULL_VALUE ? "1" : "0";
    }
    if (func_name == "MD5") {
        auto hash = md5_hash(arg_at(0));
        std::string out;
        char buf[3];
        for (auto b : hash) {
            std::snprintf(buf, sizeof(buf), "%02x", b);
            out += buf;
        }
        return out;
    }
    if (func_name == "UUID") {
        auto ts = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%08x-%04x-4%03x-%04x-%012llx", static_cast<unsigned>(ts >> 32),
                      static_cast<unsigned>((ts >> 16) & 0xffff), static_cast<unsigned>(ts & 0x0fff),
                      static_cast<unsigned>(0x8000u | ((ts >> 48) & 0x3fff)), static_cast<unsigned long long>(ts & 0xffffffffffffULL));
        return buf;
    }
    if (func_name == "JSON_EXTRACT") return json_extract(arg_at(0), arg_at(1));
    if (func_name == "JSON_UNQUOTE") {
        std::string v = arg_at(0);
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
            std::string inner = v.substr(1, v.size() - 2);
            std::string out;
            for (std::size_t i = 0; i < inner.size(); i++) {
                if (inner[i] == '\\' && i + 1 < inner.size() && (inner[i + 1] == '"' || inner[i + 1] == '\\')) {
                    out += inner[i + 1];
                    i++;
                } else {
                    out += inner[i];
                }
            }
            return out;
        }
        return v;
    }
    if (func_name == "JSON_VALUE") {
        std::string extracted = json_extract(arg_at(0), arg_at(1));
        if (extracted.size() >= 2 && extracted.front() == '"' && extracted.back() == '"') {
            std::string inner = extracted.substr(1, extracted.size() - 2);
            std::string out;
            for (std::size_t i = 0; i < inner.size(); i++) {
                if (inner[i] == '\\' && i + 1 < inner.size() && (inner[i + 1] == '"' || inner[i + 1] == '\\')) {
                    out += inner[i + 1];
                    i++;
                } else {
                    out += inner[i];
                }
            }
            return out;
        }
        return extracted;
    }
    if (func_name == "DATABASE" || func_name == "SCHEMA") return g_current_db_ctx;
    if (func_name == "VERSION") return "8.4.9"; // (the same as @@version and the handshake of the MySQL protocol)
    // Regression: used to hardcode "root@localhost" regardless of who actually
    // authenticated -- g_current_user_ctx is synced from Executor::auth_user (set by the
    // native/MySQL protocol servers after a successful AUTH handshake) the same way
    // g_current_db_ctx is synced from current_db, just above.
    if (func_name == "CURRENT_USER" || func_name == "USER" || func_name == "SESSION_USER" || func_name == "SYSTEM_USER")
        return g_current_user_ctx + "@localhost";

    throw StatementError("FUNCTION " + g_current_db_ctx + "." + to_lower_str(typed_name) + " does not exist");
}

} // namespace engine
