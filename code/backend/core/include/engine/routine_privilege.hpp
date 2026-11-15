#pragma once

// Who is shown the TEXT of a routine (R84). MySQL shows the rows of SHOW CREATE PROCEDURE / FUNCTION, information_schema.routines and SHOW ... STATUS to an account that may run,
// change or make the routine (Executor::check_show_routine); the text itself -- the "Create Procedure" column, ROUTINE_DEFINITION -- only to the account that made it, to one that
// holds SHOW_ROUTINE and to one that holds SELECT on everything. The others see NULL there. (Measured on MySQL 8.4.9.)

#include <string>

#include "engine/executor/executor.hpp"

namespace engine {

// `kind`: "procedure" or "function"; always true for a session that the privilege tables do not hold
bool routine_text_shown(Executor& ex, const SharedDatabase& s, const std::string& kind, const std::string& name);

} // namespace engine
