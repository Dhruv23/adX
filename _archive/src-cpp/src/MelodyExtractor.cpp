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
    constexpr int kEnergyTolFrames = 11;     // energy_tol in output_to_notes_polyphonic
    constexpr int kOnsetInferDiffFrames = 2; // n_diff in get_infered_onsets
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

        // Postprocess: port of basic-pitch's output_to_notes_polyphonic
        // (note_creation.py) with infer_onsets=True and melodia_trick=True.
        // The earlier simplified per-bin onset-gated scan systematically lost
        // chords: pad/synth chord tones have slow attacks, and send effects
        // (reverb/delay) smear what little onset energy there is below the 0.5
        // gate, so only the melody's hard attacks survived. Three additions fix
        // that: (1) onsets INFERRED from frame-energy rises, so a chord tone
        // that fades in still starts a note; (2) an energy-tolerance gap of
        // kEnergyTolFrames, so tremolo/effect dips don't split or truncate
        // sustained chords; (3) the "melodia trick" — residual frame energy
        // that never produced any onset at all is still swept into notes.
        const int n_frames = static_cast<int>(noteMatrix.size());
        const int numBins = n_frames > 0 ? std::min(static_cast<int>(noteMatrix[0].size()), kNumPitchBins) : 0;
        const int maxFreqIdx = numBins - 1;

        // --- get_infered_onsets: onset = max(model onset, rescaled positive
        // frame-energy rise). The rise at frame f is min over n=1..n_diff of
        // (frame[f] - frame[f-n]), clamped >= 0, zeroed for the first n_diff
        // frames, rescaled so its max equals the model onsets' max.
        std::vector<std::vector<float>> onsets = onsetMatrix; // combined copy; onsetMatrix stays pristine
        {
            float maxOnset = 0.0f, maxDiff = 0.0f;
            std::vector<std::vector<float>> frameDiff(n_frames, std::vector<float>(numBins, 0.0f));
            for (int f = 0; f < n_frames; ++f) {
                for (int b = 0; b < numBins; ++b) {
                    maxOnset = std::max(maxOnset, onsetMatrix[f][b]);
                    if (f < kOnsetInferDiffFrames) continue;
                    float d = noteMatrix[f][b] - noteMatrix[f - 1][b];
                    for (int n = 2; n <= kOnsetInferDiffFrames; ++n)
                        d = std::min(d, noteMatrix[f][b] - noteMatrix[f - n][b]);
                    frameDiff[f][b] = std::max(0.0f, d);
                    maxDiff = std::max(maxDiff, frameDiff[f][b]);
                }
            }
            if (maxDiff > 0.0f) {
                float scale = maxOnset / maxDiff;
                for (int f = 0; f < n_frames; ++f)
                    for (int b = 0; b < numBins; ++b)
                        onsets[f][b] = std::max(onsets[f][b], frameDiff[f][b] * scale);
            }
        }

        // remainingEnergy: frame activations not yet claimed by a note. Claimed
        // spans are zeroed (including the two adjacent semitone bins, since the
        // model bleeds energy into neighbours) so the melodia pass below only
        // sees genuinely unexplained energy.
        std::vector<std::vector<float>> remainingEnergy = noteMatrix;

        struct RawNote { int startFrame; int endFrame; int bin; float amplitude; };
        std::vector<RawNote> rawNotes;

        auto claimSpan = [&](int startFrame, int endFrame, int bin) { // [start, end)
            for (int f = startFrame; f < endFrame; ++f) {
                remainingEnergy[f][bin] = 0.0f;
                if (bin < maxFreqIdx) remainingEnergy[f][bin + 1] = 0.0f;
                if (bin > 0) remainingEnergy[f][bin - 1] = 0.0f;
            }
        };
        auto meanFrameActivation = [&](int startFrame, int endFrame, int bin) {
            float sum = 0.0f;
            for (int f = startFrame; f < endFrame; ++f) sum += noteMatrix[f][bin];
            return endFrame > startFrame ? sum / static_cast<float>(endFrame - startFrame) : 0.0f;
        };

        // --- Pass 1: onset-peak triggered notes. Peaks are time-local maxima
        // of the combined onset matrix above threshold, processed in reverse
        // chronological order (as upstream does, so late re-articulations claim
        // their own energy before an earlier onset's forward scan absorbs it).
        for (int f = n_frames - 2; f >= 1; --f) {
            for (int b = 0; b < numBins; ++b) {
                float v = onsets[f][b];
                if (v <= kOnsetThreshold || v < onsets[f - 1][b] || v < onsets[f + 1][b]) continue;

                // Scan forward while energy stays above the frame threshold,
                // tolerating up to kEnergyTolFrames consecutive quiet frames.
                int i = f + 1, quiet = 0;
                while (i < n_frames - 1 && quiet < kEnergyTolFrames) {
                    quiet = (remainingEnergy[i][b] < kFrameThreshold) ? quiet + 1 : 0;
                    ++i;
                }
                i -= quiet; // back to the last frame that was above threshold
                if (i - f <= kMinNoteLenFrames) continue;

                claimSpan(f, i, b);
                rawNotes.push_back({f, i, b, meanFrameActivation(f, i, b)});
            }
        }

        // --- Pass 2 (melodia trick): repeatedly take the loudest unclaimed
        // frame activation and grow a note around it in both directions. This
        // is what recovers sustained chord tones whose onsets were never
        // detected at all (e.g. a pad chord fading in under a delay wash).
        while (true) {
            float best = kFrameThreshold;
            int bestF = -1, bestB = -1;
            for (int f = 0; f < n_frames; ++f)
                for (int b = 0; b < numBins; ++b)
                    if (remainingEnergy[f][b] > best) { best = remainingEnergy[f][b]; bestF = f; bestB = b; }
            if (bestF < 0) break;

            remainingEnergy[bestF][bestB] = 0.0f;

            int i = bestF + 1, quiet = 0;
            while (i < n_frames - 1 && quiet < kEnergyTolFrames) {
                quiet = (remainingEnergy[i][bestB] < kFrameThreshold) ? quiet + 1 : 0;
                remainingEnergy[i][bestB] = 0.0f;
                if (bestB < maxFreqIdx) remainingEnergy[i][bestB + 1] = 0.0f;
                if (bestB > 0) remainingEnergy[i][bestB - 1] = 0.0f;
                ++i;
            }
            int endFrame = i - 1 - quiet + 1; // last above-threshold frame, exclusive

            i = bestF - 1; quiet = 0;
            while (i > 0 && quiet < kEnergyTolFrames) {
                quiet = (remainingEnergy[i][bestB] < kFrameThreshold) ? quiet + 1 : 0;
                remainingEnergy[i][bestB] = 0.0f;
                if (bestB < maxFreqIdx) remainingEnergy[i][bestB + 1] = 0.0f;
                if (bestB > 0) remainingEnergy[i][bestB - 1] = 0.0f;
                --i;
            }
            int startFrame = i + 1 + quiet;

            if (endFrame - startFrame <= kMinNoteLenFrames) continue;
            rawNotes.push_back({startFrame, endFrame, bestB, meanFrameActivation(startFrame, endFrame, bestB)});
        }

        std::vector<Note> notes;
        notes.reserve(rawNotes.size());
        for (const RawNote& rn : rawNotes) {
            float startSeconds = static_cast<float>(rn.startFrame) / static_cast<float>(kAnnotationsFps);
            float lengthSeconds = static_cast<float>(rn.endFrame - rn.startFrame) / static_cast<float>(kAnnotationsFps);
            Note n;
            n.startBeat = startSeconds * bpm / 60.0f;
            n.lengthBeats = std::max(0.01f, lengthSeconds * bpm / 60.0f);
            n.pitch = static_cast<uint8_t>(std::clamp(kMidiBase + rn.bin, 0, 127));
            n.velocity = static_cast<uint8_t>(std::clamp(rn.amplitude * 127.0f, 1.0f, 127.0f));
            notes.push_back(n);
        }
        std::sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
            return a.startBeat < b.startBeat || (a.startBeat == b.startBeat && a.pitch < b.pitch);
        });

        return notes;
    } catch (const Ort::Exception& e) {
        std::cerr << "MelodyExtractor: ONNX inference failed: " << e.what() << "\n";
        return std::nullopt;
    } catch (const std::exception& e) {
        std::cerr << "MelodyExtractor: unexpected error during extraction: " << e.what() << "\n";
        return std::nullopt;
    }
}
