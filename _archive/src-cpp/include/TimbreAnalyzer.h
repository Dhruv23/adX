#pragma once

#include "AudioData.h"
#include "MidiImporter.h"
#include <optional>
#include <string>
#include <vector>

// Derives an additive-synth Patch from a reference mixdown (mp3/wav) of the
// same song a .mid was imported from. The MIDI notes tell us exactly WHERE
// and at WHAT pitch the synth is sounding, so instead of blind ML timbre
// guessing this measures the mix directly: per-note Goertzel probes at the
// first 16 harmonic multiples of each note's known fundamental (-> timbre
// keyframes), plus an RMS-envelope fit around note boundaries (-> ADSR).
// Deliberately effect-agnostic: it samples mid-sustain windows and medians
// across many notes, so reverb tails / delay repeats average out instead of
// polluting the estimate.
//
// Heavy (full-file decode + analysis): call from a background std::thread,
// never the audio thread. Returns std::nullopt if the audio can't be decoded
// or no note yields a usable measurement; the returned Patch carries built
// envelope tables and is immediately playable.
namespace TimbreAnalyzer {
    std::optional<Patch> AnalyzePatch(const std::string& audioPath,
                                      const std::vector<MidiImporter::ImportedTrack>& tracks,
                                      float bpm,
                                      float tuningA4);
}
