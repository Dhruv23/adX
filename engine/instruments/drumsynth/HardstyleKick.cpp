// adx-thread: main
#include "engine/instruments/drumsynth/HardstyleKick.h"

#include <algorithm>
#include <array>

namespace adx::instruments {
namespace {

/// docs/STAKILLAZ.md §2.3 and the archive's Hardstyle Kick patch: a three-octave drop
/// over ~45 ms for the "tok", a sine sub welded under it, driven hard.
constexpr std::array<std::pair<std::string_view, float>, 10> kPatch{{
    {"model", 0.0F},
    {"keyTrack", 1.0F},
    {"tune", -12.0F},
    {"decay", 0.55F},
    {"click", 0.8F},
    {"tone", 0.7F},
    {"pitchDrop", 36.0F},
    {"pitchTime", 0.015F},
    {"drive", 12.0F},
    {"sub", 0.9F},
}};

} // namespace

std::span<const std::pair<std::string_view, float>> hardstyleKickParams() noexcept {
    return kPatch;
}

int wrapSemitones(int semitones) noexcept {
    int wrapped = ((semitones % 12) + 12) % 12; // 0..11
    if (wrapped > 6) {
        wrapped -= 12; // -5..6
    }
    return wrapped;
}

std::vector<KickNote> hardstyleKickNotes(std::span<const MelodyNote> melody, core::Ticks spacing,
                                         int fallbackBars, std::uint8_t rootKey) {
    std::vector<KickNote> kicks;
    if (spacing.value <= 0) {
        return kicks;
    }
    const std::int64_t beat = core::kPpq;
    std::int64_t start = 0;
    std::int64_t end = std::int64_t{std::max(fallbackBars, 0)} * 4 * beat;
    if (!melody.empty()) {
        std::int64_t first = melody.front().start.value;
        std::int64_t last = 0;
        for (const MelodyNote& note : melody) {
            first = std::min(first, note.start.value);
            last = std::max(last, note.start.value + note.length.value);
        }
        // Whole beats, rounded outward (the archive's floor and ceil).
        start = (first / beat) * beat;
        end = ((last + beat - 1) / beat) * beat;
    }
    for (std::int64_t at = start; at < end; at += spacing.value) {
        int shift = 0;
        const MelodyNote* covering = nullptr;
        for (const MelodyNote& note : melody) {
            if (note.start.value <= at && at < note.start.value + note.length.value &&
                (covering == nullptr || note.start > covering->start)) {
                covering = &note; // the latest-starting note covering it is the one heard
            }
        }
        if (covering != nullptr) {
            shift = wrapSemitones(static_cast<int>(covering->pitch) -
                                  static_cast<int>(melody.front().pitch));
        }
        kicks.push_back(KickNote{.start = core::Ticks{at},
                                 .length = spacing,
                                 .pitch = static_cast<std::uint8_t>(
                                     std::clamp(static_cast<int>(rootKey) + shift, 0, 127))});
    }
    return kicks;
}

} // namespace adx::instruments
