#pragma once

#include "AudioData.h"
#include <string>

class AdxParser {
public:
    static bool LoadProject(const std::string& filepath, SequencerState& state, std::string& outFirstPatchName);
    static bool SaveProject(const std::string& filepath, const SequencerState& state);

private:
    static uint8_t NoteNameToMidi(std::string_view noteName);
    static std::string MidiToNoteName(uint8_t midiNote);
};