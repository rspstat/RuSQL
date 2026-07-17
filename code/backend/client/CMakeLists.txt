# Deliberately does NOT link engine_core: rusql-client's Rust original has zero
# dependency on rusql-core (pure TCP client for the line-based protocol), and this
# port preserves that separation per the migration plan. It does use the vendored
# header-only SHA1 utility directly (third_party/sha1, no .cpp to compile/link) for
# the native-protocol challenge-response AUTH handshake -- that's a shared crypto
# primitive, not a dependency on the engine itself.
add_executable(engine_client src/main.cpp)
target_compile_features(engine_client PRIVATE cxx_std_20)
target_include_directories(engine_client PRIVATE ${CMAKE_SOURCE_DIR}/backend/third_party)

if(WIN32)
    target_link_libraries(engine_client PRIVATE ws2_32)
endif()
