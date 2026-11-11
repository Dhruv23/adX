pybind11_add_module(adx_engine MODULE
    module.cpp
    analysis.cpp
    analyzers.cpp
    audio.cpp
    capture.cpp
    dx7.cpp
    edit.cpp
    export.cpp
    format.cpp
    generate.cpp
    live.cpp
    pads.cpp
    geometry.cpp
    meters.cpp
    midi.cpp
    mini.cpp
    notes.cpp
    plugin.cpp
    panels.cpp
    project.cpp
    racks.cpp
    record.cpp
    render.cpp
    script.cpp
    session.cpp
    structure.cpp
    sync.cpp
    transport.cpp
)
target_link_libraries(adx_engine PRIVATE adx::engine)
adx_set_warnings(adx_engine)

# scikit-build-core installs whatever CMake installs into the wheel root, so the
# module lands beside the adx/ package and imports as a top-level `adx_engine`
# (phase_0.md §3.6).
install(TARGETS adx_engine DESTINATION .)

# The scene-graph plugin. Optional: it needs a Qt C++ SDK (bindings/qml/CMakeLists.txt).
add_subdirectory(qml)
