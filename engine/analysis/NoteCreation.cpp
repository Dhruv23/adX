// adx-thread: main
#include "engine/analysis/NoteCreation.h"

#include <algorithm>

namespace adx::analysis {

namespace {

constexpr int kMidiBase = 21;
/// n_diff in basic-pitch's get_infered_onsets.
constexpr std::uint32_t kInferDiffFrames = 2;

struct Raw {
    std::uint32_t start;
    std::uint32_t end; // exclusive
    std::uint32_t bin;
    float amplitude;
};

class Grid {
public:
    Grid(std::uint32_t frames, std::vector<float> values)
        : m_frames(frames), m_values(std::move(values)) {}

    [[nodiscard]] float& at(std::uint32_t frame, std::uint32_t bin) noexcept {
        return m_values[(static_cast<std::size_t>(frame) * kPitchBins) + bin];
    }
    [[nodiscard]] float at(std::uint32_t frame, std::uint32_t bin) const noexcept {
        return m_values[(static_cast<std::size_t>(frame) * kPitchBins) + bin];
    }
    [[nodiscard]] std::uint32_t frames() const noexcept {
        return m_frames;
    }

    /// Zeroes `bin` and its two neighbours at `frame`.
    void claim(std::uint32_t frame, std::uint32_t bin) noexcept {
        at(frame, bin) = 0.0F;
        if (bin + 1 < kPitchBins) {
            at(frame, bin + 1) = 0.0F;
        }
        if (bin > 0) {
            at(frame, bin - 1) = 0.0F;
        }
    }

private:
    std::uint32_t m_frames;
    std::vector<float> m_values;
};

/// The model's onsets, or the positive frame-energy rise (the smallest over the last
/// two frames) rescaled to the onsets' peak, whichever is larger.
Grid inferredOnsets(const Activations& a) {
    Grid onsets(a.frames, a.onset);
    std::vector<float> rise(a.note.size(), 0.0F);
    float peakOnset = 0.0F;
    float peakRise = 0.0F;
    for (std::uint32_t f = 0; f < a.frames; ++f) {
        for (std::uint32_t b = 0; b < kPitchBins; ++b) {
            peakOnset = std::max(peakOnset, onsets.at(f, b));
            if (f < kInferDiffFrames) {
                continue;
            }
            float d = a.noteAt(f, b) - a.noteAt(f - 1, b);
            for (std::uint32_t n = 2; n <= kInferDiffFrames; ++n) {
                d = std::min(d, a.noteAt(f, b) - a.noteAt(f - n, b));
            }
            const std::size_t i = (static_cast<std::size_t>(f) * kPitchBins) + b;
            rise[i] = std::max(0.0F, d);
            peakRise = std::max(peakRise, rise[i]);
        }
    }
    if (peakRise > 0.0F) {
        const float scale = peakOnset / peakRise;
        for (std::uint32_t f = 0; f < a.frames; ++f) {
            for (std::uint32_t b = 0; b < kPitchBins; ++b) {
                const std::size_t i = (static_cast<std::size_t>(f) * kPitchBins) + b;
                onsets.at(f, b) = std::max(onsets.at(f, b), rise[i] * scale);
            }
        }
    }
    return onsets;
}

float meanActivation(const Activations& a, std::uint32_t start, std::uint32_t end,
                     std::uint32_t bin) {
    float sum = 0.0F;
    for (std::uint32_t f = start; f < end; ++f) {
        sum += a.noteAt(f, bin);
    }
    return end > start ? sum / static_cast<float>(end - start) : 0.0F;
}

} // namespace

std::vector<DetectedNote> notesFromActivations(const Activations& a,
                                               const NoteCreationOptions& options) {
    std::vector<DetectedNote> out;
    if (a.frames < 3 || a.note.size() != static_cast<std::size_t>(a.frames) * kPitchBins ||
        a.onset.size() != a.note.size()) {
        return out;
    }
    const Grid onsets = options.inferOnsets ? inferredOnsets(a) : Grid(a.frames, a.onset);
    Grid remaining(a.frames, a.note);
    std::vector<Raw> raw;
    const std::uint32_t frames = a.frames;

    // Pass 1, latest first, as upstream does: a late re-articulation claims its own
    // energy before an earlier onset's forward scan swallows it.
    for (std::uint32_t f = frames - 2; f >= 1; --f) {
        for (std::uint32_t b = 0; b < kPitchBins; ++b) {
            const float v = onsets.at(f, b);
            if (v <= options.onsetThreshold || v < onsets.at(f - 1, b) || v < onsets.at(f + 1, b)) {
                continue;
            }
            std::uint32_t i = f + 1;
            std::uint32_t quiet = 0;
            while (i < frames - 1 && quiet < options.energyTolerance) {
                quiet = remaining.at(i, b) < options.frameThreshold ? quiet + 1 : 0;
                ++i;
            }
            i -= quiet;
            if (i - f <= options.minimumFrames) {
                continue;
            }
            for (std::uint32_t g = f; g < i; ++g) {
                remaining.claim(g, b);
            }
            raw.push_back(
                {.start = f, .end = i, .bin = b, .amplitude = meanActivation(a, f, i, b)});
        }
    }

    // Pass 2: the loudest unclaimed energy, grown both ways.
    while (options.melodiaTrick) {
        float best = options.frameThreshold;
        std::uint32_t bestFrame = frames;
        std::uint32_t bestBin = 0;
        for (std::uint32_t f = 0; f < frames; ++f) {
            for (std::uint32_t b = 0; b < kPitchBins; ++b) {
                if (remaining.at(f, b) > best) {
                    best = remaining.at(f, b);
                    bestFrame = f;
                    bestBin = b;
                }
            }
        }
        if (bestFrame == frames) {
            break;
        }
        remaining.at(bestFrame, bestBin) = 0.0F;

        std::uint32_t i = bestFrame + 1;
        std::uint32_t quiet = 0;
        while (i < frames - 1 && quiet < options.energyTolerance) {
            quiet = remaining.at(i, bestBin) < options.frameThreshold ? quiet + 1 : 0;
            remaining.claim(i, bestBin);
            ++i;
        }
        const std::uint32_t end = i - quiet;

        std::int64_t j = static_cast<std::int64_t>(bestFrame) - 1;
        quiet = 0;
        while (j > 0 && quiet < options.energyTolerance) {
            const auto g = static_cast<std::uint32_t>(j);
            quiet = remaining.at(g, bestBin) < options.frameThreshold ? quiet + 1 : 0;
            remaining.claim(g, bestBin);
            --j;
        }
        const auto start = static_cast<std::uint32_t>(j + 1 + quiet);
        if (end <= start || end - start <= options.minimumFrames) {
            continue;
        }
        raw.push_back({.start = start,
                       .end = end,
                       .bin = bestBin,
                       .amplitude = meanActivation(a, start, end, bestBin)});
    }

    out.reserve(raw.size());
    for (const Raw& r : raw) {
        out.push_back({.startSeconds = r.start / kActivationFps,
                       .seconds = (r.end - r.start) / kActivationFps,
                       .pitch = static_cast<std::uint8_t>(
                           std::clamp(kMidiBase + static_cast<int>(r.bin), 0, 127)),
                       .velocity = static_cast<std::uint8_t>(
                           std::clamp(r.amplitude * 127.0F, 1.0F, 127.0F))});
    }
    std::ranges::sort(out, [](const DetectedNote& x, const DetectedNote& y) {
        return x.startSeconds < y.startSeconds ||
               (x.startSeconds == y.startSeconds && x.pitch < y.pitch);
    });
    return out;
}

} // namespace adx::analysis
