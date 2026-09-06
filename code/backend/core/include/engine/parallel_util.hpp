#pragma once

// Shared parallel-execution gating/helpers used by multiple executor_*.cpp files
// (SELECT's WHERE/ORDER BY/GROUP BY paths and the set-operation UNION/INTERSECT/EXCEPT
// paths both need the same rayon-equivalent gating and a real parallel sort). Faithful
// port of rusql-core/src/engine/executor.rs's PARALLEL_CHUNK/parallel_enabled()/
// parallel_min_rows() free functions.

#include <algorithm>
#include <cstdlib>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include "engine/row.hpp"
#include "engine/thread_pool.hpp"

namespace engine {

constexpr std::size_t PARALLEL_CHUNK = 4096;

// 병렬 쿼리 실행 on/off. 환경변수 RUSTDB_PARALLEL=0|off 이면 비활성(벤치마크 대조군용).
inline bool parallel_enabled() {
    const char* v = std::getenv("RUSTDB_PARALLEL");
    if (!v) return true;
    std::string s(v);
    if (s == "0") return false;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s != "off";
}

// 병렬 스캔 최소 행 수: CPU 코어 수에 반비례 조정.
// NOTE: the Rust original checked the env var literally named "RUSTDB_parallel_min_rows()"
// (a real typo — a valid env var name can't contain '(' ')', so the override never
// actually applied) and was faithfully preserved that way here for a while (docs/mds/
// PLAN.md P2). Fixed to a real, settable name matching parallel_enabled()'s
// RUSTDB_PARALLEL convention.
inline std::size_t parallel_min_rows() {
    if (const char* v = std::getenv("RUSTDB_PARALLEL_MIN_ROWS")) {
        try {
            std::size_t n = std::stoull(v);
            return n < 1 ? 1 : n;
        } catch (...) {}
    }
    unsigned cpus = std::thread::hardware_concurrency();
    if (cpus == 0) cpus = 1;
    std::size_t v = 10000 / cpus;
    return v < 1000 ? 1000 : v;
}

// Actually-parallel sort: splits into hardware_concurrency() contiguous slices, sorts
// each slice in parallel via the global ThreadPool (unstable, matching Rust's
// par_sort_unstable_by), then merges the sorted slices back with a sequential merge
// tree (std::merge is cheap relative to the comparison-heavy sort, so parallelizing
// only the sort phase still gives a real speedup for large row counts).
template <typename Less>
void parallel_sort(std::vector<Row>& v, Less less) {
    std::size_t n = v.size();
    unsigned cpus = std::thread::hardware_concurrency();
    if (cpus == 0) cpus = 4;
    std::size_t n_chunks = std::min<std::size_t>(cpus, n);
    if (n_chunks < 2) {
        std::sort(v.begin(), v.end(), less);
        return;
    }

    std::vector<std::size_t> starts(n_chunks + 1);
    std::size_t base = n / n_chunks, rem = n % n_chunks;
    starts[0] = 0;
    for (std::size_t c = 0; c < n_chunks; c++) starts[c + 1] = starts[c] + base + (c < rem ? 1 : 0);

    std::vector<std::vector<Row>> chunks(n_chunks);
    for (std::size_t c = 0; c < n_chunks; c++) {
        chunks[c].assign(std::make_move_iterator(v.begin() + static_cast<std::ptrdiff_t>(starts[c])),
                          std::make_move_iterator(v.begin() + static_cast<std::ptrdiff_t>(starts[c + 1])));
    }
    ThreadPool::global().parallel_for(n_chunks, [&](std::size_t c) { std::sort(chunks[c].begin(), chunks[c].end(), less); });

    while (chunks.size() > 1) {
        std::vector<std::vector<Row>> next;
        next.reserve((chunks.size() + 1) / 2);
        for (std::size_t i = 0; i < chunks.size(); i += 2) {
            if (i + 1 < chunks.size()) {
                std::vector<Row> merged;
                merged.reserve(chunks[i].size() + chunks[i + 1].size());
                std::merge(std::make_move_iterator(chunks[i].begin()), std::make_move_iterator(chunks[i].end()),
                           std::make_move_iterator(chunks[i + 1].begin()), std::make_move_iterator(chunks[i + 1].end()),
                           std::back_inserter(merged), less);
                next.push_back(std::move(merged));
            } else {
                next.push_back(std::move(chunks[i]));
            }
        }
        chunks = std::move(next);
    }
    v = std::move(chunks[0]);
}

} // namespace engine
