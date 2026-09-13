include_guard(GLOBAL)

# adx_set_warnings(<target>)
#
# Applied per target, never globally. FetchContent compiles third-party sources
# inside this same build, and /WX on code we do not own turns someone else's
# harmless warning into our red build (phase_0.md §9). Dependency targets never
# receive this.
#
# Platform branches are isolated here so adding macOS/Linux later is a change to
# one file rather than a hunt through the tree (phase_0.md §7).
function(adx_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:preprocessor
            /Zc:__cplusplus
            /EHsc
            # Bit-identical offline-vs-realtime rendering (Phase 3) and the
            # golden-hash corpus (FINAL_PLAN §9) both require that float
            # arithmetic means exactly one thing. /fp:fast is banned
            # project-wide because it permits reassociation; FMA contraction is
            # banned for the same reason - a contraction that fires in one build
            # and not another changes the hash.
            #
            # phase_0.md §4.1 spells this /fp:contract=off, which MSVC rejects
            # (D9002): there is no negative form of /fp:contract. /fp:precise is
            # the flag that disables contraction on MSVC 2022 and later, and
            # stating it explicitly also overrides any /fp:fast inherited from a
            # toolchain file, since the last /fp option on the command line wins.
            /fp:precise
        )
        # CMAKE_CXX_FLAGS_RELEASE already supplies /O2; it is not repeated here so
        # there is only one place that decides optimization level per config.
        if(ADX_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        if(ADX_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE /fsanitize=address)
            # MSVC's ASan and incremental linking are mutually exclusive, and
            # CMake's Debug linker flags ask for incremental by default.
            target_link_options(${target} PRIVATE /INCREMENTAL:NO)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion
            -ffp-contract=off
        )
        if(ADX_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
        if(ADX_ENABLE_ASAN)
            target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(${target} PRIVATE -fsanitize=address)
        endif()
    endif()
endfunction()
