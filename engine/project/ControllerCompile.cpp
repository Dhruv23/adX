// adx-thread: main
#include "engine/project/ControllerCompile.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"
#include "engine/project/Macro.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/Project.h"

namespace adx::project {
namespace {

/// One point of an envelope's level, in seconds.
struct LevelPoint {
    double seconds{0.0};
    double level{0.0};
};

/// The polyline's level at `seconds`, held flat outside it.
double levelAt(std::span<const LevelPoint> line, double seconds) noexcept {
    if (line.empty()) {
        return 0.0;
    }
    if (seconds <= line.front().seconds) {
        return line.front().level;
    }
    for (std::size_t i = 1; i < line.size(); ++i) {
        if (seconds <= line[i].seconds) {
            const LevelPoint& a = line[i - 1];
            const LevelPoint& b = line[i];
            const double span = b.seconds - a.seconds;
            return span <= 0.0 ? b.level
                               : a.level + ((b.level - a.level) * (seconds - a.seconds) / span);
        }
    }
    return line.back().level;
}

/// The ADSR of one note from `start` level `from`: attack to 1, decay to sustain, hold
/// to the note-off, release to 0. A note-off before the decay ends releases from where
/// the level is then.
std::vector<LevelPoint> noteEnvelope(const Controller& controller, double on, double off,
                                     double from) {
    const double attack = std::max(0.0, static_cast<double>(controller.attackMs) * 1e-3);
    const double decay = std::max(0.0, static_cast<double>(controller.decayMs) * 1e-3);
    const double release = std::max(0.0, static_cast<double>(controller.releaseMs) * 1e-3);
    const double sustain = std::clamp(static_cast<double>(controller.sustain), 0.0, 1.0);
    std::vector<LevelPoint> line{{.seconds = on, .level = from},
                                 {.seconds = on + attack, .level = 1.0},
                                 {.seconds = on + attack + decay, .level = sustain}};
    if (off < line.back().seconds) {
        const double level = levelAt(line, off);
        std::erase_if(line, [off](const LevelPoint& p) { return p.seconds > off; });
        line.push_back({.seconds = off, .level = level});
    } else {
        line.push_back({.seconds = off, .level = sustain});
    }
    line.push_back({.seconds = off + release, .level = 0.0});
    return line;
}

void push(std::vector<Knot>& out, std::int64_t tick, float value) {
    if (!out.empty() && tick < out.back().tick) {
        tick = out.back().tick;
    }
    out.push_back(Knot{.tick = tick, .value = value});
}

/// The random shapes' level for one cycle: splitmix64 of (seed, cycle), -1..1.
double randomLevel(std::uint32_t seed, std::int64_t cycle) noexcept {
    std::uint64_t z = (static_cast<std::uint64_t>(seed) << 32U) ^ static_cast<std::uint64_t>(cycle);
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    z ^= z >> 31U;
    return (static_cast<double>(z >> 11U) * 0x1.0p-52) - 1.0;
}

} // namespace

ControllerRange ControllerRange::of(const ParamDescriptor* descriptor) noexcept {
    if (descriptor == nullptr || !(descriptor->maximum > descriptor->minimum)) {
        return {};
    }
    return ControllerRange{.minimum = descriptor->minimum,
                           .maximum = descriptor->maximum,
                           .logarithmic = descriptor->scale == ScaleKind::Logarithmic &&
                                          descriptor->minimum > 0.0F};
}

float ControllerRange::map(float normalised) const noexcept {
    const double n = std::clamp(static_cast<double>(normalised), 0.0, 1.0);
    const auto low = static_cast<double>(minimum);
    const auto high = static_cast<double>(maximum);
    if (logarithmic) {
        return static_cast<float>(low * dsp::exp2(n * dsp::log2(high / low)));
    }
    return static_cast<float>(low + (n * (high - low)));
}

float ControllerDestination::map(float normalised) const noexcept {
    if (through == nullptr) {
        return range.map(normalised);
    }
    const float value = macroTargetValue(*through, normalised);
    return range.maximum > range.minimum ? std::clamp(value, range.minimum, range.maximum) : value;
}

float padValue(const Controller& pad, std::uint16_t axis) noexcept {
    return std::clamp(axis == kAxisX ? pad.x : pad.y, 0.0F, 1.0F);
}

ParamRef padTarget(const Controller& pad, std::uint16_t axis) noexcept {
    return axis == kAxisX ? pad.target : pad.targetY;
}

std::vector<ControllerDestination> controllerDestinations(const Project& project, ParamRef target) {
    std::vector<ControllerDestination> out;
    if (target.kind == ParamKind::ControllerAxis) {
        // Whatever the pad's axis drives - never another pad's axis, so this stops.
        const Controller* pad = project.find(core::ControllerId{target.owner});
        if (pad == nullptr || pad->kind != ControllerKind::Xy) {
            return out;
        }
        target = padTarget(*pad, target.index);
        if (target.kind == ParamKind::ControllerAxis) {
            return out;
        }
    }
    if (!target.valid()) {
        return out;
    }
    if (target.kind != ParamKind::Macro) {
        out.push_back(ControllerDestination{
            .target = target,
            .range = ControllerRange::of(ParamRegistry::descriptorFor(target, project)),
            .through = nullptr});
        return out;
    }
    const Macro* macro = project.find(core::MacroId{target.owner});
    if (macro == nullptr) {
        return out;
    }
    for (const MacroTarget& each : macro->targets) {
        if (!each.target.valid() || each.target.kind == ParamKind::Macro ||
            each.target.kind == ParamKind::ControllerAxis) {
            continue;
        }
        const ParamDescriptor* descriptor = ParamRegistry::descriptorFor(each.target, project);
        ControllerRange range = ControllerRange::of(descriptor);
        if (descriptor == nullptr) {
            range = ControllerRange{}; // no clamp but the macro target's own two ends
            range.minimum = std::min(each.minimum, each.maximum);
            range.maximum = std::max(each.minimum, each.maximum);
        }
        out.push_back(
            ControllerDestination{.target = each.target, .range = range, .through = &each});
    }
    return out;
}

float lfoShapeAt(LfoShape shape, double turns, std::uint32_t seed) noexcept {
    const double whole = std::floor(turns);
    const double x = turns - whole;
    const auto cycle = static_cast<std::int64_t>(whole);
    switch (shape) {
    case LfoShape::SawDown:
        return static_cast<float>(1.0 - (2.0 * x));
    case LfoShape::Random:
        return static_cast<float>(randomLevel(seed, cycle));
    case LfoShape::SmoothRandom: {
        const double from = randomLevel(seed, cycle);
        const double to = randomLevel(seed, cycle + 1);
        return static_cast<float>(from + ((to - from) * x));
    }
    case LfoShape::Sine:
        return static_cast<float>(dsp::sinTurns(x));
    case LfoShape::Triangle:
        if (x < 0.25) {
            return static_cast<float>(4.0 * x);
        }
        return static_cast<float>(x < 0.75 ? 2.0 - (4.0 * x) : (4.0 * x) - 4.0);
    case LfoShape::Saw:
        return static_cast<float>((2.0 * x) - 1.0);
    case LfoShape::Square:
        return x < 0.5 ? 1.0F : -1.0F;
    }
    return 0.0F;
}

void compileLfo(std::vector<Knot>& out, const Controller& controller, ControllerRange range,
                std::int64_t endTick) {
    const double period = static_cast<double>(std::max<std::int64_t>(1, controller.period.value));
    const auto phase = static_cast<double>(controller.phase);
    const auto value = [&](double shape) {
        return range.map(controller.center + (controller.depth * static_cast<float>(shape)));
    };
    const double cycles = (static_cast<double>(std::max<std::int64_t>(0, endTick)) / period) + 1.0;
    // Resolution falls before the length does: a fast LFO over a long project keeps its
    // full length at fewer segments per cycle.
    int segments = kLfoSegmentsPerCycle;
    while (segments > 4 && cycles * segments > static_cast<double>(kMaxControllerKnots)) {
        segments /= 2;
    }
    const auto tickOf = [&](double turns) {
        return static_cast<std::int64_t>(std::llround((turns - phase) * period));
    };

    const std::uint32_t seed = controller.seed;
    const std::size_t first = out.size();
    push(out, 0, value(lfoShapeAt(controller.shape, phase, seed)));
    const auto startCycle = static_cast<std::int64_t>(std::floor(phase));
    for (std::int64_t cycle = startCycle;; ++cycle) {
        const auto base = static_cast<double>(cycle);
        if (tickOf(base) > endTick || out.size() - first > kMaxControllerKnots) {
            break;
        }
        switch (controller.shape) {
        case LfoShape::SawDown:
            if (base > phase) {
                push(out, tickOf(base), value(-1.0));
                push(out, tickOf(base), value(1.0));
            }
            break;
        case LfoShape::Random:
            // Held for the cycle: a jump at each boundary.
            if (base > phase) {
                push(out, tickOf(base), value(randomLevel(seed, cycle - 1)));
                push(out, tickOf(base), value(randomLevel(seed, cycle)));
            }
            break;
        case LfoShape::SmoothRandom:
            // A straight line from one cycle's level to the next: one knot a cycle.
            if (base > phase) {
                push(out, tickOf(base), value(randomLevel(seed, cycle)));
            }
            break;
        case LfoShape::Sine:
        case LfoShape::Triangle:
            // Knots on the turn grid, so a triangle's corners land exactly on knots.
            for (int s = 0; s < segments; ++s) {
                const double turns = base + (static_cast<double>(s) / segments);
                if (turns > phase) {
                    push(out, tickOf(turns), value(lfoShapeAt(controller.shape, turns)));
                }
            }
            break;
        case LfoShape::Saw:
            // A jump is two knots at one tick: the later one wins from there on.
            if (base > phase) {
                push(out, tickOf(base), value(1.0));
                push(out, tickOf(base), value(-1.0));
            }
            break;
        case LfoShape::Square:
            for (const double half : {0.0, 0.5}) {
                const double turns = base + half;
                if (turns > phase) {
                    const float high = value(1.0);
                    const float low = value(-1.0);
                    push(out, tickOf(turns), half == 0.0 ? low : high);
                    push(out, tickOf(turns), half == 0.0 ? high : low);
                }
            }
            break;
        }
    }
    // A ramp runs on to where it is at the end of the last cycle drawn.
    if (controller.shape == LfoShape::Saw || controller.shape == LfoShape::SawDown ||
        controller.shape == LfoShape::SmoothRandom || controller.shape == LfoShape::Random) {
        const double turns = (static_cast<double>(endTick) / period) + phase;
        push(out, endTick, value(lfoShapeAt(controller.shape, turns, seed)));
    }
}

void compileFormula(std::vector<Knot>& out, const Controller& controller, const Formula& formula,
                    ControllerRange range, std::int64_t endTick, const core::TempoMap& tempo) {
    if (formula.empty()) {
        return;
    }
    const auto period = static_cast<double>(std::max<std::int64_t>(1, controller.period.value));
    FormulaInputs inputs{
        .t = 0.0, .s = 0.0, .p = 0.0, .a = controller.a, .b = controller.b, .c = controller.c};
    const auto valueAt = [&](std::int64_t tick) {
        inputs.t = static_cast<double>(tick) / static_cast<double>(core::kPpq);
        inputs.s = tempo.secondsAt(core::Ticks{tick});
        const double turns = (static_cast<double>(tick) / period) + controller.phase;
        inputs.p = turns - std::floor(turns);
        const double shape = formula.evaluate(inputs);
        return range.map(controller.center + (controller.depth * static_cast<float>(shape)));
    };
    if (!formula.varies()) {
        push(out, 0, valueAt(0));
        return;
    }
    const std::int64_t end = std::max<std::int64_t>(0, endTick);
    std::int64_t step = kFormulaStep;
    while (end / step > static_cast<std::int64_t>(kMaxControllerKnots)) {
        step *= 2;
    }
    for (std::int64_t tick = 0; tick < end; tick += step) {
        push(out, tick, valueAt(tick));
    }
    push(out, end, valueAt(end));
}

void compileKeyboard(std::vector<Knot>& out, const Controller& controller, ControllerRange range,
                     std::span<const ScheduledEvent> events, const core::TempoMap& tempo) {
    const double low = controller.low;
    const double high = controller.high;
    const auto levelOf = [&](const ScheduledEvent& event) {
        const double n = controller.input == KeyboardInput::Pitch ? event.pitch : event.velocity;
        if (!(high != low)) {
            return n >= high ? 1.0 : 0.0;
        }
        return std::clamp((n - low) / (high - low), 0.0, 1.0);
    };
    const auto value = [&](double level) {
        return range.map(controller.center + (controller.depth * static_cast<float>(level)));
    };
    const double glide = std::max(0.0, static_cast<double>(controller.glideMs) * 1e-3);

    // One level per onset: a chord is its highest key (or loudest note).
    std::vector<LevelPoint> onsets;
    for (const auto& event : events) {
        if (event.kind != EventKind::NoteOn) {
            continue;
        }
        const double seconds = tempo.secondsAt(core::Ticks{event.tick});
        const double level = levelOf(event);
        if (!onsets.empty() && onsets.back().seconds == seconds) {
            onsets.back().level = std::max(onsets.back().level, level);
        } else {
            onsets.push_back({.seconds = seconds, .level = level});
        }
    }
    if (onsets.empty()) {
        return;
    }
    // It holds the last note's value; before the first note, that note's.
    std::vector<LevelPoint> line{{.seconds = 0.0, .level = onsets.front().level}};
    for (const LevelPoint& onset : onsets) {
        // A new note mid-glide starts from wherever the glide had got to.
        const double from = levelAt(line, onset.seconds);
        std::erase_if(line, [&onset](const LevelPoint& p) { return p.seconds > onset.seconds; });
        line.push_back({.seconds = onset.seconds, .level = from});
        line.push_back({.seconds = onset.seconds + glide, .level = onset.level});
    }
    for (const LevelPoint& point : line) {
        push(out, tempo.ticksAtSeconds(point.seconds).value, value(point.level));
    }
}

void compileEnvelope(std::vector<Knot>& out, const Controller& controller, ControllerRange range,
                     std::span<const ScheduledEvent> events, const core::TempoMap& tempo) {
    const auto value = [&](double level) {
        return range.map(controller.center + (controller.depth * static_cast<float>(level)));
    };
    std::vector<LevelPoint> current;
    bool any = false;
    for (const auto& event : events) {
        if (event.kind != EventKind::NoteOn) {
            continue;
        }
        // A chord retriggers once.
        if (any && tempo.secondsAt(core::Ticks{event.tick}) <= current.front().seconds) {
            continue;
        }
        const double on = tempo.secondsAt(core::Ticks{event.tick});
        const double off = tempo.secondsAt(core::Ticks{std::max(event.endTick, event.tick)});
        const double from = any ? levelAt(current, on) : 0.0;
        if (any) {
            // The previous note's envelope, cut where this one starts.
            for (const LevelPoint& point : current) {
                if (point.seconds < on) {
                    push(out, tempo.ticksAtSeconds(point.seconds).value, value(point.level));
                }
            }
        } else {
            push(out, 0, value(0.0));
        }
        current = noteEnvelope(controller, on, off, from);
        any = true;
    }
    for (const LevelPoint& point : current) {
        push(out, tempo.ticksAtSeconds(point.seconds).value, value(point.level));
    }
}

} // namespace adx::project
