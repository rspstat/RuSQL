#pragma once

// Faithful port of rusql-server/src/mysql.rs — the MySQL wire protocol (text
// protocol, MySQL 4.1+) listener. Auth is verified via mysql_native_password.

#include <cstdint>
#include <memory>

#include "engine/executor/executor.hpp"

void start_mysql_listener(int port, std::shared_ptr<engine::RwLock<engine::SharedDatabase>> shared);
