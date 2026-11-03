target_sources(adx_engine_static PRIVATE
    ClipEdit.cpp
    ClipRenderCache.cpp
    Destructive.cpp
    Freeze.cpp
    Stretch.cpp
)
target_link_libraries(adx_engine_static PRIVATE adx_rubberband)
