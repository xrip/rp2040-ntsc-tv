add_library(st7789 INTERFACE)

target_sources(st7789 INTERFACE ${CMAKE_CURRENT_LIST_DIR}/st7789.c)

target_compile_definitions(st7789 INTERFACE TFT=1)

target_link_libraries(st7789 INTERFACE
        hardware_gpio
        hardware_pio
        hardware_sync
        pico_stdlib
)

target_include_directories(st7789 INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
)
