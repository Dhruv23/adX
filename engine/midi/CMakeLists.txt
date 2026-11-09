target_sources(adx_engine_static PRIVATE
    DeviceNames.cpp
    LiveOutputs.cpp
    MidiClock.cpp
    MidiDevices.cpp
    MidiLearn.cpp
    MidiInput.cpp
    MidiRouter.cpp
    MidiSync.cpp
    PadController.cpp
    PadFeedback.cpp
    dx7/Dx7Codec.cpp
    dx7/Dx7ParamChange.cpp
    sysex/SysexAssembler.cpp
)

# PRIVATE: RtMidi is an implementation detail of MidiDevices.cpp, as RtAudio is of
# RtAudioBackend.cpp.
target_link_libraries(adx_engine_static PRIVATE rtmidi)
