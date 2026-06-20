# One test executable per test_*.cpp file. Tests are compiled WITH exceptions
# and RTTI (GoogleTest requires them), unlike the strict production units.
include(GoogleTest)

file(GLOB HFT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)

foreach(test_src ${HFT_TEST_SOURCES})
  get_filename_component(test_name ${test_src} NAME_WE)
  add_executable(${test_name} ${test_src})
  target_link_libraries(${test_name} PRIVATE hft_core GTest::gtest GTest::gtest_main)
  gtest_discover_tests(${test_name})
endforeach()
