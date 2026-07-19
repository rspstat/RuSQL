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
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <regex>
#include <sstream>

#include "engine/parser/parser.hpp"

namespace engine {

namespace {

using namespace std::chrono;

constexpr double kE = 2.71828182845904523536;
constexpr double kPi = 3.14159265358979323846;

thread_local std::unordered_map<std::string, UserFunctionDef> g_user_functions;
thread_local std::string g_current_db_ctx;
thread_local std::string g_current_user_ctx;

std::string to_lower_str(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::optional<double> parse_f64(const std::string& s) {
    if (s.empty()) return std::nullopt;
    try {
        std::size_t pos;
        double v = std::stod(s, &pos);
        if (pos != s.size()) return std::nullopt;
        return v;
    } catch (...) {
        return std::nullopt;
    }
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

std::string to_lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

std::string trim_both(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
std::string trim_left(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a);
}
std::string trim_right(const std::string& s) {
    auto b = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(0, b + 1);
}

// Matches Rust's `format!("{}", v)` / f64 Display: the shortest decimal (never
// scientific-notation) representation that round-trips exactly, with no forced
// trailing ".0" for whole numbers (e.g. 3.0 -> "3", matching Rust). The previous
// `oss << v` used std::ostream's default precision (6 significant digits), which
// silently truncated anything needing more precision to round-trip -- e.g. PI()
// showed "3.14159" instead of the full "3.141592653589793".
std::string fmt_double(double v) {
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed);
    return std::string(buf, res.ptr);
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

std::string format_date(const SimpleDate& sd) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", sd.y, sd.mo, sd.d);
    return buf;
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

std::string format_datetime(const SimpleDateTime& dt) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", dt.y, dt.mo, dt.d, dt.h, dt.mi, dt.s);
    return buf;
}

sys_days to_sys_days(const SimpleDate& sd) {
    return sys_days{year{sd.y} / month{static_cast<unsigned>(sd.mo)} / day{static_cast<unsigned>(sd.d)}};
}
SimpleDate from_sys_days(sys_days sd) {
    year_month_day ymd{sd};
    return SimpleDate{static_cast<int>(ymd.year()), static_cast<int>(static_cast<unsigned>(ymd.month())),
                       static_cast<int>(static_cast<unsigned>(ymd.day()))};
}
sys_seconds to_sys_seconds(const SimpleDateTime& dt) {
    return sys_seconds{to_sys_days(SimpleDate{dt.y, dt.mo, dt.d}).time_since_epoch() + hours{dt.h} + minutes{dt.mi} + seconds{dt.s}};
}

long long floor_div(long long a, long long b) {
    long long q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) q--;
    return q;
}
long long rem_euclid(long long a, long long b) {
    long long r = a % b;
    if (r < 0) r += (b < 0 ? -b : b);
    return r;
}

// Rust's from_ymd_opt(...).unwrap_or(d): if the shifted (year, month) + original day
// isn't a valid calendar date, the date is returned unchanged rather than clamped.
SimpleDate add_months(const SimpleDate& d, long long delta_months) {
    long long months0 = static_cast<long long>(d.mo) - 1 + delta_months;
    int year = d.y + static_cast<int>(floor_div(months0, 12));
    int month = static_cast<int>(rem_euclid(months0, 12)) + 1;
    if (date_ok(year, month, d.d)) return SimpleDate{year, month, d.d};
    return d;
}

SimpleDate add_years(const SimpleDate& d, long long delta_years) {
    int year = d.y + static_cast<int>(delta_years);
    if (date_ok(year, d.mo, d.d)) return SimpleDate{year, d.mo, d.d};
    return d;
}

int weekday_sunday1(sys_days sd) { return weekday{sd}.c_encoding() + 1; }
int weekday_monday0(sys_days sd) { return weekday{sd}.iso_encoding() - 1; }
int day_of_year(sys_days sd) {
    year_month_day ymd{sd};
    sys_days jan1 = sys_days{ymd.year() / January / 1};
    return static_cast<int>((sd - jan1).count()) + 1;
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

} // namespace

void Executor::sync_udf_context(const std::unordered_map<std::string, UserFunctionDef>& user_functions, const std::string& current_db,
                                 const std::string& current_user) {
    g_user_functions = user_functions;
    g_current_db_ctx = current_db;
    g_current_user_ctx = current_user;
}

std::string Executor::apply_scalar_func(const std::string& func_name, const std::vector<std::string>& args, const Row& row) {
    if (auto uf_it = g_user_functions.find(to_lower_str(func_name)); uf_it != g_user_functions.end()) {
        auto& [params, body_json] = uf_it->second;
        try {
            ArithExpr expr = nlohmann::json::parse(body_json).get<ArithExpr>();
            Row bound_row = row;
            auto resolve_arg = [&](const std::string& arg) -> std::string {
                if (arg.size() >= 2 && arg.front() == '\'' && arg.back() == '\'') return arg.substr(1, arg.size() - 2);
                if (auto it = row.find(arg); it != row.end()) return it->second;
                return arg;
            };
            for (std::size_t i = 0; i < params.size(); i++) {
                bound_row[params[i]] = i < args.size() ? resolve_arg(args[i]) : std::string();
            }
            return eval_arith(bound_row, expr);
        } catch (...) {
        }
    }

    auto resolve = [&](const std::string& arg) -> std::string {
        if (arg.size() >= 2 && arg.front() == '\'' && arg.back() == '\'') return arg.substr(1, arg.size() - 2);
        if (const std::string* v = get_col(row, arg)) return *v;
        if (auto dot = arg.rfind('.'); dot != std::string::npos) {
            auto it = row.find(arg.substr(dot + 1));
            if (it != row.end()) return it->second;
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

    if (func_name == "UPPER") return to_upper(arg_at(0));
    if (func_name == "LOWER") return to_lower(arg_at(0));
    if (func_name == "LENGTH") return std::to_string(arg_at(0).size());
    if (func_name == "TRIM") return trim_both(arg_at(0));
    if (func_name == "CONCAT") {
        std::vector<std::string> parts;
        for (auto& a : args) parts.push_back(resolve(a));
        if (std::any_of(parts.begin(), parts.end(), [](const std::string& p) { return p == EXECUTOR_NULL_VALUE; })) return EXECUTOR_NULL_VALUE;
        std::string out;
        for (auto& p : parts) out += p;
        return out;
    }
    if (func_name == "CONCAT_WS") {
        std::string sep = arg_at(0);
        std::string out;
        bool first = true;
        for (std::size_t i = 1; i < args.size(); i++) {
            std::string v = resolve(args[i]);
            if (v == EXECUTOR_NULL_VALUE) continue;
            if (!first) out += sep;
            out += v;
            first = false;
        }
        return out;
    }
    if (func_name == "SUBSTR" || func_name == "SUBSTRING") {
        std::string v = arg_at(0);
        long long n = args.size() > 1 ? parse_i64(resolve(args[1])).value_or(0) : 0;
        std::size_t start = n > 0 ? static_cast<std::size_t>(n - 1) : 0;
        std::optional<std::size_t> len_opt;
        if (args.size() > 2) {
            if (auto li = parse_i64(resolve(args[2])); li && *li >= 0) len_opt = static_cast<std::size_t>(*li);
        }
        if (start >= v.size()) return "";
        std::size_t end = len_opt ? std::min(start + *len_opt, v.size()) : v.size();
        return v.substr(start, end - start);
    }
    if (func_name == "NOW") {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
        localtime_s(&tm_buf, &t);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
        return buf;
    }
    if (func_name == "CURDATE") {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
        localtime_s(&tm_buf, &t);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
        return buf;
    }
    if (func_name == "DATE_FORMAT") {
        std::string date_val = arg_at(0);
        std::string fmt_arg = arg_at(1);
        std::vector<std::string> parts;
        std::size_t p = 0;
        while (true) {
            auto dash = date_val.find('-', p);
            parts.push_back(date_val.substr(p, dash == std::string::npos ? std::string::npos : dash - p));
            if (dash == std::string::npos) break;
            p = dash + 1;
        }
        auto replace_all = [](std::string s, const std::string& from, const std::string& to) {
            std::size_t pos = 0;
            while ((pos = s.find(from, pos)) != std::string::npos) {
                s.replace(pos, from.size(), to);
                pos += to.size();
            }
            return s;
        };
        std::string out = fmt_arg;
        out = replace_all(out, "%Y", parts.size() > 0 ? parts[0] : "");
        out = replace_all(out, "%m", parts.size() > 1 ? parts[1] : "");
        out = replace_all(out, "%d", parts.size() > 2 ? parts[2] : "");
        return out;
    }
    if (func_name == "COALESCE") {
        for (auto& a : args) {
            std::string v = resolve(a);
            if (v != EXECUTOR_NULL_VALUE && !v.empty()) return v;
        }
        return EXECUTOR_NULL_VALUE;
    }
    if (func_name == "IFNULL") {
        std::string v = arg_at(0);
        if (v == EXECUTOR_NULL_VALUE || v.empty()) return arg_at(1);
        return v;
    }
    if (func_name == "REPLACE") {
        std::string v = arg_at(0), from = arg_at(1), to = arg_at(2);
        if (from.empty()) return v;
        std::string out;
        std::size_t pos = 0;
        while (true) {
            auto next = v.find(from, pos);
            if (next == std::string::npos) {
                out += v.substr(pos);
                break;
            }
            out += v.substr(pos, next - pos) + to;
            pos = next + from.size();
        }
        return out;
    }
    if (func_name == "ROUND") {
        double v = parse_f64(arg_at(0)).value_or(0.0);
        int decimals = args.size() > 1 ? static_cast<int>(parse_i64(arg_at(1)).value_or(0)) : 0;
        double factor = std::pow(10.0, decimals);
        return fmt_double(std::round(v * factor) / factor);
    }
    if (func_name == "ABS") return fmt_double(std::abs(parse_f64(arg_at(0)).value_or(0.0)));
    if (func_name == "CEIL") return fmt_double(std::ceil(parse_f64(arg_at(0)).value_or(0.0)));
    if (func_name == "FLOOR") return fmt_double(std::floor(parse_f64(arg_at(0)).value_or(0.0)));
    if (func_name == "MOD") {
        double a = parse_f64(arg_at(0)).value_or(0.0);
        double b = args.size() > 1 ? parse_f64(arg_at(1)).value_or(1.0) : 1.0;
        if (b == 0.0) return EXECUTOR_NULL_VALUE;
        return fmt_double(std::fmod(a, b));
    }
    if (func_name == "IF") {
        std::string cond_val = arg_at(0);
        std::string true_val = arg_at(1);
        std::string false_val = arg_at(2);
        bool is_true = !cond_val.empty() && cond_val != "0" && cond_val != "false" && cond_val != EXECUTOR_NULL_VALUE;
        return is_true ? true_val : false_val;
    }
    if (func_name == "NULLIF") {
        std::string a = arg_at(0), b = arg_at(1);
        return a == b ? EXECUTOR_NULL_VALUE : a;
    }
    if (func_name == "LPAD" || func_name == "RPAD") {
        std::string s = arg_at(0);
        std::size_t len = args.size() > 1 ? static_cast<std::size_t>(std::max<long long>(0, parse_i64(resolve(args[1])).value_or(0))) : 0;
        std::string pad = args.size() > 2 ? resolve(args[2]) : " ";
        if (s.size() >= len) return s.substr(0, len);
        if (pad.empty()) return s;
        std::size_t pad_needed = len - s.size();
        std::string full_pad;
        while (full_pad.size() < pad_needed) full_pad += pad;
        full_pad = full_pad.substr(0, pad_needed);
        return func_name == "LPAD" ? full_pad + s : s + full_pad;
    }
    if (func_name == "CAST" || func_name == "CONVERT") {
        std::string val = arg_at(0);
        std::string raw_type = args.size() > 1 ? args[1] : "TEXT";
        std::string type_str = raw_type;
        if (type_str.size() >= 2 && type_str.front() == '\'' && type_str.back() == '\'') type_str = type_str.substr(1, type_str.size() - 2);
        if (type_str == "INT" || type_str == "INTEGER" || type_str == "SIGNED" || type_str == "TINYINT" || type_str == "SMALLINT" ||
            type_str == "MEDIUMINT") {
            if (auto n = parse_i64(val)) return std::to_string(*n);
            if (auto f = parse_f64(val)) return std::to_string(static_cast<long long>(*f));
            return "0";
        }
        if (type_str == "UNSIGNED" || type_str == "BIGINT") {
            if (auto n = parse_i64(val); n && *n >= 0) return std::to_string(*n);
            if (auto f = parse_f64(val); f && *f >= 0) return std::to_string(static_cast<unsigned long long>(*f));
            return "0";
        }
        if (type_str == "FLOAT" || type_str == "DOUBLE" || type_str == "DECIMAL" || type_str == "NUMERIC" || type_str == "REAL") {
            if (auto f = parse_f64(val)) return fmt_double(*f);
            return "0";
        }
        if (type_str == "BOOLEAN" || type_str == "BOOL") {
            bool b = !val.empty() && val != "0" && to_lower(val) != "false" && val != EXECUTOR_NULL_VALUE;
            return b ? "1" : "0";
        }
        if (type_str == "DATE") {
            if (auto dt = parse_datetime(val)) return format_date(SimpleDate{dt->y, dt->mo, dt->d});
            return EXECUTOR_NULL_VALUE;
        }
        if (type_str == "DATETIME" || type_str == "TIMESTAMP") {
            if (auto dt = parse_datetime(val)) return format_datetime(*dt);
            return EXECUTOR_NULL_VALUE;
        }
        return val;
    }
    if (func_name == "DATEDIFF") {
        auto d1 = parse_date(arg_at(0));
        auto d2 = parse_date(arg_at(1));
        if (d1 && d2) return std::to_string((to_sys_days(*d1) - to_sys_days(*d2)).count());
        return EXECUTOR_NULL_VALUE;
    }
    if (func_name == "DATE_ADD" || func_name == "DATE_SUB") {
        std::string date_str = arg_at(0);
        long long amount = args.size() > 1 ? parse_i64(resolve(args[1])).value_or(0) : 0;
        if (func_name == "DATE_SUB") amount = -amount;
        std::string unit = args.size() > 2 ? args[2] : "DAY";
        auto d = parse_date(date_str);
        if (!d) return EXECUTOR_NULL_VALUE;
        SimpleDate result = *d;
        if (unit == "DAY") result = from_sys_days(to_sys_days(*d) + days{amount});
        else if (unit == "MONTH") result = add_months(*d, amount);
        else if (unit == "YEAR") result = add_years(*d, amount);
        return format_date(result);
    }
    if (func_name == "REGEXP_LIKE" || func_name == "REGEXP") {
        std::string val = arg_at(0), pat = arg_at(1);
        try {
            return std::regex_search(val, std::regex(pat)) ? "1" : "0";
        } catch (...) {
            return "0";
        }
    }
    if (func_name == "REGEXP_REPLACE") {
        std::string val = arg_at(0), pat = arg_at(1), rep = arg_at(2);
        try {
            return std::regex_replace(val, std::regex(pat), rep);
        } catch (...) {
            return val;
        }
    }
    if (func_name == "REGEXP_MATCH" || func_name == "REGEXP_SUBSTR") {
        std::string val = arg_at(0), pat = arg_at(1);
        try {
            std::smatch m;
            std::regex re(pat);
            if (std::regex_search(val, m, re)) return m.str();
        } catch (...) {
        }
        return EXECUTOR_NULL_VALUE;
    }
    if (func_name == "SQRT") {
        double v = parse_f64(arg_at(0)).value_or(0.0);
        return v < 0.0 ? EXECUTOR_NULL_VALUE : fmt_prec(std::sqrt(v), 6);
    }
    if (func_name == "POW" || func_name == "POWER") {
        double base = parse_f64(arg_at(0)).value_or(0.0);
        double exp = args.size() > 1 ? parse_f64(arg_at(1)).value_or(0.0) : 0.0;
        return fmt_double(std::pow(base, exp));
    }
    if (func_name == "LOG") {
        double v = parse_f64(arg_at(0)).value_or(0.0);
        if (args.size() >= 2) {
            double base = parse_f64(arg_at(1)).value_or(kE);
            if (v <= 0.0 || base <= 0.0 || base == 1.0) return EXECUTOR_NULL_VALUE;
            return fmt_prec(std::log(v) / std::log(base), 6);
        }
        return v <= 0.0 ? EXECUTOR_NULL_VALUE : fmt_prec(std::log(v), 6);
    }
    if (func_name == "LOG2") {
        double v = parse_f64(arg_at(0)).value_or(0.0);
        return v <= 0.0 ? EXECUTOR_NULL_VALUE : fmt_prec(std::log2(v), 6);
    }
    if (func_name == "LOG10") {
        double v = parse_f64(arg_at(0)).value_or(0.0);
        return v <= 0.0 ? EXECUTOR_NULL_VALUE : fmt_prec(std::log10(v), 6);
    }
    if (func_name == "EXP") return fmt_prec(std::exp(parse_f64(arg_at(0)).value_or(0.0)), 6);
    if (func_name == "SIN") return fmt_prec(std::sin(parse_f64(arg_at(0)).value_or(0.0)), 6);
    if (func_name == "COS") return fmt_prec(std::cos(parse_f64(arg_at(0)).value_or(0.0)), 6);
    if (func_name == "TAN") return fmt_prec(std::tan(parse_f64(arg_at(0)).value_or(0.0)), 6);
    if (func_name == "PI") return fmt_double(kPi);
    if (func_name == "SIGN") {
        double v = parse_f64(arg_at(0)).value_or(0.0);
        return v > 0.0 ? "1" : (v < 0.0 ? "-1" : "0");
    }
    if (func_name == "TRUNCATE") {
        double v = parse_f64(arg_at(0)).value_or(0.0);
        int d = args.size() > 1 ? static_cast<int>(parse_i64(arg_at(1)).value_or(0)) : 0;
        double factor = std::pow(10.0, d);
        return fmt_double(std::trunc(v * factor) / factor);
    }
    if (func_name == "RAND") {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() % 1000000000;
        return fmt_prec(static_cast<double>(ns) / 1e9, 6);
    }
    if (func_name == "GREATEST" || func_name == "LEAST") {
        std::vector<std::string> vals;
        for (auto& a : args) vals.push_back(resolve(a));
        if (vals.empty()) return "";
        auto better = [&](const std::string& a, const std::string& b) {
            auto fa = parse_f64(a), fb = parse_f64(b);
            bool a_better = (fa && fb) ? (func_name == "GREATEST" ? *fa > *fb : *fa < *fb) : (func_name == "GREATEST" ? a > b : a < b);
            return a_better;
        };
        std::string best = vals[0];
        for (std::size_t i = 1; i < vals.size(); i++) {
            if (better(vals[i], best)) best = vals[i];
        }
        return best;
    }
    if (func_name == "CHAR_LENGTH" || func_name == "CHARACTER_LENGTH") return std::to_string(arg_at(0).size());
    if (func_name == "LEFT") {
        std::string s = arg_at(0);
        std::size_t n = static_cast<std::size_t>(std::max<long long>(0, args.size() > 1 ? parse_i64(resolve(args[1])).value_or(0) : 0));
        return s.substr(0, std::min(n, s.size()));
    }
    if (func_name == "RIGHT") {
        std::string s = arg_at(0);
        std::size_t n = static_cast<std::size_t>(std::max<long long>(0, args.size() > 1 ? parse_i64(resolve(args[1])).value_or(0) : 0));
        n = std::min(n, s.size());
        return s.substr(s.size() - n);
    }
    if (func_name == "REVERSE") {
        std::string v = arg_at(0);
        std::reverse(v.begin(), v.end());
        return v;
    }
    if (func_name == "REPEAT") {
        std::string s = arg_at(0);
        std::size_t n = static_cast<std::size_t>(std::max<long long>(0, args.size() > 1 ? parse_i64(resolve(args[1])).value_or(0) : 0));
        std::string out;
        out.reserve(s.size() * n);
        for (std::size_t i = 0; i < n; i++) out += s;
        return out;
    }
    if (func_name == "INSTR") {
        std::string haystack = arg_at(0), needle = arg_at(1);
        auto pos = haystack.find(needle);
        return pos == std::string::npos ? "0" : std::to_string(pos + 1);
    }
    if (func_name == "LOCATE") {
        std::string needle = arg_at(0), haystack = arg_at(1);
        std::size_t start = args.size() > 2 ? static_cast<std::size_t>(std::max<long long>(0, parse_i64(resolve(args[2])).value_or(1) - 1)) : 0;
        if (start >= haystack.size()) return "0";
        auto pos = haystack.find(needle, start);
        return pos == std::string::npos ? "0" : std::to_string(pos + 1);
    }
    if (func_name == "LTRIM") return trim_left(arg_at(0));
    if (func_name == "RTRIM") return trim_right(arg_at(0));
    if (func_name == "SPACE") {
        std::size_t n = static_cast<std::size_t>(std::max<long long>(0, args.empty() ? 0 : parse_i64(resolve(args[0])).value_or(0)));
        return std::string(n, ' ');
    }
    if (func_name == "ASCII") {
        std::string v = arg_at(0);
        return v.empty() ? "0" : std::to_string(static_cast<unsigned char>(v[0]));
    }
    if (func_name == "CHAR") {
        std::string out;
        for (auto& a : args) {
            if (auto n = parse_i64(resolve(a)); n && *n >= 0 && *n <= 255) out += static_cast<char>(*n);
        }
        return out;
    }
    if (func_name == "HEX") {
        std::string v = arg_at(0);
        char buf[32];
        if (auto n = parse_i64(v); n && *n >= 0) {
            std::snprintf(buf, sizeof(buf), "%llX", static_cast<unsigned long long>(*n));
            return buf;
        }
        std::string out;
        for (unsigned char c : v) {
            std::snprintf(buf, sizeof(buf), "%02X", c);
            out += buf;
        }
        return out;
    }
    if (func_name == "UNHEX") {
        std::string v = arg_at(0);
        std::string out;
        for (std::size_t i = 0; i + 1 < v.size(); i += 2) {
            try {
                out += static_cast<char>(std::stoul(v.substr(i, 2), nullptr, 16));
            } catch (...) {
                break;
            }
        }
        return out;
    }
    if (func_name == "FORMAT") {
        double v = parse_f64(arg_at(0)).value_or(0.0);
        int d = args.size() > 1 ? static_cast<int>(parse_i64(arg_at(1)).value_or(0)) : 0;
        std::string s = fmt_prec(v, d);
        auto dot = s.find('.');
        std::string int_part = dot == std::string::npos ? s : s.substr(0, dot);
        std::string dec_part = dot == std::string::npos ? "" : s.substr(dot);
        bool is_neg = !int_part.empty() && int_part[0] == '-';
        std::string digits = is_neg ? int_part.substr(1) : int_part;
        std::string grouped;
        int count = 0;
        for (auto rit = digits.rbegin(); rit != digits.rend(); ++rit) {
            if (count > 0 && count % 3 == 0) grouped += ',';
            grouped += *rit;
            count++;
        }
        std::reverse(grouped.begin(), grouped.end());
        return (is_neg ? "-" : "") + grouped + dec_part;
    }
    if (func_name == "YEAR") {
        std::string v = arg_at(0);
        auto dash = v.find('-');
        return dash == std::string::npos ? "" : v.substr(0, dash);
    }
    if (func_name == "MONTH") {
        std::string v = arg_at(0);
        auto d1 = v.find('-');
        if (d1 == std::string::npos) return "";
        auto d2 = v.find('-', d1 + 1);
        return v.substr(d1 + 1, d2 == std::string::npos ? std::string::npos : d2 - d1 - 1);
    }
    if (func_name == "DAY" || func_name == "DAYOFMONTH") {
        std::string v = arg_at(0);
        auto d1 = v.find('-');
        auto d2 = d1 == std::string::npos ? std::string::npos : v.find('-', d1 + 1);
        if (d2 == std::string::npos) return "";
        std::string rest = v.substr(d2 + 1);
        auto sp = rest.find(' ');
        return sp == std::string::npos ? rest : rest.substr(0, sp);
    }
    if (func_name == "HOUR" || func_name == "MINUTE" || func_name == "SECOND") {
        std::string v = arg_at(0);
        auto sp = v.find(' ');
        if (sp == std::string::npos) return "0";
        std::string t = v.substr(sp + 1);
        auto c1 = t.find(':');
        if (func_name == "HOUR") return c1 == std::string::npos ? "0" : t.substr(0, c1);
        if (c1 == std::string::npos) return "0";
        auto c2 = t.find(':', c1 + 1);
        if (func_name == "MINUTE") return t.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        return c2 == std::string::npos ? "0" : t.substr(c2 + 1);
    }
    if (func_name == "DAYOFWEEK") {
        auto d = parse_date(arg_at(0));
        return d ? std::to_string(weekday_sunday1(to_sys_days(*d))) : EXECUTOR_NULL_VALUE;
    }
    if (func_name == "DAYOFYEAR") {
        auto d = parse_date(arg_at(0));
        return d ? std::to_string(day_of_year(to_sys_days(*d))) : EXECUTOR_NULL_VALUE;
    }
    if (func_name == "WEEKDAY") {
        auto d = parse_date(arg_at(0));
        return d ? std::to_string(weekday_monday0(to_sys_days(*d))) : EXECUTOR_NULL_VALUE;
    }
    if (func_name == "LAST_DAY") {
        auto d = parse_date(arg_at(0));
        if (!d) return EXECUTOR_NULL_VALUE;
        int y = d->mo == 12 ? d->y + 1 : d->y;
        int m = d->mo == 12 ? 1 : d->mo + 1;
        sys_days next = to_sys_days(SimpleDate{y, m, 1});
        return format_date(from_sys_days(next - days{1}));
    }
    if (func_name == "TIMESTAMPDIFF") {
        std::string unit = to_upper(arg_at(0));
        auto dt1 = parse_datetime(arg_at(1));
        auto dt2 = parse_datetime(arg_at(2));
        if (!dt1 || !dt2) return EXECUTOR_NULL_VALUE;
        auto s1 = to_sys_seconds(*dt1);
        auto s2 = to_sys_seconds(*dt2);
        auto diff_sec = (s2 - s1).count();
        if (unit == "SECOND") return std::to_string(diff_sec);
        if (unit == "MINUTE") return std::to_string(diff_sec / 60);
        if (unit == "HOUR") return std::to_string(diff_sec / 3600);
        if (unit == "DAY") return std::to_string(diff_sec / 86400);
        if (unit == "WEEK") return std::to_string(diff_sec / 86400 / 7);
        if (unit == "MONTH") {
            long long months = (static_cast<long long>(dt2->y) - dt1->y) * 12 + dt2->mo - dt1->mo;
            return std::to_string(months);
        }
        if (unit == "YEAR") {
            long long years = static_cast<long long>(dt2->y) - dt1->y;
            if (std::make_pair(dt2->mo, dt2->d) < std::make_pair(dt1->mo, dt1->d)) years -= 1;
            return std::to_string(years);
        }
        return EXECUTOR_NULL_VALUE;
    }
    if (func_name == "CURTIME" || func_name == "CURRENT_TIME") {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
        localtime_s(&tm_buf, &t);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm_buf);
        return buf;
    }
    if (func_name == "CURRENT_TIMESTAMP" || func_name == "LOCALTIME" || func_name == "LOCALTIMESTAMP") {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
        localtime_s(&tm_buf, &t);
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_buf);
        return buf;
    }
    if (func_name == "UNIX_TIMESTAMP") {
        if (args.empty()) {
            auto now = std::chrono::system_clock::now().time_since_epoch();
            return std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now).count());
        }
        auto dt = parse_datetime(arg_at(0));
        if (!dt) return EXECUTOR_NULL_VALUE;
        return std::to_string(to_sys_seconds(*dt).time_since_epoch().count());
    }
    if (func_name == "FROM_UNIXTIME") {
        long long ts = args.empty() ? 0 : parse_i64(resolve(args[0])).value_or(0);
        sys_seconds sec{seconds{ts}};
        auto days_part = std::chrono::floor<days>(sec);
        auto time_part = sec - days_part;
        year_month_day ymd{days_part};
        auto hms = std::chrono::hh_mm_ss{time_part};
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02u-%02u %02lld:%02lld:%02lld", static_cast<int>(ymd.year()),
                      static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()), static_cast<long long>(hms.hours().count()),
                      static_cast<long long>(hms.minutes().count()), static_cast<long long>(hms.seconds().count()));
        return buf;
    }
    if (func_name == "ISNULL") {
        std::string v = arg_at(0);
        return (v == EXECUTOR_NULL_VALUE || v.empty()) ? "1" : "0";
    }
    if (func_name == "BIT_LENGTH") return std::to_string(arg_at(0).size() * 8);
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
    if (func_name == "VERSION") return "2.3.0";
    // Regression: used to hardcode "root@localhost" regardless of who actually
    // authenticated -- g_current_user_ctx is synced from Executor::auth_user (set by the
    // native/MySQL protocol servers after a successful AUTH handshake) the same way
    // g_current_db_ctx is synced from current_db, just above.
    if (func_name == "CURRENT_USER" || func_name == "USER" || func_name == "SESSION_USER" || func_name == "SYSTEM_USER")
        return g_current_user_ctx + "@localhost";

    return func_name + "()";
}

} // namespace engine
