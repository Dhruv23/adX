target_sources(adx_engine_static PRIVATE
    Automation.cpp
    Channel.cpp
    Controller.cpp
    ControllerCompile.cpp
    CurveCompile.cpp
    Diff.cpp
    EventCompile.cpp
    ParamRegistry.cpp
    Project.cpp
    Snapshot.cpp
    SnapshotBuilder.cpp
    TypeCatalog.cpp
    Validate.cpp
)

add_subdirectory(commands)
