#pragma once

#include "AudioData.h"
#include <optional>
#include <string>
#include <vector>

// Algorithmic (non-ML) track extraction from Standard MIDI Files (.mid),
// the deterministic fallback/complement to MelodyExtractor's ONNX path.
// Pure in-memory parsing — no external MIDI library, safe to call from a
// background std::thread (touches nothing but its arguments).
namespace MidiImporter {

    // One playable track recovered from the file: SMF format 1 gives one per
    // MTrk chunk; a multi-channel chunk (typical format 0) is split per
    // channel so different instruments don't collapse into one piano roll.
    struct ImportedTrack {
        std::string name;        // MTrk name meta event, or "MIDI Ch N"
        bool isPercussion = false; // MIDI channel 10 — kept, but flagged in the name
        std::vector<Note> notes; // beats derived exactly: tick / division
    };

    struct Result {
        std::vector<ImportedTrack> tracks;
        float bpm = 0.0f; // first Set Tempo meta event; 0 = none in file
    };

    // MIDI ticks are defined in quarter notes, so tick/division -> beats is
    // exact — no tempo guesswork, unlike the ML path. The file's tempo only
    // matters for playback speed and is returned as Result::bpm for the
    // caller to apply to the project. SMPTE-division files are rejected.
    // Returns std::nullopt on unreadable/malformed files (reason on stderr).
    std::optional<Result> ImportFile(const std::string& filePath);
}
