# Run with `cmake -P`, at build time, not configure time.
#
# Capturing the commit during configure means a build made after committing
# reports the *previous* commit, which is harmless for a version banner and not
# harmless at all once a golden render hash is attributed to a revision
# (FINAL_PLAN §9). So this runs on every build instead.
#
# configure_file only rewrites the output when the content differs, so the common
# case - building twice on the same commit with no local edits - touches nothing
# and triggers no recompile.
#
# Expects: ADX_SOURCE_DIR, ADX_TEMPLATE, ADX_OUTPUT, and the adx_VERSION_* values,
# all passed with -D by the custom command in engine/core/CMakeLists.txt.

set(ADX_GIT_SHA "unknown")

find_package(Git QUIET)
if(GIT_EXECUTABLE)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
        WORKING_DIRECTORY "${ADX_SOURCE_DIR}"
        OUTPUT_VARIABLE adx_sha
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE adx_sha_result
    )
    if(adx_sha_result EQUAL 0 AND adx_sha)
        set(ADX_GIT_SHA "${adx_sha}")

        # A commit id alone is a claim the working tree does not support. Tracked
        # modifications get a -dirty suffix so a hash produced from an uncommitted
        # edit can never be mistaken for one produced from that commit. Untracked
        # files are excluded: they cannot change what the build compiles.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${ADX_SOURCE_DIR}"
            OUTPUT_VARIABLE adx_dirty
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE adx_dirty_result
        )
        if(adx_dirty_result EQUAL 0 AND adx_dirty)
            set(ADX_GIT_SHA "${ADX_GIT_SHA}-dirty")
        endif()
    endif()
endif()

configure_file("${ADX_TEMPLATE}" "${ADX_OUTPUT}" @ONLY)
