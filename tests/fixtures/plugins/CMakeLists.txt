# adx_test_plugin (phase_9.md §4.8): a CLAP and a VST3 plugin adX builds itself, so the
# sandbox's failure handling is tested against plugins whose misbehaviour is chosen,
# not hoped for. Both go to one directory the tests find through a compile definition.
set(adx_fixture_plugins "${CMAKE_BINARY_DIR}/fixtures/plugins")

add_library(adx_test_plugin_clap MODULE ClapTestPlugin.cpp)
target_link_libraries(adx_test_plugin_clap PRIVATE adx_clap)
target_compile_features(adx_test_plugin_clap PRIVATE cxx_std_20)

add_library(adx_test_plugin_vst3 MODULE Vst3TestPlugin.cpp)
target_link_libraries(adx_test_plugin_vst3 PRIVATE adx_vst3sdk)
target_compile_features(adx_test_plugin_vst3 PRIVATE cxx_std_20)

foreach(target adx_test_plugin_clap adx_test_plugin_vst3)
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${adx_fixture_plugins}"
        LIBRARY_OUTPUT_DIRECTORY_DEBUG "${adx_fixture_plugins}"
        LIBRARY_OUTPUT_DIRECTORY_RELEASE "${adx_fixture_plugins}"
        LIBRARY_OUTPUT_DIRECTORY_RELWITHDEBINFO "${adx_fixture_plugins}"
        RUNTIME_OUTPUT_DIRECTORY "${adx_fixture_plugins}")
    if(MSVC)
        # Test scaffolding with deliberate faults in it: not held to adX's warnings.
        target_compile_options(${target} PRIVATE /W3)
    endif()
endforeach()
# A name each: one directory, and their import libraries and debug databases must not
# collide. The tests find them by target path.
set_target_properties(adx_test_plugin_clap PROPERTIES OUTPUT_NAME adx_test_plugin SUFFIX ".clap")
set_target_properties(adx_test_plugin_vst3 PROPERTIES OUTPUT_NAME adx_test_plugin_vst3 SUFFIX ".vst3")
