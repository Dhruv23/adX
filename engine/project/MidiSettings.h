// A project's MIDI setup (phase_9.md §4.1, §3 step 2): which input plays which channel,
// and which controller moves which parameter.
//
// Both are project settings, saved in the `[MIDI]` section and diffable like anything
// else. A learned binding's target is a ParamRef, resolved at load and written back out
// through ParamRegistry::pathOf - so renaming the channel it points into keeps the
// binding, exactly as it keeps an automation lane.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/Ids.h"
#include "engine/project/MidiControl.h"
#include "engine/project/ParamRef.h"

namespace adx::project {

/// "cc", "note", "bend", "pressure": how the file names a kind.
[[nodiscard]] const char* toString(MidiControlKind kind) noexcept;
[[nodiscard]] bool midiControlKindFromString(std::string_view text, MidiControlKind& out) noexcept;

struct MidiBinding {
    /// An input's name as the system reports it; empty for any input.
    std::string device;
    /// 0..15, or kAnyMidiChannel.
    std::uint8_t channel{kAnyMidiChannel};
    MidiControlKind kind{MidiControlKind::ControlChange};
    std::uint8_t number{0};
    /// The path as the file spelled it, kept for a target that no longer resolves.
    std::string targetPath;
    ParamRef target;
    /// The parameter's value at controller 0 and at controller full scale. Reversed
    /// (minimum > maximum) inverts the control.
    float minimum{0.0F};
    float maximum{1.0F};
    /// A note binding that flips between minimum and maximum on each press.
    bool toggle{false};

    [[nodiscard]] friend bool operator==(const MidiBinding&, const MidiBinding&) noexcept = default;
};

/// One input route (midi::MidiRoute's project form). A project with none plays the
/// selected channel from every input, which is what a keyboard plugged in expects.
struct MidiRouteSetting {
    std::string device;
    /// Bit c: MIDI channel c passes.
    std::uint16_t channels{0xFFFF};
    std::uint8_t keyLow{0};
    std::uint8_t keyHigh{127};
    std::uint8_t velocityLow{1};
    std::uint8_t velocityHigh{127};
    std::int8_t transpose{0};
    float curve{0.0F};
    core::ChannelId target;

    [[nodiscard]] friend bool operator==(const MidiRouteSetting&,
                                         const MidiRouteSetting&) noexcept = default;
};

struct MidiSettings {
    std::vector<MidiRouteSetting> routes;
    std::vector<MidiBinding> bindings;

    [[nodiscard]] bool empty() const noexcept {
        return routes.empty() && bindings.empty();
    }
    [[nodiscard]] friend bool operator==(const MidiSettings&,
                                         const MidiSettings&) noexcept = default;
};

} // namespace adx::project
