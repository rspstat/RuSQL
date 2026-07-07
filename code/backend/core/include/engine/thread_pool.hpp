#pragma once

// A small, reusable worker-thread pool for parallelizing independent chunks of work.
//
// Rust's rayon (used by the original engine for par_chunks()/par_iter()) maintains a
// fixed-size pool of OS threads reused across every parallel call; earlier in this port,
// each parallel call site spawned brand-new std::thread objects per chunk/group instead,
// which measurably regressed performance (thread-creation overhead exceeding the actual
// work for GROUP BY/ORDER BY benchmarks). This pool exists to match rayon's actual
// behavior: threads are created once (sized to hardware_concurrency()) and reused for
// the lifetime of the process, for every parallel_for() call from any thread.

#include <cstddef>
#include <functional>

namespace engine {

class ThreadPool {
public:
    // Process-wide singleton, lazily created on first use.
    static ThreadPool& global();

    // Calls fn(i) once for each i in [0, count), distributed across the pool's worker
    // threads, and blocks the calling thread until all calls have completed. Safe to
    // call concurrently from multiple threads (e.g. multiple server connections each
    // running a parallel-eligible query at the same time) — each call's tasks and
    // completion tracking are independent of any other concurrent call's.
    // Falls back to calling fn(0) inline when count <= 1 (no pool overhead for one item).
    void parallel_for(std::size_t count, const std::function<void(std::size_t)>& fn);

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

private:
    explicit ThreadPool(std::size_t n_threads);
    ~ThreadPool();

    struct Impl;
    Impl* impl_;
};

} // namespace engine
