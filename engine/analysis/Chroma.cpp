// adx-thread: main
#include "engine/analysis/Chroma.h"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "engine/dsp/Fft.h"
#include "engine/dsp/Window.h"

namespace adx::analysis {

double Chromagram::timeOf(std::size_t index, std::uint32_t frameSize) const noexcept {
    return (static_cast<double>(index) * hop + (frameSize / 2.0)) / static_cast<double>(sampleRate);
}

void normalise(ChromaVector& chroma) noexcept {
    const float sum = std::accumulate(chroma.begin(), chroma.end(), 0.0F);
    if (sum <= 0.0F) {
        chroma.fill(0.0F);
        return;
    }
    for (float& value : chroma) {
        value /= sum;
    }
}

Chromagram chromagram(std::span<const float> mono, std::uint32_t sampleRate,
                      const ChromaOptions& options) {
    Chromagram out;
    out.sampleRate = sampleRate;
    out.hop = options.hop;
    const std::size_t size = options.frameSize;
    if (mono.size() < size || sampleRate == 0) {
        return out;
    }
    dsp::Fft fft;
    fft.prepare(size);
    std::vector<float> window(size);
    for (std::size_t i = 0; i < size; ++i) {
        window[i] = static_cast<float>(dsp::windowValue(dsp::WindowKind::Hann, i, size));
    }
    // Each bin's two semitone targets and weights, computed once.
    struct Share {
        std::uint32_t bin;
        std::uint8_t lowClass;
        float lowWeight;
        bool bass;
    };
    std::vector<Share> shares;
    const double binHz = static_cast<double>(sampleRate) / static_cast<double>(size);
    const double a4 = 440.0 * std::exp2(options.tuningCents / 1200.0);
    for (std::size_t bin = 1; bin < size / 2; ++bin) {
        const double hz = static_cast<double>(bin) * binHz;
        if (hz < options.minimumHz || hz > options.maximumHz) {
            continue;
        }
        const double midi = 69.0 + (12.0 * std::log2(hz / a4));
        const double below = std::floor(midi);
        const auto lowClass =
            static_cast<std::uint8_t>(((static_cast<long>(below) % 12) + 12) % 12);
        shares.push_back(Share{.bin = static_cast<std::uint32_t>(bin),
                               .lowClass = lowClass,
                               .lowWeight = static_cast<float>(1.0 - (midi - below)),
                               .bass = hz <= options.bassMaximumHz});
    }
    std::vector<dsp::Complex> spectrum(size);
    for (std::size_t start = 0; start + size <= mono.size(); start += options.hop) {
        if (options.progress && out.frames.size() % 64 == 0 &&
            !options.progress(static_cast<double>(start) / static_cast<double>(mono.size()))) {
            break;
        }
        double energy = 0.0;
        for (std::size_t i = 0; i < size; ++i) {
            const float sample = mono[start + i];
            energy += static_cast<double>(sample) * sample;
            spectrum[i] = dsp::Complex(sample * window[i], 0.0F);
        }
        ChromaVector chroma{};
        ChromaVector bass{};
        if (std::sqrt(energy / static_cast<double>(size)) >= options.silence) {
            fft.forward(spectrum);
            for (const Share& share : shares) {
                const float weight = std::sqrt(std::abs(spectrum[share.bin]));
                chroma.at(share.lowClass) += weight * share.lowWeight;
                chroma.at((share.lowClass + 1U) % 12U) += weight * (1.0F - share.lowWeight);
                if (share.bass) {
                    bass.at(share.lowClass) += weight * share.lowWeight;
                    bass.at((share.lowClass + 1U) % 12U) += weight * (1.0F - share.lowWeight);
                }
            }
            if (options.subtractFloor) {
                ChromaVector sorted = chroma;
                std::ranges::nth_element(sorted, sorted.begin() + 2);
                const float floor = sorted[2];
                for (float& value : chroma) {
                    value = std::max(0.0F, value - floor);
                }
            }
            normalise(chroma);
        }
        out.frames.push_back(chroma);
        out.bass.push_back(bass);
    }
    return out;
}

ChromaVector profileOf(const Chromagram& chroma) {
    ChromaVector sum{};
    for (const ChromaVector& frame : chroma.frames) {
        for (std::size_t c = 0; c < 12; ++c) {
            sum.at(c) += frame.at(c);
        }
    }
    normalise(sum);
    return sum;
}

ChromaVector bassProfileOf(const Chromagram& chroma) {
    ChromaVector sum{};
    for (const ChromaVector& frame : chroma.bass) {
        for (std::size_t c = 0; c < 12; ++c) {
            sum.at(c) += frame.at(c);
        }
    }
    normalise(sum);
    return sum;
}

ChromaVector profileOfNotes(std::span<const ChromaNote> notes) {
    ChromaVector sum{};
    for (const ChromaNote& note : notes) {
        sum.at(note.pitch % 12U) += static_cast<float>(note.duration) * note.velocity;
    }
    normalise(sum);
    return sum;
}

Chromagram chromagramOfNotes(std::span<const TimedChromaNote> notes, double frameSeconds) {
    Chromagram out;
    out.fromNotes = true;
    out.sampleRate = 48000;
    out.hop = static_cast<std::uint32_t>(std::max(1.0, frameSeconds * out.sampleRate));
    if (notes.empty() || frameSeconds <= 0.0) {
        return out;
    }
    double end = 0.0;
    for (const TimedChromaNote& n : notes) {
        end = std::max(end, n.startSeconds + n.seconds);
    }
    const auto frames = static_cast<std::size_t>(std::ceil(end / frameSeconds));
    out.frames.assign(frames, ChromaVector{});
    out.bass.assign(frames, ChromaVector{});
    std::vector<int> lowest(frames, 128);
    for (const TimedChromaNote& n : notes) {
        const auto first = static_cast<std::size_t>(std::max(0.0, n.startSeconds / frameSeconds));
        for (std::size_t f = first; f < frames; ++f) {
            const double from = std::max(n.startSeconds, static_cast<double>(f) * frameSeconds);
            const double to =
                std::min(n.startSeconds + n.seconds, static_cast<double>(f + 1) * frameSeconds);
            if (to <= from) {
                break;
            }
            out.frames[f].at(n.pitch % 12U) +=
                static_cast<float>((to - from) / frameSeconds) * n.velocity;
            lowest[f] = std::min<int>(lowest[f], n.pitch);
        }
    }
    for (std::size_t f = 0; f < frames; ++f) {
        normalise(out.frames[f]);
        if (lowest[f] < 128) {
            out.bass[f].at(static_cast<std::size_t>(lowest[f] % 12)) = 1.0F;
        }
    }
    return out;
}

} // namespace adx::analysis
