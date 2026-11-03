// MIDI learn and project routes (phase_9.md §3 step 2, §5): a controller bound to a
// parameter moves it, the binding saves and reloads, it follows its channel through a
// rename, and a project's own routes decide which input plays what.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>
#include <vector>

#include "engine/format/adx/Writer.h"
#include "engine/midi/MidiInput.h"
#include "engine/midi/MidiLearn.h"
#include "engine/project/ParamRegistry.h"
#include "engine/project/commands/ChannelCommands.h"
#include "engine/project/commands/MidiCommands.h"
#include "tests/cpp/render/RenderFixtures.h"

namespace {

using namespace adx;
using adx::midi::MidiEvent;
using adx::midi::MidiInput;

constexpr std::uint32_t kRate = 48000;

std::string learnText(const std::string& midi) {
    return "[PROJECT]\nADX_VERSION=2\n"
           "\n[CHANNEL Lead]\nINSTRUMENT=testtone\n"
           "\n[CHANNEL Pad]\nINSTRUMENT=testtone\n"
           "\n[PATTERN P]\nLENGTH=4:0:0\nNOTES Lead\n  A4 0:0:0 4:0:0 100\n"
           "\n[PLAYLIST]\nTRACK 1 name=\"L\"\n  PATTERN P 0:0:0\n"
           "\n[MIXER]\nINSERT 1 name=\"Master\"\n"
           "\n[MIDI]\n" +
           midi;
}

double rms(const std::vector<float>& samples, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        sum += static_cast<double>(samples[i]) * samples[i];
    }
    return std::sqrt(sum / static_cast<double>(to - from));
}

} // namespace

TEST_CASE("midi_learn_controls_normalise", "[midi][learn]") {
    midi::ControlMessage control;
    REQUIRE(midi::controlOf(MidiEvent{.status = 0xB3, .data1 = 74, .data2 = 127}, control));
    CHECK(control.kind == project::MidiControlKind::ControlChange);
    CHECK(control.channel == 3);
    CHECK(control.number == 74);
    CHECK(control.value == 1.0F);
    REQUIRE(midi::controlOf(MidiEvent{.status = 0xE0, .data1 = 0x00, .data2 = 0x40}, control));
    CHECK(control.kind == project::MidiControlKind::PitchBend);
    CHECK(std::abs(control.value - 0.5F) < 0.001F);
    CHECK_FALSE(midi::controlOf(MidiEvent{.status = 0xF8}, control)); // clock

    const midi::CompiledBinding toggle{.device = midi::kAnyDevice,
                                       .channel = project::kAnyMidiChannel,
                                       .kind = project::MidiControlKind::Note,
                                       .number = 36,
                                       .target = {},
                                       .minimum = 0.0F,
                                       .maximum = 1.0F,
                                       .toggle = true};
    bool state = false;
    float value = -1.0F;
    REQUIRE(midi::controlOf(MidiEvent{.status = 0x99, .data1 = 36, .data2 = 90}, control));
    REQUIRE(midi::matches(toggle, control));
    REQUIRE(midi::valueFor(toggle, control, state, value));
    CHECK(value == 1.0F);
    REQUIRE(midi::controlOf(MidiEvent{.status = 0x89, .data1 = 36}, control));
    CHECK_FALSE(midi::valueFor(toggle, control, state, value)); // a release does nothing
    REQUIRE(midi::controlOf(MidiEvent{.status = 0x99, .data1 = 36, .data2 = 90}, control));
    REQUIRE(midi::valueFor(toggle, control, state, value));
    CHECK(value == 0.0F);

    midi::LastControl last;
    std::uint64_t seen = 0;
    CHECK_FALSE(last.read(seen, control));
    last.publish(midi::ControlMessage{.device = 2,
                                      .channel = 9,
                                      .kind = project::MidiControlKind::ControlChange,
                                      .number = 7,
                                      .value = 0.5F,
                                      .pressed = false});
    REQUIRE(last.read(seen, control));
    CHECK(control.device == 2);
    CHECK(control.channel == 9);
    CHECK(control.number == 7);
    CHECK(std::abs(control.value - 0.5F) < 0.001F);
    CHECK_FALSE(last.read(seen, control));
}

TEST_CASE("midi_learn_binds_and_persists", "[midi][learn][render]") {
    const auto loaded =
        tests::loadText(learnText("LEARN cc 7 channel.Lead.volume channel=1 min=0 max=1\n"));
    REQUIRE(loaded->project.midi.bindings.size() == 1);
    const project::MidiBinding& binding = loaded->project.midi.bindings.front();
    CHECK(binding.target.valid());
    CHECK(binding.channel == 0);
    CHECK(binding.number == 7);

    // Saved and reloaded: the same binding, and the same text.
    const std::string written = format::write(loaded->project);
    CHECK(written.find("[MIDI]\nLEARN cc 7 channel.Lead.volume channel=1\n") != std::string::npos);
    const auto reloaded = tests::loadText(written);
    CHECK(reloaded->project.midi == loaded->project.midi);
    CHECK(format::write(reloaded->project) == written);

    // And the reloaded binding drives the parameter: CC 7 to 0 silences the channel,
    // and the project hears about it.
    project::Project project = reloaded->project;
    project::CommandStack stack;
    tests::OfflineRig const rig(256);
    REQUIRE(rig.engine->setProject(project, 0).rebuilt);
    MidiInput input(rig.engine->core().midiClock(), kRate, 256, 0);
    rig.engine->core().setMidiInput(0, &input);
    rig.engine->play();
    const std::vector<float> before = rig.render(4800);
    REQUIRE(rms(before, 0, before.size()) > 0.01);
    REQUIRE(
        input.push(MidiEvent{.timestampSamples = 4800, .status = 0xB0, .data1 = 7, .data2 = 0}));
    // On another MIDI channel: ignored.
    REQUIRE(
        input.push(MidiEvent{.timestampSamples = 4801, .status = 0xB5, .data1 = 7, .data2 = 127}));
    static_cast<void>(rig.render(1024)); // the block it lands in
    const std::vector<float> after = rig.render(4800);
    CHECK(rms(after, 0, after.size()) < 1e-6);
    CHECK(rig.engine->applyLearned(project, stack) == 1);
    CHECK(project.find(core::ChannelId{binding.target.owner})->volume == 0.0F);
    rig.engine->core().setMidiInput(0, nullptr);
}

TEST_CASE("midi_learn_survives_rename", "[midi][learn]") {
    const auto loaded = tests::loadText(learnText("LEARN cc 74 channel.Lead.pan\n"));
    project::Project project = loaded->project;
    project::CommandStack stack;
    const core::ChannelId lead = project.findChannelByName("Lead")->id;
    stack.execute(std::make_unique<project::RenameChannel>(lead, "Synth Lead"), project);
    // A ParamRef, not a path: the binding still points at the same parameter, and the
    // file now names it by the new name.
    CHECK(project.midi.bindings.front().target.owner == lead.value);
    const std::string written = format::write(project);
    CHECK(written.find("LEARN cc 74 channel.\"Synth Lead\".pan\n") != std::string::npos);
    const auto reloaded = tests::loadText(written);
    REQUIRE(reloaded->project.midi.bindings.size() == 1);
    CHECK(reloaded->project.midi.bindings.front().target ==
          project::ParamRegistry::resolve("channel.\"Synth Lead\".pan", reloaded->project).ref);
}

TEST_CASE("midi_learn_relearn_replaces", "[midi][learn]") {
    const auto loaded = tests::loadText(learnText("LEARN cc 74 channel.Lead.pan\n"));
    project::Project project = loaded->project;
    project::CommandStack stack;
    project::MidiBinding again = project.midi.bindings.front();
    again.targetPath = "channel.Pad.pan";
    again.target = project::ParamRegistry::resolve(again.targetPath, project).ref;
    stack.execute(std::make_unique<project::AddMidiBinding>(again), project);
    REQUIRE(project.midi.bindings.size() == 1);
    CHECK(project.midi.bindings.front().target == again.target);
    REQUIRE(stack.undo(project));
    CHECK(project.midi.bindings.front().target != again.target);
    stack.execute(std::make_unique<project::RemoveMidiBinding>(0), project);
    CHECK(project.midi.bindings.empty());
}

TEST_CASE("midi_project_routes_play_their_channel", "[midi][router][render]") {
    // Keys below C4 to Lead, the rest to Pad, transposed an octave down.
    const auto loaded =
        tests::loadText(learnText("ROUTE Lead keys=0..59\nROUTE Pad keys=60..127 transpose=-12\n"));
    REQUIRE(loaded->project.midi.routes.size() == 2);
    CHECK(loaded->project.midi.routes[1].transpose == -12);
    const std::string written = format::write(loaded->project);
    CHECK(written.find("ROUTE Lead keys=0..59\nROUTE Pad keys=60..127 transpose=-12\n") !=
          std::string::npos);

    project::Project project = loaded->project;
    // No arrangement: only what the input plays.
    project.patterns.front().noteClips.clear();
    tests::OfflineRig const rig(256);
    REQUIRE(rig.engine->setProject(project, 0).rebuilt);
    MidiInput input(rig.engine->core().midiClock(), kRate, 256, 0);
    rig.engine->core().setMidiInput(0, &input);
    rig.engine->play();
    static_cast<void>(rig.render(512));
    REQUIRE(
        input.push(MidiEvent{.timestampSamples = 1024, .status = 0x90, .data1 = 72, .data2 = 100}));
    const std::vector<float> heard = rig.render(8192);
    CHECK(rms(heard, 2048, heard.size()) > 0.01); // Pad played it
    rig.engine->core().setMidiInput(0, nullptr);
}
