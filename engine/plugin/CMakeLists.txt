target_sources(adx_engine_static PRIVATE
    Bridge/AudioLink.cpp
    Bridge/EditorWindow.cpp
    Bridge/HostProcess.cpp
    Bridge/Platform.cpp
    Bridge/Protocol.cpp
    clap/ClapModule.cpp
    vst3/Vst3Module.cpp
    Json.cpp
    Module.cpp
    PluginBinding.cpp
    PluginCache.cpp
    PluginCore.cpp
    PluginDescriptor.cpp
    PluginHost.cpp
    PluginNode.cpp
    ProjectSync.cpp
    PluginInstrument.cpp
    Scanner.cpp
    StateBlob.cpp
)

# PRIVATE: the formats' headers are the sandbox's business. Nothing outside
# engine/plugin includes them.
target_link_libraries(adx_engine_static PRIVATE adx_clap adx_vst3sdk ole32 user32)

# The sandbox child (phase_9.md §4.2, §4.3): the one executable engine/ builds besides
# the library, because a plugin must never be loaded into adX's own process. It links
# the engine for the plugin modules and the protocol; the linker keeps only what they
# reach.
add_executable(adx_plugin_host Bridge/ChildMain.cpp)
target_link_libraries(adx_plugin_host PRIVATE adx::engine adx_clap adx_vst3sdk ole32 user32)
adx_set_warnings(adx_plugin_host)
# A console subsystem: the protocol is its stdin and stdout. The window it would open
# is suppressed by CREATE_NO_WINDOW at spawn.

if(ADX_BUILD_BINDINGS)
    # Beside adx_engine.pyd in the wheel, where PluginHost looks for it.
    install(TARGETS adx_plugin_host DESTINATION .)
endif()
