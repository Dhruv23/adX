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

    # Catch2 hides a test tagged [.something] from --list-tests, which is how a slow
    # test stays out of a developer's inner loop - but it also means
    # catch_discover_tests never sees it, and a test ctest cannot run is a test that
    # does not exist. Phase 1's whole gate, null_backend_60s_zero_violations, is one
    # of these.
    #
    # So the hidden sets get one ctest entry each, labelled, and they run by default.
    # Opt-out, not opt-in: this project's central claim is realtime safety, and a gate
    # somebody has to remember to ask for is a gate that rots. `ctest -LE slow` is the
    # fast loop, and it is documented in the README.
    add_test(NAME ${name}_slow COMMAND ${name} "[.slow]" --allow-running-no-tests)
    set_tests_properties(${name}_slow PROPERTIES LABELS slow TIMEOUT 600)

    # Skips itself where there is no audio hardware, which is every CI runner.
    add_test(NAME ${name}_device COMMAND ${name} "[.device]" --allow-running-no-tests)
    set_tests_properties(${name}_device PROPERTIES LABELS device TIMEOUT 180)
endfunction()
