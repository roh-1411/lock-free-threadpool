cmake_minimum_required(VERSION 3.14)
project(ThreadPoolV4
    VERSION 4.0.0
    DESCRIPTION "Distributed lock-free task server with Prometheus observability"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -Wno-interference-size is only valid on GCC 12+; guard it to avoid
# a hard error on ubuntu-latest (GCC 11) and all Clang versions.
add_compile_options(-Wall -Wextra -Wpedantic)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "12")
    add_compile_options(-Wno-interference-size)
endif()

# ── Sanitizer options ─────────────────────────────────────────
option(ENABLE_SANITIZERS "Enable AddressSanitizer + UBSan" OFF)
option(ENABLE_TSAN       "Enable ThreadSanitizer" OFF)

if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(ENABLE_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

# ── Header-only core library ──────────────────────────────────
add_library(threadpool_core INTERFACE)
target_include_directories(threadpool_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>)
target_compile_features(threadpool_core INTERFACE cxx_std_17)
target_link_libraries(threadpool_core INTERFACE Threads::Threads)

# ── Executables ───────────────────────────────────────────────
add_executable(server    examples/server.cpp)
add_executable(client    examples/client.cpp)
add_executable(demo      examples/demo.cpp)
add_executable(benchmark examples/benchmark.cpp)

foreach(target server client demo benchmark)
    target_link_libraries(${target} PRIVATE threadpool_core)
endforeach()

# ── GoogleTest ────────────────────────────────────────────────
# Try system-installed GTest first (faster CI, no network required).
# Fall back to FetchContent only if not found.
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(STATUS "GTest not found locally — fetching via FetchContent")
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
        DOWNLOAD_EXTRACT_TIMESTAMP true
    )
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
else()
    message(STATUS "Using system GTest: ${GTest_VERSION}")
endif()

enable_testing()
include(GoogleTest)

# ── Test binaries ─────────────────────────────────────────────
add_executable(test_lockfree    tests/test_lockfree_gtest.cpp)
add_executable(test_metrics     tests/test_metrics.cpp)
add_executable(test_protocol    tests/test_protocol.cpp)
add_executable(test_integration tests/test_client_server.cpp)

foreach(target test_lockfree test_metrics test_protocol test_integration)
    target_link_libraries(${target} PRIVATE threadpool_core GTest::gtest_main)
    gtest_discover_tests(${target})
endforeach()

message(STATUS "")
message(STATUS "ThreadPoolV4 ${PROJECT_VERSION} — Distributed Task Server")
message(STATUS "  Build:  mkdir build && cd build && cmake .. && make -j\$(nproc)")
message(STATUS "  Test:   ctest --output-on-failure")
message(STATUS "  Run:    ./server   (then ./client in another terminal)")
message(STATUS "")
