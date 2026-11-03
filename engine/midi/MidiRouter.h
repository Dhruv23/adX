// adx-thread: realtime
//
// Device -> channel routing (phase_9.md §4.1): which input and MIDI channel reaches
// which adX channel, over which keys and velocities, transposed and through which
// velocity curve. Every route is a project setting, so a setup saves and diffs like
// everything else; this is only the transform.
//
// A note-off goes where its note-on went. The router remembers each sounding key's
// destination, so changing the routes - or the transpose - while a key is held cannot
// leave a note stuck on.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "engine/midi/MidiEvent.h"

namespace adx::midi {

/// Any device, any MIDI channel.
inline constexpr std::uint8_t kAnyDevice = 0xFF;
inline constexpr std::uint16_t kAllChannels = 0xFFFF;

struct MidiRoute {
    std::uint8_t device{kAnyDevice};
    /// Bit c set: MIDI channel c (0..15) passes.
    std::uint16_t channels{kAllChannels};
    std::uint8_t keyLow{0};
    std::uint8_t keyHigh{127};
    std::uint8_t velocityLow{1};
    std::uint8_t velocityHigh{127};
    /// Semitones; a key pushed outside 0..127 is dropped.
    std::int8_t transpose{0};
    /// The curve's bend, -1..1: 0 is straight, above 0 lifts soft playing, below 0
    /// presses it down. out = 127 * (in / 127) ^ (2 ^ -curve).
    float curve{0.0F};
    /// The adX channel this route plays.
    std::uint32_t target{0};

    [[nodiscard]] friend bool operator==(const MidiRoute&, const MidiRoute&) noexcept = default;
};

/// What a route made of an event, for one channel.
struct RoutedEvent {
    std::uint32_t target{0};
    MidiEvent event;
};

/// The velocity `route` turns `velocity` (1..127) into; never 0, which would be a
/// note-off.
[[nodiscard]] std::uint8_t curveVelocity(float curve, std::uint8_t velocity) noexcept;

class MidiRouter {
public:
    /// Most routes one event can fan out to.
    static constexpr std::size_t kMaxFanOut = 8;
    static constexpr std::size_t kDevices = 8;

    /// The routes, in order. Held by reference: the caller keeps them alive and swaps
    /// them only between blocks.
    void setRoutes(std::span<const MidiRoute> routes) noexcept {
        m_routes = routes;
    }

    /// Routes one event into `out`; returns how many it became. Note-ons go to every
    /// route they pass, note-offs to wherever their note-on went, everything else (CC,
    /// bend, pressure) to every route whose device and channel match.
    std::size_t route(const MidiEvent& event, std::span<RoutedEvent> out) noexcept;

    /// Forgets every sounding key: after a panic, or when the device closed.
    void reset() noexcept;

private:
    struct Sounding {
        std::array<std::uint32_t, kMaxFanOut> target{};
        std::array<std::uint8_t, kMaxFanOut> key{};
        std::uint8_t count{0};
    };
    [[nodiscard]] Sounding& sounding(const MidiEvent& event) noexcept;

    std::span<const MidiRoute> m_routes;
    /// [device][channel][key].
    std::array<std::array<std::array<Sounding, 128>, 16>, kDevices> m_sounding{};
};

} // namespace adx::midi
