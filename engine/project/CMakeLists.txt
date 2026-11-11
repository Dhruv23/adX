target_sources(adx_engine_static PRIVATE
    Automation.cpp
    AutomationWriter.cpp
    Channel.cpp
    Controller.cpp
    ControllerCompile.cpp
    CurveCompile.cpp
    Diff.cpp
    EventCompile.cpp
    Macro.cpp
    MacroCompile.cpp
    MidiSettings.cpp
    ParamRegistry.cpp
    Project.cpp
    Rack.cpp
    Snapshot.cpp
    SnapshotBuilder.cpp
    TypeCatalog.cpp
    Validate.cpp
)

add_subdirectory(commands)
