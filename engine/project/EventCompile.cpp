// adx-thread: main
#include "engine/project/EventCompile.h"

#include <algorithm>
#include <cmath>

#include "engine/core/Time.h"
#include "engine/project/CurveCompile.h"

namespace adx::project {
namespace {

/// Half a cent: below anything audible, and well inside the 0.1 % phase_4.md asks of
/// a parameter whose range is several octaves.
constexpr float kPitchTolerance = 0.5F;

/// The value a chain approaches at `tick` from the left. Differs from knotValueAt only
/// at a jump, where it is the value *before* the jump.
[[nodiscard]] float leftValueAt(const std::vector<Knot>& chain, std::int64_t tick) noexcept {
    if (chain.empty()) {
        return 0.0F;
    }
    if (tick <= chain.front().tick) {
        return chain.front().value;
    }
    const auto at = std::ranges::lower_bound(chain, tick, {}, &Knot::tick);
    if (at == chain.end()) {
        return chain.back().value;
    }
    if (at->tick == tick) {
        return at->value;
    }
    const Knot& from = *(at - 1);
    const auto fraction =
        static_cast<double>(tick - from.tick) / static_cast<double>(at->tick - from.tick);
    return static_cast<float>(static_cast<double>(from.value) +
                              (static_cast<double>(at->value - from.value) * fraction));
}

[[nodiscard]] std::vector<Knot> slideChain(const NoteSlide& slide) {
    std::vector<Knot> chain{Knot{.tick = 0, .value = 0.0F}};
    if (slide.start.value > 0) {
        chain.push_back(Knot{.tick = slide.start.value, .value = 0.0F});
    }
    appendCurveSegment(chain, slide.start.value, 0.0F, slide.start.value + slide.length.value,
                       static_cast<float>(slide.targetCents), slide.curve, kPitchTolerance);
    return chain;
}

[[nodiscard]] std::vector<Knot> curveChain(const std::vector<PitchPoint>& points) {
    std::vector<Knot> chain;
    if (points.empty()) {
        return chain;
    }
    chain.push_back(
        Knot{.tick = points.front().at.value, .value = static_cast<float>(points.front().cents)});
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        appendCurveSegment(chain, points[i].at.value, static_cast<float>(points[i].cents),
                           points[i + 1].at.value, static_cast<float>(points[i + 1].cents),
                           points[i].curve, kPitchTolerance);
    }
    return chain;
}

std::uint32_t xorshift(std::uint32_t state) noexcept {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    return state;
}

struct HeldNote {
    std::int64_t on{0};
    std::int64_t off{0};
    std::uint8_t pitch{0};
    std::uint8_t velocity{0};
};

/// Which chord member step `step` plays, for a chord of `total` (members x octaves).
[[nodiscard]] std::size_t arpIndex(ArpMode mode, std::int64_t step, std::size_t total) noexcept {
    const auto count = static_cast<std::int64_t>(total);
    const std::int64_t k = step % count;
    switch (mode) {
    case ArpMode::Down:
        return static_cast<std::size_t>(count - 1 - k);
    case ArpMode::UpDown:
    case ArpMode::DownUp: {
        const std::int64_t period = std::max<std::int64_t>(1, (2 * count) - 2);
        const std::int64_t p = step % period;
        const std::int64_t up = p < count ? p : (2 * count) - 2 - p;
        return static_cast<std::size_t>(mode == ArpMode::UpDown ? up : count - 1 - up);
    }
    case ArpMode::Random: {
        // Seeded by the step alone: the same step plays the same note on every render.
        const std::uint32_t r =
            xorshift((static_cast<std::uint32_t>(step) * 2654435761U) ^ 0x9E3779B9U);
        return static_cast<std::size_t>(r % static_cast<std::uint32_t>(count));
    }
    case ArpMode::Up:
    case ArpMode::Order:
    case ArpMode::Off:
        break;
    }
    return static_cast<std::size_t>(k);
}

} // namespace

bool eventLess(const ScheduledEvent& a, const ScheduledEvent& b) noexcept {
    if (a.tick != b.tick) {
        return a.tick < b.tick;
    }
    if (a.kind != b.kind) {
        return a.kind < b.kind;
    }
    if (a.noteId != b.noteId) {
        return a.noteId < b.noteId;
    }
    if (a.instance != b.instance) {
        return a.instance < b.instance;
    }
    return a.endTick < b.endTick;
}

std::vector<Knot> pitchOffsetChain(const NoteExtras& extras) {
    std::vector<Knot> slide =
        extras.slide.has_value() ? slideChain(*extras.slide) : std::vector<Knot>{};
    std::vector<Knot> curve = curveChain(extras.pitchCurve);
    if (slide.empty()) {
        return curve;
    }
    if (curve.empty()) {
        return slide;
    }
    // The slide first, the curve on top (phase_4.md §4.0): two straight-line chains
    // summed are a straight-line chain whose corners are the union of theirs.
    std::vector<std::int64_t> ticks;
    ticks.reserve(slide.size() + curve.size());
    for (const Knot& knot : slide) {
        ticks.push_back(knot.tick);
    }
    for (const Knot& knot : curve) {
        ticks.push_back(knot.tick);
    }
    std::ranges::sort(ticks);
    const auto [first, last] = std::ranges::unique(ticks);
    ticks.erase(first, last);

    std::vector<Knot> sum;
    for (const std::int64_t tick : ticks) {
        const float left = leftValueAt(slide, tick) + leftValueAt(curve, tick);
        const float right = knotValueAt(slide.data(), slide.size(), tick) +
                            knotValueAt(curve.data(), curve.size(), tick);
        if (left != right) {
            sum.push_back(Knot{.tick = tick, .value = left});
        }
        sum.push_back(Knot{.tick = tick, .value = right});
    }
    return sum;
}

void appendNoteExtras(std::vector<ScheduledEvent>& out, std::vector<std::string>& lyrics,
                      const NoteExtras& extras, std::int64_t onTick, std::int64_t offTick,
                      std::uint32_t noteId, std::uint32_t instance) {
    if (!extras.lyric.empty()) {
        out.push_back(ScheduledEvent{.tick = onTick,
                                     .endTick = static_cast<std::int64_t>(lyrics.size()),
                                     .noteId = noteId,
                                     .instance = instance,
                                     .value = 0.0F,
                                     .kind = EventKind::Lyric});
        lyrics.push_back(extras.lyric);
    }

    const std::vector<Knot> chain = pitchOffsetChain(extras);
    if (chain.empty()) {
        return;
    }
    const auto glide = [&](std::int64_t from, std::int64_t to, float value) {
        if (onTick + from >= offTick) {
            return;
        }
        out.push_back(ScheduledEvent{.tick = onTick + from,
                                     .endTick = onTick + std::max(from, to),
                                     .noteId = noteId,
                                     .instance = instance,
                                     .value = value,
                                     .kind = EventKind::PitchGlide});
    };

    // A voice starts at offset zero. If the chain is somewhere else at the note's
    // start - a pitch curve that begins bent - it jumps there with the note.
    const float atStart = knotValueAt(chain.data(), chain.size(), 0);
    if (atStart != 0.0F) {
        glide(0, 0, atStart);
    }
    for (std::size_t i = 0; i + 1 < chain.size(); ++i) {
        const Knot& a = chain[i];
        const Knot& b = chain[i + 1];
        if (b.tick <= 0) {
            continue; // before the note: already folded into atStart
        }
        const std::int64_t from = std::max<std::int64_t>(a.tick, 0);
        if (a.tick == b.tick) {
            glide(from, from, b.value);
        } else if (a.value != b.value || from != a.tick) {
            glide(from, b.tick, b.value);
        }
    }
}

void arpeggiate(std::vector<ScheduledEvent>& events, const ArpSettings& arp) {
    if (arp.mode == ArpMode::Off || arp.rate.numerator <= 0 || arp.rate.denominator <= 0) {
        return;
    }
    std::vector<HeldNote> held;
    for (const ScheduledEvent& event : events) {
        if (event.kind == EventKind::NoteOn) {
            held.push_back(HeldNote{.on = event.tick,
                                    .off = event.endTick,
                                    .pitch = event.pitch,
                                    .velocity = event.velocity});
        }
    }
    events.clear();
    if (held.empty()) {
        return;
    }
    std::ranges::stable_sort(held, {}, &HeldNote::on);

    // The step on the song's grid, from the rate as a fraction of a whole note. v1's
    // floor of a 64th is kept, so a rate typo cannot ask for millions of steps.
    const double whole = static_cast<double>(core::kPpq) * 4.0;
    const auto step = std::max<std::int64_t>(
        static_cast<std::int64_t>(std::llround(whole * static_cast<double>(arp.rate.numerator) /
                                               static_cast<double>(arp.rate.denominator))),
        static_cast<std::int64_t>(core::kPpq / 16));
    const auto gate = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(std::llround(
               static_cast<double>(step) * std::clamp(static_cast<double>(arp.gate), 0.05, 0.98))));
    const auto octaves = static_cast<std::size_t>(std::clamp<int>(arp.octaves, 1, 4));

    std::int64_t lastOff = 0;
    for (const HeldNote& note : held) {
        lastOff = std::max(lastOff, note.off);
    }

    std::vector<HeldNote> active;
    std::vector<HeldNote> chord;
    std::size_t next = 0;
    std::int64_t s = held.front().on / step;
    while (s * step < lastOff) {
        const std::int64_t t = s * step;
        while (next < held.size() && held[next].on <= t) {
            active.push_back(held[next]);
            ++next;
        }
        std::erase_if(active, [t](const HeldNote& note) { return note.off <= t; });
        if (active.empty()) {
            if (next >= held.size()) {
                break;
            }
            // Skip the silence to the next note's first step rather than walking it.
            s = std::max(s + 1, (held[next].on + step - 1) / step);
            continue;
        }

        chord = active;
        if (arp.mode == ArpMode::Order) {
            std::ranges::stable_sort(chord, [](const HeldNote& a, const HeldNote& b) {
                return a.on != b.on ? a.on < b.on : a.pitch < b.pitch;
            });
        } else {
            std::ranges::stable_sort(chord, {}, &HeldNote::pitch);
        }
        // The velocity of the most recently struck note: what the player last did.
        const HeldNote& latest =
            *std::ranges::max_element(active, [](const HeldNote& a, const HeldNote& b) {
                return a.on != b.on ? a.on < b.on : a.pitch < b.pitch;
            });

        const std::size_t total = chord.size() * octaves;
        const std::size_t index = arpIndex(arp.mode, s, total);
        const int pitch = static_cast<int>(chord[index % chord.size()].pitch) +
                          (12 * static_cast<int>(index / chord.size()));
        const auto id = kArpNoteIdBit | static_cast<std::uint32_t>(s & 0x7FFFFFFF);
        const auto midi = static_cast<std::uint8_t>(std::clamp(pitch, 0, 127));
        events.push_back(ScheduledEvent{.tick = t,
                                        .endTick = t + gate,
                                        .noteId = id,
                                        .instance = 0,
                                        .value = 0.0F,
                                        .kind = EventKind::NoteOn,
                                        .pitch = midi,
                                        .velocity = latest.velocity});
        events.push_back(ScheduledEvent{.tick = t + gate,
                                        .endTick = t + gate,
                                        .noteId = id,
                                        .instance = 0,
                                        .value = 0.0F,
                                        .kind = EventKind::NoteOff,
                                        .pitch = midi,
                                        .velocity = 0});
        ++s;
    }
    std::ranges::sort(events, eventLess);
}

float automationTolerance(float minimum, float maximum) noexcept {
    const float range = std::abs(maximum - minimum);
    return std::max(range * 0.001F, 1e-6F);
}

void compileBreakpoints(std::vector<Knot>& out, std::span<const AutomationPoint> points,
                        float tolerance) {
    if (points.empty()) {
        return;
    }
    out.push_back(Knot{.tick = points.front().tick, .value = points.front().value});
    for (std::size_t i = 0; i + 1 < points.size(); ++i) {
        appendCurveSegment(out, points[i].tick, points[i].value, points[i + 1].tick,
                           points[i + 1].value, points[i].curve, tolerance);
    }
}

} // namespace adx::project
