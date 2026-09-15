# Bare-metal ARM toolchain file for the STM32H743 (Cortex-M7F).
# Selected with -DCMAKE_TOOLCHAIN_FILE=... (CMakePresets.json does this for you).

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

# CMake's compiler probe links a full executable by default, which would need a
# linker script we haven't applied yet. Probe with a static library instead.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(TOOLCHAIN_PREFIX arm-none-eabi-)

# Resolved from PATH first. The PATHS below are only consulted if PATH has no
# toolchain -- deliberately PATHS, not HINTS: CMake searches HINTS *before*
# PATH, so a stale install in a hinted location would silently beat a good one
# on PATH. (That happened: macOS 27 dropped Rosetta, and the old x86-only
# Homebrew gcc-arm-none-eabi formula won over the working native toolchain.)
file(GLOB ARM_TOOLCHAIN_DIRS
    "/Applications/ArmGNUToolchain/*/arm-none-eabi/bin"           # macOS cask
    "/opt/arm-gnu-toolchain-*/bin"                                 # Linux tarball
    "C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/*/bin"  # Windows
    "C:/Program Files/Arm GNU Toolchain arm-none-eabi/*/bin")
find_program(ARM_GCC ${TOOLCHAIN_PREFIX}gcc
    PATHS ${ARM_TOOLCHAIN_DIRS} /usr/bin /usr/local/bin
    REQUIRED)
get_filename_component(TOOLCHAIN_BIN "${ARM_GCC}" DIRECTORY)

set(CMAKE_C_COMPILER   "${TOOLCHAIN_BIN}/${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_ASM_COMPILER "${TOOLCHAIN_BIN}/${TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_BIN}/${TOOLCHAIN_PREFIX}g++")
set(CMAKE_OBJCOPY      "${TOOLCHAIN_BIN}/${TOOLCHAIN_PREFIX}objcopy" CACHE FILEPATH "objcopy")
set(CMAKE_SIZE         "${TOOLCHAIN_BIN}/${TOOLCHAIN_PREFIX}size"    CACHE FILEPATH "size")

# Cortex-M7 with the double-precision FPU, as fitted on the STM32H743.
set(MCU_FLAGS "-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb")
set(CMAKE_C_FLAGS_INIT   "${MCU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${MCU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${MCU_FLAGS}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
