include_guard(GLOBAL)

# Cache dependency sources outside the build tree so `rm -rf build/` costs seconds
# instead of hundreds of megabytes of re-download (phase_0.md §3.4). .deps/ is
# gitignored.
#
# One subdirectory per build tree, not one shared directory. phase_0.md §3.4
# specifies a bare ${CMAKE_SOURCE_DIR}/.deps, which cannot work: FETCHCONTENT_BASE_DIR
# also holds each dependency's -subbuild and -build directory, and those carry a
# CMakeCache.txt bound to one binary directory. Two build trees always exist - the
# preset tree and the one scikit-build-core creates for the wheel - so the shared
# layout fails the second configure with "CMakeCache.txt was created for a
# different binary directory", which is exactly how `pip install -e .` failed the
# first time it ran here.
#
# Keyed on the build tree's directory name rather than the build type, because the
# wheel build is also a Release tree and would otherwise collide with the
# windows-x64-release preset. Deleting build/ and reconfiguring the same preset
# reuses the same key, which is the property §3.4 actually wants.
get_filename_component(adx_build_tree_name "${CMAKE_BINARY_DIR}" NAME)
set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.deps/${adx_build_tree_name}" CACHE PATH
    "Where FetchContent clones and builds dependencies for this build tree")

include(FetchContent)

# FetchContent with pinned tags, not vcpkg: iteration one proved FetchContent
# resolves every dependency this project needs, and vcpkg would add a toolchain
# bootstrap plus a manifest to maintain for zero gain on one target platform.
#
# Escape hatch, recorded so it is not relitigated: if a future dependency
# (realistically the VST3 SDK in Phase 9) is painful under FetchContent, that one
# dependency may be vendored or moved to find_package, with the reason written
# into FINAL_PLAN.md §8.
#
# A dependency is declared in the phase that first needs it, not before. Phase 0
# needs exactly two. The known-good tags for the rest (rtaudio 6.0.1, miniaudio,
# rubberband v4.0.0, onnxruntime 1.27.1) are in
# _archive/src-cpp/CMakeLists.txt and are the starting points.
#
# Nothing here tracks a branch. SYSTEM suppresses warnings from headers we do not
# own, which is the other half of adx_set_warnings() being per-target.

#
# EXCLUDE_FROM_ALL on every dependency. Without it a dependency's own install()
# rules run during `cmake --install`, which is the step scikit-build-core uses to
# assemble the wheel: RtAudio was installing rtaudio.lib and its headers *into the
# adx wheel* and then failing outright on its config file. A dependency is still
# built, because our targets link it - it is just not part of `all`, and its install
# rules are not ours.
# adx-license: pybind11 | pybind11 | BSD-3-Clause | bindings
FetchContent_Declare(pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v2.13.6
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)

# Phase 1: real audio devices. Same tag iteration one used and shipped on.
# adx-license: rtaudio | RtAudio | MIT | audio devices
FetchContent_Declare(rtaudio
    GIT_REPOSITORY https://github.com/thestk/rtaudio.git
    GIT_TAG 6.0.1
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)

# adx-license: Catch2 | Catch2 | BSL-1.0 | tests only, not shipped
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.7.1
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)

# Populated only when the option that needs them is on. The wheel build sets
# ADX_BUILD_TESTS=OFF, so `pip install` never downloads Catch2.
if(ADX_BUILD_BINDINGS)
    # Build the extension against the interpreter that is going to import it.
    #
    # Left to itself, pybind11 uses its own legacy FindPythonLibsNew, which
    # searches the Windows registry and readily lands on the Microsoft Store stub
    # - on this project's first configure that was Python 3.14, while pip, pytest
    # and the console script were all 3.12, so the .pyd would have built fine and
    # then failed to import. PYBIND11_FINDPYTHON switches it to CMake's FindPython,
    # and finding Python here rather than letting pybind11 do it lets the version
    # floor match requires-python in pyproject.toml.
    set(PYBIND11_FINDPYTHON ON)
    if(NOT DEFINED Python_EXECUTABLE)
        # PATH, not the registry: `python` on PATH is the interpreter whose pip
        # installs this package. scikit-build-core passes Python_EXECUTABLE
        # explicitly, so this branch is skipped during a wheel build.
        find_program(Python_EXECUTABLE
            NAMES python python3
            NO_CMAKE_SYSTEM_PATH
            DOC "Interpreter the adx_engine extension is built for"
        )
    endif()
    find_package(Python 3.12 REQUIRED COMPONENTS Interpreter Development.Module)

    FetchContent_MakeAvailable(pybind11)
endif()

# Static, and no test programs: adX ships one binary, and RtAudio's test suite is
# not ours to run. Set before MakeAvailable because they are cache variables the
# subproject reads at configure time.
set(RTAUDIO_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTAUDIO_BUILD_PYTHON OFF CACHE BOOL "" FORCE)
# ASIO (Phase 9, phase_9.md §4.9): a device-API flag on the same backend class, not a
# new backend. The ASIO host files come with RtAudio.
set(RTAUDIO_API_ASIO ${ADX_ENABLE_ASIO} CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rtaudio)

# Phase 9: MIDI devices. RtAudio's sibling - same author, same licence, WinMM on Windows
# (FINAL_PLAN §8). Static and without its tests, for the reasons RtAudio is.
# adx-license: rtmidi | RtMidi | MIT | MIDI devices
FetchContent_Declare(rtmidi
    GIT_REPOSITORY https://github.com/thestk/rtmidi.git
    GIT_TAG 6.0.0
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
# Its static switch is BUILD_SHARED_LIBS itself: set as a plain variable around the fetch
# (RtMidi requires CMake 3.24, so CMP0077 lets it override the option) and no further.
# Its uninstall target is renamed, because RtAudio already took the name.
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(RTMIDI_TARGETNAME_UNINSTALL "rtmidi-uninstall" CACHE STRING "" FORCE)
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(rtmidi)
unset(BUILD_SHARED_LIBS)

if(ADX_BUILD_TESTS)
    FetchContent_MakeAvailable(Catch2)
endif()

# miniaudio, decode only (phase_4.md §4.11): WAV, AIFF, FLAC and MP3 are built in, and
# Ogg Vorbis comes from the stb_vorbis that ships beside it. The archive fetched
# miniaudio's master branch, which is how a dependency changes under a build nobody
# touched; this pins the release it actually got (0.11.25, from its miniaudio.h).
#
# SOURCE_SUBDIR names a directory that does not exist, so MakeAvailable fetches the
# sources without configuring miniaudio's own CMake project (its examples, tests and
# device backends are none of ours). The implementation is one translation unit,
# generated here rather than kept in engine/: third-party code is compiled with the
# compiler's default warnings rather than adX's warnings-as-errors, and nothing under
# engine/ is exempt from clang-tidy.
# adx-license: miniaudio | miniaudio | MIT-0 | audio decoding
FetchContent_Declare(miniaudio
    GIT_REPOSITORY https://github.com/mackron/miniaudio.git
    GIT_TAG 0.11.25
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR adx-do-not-configure
)
FetchContent_MakeAvailable(miniaudio)
set(adx_miniaudio_impl "${CMAKE_BINARY_DIR}/generated/adx_miniaudio.cpp")
file(CONFIGURE OUTPUT "${adx_miniaudio_impl}" CONTENT [[
// Generated by cmake/AdxDependencies.cmake. miniaudio's decoder, plus stb_vorbis for
// Ogg Vorbis, in one translation unit.
#define STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#undef STB_VORBIS_HEADER_ONLY
#include "extras/stb_vorbis.c"
]])
add_library(adx_miniaudio STATIC "${adx_miniaudio_impl}")
target_include_directories(adx_miniaudio SYSTEM PUBLIC "${miniaudio_SOURCE_DIR}")
# Decode only: RtAudio owns device I/O, and nothing here wants miniaudio's engine.
target_compile_definitions(adx_miniaudio PUBLIC
    MA_NO_DEVICE_IO MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_GENERATION)
set_target_properties(adx_miniaudio PROPERTIES POSITION_INDEPENDENT_CODE ON)

# WORLD (modified BSD), the vocoder behind UTAU's tn_fnds and moresampler: F0 (Harvest,
# DIO), spectral envelope (CheapTrick), aperiodicity (D4C) and resynthesis, for the
# Voice instrument's resampler (phase_4.md §4.13, FINAL_PLAN §8). Pinned to a commit:
# the project has no release tags past 0.2.x. Like miniaudio it is compiled as its own
# library with the compiler's default warnings, and its own CMake project (examples,
# tools) is not configured.
# adx-license: world | WORLD | BSD-3-Clause | voice synthesis
FetchContent_Declare(world
    GIT_REPOSITORY https://github.com/mmorise/World.git
    GIT_TAG d625e7608ca23a870018f01e7c562ac683d9847f
    SOURCE_SUBDIR adx-do-not-configure
)
FetchContent_MakeAvailable(world)
add_library(adx_world STATIC
    "${world_SOURCE_DIR}/src/cheaptrick.cpp"
    "${world_SOURCE_DIR}/src/common.cpp"
    "${world_SOURCE_DIR}/src/d4c.cpp"
    "${world_SOURCE_DIR}/src/dio.cpp"
    "${world_SOURCE_DIR}/src/fft.cpp"
    "${world_SOURCE_DIR}/src/harvest.cpp"
    "${world_SOURCE_DIR}/src/matlabfunctions.cpp"
    "${world_SOURCE_DIR}/src/stonemask.cpp"
    "${world_SOURCE_DIR}/src/synthesis.cpp"
)
target_include_directories(adx_world SYSTEM PUBLIC "${world_SOURCE_DIR}/src")
set_target_properties(adx_world PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(adx_world PRIVATE /W0)
else()
    target_compile_options(adx_world PRIVATE -w)
endif()

# --- Phase 8: offline stretching and the export encoders (phase_8.md §4, FINAL_PLAN §8) ---
#
# Each is compiled as its own static library with the compiler's default warnings, and
# none of their own CMake projects is configured except libFLAC's: their tests, tools
# and install rules are not ours, and three of the four have no CMake upstream worth
# the trouble. Tags are the archive's where the archive had the dependency.

# RubberBand, offline engine only (engine/clip/Stretch.cpp). The single-file
# amalgamation needs no external FFT or resampler. GPL-2.0-or-later (FINAL_PLAN §8).
# adx-license: rubberband | Rubber Band Library | GPL-2.0-or-later | time-stretch; makes a distributed adX binary GPL
FetchContent_Declare(rubberband
    GIT_REPOSITORY https://github.com/breakfastquay/rubberband.git
    GIT_TAG v4.0.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR adx-do-not-configure
)
FetchContent_MakeAvailable(rubberband)
add_library(adx_rubberband STATIC "${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp")
target_include_directories(adx_rubberband SYSTEM PUBLIC "${rubberband_SOURCE_DIR}")
set_target_properties(adx_rubberband PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    # Its Windows thread code includes <windows.h> without NOMINMAX, which breaks
    # std::min and std::max elsewhere in the amalgamation (the archive hit this too).
    target_compile_definitions(adx_rubberband PRIVATE NOMINMAX)
endif()

# shine 3.1.1, the fixed-point MP3 encoder (LGPL-2.0). Pure C, no CMake upstream.
# adx-license: shine | shine | LGPL-2.0 | MP3 encoding; linked statically, relinkable on request
FetchContent_Declare(shine
    GIT_REPOSITORY https://github.com/toots/shine.git
    GIT_TAG 3.1.1
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR adx-do-not-configure
)
FetchContent_MakeAvailable(shine)
add_library(adx_shine STATIC
    "${shine_SOURCE_DIR}/src/lib/bitstream.c"
    "${shine_SOURCE_DIR}/src/lib/huffman.c"
    "${shine_SOURCE_DIR}/src/lib/l3bitstream.c"
    "${shine_SOURCE_DIR}/src/lib/l3loop.c"
    "${shine_SOURCE_DIR}/src/lib/l3mdct.c"
    "${shine_SOURCE_DIR}/src/lib/l3subband.c"
    "${shine_SOURCE_DIR}/src/lib/layer3.c"
    "${shine_SOURCE_DIR}/src/lib/reservoir.c"
    "${shine_SOURCE_DIR}/src/lib/tables.c"
)
target_include_directories(adx_shine SYSTEM PUBLIC "${shine_SOURCE_DIR}/src/lib")
set_target_properties(adx_shine PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    # shine annotates locals with GCC's __attribute__((unused)), which MSVC cannot
    # define from the command line; a force-included header expands it to nothing.
    target_compile_options(adx_shine PRIVATE /W0 "/FI${CMAKE_SOURCE_DIR}/cmake/shine_msvc_compat.h")
else()
    target_compile_options(adx_shine PRIVATE -w)
endif()

# libFLAC 1.4.3 (BSD-3-Clause), through its own CMake: encoder and decoder, no Ogg,
# no C++ wrapper, no programs.
# adx-license: flac | libFLAC | BSD-3-Clause | FLAC
FetchContent_Declare(flac
    GIT_REPOSITORY https://github.com/xiph/flac.git
    GIT_TAG 1.4.3
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)
set(BUILD_CXXLIBS OFF CACHE BOOL "" FORCE)
set(BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(INSTALL_MANPAGES OFF CACHE BOOL "" FORCE)
set(INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(INSTALL_PKGCONFIG_MODULES OFF CACHE BOOL "" FORCE)
set(WITH_OGG OFF CACHE BOOL "" FORCE)
set(WITH_FORTIFY_SOURCE OFF CACHE BOOL "" FORCE)
set(WITH_STACK_PROTECTOR OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(flac)
set_target_properties(FLAC PROPERTIES POSITION_INDEPENDENT_CODE ON)

# libogg 1.3.5 and libvorbis 1.3.7 (BSD-3-Clause), encode only (FINAL_PLAN §8). Built
# from their sources: vorbis's CMake insists on find_package(Ogg), which a FetchContent
# build cannot satisfy without an install step.
# adx-license: ogg | libogg | BSD-3-Clause | Ogg
FetchContent_Declare(ogg
    GIT_REPOSITORY https://github.com/xiph/ogg.git
    GIT_TAG v1.3.5
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR adx-do-not-configure
)
# adx-license: vorbis | libvorbis | BSD-3-Clause | Vorbis
FetchContent_Declare(vorbis
    GIT_REPOSITORY https://github.com/xiph/vorbis.git
    GIT_TAG v1.3.7
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR adx-do-not-configure
)
FetchContent_MakeAvailable(ogg vorbis)
# The one generated header libogg needs, written for <stdint.h> rather than probed.
set(adx_ogg_generated "${CMAKE_BINARY_DIR}/generated/ogg")
file(CONFIGURE OUTPUT "${adx_ogg_generated}/ogg/config_types.h" CONTENT [[
#ifndef __CONFIG_TYPES_H__
#define __CONFIG_TYPES_H__
#include <stdint.h>
typedef int16_t ogg_int16_t;
typedef uint16_t ogg_uint16_t;
typedef int32_t ogg_int32_t;
typedef uint32_t ogg_uint32_t;
typedef int64_t ogg_int64_t;
typedef uint64_t ogg_uint64_t;
#endif
]])
add_library(adx_vorbis STATIC
    "${ogg_SOURCE_DIR}/src/bitwise.c"
    "${ogg_SOURCE_DIR}/src/framing.c"
    "${vorbis_SOURCE_DIR}/lib/analysis.c"
    "${vorbis_SOURCE_DIR}/lib/bitrate.c"
    "${vorbis_SOURCE_DIR}/lib/block.c"
    "${vorbis_SOURCE_DIR}/lib/codebook.c"
    "${vorbis_SOURCE_DIR}/lib/envelope.c"
    "${vorbis_SOURCE_DIR}/lib/floor0.c"
    "${vorbis_SOURCE_DIR}/lib/floor1.c"
    "${vorbis_SOURCE_DIR}/lib/info.c"
    "${vorbis_SOURCE_DIR}/lib/lookup.c"
    "${vorbis_SOURCE_DIR}/lib/lpc.c"
    "${vorbis_SOURCE_DIR}/lib/lsp.c"
    "${vorbis_SOURCE_DIR}/lib/mapping0.c"
    "${vorbis_SOURCE_DIR}/lib/mdct.c"
    "${vorbis_SOURCE_DIR}/lib/psy.c"
    "${vorbis_SOURCE_DIR}/lib/registry.c"
    "${vorbis_SOURCE_DIR}/lib/res0.c"
    "${vorbis_SOURCE_DIR}/lib/sharedbook.c"
    "${vorbis_SOURCE_DIR}/lib/smallft.c"
    "${vorbis_SOURCE_DIR}/lib/synthesis.c"
    "${vorbis_SOURCE_DIR}/lib/vorbisenc.c"
    "${vorbis_SOURCE_DIR}/lib/window.c"
)
target_include_directories(adx_vorbis SYSTEM PUBLIC
    "${ogg_SOURCE_DIR}/include" "${adx_ogg_generated}" "${vorbis_SOURCE_DIR}/include")
target_include_directories(adx_vorbis PRIVATE "${vorbis_SOURCE_DIR}/lib")
set_target_properties(adx_vorbis PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_options(adx_vorbis PRIVATE /W0)
    target_compile_definitions(adx_vorbis PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(adx_vorbis PRIVATE -w)
endif()

# --- Phase 9: plugin hosting (phase_9.md §3 steps 5-9, FINAL_PLAN §8) ---------------

# CLAP 1.2.6 (MIT): header-only C ABI. Its CMake project is not configured; the
# include directory is all adX needs, for the host and for the test plugin.
# adx-license: clap | CLAP | MIT | plugin hosting
FetchContent_Declare(clap
    GIT_REPOSITORY https://github.com/free-audio/clap.git
    GIT_TAG 1.2.6
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR adx-do-not-configure
)
FetchContent_MakeAvailable(clap)
add_library(adx_clap INTERFACE)
target_include_directories(adx_clap SYSTEM INTERFACE "${clap_SOURCE_DIR}/include")

# The VST3 SDK 3.8.0 (MIT since 3.8.0), as the two of its repositories hosting needs -
# pluginterfaces and public.sdk - side by side under one root, because the SDK includes
# itself as "pluginterfaces/..." and "public.sdk/...". Not its CMake project: that one
# builds VSTGUI, the validator and the examples, and links plugins into the user's
# system VST3 folder. This is the escape hatch Phase 0 §3.4 kept for exactly this
# dependency, and it did not need vendoring (FINAL_PLAN §8).
set(adx_vst3sdk_root "${FETCHCONTENT_BASE_DIR}/vst3sdk")
# adx-license: vst3_pluginterfaces | VST 3 SDK interfaces | MIT | plugin hosting
FetchContent_Declare(vst3_pluginterfaces
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3_pluginterfaces.git
    GIT_TAG v3.8.0_build_66
    GIT_SHALLOW TRUE
    SOURCE_DIR "${adx_vst3sdk_root}/pluginterfaces"
    SOURCE_SUBDIR adx-do-not-configure
)
# adx-license: vst3_public_sdk | VST 3 public SDK | MIT | plugin hosting
FetchContent_Declare(vst3_public_sdk
    GIT_REPOSITORY https://github.com/steinbergmedia/vst3_public_sdk.git
    GIT_TAG v3.8.0_build_66
    GIT_SHALLOW TRUE
    SOURCE_DIR "${adx_vst3sdk_root}/public.sdk"
    SOURCE_SUBDIR adx-do-not-configure
)
FetchContent_MakeAvailable(vst3_pluginterfaces vst3_public_sdk)
set(adx_vst3_public "${adx_vst3sdk_root}/public.sdk/source")
add_library(adx_vst3sdk STATIC
    "${adx_vst3sdk_root}/pluginterfaces/base/coreiids.cpp"
    "${adx_vst3sdk_root}/pluginterfaces/base/funknown.cpp"
    "${adx_vst3sdk_root}/pluginterfaces/base/ustring.cpp"
    "${adx_vst3_public}/common/commoniids.cpp"
    "${adx_vst3_public}/common/commonstringconvert.cpp"
    "${adx_vst3_public}/common/memorystream.cpp"
    "${adx_vst3_public}/vst/vstinitiids.cpp"
    "${adx_vst3_public}/vst/utility/stringconvert.cpp"
    "${adx_vst3_public}/vst/hosting/eventlist.cpp"
    "${adx_vst3_public}/vst/hosting/hostclasses.cpp"
    "${adx_vst3_public}/vst/hosting/module.cpp"
    "${adx_vst3_public}/vst/hosting/module_win32.cpp"
    "${adx_vst3_public}/vst/hosting/parameterchanges.cpp"
    "${adx_vst3_public}/vst/hosting/pluginterfacesupport.cpp"
    "${adx_vst3_public}/vst/hosting/processdata.cpp"
)
target_include_directories(adx_vst3sdk SYSTEM PUBLIC "${adx_vst3sdk_root}")
target_compile_definitions(adx_vst3sdk PUBLIC
    $<$<CONFIG:Debug>:DEVELOPMENT=1> $<$<NOT:$<CONFIG:Debug>>:RELEASE=1>)
# C++17 for its own sources: module_win32.cpp passes path::u8string() where a
# std::string is wanted, which C++20's char8_t breaks. Its headers are fine in C++20.
set_target_properties(adx_vst3sdk PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_STANDARD 17)
if(MSVC)
    target_compile_options(adx_vst3sdk PRIVATE /W0)
    target_link_libraries(adx_vst3sdk PUBLIC ole32)
else()
    target_compile_options(adx_vst3sdk PRIVATE -w)
endif()

# Phase 10 (phase_10.md §4.2, FINAL_PLAN §8.2): ONNX Runtime and Spotify's basic-pitch
# model, for the melody extractor - only with ADX_ENABLE_ONNX, which is off by default,
# so a clean clone never downloads 200 MB for one feature. The prebuilt runtime, as
# iteration one used it (building it from source is impractical here); the model is
# one file, pinned to the basic-pitch release it shipped in.
if(ADX_ENABLE_ONNX)
    # adx-license: onnxruntime | ONNX Runtime | MIT | optional melody extractor
    FetchContent_Declare(onnxruntime
        URL https://github.com/microsoft/onnxruntime/releases/download/v1.27.1/onnxruntime-win-x64-1.27.1.zip
    )
    # adx-license: basicpitch_model | Basic Pitch model | Apache-2.0 | optional melody extractor
    FetchContent_Declare(basicpitch_model
        URL https://raw.githubusercontent.com/spotify/basic-pitch/v0.4.0/basic_pitch/saved_models/icassp_2022/nmp.onnx
        DOWNLOAD_NO_EXTRACT TRUE
    )
    FetchContent_MakeAvailable(onnxruntime basicpitch_model)
    add_library(adx_onnxruntime SHARED IMPORTED GLOBAL)
    set_target_properties(adx_onnxruntime PROPERTIES
        IMPORTED_LOCATION "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll"
        IMPORTED_IMPLIB "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_SOURCE_DIR}/include"
    )
    set(ADX_ONNX_RUNTIME_DLL "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll")
    set(ADX_ONNX_MODEL "${basicpitch_model_SOURCE_DIR}/nmp.onnx")
endif()

# --- Phase 11.5: the dx7 instrument's engine (phase_11.5.md §4.1, FINAL_PLAN §8) -------
#
# msfa, Google's music-synthesizer-for-android: the engine the M-Vave FM-1 runs and
# Dexed is built on. adX ports its core into engine/instruments/dx7/msfa/ (Apache
# headers kept, changes marked), so the shipped binary contains msfa's code and this
# line puts its notice in THIRD-PARTY. The pinned upstream tree itself is compiled only
# into the tests, unmodified, as dx7_engine_matches_msfa's oracle. Pinned to a commit:
# upstream has no release tags.
# adx-license: msfa | msfa (music-synthesizer-for-android) | Apache-2.0 | the dx7 instrument (ported); its test oracle
FetchContent_Declare(msfa
    GIT_REPOSITORY https://github.com/google/music-synthesizer-for-android.git
    GIT_TAG f67d41d313b7dc85f6fb99e79e515cc9d208cfff
    SOURCE_SUBDIR adx-do-not-configure
)
if(ADX_BUILD_TESTS)
    FetchContent_MakeAvailable(msfa)
    set(adx_msfa_src "${msfa_SOURCE_DIR}/app/src/main/jni")
    add_library(adx_msfa_upstream STATIC
        "${adx_msfa_src}/dx7note.cc"
        "${adx_msfa_src}/env.cc"
        "${adx_msfa_src}/exp2.cc"
        "${adx_msfa_src}/fm_core.cc"
        "${adx_msfa_src}/fm_op_kernel.cc"
        "${adx_msfa_src}/freqlut.cc"
        "${adx_msfa_src}/lfo.cc"
        "${adx_msfa_src}/patch.cc"
        "${adx_msfa_src}/pitchenv.cc"
        "${adx_msfa_src}/sin.cc"
    )
    target_include_directories(adx_msfa_upstream SYSTEM PUBLIC "${adx_msfa_src}")
    target_compile_definitions(adx_msfa_upstream PRIVATE _USE_MATH_DEFINES)
    if(MSVC)
        # msfa_compat.h gives synth.h its memory barrier (or it #warns) before any
        # upstream file is read; nothing in the upstream sources is edited.
        target_compile_options(adx_msfa_upstream PRIVATE /W0
            "/FI${CMAKE_SOURCE_DIR}/cmake/msfa_compat.h")
    else()
        target_compile_options(adx_msfa_upstream PRIVATE -w
            -include "${CMAKE_SOURCE_DIR}/cmake/msfa_compat.h")
    endif()
endif()

# Every dependency names its licence (phase_10.md 4.8): an "# adx-license:" line right
# above each FetchContent_Declare, from which tools/gen_third_party.py writes
# installer/THIRD-PARTY.md. One without fails the configure, here.
file(STRINGS "${CMAKE_CURRENT_LIST_FILE}" adx_dependency_lines)
set(adx_previous_line "")
foreach(adx_line IN LISTS adx_dependency_lines)
    # A quoted argument unescapes "\(" to "(", so the regex needs "\\(".
    if(adx_line MATCHES "^[ \t]*FetchContent_Declare\\(([A-Za-z0-9_]+)")
        set(adx_declared "${CMAKE_MATCH_1}")
        if(NOT adx_previous_line MATCHES "^[ \t]*# adx-license: ${adx_declared} \\|")
            message(FATAL_ERROR "${adx_declared}: no '# adx-license: ${adx_declared} | name | SPDX | use' line above its FetchContent_Declare")
        endif()
    endif()
    set(adx_previous_line "${adx_line}")
endforeach()
