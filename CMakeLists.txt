cmake_minimum_required(VERSION 3.26)
project(nse_hft_system LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Standard / global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Options.
option(HFT_BUILD_TESTS "Build unit tests" ON)
option(HFT_BUILD_BENCHMARKS "Build benchmarks" ON)
option(HFT_BUILD_TOOLS "Build CLI tools" ON)
option(HFT_NATIVE_ARCH "Optimise for the local CPU (-march=native)" ON)
option(HFT_ENABLE_ASAN "Build with AddressSanitizer" OFF)
option(HFT_ENABLE_TSAN "Build with ThreadSanitizer" OFF)
option(HFT_ENABLE_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)

# ---------------------------------------------------------------------------
# Optimisation / warning flags shared by all targets.
# Hot-path translation units additionally get -fno-exceptions / -fno-rtti via
# the hft_core_strict interface target (tests need exceptions for gtest).
# ---------------------------------------------------------------------------
add_library(hft_flags INTERFACE)
target_include_directories(hft_flags INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(hft_flags INTERFACE
  -Wall -Wextra -Wpedantic
  $<$<CONFIG:Release>:-O3>
  $<$<CONFIG:Release>:-funroll-loops>)

if(HFT_NATIVE_ARCH)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-march=native" HFT_HAS_MARCH_NATIVE)
  if(HFT_HAS_MARCH_NATIVE)
    target_compile_options(hft_flags INTERFACE -march=native)
  endif()
endif()

# Sanitizers (mutually composable except ASan+TSan).
set(HFT_SANITIZER_FLAGS "")
if(HFT_ENABLE_ASAN)
  list(APPEND HFT_SANITIZER_FLAGS -fsanitize=address -fno-omit-frame-pointer)
endif()
if(HFT_ENABLE_TSAN)
  list(APPEND HFT_SANITIZER_FLAGS -fsanitize=thread)
endif()
if(HFT_ENABLE_UBSAN)
  list(APPEND HFT_SANITIZER_FLAGS -fsanitize=undefined -fno-omit-frame-pointer)
endif()
if(HFT_SANITIZER_FLAGS)
  target_compile_options(hft_flags INTERFACE ${HFT_SANITIZER_FLAGS})
  target_link_options(hft_flags INTERFACE ${HFT_SANITIZER_FLAGS})
endif()

# Strict flags for production (non-test) translation units only.
add_library(hft_strict INTERFACE)
target_compile_options(hft_strict INTERFACE
  $<$<CONFIG:Release>:-fno-exceptions>
  $<$<CONFIG:Release>:-fno-rtti>)

find_package(Threads REQUIRED)

# numa / rt are Linux-only and may be absent on dev machines; link if present.
find_library(NUMA_LIB numa)
find_library(RT_LIB rt)

# ---------------------------------------------------------------------------
# Core library (header-heavy; a handful of .cpp implementation units).
# ---------------------------------------------------------------------------
file(GLOB_RECURSE HFT_CORE_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/src/common/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/feed_handler/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/order_book/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/strategy/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/risk/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/order_manager/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/gateway/*.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/session/*.cpp)

add_library(hft_core STATIC ${HFT_CORE_SOURCES})
target_link_libraries(hft_core PUBLIC hft_flags Threads::Threads)
target_link_libraries(hft_core PRIVATE hft_strict)
if(NUMA_LIB)
  target_link_libraries(hft_core PUBLIC ${NUMA_LIB})
endif()
if(RT_LIB)
  target_link_libraries(hft_core PUBLIC ${RT_LIB})
endif()

# ---------------------------------------------------------------------------
# Main executable.
# ---------------------------------------------------------------------------
add_executable(nse_hft src/main.cpp)
target_link_libraries(nse_hft PRIVATE hft_core hft_strict)

# ---------------------------------------------------------------------------
# Third-party (GoogleTest + Google Benchmark) via FetchContent.
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

if(HFT_BUILD_TESTS)
  enable_testing()
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  add_subdirectory(tests)
endif()

if(HFT_BUILD_BENCHMARKS)
  FetchContent_Declare(benchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG v1.9.0)
  set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(benchmark)
  add_subdirectory(benchmarks)
endif()

if(HFT_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

# iRage ConnectLib strategy adapter (built against the local mock; C++17).
add_subdirectory(irage)
