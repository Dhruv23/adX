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
FetchContent_Declare(pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG v2.13.6
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)

# Phase 1: real audio devices. Same tag iteration one used and shipped on.
FetchContent_Declare(rtaudio
    GIT_REPOSITORY https://github.com/thestk/rtaudio.git
    GIT_TAG 6.0.1
    GIT_SHALLOW TRUE
    SYSTEM
    EXCLUDE_FROM_ALL
)

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
# ASIO is Phase 9. It is a device-API flag on the same backend class, not a new
# backend, so enabling it later is one line here plus a StreamConfig field.
set(RTAUDIO_API_ASIO OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rtaudio)

if(ADX_BUILD_TESTS)
    FetchContent_MakeAvailable(Catch2)
endif()
