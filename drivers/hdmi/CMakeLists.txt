add_library(hdmi INTERFACE)

target_sources(hdmi INTERFACE ${CMAKE_CURRENT_LIST_DIR}/hdmi.c)

target_compile_definitions(hdmi INTERFACE HDMI=1)

target_link_libraries(hdmi INTERFACE
        hardware_clocks
        hardware_dma
        hardware_gpio
        hardware_irq
        hardware_pio
        hardware_sync
)

target_include_directories(hdmi INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
)
