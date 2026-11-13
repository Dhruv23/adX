#include "engine/instruments/midiout/MidiOutInstrument.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "engine/midi/LiveOutputs.h"
#include "engine/midi/MidiOutput.h"

namespace adx::instruments {

namespace {

[[nodiscard]] int intParam(const graph::ProcessContext& c, MidiOutParam p) noexcept {
    return static_cast<int>(std::lround(
        graph::paramAt(c, graph::kChannelParamCount + static_cast<std::uint32_t>(p), 0)));
}

} // namespace

void MidiOutInstrument::send(std::int8_t port, std::uint64_t frame, std::uint8_t status,
                             std::uint8_t data1, std::uint8_t data2) noexcept {
    if (port < 0 || std::cmp_greater_equal(port, midi::kLiveOutputSlots)) {
        return;
    }
    midi::MidiOutput* output = midi::liveOutputs()
                                   .outputs.at(static_cast<std::size_t>(port))
                                   .load(std::memory_order_acquire);
    if (output == nullptr) {
        return;
    }
    midi::OutMessage message{.streamFrame = frame, .bytes = {}, .size = 3};
    message.bytes[0] = status;
    message.bytes[1] = data1;
    message.bytes[2] = data2;
    static_cast<void>(output->push(message));
}

void MidiOutInstrument::release(std::size_t index, std::uint64_t frame) noexcept {
    const Held& h = m_held.at(index);
    send(h.port, frame, static_cast<std::uint8_t>(0x80U | h.channel), h.key, 0);
    m_held.at(index) = m_held.at(m_heldCount - 1);
    --m_heldCount;
}

bool MidiOutInstrument::renderWhole(const graph::ProcessContext& context, std::span<float> left,
                                    std::span<float> right, bool releaseAll) noexcept {
    std::ranges::fill(left, 0.0F);
    std::ranges::fill(right, 0.0F);
    const std::uint64_t block = midi::liveOutputs().blockFrame.load(std::memory_order_acquire);
    if (releaseAll) {
        while (m_heldCount > 0) {
            release(m_heldCount - 1, block);
        }
    }
    const auto port =
        static_cast<std::int8_t>(std::clamp(intParam(context, MidiOutParam::Port), -1, 7));
    const auto channel =
        static_cast<std::uint8_t>(std::clamp(intParam(context, MidiOutParam::Channel), 1, 16) - 1);
    for (const graph::BlockEvent& e : context.events) {
        const std::uint64_t at = block + e.offset;
        switch (e.kind) {
        case graph::BlockEventKind::NoteOn:
            if (m_heldCount < kMaxHeld) {
                m_held.at(m_heldCount++) = Held{.noteId = e.noteId,
                                                .instance = e.instance,
                                                .source = e.timeSource,
                                                .endTick = e.endTick,
                                                .key = e.pitch,
                                                .channel = channel,
                                                .port = port};
                send(port, at, static_cast<std::uint8_t>(0x90U | channel), e.pitch,
                     std::max<std::uint8_t>(e.velocity, 1));
            }
            break;
        case graph::BlockEventKind::NoteOff:
            for (std::size_t i = 0; i < m_heldCount; ++i) {
                if (m_held.at(i).noteId == e.noteId && m_held.at(i).instance == e.instance) {
                    release(i, at);
                    break;
                }
            }
            break;
        case graph::BlockEventKind::ReleaseAll:
            for (std::size_t i = m_heldCount; i-- > 0;) {
                if (m_held.at(i).source == e.timeSource) {
                    release(i, at);
                }
            }
            break;
        case graph::BlockEventKind::LoopWrap:
            for (std::size_t i = m_heldCount; i-- > 0;) {
                if (m_held.at(i).source == e.timeSource && m_held.at(i).endTick >= e.endTick) {
                    release(i, at);
                }
            }
            break;
        case graph::BlockEventKind::PitchGlide:
        case graph::BlockEventKind::Lyric:
        case graph::BlockEventKind::Controller:
            break;
        }
    }
    return true;
}

} // namespace adx::instruments
