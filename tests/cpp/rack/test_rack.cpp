// Racks and macros (phase_12.md §4.1, §4.2, §5): the `.adxrack` file, its expansion
// into ordinary entities in one undo step, the inverse, and macros folded into the
// parameters they move.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "engine/format/adx/Parser.h"
#include "engine/format/adx/Value.h"
#include "engine/format/adx/Writer.h"
#include "engine/preset/Rack.h"
#include "engine/project/MacroCompile.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/Rack.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/project/commands/RackCommands.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/render/RenderFixtures.h"

using namespace adx::project;
using Catch::Matchers::WithinAbs;

namespace {

constexpr const char* kKickRack = R"([RACK]
NAME="Test Kick"
PACK=demo
CATEGORY=Drums/Kick
TAGS=kick, layered
TEMPO_HINT=150
ROOT_HINT=F1
VERSION=1
FUTURE_KEY=kept

[LAYER punch]
TYPE=drumsynth
KEYS=30..60
GAIN=-4
PARAM model=0
PARAM decay=0.12
SLOT ParametricEq band1.freq=60

[LAYER tail]
TYPE=va
PAN=0.25
PARAM env.decay=0.3 curve=exponential(0.3)
PARAM osc1.wave=0

[LAYER click]
TYPE=drumsynth
VELOCITY=100..127
PARAM model=2

[CHAIN]
SLOT Clipper mode=1 drive=12
SLOT Overdrive mix=0.5 drive=14
SLOT Limiter

[SEND reverb]
LEVEL=-18
SLOT Reverb room=0.6
SLOT Ducker amount=0.8 key=rack

[MACRO Tail Length]
VALUE=0.5
TARGET layer.tail.env.decay range=0.15..0.6 curve=exponential(0.3)
TARGET layer.tail.env.release range=0.03..0.09

[MACRO Crunch]
VALUE=0.4
TARGET chain.1.drive range=6..24
TARGET chain.2.mix range=0.2..1
TARGET send.reverb.level range=0..0.5
)";

constexpr const char* kEmpty = R"([PROJECT]
ADX_VERSION=2

[MIXER]
INSERT 1 name="Master"
)";

Rack kickRack() {
    adx::format::DiagnosticList diagnostics;
    Rack rack = adx::preset::parseRack(kKickRack, diagnostics);
    // The one diagnostic is the unknown key, which is kept.
    CHECK_FALSE(diagnostics.hasErrors());
    return rack;
}

std::string textOf(const Project& project) {
    return adx::format::write(project);
}

/// One tone on one channel, for two bars; `extra` is appended to the file.
std::string toneText(const std::string& channelLines, const std::string& extra) {
    return std::string("[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n\n[CHANNEL Lead]\n"
                       "INSTRUMENT=va\nOUTPUT=insert.1\n") +
           channelLines +
           "\n[PATTERN P]\nLENGTH=2:0:0\nNOTES Lead\n  A4 0:0:0 2:0:0 100\n\n"
           "[PLAYLIST]\nTRACK 1\n  PATTERN P 0:0:0\n" +
           extra + "\n[MIXER]\nINSERT 1 name=\"Master\"\n";
}

std::vector<float> render(const std::string& text) {
    const auto loaded = adx::tests::loadText(text);
    adx::render::RenderStats stats;
    return adx::tests::renderFrames(loaded->project, 256, 48000, stats);
}

float maxDifference(const std::vector<float>& a, const std::vector<float>& b) {
    REQUIRE(a.size() == b.size());
    float worst = 0.0F;
    for (std::size_t i = 0; i < a.size(); ++i) {
        worst = std::max(worst, std::abs(a[i] - b[i]));
    }
    return worst;
}

} // namespace

TEST_CASE("rack_roundtrip", "[rack]") {
    adx::format::DiagnosticList diagnostics;
    const Rack first = adx::preset::parseRack(kKickRack, diagnostics);
    // Unknown keys are warned about and kept.
    CHECK(diagnostics.count(adx::format::Severity::Warning) == 1);
    REQUIRE(first.extra.size() == 1);
    CHECK(first.extra.front().first == "FUTURE_KEY");

    CHECK(first.name == "Test Kick");
    REQUIRE(first.layers.size() == 3);
    CHECK(first.layers[0].filter.keyLow == 30);
    CHECK(first.layers[0].filter.keyHigh == 60);
    CHECK(first.layers[2].filter.velocityLow == 100);
    REQUIRE(first.layers[0].chain.size() == 1);
    REQUIRE(first.chain.size() == 3);
    CHECK(first.chain[1].mix == 0.5F);
    REQUIRE(first.sends.size() == 1);
    CHECK(first.sends[0].slots[1].keyRack);
    REQUIRE(first.macros.size() == 2);
    CHECK(first.macros[0].targets[0].curve.kind == adx::core::CurveKind::Exponential);

    const std::string written = adx::preset::writeRack(first);
    adx::format::DiagnosticList again;
    const Rack second = adx::preset::parseRack(written, again);
    CHECK(second == first);
    CHECK(adx::preset::writeRack(second) == written);
}

TEST_CASE("rack_insert_one_undo", "[rack]") {
    const auto loaded = adx::tests::loadText(kEmpty);
    Project& project = loaded->project;
    const std::string before = textOf(project);
    const std::size_t depth = loaded->stack.undoDepth();

    auto command = std::make_unique<InsertRack>(kickRack());
    const InsertRack* raw = command.get();
    loaded->stack.execute(std::move(command), project);
    CHECK(raw->unresolved().empty());
    CHECK(loaded->stack.undoDepth() == depth + 1);
    CHECK(project.channels.size() == 3);
    CHECK(project.macros.size() == 2);
    const std::string inserted = textOf(project);
    CHECK(inserted != before);

    REQUIRE(loaded->stack.undo(project));
    CHECK(textOf(project) == before);
    REQUIRE(loaded->stack.redo(project));
    CHECK(textOf(project) == inserted);
}

TEST_CASE("rack_expands_to_ordinary_entities", "[rack]") {
    const auto loaded = adx::tests::loadText(kEmpty);
    Project& project = loaded->project;
    auto command = std::make_unique<InsertRack>(kickRack());
    const InsertRack* raw = command.get();
    loaded->stack.execute(std::move(command), project);

    // The rack insert, the punch layer's own insert and the reverb return, beside the
    // master; each routed on.
    CHECK(project.mixer.inserts.size() == 4);
    CHECK(project.mixer.routes.size() == 3);
    const Insert* own = project.mixer.find(raw->insert());
    REQUIRE(own != nullptr);
    CHECK(own->name == "Test Kick");
    CHECK(own->slots.size() == 3);
    REQUIRE(own->sends.size() == 1);
    const Insert* reverb = project.mixer.find(own->sends[0].target);
    REQUIRE(reverb != nullptr);
    // key=rack: the ducker on the return is keyed from the rack's insert.
    CHECK(reverb->slots[1].sidechain == raw->insert());

    const Channel* punch = project.findChannelByName("Test Kick/punch");
    const Channel* tail = project.findChannelByName("Test Kick/tail");
    REQUIRE(punch != nullptr);
    REQUIRE(tail != nullptr);
    CHECK(punch->output != raw->insert()); // through its own insert
    CHECK(tail->output == raw->insert());
    CHECK(punch->noteFilter.keyLow == 30);
    CHECK_THAT(punch->volume, WithinAbs(std::pow(10.0, -4.0 / 20.0), 1e-5));

    // Every macro target is an ordinary resolved path.
    for (const Macro& macro : project.macros) {
        for (const MacroTarget& target : macro.targets) {
            INFO(target.path);
            CHECK(target.target.valid());
            CHECK(ParamRegistry::pathOf(target.target, project) == target.path);
            CHECK(target.path.find("layer.") == std::string::npos);
        }
    }
    // A target the rack left at its default was given a value to resolve to.
    CHECK(tail->instrument.find("env.release") != nullptr);

    // The file is an ordinary project: it reloads to the same text, with no rack in it.
    const std::string text = textOf(project);
    CHECK(text.find("RACK") == std::string::npos);
    const auto reloaded = adx::tests::loadText(text);
    CHECK(textOf(reloaded->project) == text);

    // A second copy takes a suffixed name and its own entities.
    loaded->stack.execute(std::make_unique<InsertRack>(kickRack()), project);
    CHECK(project.findChannelByName("Test Kick 2/punch") != nullptr);
    CHECK(project.macros.size() == 4);
}

TEST_CASE("save_rack_inverse", "[rack]") {
    const auto first = adx::tests::loadText(kEmpty);
    auto command = std::make_unique<InsertRack>(kickRack());
    const InsertRack* raw = command.get();
    first->stack.execute(std::move(command), first->project);

    const Rack saved = rackFrom(first->project, raw->insert());
    const Rack original = kickRack();
    REQUIRE(saved.layers.size() == original.layers.size());
    for (std::size_t i = 0; i < saved.layers.size(); ++i) {
        INFO(original.layers[i].name);
        CHECK(saved.layers[i].name == original.layers[i].name);
        CHECK(saved.layers[i].type == original.layers[i].type);
        CHECK(saved.layers[i].filter == original.layers[i].filter);
        CHECK(saved.layers[i].chain == original.layers[i].chain);
        CHECK_THAT(saved.layers[i].gainDb, WithinAbs(original.layers[i].gainDb, 1e-4));
    }
    CHECK(saved.chain == original.chain);
    REQUIRE(saved.sends.size() == 1);
    CHECK(saved.sends[0].name == "reverb");
    CHECK(saved.sends[0].slots == original.sends[0].slots);
    REQUIRE(saved.macros.size() == original.macros.size());
    for (std::size_t i = 0; i < saved.macros.size(); ++i) {
        CHECK(saved.macros[i].name == original.macros[i].name);
        CHECK(saved.macros[i].targets == original.macros[i].targets);
    }

    // Inserting what was saved gives the project inserting the original gave.
    const auto second = adx::tests::loadText(kEmpty);
    second->stack.execute(std::make_unique<InsertRack>(saved), second->project);
    CHECK(textOf(second->project) == textOf(first->project));
}

TEST_CASE("macro_format_roundtrip", "[rack][macro]") {
    const std::string text = toneText(
        "PARAM filter.cutoff=2000\n",
        "\n[MACRO Bright]\nVALUE=0.25\n"
        "TARGET channel.Lead.filter.cutoff range=200..8000 curve=exponential(0.2)\n"
        "TARGET channel.Lead.volume range=0.5..1\n");
    const auto loaded = adx::tests::loadText(text);
    REQUIRE(loaded->project.macros.size() == 1);
    const Macro& macro = loaded->project.macros.front();
    CHECK(macro.value == 0.25F);
    REQUIRE(macro.targets.size() == 2);
    CHECK(macro.targets[0].target.kind == ParamKind::ChannelInstrumentParam);
    CHECK(macro.targets[1].target.kind == ParamKind::ChannelVolume);

    const std::string written = textOf(loaded->project);
    const auto again = adx::tests::loadText(written);
    CHECK(textOf(again->project) == written);
    CHECK(again->project.macros.front().targets == macro.targets);

    // The macro is a parameter with a path of its own.
    const ParamResolution resolved = ParamRegistry::resolve("macro.Bright", loaded->project);
    REQUIRE(resolved.ok());
    CHECK(resolved.ref.kind == ParamKind::Macro);
    CHECK(ParamRegistry::pathOf(resolved.ref, loaded->project) == "macro.Bright");

    // A knob turn is undoable and coalesces.
    Project& project = loaded->project;
    const std::size_t depth = loaded->stack.undoDepth();
    loaded->stack.execute(std::make_unique<SetMacros>(macrosWithValue(project, macro.id, 0.5F)),
                          project);
    loaded->stack.execute(std::make_unique<SetMacros>(macrosWithValue(project, macro.id, 0.75F)),
                          project);
    CHECK(project.macros.front().value == 0.75F);
    CHECK(loaded->stack.undoDepth() == depth + 1);
    REQUIRE(loaded->stack.undo(project));
    CHECK(project.macros.front().value == 0.25F);
}

TEST_CASE("macro_targets_compile", "[rack][macro]") {
    // A macro at 0, 0.5 and 1 puts its target at exactly min + (max - min) * curve(v):
    // the render is the render of the same project with that value written in.
    const adx::core::Curve curve{.kind = adx::core::CurveKind::Exponential, .tension = 0.3F};
    for (const float v : {0.0F, 0.5F, 1.0F}) {
        INFO("macro at " << v);
        const float volume = 0.25F + ((1.0F - 0.25F) * curve.evaluate(v));
        const float cutoff = 300.0F + ((6000.0F - 300.0F) * v);
        const std::string macro =
            "\n[MACRO Open]\nVALUE=" + adx::format::formatFloat(v) +
            "\nTARGET channel.Lead.volume range=0.25..1 curve=exponential(0.3)\n"
            "TARGET channel.Lead.filter.cutoff range=300..6000\n";
        const std::vector<float> withMacro =
            render(toneText("VOLUME=0.9\nPARAM filter.cutoff=1234\n", macro));
        const std::vector<float> written =
            render(toneText("VOLUME=" + adx::format::formatFloat(volume) +
                                "\nPARAM filter.cutoff=" + adx::format::formatFloat(cutoff) + "\n",
                            ""));
        CHECK(std::ranges::any_of(written, [](float s) { return s != 0.0F; }));
        CHECK(withMacro == written);
    }
}

TEST_CASE("macro_offsets_automation", "[rack][macro]") {
    // The lane runs 0.2 -> 0.6 on the channel's volume (range 0..2, so 0.1 -> 0.3
    // normalised). The project's own value is 0.5 and the macro puts it at 0.9: an
    // offset of +0.2 normalised, so the lane plays 0.6 -> 1.0. It is not replaced.
    const std::string lane = "TRACK 2\n  AUTOMATION channel.Lead.volume 0:0:0\n"
                             "    0:0:0 0.2 linear\n    2:0:0 0.6 linear\n";
    const std::string shifted = "TRACK 2\n  AUTOMATION channel.Lead.volume 0:0:0\n"
                                "    0:0:0 0.6 linear\n    2:0:0 1 linear\n";
    const std::string macro = "\n[MACRO Lift]\nVALUE=1\n"
                              "TARGET channel.Lead.volume range=0.5..0.9\n";
    const std::vector<float> plain = render(toneText("VOLUME=0.5\n", lane));
    const std::vector<float> offset = render(toneText("VOLUME=0.5\n", lane + macro));
    const std::vector<float> expected = render(toneText("VOLUME=0.5\n", shifted));
    CHECK(maxDifference(offset, expected) < 1e-5F);
    CHECK(maxDifference(offset, plain) > 0.01F);

    // A macro that leaves the target where the project has it changes nothing.
    const std::string still = "\n[MACRO Lift]\nVALUE=0\n"
                              "TARGET channel.Lead.volume range=0.5..0.9\n";
    CHECK(render(toneText("VOLUME=0.5\n", lane + still)) == plain);
}

TEST_CASE("macro_lane_drives_its_targets", "[rack][macro]") {
    // A lane drawn on the macro is the same lane on each target, through its range.
    const std::string macro = "\n[MACRO Lift]\nVALUE=0\n"
                              "TARGET channel.Lead.volume range=0.2..0.6\n";
    const std::string onMacro = "TRACK 2\n  AUTOMATION macro.Lift 0:0:0\n"
                                "    0:0:0 0 linear\n    2:0:0 1 linear\n";
    const std::string onTarget = "TRACK 2\n  AUTOMATION channel.Lead.volume 0:0:0\n"
                                 "    0:0:0 0.2 linear\n    2:0:0 0.6 linear\n";
    const std::vector<float> driven = render(toneText("VOLUME=0.2\n", onMacro + macro));
    const std::vector<float> direct = render(toneText("VOLUME=0.2\n", onTarget));
    CHECK(maxDifference(driven, direct) < 1e-6F);
}

TEST_CASE("channel_note_filter_drops_notes_at_compile_time", "[rack]") {
    // Two notes; KEYS keeps one. The filtered render is the render of that one note.
    const auto text = [](const std::string& channel, const std::string& notes) {
        return std::string("[PROJECT]\nADX_VERSION=2\n\n[CHANNEL Lead]\nINSTRUMENT=va\n"
                           "OUTPUT=insert.1\n") +
               channel + "\n[PATTERN P]\nLENGTH=1:0:0\nNOTES Lead\n" + notes +
               "\n[PLAYLIST]\nTRACK 1\n  PATTERN P 0:0:0\n\n[MIXER]\nINSERT 1 name=\"Master\"\n";
    };
    const std::string both = "  C3 0:0:0 0:1:0 100\n  C5 0:1:0 0:1:0 60\n";
    const std::vector<float> keys = render(text("KEYS=60..127\n", both));
    CHECK(keys == render(text("", "  C5 0:1:0 0:1:0 60\n")));
    const std::vector<float> velocity = render(text("VELOCITY=90..127\n", both));
    CHECK(velocity == render(text("", "  C3 0:0:0 0:1:0 100\n")));

    const auto loaded = adx::tests::loadText(text("KEYS=60..127\nVELOCITY=10..120\n", both));
    CHECK(loaded->project.channels.front().noteFilter ==
          NoteFilter{.keyLow = 60, .keyHigh = 127, .velocityLow = 10, .velocityHigh = 120});
    const std::string written = textOf(loaded->project);
    CHECK(written.find("KEYS=60..127") != std::string::npos);
    CHECK(textOf(adx::tests::loadText(written)->project) == written);
}
