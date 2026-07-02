# Local build of the iRage strategy adapter against the mock ConnectHeader.h.
# Deliberately compiled as C++17: the iRage production toolchain builds
# strategies with -std=c++1z, so this target proves the code stays compatible.
# (Production builds on the iRage box use irage/Makefile with their real lib.)

add_library(irage_strat STATIC
  mystrat.cpp
  mock/connect_mock.cpp)
target_include_directories(irage_strat PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}          # mystrat.h
  ${CMAKE_CURRENT_SOURCE_DIR}/mock     # mock ConnectHeader.h
  ${CMAKE_CURRENT_SOURCE_DIR}/../src)  # strategy/quoting_math.hpp
set_target_properties(irage_strat PROPERTIES
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF)
target_compile_options(irage_strat PRIVATE -Wall -Wextra)
find_package(Threads REQUIRED)
target_link_libraries(irage_strat PUBLIC Threads::Threads)

if(HFT_BUILD_TESTS)
  add_executable(test_irage_strat test_irage_strat.cpp)
  target_link_libraries(test_irage_strat PRIVATE irage_strat GTest::gtest GTest::gtest_main)
  set_target_properties(test_irage_strat PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)
  include(GoogleTest)
  gtest_discover_tests(test_irage_strat)
endif()
