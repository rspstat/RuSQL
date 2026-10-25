#pragma once

// MAX_EXECUTION_TIME: the time a SELECT may run (SET max_execution_time = ms, or the hint /*+ MAX_EXECUTION_TIME(ms) */). The statement is not interrupted from
// outside: the loops that go through rows (the evaluation of a condition, a join, writing the answer) ask `tick()` as they go, and the stage boundaries of exec_select
// ask `check_now()`; once the time is up they throw the error MySQL gives (3024). The clock is per thread -- the thread that runs the statement -- so the worker
// threads of a parallel scan do not ask, and such a scan is stopped at the next step of the thread that started it.

#include <atomic>
#include <chrono>
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

// For a loop that runs for every row: the clock is read every 1024th call.
inline void tick() {
    if (kill_read_flag && kill_read_flag->load(std::memory_order_relaxed) != 0) interrupted();
    if (timed && (++ticks & 1023u) == 0) check_now();
}

} // namespace engine::deadline
