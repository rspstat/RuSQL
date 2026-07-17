cmake_minimum_required(VERSION 3.20)

# Project name is a placeholder pending the rename decision recorded in the
# migration plan (C:\Users\win11\.claude\plans\rust-drifting-glade.md).
project(rusql_cpp_port LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    # /bigobj: executor_core.cpp's std::visit-heavy code (is_pure_read_only's recursive
    # Statement-variant dispatch, among others) exceeds the default COFF object file
    # section limit in Debug builds specifically (Debug doesn't fold/inline as
    # aggressively as Release, generating far more sections per translation unit).
    add_compile_options(/utf-8 /W4 /bigobj)
else()
    add_compile_options(-Wall -Wextra)
endif()

add_subdirectory(backend)
