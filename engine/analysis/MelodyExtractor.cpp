// adx-thread: main
//
// Built only with ADX_ENABLE_ONNX (engine/analysis/CMakeLists.txt).
#include "engine/analysis/MelodyExtractor.h"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>

namespace adx::analysis {

namespace {

// basic-pitch's constants (basic_pitch/constants.py, inference.py).
constexpr std::size_t kFftHop = 256;
constexpr std::size_t kOverlapFrames = 30;
constexpr std::size_t kWindowSamples = (kMelodySampleRate * 2) - kFftHop; // 43844
constexpr const char* kInputName = "serving_default_input_2:0";
/// In this order the model returns note, onset, contour (inference.py's zip).
constexpr std::array<const char*, 3> kOutputNames{
    "StatefulPartitionedCall:1", "StatefulPartitionedCall:2", "StatefulPartitionedCall:0"};

Ort::Env& environment() {
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "adx-melody");
    return env;
}

} // namespace

std::filesystem::path defaultMelodyModel() {
    return std::filesystem::path(ADX_ONNX_MODEL);
}

std::optional<std::vector<DetectedNote>> extractMelody(std::span<const float> mono,
                                                       const std::filesystem::path& model,
                                                       const MelodyProgress& progress,
                                                       const NoteCreationOptions& options) {
    if (mono.empty()) {
        return std::vector<DetectedNote>{};
    }
    std::ifstream file(model, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    const std::vector<char> bytes{std::istreambuf_iterator<char>(file),
                                  std::istreambuf_iterator<char>()};
    try {
        Ort::SessionOptions sessionOptions;
        Ort::Session session(environment(), bytes.data(), bytes.size(), sessionOptions);
        const Ort::MemoryInfo memory =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        // basic-pitch pads the start by half the overlap and keeps the middle of each
        // window. Concatenating whole windows less a fixed count (as v1 did) drifts:
        // a window advances 141.27 frames but would contribute 142, about 145 ms over
        // 30 s. Instead every output frame g, at sample g * 256 of the original, is read
        // from the window whose middle covers it, at the local frame that is that time.
        constexpr std::size_t kPad = kOverlapFrames * kFftHop / 2;
        const std::size_t hop = kWindowSamples - (kOverlapFrames * kFftHop);
        std::vector<float> padded(kPad + mono.size(), 0.0F);
        std::ranges::copy(mono, padded.begin() + kPad);
        std::vector<std::vector<float>> notes;
        std::vector<std::vector<float>> onsets;
        std::size_t windowFrames = 0;
        std::vector<float> window(kWindowSamples);
        for (std::size_t start = 0; start < padded.size(); start += hop) {
            if (progress &&
                !progress(static_cast<double>(start) / static_cast<double>(padded.size()))) {
                return std::nullopt;
            }
            std::ranges::fill(window, 0.0F);
            const std::size_t available = std::min(kWindowSamples, padded.size() - start);
            std::copy_n(padded.begin() + static_cast<std::ptrdiff_t>(start), available,
                        window.begin());

            const std::array<std::int64_t, 3> shape{1, static_cast<std::int64_t>(kWindowSamples),
                                                    1};
            Ort::Value input = Ort::Value::CreateTensor<float>(memory, window.data(), window.size(),
                                                               shape.data(), shape.size());
            Ort::RunOptions run;
            auto outputs =
                session.Run(run, &kInputName, &input, 1, kOutputNames.data(), kOutputNames.size());
            const auto noteShape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
            const auto onsetShape = outputs[1].GetTensorTypeAndShapeInfo().GetShape();
            if (noteShape.size() != 3 || noteShape != onsetShape || noteShape[2] != kPitchBins) {
                return std::nullopt;
            }
            windowFrames = static_cast<std::size_t>(noteShape[1]);
            const float* note = outputs[0].GetTensorData<float>();
            const float* onset = outputs[1].GetTensorData<float>();
            notes.emplace_back(note, note + (windowFrames * kPitchBins));
            onsets.emplace_back(onset, onset + (windowFrames * kPitchBins));
            if (start + kWindowSamples >= padded.size()) {
                break;
            }
        }
        Activations activations;
        activations.frames = static_cast<std::uint32_t>(mono.size() / kFftHop);
        activations.note.assign(static_cast<std::size_t>(activations.frames) * kPitchBins, 0.0F);
        activations.onset = activations.note;
        const double hopFrames = static_cast<double>(hop) / kFftHop;
        const double padFrames = static_cast<double>(kPad) / kFftHop;
        for (std::size_t g = 0; g < activations.frames; ++g) {
            const auto k = std::min(notes.size() - 1,
                                    static_cast<std::size_t>(static_cast<double>(g) / hopFrames));
            const auto local = static_cast<std::size_t>(std::lround(
                static_cast<double>(g) - (static_cast<double>(k) * hopFrames) + padFrames));
            if (local >= windowFrames) {
                continue;
            }
            std::copy_n(notes[k].begin() + static_cast<std::ptrdiff_t>(local * kPitchBins),
                        kPitchBins,
                        activations.note.begin() + static_cast<std::ptrdiff_t>(g * kPitchBins));
            std::copy_n(onsets[k].begin() + static_cast<std::ptrdiff_t>(local * kPitchBins),
                        kPitchBins,
                        activations.onset.begin() + static_cast<std::ptrdiff_t>(g * kPitchBins));
        }
        if (progress && !progress(1.0)) {
            return std::nullopt;
        }
        return notesFromActivations(activations, options);
    } catch (const Ort::Exception&) {
        return std::nullopt;
    }
}

} // namespace adx::analysis
