add_library(graphics INTERFACE)

target_sources(graphics INTERFACE ${CMAKE_CURRENT_LIST_DIR}/graphics.c)

if (GRAPHICS_DUAL_OUTPUT)
    target_compile_definitions(graphics INTERFACE
            GRAPHICS_DUAL_OUTPUT=1
            VGA=1
    )
    target_link_libraries(graphics INTERFACE
            vga-nextgen
            hardware_clocks
            hardware_dma
            hardware_pwm
            hardware_vreg
    )
    target_include_directories(graphics INTERFACE ${CMAKE_SOURCE_DIR})
endif()

#target_link_libraries(graphics INTERFACE
#vga-nextgen
#hdmi
#tft
#)

target_include_directories(graphics INTERFACE ${CMAKE_CURRENT_LIST_DIR})

#pico_generate_pio_header(st7789
#        ${CMAKE_CURRENT_LIST_DIR}/st7789.pio
#)
