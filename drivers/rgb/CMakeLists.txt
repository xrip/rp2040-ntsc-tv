add_library(rgb INTERFACE)

target_sources(rgb INTERFACE ${CMAKE_CURRENT_LIST_DIR}/tv.c)

target_link_libraries(rgb INTERFACE hardware_pio hardware_clocks hardware_dma)

target_include_directories(rgb INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
)

