# One benchmark executable per bench_*.cpp file.
file(GLOB HFT_BENCH_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/bench_*.cpp)

foreach(bench_src ${HFT_BENCH_SOURCES})
  get_filename_component(bench_name ${bench_src} NAME_WE)
  add_executable(${bench_name} ${bench_src})
  target_link_libraries(${bench_name} PRIVATE hft_core benchmark::benchmark)
endforeach()
