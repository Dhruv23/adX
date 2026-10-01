set(adx_version_header "${ADX_GENERATED_INCLUDE_DIR}/engine/core/Version.h")
set(adx_git_stamp_script "${CMAKE_SOURCE_DIR}/cmake/AdxGitStamp.cmake")

# Stamped at build time rather than configure time, so the commit recorded in the
# binary is the one being built rather than whichever was checked out when CMake
# last ran (AdxGitStamp.cmake says why that matters).
#
# An always-run custom target rather than an OUTPUT rule: the whole point is to
# re-read HEAD on every build, so there is no input whose timestamp could decide
# it. The cost of running it unconditionally is one git invocation; the cost of
# *rebuilding* unconditionally is avoided inside the script, where configure_file
# rewrites the header only when its content actually differs.
add_custom_target(adx_version_stamp
    BYPRODUCTS "${adx_version_header}"
    COMMAND "${CMAKE_COMMAND}"
        -D "ADX_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        -D "ADX_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/Version.h.in"
        -D "ADX_OUTPUT=${adx_version_header}"
        -D "adx_VERSION=${PROJECT_VERSION}"
        -D "adx_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}"
        -D "adx_VERSION_MINOR=${PROJECT_VERSION_MINOR}"
        -D "adx_VERSION_PATCH=${PROJECT_VERSION_PATCH}"
        -P "${adx_git_stamp_script}"
    COMMENT "Stamping engine/core/Version.h with the current commit"
    VERBATIM
)
add_dependencies(adx_engine_static adx_version_stamp)

# The header has to exist before the first compile, and a target dependency only
# orders the *build*, not the configure. Generate it once here so a fresh tree can
# be configured and then compiled by something that is not Ninja.
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -D "ADX_SOURCE_DIR=${CMAKE_SOURCE_DIR}"
        -D "ADX_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/Version.h.in"
        -D "ADX_OUTPUT=${adx_version_header}"
        -D "adx_VERSION=${PROJECT_VERSION}"
        -D "adx_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}"
        -D "adx_VERSION_MINOR=${PROJECT_VERSION_MINOR}"
        -D "adx_VERSION_PATCH=${PROJECT_VERSION_PATCH}"
        -P "${adx_git_stamp_script}"
    RESULT_VARIABLE adx_stamp_result
)
if(NOT adx_stamp_result EQUAL 0)
    message(FATAL_ERROR "Failed to generate ${adx_version_header}")
endif()

target_sources(adx_engine_static PRIVATE
    Config.cpp
    Curve.cpp
    TempoMap.cpp
    TempoMath.cpp
    Version.cpp
)
