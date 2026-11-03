// MIDI from devices (phase_9.md §4.1, §4.10, §5): the input ring and its clock, the
// router, and clock follow and send. No device is needed; the device thread's side is
// called directly with the times a device would have given.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "engine/midi/MidiClock.h"
#include "engine/midi/MidiInput.h"
#include "engine/midi/MidiRouter.h"
#include "engine/rt/RtSection.h"
#include "engine/rt/Violation.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::midi::AudioClock;
using adx::midi::BlockMidi;
using adx::midi::MidiEvent;
using adx::midi::MidiInput;
using adx::midi::MidiRoute;
using adx::midi::MidiRouter;
using adx::midi::RoutedEvent;

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::int64_t kSecond = 1'000'000'000;

std::array<std::uint8_t, 3> noteOn(std::uint8_t key, std::uint8_t velocity,
                                   std::uint8_t channel = 0) {
    return {static_cast<std::uint8_t>(0x90U | channel), key, velocity};
}

MidiEvent event(std::uint8_t status, std::uint8_t data1, std::uint8_t data2,
                std::uint8_t device = 0) {
    return MidiEvent{
        .timestampSamples = 0, .status = status, .data1 = data1, .data2 = data2, .device = device};
}

} // namespace

TEST_CASE("midi_input_sample_accurate", "[midi][input]") {
    // At three buffer sizes: a message that arrives part-way through a block plays that
    // far into the next one - its true offset, not the block's start.
    for (const std::uint32_t block : {64U, 256U, 1024U}) {
        INFO("block " << block);
        AudioClock clock;
        MidiInput input(clock, kRate, block, 3);
        const std::uint64_t blockStart = 10ULL * block;
        const std::int64_t blockWall = 5 * kSecond;
        clock.publish(blockStart, blockWall);

        // Arrivals at a few fractions of the block.
        std::vector<std::uint32_t> expected;
        for (const std::uint32_t frame : {0U, 1U, block / 3, block - 1}) {
            const std::int64_t wall =
                blockWall + (static_cast<std::int64_t>(frame) * kSecond / kRate);
            const auto bytes = noteOn(60, 100);
            input.onMessage(wall, bytes);
            expected.push_back(frame);
        }
        std::array<BlockMidi, 16> out{};
        // Nothing is due in the block they arrived during.
        CHECK(input.drain(blockStart, block, out) == 0);
        const std::size_t got = input.drain(blockStart + block, block, out);
        REQUIRE(got == expected.size());
        for (std::size_t i = 0; i < got; ++i) {
            CHECK(out.at(i).offset == expected[i]);
            CHECK(out.at(i).event.device == 3);
            CHECK(out.at(i).event.timestampSamples == blockStart + block + expected[i]);
        }
    }
}

TEST_CASE("midi_input_holds_late_and_early_events", "[midi][input]") {
    AudioClock clock;
    MidiInput input(clock, kRate, 256, 0);
    clock.publish(0, 0);
    const auto bytes = noteOn(64, 90);
    input.onMessage(kSecond / 100, bytes); // 480 frames in: due at 736
    std::array<BlockMidi, 4> out{};
    CHECK(input.drain(256, 256, out) == 0); // [256, 512): not yet
    CHECK(input.drain(512, 256, out) == 1); // [512, 768): at 224
    CHECK(out[0].offset == 224);
    // An event whose block has passed plays at the start of the next one.
    input.onMessage(0, bytes); // due at 256
    CHECK(input.drain(1024, 256, out) == 1);
    CHECK(out[0].offset == 0);
}

TEST_CASE("midi_input_drops_what_a_channel_cannot_play", "[midi][input]") {
    AudioClock clock;
    MidiInput input(clock, kRate, 256, 0);
    clock.publish(0, 0);
    const std::array<std::uint8_t, 1> sensing{0xFE};
    const std::array<std::uint8_t, 4> sysex{0xF0, 0x7E, 0x7F, 0xF7};
    const std::array<std::uint8_t, 2> running{0x40, 0x7F}; // no status byte
    input.onMessage(0, sensing);
    input.onMessage(0, sysex);
    input.onMessage(0, running);
    std::array<BlockMidi, 4> out{};
    CHECK(input.drain(0, 100000, out) == 0);
}

TEST_CASE("midi_input_counts_what_it_loses", "[midi][input]") {
    AudioClock clock;
    MidiInput input(clock, kRate, 256, 0);
    clock.publish(0, 0);
    const auto bytes = noteOn(60, 1);
    for (std::size_t i = 0; i < MidiInput::kRingCapacity + 10; ++i) {
        input.onMessage(0, bytes);
    }
    CHECK(input.lostCount() == 10);
}

TEST_CASE("midi_input_no_alloc", "[midi][input][rt]") {
    AudioClock clock;
    MidiInput input(clock, kRate, 256, 0);
    std::array<BlockMidi, 64> out{};
    adx::rt::ViolationLog const& log = adx::rt::ViolationLog::instance();
    const std::uint64_t before = log.count();
    {
        const adx::rt::ScopedRtSection section;
        for (std::uint64_t b = 0; b < 2000; ++b) {
            clock.publish(b * 256, static_cast<std::int64_t>(b) * 256 * kSecond / kRate);
            const auto bytes = noteOn(static_cast<std::uint8_t>(b % 128), 100);
            input.onMessage(static_cast<std::int64_t>(b) * 256 * kSecond / kRate, bytes);
            static_cast<void>(input.drain(b * 256, 256, out));
        }
    }
    CHECK(log.count() == before);
}

TEST_CASE("midi_router_filters", "[midi][router]") {
    struct Case {
        const char* what{};
        MidiRoute route;
        MidiEvent in;
        std::size_t count{};
        std::uint8_t key{};
        std::uint8_t velocity{};
    };
    const std::array<Case, 10> cases{{
        {.what = "passes by default",
         .route = MidiRoute{.target = 7},
         .in = event(0x90, 60, 100),
         .count = 1,
         .key = 60,
         .velocity = 100},
        {.what = "channel filter blocks",
         .route = MidiRoute{.channels = 1U << 2U, .target = 7},
         .in = event(0x91, 60, 100),
         .count = 0,
         .key = 0,
         .velocity = 0},
        {.what = "channel filter passes",
         .route = MidiRoute{.channels = 1U << 1U, .target = 7},
         .in = event(0x91, 60, 100),
         .count = 1,
         .key = 60,
         .velocity = 100},
        {.what = "device filter blocks",
         .route = MidiRoute{.device = 2, .target = 7},
         .in = event(0x90, 60, 100, 1),
         .count = 0,
         .key = 0,
         .velocity = 0},
        {.what = "below the split",
         .route = MidiRoute{.keyLow = 61, .target = 7},
         .in = event(0x90, 60, 100),
         .count = 0,
         .key = 0,
         .velocity = 0},
        {.what = "above the split",
         .route = MidiRoute{.keyHigh = 59, .target = 7},
         .in = event(0x90, 60, 100),
         .count = 0,
         .key = 0,
         .velocity = 0},
        {.what = "velocity range",
         .route = MidiRoute{.velocityLow = 101, .target = 7},
         .in = event(0x90, 60, 100),
         .count = 0,
         .key = 0,
         .velocity = 0},
        {.what = "transpose",
         .route = MidiRoute{.transpose = -12, .target = 7},
         .in = event(0x90, 60, 100),
         .count = 1,
         .key = 48,
         .velocity = 100},
        {.what = "transposed off the keyboard",
         .route = MidiRoute{.transpose = 100, .target = 7},
         .in = event(0x90, 60, 100),
         .count = 0,
         .key = 0,
         .velocity = 0},
        {.what = "soft curve lifts",
         .route = MidiRoute{.curve = 1.0F, .target = 7},
         .in = event(0x90, 60, 32),
         .count = 1,
         .key = 60,
         .velocity = 64},
    }};
    for (const Case& c : cases) {
        INFO(c.what);
        MidiRouter router;
        const std::array<MidiRoute, 1> routes{c.route};
        router.setRoutes(routes);
        std::array<RoutedEvent, 8> out{};
        const std::size_t got = router.route(c.in, out);
        REQUIRE(got == c.count);
        if (got > 0) {
            CHECK(out[0].target == 7);
            CHECK(out[0].event.data1 == c.key);
            CHECK(out[0].event.data2 == c.velocity);
        }
    }
}

TEST_CASE("midi_router_note_off_follows_its_note_on", "[midi][router]") {
    // A split keyboard, two layers on the left; the routes change while keys are held.
    const std::array<MidiRoute, 3> split{{
        MidiRoute{.keyHigh = 59, .target = 1},
        MidiRoute{.keyHigh = 59, .transpose = 12, .target = 2},
        MidiRoute{.keyLow = 60, .target = 3},
    }};
    MidiRouter router;
    router.setRoutes(split);
    std::array<RoutedEvent, 8> out{};
    REQUIRE(router.route(event(0x90, 48, 90), out) == 2);
    REQUIRE(router.route(event(0x90, 72, 90), out) == 1);

    // Everything to target 9 now: the held keys still release where they sounded.
    const std::array<MidiRoute, 1> moved{MidiRoute{.transpose = 5, .target = 9}};
    router.setRoutes(moved);
    REQUIRE(router.route(event(0x80, 48, 0), out) == 2);
    CHECK(out[0].target == 1);
    CHECK(out[0].event.data1 == 48);
    CHECK(out[1].target == 2);
    CHECK(out[1].event.data1 == 60);
    // A note-on with velocity 0 is a note-off too.
    REQUIRE(router.route(event(0x90, 72, 0), out) == 1);
    CHECK(out[0].target == 3);
    CHECK(out[0].event.kind() == adx::midi::MidiKind::NoteOff);
    // And a key never pressed releases nothing.
    CHECK(router.route(event(0x80, 30, 0), out) == 0);
}

TEST_CASE("midi_router_controllers_fan_out", "[midi][router]") {
    const std::array<MidiRoute, 2> layers{{MidiRoute{.target = 1}, MidiRoute{.target = 2}}};
    MidiRouter router;
    router.setRoutes(layers);
    std::array<RoutedEvent, 8> out{};
    CHECK(router.route(event(0xB0, 1, 64), out) == 2); // mod wheel
    CHECK(router.route(event(0xE0, 0, 96), out) == 2); // bend
}

TEST_CASE("midi_velocity_curve_shape", "[midi][router]") {
    using adx::midi::curveVelocity;
    for (int v = 1; v <= 127; ++v) {
        const auto in = static_cast<std::uint8_t>(v);
        CHECK(curveVelocity(0.0F, in) == in);
        CHECK(curveVelocity(0.5F, in) >= in);
        CHECK(curveVelocity(-0.5F, in) <= in);
        CHECK(curveVelocity(-1.0F, in) >= 1);
    }
    CHECK(curveVelocity(1.0F, 127) == 127);
    CHECK(curveVelocity(-1.0F, 127) == 127);
}

TEST_CASE("midi_clock_pll_stability", "[midi][clock]") {
    // 120 bpm is a pulse every 1000 samples at 48 kHz. Each pulse arrives up to a
    // millisecond early or late, as a USB keyboard's clock does; after two bars the
    // followed tempo stays within 0.1 %, which is inaudible as wow.
    adx::midi::ClockFollower follower(kRate);
    std::mt19937 random(42);
    std::uniform_real_distribution<double> jitter(-48.0, 48.0);
    double worst = 0.0;
    for (int pulse = 0; pulse < 24 * 4 * 32; ++pulse) {
        const double ideal = (1000.0 * pulse) + 100000.0;
        follower.pulse(static_cast<std::uint64_t>(std::llround(ideal + jitter(random))));
        if (pulse > 24 * 4 * 2) {
            worst = std::max(worst, std::abs(follower.bpm() - 120.0) / 120.0);
        }
    }
    INFO("worst deviation " << worst * 100.0 << " %");
    CHECK(worst < 0.001);

    // And it follows a change: 120 to 126 bpm, within 0.1 % four bars later.
    double at = (1000.0 * (24 * 4 * 32)) + 100000.0;
    const double period = 60.0 * kRate / (126.0 * 24.0);
    for (int pulse = 0; pulse < 24 * 4 * 8; ++pulse) {
        at += period;
        follower.pulse(static_cast<std::uint64_t>(std::llround(at + jitter(random))));
    }
    CHECK(std::abs(follower.bpm() - 126.0) / 126.0 < 0.001);
}

TEST_CASE("midi_clock_follows_position", "[midi][clock]") {
    adx::midi::ClockFollower follower(kRate);
    follower.songPosition(16); // bar 2 in 4/4: sixteen sixteenths
    follower.resume();
    follower.pulse(0); // the downbeat itself
    CHECK(follower.position().value == 16 * (adx::core::kPpq / 4));
    for (int i = 0; i < 24; ++i) {
        follower.pulse(1000ULL * static_cast<std::uint64_t>(i + 1));
    }
    CHECK(follower.position().value == (16 * (adx::core::kPpq / 4)) + adx::core::kPpq);
    follower.stop();
    follower.pulse(30000);
    CHECK(follower.position().value == (16 * (adx::core::kPpq / 4)) + adx::core::kPpq);
    follower.start();
    follower.pulse(31000);
    CHECK(follower.position().value == 0);
}

TEST_CASE("midi_clock_sends_24_a_beat", "[midi][clock]") {
    // At 120 bpm, 256-frame blocks: a second is two beats, 48 pulses, each where the
    // beat's fraction puts it.
    const std::int64_t ticksPerSecond = adx::core::kPpq * 2;
    std::vector<std::uint64_t> at;
    std::array<adx::midi::ClockMessage, 32> out{};
    for (std::uint64_t frame = 0; frame < kRate; frame += 256) {
        const auto from = static_cast<std::int64_t>(frame) * ticksPerSecond / kRate;
        const auto to = static_cast<std::int64_t>(frame + 256) * ticksPerSecond / kRate;
        const std::size_t got = adx::midi::clockPulses(from, to, 256, out);
        for (std::size_t i = 0; i < got; ++i) {
            CHECK(out.at(i).status == adx::midi::kClockPulse);
            if (frame + out.at(i).offset < kRate) { // the last block runs past the second
                at.push_back(frame + out.at(i).offset);
            }
        }
    }
    REQUIRE(at.size() == 48);
    // Within a tick of the beat's fraction: a block's range is whole ticks, and a tick at
    // 120 bpm is 6.25 frames (0.13 ms - less than one MIDI byte takes on the wire).
    for (std::size_t i = 0; i < at.size(); ++i) {
        CHECK(std::llabs(static_cast<long long>(at[i]) - static_cast<long long>(i * 1000)) <= 7);
    }
    const adx::midi::ClockMessage position =
        adx::midi::songPositionAt(adx::core::kPpq * 4 * 300, 0);
    CHECK(position.status == adx::midi::kSongPosition);
    CHECK((position.data1 | (position.data2 << 7U)) == 300 * 16);
}

TEST_CASE("midi_input_plays_at_its_offset_through_the_engine", "[midi][input][render]") {
    // A live note reaches its channel's instrument at its own frame, whatever the block
    // size - including in the second piece of a block split by a loop boundary.
    std::vector<std::size_t> onsets;
    for (const std::uint32_t block : {64U, 256U, 1024U}) {
        INFO("block " << block);
        adx::tests::OfflineRig rig{block};
        const adx::project::Project project = adx::tests::toneProject({});
        REQUIRE(rig.engine->setProject(project, 1).rebuilt);
        MidiInput input(rig.engine->core().midiClock(), kRate, block, 0);
        rig.engine->core().setMidiInput(0, &input);
        rig.engine->setMidiTarget(project.channels.front().id);
        // A loop ending at tick 1645 - frame 10281.25 at 120 bpm - splits the block that
        // holds it there at every block size, and the note lands just after the split.
        rig.engine->setLoop(
            adx::transport::LoopRegion{.start = adx::core::Ticks{0},
                                       .end = adx::core::Ticks{adx::core::kPpq * 3 / 7},
                                       .enabled = true});
        rig.engine->play();
        const std::uint64_t at = 10290;
        REQUIRE(input.push(
            MidiEvent{.timestampSamples = at, .status = 0x90, .data1 = 69, .data2 = 100}));
        std::vector<float> heard;
        rig.render(16384, heard);
        const std::size_t onset = adx::tests::firstSoundFrom(heard, 0);
        INFO("onset " << onset << ", note at " << at);
        CHECK(onset >= at);
        CHECK(onset <= at + 1);
        onsets.push_back(onset - at);
        CHECK(rig.stats().subBlocks > 16384 / block); // the loop did split blocks

        REQUIRE(input.push(MidiEvent{.timestampSamples = 16384 + 3, .status = 0x80, .data1 = 69}));
        heard.clear();
        rig.render(kRate, heard);
        CHECK(std::abs(heard[heard.size() - 2]) < 1e-4F); // released and rung out
        rig.engine->core().setMidiInput(0, nullptr);
    }
    // And the same frame at every block size.
    CHECK(std::ranges::all_of(onsets, [&](std::size_t o) { return o == onsets.front(); }));
}
