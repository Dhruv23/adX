pybind11_add_module(adx_engine MODULE
    module.cpp
    audio.cpp
    format.cpp
    geometry.cpp
    meters.cpp
    notes.cpp
    project.cpp
    render.cpp
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
