cmake_minimum_required(VERSION 3.26)

# The version lives here and nowhere else. It reaches C++ through
# engine/core/Version.h.in, the wheel through scikit-build-core's regex metadata
# provider reading this exact line, and Python through adx_engine.version().
# tests/python/test_import.py::test_version_agrees turns any divergence into a
# CI failure (phase_0.md §3.6).
project(adx
    VERSION 0.1.0
    DESCRIPTION "adX - a music production environment whose project format is text"
    HOMEPAGE_URL "https://github.com/Dhruv23/adX"
    LANGUAGES CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Set before any dependency is populated, and deliberately global rather than
# per-target: the language standard is not a style preference, it is part of the
# ABI. Catch2 declares its own sources as cxx_std_14, which means _MSVC_LANG reads
# 201402 while it compiles, which means it never emits
# StringMaker<std::string_view>::convert - and a C++20 test TU that compares a
# string_view then fails to link with LNK2019. Warnings stay per-target
# (AdxWarnings.cmake); the standard cannot.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(AdxOptions)
include(AdxWarnings)
include(AdxDependencies)

# Generated headers are reachable by the same repo-root-relative include spelling
# as checked-in ones, so `#include "engine/core/Version.h"` does not care whether
# the file was written by hand or by configure_file (phase_0.md §3.2).
set(ADX_GENERATED_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated")

add_subdirectory(engine)

if(ADX_BUILD_BINDINGS)
    add_subdirectory(bindings)
endif()

if(ADX_BUILD_TESTS)
    enable_testing()
    include(AdxTesting)
    add_subdirectory(tests/cpp)
endif()

adx_print_configuration_summary()
