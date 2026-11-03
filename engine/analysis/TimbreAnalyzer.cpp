// adx-thread: main
#include "engine/analysis/TimbreAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace adx::analysis {

namespace {

constexpr double kRmsHopSeconds = 0.005;
constexpr double kRmsWindowSeconds = 0.025;
/// Below this, relative to the strongest harmonic (-60 dB), a harmonic is written as 0.
constexpr float kHarmonicFloor = 1e-3F;
/// v1's caps: beyond 2 s after note-off it is the reverb, not the amp envelope.
constexpr double kReleaseSearchSeconds = 2.0;

/// Single-bin DFT magnitude of `mono[start, start + length)` at `hz`, Hann-windowed and
/// scaled so a sine of amplitude A reads A. The window matters: leakage from the chord's
/// other tones is the dominant error.
float goertzel(std::span<const float> mono, std::size_t start, std::size_t length, double hz,
               double rate) {
    const double w = 2.0 * std::numbers::pi * hz / rate;
    const double coefficient = 2.0 * std::cos(w);
    double s1 = 0.0;
    double s2 = 0.0;
    const double span = static_cast<double>(length - 1);
    for (std::size_t i = 0; i < length; ++i) {
        const double hann =
            0.5 - (0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / span));
        const double s0 = (static_cast<double>(mono[start + i]) * hann) + (coefficient * s1) - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = (s1 * s1) + (s2 * s2) - (coefficient * s1 * s2);
    // Hann's coherent gain is 0.5, and a real sine puts half its amplitude in each bin.
    return static_cast<float>(std::sqrt(std::max(0.0, power)) /
                              (static_cast<double>(length) * 0.25));
}

struct Weighted {
    float value;
    double weight;
};

/// The weighted median; `fallback` when empty.
float medianOf(std::vector<Weighted> values, float fallback) {
    if (values.empty()) {
        return fallback;
    }
    std::ranges::sort(values, {}, &Weighted::value);
    double total = 0.0;
    for (const Weighted& v : values) {
        total += v.weight;
    }
    double running = 0.0;
    for (const Weighted& v : values) {
        running += v.weight;
        if (running >= total * 0.5) {
            return v.value;
        }
    }
    return values.back().value;
}

/// Frame i is the RMS of a `window`-long stretch centred on sample i * hop. The window
/// is several periods of a low note, so the envelope does not ripple with the waveform.
std::vector<float> rmsEnvelope(std::span<const float> mono, std::size_t hop, std::size_t window) {
    std::vector<float> out((mono.size() / hop) + 1, 0.0F);
    for (std::size_t frame = 0; frame * hop < mono.size(); ++frame) {
        const std::size_t centre = frame * hop;
        const std::size_t begin = centre > window / 2 ? centre - (window / 2) : 0;
        const std::size_t end = std::min(mono.size(), centre + (window / 2) + 1);
        double sum = 0.0;
        for (std::size_t i = begin; i < end; ++i) {
            sum += static_cast<double>(mono[i]) * mono[i];
        }
        out[frame] = static_cast<float>(std::sqrt(sum / static_cast<double>(end - begin)));
    }
    return out;
}

project::ParamValue param(std::string name, double value) {
    return project::ParamValue{
        .name = std::move(name), .value = value, .hasCurve = false, .curve = {}};
}

/// Rounded to four significant digits, so the preset text stays readable and the
/// analysis is not claiming more precision than a mix gives it.
double rounded(double v) {
    if (v == 0.0) {
        return 0.0;
    }
    const double scale = std::pow(10.0, 3 - static_cast<int>(std::floor(std::log10(std::abs(v)))));
    return std::round(v * scale) / scale;
}

} // namespace

std::optional<TimbreAnalysis> analyzeTimbre(std::span<const float> mono, std::uint32_t sampleRate,
                                            std::span<const TimbreNote> notes,
                                            const TimbreOptions& options) {
    if (mono.empty() || sampleRate == 0) {
        return std::nullopt;
    }
    const double rate = sampleRate;
    const double audioSeconds = static_cast<double>(mono.size()) / rate;

    std::vector<TimbreNote> candidates;
    for (const TimbreNote& n : notes) {
        if (n.seconds >= options.minimumSeconds && n.startSeconds >= 0.0 &&
            n.startSeconds + n.seconds < audioSeconds) {
            candidates.push_back(n);
        }
    }
    std::ranges::stable_sort(candidates, std::ranges::greater{}, &TimbreNote::seconds);
    if (candidates.size() > options.maximumNotes) {
        candidates.resize(options.maximumNotes);
    }

    // --- harmonics ----------------------------------------------------------------
    std::array<std::vector<Weighted>, kTimbreHarmonics> votes;
    std::uint32_t measured = 0;
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (options.progress && !options.progress(static_cast<double>(index) /
                                                  static_cast<double>(candidates.size()))) {
            return std::nullopt;
        }
        const TimbreNote& n = candidates[index];
        const double f0 =
            options.tuningA4 * std::exp2((static_cast<double>(n.pitch) - 69.0) / 12.0);
        if (f0 * 2.0 >= rate * 0.45) {
            continue;
        }
        const double windowStart = n.startSeconds + std::min(0.05, n.seconds * 0.25);
        const double windowSeconds = std::min(0.4, n.seconds * 0.6);
        const auto start = static_cast<std::size_t>(windowStart * rate);
        const auto length = static_cast<std::size_t>(windowSeconds * rate);
        if (length < 32 || start + length > mono.size()) {
            continue;
        }
        std::array<float, kTimbreHarmonics> h{};
        float peak = 0.0F;
        for (std::uint32_t k = 0; k < kTimbreHarmonics; ++k) {
            const double fk = f0 * (k + 1);
            h[k] = fk < rate * 0.45 ? goertzel(mono, start, length, fk, rate) : 0.0F;
            peak = std::max(peak, h[k]);
        }
        if (peak <= 0.0F) {
            continue;
        }
        for (std::uint32_t k = 0; k < kTimbreHarmonics; ++k) {
            // A harmonic above Nyquist was never measured: it does not vote.
            if (f0 * (k + 1) < rate * 0.45) {
                votes[k].push_back({.value = h[k] / peak, .weight = n.seconds});
            }
        }
        ++measured;
    }
    if (measured == 0) {
        return std::nullopt;
    }

    TimbreAnalysis out;
    out.notesMeasured = measured;
    float strongest = 0.0F;
    for (std::uint32_t k = 0; k < kTimbreHarmonics; ++k) {
        out.harmonics[k] = medianOf(votes[k], 0.0F);
        strongest = std::max(strongest, out.harmonics[k]);
    }
    if (strongest <= 0.0F) {
        return std::nullopt;
    }
    for (float& h : out.harmonics) {
        h /= strongest;
        if (h < kHarmonicFloor) {
            h = 0.0F;
        }
    }

    // --- envelope -----------------------------------------------------------------
    const auto hop = static_cast<std::size_t>(kRmsHopSeconds * rate);
    const std::vector<float> rms =
        rmsEnvelope(mono, hop, static_cast<std::size_t>(kRmsWindowSeconds * rate));
    const auto rmsAt = [&](double seconds) {
        const auto index =
            static_cast<std::size_t>(std::max(0.0, seconds) * rate / static_cast<double>(hop));
        return index < rms.size() ? rms[index] : 0.0F;
    };

    std::vector<Weighted> attacks;
    std::vector<Weighted> decays;
    std::vector<Weighted> sustains;
    std::vector<Weighted> releases;
    for (const TimbreNote& n : candidates) {
        const double search = std::min(n.seconds, 0.5);
        float peak = 0.0F;
        double peakAt = 0.0;
        for (double t = 0.0; t <= search; t += kRmsHopSeconds) {
            const float v = rmsAt(n.startSeconds + t);
            if (v > peak) {
                peak = v;
                peakAt = t;
            }
        }
        if (peak <= 0.0F) {
            continue;
        }
        attacks.push_back({.value = static_cast<float>(peakAt), .weight = n.seconds});
        const float end = rmsAt(n.startSeconds + (n.seconds * 0.9));
        const float sustain = std::clamp(end / peak, 0.0F, 1.0F);
        sustains.push_back({.value = sustain, .weight = n.seconds});

        if (sustain < 0.95F) {
            const float target = end + (0.05F * (peak - end));
            for (double t = peakAt; t <= n.seconds * 0.9; t += kRmsHopSeconds) {
                if (rmsAt(n.startSeconds + t) <= target) {
                    decays.push_back(
                        {.value = static_cast<float>(t - peakAt), .weight = n.seconds});
                    break;
                }
            }
        }

        const float floor = end * 0.01F;
        const double off = n.startSeconds + n.seconds;
        for (double t = kRmsHopSeconds; t <= kReleaseSearchSeconds && off + t < audioSeconds;
             t += kRmsHopSeconds) {
            if (rmsAt(off + t) <= floor) {
                releases.push_back({.value = static_cast<float>(t), .weight = n.seconds});
                break;
            }
        }
    }
    out.attackSeconds = std::clamp(medianOf(attacks, 0.01F), 0.0F, 1.5F);
    out.sustain = std::clamp(medianOf(sustains, 0.7F), 0.0F, 1.0F);
    out.decaySeconds = std::clamp(medianOf(decays, 0.2F), 0.0F, 5.0F);
    out.releaseSeconds = std::clamp(medianOf(releases, 0.25F), 0.02F, 2.5F);

    preset::Preset& p = out.preset;
    p.name = options.name;
    p.type = "additive";
    p.tags = {"analyzed"};
    p.params.push_back(param("env.attack", rounded(out.attackSeconds)));
    p.params.push_back(param("env.decay", rounded(out.decaySeconds)));
    p.params.push_back(param("env.sustain", rounded(out.sustain)));
    p.params.push_back(param("env.release", rounded(out.releaseSeconds)));
    for (std::uint32_t k = 0; k < kTimbreHarmonics; ++k) {
        // harmonic.1 defaults to 1 and the rest to 0: write only what differs.
        const float defaultLevel = k == 0 ? 1.0F : 0.0F;
        if (out.harmonics[k] != defaultLevel) {
            p.params.push_back(
                param("harmonic." + std::to_string(k + 1), rounded(out.harmonics[k])));
        }
    }
    return out;
}

} // namespace adx::analysis
