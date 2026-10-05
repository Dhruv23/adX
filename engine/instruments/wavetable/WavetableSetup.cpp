// adx-thread: main
#include "engine/instruments/wavetable/WavetableSetup.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine/dsp/FormantBank.h"
#include "engine/dsp/Math.h"
#include "engine/format/audio/AudioFileLoader.h"
#include "engine/format/audio/SamplePool.h"
#include "engine/project/Channel.h"
#include "engine/project/Resources.h"

namespace adx::instruments {

struct WavetablePins {
    std::unique_ptr<WavetablePair> table;
    std::filesystem::path source;
};

void destroyWavetablePins(WavetablePins* pins) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) - the node's opaque owner.
    delete pins;
}

namespace {

constexpr std::size_t kCycle = kImportFrameLength;
/// Harmonics summed into a built-in cycle: the table band-limits them again per level.
constexpr std::size_t kHarmonics = 256;

/// One cycle from sine-series amplitudes (and optional cosine ones), appended to `out`.
void appendCycle(std::vector<float>& out, const std::vector<float>& sines,
                 const std::vector<float>& cosines = {}) {
    const std::size_t base = out.size();
    out.resize(base + kCycle, 0.0F);
    for (std::size_t k = 1; k <= sines.size(); ++k) {
        const float a = sines[k - 1];
        const float b = k <= cosines.size() ? cosines[k - 1] : 0.0F;
        if (a == 0.0F && b == 0.0F) {
            continue;
        }
        for (std::size_t i = 0; i < kCycle; ++i) {
            // The phase in turns, reduced exactly: (k i mod N) / N.
            const auto turns = static_cast<double>((k * i) % kCycle) / static_cast<double>(kCycle);
            out[base + i] +=
                static_cast<float>((a * dsp::sinTurns(turns)) + (b * dsp::cosTurns(turns)));
        }
    }
}

std::vector<float> classicCycles() {
    std::vector<float> out;
    std::vector<float> sine(kHarmonics, 0.0F);
    sine[0] = 1.0F;
    std::vector<float> triangle(kHarmonics, 0.0F);
    std::vector<float> saw(kHarmonics, 0.0F);
    std::vector<float> square(kHarmonics, 0.0F);
    for (std::size_t k = 1; k <= kHarmonics; ++k) {
        const auto kk = static_cast<double>(k);
        saw[k - 1] = static_cast<float>(-2.0 / (dsp::kPi * kk));
        if (k % 2 == 1) {
            square[k - 1] = static_cast<float>(4.0 / (dsp::kPi * kk));
            const double sign = ((k - 1) / 2) % 2 == 0 ? 1.0 : -1.0;
            triangle[k - 1] = static_cast<float>(sign * 8.0 / (dsp::kPi * dsp::kPi * kk * kk));
        }
    }
    appendCycle(out, sine);
    appendCycle(out, triangle);
    appendCycle(out, saw);
    appendCycle(out, square);
    return out;
}

std::vector<float> pulseCycles() {
    // Widths from a square (0.5) down to a thin pulse (0.05), eight frames.
    std::vector<float> out;
    for (std::size_t f = 0; f < 8; ++f) {
        const double width = 0.5 - (0.45 * static_cast<double>(f) / 7.0);
        std::vector<float> sines(kHarmonics, 0.0F);
        std::vector<float> cosines(kHarmonics, 0.0F);
        for (std::size_t k = 1; k <= kHarmonics; ++k) {
            // A +-1 pulse high for `width` of the cycle, DC removed:
            // (4 / pi k) sin(pi k w) cos(2 pi k (t - w/2)).
            const auto kk = static_cast<double>(k);
            const double a = 4.0 / (dsp::kPi * kk) * dsp::sinTurns(0.5 * kk * width);
            const double shift = kk * width * 0.5;
            cosines[k - 1] = static_cast<float>(a * dsp::cosTurns(shift));
            sines[k - 1] = static_cast<float>(a * dsp::sinTurns(shift));
        }
        appendCycle(out, sines, cosines);
    }
    return out;
}

std::vector<float> harmonicCycles() {
    // Frame f carries harmonics 1 .. 2^f at 1/k: a sine opening into a saw.
    std::vector<float> out;
    for (std::size_t f = 0; f < 8; ++f) {
        std::vector<float> sines(kHarmonics, 0.0F);
        const std::size_t top = std::size_t{1} << f;
        for (std::size_t k = 1; k <= top && k <= kHarmonics; ++k) {
            sines[k - 1] = static_cast<float>(1.0 / static_cast<double>(k));
        }
        appendCycle(out, sines);
    }
    return out;
}

std::vector<float> formantCycles() {
    // The vowels a e i o u on a 110 Hz source: each harmonic weighted by the vowel's
    // formant peaks (the Additive's tenor table).
    std::vector<float> out;
    constexpr double kSource = 110.0;
    for (std::size_t v = 0; v < dsp::kVowelCount; ++v) {
        const dsp::VowelSpec& vowel = dsp::vowelSpec(dsp::VowelSet::Tenor5, v);
        std::vector<float> sines(kHarmonics, 0.0F);
        for (std::size_t k = 1; k <= kHarmonics; ++k) {
            const double hz = kSource * static_cast<double>(k);
            double weight = 0.0;
            for (std::size_t b = 0; b < vowel.count; ++b) {
                const dsp::Formant& f = vowel.bands[b];
                const double x = (hz - f.frequency) / std::max(1.0F, f.bandwidth);
                weight += f.gain / (1.0 + (x * x));
            }
            sines[k - 1] = static_cast<float>(weight / static_cast<double>(k));
        }
        appendCycle(out, sines);
    }
    // Normalise the frames together, so a sweep does not jump in level.
    float peak = 0.0F;
    for (const float s : out) {
        peak = std::max(peak, std::abs(s));
    }
    if (peak > 0.0F) {
        for (float& s : out) {
            s /= peak;
        }
    }
    return out;
}

} // namespace

WavetablePair buildWavetablePair(std::span<const float> cycles, std::size_t frameLength) {
    WavetablePair pair;
    pair.linear.buildFromCycles(cycles, frameLength);
    const std::size_t frames = frameLength == 0 ? 0 : cycles.size() / frameLength;
    // As dense as kSpectralSteps allows, within kMaxImportedFrames frames.
    pair.steps = frames < 2 ? 1
                            : std::clamp<std::size_t>((kMaxImportedFrames - 1) / (frames - 1), 1,
                                                      kSpectralSteps);
    pair.spectral.buildSpectralMorph(cycles, frameLength, pair.steps);
    return pair;
}

const WavetablePair& builtinWavetable(WavetableBank bank) {
    static const std::array<WavetablePair, 4> kTables = [] {
        std::array<WavetablePair, 4> tables;
        tables[0] = buildWavetablePair(classicCycles(), kCycle);
        tables[1] = buildWavetablePair(pulseCycles(), kCycle);
        tables[2] = buildWavetablePair(harmonicCycles(), kCycle);
        tables[3] = buildWavetablePair(formantCycles(), kCycle);
        return tables;
    }();
    const auto index = static_cast<std::size_t>(bank);
    return kTables[index < kTables.size() ? index : 0];
}

namespace {

std::filesystem::path sourceOf(const project::Channel& channel,
                               const project::Resources* resources) {
    if (channel.instrument.zones.empty() || resources == nullptr) {
        return {};
    }
    const project::SampleRef* ref = resources->find(channel.instrument.zones.front().sample);
    return ref != nullptr ? format::SamplePool::global().resolve(*resources, ref->path)
                          : std::filesystem::path{};
}

} // namespace

void configureWavetable(WavetableInstrument& node, const project::Channel& channel,
                        const project::Resources* resources) {
    auto pins = std::make_unique<WavetablePins>();
    pins->source = sourceOf(channel, resources);
    if (!pins->source.empty()) {
        format::SampleBuffer buffer;
        std::string error;
        if (format::decodeFile(pins->source, buffer, error) && buffer.frames >= kCycle) {
            const std::span<const float> audio = buffer.leftAudio();
            const std::size_t frames = std::min(audio.size() / kCycle, kMaxImportedFrames);
            pins->table = std::make_unique<WavetablePair>(
                buildWavetablePair(audio.first(frames * kCycle), kCycle));
        }
    }
    const WavetablePair* table = pins->table.get();
    node.setUserTable(table, pins.release());
}

bool wavetableMatches(const WavetableInstrument& node, const project::Channel& channel,
                      const project::Resources* resources) {
    const std::filesystem::path wanted = sourceOf(channel, resources);
    const WavetablePins* pins = node.pins();
    return pins != nullptr ? pins->source == wanted : wanted.empty();
}

} // namespace adx::instruments
