add_executable(engine_tests
    test_main.cpp
    test_result.cpp
    test_row.cpp
    test_vendored_libs.cpp
    test_ast.cpp
    test_ast_json.cpp
    test_schema.cpp
    test_lexer.cpp
    test_parser.cpp
    test_btree.cpp
    test_storage.cpp
    test_txn.cpp
    test_lock_manager.cpp
    test_query_cache.cpp
    test_join.cpp
    test_planner.cpp
    test_sync.cpp
    test_executor_ddl.cpp
    test_executor_insert.cpp
    test_executor_update_delete.cpp
    test_executor_select.cpp
    test_executor_scalar_func.cpp
    test_executor_subquery.cpp
    test_executor_cte.cpp
    test_executor_setops.cpp
    test_executor_window.cpp
    test_executor_txn.cpp
    test_executor_proc.cpp
    test_executor_dcl.cpp
    test_executor_misc.cpp
    test_server_stmt_split.cpp
    test_concurrency.cpp
)

target_link_libraries(engine_tests PRIVATE engine_core)
target_include_directories(engine_tests PRIVATE ${CMAKE_SOURCE_DIR}/backend/third_party/catch2)
target_compile_definitions(engine_tests PRIVATE
    TEST_FULL_SQL_PATH="${CMAKE_SOURCE_DIR}/test/test_full.sql")

add_test(NAME engine_tests COMMAND engine_tests)
