# CMakeLists.txt — Lock-Free Thread Pool v3
# ==========================================
#
# CMake is the industry-standard C++ build system.
# It generates platform-specific build files (Makefiles on Linux,
# Xcode projects on macOS, VS solutions on Windows) from this
# single cross-platform description.
#
# WHY CMAKE OVER MAKEFILE?
# -------------------------
# Our Makefile was fine for a small project. CMake adds:
#   - Automatic dependency tracking (no manual -I flags everywhere)
#   - FetchContent: download+build GoogleTest automatically
#   - CTest: integrated test runner (ctest -V)
#   - Install targets, packaging, cross-compilation
#   - IDE integration (CLion, VS Code, CLangd, etc.)
#
# This is what Google, Meta, and every serious C++ project uses.
#
# BUILD INSTRUCTIONS:
# -------------------
#   mkdir build && cd build
#   cmake .. -DCMAKE_BUILD_TYPE=Release
#   make -j$(nproc)
#   ctest --output-on-failure
#
# Or with Debug + sanitizers:
#   cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
#   make -j$(nproc)
#   ctest --output-on-failure

cmake_minimum_required(VERSION 3.14)
project(ThreadPoolV3
    VERSION 3.0.0
    DESCRIPTION "Lock-free thread pool with Prometheus observability"
    LANGUAGES CXX
)

# ─────────────────────────────────────────────────────────────
# Compiler settings
# ─────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)  # Use -std=c++17 not -std=gnu++17

# Warning flags — strict, like a production codebase
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wnon-virtual-dtor
    -Woverloaded-virtual
)

# ─────────────────────────────────────────────────────────────
# Optional sanitizers (enable with -DENABLE_SANITIZERS=ON)
# ThreadSanitizer catches data races in lock-free code.
# AddressSanitizer catches memory errors.
# ─────────────────────────────────────────────────────────────
option(ENABLE_SANITIZERS "Enable ThreadSanitizer + AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer only" OFF)

if(ENABLE_SANITIZERS)
    message(STATUS "Sanitizers: AddressSanitizer + UBSan enabled")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
elseif(ENABLE_TSAN)
    message(STATUS "Sanitizers: ThreadSanitizer enabled")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

# ─────────────────────────────────────────────────────────────
# Interface library — header-only core
# Using INTERFACE means dependents get include path automatically
# No compilation needed for header-only libraries.
# ─────────────────────────────────────────────────────────────
add_library(threadpool_core INTERFACE)
target_include_directories(threadpool_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
)
target_compile_features(threadpool_core INTERFACE cxx_std_17)
target_link_libraries(threadpool_core INTERFACE
    Threads::Threads
)

find_package(Threads REQUIRED)

# ─────────────────────────────────────────────────────────────
# Examples
# ─────────────────────────────────────────────────────────────
add_executable(demo examples/demo.cpp)
target_link_libraries(demo PRIVATE threadpool_core)

add_executable(benchmark examples/benchmark.cpp)
target_link_libraries(benchmark PRIVATE threadpool_core)

# ─────────────────────────────────────────────────────────────
# GoogleTest via FetchContent
# ─────────────────────────────────────────────────────────────
# FetchContent downloads and builds GTest at configure time.
# No system-wide installation needed.
# This is the modern CMake way to manage test dependencies.
include(FetchContent)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP true
)

# Prevent GTest from installing itself into the system
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# ─────────────────────────────────────────────────────────────
# Tests
# ─────────────────────────────────────────────────────────────
enable_testing()
include(GoogleTest)

# Test binary 1: metrics + instrumented pool
add_executable(test_metrics tests/test_metrics.cpp)
target_link_libraries(test_metrics PRIVATE
    threadpool_core
    GTest::gtest_main
)
gtest_discover_tests(test_metrics)

# Test binary 2: lock-free queue + threadpool v2
add_executable(test_lockfree tests/test_lockfree_gtest.cpp)
target_link_libraries(test_lockfree PRIVATE
    threadpool_core
    GTest::gtest_main
)
gtest_discover_tests(test_lockfree)

# ─────────────────────────────────────────────────────────────
# Print summary at configure time
# ─────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "ThreadPoolV3 ${PROJECT_VERSION}")
message(STATUS "  Build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Sanitizers:  ${ENABLE_SANITIZERS}")
message(STATUS "  TSan:        ${ENABLE_TSAN}")
message(STATUS "")
message(STATUS "Build:")
message(STATUS "  mkdir build && cd build")
message(STATUS "  cmake .. -DCMAKE_BUILD_TYPE=Release")
message(STATUS "  make -j\$(nproc)")
message(STATUS "  ctest --output-on-failure")
message(STATUS "")
