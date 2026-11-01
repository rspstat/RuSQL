#pragma once

// MAX_EXECUTION_TIME: the time a SELECT may run (SET max_execution_time = ms, or the hint /*+ MAX_EXECUTION_TIME(ms) */). The statement is not interrupted from
// outside: the loops that go through rows (the evaluation of a condition, a join, writing the answer) ask `tick()` as they go, and the stage boundaries of exec_select
// ask `check_now()`; once the time is up they throw the error MySQL gives (3024). The clock is per thread -- the thread that runs the statement -- so the worker
// threads of a parallel stage ask through a `Watch` that the statement's thread hands them (`ParallelStop`), and the statement's thread throws when they are back.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>

#include "engine/statement_error.hpp"

namespace engine::deadline {

inline thread_local bool timed = false;
inline thread_local std::chrono::steady_clock::time_point at;
inline thread_local unsigned ticks = 0;

// Starts the clock for the statement that is run on this thread; the previous state returns when it goes out of scope (statements run inside statements).
struct Scope {
    bool was_timed;
    std::chrono::steady_clock::time_point was_at;
    explicit Scope(std::uint64_t ms) : was_timed(timed), was_at(at) {
        const auto limit = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        if (!timed || limit < at) at = limit;
        timed = true;
    }
    ~Scope() {
        timed = was_timed;
        at = was_at;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

// KILL [QUERY] n: another session sets the kill state of this one (the Executor owns it); the thread that runs the statement looks at it where it can stop
// without leaving anything half done: the waits for a row lock (every statement), and, for a SELECT, the loops over rows (the same places as the clock).
inline thread_local const std::atomic<int>* kill_flag = nullptr;       // the kill state of the session whose statement this thread runs
inline thread_local const std::atomic<int>* kill_read_flag = nullptr;  // ... the same while that statement is a SELECT

inline bool kill_asked(const std::atomic<int>* flag) { return flag && flag->load(std::memory_order_relaxed) != 0; }

// The kill state of the session whose statement runs on this thread until it goes out of scope (statements run inside statements: the previous one returns).
struct KillScope {
    const std::atomic<int>* was_flag;
    const std::atomic<int>* was_read_flag;
    KillScope(const std::atomic<int>* flag, bool reads) : was_flag(kill_flag), was_read_flag(kill_read_flag) {
        kill_flag = flag;
        kill_read_flag = reads ? flag : nullptr;
    }
    ~KillScope() {
        kill_flag = was_flag;
        kill_read_flag = was_read_flag;
    }
    KillScope(const KillScope&) = delete;
    KillScope& operator=(const KillScope&) = delete;
};

// The part of an UPDATE / DELETE that only looks -- finding the rows it will change, before anything is written -- may be stopped by KILL QUERY as a SELECT may: nothing has been
// changed yet. (Once rows are written the statement runs on to its end: stopped half way there is no way back.)
struct ReadPhase {
    const std::atomic<int>* was;
    ReadPhase() : was(kill_read_flag) { kill_read_flag = kill_flag; }
    ~ReadPhase() { kill_read_flag = was; }
    ReadPhase(const ReadPhase&) = delete;
    ReadPhase& operator=(const ReadPhase&) = delete;
};

[[noreturn]] inline void interrupted() {
    throw StatementError("ERROR 1317 (70100): Query execution was interrupted");
}

[[noreturn]] inline void expired() {
    throw StatementError("ERROR 3024 (HY000): Query execution was interrupted, maximum statement execution time exceeded");
}

inline void check_now() {
    if (kill_asked(kill_read_flag)) interrupted();
    if (timed && std::chrono::steady_clock::now() >= at) expired();
}

// The worker threads of a parallel stage have no clock and no kill flag of their own (both belong to the thread that runs the statement). The statement's thread takes a
// `Watch` before it hands the work out, the workers ask `due()` as they go and stop, and the statement's thread, when the workers are back, throws the error with check_now().
struct Watch {
    const std::atomic<int>* kill;
    bool timed;
    std::chrono::steady_clock::time_point at;
    bool due() const { return (kill && kill->load(std::memory_order_relaxed) != 0) || (timed && std::chrono::steady_clock::now() >= at); }
};
inline Watch watch() { return Watch{kill_read_flag, timed, at}; }

// One parallel stage (a probe, a scan in chunks, a sort in slices): the statement's thread makes a `ParallelStop` before it hands the work out, a worker asks `halt(i)` for the row `i` it
// is at (once in 256 rows) or `due()` and returns when it says so, and the statement's thread calls `finish()` when the workers are back -- it throws the error of the stop there, for a
// worker cannot (an exception that leaves a pool thread ends the process). What the workers made before they stopped is not the answer: the statement ends.
struct ParallelStop {
    Watch watch = deadline::watch();
    std::atomic<bool> stopped{false};
    bool due() {
        if (stopped.load(std::memory_order_relaxed) || watch.due()) {
            stopped = true;
            return true;
        }
        return false;
    }
    bool halt(std::size_t i) { return (i & 255u) == 0 && due(); }
    void finish() const {
        if (!stopped) return;
        check_now();
        interrupted();
    }
};

// For a loop that runs for every row: the clock is read every 1024th call.
inline void tick() {
    if (kill_read_flag && kill_read_flag->load(std::memory_order_relaxed) != 0) interrupted();
    if (timed && (++ticks & 1023u) == 0) check_now();
}

// The comparison of a sort on the statement's thread, with the clock and the KILL flag asked once in a thousand calls (a sort of n rows makes n log n of them, and there is no loop
// over rows to ask in). A sort in slices on the pool's threads asks through a ParallelStop instead (parallel_sort).
template <typename Less>
auto checked_less(Less less) {
    return [less, compared = std::size_t{0}](const auto& a, const auto& b) mutable {
        if ((++compared & 1023u) == 0) check_now();
        return less(a, b);
    };
}

} // namespace engine::deadline
