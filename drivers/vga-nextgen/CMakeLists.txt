add_library(vga-nextgen INTERFACE)

target_sources(vga-nextgen INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/vga.c
        ${CMAKE_CURRENT_LIST_DIR}/graphics.c
)

target_compile_definitions(vga-nextgen INTERFACE VGA=1)

# The same pins and the same PIO program, at the 15 kHz 240p a SCART set takes.
if (RGBS)
    target_compile_definitions(vga-nextgen INTERFACE VGA_RGBS=1)
endif()

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
