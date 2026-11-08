// adx-thread: realtime
#include "engine/midi/MidiSync.h"

#include <algorithm>
#include <cmath>

#include "engine/midi/MidiClock.h"

namespace adx::midi {

namespace {

SyncMessage status(std::uint8_t byte, std::uint32_t offset) noexcept {
    SyncMessage out{.offset = offset};
    out.bytes[0] = byte;
    out.size = 1;
    return out;
}

SyncMessage songPosition(std::int64_t tick, std::uint32_t offset) noexcept {
    const ClockMessage spp = songPositionAt(tick, offset);
    SyncMessage out{.offset = offset};
    out.bytes[0] = spp.status;
    out.bytes[1] = spp.data1;
    out.bytes[2] = spp.data2;
    out.size = 3;
    return out;
}

/// A jump is a block that does not start where the last one ended. Half a clock pulse
/// of slack absorbs the rounding of a position derived from samples.
bool jumped(std::int64_t from, std::int64_t lastTo, std::int64_t slack) noexcept {
    return std::abs(from - lastTo) > slack;
}

struct Writer {
    std::span<SyncMessage> out;
    std::size_t count{0};
    void add(const SyncMessage& message) noexcept {
        if (count < out.size()) {
            out[count++] = message;
        }
    }
};

/// MTC's rate code in the top bits of the hours byte: 0 = 24, 1 = 25, 2 = 29.97 drop,
/// 3 = 30 fps.
constexpr std::uint8_t kRate30 = 3;

} // namespace

std::size_t ClockSender::block(const SyncBlock& block, std::span<SyncMessage> out) noexcept {
    Writer writer{.out = out};
    if (block.rolling) {
        if (!m_wasRolling) {
            if (block.fromTicks <= 0) {
                writer.add(status(kClockStart, 0));
            } else {
                writer.add(songPosition(block.fromTicks, 0));
                writer.add(status(kClockContinue, 0));
            }
        } else if (jumped(block.fromTicks, m_lastTo, kTicksPerPulse / 2)) {
            writer.add(status(kClockStop, 0));
            writer.add(songPosition(block.fromTicks, 0));
            writer.add(status(kClockContinue, 0));
        }
        // Every pulse whose sample falls in [fromSamples, toSamples). The tick range is
        // widened by a pulse each way, since ticks derived from samples are rounded.
        const double rate = block.rate > 0.0 ? block.rate : 1.0;
        std::int64_t tick =
            ((std::max<std::int64_t>(0, block.fromTicks - kTicksPerPulse) + kTicksPerPulse - 1) /
             kTicksPerPulse) *
            kTicksPerPulse;
        for (; tick <= block.toTicks + kTicksPerPulse; tick += kTicksPerPulse) {
            const std::int64_t at =
                block.tempo.toSamples(core::Ticks{tick}, block.sampleRate).value;
            if (at < block.fromSamples || at >= block.toSamples) {
                continue;
            }
            auto offset = static_cast<std::uint32_t>(
                std::floor(static_cast<double>(at - block.fromSamples) / rate));
            writer.add(status(kClockPulse, std::min(offset, block.frames - 1)));
        }
    } else if (m_wasRolling) {
        writer.add(status(kClockStop, 0));
    }
    m_wasRolling = block.rolling;
    m_lastTo = block.toTicks;
    return writer.count;
}

Timecode timecodeOf(std::int64_t frame, std::uint32_t fps) noexcept {
    const std::int64_t f = std::max<std::int64_t>(0, frame);
    const std::int64_t perHour = static_cast<std::int64_t>(fps) * 3600;
    const std::int64_t inDay = f % (perHour * 24);
    return Timecode{
        .hours = static_cast<std::uint8_t>(inDay / perHour),
        .minutes = static_cast<std::uint8_t>((inDay / (static_cast<std::int64_t>(fps) * 60)) % 60),
        .seconds = static_cast<std::uint8_t>((inDay / fps) % 60),
        .frames = static_cast<std::uint8_t>(inDay % fps)};
}

SyncMessage fullFrame(const Timecode& code, std::uint32_t offset) noexcept {
    SyncMessage out{.offset = offset};
    out.bytes = {0xF0,         0x7F,
                 0x7F,         0x01,
                 0x01,         static_cast<std::uint8_t>((kRate30 << 5U) | (code.hours & 0x1FU)),
                 code.minutes, code.seconds,
                 code.frames,  0xF7};
    out.size = 10;
    return out;
}

SyncMessage quarterFrame(const Timecode& code, std::uint32_t piece, std::uint32_t offset) noexcept {
    std::uint8_t nibble = 0;
    switch (piece & 7U) {
    case 0:
        nibble = code.frames & 0x0FU;
        break;
    case 1:
        nibble = (code.frames >> 4U) & 0x01U;
        break;
    case 2:
        nibble = code.seconds & 0x0FU;
        break;
    case 3:
        nibble = (code.seconds >> 4U) & 0x03U;
        break;
    case 4:
        nibble = code.minutes & 0x0FU;
        break;
    case 5:
        nibble = (code.minutes >> 4U) & 0x03U;
        break;
    case 6:
        nibble = code.hours & 0x0FU;
        break;
    default:
        nibble = static_cast<std::uint8_t>((kRate30 << 1U) | ((code.hours >> 4U) & 0x01U));
    }
    SyncMessage out{.offset = offset};
    out.bytes[0] = kMtcQuarterFrame;
    out.bytes[1] = static_cast<std::uint8_t>(((piece & 7U) << 4U) | nibble);
    out.size = 2;
    return out;
}

std::size_t MtcSender::block(const SyncBlock& block, std::span<SyncMessage> out) noexcept {
    Writer writer{.out = out};
    if (!block.rolling || block.frames == 0) {
        m_wasRolling = false;
        m_lastTo = block.toSamples;
        return 0;
    }
    const double sr = block.sampleRate;
    const double quarterSamples = sr / (4.0 * kMtcFps);
    // Quarter frame q is due at timeline sample q * quarterSamples.
    if (!m_wasRolling ||
        jumped(block.fromSamples, m_lastTo, static_cast<std::int64_t>(quarterSamples))) {
        const auto frame = static_cast<std::int64_t>(
            std::floor(static_cast<double>(block.fromSamples) / (quarterSamples * 4.0)));
        writer.add(fullFrame(timecodeOf(frame, kMtcFps), 0));
    }
    const double rate = block.rate > 0.0 ? block.rate : 1.0;
    auto q = static_cast<std::int64_t>(
        std::ceil(static_cast<double>(block.fromSamples) / quarterSamples));
    for (;; ++q) {
        const double at = static_cast<double>(q) * quarterSamples;
        if (at >= static_cast<double>(block.toSamples)) {
            break;
        }
        const auto piece = static_cast<std::uint32_t>(q % 8);
        // The code a run of eight describes is the frame its piece 0 went out on.
        const std::int64_t frame = (q - static_cast<std::int64_t>(piece)) / 4;
        auto offset = static_cast<std::uint32_t>(
            std::floor((at - static_cast<double>(block.fromSamples)) / rate));
        offset = std::min(offset, block.frames - 1);
        writer.add(quarterFrame(timecodeOf(frame, kMtcFps), piece, offset));
    }
    m_wasRolling = true;
    m_lastTo = block.toSamples;
    return writer.count;
}

void MtcFollower::quarter(std::uint8_t data, std::uint64_t at, std::uint32_t sampleRate) noexcept {
    const std::uint32_t piece = (data >> 4U) & 7U;
    m_lastQuarter = at;
    if (piece != m_expect) {
        // Out of sequence: wait for the next piece 0.
        m_seen = 0;
        m_expect = 0;
        if (piece != 0) {
            return;
        }
    }
    m_pieces.at(piece) = data & 0x0FU;
    m_seen |= 1U << piece;
    m_expect = (piece + 1) & 7U;
    if (piece != 7 || m_seen != 0xFFU) {
        return;
    }
    m_seen = 0;
    const std::uint32_t frames = m_pieces[0] | ((m_pieces[1] & 1U) << 4U);
    const std::uint32_t seconds = m_pieces[2] | ((m_pieces[3] & 3U) << 4U);
    const std::uint32_t minutes = m_pieces[4] | ((m_pieces[5] & 3U) << 4U);
    const std::uint32_t hours = m_pieces[6] | ((m_pieces[7] & 1U) << 4U);
    static constexpr std::array<double, 4> kFps{24.0, 25.0, 29.97, 30.0};
    const double fps = kFps.at((m_pieces[7] >> 1U) & 3U);
    // Piece 0 went out on the frame this code names; piece 7 is seven quarter frames on.
    const double seconds0 = (hours * 3600.0) + (minutes * 60.0) + seconds + (frames / fps);
    const double now = seconds0 + (7.0 / (4.0 * fps));
    if (m_codes > 0 && at > m_codeAt) {
        const double heard = static_cast<double>(at - m_codeAt) / sampleRate;
        const double coded = now - m_codeSeconds;
        if (heard > 0.0 && coded > 0.0 && coded < 1.0) {
            // A slow average: one code is two frames, and its arrival jitters by a block.
            m_rate += 0.2 * ((coded / heard) - m_rate);
        }
    }
    m_codeSeconds = now;
    m_codeAt = at;
    ++m_codes;
}

bool MtcFollower::running(std::uint64_t now, std::uint32_t sampleRate) const noexcept {
    // A quarter frame drained this block may be stamped later in it than `now`.
    return m_codes > 0 && (now < m_lastQuarter || (now - m_lastQuarter) < sampleRate / 10);
}

double MtcFollower::secondsAt(std::uint64_t now, std::uint32_t sampleRate) const noexcept {
    const double since = now >= m_codeAt ? static_cast<double>(now - m_codeAt) / sampleRate
                                         : -static_cast<double>(m_codeAt - now) / sampleRate;
    return m_codeSeconds + (since * m_rate);
}

} // namespace adx::midi
