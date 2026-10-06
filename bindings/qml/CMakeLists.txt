# adx_quick: the C++ QQuickItems the piano roll and waveform view draw with
# (phase_5.md §4.3), built as a plain DLL that Python loads into PySide6's Qt.
#
# It needs a Qt C++ SDK of exactly PySide6's version, because PySide6 ships Qt's DLLs
# but not its headers or import libraries: `python tools/fetch_qt.py` puts one in
# .qt/<version>/, which is searched first. Without an SDK the target is skipped with a
# warning and the GUI reports, at start-up, that the plugin was not built; the engine,
# the bindings and the CLI do not need it.
#
# PySide6 ships only release Qt, so the plugin always links release Qt and the release
# CRT - in every configuration, Debug included. It shares no C++ objects with the
# engine (vertices cross as raw floats), which is what makes that mismatch harmless.

set(ADX_QT_VERSION "6.11.2" CACHE STRING "The Qt (and PySide6) version adx_quick builds against")

set(adx_qt_prefix "${CMAKE_SOURCE_DIR}/.qt/${ADX_QT_VERSION}")
if(EXISTS "${adx_qt_prefix}/lib/cmake/Qt6")
    list(PREPEND CMAKE_PREFIX_PATH "${adx_qt_prefix}")
endif()

# Every configuration consumes Qt's release libraries (see above). Set before
# find_package: the variables initialise the imported targets it creates.
set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG Release)
set(CMAKE_MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
find_package(Qt6 ${ADX_QT_VERSION} EXACT QUIET COMPONENTS Quick Qml)

if(NOT Qt6Quick_FOUND)
    message(WARNING
        "adx_quick not built: no Qt ${ADX_QT_VERSION} SDK found. Run `python tools/fetch_qt.py` "
        "(phase_5.md §4.3). The engine, bindings and CLI are unaffected.")
    set(ADX_HAVE_QUICK OFF PARENT_SCOPE)
    return()
endif()
set(ADX_HAVE_QUICK ON PARENT_SCOPE)

add_library(adx_quick SHARED
    AdxQuickPlugin.cpp
    PianoRollItem.cpp
    SceneLayers.cpp
    WaveformItem.cpp
)
set_target_properties(adx_quick PROPERTIES
    AUTOMOC ON
    MSVC_RUNTIME_LIBRARY MultiThreadedDLL
    # One name in every configuration: a dependency sets a global "d" debug postfix,
    # and Python looks for adx_quick.dll.
    DEBUG_POSTFIX ""
)
target_link_libraries(adx_quick PRIVATE Qt6::Quick Qt6::Qml)
target_include_directories(adx_quick PRIVATE "${CMAKE_SOURCE_DIR}")
target_compile_definitions(adx_quick PRIVATE QT_NO_CAST_FROM_ASCII)
adx_set_warnings(adx_quick)

# Beside adx_engine in the wheel root; engine_bridge finds it there.
install(TARGETS adx_quick RUNTIME DESTINATION . LIBRARY DESTINATION .)
