#pragma once

// MAX_EXECUTION_TIME: the time a SELECT may run (SET max_execution_time = ms, or the hint /*+ MAX_EXECUTION_TIME(ms) */). The statement is not interrupted from
// outside: the loops that go through rows (the evaluation of a condition, a join, writing the answer) ask `tick()` as they go, and the stage boundaries of exec_select
// ask `check_now()`; once the time is up they throw the error MySQL gives (3024). The clock is per thread -- the thread that runs the statement -- so the worker
// threads of a parallel scan do not ask, and such a scan is stopped at the next step of the thread that started it.

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

[[noreturn]] inline void expired() {
    throw StatementError("ERROR 3024 (HY000): Query execution was interrupted, maximum statement execution time exceeded");
}

inline void check_now() {
    if (timed && std::chrono::steady_clock::now() >= at) expired();
}

// For a loop that runs for every row: the clock is read every 1024th call.
inline void tick() {
    if (timed && (++ticks & 1023u) == 0) check_now();
}

} // namespace engine::deadline
