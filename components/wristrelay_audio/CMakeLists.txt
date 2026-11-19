idf_component_register()
include("${CMAKE_CURRENT_LIST_DIR}/opus_source.cmake")
wr_add_opus()
target_link_libraries(${COMPONENT_LIB} INTERFACE opus)
