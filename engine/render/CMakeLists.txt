target_sources(adx_engine_static PRIVATE
    Bundle.cpp
    Dither.cpp
    Encoders.cpp
    ExportJob.cpp
    FlacEncoder.cpp
    Mp3Encoder.cpp
    OfflineRender.cpp
    OggEncoder.cpp
    RenderEngine.cpp
    RenderHash.cpp
    RenderWorkers.cpp
    StemRender.cpp
    WavEncoder.cpp
)
# The export encoders (phase_8.md §4): shine, libFLAC, libvorbis + libogg.
target_link_libraries(adx_engine_static PRIVATE adx_shine FLAC adx_vorbis)
