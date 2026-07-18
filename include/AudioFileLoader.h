#pragma once

#include "AudioData.h"
#include <optional>
#include <string>
#include <vector>

// Decodes an audio file (WAV/MP3) on the calling thread (Main thread only —
// never call this from AudioEngine::process()) into an AudioClip whose PCM
// data is interleaved stereo f32 at kEngineSampleRate, ready to be summed
// directly by the audio engine. Returns std::nullopt on failure (missing
// file, unsupported/corrupt format); logs the reason to stderr.
namespace AudioFileLoader {
    std::optional<AudioClip> LoadAudioClip(const std::string& filePath, float startTimeSeconds);

    // Decodes filePath to mono f32 @ targetSampleRate (e.g. 22050 for
    // basic-pitch ML analysis). Separate from LoadAudioClip, which is
    // hardcoded to stereo @ kEngineSampleRate for playback — this is for
    // analysis input, not engine mixing. Never call from AudioEngine::process();
    // otherwise safe from the Main Thread or a background std::thread (each
    // call opens its own independent miniaudio decoder instance) — this is
    // called from MelodyExtractor::ExtractMelody's background thread.
    std::optional<std::vector<float>> DecodeMono(const std::string& filePath, unsigned int targetSampleRate);
}
