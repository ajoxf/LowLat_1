# One tool executable per *.cpp file in tools/.
file(GLOB HFT_TOOL_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)

foreach(tool_src ${HFT_TOOL_SOURCES})
  get_filename_component(tool_name ${tool_src} NAME_WE)
  add_executable(${tool_name} ${tool_src})
  target_link_libraries(${tool_name} PRIVATE hft_core hft_strict)
endforeach()
