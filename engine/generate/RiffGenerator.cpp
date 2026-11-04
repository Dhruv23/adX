// adx-thread: main
#include "engine/generate/RiffGenerator.h"

#include <algorithm>
#include <cstdlib>

namespace adx::generate {

int roll(std::mt19937& random, int lo, int hi) noexcept {
    if (hi <= lo) {
        return lo;
    }
    const auto span = static_cast<std::uint32_t>(hi - lo + 1);
    return lo + static_cast<int>(random() % span);
}

bool chance(std::mt19937& random, double p) noexcept {
    return static_cast<double>(random()) < p * 4294967296.0;
}

int scalePitch(const analysis::Key& key, int tonicPitch, int degree) noexcept {
    const auto scale = analysis::scaleOf(key);
    const int octave = degree >= 0 ? degree / 7 : -((-degree + 6) / 7);
    const int step = degree - (octave * 7);
    const int offset = (scale.at(static_cast<std::size_t>(step)) - key.tonic + 12) % 12;
    return tonicPitch + (12 * octave) + offset;
}

std::vector<Chord> progression(const analysis::Key& key, const std::vector<int>& degrees,
                               int centre, bool sevenths) {
    std::vector<Chord> out;
    const int tonic = centre - (((centre % 12) - key.tonic + 12) % 12);
    for (const int degree : degrees) {
        Chord chord;
        chord.degree = degree;
        const int count = sevenths ? 4 : 3;
        std::vector<int> tones;
        tones.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            tones.push_back(scalePitch(key, tonic, degree - 1 + (2 * i)));
        }
        // Invert until the chord sits within a fifth or so of the centre.
        while (tones.front() < centre - 2) {
            tones.push_back(tones.front() + 12);
            tones.erase(tones.begin());
        }
        while (tones.back() - 12 >= centre + 7) {
            tones.insert(tones.begin(), tones.back() - 12);
            tones.pop_back();
        }
        chord.pitches = tones;
        out.push_back(std::move(chord));
    }
    return out;
}

std::vector<GenNote> arpeggio(const Chord& chord, std::int64_t start, std::int64_t length,
                              std::int64_t step, int octaves, ArpMode mode, std::uint8_t velocity,
                              double gate, std::mt19937& random) {
    std::vector<int> ladder;
    for (int o = 0; o < std::max(1, octaves); ++o) {
        for (const int p : chord.pitches) {
            ladder.push_back(p + (12 * o));
        }
    }
    std::ranges::sort(ladder);
    if (mode == ArpMode::Down) {
        std::ranges::reverse(ladder);
    } else if (mode == ArpMode::UpDown && ladder.size() > 2) {
        for (std::size_t i = ladder.size() - 2; i > 0; --i) {
            ladder.push_back(ladder[i]);
        }
    }
    std::vector<GenNote> out;
    std::size_t i = 0;
    for (std::int64_t t = start; t < start + length; t += step, ++i) {
        const int pitch = mode == ArpMode::Random
                              ? ladder[static_cast<std::size_t>(
                                    roll(random, 0, static_cast<int>(ladder.size()) - 1))]
                              : ladder[i % ladder.size()];
        out.push_back({.start = t,
                       .length = std::max<std::int64_t>(
                           1, static_cast<std::int64_t>(static_cast<double>(step) * gate)),
                       .pitch = static_cast<std::uint8_t>(std::clamp(pitch, 0, 127)),
                       .velocity = static_cast<std::uint8_t>(
                           std::clamp(velocity - roll(random, 0, 12), 1, 127))});
    }
    return out;
}

std::vector<GenNote> melody(const MelodySpec& spec, std::mt19937& random) {
    std::vector<GenNote> out;
    const int tonic = spec.tonicPitch;
    // Degrees within the range, as indices into the scale ladder.
    std::vector<int> ladder;
    for (int d = -21; d <= 28; ++d) {
        const int p = scalePitch(spec.key, tonic, d);
        if (p >= spec.lowest && p <= spec.highest) {
            ladder.push_back(p);
        }
    }
    if (ladder.empty() || spec.step <= 0) {
        return out;
    }
    auto at = static_cast<int>(ladder.size() / 2);
    for (std::int64_t t = spec.start; t < spec.start + spec.length; t += spec.step) {
        const bool onBeat = spec.barTicks > 0 && (t - spec.start) % (spec.barTicks / 4) == 0;
        if (!chance(random, onBeat ? std::min(1.0, spec.density * 1.4) : spec.density)) {
            continue;
        }
        const int move = chance(random, spec.leap) ? roll(random, -4, 4) : roll(random, -1, 1);
        at = std::clamp(at + move, 0, static_cast<int>(ladder.size()) - 1);
        int pitch = ladder[static_cast<std::size_t>(at)];
        if (onBeat && spec.chords != nullptr && !spec.chords->empty() && spec.barTicks > 0) {
            // On the beat, the nearest tone of the chord under it.
            const auto bar =
                static_cast<std::size_t>((t - spec.start) / spec.barTicks) % spec.chords->size();
            int best = pitch;
            int distance = 128;
            for (const int c : (*spec.chords)[bar].pitches) {
                for (int o = -3; o <= 3; ++o) {
                    const int candidate = c + (12 * o);
                    if (candidate >= spec.lowest && candidate <= spec.highest &&
                        std::abs(candidate - pitch) < distance) {
                        distance = std::abs(candidate - pitch);
                        best = candidate;
                    }
                }
            }
            pitch = best;
            at = static_cast<int>(std::ranges::lower_bound(ladder, pitch) - ladder.begin());
            at = std::clamp(at, 0, static_cast<int>(ladder.size()) - 1);
        }
        const std::int64_t steps = chance(random, 0.3) ? 2 : 1;
        out.push_back({.start = t,
                       .length = spec.step * steps,
                       .pitch = static_cast<std::uint8_t>(pitch),
                       .velocity = static_cast<std::uint8_t>(
                           std::clamp(spec.velocity - roll(random, 0, 15), 1, 127))});
        if (steps == 2) {
            t += spec.step;
        }
    }
    return out;
}

std::vector<GenNote> pulse(std::string_view pattern, int bars, std::int64_t barTicks,
                           std::uint8_t pitch, std::uint8_t velocity, std::uint8_t accent,
                           double gate) {
    std::vector<GenNote> out;
    if (pattern.empty()) {
        return out;
    }
    const std::int64_t step = barTicks / 16;
    for (int bar = 0; bar < bars; ++bar) {
        for (std::size_t i = 0; i < 16; ++i) {
            const char c = pattern[((static_cast<std::size_t>(bar) * 16) + i) % pattern.size()];
            if (c != 'x' && c != 'X') {
                continue;
            }
            out.push_back({.start = (bar * barTicks) + (static_cast<std::int64_t>(i) * step),
                           .length = std::max<std::int64_t>(
                               1, static_cast<std::int64_t>(static_cast<double>(step) * gate)),
                           .pitch = pitch,
                           .velocity = c == 'X' ? accent : velocity});
        }
    }
    return out;
}

} // namespace adx::generate
