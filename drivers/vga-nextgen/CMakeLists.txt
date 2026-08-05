add_library(vga-nextgen INTERFACE)

target_sources(vga-nextgen INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/vga.c
        ${CMAKE_CURRENT_LIST_DIR}/graphics.c
)

target_compile_definitions(vga-nextgen INTERFACE VGA=1)

target_link_libraries(vga-nextgen INTERFACE
        graphics
        hardware_pio
        hardware_dma
        hardware_gpio
        hardware_irq
        hardware_clocks
)

target_include_directories(vga-nextgen INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
)
