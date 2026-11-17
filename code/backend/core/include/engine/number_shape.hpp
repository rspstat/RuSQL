#pragma once

// R96 -- how many places a number shows, worked out from the expression (the types MySQL gives it), not from the text of the value that came out:
//   * the exact number of an expression (a DECIMAL(10,2) column shows 2 places, an integer 0, `1.5` 1, `a + b` the wider, `a * b` the sum, `a / b` the dividend's plus 4) -- COALESCE(n, 0)
//     of a DECIMAL(10,2) column is 0.00 where n is NULL, LEAST(1.5, 2.25) is 1.50, the columns of a UNION have the widest of their branches;
//   * a DOUBLE that has a fixed number of places: PI() is 3.141593 (6 places) in MySQL's text while its value is the whole double (PI() * 2 is 6.283185, PI() / 2 1.5707963268).
// The values stay what they are inside an expression; only the text a result shows (and the text a string function is given) is written with the places.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "engine/parser/ast.hpp"

namespace engine::number_shape {

inline std::string upper_of(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

// COALESCE / IFNULL / GREATEST / LEAST: the value is one of the arguments, written with the places of the widest
inline bool picks_an_argument(const std::string& name) {
    const std::string n = upper_of(name);
    return n == "COALESCE" || n == "IFNULL" || n == "GREATEST" || n == "LEAST";
}

constexpr int kNotExact = -1; // a DOUBLE, a text, a date ...: no places to tell
constexpr int kNull = -2;     // a NULL written out: it has no type, the other arguments give the places

// The places an expression shows as an exact number, kNotExact when it is not one
inline int exact_scale(const ArithExpr& e) {
    if (auto* num = std::get_if<ArithExpr::Num>(&e.data)) {
        if (num->approx || num->value.empty() || num->value.find_first_of("eE") != std::string::npos) return kNotExact;
        const std::size_t dot = num->value.find('.');
        return dot == std::string::npos ? 0 : static_cast<int>(num->value.size() - dot - 1);
    }
    if (auto* str = std::get_if<ArithExpr::Str>(&e.data)) return str->value == "NULL" ? kNull : kNotExact;
    if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) return col->outer == 0 && col->kind == ColKind::Plain && !col->approx && !col->single ? (col->scale >= 0 ? col->scale : kNotExact) : kNotExact;
    auto both = [](int l, int r, auto&& combine) {
        if (l == kNotExact || r == kNotExact) return kNotExact;
        if (l == kNull) return r; // (NULL + x has the type of x)
        if (r == kNull) return l;
        return combine(l, r);
    };
    if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) return both(exact_scale(*v->lhs), exact_scale(*v->rhs), [](int l, int r) { return std::max(l, r); });
    if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) return both(exact_scale(*v->lhs), exact_scale(*v->rhs), [](int l, int r) { return std::max(l, r); });
    if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) return both(exact_scale(*v->lhs), exact_scale(*v->rhs), [](int l, int r) { return std::min(30, l + r); });
    if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) return both(exact_scale(*v->lhs), exact_scale(*v->rhs), [](int l, int) { return std::min(30, l + 4); });
    if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) {
        if (picks_an_argument(f->name)) {
            int widest = kNull;
            for (const ArithExpr& a : f->args) {
                const int s = exact_scale(a);
                if (s == kNotExact) return kNotExact;
                if (s != kNull) widest = widest == kNull ? s : std::max(widest, s);
            }
            return widest;
        }
        if (upper_of(f->name) == "NULLIF" && !f->args.empty()) return exact_scale(f->args[0]);
    }
    return kNotExact;
}

constexpr int kNoPlaces = -1;

// The places a DOUBLE has when MySQL gives it a FIXED number of them: PI() has 6 (SELECT PI() is 3.141593 and PI() * 2 is 6.283185, though the value inside is the whole double).
// Arithmetic with it keeps them: + - * have the widest of the two sides (an exact side counts its places, an integer 0), / adds 4 (PI() / 2 is 1.5707963268), and ABS, MOD,
// COALESCE, IFNULL, NULLIF, GREATEST, LEAST, CASE / IF hand them on. A side that is a DOUBLE with no fixed places (a column, SQRT(...), 1e0) takes them away: kNoPlaces.
inline int fixed_places(const ArithExpr& e);

// The places an expression has as a number MySQL counts places of: its fixed places, else the places of an exact number; kNoPlaces for a DOUBLE or a text; kNull for a written NULL
inline int places_of(const ArithExpr& e) {
    if (const int f = fixed_places(e); f >= 0) return f;
    return exact_scale(e);
}

inline int fixed_places(const ArithExpr& e) {
    // an aggregate of such a DOUBLE named in an expression (`SUM(PI()) + 1`, the column the row holds it as): the binder leaves the places it shows in `scale`, which an exact column
    // does not use for a DOUBLE (exact_scale asks !approx)
    if (auto* col = std::get_if<ArithExpr::Col>(&e.data)) return col->outer == 0 && col->approx && col->scale >= 0 ? col->scale : kNoPlaces;
    if (auto* f = std::get_if<ArithExpr::Func>(&e.data)) {
        const std::string name = upper_of(f->name);
        if (name == "PI" && f->args.empty()) return 6;
        auto widest = [&](const std::vector<const ArithExpr*>& parts) {
            int width = kNoPlaces;
            bool fixed = false;
            for (const ArithExpr* p : parts) {
                const int fp = fixed_places(*p);
                const int pl = fp >= 0 ? fp : exact_scale(*p);
                if (pl == kNotExact) return kNoPlaces;
                if (pl == kNull) continue;
                fixed = fixed || fp >= 0;
                width = std::max(width, pl);
            }
            return fixed ? width : kNoPlaces;
        };
        if (picks_an_argument(name) || name == "ABS" || name == "MOD") {
            std::vector<const ArithExpr*> parts;
            for (const ArithExpr& a : f->args) parts.push_back(&a);
            return widest(parts);
        }
        if (name == "NULLIF" && !f->args.empty()) return fixed_places(f->args[0]);
        if (name == "CASE") { // (the results: the 2nd, 4th ... argument and an ELSE that comes last)
            std::vector<const ArithExpr*> parts;
            for (std::size_t i = 1; i < f->args.size(); i += 2) parts.push_back(&f->args[i]);
            if (f->args.size() % 2 == 1) parts.push_back(&f->args.back());
            return widest(parts);
        }
        return kNoPlaces;
    }
    auto two = [&](const ArithExpr& l, const ArithExpr& r, int extra) {
        const int lf = fixed_places(l), rf = fixed_places(r);
        if (lf < 0 && rf < 0) return kNoPlaces; // (no fixed side: an exact number or a DOUBLE as before)
        const int lp = lf >= 0 ? lf : exact_scale(l), rp = rf >= 0 ? rf : exact_scale(r);
        if (lp == kNotExact || rp == kNotExact) return kNoPlaces;
        if (lp == kNull && rp == kNull) return kNoPlaces;
        return std::min(30, std::max(lp == kNull ? 0 : lp, rp == kNull ? 0 : rp) + extra);
    };
    if (auto* v = std::get_if<ArithExpr::Add>(&e.data)) return two(*v->lhs, *v->rhs, 0);
    if (auto* v = std::get_if<ArithExpr::Sub>(&e.data)) return two(*v->lhs, *v->rhs, 0);
    if (auto* v = std::get_if<ArithExpr::Mul>(&e.data)) return two(*v->lhs, *v->rhs, 0);
    if (auto* v = std::get_if<ArithExpr::Div>(&e.data)) return two(*v->lhs, *v->rhs, 4);
    return kNoPlaces;
}

// `v` with exactly `places` digits after the point
inline std::string with_places(double v, int places) {
    char buf[400];
    std::snprintf(buf, sizeof buf, "%.*f", places, v);
    return buf;
}

inline bool plain_decimal(const std::string& s) {
    std::size_t i = !s.empty() && (s[0] == '-' || s[0] == '+') ? 1 : 0;
    const std::size_t digits_from = i;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++;
    if (i == digits_from) return false;
    if (i == s.size()) return true;
    if (s[i] != '.') return false;
    i++;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) i++;
    return i == s.size();
}

// `value` (a number as text) with `places` digits after the point when it shows fewer: 1.5 with 2 is 1.50, 0 with 2 is 0.00. Anything else comes back as it is.
inline std::string written_with_places(const std::string& value, int places) {
    if (places <= 0 || !plain_decimal(value)) return value;
    const std::size_t dot = value.find('.');
    const std::size_t have = dot == std::string::npos ? 0 : value.size() - dot - 1;
    if (static_cast<int>(have) >= places) return value;
    std::string out = value;
    if (dot == std::string::npos) out += '.';
    out.append(static_cast<std::size_t>(places) - have, '0');
    return out;
}

} // namespace engine::number_shape
