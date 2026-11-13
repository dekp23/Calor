cmake_minimum_required(VERSION 3.16)
project(calor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

# Header-only core: segment tree, trie, nutrient list, dates.
add_library(calor_core INTERFACE)
target_include_directories(calor_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

enable_testing()
add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE calor_core)
add_test(NAME core COMMAND test_core)

# Engine: SQLite storage + tracker.
add_library(calor_engine STATIC src/storage.cpp src/tracker.cpp)
target_link_libraries(calor_engine PUBLIC calor_core SQLite::SQLite3 Threads::Threads)

add_executable(test_tracker tests/test_tracker.cpp)
target_link_libraries(test_tracker PRIVATE calor_engine)
add_test(NAME tracker COMMAND test_tracker)

# HTTP server (cpp-httplib, vendored single header, MIT).
add_executable(calor src/main.cpp)
target_include_directories(calor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(calor PRIVATE calor_engine)

# Benchmark: linear scan vs indexed SQLite vs segment tree.
add_executable(benchmark bench/benchmark.cpp)
target_link_libraries(benchmark PRIVATE calor_core SQLite::SQLite3)
