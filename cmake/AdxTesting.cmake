include_guard(GLOBAL)

# Catch2 ships its ctest integration as a module in extras/ rather than in its
# package config, so it has to be put on the module path by hand.
if(NOT COMMAND catch_discover_tests)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    include(Catch)
endif()

# adx_add_cpp_test(<name> SOURCES <source>...)
#
# Adding a Catch2 suite in a later phase is one call to this (phase_0.md §8).
# catch_discover_tests registers each TEST_CASE as its own ctest test, so a
# failure names the case rather than the binary.
function(adx_add_cpp_test name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "adx_add_cpp_test(${name}): unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arg_SOURCES)
        message(FATAL_ERROR "adx_add_cpp_test(${name}): SOURCES is required")
    endif()

    add_executable(${name} ${arg_SOURCES})
    target_link_libraries(${name} PRIVATE adx::engine Catch2::Catch2WithMain)
    adx_set_warnings(${name})
    catch_discover_tests(${name})
endfunction()
