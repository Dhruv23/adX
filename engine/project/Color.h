// An entity's colour.
//
// Its own header rather than a member of whichever entity needed it first:
// channels, patterns, inserts and playlist tracks all carry one, and putting it in
// Channel.h would make Mixer.h include Channel.h for three bytes.
//
// phase_2.md §2's manifest does not list this file. It is an addition, recorded in
// that plan's §10.
#pragma once

#include <cstdint>

namespace adx::project {

/// 8 bits per channel, which is what `#rrggbb` in the file carries and what a UI
/// needs. No alpha: nothing in the model is translucent.
struct Color {
    std::uint8_t red{0x80};
    std::uint8_t green{0x80};
    std::uint8_t blue{0x80};

    [[nodiscard]] friend bool operator==(const Color&, const Color&) noexcept = default;
};

/// The colour an entity gets when the file does not say. Matching the member
/// defaults, so a round-trip of a file with no COLOR= key writes no COLOR= key.
inline constexpr Color kDefaultColor{};

} // namespace adx::project
