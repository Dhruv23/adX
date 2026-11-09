#include "engine/instruments/dx7/Dx7Instrument.h"

#include "engine/dsp/Math.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace adx::dx7 {

namespace {

constexpr std::uint32_t kPitch = static_cast<std::uint32_t>(graph::ChannelParam::PitchCents);
/// msfa's int32 mix at the int16 converter's full scale (upstream: >> 4, then >> 9).
constexpr float kFullScale = 1.0F / static_cast<float>(1 << 28);
/// The interpolator's look-ahead in FM-1-rate mode, in internal samples.
constexpr double kLookAhead = 3.0;

[[nodiscard]] std::uint64_t tagOf(const graph::BlockEvent& e) noexcept {
    return (static_cast<std::uint64_t>(e.noteId) << 32) | e.instance;
}

[[nodiscard]] float param(const graph::ProcessContext& context, Dx7Param p,
                          std::uint32_t frame) noexcept {
    return graph::paramAt(context, graph::kChannelParamCount + paramIndex(p), frame);
}

[[nodiscard]] int intParam(const graph::ProcessContext& context, Dx7Param p,
                           std::uint32_t frame) noexcept {
    return static_cast<int>(std::lround(param(context, p, frame)));
}

/// Catmull-Rom through four samples, at `t` between the middle two.
[[nodiscard]] float hermite(float xm1, float x0, float x1, float x2, float t) noexcept {
    const float c1 = 0.5F * (x1 - xm1);
    const float c2 = xm1 - (2.5F * x0) + (2.0F * x1) - (0.5F * x2);
    const float c3 = (0.5F * (x2 - xm1)) + (1.5F * (x0 - x1));
    return (((((c3 * t) + c2) * t) + c1) * t) + x0;
}

} // namespace

void Dx7Instrument::prepare(const graph::PrepareInfo& info) {
    ChannelNode::prepare(info);
    m_hostRate = static_cast<double>(info.sampleRate);
    m_engines[0].prepare(m_hostRate);
    m_engines[1].prepare(kFm1Rate);
    reset();
}

void Dx7Instrument::reset() noexcept {
    ChannelNode::reset();
    for (Dx7Engine& engine : m_engines) {
        engine.reset();
    }
    m_pendingCount = 0;
    m_awake = false;
    m_now = 0;
    m_produced = 0;
    m_ring.fill(0.0F);
}

void Dx7Instrument::readParams(const graph::ProcessContext& context, std::uint32_t frame) noexcept {
    Dx7Engine& engine = m_engines[m_mode];
    Dx7Voice voice;
    std::array<float, kVoiceBytes> values{};
    for (std::uint32_t i = 0; i < kVoiceBytes; ++i) {
        values[i] = graph::paramAt(context, graph::kChannelParamCount + i, frame);
    }
    voiceFromParams(values, voice);
    if (voice != engine.voice()) {
        engine.setVoice(voice);
    }
    Dx7Function function;
    function.mono = intParam(context, Dx7Param::Mono, frame) != 0;
    function.bendRange = intParam(context, Dx7Param::BendRange, frame);
    function.bendStep = intParam(context, Dx7Param::BendStep, frame);
    function.portaMode = intParam(context, Dx7Param::PortaMode, frame);
    function.portaGliss = intParam(context, Dx7Param::PortaGliss, frame) != 0;
    function.portaTime = intParam(context, Dx7Param::PortaTime, frame);
    constexpr std::array<Dx7Param, 4> kRanges = {Dx7Param::WheelRange, Dx7Param::FootRange,
                                                 Dx7Param::BreathRange, Dx7Param::AftertouchRange};
    constexpr std::array<Dx7Param, 4> kAssigns = {Dx7Param::WheelAssign, Dx7Param::FootAssign,
                                                  Dx7Param::BreathAssign,
                                                  Dx7Param::AftertouchAssign};
    constexpr std::array<Dx7Param, 4> kValues = {Dx7Param::CtlWheel, Dx7Param::CtlFoot,
                                                 Dx7Param::CtlBreath, Dx7Param::CtlAftertouch};
    for (std::size_t c = 0; c < 4; ++c) {
        function.range[c] = intParam(context, kRanges[c], frame);
        function.assign[c] = intParam(context, kAssigns[c], frame);
        engine.setController(static_cast<int>(c), intParam(context, kValues[c], frame));
    }
    // The parameter, under the channel's own voice ceiling.
    function.polyphony =
        std::min(intParam(context, Dx7Param::Polyphony, frame), static_cast<int>(maxPolyphony()));
    engine.setFunction(function);
    engine.setPitchBend(
        std::clamp(intParam(context, Dx7Param::CtlBend, frame) + 0x2000, 0, 0x3FFF));
    const bool sustain = intParam(context, Dx7Param::CtlSustain, frame) != 0;
    if (sustain != m_sustain) {
        m_sustain = sustain;
        engine.setSustain(sustain);
    }
    const float cents = param(context, Dx7Param::PartTuning, frame) +
                        graph::paramAt(context, kPitch, frame) + context.tuningCents;
    engine.setTuning(static_cast<std::int32_t>(std::lround(cents * (1 << 24) / 1200.0F)));
    const float level = std::clamp(param(context, Dx7Param::PartLevel, frame), 0.0F, 127.0F);
    const float db = param(context, Dx7Param::Gain, frame);
    m_gain = (level / 127.0F) * dsp::dbToGainF(db);
    // Equal power, unity at the centre (the FM-1's part pan, -64..63).
    const float pan = std::clamp(param(context, Dx7Param::PartPan, frame), -64.0F, 64.0F);
    if (pan == 0.0F) {
        m_panLeft = 1.0F;
        m_panRight = 1.0F;
    } else {
        const float turns = ((pan / 64.0F) + 1.0F) / 8.0F; // 0..1/4 of a turn
        m_panLeft = dsp::cosTurnsF(turns) * std::numbers::sqrt2_v<float>;
        m_panRight = dsp::sinTurnsF(turns) * std::numbers::sqrt2_v<float>;
    }
}

void Dx7Instrument::apply(const graph::BlockEvent& event) noexcept {
    Dx7Engine& engine = m_engines[m_mode];
    switch (event.kind) {
    case graph::BlockEventKind::NoteOn:
        engine.noteOn(event.pitch, std::max<int>(event.velocity, 1), tagOf(event), event.timeSource,
                      event.endTick);
        break;
    case graph::BlockEventKind::NoteOff:
        engine.noteOff(tagOf(event));
        break;
    case graph::BlockEventKind::ReleaseAll:
        engine.releaseAll(event.timeSource);
        break;
    case graph::BlockEventKind::LoopWrap:
        engine.releaseFrom(event.timeSource, event.endTick);
        break;
    case graph::BlockEventKind::PitchGlide:
    case graph::BlockEventKind::Lyric:
        break; // slides and pitch curves: not on a dx7 (phase_11.5.md §9)
    }
}

void Dx7Instrument::wake(std::int64_t at) noexcept {
    m_engines[m_mode].reset();
    m_sustain = false;
    m_wakeAt = at;
    m_produced = 0;
    m_ring.fill(0.0F);
    m_awake = true;
}

void Dx7Instrument::renderBlock(const graph::ProcessContext& context,
                                std::uint32_t frame) noexcept {
    readParams(context, frame);
    // Every pending event due at or before this block's start, in order.
    const double lookAhead = m_mode == 0 ? 0.0 : kLookAhead;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < m_pendingCount; ++i) {
        const Pending& p = m_pending[i];
        const double internal = static_cast<double>(p.at - m_wakeAt) * m_ratio;
        const bool due =
            p.at <= m_wakeAt || internal + lookAhead <= static_cast<double>(m_produced);
        if (due) {
            apply(p.event);
        } else {
            m_pending[kept++] = p;
        }
    }
    m_pendingCount = kept;
    m_engines[m_mode].render(m_block.data());
    const auto base = static_cast<std::size_t>(m_produced % static_cast<std::int64_t>(kRing));
    for (std::size_t i = 0; i < m_block.size(); ++i) {
        m_ring[base + i] = static_cast<float>(m_block[i]) * kFullScale;
    }
    m_produced += msfa::kN;
}

float Dx7Instrument::sampleAt(std::int64_t hostFrame) noexcept {
    const auto ring = static_cast<std::int64_t>(kRing);
    const auto at = [this](std::int64_t i) {
        return i < 0 ? 0.0F : m_ring[static_cast<std::size_t>(i % ring)];
    };
    if (m_mode == 0) {
        return at(hostFrame - m_wakeAt);
    }
    const double p = static_cast<double>(hostFrame - m_wakeAt) * m_ratio;
    const auto i = static_cast<std::int64_t>(std::floor(p));
    const auto t = static_cast<float>(p - static_cast<double>(i));
    return hermite(at(i - 1), at(i), at(i + 1), at(i + 2), t);
}

bool Dx7Instrument::renderWhole(const graph::ProcessContext& context, std::span<float> left,
                                std::span<float> right, bool releaseAll) noexcept {
    const std::size_t mode =
        graph::paramAt(context, graph::kChannelParamCount + paramIndex(Dx7Param::Fm1Rate), 0) >=
                0.5F
            ? 1
            : 0;
    if (mode != m_mode) {
        m_mode = mode;
        m_awake = false;
        m_pendingCount = 0;
        for (Dx7Engine& engine : m_engines) {
            engine.reset();
        }
    }
    m_ratio = m_mode == 0 ? 1.0 : kFm1Rate / m_hostRate;
    if (releaseAll) {
        m_engines[m_mode].releaseAll(Dx7Engine::kAllSources);
        m_pendingCount = 0;
    }
    for (const graph::BlockEvent& event : context.events) {
        if (event.kind == graph::BlockEventKind::PitchGlide ||
            event.kind == graph::BlockEventKind::Lyric) {
            continue;
        }
        if (m_pendingCount == m_pending.size()) {
            ++m_droppedEvents;
            continue;
        }
        m_pending[m_pendingCount++] =
            Pending{.at = m_now + static_cast<std::int64_t>(event.offset), .event = event};
    }
    for (std::uint32_t f = 0; f < context.frames; ++f) {
        const std::int64_t host = m_now + f;
        // A note-on reaching an idle instrument restarts the grid at itself. Every
        // decision here reads only events already known (a later buffer's are all after
        // this frame) and the engine's state, so it is the same at any block size.
        if (m_engines[m_mode].idle()) {
            bool now = false;
            bool earlier = false;
            for (std::size_t i = 0; i < m_pendingCount; ++i) {
                const Pending& p = m_pending[i];
                if (p.event.kind == graph::BlockEventKind::NoteOn) {
                    now = now || p.at == host;
                    earlier = earlier || p.at < host;
                }
            }
            if (now && !earlier) {
                std::size_t kept = 0;
                for (std::size_t i = 0; i < m_pendingCount; ++i) {
                    if (m_pending[i].at >= host) {
                        m_pending[kept++] = m_pending[i];
                    }
                }
                m_pendingCount = kept;
                wake(host);
            }
        }
        if (!m_awake) {
            left[f] = 0.0F;
            right[f] = 0.0F;
            continue;
        }
        const double internal = static_cast<double>(host - m_wakeAt) * m_ratio;
        const auto need = static_cast<std::int64_t>(std::floor(internal)) + (m_mode == 0 ? 0 : 2);
        if (need >= m_produced && m_engines[m_mode].idle() &&
            std::none_of(m_pending.begin(),
                         m_pending.begin() + static_cast<std::ptrdiff_t>(m_pendingCount),
                         [host](const Pending& p) { return p.at <= host; })) {
            // Played out: nothing sounds and nothing is due. A voice is freed only after
            // a block it added nothing to, so what would follow is silence.
            m_awake = false;
            left[f] = 0.0F;
            right[f] = 0.0F;
            continue;
        }
        while (need >= m_produced) {
            renderBlock(context, f);
        }
        const float s = sampleAt(host) * m_gain;
        left[f] = s * m_panLeft;
        right[f] = s * m_panRight;
    }
    m_now += context.frames;
    return true;
}

} // namespace adx::dx7
