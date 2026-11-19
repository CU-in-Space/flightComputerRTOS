# Writes fw_version.h: the git commit and the build time, so a log's boot line
# says which firmware made it. Run at every build (see CMakeLists.txt). The
# header is only replaced when the text changes.
#   cmake -DSRC=<source dir> -DOUT=<header> -P fw_version.cmake

execute_process(COMMAND git describe --always --dirty --abbrev=8
                WORKING_DIRECTORY ${SRC}
                OUTPUT_VARIABLE commit
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(NOT commit)
    set(commit "unknown")
endif()
string(TIMESTAMP built "%Y-%m-%d %H:%M")

file(WRITE ${OUT}.new "#define FW_VERSION \"${commit} built ${built}\"\n")
execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different ${OUT}.new ${OUT})
