cmake_minimum_required(VERSION 3.20)

# Project name is a placeholder pending the rename decision recorded in the
# migration plan (C:\Users\win11\.claude\plans\rust-drifting-glade.md).
project(rusql_cpp_port LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /W4)
else()
    add_compile_options(-Wall -Wextra)
endif()

add_subdirectory(backend)
