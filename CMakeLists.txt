cmake_minimum_required(VERSION 3.21)
set(PICO_FLASH_SIZE_BYTES 16777216)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE "MinSizeRel")

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(ntsc-tv C CXX ASM)


set(OUTPUT_DIR "${CMAKE_SOURCE_DIR}/bin/${PICO_PLATFORM}/${CMAKE_BUILD_TYPE}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${OUTPUT_DIR}")
pico_sdk_init()

option(VGA "Enable VGA output" ON)
option(NTSC_TV "Enable NTSC composite output" ON)
option(HDMI "Enable HDMI output" OFF)
option(TFT "Enable TFT output" OFF)
option(NTSC_LOW_RAM "Use compact NTSC palette tables" OFF)
option(NTSC_USE_SCRATCH_Y "Place NTSC palette tables in scratch Y" OFF)

if (NOT VGA AND NOT NTSC_TV AND NOT HDMI AND NOT TFT)
    message(FATAL_ERROR "Enable at least one video output: VGA, NTSC_TV, HDMI, or TFT")
endif()

if (HDMI AND (VGA OR NTSC_TV))
    message(FATAL_ERROR "HDMI is a single-output form; disable VGA and NTSC_TV")
endif()

# The panel sits on GPIO 6, 8, 9, 10, 12 and 13, which is the VGA range.
if (TFT AND VGA)
    message(FATAL_ERROR "TFT and VGA want the same pins; select one of them")
endif()

# TFT with NTSC is allowed. The panel needs GPIO 6..13 and PIO0, composite
# needs GPIO28 and a PWM slice, and the panel is pushed a frame at a time
# rather than from an interrupt, so the two do not get in each other's way.

if (HDMI AND TFT)
    message(FATAL_ERROR "Select HDMI or TFT, not both")
endif()

if (VGA)
    add_subdirectory(drivers/vga-nextgen)
endif()
if (NTSC_TV)
    add_subdirectory(drivers/ntsc-tv)
endif()
if (HDMI)
    add_subdirectory(drivers/hdmi)
endif()
if (TFT)
    add_subdirectory(drivers/st7789)
endif()
add_subdirectory(drivers/graphics)

# No -fwhole-program here. It tells the compiler that the file it is reading is
# the whole program, so every symbol but main may be made static. These options
# reach each file on its own, so each one believed it was alone: nothing inside
# graphics_modes.c calls graphics_source_set_mode(), so the function was dropped
# as dead and the calls from the other files folded away. graphics_set_mode()
# then did nothing and text mode never came on. -flto already does this analysis
# across files, and the GCC manual says not to put the two together.
add_compile_options(-flto -ffunction-sections -fdata-sections -fms-extensions -O3)
add_link_options(-flto)

add_executable(${PROJECT_NAME})

pico_define_boot_stage2(slower_boot ${PICO_DEFAULT_BOOT_STAGE2_FILE})
target_compile_definitions(slower_boot PRIVATE PICO_FLASH_SPI_CLKDIV=4)
pico_set_boot_stage2(${PROJECT_NAME} slower_boot)

pico_add_extra_outputs(${PROJECT_NAME})

target_compile_definitions(${PROJECT_NAME} PRIVATE
        PICO_PRINTF_ALWAYS_INCLUDED=0
        PICO_PANIC_FUNCTION=
)
#pico_set_float_implementation(${PROJECT_NAME} none) # size optimizations
pico_set_double_implementation(${PROJECT_NAME} none) # size optimizations
pico_set_printf_implementation(${PROJECT_NAME} none)

target_sources(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/ntsc-tv.c)

target_link_libraries(${PROJECT_NAME}
        graphics
        pico_runtime
        hardware_dma
        hardware_pwm
        pico_multicore

        -Wl,--wrap=atexit # size optimizations
)

if (VGA)
    target_link_libraries(${PROJECT_NAME} vga-nextgen)
endif()
if (NTSC_TV)
    target_link_libraries(${PROJECT_NAME} ntsc-tv-driver)
endif()
if (HDMI)
    target_link_libraries(${PROJECT_NAME} hdmi)
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "ntsc-tv-hdmi")
endif()
if (TFT)
    target_link_libraries(${PROJECT_NAME} st7789)
    if (NTSC_TV)
        set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "ntsc-tv-tft-composite")
    else()
        set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "ntsc-tv-tft")
    endif()
endif()

target_link_options(${PROJECT_NAME} PRIVATE -Xlinker --print-memory-usage --data-sections --function-sections)
