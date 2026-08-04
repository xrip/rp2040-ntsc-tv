add_library(vga-nextgen INTERFACE)

if (GRAPHICS_DUAL_OUTPUT)
    target_sources(vga-nextgen INTERFACE
            ${CMAKE_CURRENT_LIST_DIR}/vga-dual.c
    )
    target_compile_definitions(vga-nextgen INTERFACE GRAPHICS_DUAL_OUTPUT=1)
else()
    target_sources(vga-nextgen INTERFACE
            ${CMAKE_CURRENT_LIST_DIR}/vga.c
    )
endif()

target_link_libraries(vga-nextgen INTERFACE
        hardware_pio
        hardware_dma
        hardware_gpio
        hardware_irq
        hardware_clocks
)

target_include_directories(vga-nextgen INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
)
