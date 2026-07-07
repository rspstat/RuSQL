add_executable(engine_server src/main.cpp src/mysql.cpp)
target_link_libraries(engine_server PRIVATE engine_core)
