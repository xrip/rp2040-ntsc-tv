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

if ((HDMI OR TFT) AND (VGA OR NTSC_TV))
    message(FATAL_ERROR "HDMI and TFT are single-output forms; disable VGA and NTSC_TV")
endif()

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

#add_compile_options(-ffunction-sections -fdata-sections -fms-extensions -O3)
add_compile_options(-flto -fwhole-program -ffunction-sections -fdata-sections -fms-extensions -O3)
add_link_options(-flto -fwhole-program)

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
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "ntsc-tv-tft")
endif()

target_link_options(${PROJECT_NAME} PRIVATE -Xlinker --print-memory-usage --data-sections --function-sections)
