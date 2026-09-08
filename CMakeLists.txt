cmake_minimum_required(VERSION 3.22)

project(cuis-fc VERSION 0.1.0 LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)   # feeds clangd / C-C++ IntelliSense

set(TARGET_MCU     STM32H753xx)
set(LINKER_SCRIPT  ${CMAKE_SOURCE_DIR}/linker/STM32H753ZI_FLASH.ld)
set(STARTUP_SOURCE ${CMAKE_SOURCE_DIR}/lib/cmsis_device_h7/Source/Templates/gcc/startup_stm32h753xx.s)

# -Og keeps the code debuggable without the noise of -O0.
set(CMAKE_C_FLAGS_DEBUG "-Og -g3 -ggdb")

# Let the linker drop everything nothing references.
add_compile_options(-ffunction-sections -fdata-sections)

# ---------------------------------------------------------------------------
# FreeRTOS kernel
#
# The kernel's own CMakeLists expects the application to supply a target named
# freertos_config that points at our FreeRTOSConfig.h.
# ---------------------------------------------------------------------------
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config INTERFACE ${CMAKE_SOURCE_DIR}/include)

set(FREERTOS_PORT GCC_ARM_CM7 CACHE STRING "Cortex-M7 r0p1 GCC port" FORCE)
set(FREERTOS_HEAP 4           CACHE STRING "heap_4: coalescing first-fit"  FORCE)
add_subdirectory(lib/FreeRTOS-Kernel)

# ---------------------------------------------------------------------------
# CMSIS + STM32H7 HAL
# ---------------------------------------------------------------------------
file(GLOB HAL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/lib/stm32h7xx_hal_driver/Src/*.c)
list(FILTER HAL_SOURCES EXCLUDE REGEX ".*_template\\.c$")

add_library(stm32cube STATIC
    ${CMAKE_SOURCE_DIR}/lib/cmsis_device_h7/Source/Templates/system_stm32h7xx.c
    ${HAL_SOURCES}
)

target_include_directories(stm32cube PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/cmsis_core/Include
    ${CMAKE_SOURCE_DIR}/lib/cmsis_device_h7/Include
    ${CMAKE_SOURCE_DIR}/lib/stm32h7xx_hal_driver/Inc
)

target_compile_definitions(stm32cube PUBLIC ${TARGET_MCU} USE_HAL_DRIVER)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
# Globbed so files lifted out of CubeMX (stm32h7xx_hal_msp.c, peripheral inits)
# get picked up without editing this file. CONFIGURE_DEPENDS makes Ninja
# re-glob when the directory changes.
file(GLOB APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/*.c)

add_executable(${PROJECT_NAME}
    ${APP_SOURCES}
    ${STARTUP_SOURCE}
)

# Strict warnings on our code only -- vendor code is noisier than we want to see.
target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra -Wshadow -Wdouble-promotion)

target_link_libraries(${PROJECT_NAME} PRIVATE stm32cube freertos_kernel)

target_link_options(${PROJECT_NAME} PRIVATE
    -T${LINKER_SCRIPT}
    --specs=nano.specs      # newlib-nano: much smaller printf/malloc
    --specs=nosys.specs     # stub out the syscalls we have no OS for
    -Wl,--gc-sections
    -Wl,-Map=${PROJECT_NAME}.map,--cref
    -Wl,--print-memory-usage
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    SUFFIX       ".elf"
    LINK_DEPENDS ${LINKER_SCRIPT}
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${PROJECT_NAME}> $<TARGET_FILE_DIR:${PROJECT_NAME}>/${PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}> $<TARGET_FILE_DIR:${PROJECT_NAME}>/${PROJECT_NAME}.bin
    COMMENT "Generating .hex and .bin"
)
