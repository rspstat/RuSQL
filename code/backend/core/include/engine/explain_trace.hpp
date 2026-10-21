#pragma once

// The steps of one SELECT as EXPLAIN ANALYZE reports them: where the statement was after the rows were read and joined, after the sort, after the groups were made,
// with the rows each step left and the time it took. exec_select marks the end of each step; nothing is recorded unless EXPLAIN ANALYZE has put a Trace in `active`
// (on the thread that runs the statement), and only the outermost SELECT counts (a subquery's own steps are part of the step that ran it).

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace engine::explain_trace {

struct Step {
    std::string label;
    std::size_t rows = 0;
    double ms = 0;
};

struct Trace {
    std::vector<Step> steps;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
};

inline thread_local Trace* active = nullptr;
inline thread_local int select_depth = 0;

// One per exec_select call: how deep the SELECT being run is nested.
struct Scope {
    Scope() { ++select_depth; }
    ~Scope() { --select_depth; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

// The step that ends here: its label, the rows it left, the time since the previous step ended.
inline void mark(const std::string& label, std::size_t rows) {
    if (!active || select_depth != 1) return;
    const auto now = std::chrono::steady_clock::now();
    active->steps.push_back({label, rows, std::chrono::duration<double, std::milli>(now - active->last).count()});
    active->last = now;
}

} // namespace engine::explain_trace
