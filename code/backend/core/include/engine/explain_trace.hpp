#pragma once

// The steps of one statement as EXPLAIN ANALYZE and TRACE report them: where the statement was after it was parsed, after its locks were taken, after the rows were
// read and joined, after the sort, after the groups were made, after it was made durable, with the rows each step left and the time it took. exec_select marks the
// end of each of its steps, the statement's path (parse, prepare, execute, commit) marks its own; nothing is recorded unless EXPLAIN ANALYZE or TRACE has put a Trace
// in `active` (on the thread that runs the statement), and only the outermost SELECT of the outermost statement counts (what a subquery or a procedure runs is
// part of the step that ran it).

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
inline thread_local int sql_depth = 0;

// One per exec_select call: how deep the SELECT being run is nested.
struct Scope {
    Scope() { ++select_depth; }
    ~Scope() { --select_depth; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

// One per statement the executor runs as text (execute_sql_inner): how deep the statements nested in each other are.
struct SqlScope {
    SqlScope() { ++sql_depth; }
    ~SqlScope() { --sql_depth; }
    SqlScope(const SqlScope&) = delete;
    SqlScope& operator=(const SqlScope&) = delete;
};

inline void add(const std::string& label, std::size_t rows) {
    const auto now = std::chrono::steady_clock::now();
    active->steps.push_back({label, rows, std::chrono::duration<double, std::milli>(now - active->last).count()});
    active->last = now;
}

// The step of a SELECT that ends here: its label, the rows it left, the time since the previous step ended.
inline void mark(const std::string& label, std::size_t rows) {
    if (!active || select_depth != 1 || sql_depth > 1) return;
    add(label, rows);
}

// The step of the statement's own path that ends here (parse, prepare, execute, commit).
inline void stage(const std::string& label, std::size_t rows = 0) {
    if (!active || sql_depth != 1) return;
    add(label, rows);
}

} // namespace engine::explain_trace
