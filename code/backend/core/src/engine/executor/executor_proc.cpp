// Faithful port of the stored-procedure/trigger/UDF DDL and the procedure control-flow
// interpreter from rusql-core/src/engine/executor.rs (Phase 8e): exec_create_procedure,
// exec_proc_stmts/exec_proc_if/exec_proc_while/exec_proc_loop/exec_proc_repeat,
// exec_call_procedure, exec_drop_procedure, exec_create_function/exec_drop_function,
// exec_create_trigger/exec_drop_trigger, and exec_execute (PREPARE/EXECUTE).

#include "engine/executor/executor.hpp"

#include "engine/parser/parser.hpp"

namespace engine {

namespace {
// WHILE/LOOP/REPEAT have no natural termination guarantee (unlike recursive CTEs, which
// are already capped at 1000 iterations in executor_cte.cpp) -- a runaway loop in a
// procedure holds SharedDatabase's exclusive shared->write() lock for its entire body
// execution (see Executor::execute()), so it would otherwise freeze the whole server for
// every connected client, forever. Set generously high so no realistic loop hits it.
constexpr std::size_t PROC_LOOP_MAX_ITERATIONS = 100000;
} // namespace

StringResult Executor::exec_create_procedure(SharedDatabase& s, std::string name,
                                              std::vector<std::tuple<std::string, std::string, std::string>> params,
                                              std::vector<Statement> body) {
    s.procedures[name] = ProcedureDef{std::move(params), std::move(body)};
    s.disk.save_procedures(s.procedures);
    return StringResult::Ok("Procedure '" + name + "' created.");
}

StringResult Executor::exec_proc_stmts(SharedDatabase& s, std::vector<Statement> stmts) {
    std::string last;
    for (auto& stmt : stmts) {
        if (proc_signal_) break;
        auto res = execute_with_s(s, std::move(stmt));
        if (res.is_err()) return res;
        last = res.value();
    }
    return StringResult::Ok(last);
}

StringResult Executor::exec_proc_if(SharedDatabase& s, CondExpr condition, std::vector<Statement> then_body,
                                     std::vector<std::pair<CondExpr, std::vector<Statement>>> elseif_branches,
                                     std::optional<std::vector<Statement>> else_body) {
    if (eval_condexpr(proc_vars, condition)) return exec_proc_stmts(s, std::move(then_body));
    for (auto& [cond, body] : elseif_branches) {
        if (eval_condexpr(proc_vars, cond)) return exec_proc_stmts(s, std::move(body));
    }
    if (else_body) return exec_proc_stmts(s, std::move(*else_body));
    return StringResult::Ok("");
}

StringResult Executor::exec_proc_while(SharedDatabase& s, std::optional<std::string> label, CondExpr condition, std::vector<Statement> body) {
    std::string last;
    for (std::size_t iter = 0;; iter++) {
        if (iter >= PROC_LOOP_MAX_ITERATIONS) {
            return StringResult::Err("WHILE loop exceeded maximum iteration count (" + std::to_string(PROC_LOOP_MAX_ITERATIONS) + ")");
        }
        if (!eval_condexpr(proc_vars, condition)) break;
        auto res = exec_proc_stmts(s, body);
        if (res.is_err()) return res;
        last = res.value();
        if (proc_signal_) {
            if (auto* lv = std::get_if<ProcSignal::Leave>(&proc_signal_->data)) {
                if (!lv->label || lv->label == label) {
                    proc_signal_.reset();
                    break;
                }
                break; // propagate to outer loop
            }
            if (auto* it = std::get_if<ProcSignal::Iterate>(&proc_signal_->data)) {
                if (!it->label || it->label == label) {
                    proc_signal_.reset(); // continue
                } else {
                    break; // propagate
                }
            }
        }
    }
    return StringResult::Ok(last);
}

StringResult Executor::exec_proc_loop(SharedDatabase& s, std::optional<std::string> label, std::vector<Statement> body) {
    std::string last;
    for (std::size_t iter = 0;; iter++) {
        if (iter >= PROC_LOOP_MAX_ITERATIONS) {
            return StringResult::Err("LOOP exceeded maximum iteration count (" + std::to_string(PROC_LOOP_MAX_ITERATIONS) + ")");
        }
        auto res = exec_proc_stmts(s, body);
        if (res.is_err()) return res;
        last = res.value();
        bool do_break = false;
        if (proc_signal_) {
            if (auto* lv = std::get_if<ProcSignal::Leave>(&proc_signal_->data)) {
                if (!lv->label || lv->label == label) {
                    proc_signal_.reset();
                    do_break = true;
                } else {
                    do_break = true;
                }
            } else if (auto* it = std::get_if<ProcSignal::Iterate>(&proc_signal_->data)) {
                if (!it->label || it->label == label) {
                    proc_signal_.reset(); // continue loop
                } else {
                    do_break = true;
                }
            }
        }
        if (do_break) break;
    }
    return StringResult::Ok(last);
}

StringResult Executor::exec_proc_repeat(SharedDatabase& s, std::optional<std::string> label, std::vector<Statement> body, CondExpr until) {
    std::string last;
    for (std::size_t iter = 0;; iter++) {
        if (iter >= PROC_LOOP_MAX_ITERATIONS) {
            return StringResult::Err("REPEAT loop exceeded maximum iteration count (" + std::to_string(PROC_LOOP_MAX_ITERATIONS) + ")");
        }
        auto res = exec_proc_stmts(s, body);
        if (res.is_err()) return res;
        last = res.value();
        bool do_break = false;
        if (proc_signal_) {
            if (auto* lv = std::get_if<ProcSignal::Leave>(&proc_signal_->data)) {
                if (!lv->label || lv->label == label) {
                    proc_signal_.reset();
                    do_break = true;
                } else {
                    do_break = true;
                }
            } else if (auto* it = std::get_if<ProcSignal::Iterate>(&proc_signal_->data)) {
                if (!it->label || it->label == label) {
                    proc_signal_.reset();
                    // fall through to UNTIL check
                } else {
                    do_break = true;
                }
            }
        }
        if (do_break) break;
        if (eval_condexpr(proc_vars, until)) break;
    }
    return StringResult::Ok(last);
}

StringResult Executor::exec_call_procedure(SharedDatabase& s, std::string name, std::vector<std::string> args) {
    auto it = s.procedures.find(name);
    if (it == s.procedures.end()) return StringResult::Err("Procedure '" + name + "' not found");
    auto [params, body] = it->second;

    auto saved_vars = std::move(proc_vars);
    proc_vars.clear();
    for (std::size_t i = 0; i < params.size(); i++) {
        auto& [dir, pname, ptype] = params[i];
        (void)ptype;
        if (dir == "IN" || dir == "INOUT") {
            proc_vars[pname] = i < args.size() ? args[i] : std::string();
        }
    }

    // Matches the Rust original's `?` early-return: on error, proc_vars/proc_signal_
    // are deliberately NOT restored here (the caller's scope is left clobbered by the
    // callee's), since that's what the `?` operator does when it propagates before
    // reaching the `self.proc_vars = saved_vars;` line below it.
    auto res = exec_proc_stmts(s, std::move(body));
    if (res.is_err()) return res;
    proc_vars = std::move(saved_vars);
    proc_signal_.reset(); // clear any signal that escaped the body

    std::string last = res.value();
    return StringResult::Ok(last.empty() ? "Procedure '" + name + "' executed." : last);
}

StringResult Executor::exec_drop_procedure(SharedDatabase& s, std::string name, bool if_exists) {
    if (s.procedures.erase(name) == 0 && !if_exists) return StringResult::Err("Procedure '" + name + "' does not exist");
    s.disk.save_procedures(s.procedures);
    return StringResult::Ok("Procedure '" + name + "' dropped.");
}

StringResult Executor::exec_create_function(SharedDatabase& s, std::string name, std::vector<std::string> params, std::string body) {
    s.user_functions[name] = UserFunctionDef{std::move(params), std::move(body)};
    s.disk.save_functions(s.user_functions);
    return StringResult::Ok("Function '" + name + "' created.");
}

StringResult Executor::exec_drop_function(SharedDatabase& s, std::string name, bool if_exists) {
    if (s.user_functions.erase(name) == 0 && !if_exists) return StringResult::Err("Function '" + name + "' does not exist");
    s.disk.save_functions(s.user_functions);
    return StringResult::Ok("Function '" + name + "' dropped.");
}

StringResult Executor::exec_create_trigger(SharedDatabase& s, std::string name, TriggerTiming timing, TriggerEvent event, std::string table,
                                            std::vector<Statement> body) {
    std::string timing_str = timing == TriggerTiming::Before ? "BEFORE" : "AFTER";
    std::string event_str = event == TriggerEvent::Insert ? "INSERT" : (event == TriggerEvent::Update ? "UPDATE" : "DELETE");
    s.triggers[name] = TriggerDef{table, timing_str, event_str, std::move(body)};
    s.disk.save_triggers(s.triggers);
    return StringResult::Ok("Trigger '" + name + "' created.");
}

StringResult Executor::exec_drop_trigger(SharedDatabase& s, std::string name, bool if_exists) {
    if (s.triggers.erase(name) == 0 && !if_exists) return StringResult::Err("Trigger '" + name + "' does not exist");
    s.disk.save_triggers(s.triggers);
    return StringResult::Ok("Trigger '" + name + "' dropped.");
}

StringResult Executor::exec_execute(SharedDatabase& s, const std::string& name, const std::vector<std::string>& using_vars) {
    auto it = prepared_stmts.find(name);
    if (it == prepared_stmts.end()) return StringResult::Err("Unknown prepared statement: " + name);
    std::string query = it->second;

    for (auto& var : using_vars) {
        auto vit = user_vars.find(var);
        std::string val = vit != user_vars.end() ? vit->second : "NULL";
        auto pos = query.find('?');
        if (pos != std::string::npos) query.replace(pos, 1, val);
    }

    Parser parser(query);
    auto parsed = parser.parse();
    if (parsed.is_err()) return StringResult::Err("EXECUTE parse error: " + parsed.error());
    return execute_with_s(s, std::move(parsed).value());
}

} // namespace engine
