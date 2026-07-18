#include "MelodyExtractor.h"
#include "AudioFileLoader.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>

#ifdef _WIN32
#define NOMINMAX // consistent with the rest of the project; must precede <windows.h>
#include <windows.h>
#endif

namespace {
    // "nmp.onnx" lives next to the executable (copied there by CMakeLists.txt's
    // post-build step), not necessarily in the process's current working
    // directory — e.g. launching AudioSequencer.exe from a shell whose cwd is
    // the parent build/ directory, not build/Debug/, makes a bare relative
    // "nmp.onnx" open() fail. Resolve it against the executable's own path instead.
    std::string GetModelPath() {
#ifdef _WIN32
        char buffer[MAX_PATH];
        DWORD len = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::string exePath(buffer, len);
            size_t pos = exePath.find_last_of("\\/");
            if (pos != std::string::npos) {
                return exePath.substr(0, pos + 1) + "nmp.onnx";
            }
        }
#endif
        return "nmp.onnx"; // fallback: cwd-relative (also the only option on non-Windows for now)
    }

    // basic-pitch model constants, verified directly against Spotify's
    // basic_pitch/constants.py and inference.py (not from memory) — see
    // Phase 4's plan for the exact source lines these were pulled from.
    constexpr unsigned int kBasicPitchSampleRate = 22050;
    constexpr int kFftHop = 256;
    constexpr int kOverlappingFrames = 30; // DEFAULT_OVERLAPPING_FRAMES
    constexpr size_t kAudioNSamples = kBasicPitchSampleRate * 2 - kFftHop; // AUDIO_WINDOW_LENGTH=2s; = 43844
    constexpr int kAnnotationsFps = kBasicPitchSampleRate / kFftHop; // = 86
    constexpr int kNumPitchBins = 88; // ANNOTATIONS_N_SEMITONES
    constexpr int kMidiBase = 21; // A0, ANNOTATIONS_BASE_FREQUENCY's MIDI number
    constexpr float kOnsetThreshold = 0.5f;  // DEFAULT_ONSET_THRESHOLD
    constexpr float kFrameThreshold = 0.3f;  // DEFAULT_FRAME_THRESHOLD
    constexpr int kMinNoteLenFrames = 11;    // round(127.7ms * 86fps / 1000)
    constexpr const char* kModelInputName = "serving_default_input_2:0";
    // Requesting outputs in this exact order returns [note, onset, contour] —
    // confirmed from basic_pitch/inference.py's actual zip(["note","onset","contour"], session.run([...], ...)).
    constexpr std::array<const char*, 3> kModelOutputNames = {
        "StatefulPartitionedCall:1", "StatefulPartitionedCall:2", "StatefulPartitionedCall:0"
    };
}

MelodyExtractor::MelodyExtractor() : m_env(ORT_LOGGING_LEVEL_WARNING, "MelodyExtractor") {}

bool MelodyExtractor::EnsureSessionLoaded() {
    if (m_session) return true;
    if (m_sessionLoadAttempted) return false; // already failed once; don't retry every call

    m_sessionLoadAttempted = true;

    std::string modelPath = GetModelPath();
    std::ifstream file(modelPath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "MelodyExtractor: could not open " << modelPath << "\n";
        return false;
    }
    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::vector<char> modelBytes(static_cast<size_t>(size));
    if (size <= 0 || !file.read(modelBytes.data(), size)) {
        std::cerr << "MelodyExtractor: failed to read nmp.onnx\n";
        return false;
    }

    try {
        Ort::SessionOptions options;
        m_session = std::make_unique<Ort::Session>(m_env, modelBytes.data(), modelBytes.size(), options);
    } catch (const Ort::Exception& e) {
        std::cerr << "MelodyExtractor: failed to create ONNX session: " << e.what() << "\n";
        m_session.reset();
        return false;
    }
    return true;
}

std::optional<std::vector<Note>> MelodyExtractor::ExtractMelody(const std::string& filePath, float bpm) {
    if (!EnsureSessionLoaded()) return std::nullopt;

    auto monoOpt = AudioFileLoader::DecodeMono(filePath, kBasicPitchSampleRate);
    if (!monoOpt || monoOpt->empty()) return std::nullopt;
    const std::vector<float>& mono = *monoOpt;

    // This runs on a background std::thread (see main.cpp's "EXTRACT MELODY..."
    // handler) with no caller-side try/catch — an uncaught Ort::Exception here
    // would propagate out of the thread's entry point and call std::terminate(),
    // crashing the whole app instead of surfacing failure via the returned
    // std::nullopt the rest of this class's contract relies on.
    try {
        // Accumulate full-length note/onset activation matrices across overlapping
        // windows, trimming kOverlappingFrames off the start of each non-first
        // window before concatenating (see the loop below for why only the start).
        std::vector<std::vector<float>> noteMatrix;  // [frame][pitchBin]
        std::vector<std::vector<float>> onsetMatrix;

        const size_t hopSize = kAudioNSamples - static_cast<size_t>(kOverlappingFrames) * kFftHop;
        Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        for (size_t windowStart = 0; windowStart < mono.size(); windowStart += hopSize) {
            std::vector<float> window(kAudioNSamples, 0.0f); // zero-padded if this is the final, partial window
            size_t available = std::min(kAudioNSamples, mono.size() - windowStart);
            std::copy(mono.begin() + windowStart, mono.begin() + windowStart + available, window.begin());

            std::array<int64_t, 3> inputShape = { 1, static_cast<int64_t>(kAudioNSamples), 1 };
            Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                memInfo, window.data(), window.size(), inputShape.data(), inputShape.size());

            Ort::RunOptions runOptions; // default-constructed (NOT RunOptions(nullptr), which is deliberately an empty/invalid placeholder)
            auto outputs = m_session->Run(runOptions, &kModelInputName, &inputTensor, 1,
                                           kModelOutputNames.data(), kModelOutputNames.size());
            // outputs[0]=note, outputs[1]=onset, outputs[2]=contour (contour/pitch-bend discarded — see Phase 4's plan)

            auto noteShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape(); // (1, frames, 88)
            auto onsetShape = outputs[1].GetTensorTypeAndShapeInfo().GetShape();
            if (noteShape.size() != 3 || onsetShape.size() != 3 ||
                noteShape[1] != onsetShape[1] || noteShape[2] != onsetShape[2]) {
                std::cerr << "MelodyExtractor: unexpected/mismatched model output shape\n";
                return std::nullopt;
            }
            int64_t frames = noteShape[1];
            int64_t bins = noteShape[2];
            const float* notePtr = outputs[0].GetTensorData<float>();
            const float* onsetPtr = outputs[1].GetTensorData<float>();

            bool isFirstWindow = (windowStart == 0);
            bool isLastWindow = (windowStart + kAudioNSamples >= mono.size());

            // hopSize already advances by (kAudioNSamples - overlap), so trimming
            // only the START of non-first windows (by overlap frames) exactly
            // abuts the previous window's un-trimmed end — no gap, no duplication.
            // (Trimming the END too, as an earlier version of this code did,
            // double-subtracts the overlap and leaves a dropped-notes gap between
            // every pair of windows.)
            int64_t startFrame = isFirstWindow ? 0 : std::min<int64_t>(kOverlappingFrames, frames);
            int64_t endFrame = frames;

            for (int64_t f = startFrame; f < endFrame; ++f) {
                const float* noteRowStart = notePtr + f * bins;
                const float* onsetRowStart = onsetPtr + f * bins;
                noteMatrix.emplace_back(noteRowStart, noteRowStart + bins);
                onsetMatrix.emplace_back(onsetRowStart, onsetRowStart + bins);
            }

            if (isLastWindow) break;
        }

        // Postprocess: simplified per-pitch-bin onset-triggered/frame-sustained
        // extraction (not a byte-for-byte port of basic-pitch's polyphonic
        // disambiguation algorithm — see Phase 4's plan for why).
        std::vector<Note> notes;
        const size_t totalFrames = noteMatrix.size();
        const int numBins = totalFrames > 0 ? static_cast<int>(noteMatrix[0].size()) : 0;

        for (int bin = 0; bin < std::min(numBins, kNumPitchBins); ++bin) {
            size_t frameIdx = 0;
            while (frameIdx < totalFrames) {
                if (onsetMatrix[frameIdx][bin] > kOnsetThreshold) {
                    size_t noteStart = frameIdx;
                    float peakActivation = noteMatrix[frameIdx][bin];
                    size_t f = frameIdx + 1;
                    while (f < totalFrames && noteMatrix[f][bin] > kFrameThreshold) {
                        peakActivation = std::max(peakActivation, noteMatrix[f][bin]);
                        ++f;
                    }
                    size_t noteEnd = f; // exclusive
                    size_t lengthFrames = noteEnd - noteStart;

                    if (lengthFrames >= static_cast<size_t>(kMinNoteLenFrames)) {
                        float startSeconds = static_cast<float>(noteStart) / static_cast<float>(kAnnotationsFps);
                        float lengthSeconds = static_cast<float>(lengthFrames) / static_cast<float>(kAnnotationsFps);

                        Note n;
                        n.startBeat = startSeconds * bpm / 60.0f;
                        n.lengthBeats = std::max(0.01f, lengthSeconds * bpm / 60.0f);
                        n.pitch = static_cast<uint8_t>(std::clamp(kMidiBase + bin, 0, 127));
                        n.velocity = static_cast<uint8_t>(std::clamp(peakActivation * 127.0f, 0.0f, 127.0f));
                        notes.push_back(n);
                    }
                    frameIdx = noteEnd; // continue scanning after this note
                } else {
                    ++frameIdx;
                }
            }
        }

        return notes;
    } catch (const Ort::Exception& e) {
        std::cerr << "MelodyExtractor: ONNX inference failed: " << e.what() << "\n";
        return std::nullopt;
    } catch (const std::exception& e) {
        std::cerr << "MelodyExtractor: unexpected error during extraction: " << e.what() << "\n";
        return std::nullopt;
    }
}
