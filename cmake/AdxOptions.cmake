include_guard(GLOBAL)

# Every knob adX has, in one file. A phase that needs a new option adds it here
# rather than declaring one next to the code it guards, so `grep ADX_` in this
# file is a complete answer to "what can this build do" (phase_0.md §3.5).

option(ADX_BUILD_TESTS "Build adx_tests and register its cases with ctest" ON)
option(ADX_BUILD_BINDINGS "Build the adx_engine Python extension module" ON)
option(ADX_WARNINGS_AS_ERRORS "Treat compiler warnings as errors in first-party targets" ON)

# Declared now with nothing behind them on purpose: Phase 1 and Phase 10 then add
# code, not build plumbing (phase_0.md §3.5).
option(ADX_ENABLE_ONNX "Compile the ONNX melody extractor (FINAL_PLAN §8.2)" OFF)
option(ADX_ENABLE_ASAN "Build with AddressSanitizer; used by the P1/P3 nightly jobs" OFF)

# The RT guard belongs wherever assertions are live, because it reports through
# them. Multi-config generators have no CMAKE_BUILD_TYPE, so default to ON there
# and let the generator expression in engine/CMakeLists.txt do the per-config work.
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "MinSizeRel")
    set(adx_rt_guard_default OFF)
else()
    set(adx_rt_guard_default ON)
endif()
option(ADX_ENABLE_RT_GUARD
    "Enable Phase 1's audio-thread allocator hook (Debug/RelWithDebInfo)"
    ${adx_rt_guard_default})

function(adx_print_configuration_summary)
    message(STATUS "")
    message(STATUS "adX ${PROJECT_VERSION} configuration")
    message(STATUS "  build type .......... ${CMAKE_BUILD_TYPE}")
    message(STATUS "  tests ............... ${ADX_BUILD_TESTS}")
    message(STATUS "  bindings ............ ${ADX_BUILD_BINDINGS}")
    message(STATUS "  warnings as errors .. ${ADX_WARNINGS_AS_ERRORS}")
    message(STATUS "  RT guard ............ ${ADX_ENABLE_RT_GUARD}")
    message(STATUS "  ONNX ................ ${ADX_ENABLE_ONNX}")
    message(STATUS "  ASan ................ ${ADX_ENABLE_ASAN}")
    message(STATUS "  dependency cache .... ${FETCHCONTENT_BASE_DIR}")
    message(STATUS "")
endfunction()
