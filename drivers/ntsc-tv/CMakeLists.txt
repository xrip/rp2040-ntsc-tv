add_library(ntsc-tv-driver INTERFACE)

target_sources(ntsc-tv-driver INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}/ntsc-tv.c
        ${CMAKE_CURRENT_LIST_DIR}/graphics.c
)

target_compile_definitions(ntsc-tv-driver INTERFACE NTSC_TV=1)

# Older graphics modules get their mode constants from the selected driver.
# Pre-include this public header so the NTSC folder can be copied on its own.
target_compile_options(ntsc-tv-driver INTERFACE
        $<$<COMPILE_LANGUAGE:C>:-include${CMAKE_CURRENT_LIST_DIR}/ntsc-tv.h>
        $<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_LIST_DIR}/ntsc-tv.h>
)

# The default is GPIO27, set by ntsc-tv.h. A board which already uses that pin
# gives another one here; a command-line define beats every header default.
if (DEFINED NTSC_PIN_OUTPUT)
    target_compile_definitions(ntsc-tv-driver INTERFACE NTSC_PIN_OUTPUT=${NTSC_PIN_OUTPUT})
endif()

option(NTSC_LOW_RAM "Use compact NTSC palette tables" OFF)
if (NTSC_LOW_RAM)
    target_compile_definitions(ntsc-tv-driver INTERFACE NTSC_LOW_RAM=1)
endif()

option(NTSC_USE_SCRATCH_Y "Place NTSC palette tables in scratch Y" OFF)
if (NTSC_USE_SCRATCH_Y)
    target_compile_definitions(ntsc-tv-driver INTERFACE NTSC_USE_SCRATCH_Y=1)
endif()

target_link_libraries(ntsc-tv-driver INTERFACE
        graphics
        hardware_dma
        hardware_gpio
        hardware_irq
        hardware_pwm
        hardware_sync
)

target_include_directories(ntsc-tv-driver INTERFACE
        ${CMAKE_CURRENT_LIST_DIR}
)
