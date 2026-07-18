#pragma once

#include "AudioData.h"
#include <onnxruntime_cxx_api.h>
#include <optional>
#include <string>
#include <vector>
#include <memory>

// Loads Spotify's basic-pitch ONNX model (nmp.onnx, expected next to the
// executable) and runs audio-to-MIDI melody extraction. This is heavy,
// potentially multi-second work — call from the Main Thread or a background
// std::thread, never from AudioEngine::process().
class MelodyExtractor {
public:
    MelodyExtractor();

    // Synchronous. bpm converts detected note times (seconds) to beats using
    // the CURRENT project tempo, not the source song's actual tempo — there is
    // no tempo detection here (see Phase 4's plan for why). Returns
    // std::nullopt on failure (decode error, missing/invalid model).
    std::optional<std::vector<Note>> ExtractMelody(const std::string& filePath, float bpm);

private:
    bool EnsureSessionLoaded();

    Ort::Env m_env;
    std::unique_ptr<Ort::Session> m_session;
    bool m_sessionLoadAttempted = false;
};
