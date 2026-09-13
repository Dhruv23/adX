target_sources(adx_engine_static PRIVATE
    AudioThread.cpp
    NullBackend.cpp
    OfflineBackend.cpp
    RtAudioBackend.cpp
)

# PRIVATE: RtAudio is an implementation detail of RtAudioBackend.cpp and nothing
# else in the engine, or above it, may see its headers. That is what keeps
# windows.h out of the rest of the tree (FINAL_PLAN non-goals: the engine stays
# portable C++20 so macOS/Linux is later work, not a rewrite).
target_link_libraries(adx_engine_static PRIVATE rtaudio)
