// adx-thread: realtime
//
// A pad controller's map as the audio thread reads it (phase_11.md §4.7): for each note
// and controller number, what pressing it does to the session. Compiled from a profile
// on the main thread (PadController.h) and handed to the engine whole; the audio thread
// only looks up and never parses. Trivially copyable, so a map is one allocation the
// main thread makes and keeps.
#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

namespace adx::midi {

struct PadAction {
    enum class Kind : std::uint8_t {
        None,
        /// Launch the clip at (track, row).
        Cell,
        /// Fire scene `row`.
        Scene,
        /// Stop track `track`'s clip.
        StopTrack,
        StopAll,
        TapTempo,
    };
    Kind kind{Kind::None};
    std::uint8_t track{0};
    std::uint8_t row{0};
};

struct PadMap {
    /// The MIDI channel the pads send on, 0-15, or kAnyChannel.
    static constexpr std::uint8_t kAnyChannel = 0xFF;
    std::uint8_t channel{kAnyChannel};
    /// Every message from the device is the pads': none of it plays an instrument.
    bool consumeAll{true};
    bool velocitySensitive{false};
    std::array<PadAction, 128> notes{};
    std::array<PadAction, 128> controls{};
    /// A press's gain by velocity (the profile's curve); used when velocitySensitive.
    std::array<float, 128> gain{};

    /// What a message does: none for one that is not the pads'.
    [[nodiscard]] PadAction actionFor(std::uint8_t status, std::uint8_t data1,
                                      std::uint8_t data2) const noexcept {
        const auto kind = static_cast<std::uint8_t>(status & 0xF0U);
        const auto on = static_cast<std::uint8_t>(status & 0x0FU);
        if (channel != kAnyChannel && on != channel) {
            return {};
        }
        if (kind == 0x90U && data2 > 0) {
            return notes.at(data1 & 0x7FU);
        }
        if (kind == 0xB0U && data2 > 0) {
            return controls.at(data1 & 0x7FU);
        }
        return {};
    }
};

static_assert(std::is_trivially_copyable_v<PadMap>);

} // namespace adx::midi
