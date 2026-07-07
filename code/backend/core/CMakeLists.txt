add_library(engine_core STATIC
    src/version.cpp
    src/catalog/schema.cpp
    src/parser/ast.cpp
    src/parser/ast_json.cpp
    src/parser/lexer.cpp
    src/parser/parser_core.cpp
    src/parser/parser_expr.cpp
    src/parser/parser_select.cpp
    src/parser/parser_dml.cpp
    src/parser/parser_ddl.cpp
    src/parser/parser_dcl.cpp
    src/parser/parser_proc.cpp
    src/parser/parser_detail.cpp
    src/storage/page.cpp
    src/storage/btree.cpp
    src/storage/btree_json.cpp
    src/storage/hash_index.cpp
    src/storage/composite_index.cpp
    src/storage/disk.cpp
    src/storage/buffer_pool.cpp
    src/transaction/wal.cpp
    src/transaction/txn_manager.cpp
    src/transaction/group_commit.cpp
    src/engine/planner.cpp
    src/engine/join.cpp
    src/engine/lock_manager.cpp
    src/engine/query_cache.cpp
    src/engine/thread_pool.cpp
    src/engine/executor/executor_core.cpp
    src/engine/executor/executor_qualify.cpp
    src/engine/executor/executor_ddl.cpp
    src/engine/executor/executor_eval.cpp
    src/engine/executor/executor_dml.cpp
    src/engine/executor/executor_update.cpp
    src/engine/executor/executor_delete.cpp
    src/engine/executor/executor_select.cpp
    src/engine/executor/executor_scalar_func.cpp
    src/engine/executor/executor_subquery.cpp
    src/engine/executor/executor_cte.cpp
    src/engine/executor/executor_setops.cpp
    src/engine/executor/executor_window.cpp
    src/engine/executor/executor_txn.cpp
    src/engine/executor/executor_proc.cpp
    src/engine/executor/executor_dcl.cpp
    src/engine/executor/executor_show.cpp
    src/engine/executor/executor_maint.cpp
    src/engine/executor/executor_explain.cpp
    src/engine/executor/executor_backup.cpp
    src/engine/executor/executor_multi.cpp
    src/engine/executor/executor_merge.cpp
    src/engine/executor/executor_infoschema.cpp
    ${CMAKE_SOURCE_DIR}/backend/third_party/lz4/lz4.c
)

target_include_directories(engine_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/backend/third_party
)

target_compile_features(engine_core PUBLIC cxx_std_20)

if(WIN32)
    target_link_libraries(engine_core PUBLIC ws2_32)
endif()
