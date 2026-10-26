add_library(hdmi INTERFACE)

target_sources(hdmi INTERFACE ${CMAKE_CURRENT_LIST_DIR}/hdmi.c)

# RP2040's scanline renderer divides row/font/VRAM addresses. Keep the SDK
# hardware-divider entry points in SRAM so the IRQ never waits for XIP code.
target_compile_definitions(hdmi INTERFACE HDMI=1 PICO_DIVIDER_IN_RAM=1)

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
