// adx-thread: realtime
#include "engine/midi/MidiRouter.h"

#include <algorithm>
#include <cmath>

namespace adx::midi {

std::uint8_t curveVelocity(float curve, std::uint8_t velocity) noexcept {
    if (velocity == 0) {
        return 0;
    }
    if (curve == 0.0F) {
        return velocity;
    }
    const float bend = std::exp2(-std::clamp(curve, -1.0F, 1.0F));
    const float shaped = 127.0F * std::pow(static_cast<float>(velocity) / 127.0F, bend);
    return static_cast<std::uint8_t>(std::clamp(std::lround(shaped), 1L, 127L));
}

MidiRouter::Sounding& MidiRouter::sounding(const MidiEvent& event) noexcept {
    return m_sounding[event.device % kDevices][event.channel()][event.data1 & 0x7FU];
}

std::size_t MidiRouter::route(const MidiEvent& event, std::span<RoutedEvent> out) noexcept {
    const std::size_t limit = std::min(out.size(), kMaxFanOut);
    std::size_t written = 0;

    if (event.isNoteOff()) {
        Sounding& held = sounding(event);
        for (std::uint8_t i = 0; i < held.count && written < limit; ++i) {
            MidiEvent off = event;
            off.status = static_cast<std::uint8_t>(0x80U | event.channel());
            off.data1 = held.key.at(i);
            off.data2 = event.kind() == MidiKind::NoteOff ? event.data2 : std::uint8_t{64};
            out[written++] = RoutedEvent{.target = held.target.at(i), .event = off};
        }
        held.count = 0;
        return written;
    }

    const bool system = event.kind() == MidiKind::System;
    Sounding* held = event.isNoteOn() ? &sounding(event) : nullptr;
    if (held != nullptr) {
        held->count = 0; // a retrigger without a note-off replaces the old one
    }
    for (const MidiRoute& route : m_routes) {
        if (written >= limit) {
            break;
        }
        if (route.device != kAnyDevice && route.device != event.device) {
            continue;
        }
        if (!system && (route.channels & (1U << event.channel())) == 0) {
            continue;
        }
        MidiEvent routed = event;
        if (held != nullptr) {
            if (event.data1 < route.keyLow || event.data1 > route.keyHigh ||
                event.data2 < route.velocityLow || event.data2 > route.velocityHigh) {
                continue;
            }
            const int key = static_cast<int>(event.data1) + route.transpose;
            if (key < 0 || key > 127) {
                continue;
            }
            routed.data1 = static_cast<std::uint8_t>(key);
            routed.data2 = curveVelocity(route.curve, event.data2);
            held->target.at(held->count) = route.target;
            held->key.at(held->count) = routed.data1;
            ++held->count;
        } else if (event.kind() == MidiKind::PolyPressure) {
            const int key = static_cast<int>(event.data1) + route.transpose;
            if (key < 0 || key > 127) {
                continue;
            }
            routed.data1 = static_cast<std::uint8_t>(key);
        }
        out[written++] = RoutedEvent{.target = route.target, .event = routed};
    }
    return written;
}

void MidiRouter::reset() noexcept {
    for (auto& device : m_sounding) {
        for (auto& channel : device) {
            for (Sounding& key : channel) {
                key.count = 0;
            }
        }
    }
}

} // namespace adx::midi
