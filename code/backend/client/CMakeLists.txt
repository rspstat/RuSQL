# Deliberately does NOT link engine_core: rusql-client's Rust original has zero
# dependency on rusql-core (pure TCP client for the line-based protocol), and this
# port preserves that separation per the migration plan.
add_executable(engine_client src/main.cpp)
target_compile_features(engine_client PRIVATE cxx_std_20)

if(WIN32)
    target_link_libraries(engine_client PRIVATE ws2_32)
endif()
