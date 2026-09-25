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
    "/Applications/ArmGNUToolchain/*/arm-none-eabi/bin"             # macOS cask
    "/opt/arm-gnu-toolchain-*/bin"                                   # Linux tarball
    "C:/Program Files (x86)/Arm GNU Toolchain arm-none-eabi/*/bin"   # Windows (winget / Arm installer)
    "C:/Program Files/Arm GNU Toolchain arm-none-eabi/*/bin"
    "C:/Program Files (x86)/Arm/GNU Toolchain *arm-none-eabi/bin"   # Windows, other Arm installer layouts
    "C:/Program Files/Arm/GNU Toolchain *arm-none-eabi/bin")
find_program(ARM_GCC ${TOOLCHAIN_PREFIX}gcc
    PATHS ${ARM_TOOLCHAIN_DIRS} /usr/bin /usr/local/bin)
if(NOT ARM_GCC)
    message(FATAL_ERROR
        "arm-none-eabi-gcc was not found on PATH or in the usual install folders.\n"
        "  - Just installed it? Restart VS Code / your terminal so it sees the new PATH.\n"
        "  - Otherwise add the toolchain's bin folder to PATH, or point CMake at it:\n"
        "      cmake --preset debug -DARM_GCC=<full path to arm-none-eabi-gcc>\n"
        "Delete the build folder before configuring again.")
endif()
get_filename_component(TOOLCHAIN_BIN "${ARM_GCC}" DIRECTORY)

# Use find_program's full paths rather than "${TOOLCHAIN_BIN}/arm-none-eabi-gcc":
# on Windows the file is arm-none-eabi-gcc.exe, and CMake won't add the .exe to a
# path built by hand.
set(CMAKE_C_COMPILER   "${ARM_GCC}")
set(CMAKE_ASM_COMPILER "${ARM_GCC}")
find_program(ARM_GXX       ${TOOLCHAIN_PREFIX}g++     PATHS "${TOOLCHAIN_BIN}" NO_DEFAULT_PATH)
find_program(CMAKE_OBJCOPY ${TOOLCHAIN_PREFIX}objcopy PATHS "${TOOLCHAIN_BIN}" NO_DEFAULT_PATH REQUIRED)
if(ARM_GXX)
    set(CMAKE_CXX_COMPILER "${ARM_GXX}")
endif()

# Cortex-M7 with the double-precision FPU, as fitted on the STM32H743.
set(MCU_FLAGS "-mcpu=cortex-m7 -mfpu=fpv5-d16 -mfloat-abi=hard -mthumb")
set(CMAKE_C_FLAGS_INIT   "${MCU_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${MCU_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${MCU_FLAGS}")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
