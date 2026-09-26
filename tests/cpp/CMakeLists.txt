adx_add_cpp_test(adx_tests
    SOURCES
        test_smoke.cpp
        rt/test_alloc_guard.cpp
        rt/test_spsc_ring.cpp
        rt/test_overwrite_ring.cpp
        rt/test_fixed_containers.cpp
        rt/test_block_arena.cpp
        rt/test_reaper.cpp
        rt/test_stream_silence.cpp
        project/test_time.cpp
        project/test_tempomap.cpp
        project/test_curve.cpp
        project/test_commands.cpp
        project/test_undo_random.cpp
        project/test_validate.cpp
        format/test_lexer.cpp
        format/test_diagnostics.cpp
        format/test_roundtrip.cpp
        format/test_parser.cpp
        format/test_writer.cpp
        format/test_v1shim.cpp
)

# The corpus in docs/examples/ is a permanent parser regression fixture
# (FINAL_PLAN §6), and docs/adx-format-v2.md is what the diagnostic-code check reads.
# Both are found relative to the source tree rather than the build tree, because the
# test binary is run from wherever ctest happens to put it.
target_compile_definitions(adx_tests PRIVATE ADX_REPO_ROOT="${CMAKE_SOURCE_DIR}")
