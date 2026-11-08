// MIDI sync through the engine (STATE.md P9-5, phase_9.md §4.10): clock and time code
// sent from the arrangement, and the arrangement following incoming clock and time
// code. No device: an output's ring is read directly, and an input's ring is fed with
// the stream frames a device would have produced.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/midi/MidiClock.h"
#include "engine/midi/MidiInput.h"
#include "engine/midi/MidiOutput.h"
#include "engine/midi/MidiSync.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::midi::MidiEvent;
using adx::midi::MidiInput;
using adx::midi::MidiOutput;
using adx::midi::OutMessage;

namespace {

constexpr std::uint32_t kRate = 48000;

std::vector<OutMessage> drainAll(MidiOutput& output) {
    std::vector<OutMessage> out;
    OutMessage message;
    while (output.pop(message)) {
        out.push_back(message);
    }
    return out;
}

std::vector<OutMessage> only(const std::vector<OutMessage>& messages, std::uint8_t status) {
    std::vector<OutMessage> out;
    for (const OutMessage& message : messages) {
        if (message.bytes[0] == status) {
            out.push_back(message);
        }
    }
    return out;
}

} // namespace

TEST_CASE("midi_clock_out_through_the_engine", "[midi][sync]") {
    // 120 bpm at 48 kHz: a beat is 24000 frames, a pulse 1000. Start, then 24 pulses a
    // beat each on its frame, then stop; a seek while playing is stop, position, continue.
    for (const std::uint32_t block : {64U, 256U, 1024U}) {
        INFO("block " << block);
        const adx::tests::OfflineRig rig{block};
        const adx::project::Project project = adx::tests::toneProject({});
        REQUIRE(rig.engine->setProject(project, 1).rebuilt);
        MidiOutput output;
        output.sync().clock.store(true);
        rig.engine->core().setMidiOutput(0, &output);

        static_cast<void>(rig.render(std::uint64_t{block} * 4)); // stopped: nothing
        CHECK(drainAll(output).empty());

        const std::uint64_t startedAt = std::uint64_t{block} * 4;
        rig.engine->play();
        static_cast<void>(rig.render(std::uint64_t{4} * 24000));
        const std::vector<OutMessage> played = drainAll(output);
        REQUIRE_FALSE(played.empty());
        CHECK(played.front().bytes[0] == adx::midi::kClockStart);
        CHECK(played.front().streamFrame == startedAt);
        const std::vector<OutMessage> pulses = only(played, adx::midi::kClockPulse);
        CHECK(pulses.size() == std::size_t{96});
        for (std::size_t k = 0; k < pulses.size(); ++k) {
            const auto ideal = static_cast<std::int64_t>(startedAt + (1000 * k));
            CHECK(std::abs(static_cast<std::int64_t>(pulses[k].streamFrame) - ideal) <= 1);
        }

        rig.engine->seek(adx::core::Ticks{adx::core::kPpq * 8});
        static_cast<void>(rig.render(std::uint64_t{block} * 2));
        const std::vector<OutMessage> jumped = drainAll(output);
        REQUIRE(jumped.size() >= 3);
        CHECK(jumped[0].bytes[0] == adx::midi::kClockStop);
        CHECK(jumped[1].bytes[0] == adx::midi::kSongPosition);
        CHECK((jumped[1].bytes[1] | (jumped[1].bytes[2] << 7U)) == 32U); // 8 beats = 32 16ths
        CHECK(jumped[2].bytes[0] == adx::midi::kClockContinue);

        rig.engine->stopPlayback();
        static_cast<void>(rig.render(std::uint64_t{block} * 2));
        const std::vector<OutMessage> stopped = drainAll(output);
        REQUIRE_FALSE(stopped.empty());
        CHECK(stopped.back().bytes[0] == adx::midi::kClockStop);
        CHECK(only(stopped, adx::midi::kClockPulse).empty());

        // An output that asked for time code only is sent no clock.
        output.sync().clock.store(false);
        output.sync().mtc.store(true);
        rig.engine->play();
        static_cast<void>(rig.render(24000));
        const std::vector<OutMessage> codes = drainAll(output);
        CHECK(only(codes, adx::midi::kClockPulse).empty());
        CHECK_FALSE(only(codes, adx::midi::kMtcQuarterFrame).empty());
        rig.engine->core().setMidiOutput(0, nullptr);
    }
}

TEST_CASE("midi_mtc_out_round_trips_through_the_follower", "[midi][sync]") {
    // A second of playback from 1:00 is 120 quarter frames after one full frame, and a
    // follower fed them reads the position back within a frame.
    const adx::tests::OfflineRig rig{256};
    const adx::project::Project project = adx::tests::toneProject({});
    REQUIRE(rig.engine->setProject(project, 1).rebuilt);
    MidiOutput output;
    output.sync().mtc.store(true);
    rig.engine->core().setMidiOutput(0, &output);
    rig.engine->seek(adx::core::Ticks{adx::core::kPpq * 120}); // 60 s at 120 bpm
    rig.engine->play();
    static_cast<void>(rig.render(kRate));
    const std::vector<OutMessage> sent = drainAll(output);
    REQUIRE_FALSE(sent.empty());
    REQUIRE(sent.front().size == 10);  // the full frame
    CHECK(sent.front().bytes[6] == 1); // minutes
    CHECK(sent.front().bytes[7] == 0); // seconds
    const std::vector<OutMessage> quarters = only(sent, adx::midi::kMtcQuarterFrame);
    CHECK(quarters.size() >= 119);
    CHECK(quarters.size() <= 121);

    adx::midi::MtcFollower follower;
    for (const OutMessage& q : quarters) {
        follower.quarter(q.bytes[1], q.streamFrame, kRate);
    }
    REQUIRE(follower.locked());
    const std::uint64_t last = quarters.back().streamFrame;
    // The stream frame playback started on is where 60 s is.
    const double expected = 60.0 + (static_cast<double>(last - sent.front().streamFrame) / kRate);
    INFO("follower " << follower.secondsAt(last, kRate) << " expected " << expected);
    CHECK(std::abs(follower.secondsAt(last, kRate) - expected) < 1.0 / 30.0);
    CHECK(std::abs(follower.rate() - 1.0) < 0.01);
    rig.engine->core().setMidiOutput(0, nullptr);
}

TEST_CASE("midi_mtc_timecode_pieces", "[midi][sync]") {
    const adx::midi::Timecode code =
        adx::midi::timecodeOf((((1 * 3600) + (2 * 60) + 3) * 30) + 4, 30);
    CHECK(code.hours == 1);
    CHECK(code.minutes == 2);
    CHECK(code.seconds == 3);
    CHECK(code.frames == 4);
    adx::midi::MtcFollower follower;
    for (std::uint32_t piece = 0; piece < 8; ++piece) {
        const adx::midi::SyncMessage q = adx::midi::quarterFrame(code, piece, 0);
        CHECK(q.bytes[0] == adx::midi::kMtcQuarterFrame);
        const std::uint32_t high = std::uint32_t{q.bytes[1]} >> 4U;
        CHECK(high == piece);
        follower.quarter(q.bytes[1], 400ULL * piece, kRate);
    }
    // Piece 7 arrives seven quarter frames after the frame the code names.
    const double named = 3600.0 + 120.0 + 3.0 + (4.0 / 30.0);
    CHECK(std::abs(follower.secondsAt(2800, kRate) - (named + (7.0 / 120.0))) < 1e-9);
}

TEST_CASE("midi_clock_follow_drives_the_arrangement", "[midi][sync]") {
    // The project says 120 bpm; the clock says 130. After eight seconds the arrangement
    // runs at the clock's rate and sits on its beat.
    const adx::tests::OfflineRig rig{256};
    const adx::project::Project project = adx::tests::toneProject({});
    REQUIRE(rig.engine->setProject(project, 1).rebuilt);
    MidiInput input(rig.engine->core().midiClock(), kRate, 256, 0);
    rig.engine->core().setMidiInput(0, &input);
    rig.engine->setSyncFollow(adx::graph::SyncFollow::Clock);
    static_cast<void>(rig.render(1024));

    const double period = 60.0 * kRate / (130.0 * 24.0);
    const double startAt = 2048.0;
    REQUIRE(input.push(MidiEvent{.timestampSamples = 2048, .status = adx::midi::kClockStart}));
    const int pulses = static_cast<int>(8.0 * kRate / period);
    std::uint64_t rendered = 1024;
    for (int k = 0; k < pulses; ++k) {
        const auto at = static_cast<std::uint64_t>(std::llround(startAt + (k * period)));
        // The ring holds 4096; render as we go, as a device would deliver them.
        while (rendered + 2048 < at) {
            static_cast<void>(rig.render(256));
            rendered += 256;
        }
        REQUIRE(input.push(MidiEvent{.timestampSamples = at, .status = adx::midi::kClockPulse}));
    }
    while (rendered < static_cast<std::uint64_t>(startAt + (pulses * period))) {
        static_cast<void>(rig.render(256));
        rendered += 256;
    }
    REQUIRE(rig.engine->state() == adx::transport::PlayState::Playing);
    const double clockTicks =
        (static_cast<double>(rendered) - startAt) / period * adx::midi::kTicksPerPulse;
    const double ours = static_cast<double>(rig.engine->positionTicks().value);
    INFO("arrangement " << ours << " ticks, clock " << clockTicks);
    CHECK(std::abs(ours - clockTicks) < adx::core::kPpq / 8.0);
    const double rate = rig.engine->transport().arrangement().rate();
    INFO("rate " << rate);
    CHECK(std::abs(rate - (130.0 / 120.0)) < 0.005);

    // Stop stops it; following off returns the rate to 1.
    REQUIRE(
        input.push(MidiEvent{.timestampSamples = rendered + 300, .status = adx::midi::kClockStop}));
    static_cast<void>(rig.render(2048));
    CHECK(rig.engine->state() == adx::transport::PlayState::Stopped);
    rig.engine->setSyncFollow(adx::graph::SyncFollow::None);
    static_cast<void>(rig.render(512));
    CHECK(rig.engine->transport().arrangement().rate() == 1.0);
    rig.engine->core().setMidiInput(0, nullptr);
}

TEST_CASE("midi_mtc_follow_chases_the_code", "[midi][sync]") {
    // Time code from 10 s at real time: the arrangement jumps there and stays within a
    // frame of it; when the code stops, playback stops.
    const adx::tests::OfflineRig rig{256};
    const adx::project::Project project = adx::tests::toneProject({});
    REQUIRE(rig.engine->setProject(project, 1).rebuilt);
    MidiInput input(rig.engine->core().midiClock(), kRate, 256, 0);
    rig.engine->core().setMidiInput(0, &input);
    rig.engine->setSyncFollow(adx::graph::SyncFollow::Mtc);
    static_cast<void>(rig.render(1024));

    const double quarter = kRate / 120.0;
    const std::int64_t firstFrame = std::int64_t{10} * 30;
    const std::uint64_t startAt = 2048;
    std::uint64_t rendered = 1024;
    const int quarters = 120 * 3;
    std::uint64_t lastAt = 0;
    for (int q = 0; q < quarters; ++q) {
        const auto piece = static_cast<std::uint32_t>(q % 8);
        const std::int64_t frame = firstFrame + ((q - static_cast<int>(piece)) / 4);
        const adx::midi::SyncMessage message =
            adx::midi::quarterFrame(adx::midi::timecodeOf(frame, 30), piece, 0);
        lastAt = startAt + static_cast<std::uint64_t>(std::llround(q * quarter));
        while (rendered + 2048 < lastAt) {
            static_cast<void>(rig.render(256));
            rendered += 256;
        }
        REQUIRE(input.push(MidiEvent{.timestampSamples = lastAt,
                                     .status = adx::midi::kMtcQuarterFrame,
                                     .data1 = message.bytes[1]}));
    }
    while (rendered < lastAt) {
        static_cast<void>(rig.render(256));
        rendered += 256;
    }
    REQUIRE(rig.engine->state() == adx::transport::PlayState::Playing);
    const double code = 10.0 + (static_cast<double>(rendered - startAt) / kRate);
    const double ours = static_cast<double>(rig.engine->positionSamples()) / kRate;
    INFO("arrangement " << ours << " s, code " << code);
    CHECK(std::abs(ours - code) < 1.0 / 30.0);

    static_cast<void>(rig.render(kRate / 5)); // the code has stopped
    CHECK(rig.engine->state() == adx::transport::PlayState::Stopped);
    rig.engine->setSyncFollow(adx::graph::SyncFollow::None);
    rig.engine->core().setMidiInput(0, nullptr);
}
