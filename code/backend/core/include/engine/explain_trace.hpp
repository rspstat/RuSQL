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

// What the statement spent on its subqueries: runs and time. The time is inside the steps (the scan that asked, the select list that asked), so it is shown next
// to them and is not a step of its own.
struct SubqueryTime {
    std::size_t runs = 0;
    double ms = 0;
};

struct Trace {
    std::vector<Step> steps;
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    SubqueryTime subquery_once;    // subqueries that name no column of the outer query: one run answers every row
    SubqueryTime subquery_per_row; // correlated ones: a run for every row
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

inline thread_local int subquery_depth = 0;

// One per subquery that runs (Executor::run_subquery): only the outermost counts, a subquery inside a subquery is part of the time of the one that ran it.
struct SubqueryScope {
    bool counted;
    bool correlated;
    std::chrono::steady_clock::time_point started{};
    explicit SubqueryScope(bool is_correlated) : counted(active && select_depth >= 1 && sql_depth <= 1 && subquery_depth == 0), correlated(is_correlated) {
        if (counted) started = std::chrono::steady_clock::now();
        ++subquery_depth;
    }
    ~SubqueryScope() {
        --subquery_depth;
        if (!counted || !active) return;
        SubqueryTime& t = correlated ? active->subquery_per_row : active->subquery_once;
        t.runs++;
        t.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    }
    SubqueryScope(const SubqueryScope&) = delete;
    SubqueryScope& operator=(const SubqueryScope&) = delete;
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
