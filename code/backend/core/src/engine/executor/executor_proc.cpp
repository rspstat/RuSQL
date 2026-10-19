// Faithful port of the stored-procedure/trigger/UDF DDL and the procedure control-flow
// interpreter from rusql-core/src/engine/executor.rs (Phase 8e): exec_create_procedure,
// exec_proc_stmts/exec_proc_if/exec_proc_while/exec_proc_loop/exec_proc_repeat,
// exec_call_procedure, exec_drop_procedure, exec_create_function/exec_drop_function,
// exec_create_trigger/exec_drop_trigger, and exec_execute (PREPARE/EXECUTE).

#include "engine/executor/executor.hpp"

#include <cctype>

#include "engine/parser/ast_json.hpp"
#include "engine/parser/parser.hpp"

namespace engine {

namespace {
std::string to_lower_str(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// WHILE/LOOP/REPEAT have no natural termination guarantee (unlike recursive CTEs, which
// are already capped at 1000 iterations in executor_cte.cpp) -- a runaway loop in a
// procedure holds SharedDatabase's exclusive shared->write() lock for its entire body
// execution (see Executor::execute()), so it would otherwise freeze the whole server for
// every connected client, forever. Set generously high so no realistic loop hits it.
constexpr std::size_t PROC_LOOP_MAX_ITERATIONS = 100000;

// The procedure, trigger or function called `name` -- its name does not depend on case (MySQL), so CALL raise finds Raise: the exact spelling first,
// else the one that differs from it in case only.
template <typename Map>
typename Map::iterator find_named(Map& named, const std::string& name) {
    if (auto it = named.find(name); it != named.end()) return it;
    const std::string wanted = to_lower_str(name);
    return std::find_if(named.begin(), named.end(), [&](const auto& entry) { return to_lower_str(entry.first) == wanted; });
}
} // namespace

StringResult Executor::exec_create_procedure(SharedDatabase& s, std::string name,
                                              std::vector<std::tuple<std::string, std::string, std::string>> params,
                                              std::vector<Statement> body) {
    if (auto same = find_named(s.procedures, name); same != s.procedures.end()) s.procedures.erase(same); // (CREATE OR REPLACE keeps one)
    s.procedures[name] = ProcedureDef{std::move(params), std::move(body)};
    s.disk.save_procedures(s.procedures);
    return StringResult::Ok("Procedure '" + name + "' created.");
}

StringResult Executor::exec_proc_stmts(SharedDatabase& s, std::vector<Statement> stmts) {
    std::string last;
    for (auto& stmt : stmts) {
        if (proc_signal_) break;
        resolve_names(s, stmt); // (the tables and columns a body names are read in the spelling they were created with)
        // an INSERT in the body numbers its rows like any other: LAST_INSERT_ID() says the first number (CALL, the statement that runs, is not an INSERT)
        if (std::holds_alternative<Statement::Insert>(stmt.data) || std::holds_alternative<Statement::InsertSelect>(stmt.data)) insert_id_unset_ = true;
        auto res = execute_with_s(s, std::move(stmt));
        if (res.is_err()) return res;
        // what the call answers is the last result table of the body (a SELECT before a SET still answers), else the last message
        if (res.value().rfind('+', 0) == 0 || last.rfind('+', 0) != 0) last = res.value();
    }
    return StringResult::Ok(last);
}

StringResult Executor::exec_proc_if(SharedDatabase& s, CondExpr condition, std::vector<Statement> then_body,
                                     std::vector<std::pair<CondExpr, std::vector<Statement>>> elseif_branches,
                                     std::optional<std::vector<Statement>> else_body) {
    if (proc_condition(condition)) return exec_proc_stmts(s, std::move(then_body));
    for (auto& [cond, body] : elseif_branches) {
        if (proc_condition(cond)) return exec_proc_stmts(s, std::move(body));
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
        if (!proc_condition(condition)) break;
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
        if (proc_condition(until)) break;
    }
    return StringResult::Ok(last);
}

StringResult Executor::exec_call_procedure(SharedDatabase& s, std::string name, std::vector<std::string> args) {
    auto it = find_named(s.procedures, name);
    if (it == s.procedures.end()) return StringResult::Err("Procedure '" + name + "' not found");
    auto [params, body] = it->second;
    if (args.size() != params.size()) {
        return StringResult::Err("Incorrect number of arguments for PROCEDURE " + current_db + "." + name + "; expected " + std::to_string(params.size()) + ", got " +
                                 std::to_string(args.size()));
    }

    // The value of each argument: text as typed (quotes taken off), an @variable's value, or an expression (kept as "\x01" + JSON by the
    // parser) evaluated with the variables of the caller.
    Row caller_vars = proc_vars;
    for (auto& [k, val] : user_vars) caller_vars["@" + k] = val;
    std::vector<std::string> arg_values;
    for (auto& arg : args) {
        if (!arg.empty() && arg[0] == '\x01') {
            ArithExpr expr = nlohmann::json::parse(arg.substr(1)).get<ArithExpr>();
            bind_expression(s, expr, false);
            arg_values.push_back(eval_arith(caller_vars, expr));
        } else if (!arg.empty() && arg[0] == '@') {
            auto it = user_vars.find(to_lower_str(arg.substr(1)));
            arg_values.push_back(it != user_vars.end() ? it->second : EXECUTOR_NULL_VALUE);
        } else if (arg.size() >= 2 && arg.front() == '\'' && arg.back() == '\'') {
            std::string unquoted = arg.substr(1, arg.size() - 2);
            for (std::size_t p = unquoted.find("''"); p != std::string::npos; p = unquoted.find("''", p + 1)) unquoted.erase(p, 1);
            arg_values.push_back(std::move(unquoted));
        } else {
            arg_values.push_back(arg);
        }
    }

    // the value given to a parameter is made to fit its type (DECIMAL(8,2): 1.005 is 1.01; INT: '7.6' is 8; a value that does not fit is an error)
    for (std::size_t i = 0; i < params.size(); i++) {
        auto& [dir, pname, ptype] = params[i];
        if (dir == "OUT") continue;
        if (auto error = coerce_declared(pname, ptype, arg_values[i])) return StringResult::Err(*error);
    }

    auto saved_vars = std::move(proc_vars);
    auto saved_types = std::move(proc_var_types);
    proc_vars.clear();
    proc_var_types.clear();
    for (std::size_t i = 0; i < params.size(); i++) {
        auto& [dir, pname, ptype] = params[i];
        proc_var_types[pname] = ptype;
        if (dir == "IN" || dir == "INOUT") {
            proc_vars[pname] = i < arg_values.size() ? arg_values[i] : std::string();
        } else {
            proc_vars[pname] = EXECUTOR_NULL_VALUE; // an OUT parameter starts as NULL
        }
    }

    // On an error the caller's variables come back too: the Rust original's `?` left the callee's in place, where a later statement read them in place of
    // the columns of the same names (after a failed CALL p(12.5), `SELECT a FROM t` was 12.50 for every row)
    auto res = exec_proc_stmts(s, std::move(body));
    if (res.is_err()) {
        proc_vars = std::move(saved_vars);
        proc_var_types = std::move(saved_types);
        proc_signal_.reset();
        return res;
    }
    // OUT and INOUT parameters called with an @variable give it their value
    for (std::size_t i = 0; i < params.size() && i < args.size(); i++) {
        auto& [dir, pname, ptype] = params[i];
        (void)ptype;
        if ((dir == "OUT" || dir == "INOUT") && !args[i].empty() && args[i][0] == '@') {
            if (auto it = proc_vars.find(pname); it != proc_vars.end()) user_vars[to_lower_str(args[i].substr(1))] = it->second;
        }
    }
    proc_vars = std::move(saved_vars);
    proc_var_types = std::move(saved_types);
    proc_signal_.reset(); // clear any signal that escaped the body

    std::string last = res.value();
    return StringResult::Ok(last.empty() ? "Procedure '" + name + "' executed." : last);
}

StringResult Executor::exec_drop_procedure(SharedDatabase& s, std::string name, bool if_exists) {
    auto it = find_named(s.procedures, name);
    if (it == s.procedures.end()) {
        if (!if_exists) return StringResult::Err("Procedure '" + name + "' does not exist");
    } else {
        s.procedures.erase(it);
    }
    s.disk.save_procedures(s.procedures);
    return StringResult::Ok("Procedure '" + name + "' dropped.");
}

StringResult Executor::exec_create_function(SharedDatabase& s, std::string name, std::vector<std::string> params, std::string body,
                                            const std::vector<std::string>& param_types, const std::string& return_type) {
    // with the types of the parameters and of the result (the values are made to fit them when it is called), the body is kept in a frame that holds them
    const bool typed = !return_type.empty() || std::any_of(param_types.begin(), param_types.end(), [](const std::string& t) { return !t.empty(); });
    if (typed) {
        body = nlohmann::json{{"routine_expr", nlohmann::json::parse(body)}, {"param_types", param_types}, {"return_type", return_type}}.dump();
    }
    if (auto same = find_named(s.user_functions, name); same != s.user_functions.end()) s.user_functions.erase(same);
    s.user_functions[name] = UserFunctionDef{std::move(params), std::move(body)};
    s.disk.save_functions(s.user_functions);
    return StringResult::Ok("Function '" + name + "' created.");
}

StringResult Executor::exec_drop_function(SharedDatabase& s, std::string name, bool if_exists) {
    auto it = find_named(s.user_functions, name);
    if (it == s.user_functions.end()) {
        if (!if_exists) return StringResult::Err("Function '" + name + "' does not exist");
    } else {
        s.user_functions.erase(it);
    }
    s.disk.save_functions(s.user_functions);
    return StringResult::Ok("Function '" + name + "' dropped.");
}

StringResult Executor::exec_create_trigger(SharedDatabase& s, std::string name, TriggerTiming timing, TriggerEvent event, std::string table,
                                            std::vector<Statement> body) {
    std::string timing_str = timing == TriggerTiming::Before ? "BEFORE" : "AFTER";
    std::string event_str = event == TriggerEvent::Insert ? "INSERT" : (event == TriggerEvent::Update ? "UPDATE" : "DELETE");
    if (auto same = find_named(s.triggers, name); same != s.triggers.end()) s.triggers.erase(same);
    s.triggers[name] = TriggerDef{table, timing_str, event_str, std::move(body)};
    s.disk.save_triggers(s.triggers);
    return StringResult::Ok("Trigger '" + name + "' created.");
}

StringResult Executor::exec_drop_trigger(SharedDatabase& s, std::string name, bool if_exists) {
    auto it = find_named(s.triggers, name);
    if (it == s.triggers.end()) {
        if (!if_exists) return StringResult::Err("Trigger '" + name + "' does not exist");
    } else {
        s.triggers.erase(it);
    }
    s.disk.save_triggers(s.triggers);
    return StringResult::Ok("Trigger '" + name + "' dropped.");
}

StringResult Executor::exec_execute(SharedDatabase& s, const std::string& name, const std::vector<std::string>& using_vars) {
    auto it = prepared_stmts.find(name);
    if (it == prepared_stmts.end()) return StringResult::Err("Unknown prepared statement: " + name);
    const std::string& prepared = it->second;

    // Each ? outside a quoted string or a quoted name is replaced by the value of the next variable of USING, written as a literal: a number as it is, NULL, any
    // other text in quotes with its quotes and backslashes doubled (a value that holds a ? or a quote is a value, not part of the statement)
    std::string query;
    std::size_t used = 0;
    for (std::size_t i = 0; i < prepared.size(); i++) {
        const char c = prepared[i];
        if (c == '\'' || c == '"' || c == '`') {
            const std::size_t start = i;
            for (i++; i < prepared.size(); i++) {
                if (prepared[i] == '\\' && c != '`' && i + 1 < prepared.size()) { i++; continue; }
                if (prepared[i] == c) {
                    if (i + 1 < prepared.size() && prepared[i + 1] == c) { i++; continue; } // (a doubled quote)
                    break;
                }
            }
            query.append(prepared, start, i - start + 1);
            continue;
        }
        if (c != '?') {
            query.push_back(c);
            continue;
        }
        if (used >= using_vars.size()) return StringResult::Err("Incorrect arguments to EXECUTE");
        auto vit = user_vars.find(to_lower_str(using_vars[used++]));
        const std::string value = vit != user_vars.end() ? vit->second : "NULL";
        bool number = !value.empty();
        std::size_t digits = 0;
        for (std::size_t k = 0; k < value.size() && number; k++) {
            const unsigned char d = static_cast<unsigned char>(value[k]);
            if (std::isdigit(d)) digits++;
            else if (!((d == '-' || d == '+') && k == 0) && !(d == '.' && k > 0)) number = false;
        }
        if (value == "NULL" || (number && digits > 0)) {
            query += value;
        } else {
            query.push_back('\'');
            for (char ch : value) {
                if (ch == '\'' || ch == '\\') query.push_back(ch);
                query.push_back(ch);
            }
            query.push_back('\'');
        }
    }
    if (used != using_vars.size()) return StringResult::Err("Incorrect arguments to EXECUTE");

    Parser parser(query, pipes_as_concat());
    auto parsed = parser.parse();
    if (parsed.is_err()) return StringResult::Err("EXECUTE parse error: " + parsed.error());
    std::string word;
    for (std::size_t i = query.find_first_not_of(" \t\r\n"); i != std::string::npos && i < query.size() && std::isalpha(static_cast<unsigned char>(query[i])); i++) {
        word.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(query[i]))));
    }
    insert_id_unset_ = word == "INSERT" || word == "REPLACE"; // (an INSERT that is prepared numbers its rows like any other: LAST_INSERT_ID() says the first number)
    return execute_with_s(s, std::move(parsed).value());
}

} // namespace engine
