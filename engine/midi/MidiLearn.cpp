// adx-thread: realtime
#include "engine/midi/MidiLearn.h"

#include <cmath>

#include "engine/midi/MidiRouter.h"

namespace adx::midi {

bool controlOf(const MidiEvent& event, ControlMessage& out) noexcept {
    out.device = event.device;
    out.channel = event.channel();
    out.pressed = false;
    switch (event.kind()) {
    case MidiKind::ControlChange:
        out.kind = project::MidiControlKind::ControlChange;
        out.number = event.data1 & 0x7FU;
        out.value = static_cast<float>(event.data2 & 0x7FU) / 127.0F;
        return true;
    case MidiKind::NoteOn:
    case MidiKind::NoteOff:
        out.kind = project::MidiControlKind::Note;
        out.number = event.data1 & 0x7FU;
        out.pressed = event.isNoteOn();
        out.value = out.pressed ? static_cast<float>(event.data2 & 0x7FU) / 127.0F : 0.0F;
        return true;
    case MidiKind::PitchBend: {
        out.kind = project::MidiControlKind::PitchBend;
        out.number = 0;
        const auto raw = static_cast<std::uint32_t>(event.data1 & 0x7FU) |
                         (static_cast<std::uint32_t>(event.data2 & 0x7FU) << 7U);
        out.value = static_cast<float>(raw) / 16383.0F;
        return true;
    }
    case MidiKind::ChannelPressure:
        out.kind = project::MidiControlKind::ChannelPressure;
        out.number = 0;
        out.value = static_cast<float>(event.data1 & 0x7FU) / 127.0F;
        return true;
    default:
        return false;
    }
}

bool matches(const CompiledBinding& binding, const ControlMessage& message) noexcept {
    return binding.kind == message.kind && binding.number == message.number &&
           (binding.channel == project::kAnyMidiChannel || binding.channel == message.channel) &&
           (binding.device == kAnyDevice || binding.device == message.device);
}

bool valueFor(const CompiledBinding& binding, const ControlMessage& message, bool& toggled,
              float& out) noexcept {
    if (binding.kind == project::MidiControlKind::Note && binding.toggle) {
        if (!message.pressed) {
            return false;
        }
        toggled = !toggled;
        out = toggled ? binding.maximum : binding.minimum;
        return true;
    }
    out = binding.minimum + ((binding.maximum - binding.minimum) * message.value);
    return true;
}

void LastControl::publish(const ControlMessage& message) noexcept {
    const auto value = static_cast<std::uint64_t>(std::lround(message.value * 16383.0F)) & 0x3FFFU;
    const std::uint64_t packed = static_cast<std::uint64_t>(message.device) |
                                 (static_cast<std::uint64_t>(message.channel) << 8U) |
                                 (static_cast<std::uint64_t>(message.kind) << 16U) |
                                 (static_cast<std::uint64_t>(message.number) << 24U) |
                                 (value << 32U) |
                                 (static_cast<std::uint64_t>(message.pressed ? 1 : 0) << 48U);
    m_packed.store(packed, std::memory_order_relaxed);
    m_sequence.fetch_add(1, std::memory_order_release);
}

bool LastControl::read(std::uint64_t& sequence, ControlMessage& out) const noexcept {
    const std::uint64_t now = m_sequence.load(std::memory_order_acquire);
    if (now == sequence) {
        return false;
    }
    sequence = now;
    const std::uint64_t packed = m_packed.load(std::memory_order_relaxed);
    out.device = static_cast<std::uint8_t>(packed & 0xFFU);
    out.channel = static_cast<std::uint8_t>((packed >> 8U) & 0xFFU);
    out.kind = static_cast<project::MidiControlKind>((packed >> 16U) & 0xFFU);
    out.number = static_cast<std::uint8_t>((packed >> 24U) & 0xFFU);
    out.value = static_cast<float>((packed >> 32U) & 0x3FFFU) / 16383.0F;
    out.pressed = ((packed >> 48U) & 1U) != 0;
    return true;
}

} // namespace adx::midi
