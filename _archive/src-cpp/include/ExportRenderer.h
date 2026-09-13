#pragma once

#include "AudioData.h"
#include <atomic>
#include <string>
#include <unordered_map>
#include <vector>

// Offline audio export (Phase 5+ "Export tab"). Renders the project through a
// FRESH AudioEngine instance — the exact same DSP code path the realtime
// stream uses (voices, per-track effects, arp, master delay/reverb/sidechain/
// drive, compressor) — into a float buffer, then encodes it to disk.
//
// Heavy work: run on the Main Thread or a background std::thread, never from
// the realtime callback. The request must carry DEEP-COPIED tracks whose
// effects were clone()d (see BuildRequest), because the live audio thread is
// still processing the originals concurrently.
namespace ExportRenderer {

enum class Format {
    Wav16,      // WAV, 16-bit PCM
    Wav24,      // WAV, 24-bit PCM
    WavFloat32, // WAV, 32-bit IEEE float
    Mp3,        // MP3, 320 kbps CBR (shine encoder)
    Flac,       // FLAC, 16-bit lossless
};

const char* FormatLabel(Format f);
const char* FormatExtension(Format f); // ".wav", ".mp3", ".flac"

struct Request {
    std::vector<Track> tracks; // effects deep-cloned, never aliasing live instances
    std::unordered_map<std::string, Patch> patches; // the full registry — tracks resolve by name (Phase 1)
    std::vector<AutomationLane> automation; // Phase 2
    float bpm = 120.0f;
    float masterVolume = 0.75f;
    float tuning = 440.0f;
    MasterFxSettings masterFx;
    Format format = Format::Wav16;
    std::string outputPath;
    float tailSeconds = 3.0f;  // rendered after the last event for release/reverb/delay tails
};

struct Result {
    bool success = false;
    std::string message;       // human-readable outcome ("Wrote 1.2 MB to x.mp3" / error)
};

// Snapshot of the live state safe to hand to a background render thread
// (deep-copies tracks and clones every effect). Main Thread only.
Request BuildRequest(const SequencerState& state,
                     Format format, const std::string& outputPath, float tailSeconds);

// Length of the project's audible content in seconds (notes + clips, no tail).
float EstimateContentSeconds(const std::vector<Track>& tracks, float bpm);

// Renders and encodes synchronously. `progress` (0..1), if non-null, is
// updated as rendering advances so a UI thread can display it.
Result Render(const Request& request, std::atomic<float>* progress);

}
