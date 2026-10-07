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
    test_gap_lock.cpp
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
    test_partitioning.cpp
    test_redo.cpp
    test_update_fast.cpp
    test_dml_index.cpp
    test_select_index.cpp
    test_row_json.cpp
    test_bulk_write.cpp
    test_query_paths.cpp
    test_null_expressions.cpp
    test_write_integrity.cpp
    test_join_names.cpp
    test_unknown_columns.cpp
    test_stored_routines.cpp
    test_aggregate_semantics.cpp
    test_typed_comparison.cpp
    test_aggregate_arguments.cpp
    test_subquery_cardinality.cpp
    test_value_expressions.cpp
    test_sort_group.cpp
    test_outer_references.cpp
    test_merge_conditions.cpp
    test_keyword_names.cpp
    test_group_by_rules.cpp
)

target_link_libraries(engine_tests PRIVATE engine_core)
target_include_directories(engine_tests PRIVATE ${CMAKE_SOURCE_DIR}/backend/third_party/catch2)
target_compile_definitions(engine_tests PRIVATE
    TEST_FULL_SQL_PATH="${CMAKE_SOURCE_DIR}/test/test_full.sql")

add_test(NAME engine_tests COMMAND engine_tests)
