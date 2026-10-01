// Index-assisted candidate search for UPDATE and DELETE.
//
// Before this, both statements found their target rows by walking every physical row
// version of the table and evaluating the WHERE clause on each (O(table)) -- only
// `pk = literal` (and DELETE ... pk BETWEEN) had a shortcut. This asks the cost-based
// planner, exactly like SELECT does, whether an index can narrow the search, and turns the
// index's answer into positions in s.tables[table].
//
// The index is only ever a SOURCE OF CANDIDATES, never the final word. Every candidate is
// resolved to its real row through the row_pk_pos cache and checked against it:
//   * the index copy and the real row must be the same version (same pk, same _xmin);
//   * the real row must be visible to the statement and must satisfy the full condition.
// Anything that does not add up (cache miss, stale position, index and row store
// disagreeing about whether a version is alive) makes the whole attempt `usable = false`,
// and the caller falls back to the plain scan it always had -- so a wrong index can cost
// speed but not correctness for the rows it does name. What an index can still get wrong is
// rows it fails to name; the paths below are limited to the ones where that cannot
// happen by construction (see widen() and the notes at each AccessPath).

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include "engine/executor/executor.hpp"
#include "engine/planner.hpp"

namespace engine {

namespace {

std::size_t min_rows_from_env() {
    if (const char* e = std::getenv("RUSQL_DML_INDEX_MIN_ROWS")) {
        try {
            return static_cast<std::size_t>(std::stoull(e));
        } catch (...) {
        }
    }
    return 64;
}

bool parse_f64(const std::string& s, double& out) {
    if (s.empty()) return false;
    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

// One index entry's view of a row version.
struct Cand {
    std::string pk, xmin, xmax;
    bool via_pk_index = false;
};

Cand cand_of(const Row& r, const std::string& pk_col) {
    auto get = [&](const std::string& k, const char* dflt) {
        auto it = r.find(k);
        return it != r.end() ? it->second : std::string(dflt);
    };
    return {get(pk_col, ""), get("_xmin", "0"), get("_xmax", "0")};
}

// B+Tree keys order numerically-equal strings by their text ("07" < "7" < "7.0"), while a
// WHERE clause treats them as equal -- so `price = 7` must also find a row stored as "7.00",
// and `qty >= 7` one stored as "07". A numeric bound is therefore widened by one ulp in the
// direction it needs to cover; the candidates are re-checked against the real condition
// afterwards, so the widening can only add rows that get filtered out again. A non-numeric
// bound is used as it is. nullopt = "cannot express this bound safely" -> no index path.
std::optional<std::string> widen(const std::string& key, bool lower) {
    double v;
    if (!parse_f64(key, v)) return key;
    if (!std::isfinite(v)) return std::nullopt;
    double w = std::nextafter(v, lower ? -INFINITY : INFINITY);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", w);
    double back;
    if (!parse_f64(buf, back) || back != w) return std::nullopt;
    return std::string(buf);
}

enum class Gather { Ok, Unsupported, TooMany };

// Parses one index value (a JSON row, or a JSON array of rows) into candidates.
bool append_json(const std::string& json, bool is_array, const std::string& pk_col, std::vector<Cand>& out, std::size_t cap,
                 bool via_pk_index = false) {
    try {
        auto j = nlohmann::json::parse(json);
        if (is_array) {
            for (auto& e : j) {
                out.push_back(cand_of(e.get<Row>(), pk_col));
                out.back().via_pk_index = via_pk_index;
                if (out.size() > cap) return false;
            }
        } else {
            out.push_back(cand_of(j.get<Row>(), pk_col));
            out.back().via_pk_index = via_pk_index;
            if (out.size() > cap) return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

Gather gather(SharedDatabase& s, const std::string& table, const AccessPath& ap, const std::string& pk_col, std::vector<Cand>& out,
              std::size_t cap) {
    // `pk_tree`: the values are the PK B+Tree's (one row per pk), not a secondary index's buckets
    auto from_range = [&](const BPlusTree& tree, const std::string& lo, const std::string& hi, bool is_array, bool pk_tree) {
        for (auto& v : tree.range_search(lo, hi)) {
            if (!append_json(v, is_array, pk_col, out, cap, pk_tree)) return Gather::TooMany;
        }
        return Gather::Ok;
    };
    auto from_pairs = [&](const std::vector<std::pair<std::string, std::string>>& pairs, bool is_array, bool pk_tree) {
        for (auto& kv : pairs) {
            if (!append_json(kv.second, is_array, pk_col, out, cap, pk_tree)) return Gather::TooMany;
        }
        return Gather::Ok;
    };

    return std::visit(
        [&](const auto& a) -> Gather {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, AccessPath::PkPoint>) {
                auto it = s.indexes.find(table);
                auto lo = widen(a.key, true), hi = widen(a.key, false);
                if (it == s.indexes.end() || !lo || !hi) return Gather::Unsupported;
                return from_range(it->second, *lo, *hi, false, true);
            } else if constexpr (std::is_same_v<T, AccessPath::PkBetween>) {
                auto it = s.indexes.find(table);
                auto lo = widen(a.start, true), hi = widen(a.end, false);
                if (it == s.indexes.end() || !lo || !hi) return Gather::Unsupported;
                return from_range(it->second, *lo, *hi, false, true);
            } else if constexpr (std::is_same_v<T, AccessPath::PkRange>) {
                auto it = s.indexes.find(table);
                if (it == s.indexes.end()) return Gather::Unsupported;
                auto bound = widen(a.key, range_op_is_lower_bound(a.op));
                if (!bound) return Gather::Unsupported;
                return from_pairs(range_op_is_lower_bound(a.op) ? it->second.scan_from(*bound, true) : it->second.scan_to(*bound, true), false, true);
            } else if constexpr (std::is_same_v<T, AccessPath::SecondaryPoint>) {
                auto it = s.indexes.find(a.index_key);
                auto lo = widen(a.key, true), hi = widen(a.key, false);
                if (it == s.indexes.end() || !lo || !hi) return Gather::Unsupported;
                return from_range(it->second, *lo, *hi, true, false);
            } else if constexpr (std::is_same_v<T, AccessPath::SecondaryBetween>) {
                auto it = s.indexes.find(a.index_key);
                auto lo = widen(a.start, true), hi = widen(a.end, false);
                if (it == s.indexes.end() || !lo || !hi) return Gather::Unsupported;
                return from_range(it->second, *lo, *hi, true, false);
            } else if constexpr (std::is_same_v<T, AccessPath::SecondaryRange>) {
                auto it = s.indexes.find(a.index_key);
                if (it == s.indexes.end()) return Gather::Unsupported;
                auto bound = widen(a.key, range_op_is_lower_bound(a.op));
                if (!bound) return Gather::Unsupported;
                return from_pairs(range_op_is_lower_bound(a.op) ? it->second.scan_from(*bound, true) : it->second.scan_to(*bound, true), true, false);
            } else if constexpr (std::is_same_v<T, AccessPath::SecondaryLikePrefix>) {
                // The prefix scan stops at the first key that does not start with the prefix. For a
                // prefix that looks like a number that is wrong ("12%": numeric order puts 13
                // before 120), so only prefixes that cannot be read as a number are supported.
                auto it = s.indexes.find(a.index_key);
                if (it == s.indexes.end() || a.prefix.empty()) return Gather::Unsupported;
                char c0 = a.prefix[0];
                if (std::isdigit(static_cast<unsigned char>(c0)) || c0 == '-' || c0 == '+' || c0 == '.') return Gather::Unsupported;
                for (auto& kv : it->second.scan_from(a.prefix, true)) {
                    if (kv.first.compare(0, a.prefix.size(), a.prefix) != 0) break;
                    if (!append_json(kv.second, true, pk_col, out, cap)) return Gather::TooMany;
                }
                return Gather::Ok;
            } else if constexpr (std::is_same_v<T, AccessPath::HashPoint>) {
                // A hash bucket is the exact key text; a numeric literal can equal several texts.
                double dummy;
                auto it = s.hash_indexes.find(a.index_key);
                if (it == s.hash_indexes.end() || parse_f64(a.key, dummy)) return Gather::Unsupported;
                for (auto& r : it->second.get(a.key)) {
                    out.push_back(cand_of(r, pk_col));
                    if (out.size() > cap) return Gather::TooMany;
                }
                return Gather::Ok;
            } else if constexpr (std::is_same_v<T, AccessPath::IndexIntersection>) {
                std::vector<std::vector<Cand>> sets;
                for (auto& sub : a.paths) {
                    std::vector<Cand> one;
                    Gather g = gather(s, table, sub, pk_col, one, cap);
                    if (g != Gather::Ok) return g;
                    sets.push_back(std::move(one));
                }
                if (sets.empty()) return Gather::Unsupported;
                std::unordered_map<std::string, std::size_t> seen;
                for (std::size_t i = 1; i < sets.size(); i++) {
                    seen.clear();
                    for (auto& c : sets[i]) seen[c.pk]++;
                    std::vector<Cand> keep;
                    for (auto& c : sets[0]) {
                        if (seen.count(c.pk)) keep.push_back(c);
                    }
                    sets[0] = std::move(keep);
                }
                out = std::move(sets[0]);
                return Gather::Ok;
            } else {
                // SeqScan, and the composite-index paths: a composite key joins every column's text,
                // so the numeric-equality problem above would apply per segment. Not supported here.
                return Gather::Unsupported;
            }
        },
        ap.data);
}

// Same single source of truth as SELECT (Planner::choose_access); on top of it, an AND of
// several predicates may use ANY ONE indexable predicate: the result is a subset of what
// that one predicate matches, and every candidate is re-checked against the whole condition.
AccessPath pick_access(const Planner& planner, const std::string& table, const std::optional<CondExpr>& condition,
                       const std::string& pk_col) {
    std::optional<std::string> pk = pk_col;
    AccessPath path = planner.choose_access(table, condition, pk);
    if (!std::holds_alternative<AccessPath::SeqScan>(path.data) || !condition) return path;
    if (std::holds_alternative<CondExpr::Leaf>(condition->data)) return path;

    auto rank = [](const AccessPath& p) {
        if (std::holds_alternative<AccessPath::SeqScan>(p.data)) return 99;
        if (std::holds_alternative<AccessPath::PkPoint>(p.data) || std::holds_alternative<AccessPath::SecondaryPoint>(p.data) ||
            std::holds_alternative<AccessPath::HashPoint>(p.data))
            return 0;
        return 1;
    };
    AccessPath best;
    int best_rank = 99;
    for (const Condition* leaf : collect_and_leaves(*condition)) {
        CondExpr one{CondExpr::Leaf{*leaf}};
        AccessPath p = planner.choose_access(table, one, pk);
        int r = rank(p);
        if (r < best_rank) {
            best = std::move(p);
            best_rank = r;
        }
    }
    return best;
}

} // namespace

std::atomic<std::size_t> Executor::dml_index_min_rows{min_rows_from_env()};
std::atomic<std::uint64_t> Executor::dml_index_hits{0};

Executor::DmlIndexHit Executor::dml_index_positions(SharedDatabase& s, const std::string& table, const std::optional<CondExpr>& condition,
                                                    const std::string& pk_col, const std::function<bool(const Row&)>& visible,
                                                    std::uint64_t self_txn_id) {
    DmlIndexHit hit;
    auto tit = s.tables.find(table);
    if (!condition || tit == s.tables.end()) return hit;
    const std::vector<Row>& rows = tit->second;
    if (rows.size() < dml_index_min_rows.load()) return hit;

    // Another open transaction may have uncommitted versions the indexes do not describe for this
    // statement (an index holds only the LATEST version of a row) -- the same rule SELECT applies.
    {
        auto active = s.active_txn_ids->lock();
        for (auto id : *active) {
            if (id != self_txn_id) return hit;
        }
    }

    // Positions come from the single-column pk -> position cache.
    auto* schema = s.catalog.get_table(table);
    if (!schema) return hit;
    std::size_t pk_count = 0;
    for (auto& c : schema->columns) {
        if (c.primary_key) pk_count++;
    }
    if (pk_count != 1) return hit;

    Planner planner(s.tables, s.indexes, s.index_meta, s.composite_indexes, s.hash_indexes, s.hash_index_meta, s.catalog, s.table_stats);
    AccessPath ap = pick_access(planner, table, condition, pk_col);
    if (std::holds_alternative<AccessPath::SeqScan>(ap.data)) return hit;

    // A result that is a large share of the table is cheaper to scan than to parse out of JSON.
    const std::size_t cap = std::max<std::size_t>(128, rows.size() / 8);
    std::vector<Cand> cands;
    if (gather(s, table, ap, pk_col, cands, cap) != Gather::Ok) return hit;

    auto mit = s.row_pk_pos.find(table);
    std::unordered_map<std::string, bool> done;
    std::vector<std::size_t> positions;
    // The real row of a pk, through the position cache; nullptr if the cache has no (valid) entry.
    auto resolve = [&](const std::string& pk) -> const Row* {
        if (mit == s.row_pk_pos.end()) return nullptr;
        auto pit = mit->second.find(pk);
        if (pit == mit->second.end() || pit->second >= rows.size()) return nullptr;
        const Row& r = rows[pit->second];
        auto k = r.find(pk_col);
        return (k != r.end() && k->second == pk) ? &r : nullptr;
    };

    for (auto& c : cands) {
        if (c.xmax != "0") {
            // A dead version's copy. Secondary-index buckets hold every version of a row (a rebuild
            // after ROLLBACK/VACUUM puts the dead ones back in next to the live one), so a dead copy
            // says nothing about the row: its live version, if any, is an entry of its own.
            if (!c.via_pk_index) continue;
            // The PK B+Tree keeps ONE copy per pk, and after such a rebuild that copy can be a dead
            // version hiding a live one -- only the real row can tell.
            const Row* r = resolve(c.pk);
            if (!r) {
                hit.cache_gap = true;
                return hit;
            }
            auto x = r->find("_xmax");
            if (x == r->end() || x->second == "0") return hit; // index hides a live row: do not trust it
            continue;
        }
        if (!done.emplace(c.pk, true).second) continue;

        const Row* row = resolve(c.pk);
        std::size_t pos = 0;
        if (row) pos = static_cast<std::size_t>(row - rows.data());
        if (!row) {
            hit.cache_gap = true; // no usable position for a live candidate: heal the cache, scan this time
            return hit;
        }
        auto xm = row->find("_xmin");
        if ((xm != row->end() ? xm->second : std::string("0")) != c.xmin) return hit; // index and row store disagree

        if (!visible(*row)) return hit; // index says alive, row store says not: do not trust either
        if (!matches_condexpr(*row, condition)) continue;
        positions.push_back(pos);
    }

    std::sort(positions.begin(), positions.end());
    hit.positions = std::move(positions);
    hit.usable = true;
    dml_index_hits.fetch_add(1, std::memory_order_relaxed);
    return hit;
}

void Executor::rebuild_pk_positions(SharedDatabase& s, const std::string& table, const std::string& pk_col) {
    auto tit = s.tables.find(table);
    auto mit = s.row_pk_pos.find(table);
    if (tit == s.tables.end() || mit == s.row_pk_pos.end()) return;
    auto& pos_map = mit->second;
    pos_map.clear();
    const std::vector<Row>& rows = tit->second;
    for (std::size_t i = 0; i < rows.size(); i++) {
        auto x = rows[i].find("_xmax");
        if (x != rows[i].end() && x->second != "0") continue; // a dead version is never "the" row of its pk
        auto k = rows[i].find(pk_col);
        if (k != rows[i].end()) pos_map[k->second] = i;
    }
}

} // namespace engine
