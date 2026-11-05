# Host unit tests for the app code. Each test_*.c is its own program:
# it returns 0, or an assert stops it.

file(GLOB APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/app/*.c)
add_library(app STATIC ${APP_SOURCES})
target_include_directories(app PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(app PUBLIC -Wall -Wextra -Wshadow)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.c)
foreach(test_source ${TEST_SOURCES})
    get_filename_component(test_name ${test_source} NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE app)
    add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
