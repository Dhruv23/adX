# Audio file decode and the sample pool (phase_4.md §4.11).
target_sources(adx_engine_static PRIVATE
    AudioFileLoader.cpp
    SamplePool.cpp
)
target_link_libraries(adx_engine_static PRIVATE adx_miniaudio)
