// The controllers added after Phase 6: formula, keyboard and X-Y pad, the extra LFO
// shapes, and any controller driving a macro (one controller, many parameters).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "engine/core/TempoMap.h"
#include "engine/format/adx/Diagnostics.h"
#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Writer.h"
#include "engine/project/ControllerCompile.h"
#include "engine/project/Formula.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/Project.h"
#include "engine/project/SnapshotBuilder.h"
#include "engine/project/commands/ArrangeCommands.h"
#include "engine/project/commands/CommandStack.h"

using namespace adx::project;
using adx::core::kPpq;
using adx::core::Ticks;

namespace {

double eval(std::string_view text, FormulaInputs inputs = {}) {
    std::optional<FormulaError> error;
    const Formula formula = Formula::compile(text, error);
    INFO("formula '" << text << "': " << (error ? error->message : std::string("ok")));
    REQUIRE_FALSE(error);
    return formula.evaluate(inputs);
}

std::optional<FormulaError> errorOf(std::string_view text) {
    std::optional<FormulaError> error;
    static_cast<void>(Formula::compile(text, error));
    return error;
}

/// The error compiling `text` gives; fails the test when it compiles.
FormulaError mustFail(std::string_view text) {
    const std::optional<FormulaError> error = errorOf(text);
    REQUIRE(error.has_value());
    return error.value_or(FormulaError{});
}

struct Loaded {
    Project project;
    CommandStack stack;
    adx::format::DiagnosticList diagnostics;
};

std::unique_ptr<Loaded> load(std::string_view text, bool allowErrors = false) {
    auto out = std::make_unique<Loaded>();
    adx::format::load(text, out->project, out->stack, out->diagnostics);
    if (!allowErrors) {
        for (const auto& d : out->diagnostics.all()) {
            INFO("ADX" << d.code << ": " << d.message);
        }
        REQUIRE_FALSE(out->diagnostics.hasErrors());
    }
    return out;
}

std::string reloaded(const Project& project) {
    Project again;
    CommandStack stack;
    adx::format::DiagnosticList diagnostics;
    adx::format::load(adx::format::write(project), again, stack, diagnostics);
    REQUIRE_FALSE(diagnostics.hasErrors());
    return adx::format::write(again);
}

constexpr const char* kBase = R"([PROJECT]
ADX_VERSION=2

[CHANNEL Lead]
INSTRUMENT=testtone
OUTPUT=insert.1

[CHANNEL Bass]
INSTRUMENT=testtone
OUTPUT=insert.1

[PATTERN P]
LENGTH=4:0:0
NOTES Lead
  C4 0:0:0 0:1:0 100
  C5 1:0:0 0:1:0 40
  C3 2:0:0 0:1:0 100
  E3 2:0:0 0:1:0 100
NOTES Bass
  C2 0:0:0 4:0:0 100

[PLAYLIST]
TRACK 1
  PATTERN P 0:0:0

[MIXER]
INSERT 1 name="Master"
)";

/// The snapshot's parameter index for `path`, or kNone.
std::uint32_t indexOf(const Snapshot& snapshot, const Project& project, std::string_view path) {
    const ParamResolution resolved = ParamRegistry::resolve(path, project);
    REQUIRE(resolved.ok());
    for (const ParamSlot& slot : snapshot.paramIndex) {
        if (slot.ref == resolved.ref) {
            return slot.index;
        }
    }
    return adx::graph::kNone;
}

/// Every lane on parameter `index`.
std::vector<const AutomationLane*> lanesOn(const Snapshot& snapshot, std::uint32_t index) {
    std::vector<const AutomationLane*> out;
    for (const AutomationLane& lane : snapshot.automation) {
        if (lane.paramIndex == index) {
            out.push_back(&lane);
        }
    }
    return out;
}

/// The value of a knot chain at `tick` (held outside, linear between, later wins on a tie).
float chainAt(std::span<const Knot> knots, std::int64_t tick) {
    if (knots.empty()) {
        return 0.0F;
    }
    if (tick <= knots.front().tick) {
        return knots.front().value;
    }
    for (std::size_t i = 1; i < knots.size(); ++i) {
        if (tick < knots[i].tick) {
            const Knot& a = knots[i - 1];
            const Knot& b = knots[i];
            const double f =
                static_cast<double>(tick - a.tick) / static_cast<double>(b.tick - a.tick);
            return static_cast<float>(a.value + ((b.value - a.value) * f));
        }
    }
    return knots.back().value;
}

} // namespace

TEST_CASE("formula_evaluates_with_precedence", "[controllers][formula]") {
    CHECK(eval("1 + 2 * 3") == 7.0);
    CHECK(eval("(1 + 2) * 3") == 9.0);
    CHECK(eval("2 ^ 3 ^ 2") == 512.0); // right-associative
    CHECK(eval("-2 ^ 2") == -4.0);     // the power binds tighter than the minus
    CHECK(eval("2 ^ -1") == 0.5);
    CHECK(eval("7 % 3") == 1.0);
    CHECK(eval("-1 % 4") == 3.0); // floored
    CHECK(eval("1 < 2 && 3 >= 3") == 1.0);
    CHECK(eval("!(1 == 1) || 0") == 0.0);
    CHECK(eval("1 != 2 ? 10 : 20") == 10.0);
    CHECK(eval("0 ? 1 : 0 ? 2 : 3") == 3.0);
    CHECK(eval("1e3 + .5") == 1000.5);
    CHECK(eval("a + b * c", FormulaInputs{.a = 1.0, .b = 2.0, .c = 3.0}) == 7.0);
    CHECK(std::abs(eval("sin(pi / 2)") - 1.0) < 1e-12);
    CHECK(std::abs(eval("cos(pi)") + 1.0) < 1e-12);
    CHECK(std::abs(eval("atan2(1, 1)") - (std::numbers::pi / 4.0)) < 1e-9);
    CHECK(std::abs(eval("log(e)") - 1.0) < 1e-12);
    CHECK(std::abs(eval("pow(-2, 3)") + 8.0) < 1e-12);
    CHECK(eval("clamp(5, 0, 1)") == 1.0);
    CHECK(eval("lerp(2, 4, 0.5)") == 3.0);
    CHECK(eval("smoothstep(0, 1, 0.5)") == 0.5);
    CHECK(eval("tri(0.25)") == 1.0);
    CHECK(eval("saw(0.75)") == 0.5);
    CHECK(eval("sq(0.6)") == -1.0);
    CHECK(eval("floor(-1.5) + ceil(1.2) + round(2.5)") == 3.0);
    // rand is fixed per whole number, and spread over 0..1.
    CHECK(eval("rand(3.2)") == eval("rand(3.9)"));
    CHECK(eval("rand(3)") != eval("rand(4)"));
    const double r = eval("rand(17)");
    CHECK((r >= 0.0 && r < 1.0));
    // A NaN or an infinity comes out as 0, never poisons the lane.
    CHECK(eval("log(-1)") == 0.0);
    CHECK(eval("1 / 0") == 0.0);
}

TEST_CASE("formula_reads_its_inputs", "[controllers][formula]") {
    std::optional<FormulaError> error;
    const Formula constant = Formula::compile("a * 2", error);
    REQUIRE_FALSE(error);
    CHECK_FALSE(constant.varies());
    const Formula moving = Formula::compile("t + s + p", error);
    REQUIRE_FALSE(error);
    CHECK(moving.varies());
    CHECK(moving.evaluate(FormulaInputs{.t = 1.0, .s = 2.0, .p = 0.25}) == 3.25);
}

TEST_CASE("formula_errors_say_where", "[controllers][formula]") {
    CHECK(mustFail("1 + * 2").offset == 4);
    CHECK(mustFail("sin(1, 2)").message.find("sin() takes 1 argument") != std::string::npos);
    const FormulaError unknown = mustFail("q + 1");
    CHECK(unknown.offset == 0);
    CHECK(unknown.length == 1);
    CHECK(unknown.message.find("unknown name 'q'") != std::string::npos);
    CHECK(mustFail("wobble(1)").message.find("unknown function") != std::string::npos);
    CHECK(errorOf("(1 + 2"));
    CHECK(errorOf("1 ? 2"));
    CHECK(errorOf(""));
    CHECK(errorOf("1 2"));
    // Nesting is bounded rather than overflowing the compiler's stack.
    CHECK(errorOf(std::string(5000, '(') + "1" + std::string(5000, ')')));
    CHECK(errorOf(std::string(5000, '-') + "1"));
}

TEST_CASE("formula_compiles_to_a_sampled_lane", "[controllers][formula]") {
    Controller controller;
    controller.kind = ControllerKind::Formula;
    controller.formula = "sin(2 * pi * t / 4)";
    controller.center = 0.5F;
    controller.depth = 0.5F;
    std::optional<FormulaError> error;
    const Formula formula = Formula::compile(controller.formula, error);
    REQUIRE_FALSE(error);
    const adx::core::TempoMap tempo;
    const ControllerRange range{.minimum = 0.0F, .maximum = 100.0F, .logarithmic = false};
    std::vector<Knot> knots;
    compileFormula(knots, controller, formula, range, kPpq * 16, tempo);
    REQUIRE(knots.size() > 100);
    CHECK(knots.front().tick == 0);
    CHECK(knots.back().tick == kPpq * 16);
    for (const Knot& knot : knots) {
        const double beats = static_cast<double>(knot.tick) / kPpq;
        const double expected =
            100.0 * (0.5 + (0.5 * std::sin(2.0 * std::numbers::pi * beats / 4.0)));
        CHECK(std::abs(knot.value - expected) < 1e-3);
    }

    // A formula that does not move is one knot.
    knots.clear();
    controller.formula = "a";
    controller.a = 0.6F;
    controller.center = 0.0F;
    controller.depth = 1.0F;
    const Formula constant = Formula::compile(controller.formula, error);
    compileFormula(knots, controller, constant, range, kPpq * 16, tempo);
    REQUIRE(knots.size() == 1);
    CHECK(std::abs(knots[0].value - 60.0F) < 1e-4F);

    // A long project lowers the resolution instead of the length.
    knots.clear();
    controller.formula = "t";
    const Formula ramp = Formula::compile(controller.formula, error);
    const std::int64_t end = kPpq * 4 * 100000;
    compileFormula(knots, controller, ramp, range, end, tempo);
    CHECK(knots.size() <= kMaxControllerKnots + 2);
    CHECK(knots.back().tick == end);
}

TEST_CASE("lfo_new_shapes", "[controllers][lfo]") {
    // Saw-down falls; random holds a level per cycle; smooth-random glides between them.
    CHECK(lfoShapeAt(LfoShape::SawDown, 0.0) == 1.0F);
    CHECK(std::abs(lfoShapeAt(LfoShape::SawDown, 0.75) + 0.5F) < 1e-6F);
    CHECK(lfoShapeAt(LfoShape::Random, 3.1, 7) == lfoShapeAt(LfoShape::Random, 3.9, 7));
    CHECK(lfoShapeAt(LfoShape::Random, 3.1, 7) != lfoShapeAt(LfoShape::Random, 4.1, 7));
    CHECK(lfoShapeAt(LfoShape::Random, 3.1, 7) != lfoShapeAt(LfoShape::Random, 3.1, 8));
    CHECK(lfoShapeAt(LfoShape::SmoothRandom, 5.0, 2) == lfoShapeAt(LfoShape::Random, 5.0, 2));
    CHECK(std::abs(lfoShapeAt(LfoShape::SmoothRandom, 5.999999, 2) -
                   lfoShapeAt(LfoShape::Random, 6.0, 2)) < 1e-4F);
    for (int cycle = 0; cycle < 200; ++cycle) {
        const float level = lfoShapeAt(LfoShape::Random, cycle + 0.5, 3);
        CHECK((level >= -1.0F && level <= 1.0F));
    }

    Controller lfo;
    lfo.kind = ControllerKind::Lfo;
    lfo.period = Ticks{kPpq};
    lfo.seed = 9;
    const ControllerRange range{.minimum = 0.0F, .maximum = 1.0F, .logarithmic = false};
    for (const LfoShape shape : {LfoShape::SawDown, LfoShape::Random, LfoShape::SmoothRandom}) {
        lfo.shape = shape;
        std::vector<Knot> knots;
        compileLfo(knots, lfo, range, kPpq * 8);
        INFO("shape " << toString(shape));
        REQUIRE(knots.size() >= 8);
        CHECK(knots.back().tick >= kPpq * 8);
        // The chain plays the shape: sampled mid-cycle, and a quarter in.
        for (int cycle = 0; cycle < 8; ++cycle) {
            for (const double at : {0.25, 0.5}) {
                const auto tick = static_cast<std::int64_t>((cycle + at) * kPpq);
                const float expected = range.map(0.5F + (0.5F * lfoShapeAt(shape, cycle + at, 9)));
                CHECK(std::abs(chainAt(knots, tick) - expected) < 1e-4F);
            }
        }
    }
}

TEST_CASE("keyboard_controller_follows_keys_and_glides", "[controllers][keyboard]") {
    auto loaded = load(std::string(kBase) + R"(
[CONTROLLER Track]
TYPE=keyboard
TARGET=channel.Bass.pan
SOURCE=channel.Lead
RANGE=48..72
CENTER=0
DEPTH=1
)");
    Project& project = loaded->project;
    const Controller& controller = *project.findControllerByName("Track");
    CHECK(controller.kind == ControllerKind::Keyboard);
    CHECK(controller.low == 48);
    CHECK(controller.high == 72);

    SnapshotBuilder builder{48000, 512};
    Snapshot* snapshot = builder.build(project, 1, dirty::kAll).snapshot;
    REQUIRE(snapshot != nullptr);
    const std::uint32_t pan = indexOf(*snapshot, project, "channel.Bass.pan");
    const auto lanes = lanesOn(*snapshot, pan);
    REQUIRE(lanes.size() == 1);
    const std::span<const Knot> knots = lanes[0]->knots;
    // pan is -1..1: C4 (60) is half way through 48..72, C5 is past the top, the C3+E3
    // chord is its highest key, E3 (52): 4/24 of the way.
    const auto at = [&](double beats) {
        return chainAt(knots, static_cast<std::int64_t>(beats * kPpq));
    };
    CHECK(std::abs(at(0.5) - 0.0F) < 1e-5F);
    CHECK(std::abs(at(4.5) - 1.0F) < 1e-5F);
    CHECK(std::abs(at(8.5) - ((4.0F / 24.0F * 2.0F) - 1.0F)) < 1e-5F);
    // It holds between notes, and before the first note it is already at the first's.
    CHECK(std::abs(at(3.0) - 0.0F) < 1e-5F);
    CHECK(std::abs(at(0.0) - 0.0F) < 1e-5F);
    destroySnapshot(snapshot);

    // Velocity instead of key; a glide takes time.
    Controller edited = controller;
    edited.input = KeyboardInput::Velocity;
    edited.low = 0;
    edited.high = 127;
    edited.glideMs = 250.0F; // half a beat at 120 bpm
    loaded->stack.execute(std::make_unique<SetController>(edited.id, edited), project);
    snapshot = builder.build(project, 2, dirty::kAll).snapshot;
    REQUIRE(snapshot != nullptr);
    const auto glided = lanesOn(*snapshot, pan);
    REQUIRE(glided.size() == 1);
    const std::span<const Knot> g = glided[0]->knots;
    const auto vAt = [&](double beats) {
        return chainAt(g, static_cast<std::int64_t>(beats * kPpq));
    };
    const float loud = (100.0F / 127.0F * 2.0F) - 1.0F;
    const float soft = (40.0F / 127.0F * 2.0F) - 1.0F;
    CHECK(std::abs(vAt(0.5) - loud) < 1e-4F);
    CHECK(std::abs(vAt(4.0) - loud) < 1e-4F);                    // the glide starts at the note
    CHECK(std::abs(vAt(4.25) - ((loud + soft) / 2.0F)) < 1e-3F); // half way a quarter-beat in
    CHECK(std::abs(vAt(4.5) - soft) < 1e-4F);
    destroySnapshot(snapshot);

    const std::string text = adx::format::write(project);
    CHECK(text.find("TYPE=keyboard\nTARGET=channel.Bass.pan\nSOURCE=channel.Lead\nINPUT=velocity\n"
                    "RANGE=0..127\nGLIDE=250") != std::string::npos);
    CHECK(reloaded(project) == text);
}

TEST_CASE("controller_drives_a_macro", "[controllers][macro]") {
    // One LFO, two parameters, each through its own range: the macro's targets.
    auto loaded = load(std::string(kBase) + R"(
[MACRO Both]
TARGET channel.Lead.volume range=0..1
TARGET channel.Bass.pan range=1..-1

[CONTROLLER Swing]
TYPE=lfo
TARGET=macro.Both
SHAPE=triangle
PERIOD=1:0:0
)");
    const Project& project = loaded->project;
    SnapshotBuilder builder{48000, 512};
    Snapshot* snapshot = builder.build(project, 1, dirty::kAll).snapshot;
    REQUIRE(snapshot != nullptr);
    const auto volume = lanesOn(*snapshot, indexOf(*snapshot, project, "channel.Lead.volume"));
    const auto pan = lanesOn(*snapshot, indexOf(*snapshot, project, "channel.Bass.pan"));
    REQUIRE(volume.size() == 1);
    REQUIRE(pan.size() == 1);
    // A quarter of the way through a bar the triangle is at its top: the macro at 1.
    const std::int64_t top = kPpq;
    CHECK(std::abs(chainAt(volume[0]->knots, top) - 1.0F) < 1e-4F);
    CHECK(std::abs(chainAt(pan[0]->knots, top) + 1.0F) < 1e-4F);
    const std::int64_t bottom = kPpq * 3;
    CHECK(std::abs(chainAt(volume[0]->knots, bottom) - 0.0F) < 1e-4F);
    CHECK(std::abs(chainAt(pan[0]->knots, bottom) - 1.0F) < 1e-4F);
    destroySnapshot(snapshot);
    CHECK(reloaded(project) == adx::format::write(project));
}

TEST_CASE("xy_pad_folds_into_its_targets", "[controllers][xy]") {
    auto loaded = load(std::string(kBase) + R"(
[MACRO Tone]
TARGET channel.Lead.volume range=0.2..1.2

[CONTROLLER Pad]
TYPE=xy
TARGET=channel.Bass.pan
TARGET_Y=macro.Tone
X=0.75
Y=0.25
)");
    Project& project = loaded->project;
    const Controller& pad = *project.findControllerByName("Pad");
    CHECK(pad.kind == ControllerKind::Xy);
    REQUIRE(pad.targetY.kind == ParamKind::Macro);

    // Its axes are parameters with paths of their own.
    const ParamResolution x = ParamRegistry::resolve("controller.Pad.x", project);
    REQUIRE(x.ok());
    CHECK(x.ref.kind == ParamKind::ControllerAxis);
    CHECK(x.ref.index == kAxisX);
    CHECK(ParamRegistry::pathOf(x.ref, project) == "controller.Pad.x");
    CHECK(ParamRegistry::pathOf(ParamRegistry::resolve("controller.Pad.y", project).ref, project) ==
          "controller.Pad.y");
    CHECK_FALSE(ParamRegistry::resolve("controller.Pad.z", project).ok());

    SnapshotBuilder builder{48000, 512};
    Snapshot* snapshot = builder.build(project, 1, dirty::kAll).snapshot;
    REQUIRE(snapshot != nullptr);
    // X straight onto pan's -1..1; Y moves the macro, which moves the Lead's volume.
    CHECK(std::abs(snapshot->params[indexOf(*snapshot, project, "channel.Bass.pan")] - 0.5F) <
          1e-6F);
    CHECK(std::abs(snapshot->params[indexOf(*snapshot, project, "channel.Lead.volume")] - 0.45F) <
          1e-6F);
    destroySnapshot(snapshot);

    // A move is SetController; the next snapshot plays it.
    Controller moved = pad;
    moved.x = 0.0F;
    moved.y = 1.0F;
    loaded->stack.execute(std::make_unique<SetController>(moved.id, moved), project);
    snapshot = builder.build(project, 2, dirty::kAll).snapshot;
    REQUIRE(snapshot != nullptr);
    CHECK(std::abs(snapshot->params[indexOf(*snapshot, project, "channel.Bass.pan")] + 1.0F) <
          1e-6F);
    CHECK(std::abs(snapshot->params[indexOf(*snapshot, project, "channel.Lead.volume")] - 1.2F) <
          1e-6F);
    destroySnapshot(snapshot);

    const std::string text = adx::format::write(project);
    CHECK(text.find("[CONTROLLER Pad]\nTYPE=xy\nTARGET=channel.Bass.pan\nTARGET_Y=macro.Tone\nX=0\n"
                    "Y=1") != std::string::npos);
    CHECK(reloaded(project) == text);
}

TEST_CASE("lane_on_a_pad_axis_drives_its_target", "[controllers][xy]") {
    // Automation drawn on controller.Pad.x, in a pattern - parsed before the controllers
    // were, once; the pad's section is now read first.
    auto loaded = load(R"([PROJECT]
ADX_VERSION=2

[CHANNEL Lead]
INSTRUMENT=testtone
OUTPUT=insert.1

[PATTERN P]
LENGTH=4:0:0
NOTES Lead
  C4 0:0:0 4:0:0 100
AUTOMATION controller.Pad.x
  0:0:0 0
  4:0:0 1

[PLAYLIST]
TRACK 1
  PATTERN P 0:0:0

[MIXER]
INSERT 1 name="Master"

[CONTROLLER Pad]
TYPE=xy
TARGET=channel.Lead.pan
)");
    const Project& project = loaded->project;
    SnapshotBuilder builder{48000, 512};
    Snapshot* snapshot = builder.build(project, 1, dirty::kAll).snapshot;
    REQUIRE(snapshot != nullptr);
    const auto lanes = lanesOn(*snapshot, indexOf(*snapshot, project, "channel.Lead.pan"));
    REQUIRE(lanes.size() == 1);
    CHECK(std::abs(chainAt(lanes[0]->knots, 0) + 1.0F) < 1e-5F);
    CHECK(std::abs(chainAt(lanes[0]->knots, kPpq * 8) - 0.0F) < 1e-3F);
    CHECK(std::abs(chainAt(lanes[0]->knots, kPpq * 16) - 1.0F) < 1e-5F);
    destroySnapshot(snapshot);
    CHECK(reloaded(project) == adx::format::write(project));
}

TEST_CASE("formula_round_trips_and_reports_errors", "[controllers][formula][format]") {
    auto loaded = load(std::string(kBase) + R"(
[CONTROLLER Wave]
TYPE=formula
TARGET=channel.Lead.volume
FORMULA="p < a ? 1 : -1"
PERIOD=0:2:0
A=0.25
)");
    const Controller& wave = *loaded->project.findControllerByName("Wave");
    CHECK(wave.kind == ControllerKind::Formula);
    CHECK(wave.formula == "p < a ? 1 : -1");
    CHECK(wave.a == 0.25F);
    const std::string text = adx::format::write(loaded->project);
    CHECK(text.find("TYPE=formula\nTARGET=channel.Lead.volume\nFORMULA=\"p < a ? 1 : -1\"\n"
                    "PERIOD=0:2:0\nA=0.25") != std::string::npos);
    CHECK(reloaded(loaded->project) == text);

    // A formula that does not parse is ADX2016, at the bad character, and kept verbatim.
    auto bad = load(std::string(kBase) + R"(
[CONTROLLER Broken]
TYPE=formula
TARGET=channel.Lead.volume
FORMULA="sin(t) +* 2"
)",
                    true);
    const auto items = bad->diagnostics.all();
    const auto found =
        std::ranges::find(items, adx::format::code::kFormulaSyntax, &adx::format::Diagnostic::code);
    REQUIRE(found != items.end());
    CHECK(found->message.find("formula:") == 0);
    CHECK(found->span.column > 0);
    const Controller* broken = bad->project.findControllerByName("Broken");
    REQUIRE(broken != nullptr);
    CHECK(broken->formula == "sin(t) +* 2");
    SnapshotBuilder builder{48000, 512};
    Snapshot* snapshot = builder.build(bad->project, 1, dirty::kAll).snapshot;
    REQUIRE(snapshot != nullptr);
    CHECK(lanesOn(*snapshot, indexOf(*snapshot, bad->project, "channel.Lead.volume")).empty());
    destroySnapshot(snapshot);
}

TEST_CASE("random_lfo_seed_round_trips", "[controllers][lfo][format]") {
    auto loaded = load(std::string(kBase) + R"(
[CONTROLLER Drift]
TYPE=lfo
TARGET=channel.Lead.pan
SHAPE=smooth-random
SEED=42
)");
    const Controller& drift = *loaded->project.findControllerByName("Drift");
    CHECK(drift.shape == LfoShape::SmoothRandom);
    CHECK(drift.seed == 42U);
    const std::string text = adx::format::write(loaded->project);
    CHECK(text.find("SHAPE=smooth-random\nSEED=42") != std::string::npos);
    CHECK(reloaded(loaded->project) == text);
}

TEST_CASE("peak_controller_fans_out_through_a_macro", "[controllers][peak][macro]") {
    auto loaded = load(std::string(kBase) + R"(
[MACRO Duck]
TARGET channel.Lead.volume range=1..0
TARGET channel.Bass.volume range=1..0.5 curve=exponential(0.3)

[CONTROLLER Follow]
TYPE=peak
TARGET=macro.Duck
SOURCE=insert.1
)");
    SnapshotBuilder builder{48000, 512};
    Snapshot* snapshot = builder.build(loaded->project, 1, dirty::kAll).snapshot;
    REQUIRE(snapshot != nullptr);
    const auto& nodes = builder.graphBuild()->graph.nodes();
    const auto first =
        std::ranges::find(nodes, std::string("controller.Follow.1"), &adx::graph::GraphNode::label);
    const auto second =
        std::ranges::find(nodes, std::string("controller.Follow.2"), &adx::graph::GraphNode::label);
    REQUIRE(first != nodes.end());
    REQUIRE(second != nodes.end());
    CHECK(first->modulates == indexOf(*snapshot, loaded->project, "channel.Lead.volume"));
    CHECK(second->modulates == indexOf(*snapshot, loaded->project, "channel.Bass.volume"));
    CHECK(first->node != second->node);
    destroySnapshot(snapshot);

    // Rebuilt unchanged, both nodes are kept.
    const auto keep1 = first->node;
    snapshot = builder.build(loaded->project, 2, dirty::kAll).snapshot;
    REQUIRE(snapshot != nullptr);
    const auto& again = builder.graphBuild()->graph.nodes();
    const auto same =
        std::ranges::find(again, std::string("controller.Follow.1"), &adx::graph::GraphNode::label);
    REQUIRE(same != again.end());
    CHECK(same->node == keep1);
    destroySnapshot(snapshot);
}

TEST_CASE("new_kinds_and_shapes_have_names", "[controllers]") {
    for (std::size_t i = 0; i < kControllerKindCount; ++i) {
        const auto kind = static_cast<ControllerKind>(i);
        ControllerKind back = ControllerKind::Lfo;
        REQUIRE(controllerKindFromString(toString(kind), back));
        CHECK(back == kind);
    }
    for (std::size_t i = 0; i < kLfoShapeCount; ++i) {
        const auto shape = static_cast<LfoShape>(i);
        LfoShape back = LfoShape::Sine;
        REQUIRE(lfoShapeFromString(toString(shape), back));
        CHECK(back == shape);
    }
    KeyboardInput input = KeyboardInput::Pitch;
    CHECK(keyboardInputFromString("velocity", input));
    CHECK(input == KeyboardInput::Velocity);
    CHECK_FALSE(keyboardInputFromString("aftertouch", input));
}
