// Pad controllers (phase_11.md §4.7, §5): pad_profile_parse, pad_learn_builds_profile,
// pad_velocity_to_gain, pad_feedback_thread, and pads launching through the engine.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "engine/midi/MidiInput.h"
#include "engine/midi/PadController.h"
#include "engine/midi/PadFeedback.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::midi::PadAction;
using adx::midi::PadLook;

namespace {

std::filesystem::path profiles() {
    return std::filesystem::path{ADX_REPO_ROOT} / "app" / "adx" / "pads" / "profiles";
}

std::string read(const std::filesystem::path& path) {
    std::ifstream const in{path, std::ios::binary};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

adx::midi::ControlMessage note(std::uint8_t number, std::uint8_t velocity = 100) {
    adx::midi::ControlMessage m;
    m.kind = adx::project::MidiControlKind::Note;
    m.number = number;
    m.pressed = velocity > 0;
    m.value = static_cast<float>(velocity) / 127.0F;
    return m;
}

adx::midi::ControlMessage cc(std::uint8_t number) {
    adx::midi::ControlMessage m;
    m.kind = adx::project::MidiControlKind::ControlChange;
    m.number = number;
    m.value = 1.0F;
    return m;
}

} // namespace

TEST_CASE("pad_profile_parse", "[pads]") {
    // Every shipped profile loads with no warning, and writes back to itself.
    std::size_t found = 0;
    for (const auto& entry : std::filesystem::directory_iterator{profiles()}) {
        if (entry.path().extension() != ".adxpad") {
            continue;
        }
        ++found;
        INFO(entry.path().filename().string());
        const adx::midi::PadParse parse = adx::midi::parsePadProfile(read(entry.path()));
        CHECK(parse.ok);
        CHECK(parse.warnings.empty());
        CHECK_FALSE(parse.profile.name.empty());
        CHECK(parse.profile.columns * parse.profile.rows == 64);
        const adx::midi::PadParse again =
            adx::midi::parsePadProfile(adx::midi::writePadProfile(parse.profile));
        CHECK(again.profile == parse.profile);
    }
    CHECK(found >= 3);

    // The Launchpad: top-left 81, a row down 71; scene 1 is CC 89.
    const adx::midi::PadParse lp =
        adx::midi::parsePadProfile(read(profiles() / "launchpad_mini_mk3.adxpad"));
    CHECK(lp.profile.noteAt(0, 0) == 81);
    CHECK(lp.profile.noteAt(7, 0) == 88);
    CHECK(lp.profile.noteAt(0, 7) == 11);
    CHECK(lp.profile.colors.at(static_cast<std::size_t>(PadLook::Queued)).blink);
    const adx::midi::PadMap map = adx::midi::compilePadMap(lp.profile);
    CHECK(map.actionFor(0x90, 81, 100).kind == PadAction::Kind::Cell);
    CHECK(map.actionFor(0x90, 72, 100).track == 1);
    CHECK(map.actionFor(0x90, 72, 100).row == 1);
    CHECK(map.actionFor(0xB0, 79, 127).kind == PadAction::Kind::Scene);
    CHECK(map.actionFor(0xB0, 79, 127).row == 1);
    CHECK(map.actionFor(0x91, 81, 100).kind == PadAction::Kind::None); // channel 2
    CHECK(map.actionFor(0x90, 81, 0).kind == PadAction::Kind::None);   // a release
    CHECK(adx::midi::profileMatches(lp.profile, "LPMiniMK3 MIDI 1"));
    CHECK_FALSE(adx::midi::profileMatches(lp.profile, "loopMIDI Port 0"));

    // An unknown key warns and is kept, and written back.
    const std::string text = "[PAD_PROFILE]\nNAME=\"X\"\nGRID=4x2\nNOTE_BASE=36 ROW_STRIDE=4\n"
                             "SHINY=yes\n[FEEDBACK]\nMODE=none\n[CONTROLS]\nSTOP_ALL=cc:7\n"
                             "[EXTRA]\nWHO=me\n";
    const adx::midi::PadParse odd = adx::midi::parsePadProfile(text);
    CHECK(odd.ok);
    CHECK(odd.warnings.size() == 3); // SHINY, [EXTRA], WHO
    const std::string written = adx::midi::writePadProfile(odd.profile);
    CHECK(written.find("SHINY=yes") != std::string::npos);
    CHECK(written.find("[EXTRA]\nWHO=me") != std::string::npos);
    CHECK(adx::midi::parsePadProfile(written).profile == odd.profile);
    // And a text that is not a profile says so.
    CHECK_FALSE(adx::midi::parsePadProfile("[PROJECT]\nX=1\n").ok);
}

TEST_CASE("pad_learn_builds_profile", "[pads]") {
    // A regular 4x2 grid (36 + row * 4 + column), then two scene CCs, stop-all on a pad,
    // tap skipped: NOTE_BASE and strides, and the controls, as pressed.
    adx::midi::PadLearn learn{"Learned", 4, 2};
    CHECK(learn.prompt() == "pad 1, 1");
    for (std::uint8_t r = 0; r < 2; ++r) {
        for (std::uint8_t c = 0; c < 4; ++c) {
            CHECK(learn.press(note(static_cast<std::uint8_t>(36 + (r * 4) + c))));
        }
    }
    CHECK_FALSE(learn.press(note(36)));    // already a pad
    CHECK_FALSE(learn.press(note(50, 0))); // a release is not a press
    CHECK(learn.prompt() == "scene 1");
    CHECK(learn.press(cc(20)));
    CHECK(learn.press(cc(21)));
    CHECK(learn.prompt() == "stop all");
    CHECK(learn.press(note(60)));
    learn.skip();
    CHECK(learn.done());
    const adx::midi::PadProfile p = learn.result();
    CHECK(p.notes.empty());
    CHECK(p.noteBase == 36);
    CHECK(p.columnStride == 1);
    CHECK(p.rowStride == 4);
    CHECK(p.sceneLaunch.cc);
    CHECK(p.sceneLaunch.numbers == std::vector<std::uint8_t>{20, 21});
    CHECK_FALSE(p.stopAll.cc);
    CHECK(p.stopAll.numbers == std::vector<std::uint8_t>{60});
    CHECK(p.tapTempo.numbers.empty());
    CHECK(adx::midi::parsePadProfile(adx::midi::writePadProfile(p)).profile == p);

    // An irregular grid is written pad by pad.
    adx::midi::PadLearn odd{"Odd", 2, 2};
    for (const std::uint8_t n : std::vector<std::uint8_t>{10, 3, 77, 5}) {
        CHECK(odd.press(note(n)));
    }
    odd.skip(); // no scene buttons
    odd.skip();
    odd.skip();
    const adx::midi::PadProfile q = odd.result();
    CHECK(q.notes == std::vector<std::uint8_t>{10, 3, 77, 5});
    CHECK(q.noteAt(0, 1) == 77);
}

TEST_CASE("pad_velocity_to_gain", "[pads]") {
    CHECK(adx::midi::padGain(127, "linear") == 1.0F);
    CHECK(std::abs(adx::midi::padGain(64, "linear") - (64.0F / 127.0F)) < 1e-6F);
    CHECK(adx::midi::padGain(64, "soft") > adx::midi::padGain(64, "linear"));
    CHECK(adx::midi::padGain(64, "hard") < adx::midi::padGain(64, "linear"));
    CHECK(adx::midi::padGain(1, "hard") > 0.0F);
    adx::midi::PadProfile p;
    p.velocitySensitive = true;
    p.velocityCurve = "soft";
    const adx::midi::PadMap map = adx::midi::compilePadMap(p);
    CHECK(map.velocitySensitive);
    CHECK(map.gain.at(100) == adx::midi::padGain(100, "soft"));
}

TEST_CASE("pad_feedback_thread", "[pads]") {
    // The lights run on their own thread at 30 Hz, send only what changed, follow the
    // looks they are given - and keep their rate while the calling thread is busy.
    const adx::midi::PadParse parse =
        adx::midi::parsePadProfile(read(profiles() / "adx_virtual_pad.adxpad"));
    REQUIRE(parse.ok);
    std::atomic<int> playing{-1};
    std::mutex lock;
    std::vector<std::vector<std::uint8_t>> sent;
    std::thread::id sender;
    {
        adx::midi::PadFeedback const feedback{parse.profile,
                                              [&](std::uint32_t c, std::uint32_t r) {
                                                  if (static_cast<int>((r * 8) + c) ==
                                                      playing.load()) {
                                                      return PadLook::Playing;
                                                  }
                                                  return r == 0 ? PadLook::Stopped : PadLook::Empty;
                                              },
                                              [&](std::span<const std::uint8_t> bytes) {
                                                  const std::scoped_lock hold{lock};
                                                  sender = std::this_thread::get_id();
                                                  sent.emplace_back(bytes.begin(), bytes.end());
                                                  return true;
                                              }};
        // Busy here: the lights keep their rate regardless.
        const auto started = std::chrono::steady_clock::now();
        volatile double sink = 0.0;
        while (std::chrono::steady_clock::now() - started < std::chrono::milliseconds(500)) {
            sink = sink + 1.0;
        }
        playing.store(3);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const std::uint64_t ticks = feedback.ticks();
        INFO(ticks << " ticks in one second");
        CHECK(ticks >= 26);
        CHECK(ticks <= 34);
        CHECK(feedback.shown(3, 0) == PadLook::Playing);
        CHECK(feedback.shown(2, 0) == PadLook::Stopped);
        CHECK(feedback.shown(2, 1) == PadLook::Empty);
        CHECK(feedback.thread() != std::this_thread::get_id());
    }
    const std::scoped_lock hold{lock};
    CHECK(sender != std::this_thread::get_id());
    // 64 first lights, then the one that changed.
    CHECK(sent.size() == 65);
    CHECK(sent.back() == std::vector<std::uint8_t>{0x9F, 3, 3}); // channel 16, pad 3, PLAYING
}

TEST_CASE("pads_launch_through_the_engine", "[pads]") {
    // A pad map on input slot 0: a press launches its cell with the velocity's gain, a
    // scene pad fires a row, and nothing the pads send plays the instrument.
    const std::string text = "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n"
                             "\n[CHANNEL Lead]\nINSTRUMENT=testtone\nOUTPUT=insert.1\n"
                             "\n[PATTERN A]\nLENGTH=1:0:0\nNOTES Lead\n  C5 0:0:0 1:0:0 100\n"
                             "\n[PLAYLIST]\nTRACK 1 name=\"Lead\"\n"
                             "\n[MIXER]\nINSERT 1 name=\"Master\"\n"
                             "\n[SESSION]\nQUANTIZE=instant\nSCENE \"One\"\nSCENE \"Two\"\n"
                             "TRACK Lead linked=1\n  1 A\n  2 A\n";
    const auto loaded = adx::tests::loadText(text);
    adx::tests::OfflineRig const rig{256};
    REQUIRE(rig.engine->setProject(loaded->project, 1).rebuilt);
    const adx::midi::PadParse parse =
        adx::midi::parsePadProfile(read(profiles() / "adx_virtual_pad.adxpad"));
    const adx::midi::PadMap map = adx::midi::compilePadMap(parse.profile);
    rig.engine->setPadMap(0, &map);
    adx::midi::MidiInput input(rig.engine->core().midiClock(), 48000, 256, 0);
    rig.engine->core().setMidiInput(0, &input);
    std::vector<float> out;
    std::uint64_t at = 256;
    rig.render(256, out);
    const auto send = [&](std::uint8_t status, std::uint8_t data1, std::uint8_t data2) {
        REQUIRE(input.push(adx::midi::MidiEvent{
            .timestampSamples = at, .status = status, .data1 = data1, .data2 = data2}));
    };
    send(0x90, 8, 64); // pad (0, 1): track 0, row 1
    rig.render(256ULL * 8, out);
    at += 256ULL * 8;
    CHECK(rig.engine->core().session().state(0, 1) == adx::session::ClipState::Playing);
    CHECK(rig.engine->core().padPresses() == 1);
    send(0x90, 64, 100); // scene 1
    rig.render(256ULL * 8, out);
    at += 256ULL * 8;
    CHECK(rig.engine->core().session().state(0, 0) == adx::session::ClipState::Playing);
    // A note on another channel - a light looping back - is the pads' too: it plays
    // nothing once the clips have stopped.
    rig.engine->stopAllClips();
    rig.render(256ULL * 200, out); // past the release tail
    at += 256ULL * 200;
    CHECK(rig.engine->core().session().state(0, 0) == adx::session::ClipState::Stopped);
    CHECK(rig.engine->core().session().active() == 0);
    out.clear();
    rig.render(256, out);
    at += 256;
    float before = 0.0F;
    for (const float s : out) {
        before = std::max(before, std::abs(s));
    }
    CHECK(before == 0.0F);
    out.clear();
    send(0x9F, 20, 3);
    rig.render(9600, out);
    float peak = 0.0F;
    for (const float s : out) {
        peak = std::max(peak, std::abs(s));
    }
    CHECK(peak == 0.0F);
    CHECK(rig.engine->core().padPresses() == 2);
    rig.engine->core().setMidiInput(0, nullptr);
    rig.engine->setPadMap(0, nullptr);
}
