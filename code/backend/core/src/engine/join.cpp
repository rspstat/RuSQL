#include "engine/join.hpp"

#include "engine/thread_pool.hpp"

#include <algorithm>
#include <charconv>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace engine {

namespace {
bool try_parse_f64(const std::string& s, double& out) {
    if (s.empty()) return false;
    auto res = std::from_chars(s.data(), s.data() + s.size(), out);
    return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

int sort_cmp(const std::string& a, const std::string& b) {
    double af, bf;
    if (try_parse_f64(a, af) && try_parse_f64(b, bf)) {
        if (af < bf) return -1;
        if (af > bf) return 1;
        return 0;
    }
    return a.compare(b) < 0 ? -1 : (a.compare(b) > 0 ? 1 : 0);
}

std::vector<std::string> non_qualified_keys(const Row* r) {
    std::vector<std::string> out;
    if (!r) return out;
    for (auto& [k, v] : *r) {
        (void)v;
        if (!k.empty() && k[0] != '_' && k.find('.') == std::string::npos) out.push_back(k);
    }
    return out;
}

// Faithful port of hash_join's Inner/Left probe phase: Rust parallelizes this
// unconditionally with `left.par_iter().flat_map(...)` (no parallel_enabled()/
// parallel_min_rows() gating — that gating only applies to the executor-level
// rayon call sites, not this one). Chunks `left` across the global ThreadPool and
// concatenates per-chunk outputs in original order, matching rayon's order-preserving
// collect() over an indexed source.
template <typename F>
std::vector<Row> probe_parallel(const std::vector<Row>& left, F&& per_row) {
    std::size_t n = left.size();
    unsigned cpus = std::thread::hardware_concurrency();
    if (cpus == 0) cpus = 4;
    std::size_t n_chunks = std::min<std::size_t>(cpus, n);
    if (n_chunks < 2) {
        std::vector<Row> out;
        for (auto& l : left) {
            for (auto& r : per_row(l)) out.push_back(std::move(r));
        }
        return out;
    }
    std::vector<std::size_t> starts(n_chunks + 1);
    std::size_t base = n / n_chunks, rem = n % n_chunks;
    starts[0] = 0;
    for (std::size_t c = 0; c < n_chunks; c++) starts[c + 1] = starts[c] + base + (c < rem ? 1 : 0);

    std::vector<std::vector<Row>> chunk_out(n_chunks);
    ThreadPool::global().parallel_for(n_chunks, [&](std::size_t c) {
        std::vector<Row> local;
        for (std::size_t i = starts[c]; i < starts[c + 1]; i++) {
            for (auto& r : per_row(left[i])) local.push_back(std::move(r));
        }
        chunk_out[c] = std::move(local);
    });

    std::size_t total = 0;
    for (auto& c : chunk_out) total += c.size();
    std::vector<Row> out;
    out.reserve(total);
    for (auto& c : chunk_out) {
        for (auto& r : c) out.push_back(std::move(r));
    }
    return out;
}
} // namespace

void merge_right(Row& merged, const Row& right, const std::string& table) {
    for (auto& [k, v] : right) {
        merged[table + "." + k] = v;
        merged.emplace(k, v); // insert-if-absent: matches Rust's `.entry(k).or_insert_with(...)`
    }
}

void null_right(Row& merged, const std::vector<std::string>& cols, const std::string& table) {
    for (auto& col : cols) {
        merged[table + "." + col] = JOIN_NULL_VALUE;
        merged.emplace(col, JOIN_NULL_VALUE);
    }
}

std::vector<Row> sort_merge_join(const std::vector<Row>& left, const std::vector<Row>& right, JoinType join_type,
                                  const std::string& table, const std::string& probe_col, const std::string& build_col,
                                  const std::vector<std::string>& right_schema_cols) {
    auto key_left = [&](const Row& row) -> std::string {
        if (auto it = row.find(probe_col); it != row.end()) return it->second;
        std::string suffix = "." + probe_col;
        for (auto& [k, v] : row) {
            if (k.size() >= suffix.size() && k.compare(k.size() - suffix.size(), suffix.size(), suffix) == 0) return v;
        }
        return "";
    };
    auto key_right = [&](const Row& row) -> std::string {
        if (auto it = row.find(build_col); it != row.end()) return it->second;
        if (auto it = row.find(table + "." + build_col); it != row.end()) return it->second;
        return "";
    };

    std::vector<Row> ls = left;
    std::stable_sort(ls.begin(), ls.end(), [&](const Row& a, const Row& b) { return sort_cmp(key_left(a), key_left(b)) < 0; });
    std::vector<Row> rs = right;
    std::stable_sort(rs.begin(), rs.end(), [&](const Row& a, const Row& b) { return sort_cmp(key_right(a), key_right(b)) < 0; });

    std::vector<Row> out;

    if (join_type == JoinType::Inner) {
        std::size_t li = 0, ri = 0;
        while (li < ls.size() && ri < rs.size()) {
            std::string lk = key_left(ls[li]);
            std::string rk = key_right(rs[ri]);
            int cmp = sort_cmp(lk, rk);
            if (cmp < 0) { li++; }
            else if (cmp > 0) { ri++; }
            else {
                std::size_t li0 = li;
                while (li < ls.size() && key_left(ls[li]) == lk) li++;
                std::size_t ri0 = ri;
                while (ri < rs.size() && key_right(rs[ri]) == lk) ri++;
                for (std::size_t a = li0; a < li; a++) {
                    for (std::size_t b = ri0; b < ri; b++) {
                        Row merged = ls[a];
                        merge_right(merged, rs[b], table);
                        out.push_back(std::move(merged));
                    }
                }
            }
        }
    } else if (join_type == JoinType::Left) {
        std::size_t ri_start = 0;
        for (auto& l : ls) {
            std::string lk = key_left(l);
            while (ri_start < rs.size() && sort_cmp(key_right(rs[ri_start]), lk) < 0) ri_start++;
            std::size_t ri = ri_start;
            bool matched = false;
            while (ri < rs.size() && sort_cmp(key_right(rs[ri]), lk) == 0) {
                Row merged = l;
                merge_right(merged, rs[ri], table);
                out.push_back(std::move(merged));
                matched = true;
                ri++;
            }
            if (!matched) {
                Row merged = l;
                null_right(merged, right_schema_cols, table);
                out.push_back(std::move(merged));
            }
        }
    } else if (join_type == JoinType::Right) {
        std::vector<std::string> left_cols = non_qualified_keys(ls.empty() ? nullptr : &ls[0]);
        std::size_t li_start = 0;
        for (auto& r : rs) {
            std::string rk = key_right(r);
            while (li_start < ls.size() && sort_cmp(key_left(ls[li_start]), rk) < 0) li_start++;
            std::size_t li = li_start;
            bool matched = false;
            while (li < ls.size() && sort_cmp(key_left(ls[li]), rk) == 0) {
                Row merged = ls[li];
                merge_right(merged, r, table);
                out.push_back(std::move(merged));
                matched = true;
                li++;
            }
            if (!matched) {
                Row merged;
                for (auto& c : left_cols) merged[c] = JOIN_NULL_VALUE;
                merge_right(merged, r, table);
                out.push_back(std::move(merged));
            }
        }
    }
    return out;
}

std::vector<Row> hash_join(const std::vector<Row>& left, const std::vector<Row>& right, JoinType join_type,
                            const std::string& table, const std::string& probe_col, const std::string& build_col,
                            const std::vector<std::string>& right_schema_cols) {
    std::unordered_map<std::string, std::vector<const Row*>> hash;
    for (auto& r : right) {
        std::string key;
        if (auto it = r.find(build_col); it != r.end()) key = it->second;
        else if (auto it2 = r.find(table + "." + build_col); it2 != r.end()) key = it2->second;
        hash[key].push_back(&r);
    }

    std::string probe_suffix = "." + probe_col;

    if (join_type == JoinType::Inner) {
        return probe_parallel(left, [&](const Row& l) -> std::vector<Row> {
            std::string pk;
            if (auto it = l.find(probe_col); it != l.end()) pk = it->second;
            else {
                for (auto& [k, v] : l) {
                    if (k.size() >= probe_suffix.size() && k.compare(k.size() - probe_suffix.size(), probe_suffix.size(), probe_suffix) == 0) {
                        pk = v;
                        break;
                    }
                }
            }
            std::vector<Row> matches;
            auto it = hash.find(pk);
            if (it != hash.end()) {
                matches.reserve(it->second.size());
                for (auto* r : it->second) {
                    Row merged = l;
                    merge_right(merged, *r, table);
                    matches.push_back(std::move(merged));
                }
            }
            return matches;
        });
    }
    if (join_type == JoinType::Left) {
        return probe_parallel(left, [&](const Row& l) -> std::vector<Row> {
            std::string pk;
            if (auto it = l.find(probe_col); it != l.end()) pk = it->second;
            else {
                for (auto& [k, v] : l) {
                    if (k.size() >= probe_suffix.size() && k.compare(k.size() - probe_suffix.size(), probe_suffix.size(), probe_suffix) == 0) {
                        pk = v;
                        break;
                    }
                }
            }
            std::vector<Row> matches;
            auto it = hash.find(pk);
            if (it != hash.end()) {
                matches.reserve(it->second.size());
                for (auto* r : it->second) {
                    Row merged = l;
                    merge_right(merged, *r, table);
                    matches.push_back(std::move(merged));
                }
            } else {
                Row merged = l;
                null_right(merged, right_schema_cols, table);
                matches.push_back(std::move(merged));
            }
            return matches;
        });
    }
    if (join_type == JoinType::Right) {
        std::unordered_map<std::string, std::vector<const Row*>> left_hash;
        for (auto& l : left) {
            std::string key;
            if (auto it = l.find(probe_col); it != l.end()) key = it->second;
            left_hash[key].push_back(&l);
        }
        std::vector<std::string> left_cols = non_qualified_keys(left.empty() ? nullptr : &left[0]);
        std::vector<Row> out;
        for (auto& r : right) {
            std::string key;
            if (auto it = r.find(build_col); it != r.end()) key = it->second;
            auto it = left_hash.find(key);
            if (it != left_hash.end()) {
                for (auto* l : it->second) {
                    Row merged = *l;
                    merge_right(merged, r, table);
                    out.push_back(std::move(merged));
                }
            } else {
                Row merged;
                for (auto& c : left_cols) merged[c] = JOIN_NULL_VALUE;
                merge_right(merged, r, table);
                out.push_back(std::move(merged));
            }
        }
        return out;
    }
    // Cross/Natural joins do not use Hash Join (matches Rust's `unreachable!()`).
    return {};
}

std::vector<Row> nested_loop_join(const std::vector<Row>& left, const std::vector<Row>& right, JoinType join_type,
                                   const std::string& table, const std::vector<std::string>& using_cols,
                                   const std::vector<std::string>& right_schema_cols,
                                   const std::function<bool(const Row&)>& on_match) {
    std::vector<Row> out;

    if (!using_cols.empty()) {
        for (auto& l : left) {
            for (auto& r : right) {
                bool matches = std::all_of(using_cols.begin(), using_cols.end(), [&](const std::string& col) {
                    auto lit = l.find(col);
                    auto rit = r.find(col);
                    std::string lv = lit != l.end() ? lit->second : JOIN_NULL_VALUE;
                    std::string rv = rit != r.end() ? rit->second : JOIN_NULL_VALUE;
                    return lv == rv && lv != JOIN_NULL_VALUE;
                });
                if (matches) {
                    Row merged = l;
                    merge_right(merged, r, table);
                    out.push_back(std::move(merged));
                }
            }
        }
        return out;
    }

    switch (join_type) {
        case JoinType::Inner:
            for (auto& l : left) {
                for (auto& r : right) {
                    Row merged = l;
                    merge_right(merged, r, table);
                    if (on_match(merged)) out.push_back(std::move(merged));
                }
            }
            break;
        case JoinType::Left:
            for (auto& l : left) {
                bool matched = false;
                for (auto& r : right) {
                    Row merged = l;
                    merge_right(merged, r, table);
                    if (on_match(merged)) {
                        out.push_back(std::move(merged));
                        matched = true;
                    }
                }
                if (!matched) {
                    Row merged = l;
                    null_right(merged, right_schema_cols, table);
                    out.push_back(std::move(merged));
                }
            }
            break;
        case JoinType::Right: {
            std::vector<std::string> left_cols = non_qualified_keys(left.empty() ? nullptr : &left[0]);
            for (auto& r : right) {
                bool matched = false;
                for (auto& l : left) {
                    Row merged = l;
                    merge_right(merged, r, table);
                    if (on_match(merged)) {
                        out.push_back(std::move(merged));
                        matched = true;
                    }
                }
                if (!matched) {
                    Row merged;
                    for (auto& c : left_cols) merged[c] = JOIN_NULL_VALUE;
                    merge_right(merged, r, table);
                    out.push_back(std::move(merged));
                }
            }
            break;
        }
        case JoinType::Cross:
            for (auto& l : left) {
                for (auto& r : right) {
                    Row merged = l;
                    merge_right(merged, r, table);
                    out.push_back(std::move(merged));
                }
            }
            break;
        case JoinType::Natural: {
            std::vector<std::string> common_cols;
            for (auto& rc : right_schema_cols) {
                bool present = !left.empty() && (left[0].count(rc) > 0);
                if (present) common_cols.push_back(rc);
            }
            for (auto& l : left) {
                for (auto& r : right) {
                    Row merged = l;
                    merge_right(merged, r, table);
                    bool matches = std::all_of(common_cols.begin(), common_cols.end(), [&](const std::string& col) {
                        auto lit = l.find(col);
                        auto rit = r.find(col);
                        std::string lv = lit != l.end() ? lit->second : "";
                        std::string rv = rit != r.end() ? rit->second : "";
                        return lv == rv;
                    });
                    if (matches) out.push_back(std::move(merged));
                }
            }
            break;
        }
        case JoinType::FullOuter: {
            std::vector<std::string> left_cols = non_qualified_keys(left.empty() ? nullptr : &left[0]);
            std::unordered_set<std::size_t> matched_right;
            for (auto& l : left) {
                bool any_match = false;
                for (std::size_t ri = 0; ri < right.size(); ri++) {
                    Row merged = l;
                    merge_right(merged, right[ri], table);
                    if (on_match(merged)) {
                        out.push_back(std::move(merged));
                        matched_right.insert(ri);
                        any_match = true;
                    }
                }
                if (!any_match) {
                    Row merged = l;
                    null_right(merged, right_schema_cols, table);
                    out.push_back(std::move(merged));
                }
            }
            for (std::size_t ri = 0; ri < right.size(); ri++) {
                if (!matched_right.count(ri)) {
                    Row merged;
                    for (auto& c : left_cols) merged[c] = JOIN_NULL_VALUE;
                    merge_right(merged, right[ri], table);
                    out.push_back(std::move(merged));
                }
            }
            break;
        }
    }
    return out;
}

} // namespace engine
