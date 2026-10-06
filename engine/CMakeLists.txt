# engine/ builds exactly one target. Each subdirectory adds its sources to it via
# target_sources; there is no per-subdirectory library.
#
# Why: twelve small static libs produce link-order problems and
# circular-dependency workarounds for no benefit on a single-binary project.
# Directory structure is for humans; the linker does not need it (phase_0.md §3.1).
add_library(adx_engine_static STATIC)
add_library(adx::engine ALIAS adx_engine_static)

# Repo-root-relative quoted includes everywhere: `#include "engine/rt/SpscRing.h"`.
# No include/ vs src/ split to keep in sync - iteration one had one and the two
# drifted - and every include site names the subsystem it reaches into, which
# makes a layering violation visible in the diff that introduces it
# (phase_0.md §3.2).
target_include_directories(adx_engine_static PUBLIC
    "${CMAKE_SOURCE_DIR}"
    "${ADX_GENERATED_INCLUDE_DIR}"
)

target_compile_features(adx_engine_static PUBLIC cxx_std_20)

# The engine is linked into a shared Python module, so it is compiled as if it
# were going into one. Free on Windows; correct everywhere else.
set_target_properties(adx_engine_static PROPERTIES POSITION_INDEPENDENT_CODE ON)

target_compile_definitions(adx_engine_static PUBLIC
    # ADX_ASSERT is live in Debug and RelWithDebInfo and compiled out in Release
    # (phase_0.md §4.3). This is keyed off an adX macro rather than NDEBUG
    # because CMake defines NDEBUG in RelWithDebInfo, which is exactly the config
    # where assertions must stay.
    $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:ADX_ASSERTIONS_ENABLED=1>
    $<$<BOOL:${ADX_ENABLE_RT_GUARD}>:ADX_ENABLE_RT_GUARD=1>
    $<$<BOOL:${ADX_ENABLE_ONNX}>:ADX_ENABLE_ONNX=1>
)

adx_set_warnings(adx_engine_static)

# A directory appears here when the phase that owns it puts its first file in it.
add_subdirectory(core)
add_subdirectory(rt)
add_subdirectory(audio)
add_subdirectory(project)
add_subdirectory(format)
add_subdirectory(transport)
add_subdirectory(graph)
add_subdirectory(render)
add_subdirectory(dsp)
add_subdirectory(instruments)
add_subdirectory(effects)
add_subdirectory(preset)
add_subdirectory(geometry)
