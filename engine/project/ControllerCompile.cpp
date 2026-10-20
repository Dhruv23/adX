// adx-thread: main
#include "engine/project/ControllerCompile.h"

#include <algorithm>
#include <cmath>

#include "engine/dsp/Math.h"

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

float lfoShapeAt(LfoShape shape, double turns) noexcept {
    const double x = turns - std::floor(turns);
    switch (shape) {
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

    const std::size_t first = out.size();
    push(out, 0, value(lfoShapeAt(controller.shape, phase)));
    const auto startCycle = static_cast<std::int64_t>(std::floor(phase));
    for (std::int64_t cycle = startCycle;; ++cycle) {
        const auto base = static_cast<double>(cycle);
        if (tickOf(base) > endTick || out.size() - first > kMaxControllerKnots) {
            break;
        }
        switch (controller.shape) {
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
    // The saw rises to its peak at the end of the last cycle drawn.
    if (controller.shape == LfoShape::Saw) {
        const double turns = (static_cast<double>(endTick) / period) + phase;
        push(out, endTick, value(lfoShapeAt(LfoShape::Saw, turns)));
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
