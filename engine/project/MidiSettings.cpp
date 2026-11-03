#include "engine/project/MidiSettings.h"

namespace adx::project {

const char* toString(MidiControlKind kind) noexcept {
    switch (kind) {
    case MidiControlKind::ControlChange:
        return "cc";
    case MidiControlKind::Note:
        return "note";
    case MidiControlKind::PitchBend:
        return "bend";
    case MidiControlKind::ChannelPressure:
        return "pressure";
    }
    return "cc";
}

bool midiControlKindFromString(std::string_view text, MidiControlKind& out) noexcept {
    if (text == "cc") {
        out = MidiControlKind::ControlChange;
    } else if (text == "note") {
        out = MidiControlKind::Note;
    } else if (text == "bend") {
        out = MidiControlKind::PitchBend;
    } else if (text == "pressure") {
        out = MidiControlKind::ChannelPressure;
    } else {
        return false;
    }
    return true;
}

} // namespace adx::project
