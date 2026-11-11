# The project format, and audio decode beside it (Phase 4), and MIDI files (Phase 8).
# The Phase 8 encoders went to engine/render/, beside the export job that drives them.
add_subdirectory(adx)
add_subdirectory(mininotation)
add_subdirectory(audio)
add_subdirectory(text)
add_subdirectory(midi)
target_sources(adx_engine_static PRIVATE
    sfz/SfzParser.cpp
    soundfont/DlsParser.cpp
    soundfont/Generators.cpp
    soundfont/Riff.cpp
    soundfont/Sf2Parser.cpp
)
