// Live pitch bend and MIDI takes (STATE.md P9-6): a keyboard's bend moves the notes it
// holds, in its RPN 0 range; and what it plays while the arrangement rolls with MIDI
// recording on becomes a pattern, its bend in each note's pitch curve.
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

#include "engine/midi/MidiInput.h"
#include "engine/project/EventCompile.h"
#include "engine/project/Knot.h"
#include "engine/project/commands/CommandStack.h"
#include "engine/record/MidiTake.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::midi::MidiEvent;
using adx::midi::MidiInput;

namespace {

constexpr std::uint32_t kRate = 48000;

/// Frequency of the left channel over [from, from + frames), by rising zero crossings.
double frequency(const std::vector<float>& stereo, std::size_t from, std::size_t frames) {
    std::size_t crossings = 0;
    std::size_t first = 0;
    std::size_t last = 0;
    for (std::size_t i = from + 1; i < from + frames; ++i) {
        if (stereo[(i - 1) * 2] < 0.0F && stereo[i * 2] >= 0.0F) {
            if (crossings == 0) {
                first = i;
            }
            last = i;
            ++crossings;
        }
    }
    if (crossings < 2) {
        return 0.0;
    }
    return static_cast<double>(crossings - 1) * kRate / static_cast<double>(last - first);
}

MidiEvent at(std::uint64_t frame, std::uint8_t status, std::uint8_t d1, std::uint8_t d2) {
    return MidiEvent{.timestampSamples = frame, .status = status, .data1 = d1, .data2 = d2};
}

double cents(double hz, double reference) {
    return 1200.0 * std::log2(hz / reference);
}

} // namespace

TEST_CASE("midi_live_bend_moves_held_notes", "[midi][bend][render]") {
    const adx::tests::OfflineRig rig{256};
    const adx::project::Project project = adx::tests::toneProject({});
    REQUIRE(rig.engine->setProject(project, 1).rebuilt);
    MidiInput input(rig.engine->core().midiClock(), kRate, 256, 0);
    rig.engine->core().setMidiInput(0, &input);
    rig.engine->setMidiTarget(project.channels.front().id);

    REQUIRE(input.push(at(1024, 0x90, 69, 100)));
    std::vector<float> heard = rig.render(kRate / 2);
    const double plain = frequency(heard, 12000, 9600);
    INFO("plain " << plain);
    REQUIRE(plain > 0.0);

    // Full up at the default range: two semitones.
    REQUIRE(input.push(at((kRate / 2) + 100, 0xE0, 0x7F, 0x7F)));
    heard = rig.render(kRate / 2);
    CHECK(std::abs(cents(frequency(heard, 4800, 9600), plain) - 200.0) < 5.0);

    // RPN 0 to twelve semitones, then half way up: 600 cents.
    const std::uint64_t t = kRate;
    REQUIRE(input.push(at(t + 10, 0xB0, 101, 0)));
    REQUIRE(input.push(at(t + 11, 0xB0, 100, 0)));
    REQUIRE(input.push(at(t + 12, 0xB0, 6, 12)));
    REQUIRE(input.push(at(t + 13, 0xE0, 0x00, 0x60))); // 8192 + 4096
    heard = rig.render(kRate / 2);
    CHECK(std::abs(cents(frequency(heard, 4800, 9600), plain) - 600.0) < 5.0);

    // A note struck while the wheel is up starts bent.
    REQUIRE(input.push(at(t + (kRate / 2) + 20, 0x80, 69, 0)));
    REQUIRE(input.push(at(t + (kRate / 2) + 4000, 0x90, 69, 100)));
    heard = rig.render(kRate / 2);
    CHECK(std::abs(cents(frequency(heard, 9600, 9600), plain) - 600.0) < 5.0);
    rig.engine->core().setMidiInput(0, nullptr);
}

TEST_CASE("midi_take_records_notes_and_bend", "[midi][bend][record]") {
    // 120 bpm: a beat is 24000 frames. Playback starts at frame 1024; a note one beat in,
    // held a beat, bent up a whole tone half way through; a second note on beat 3 still
    // held when recording stops at beat 4.
    const adx::tests::OfflineRig rig{256};
    adx::project::Project project = adx::tests::toneProject({});
    REQUIRE(rig.engine->setProject(project, 1).rebuilt);
    MidiInput input(rig.engine->core().midiClock(), kRate, 256, 0);
    rig.engine->core().setMidiInput(0, &input);
    rig.engine->setMidiTarget(project.channels.front().id);
    static_cast<void>(rig.render(1024));
    rig.engine->core().setRecordingMidi(true);
    rig.engine->play();

    const std::uint64_t zero = 1024;
    REQUIRE(input.push(at(zero + 24000, 0x90, 64, 90)));
    REQUIRE(input.push(at(zero + 36000, 0xE0, 0x7F, 0x7F)));
    REQUIRE(input.push(at(zero + 48000, 0x80, 64, 0)));
    REQUIRE(input.push(at(zero + 48001, 0xE0, 0x00, 0x40))); // centre again
    REQUIRE(input.push(at(zero + 72000, 0x90, 67, 80)));
    static_cast<void>(rig.render((std::uint64_t{4} * 24000) + 512));
    rig.engine->core().setRecordingMidi(false);

    std::vector<adx::midi::TakeEvent> take;
    adx::midi::TakeEvent event;
    while (rig.engine->core().takeMidi(event)) {
        take.push_back(event);
    }
    REQUIRE(take.size() == 5);
    CHECK(std::abs(take[0].tick - adx::core::kPpq) <= 1);

    adx::project::CommandStack stack;
    const std::string name = adx::record::commitTake(take, 0, adx::core::kPpq * 4,
                                                     project.channels.front().id, project, stack);
    REQUIRE(name == "Take 1");
    const adx::project::Pattern* pattern = project.findPatternByName("Take 1");
    REQUIRE(pattern != nullptr);
    const adx::project::NoteClip* clip = pattern->clipFor(project.channels.front().id);
    REQUIRE(clip != nullptr);
    REQUIRE(clip->notes.size() == 2);
    const adx::project::Note& first = clip->notes[0];
    CHECK(first.pitch == 64);
    CHECK(std::abs(first.start.value - adx::core::kPpq) <= 1);
    CHECK(std::abs(first.length.value - adx::core::kPpq) <= 2);
    const adx::project::NoteExtras* extras = clip->extrasFor(first.id);
    REQUIRE(extras != nullptr);
    const std::vector<adx::project::Knot> chain = adx::project::pitchOffsetChain(*extras);
    CHECK(std::abs(adx::project::knotValueAt(chain.data(), chain.size(), 0)) < 1.0F);
    CHECK(std::abs(adx::project::knotValueAt(chain.data(), chain.size(), adx::core::kPpq - 10) -
                   200.0F) < 1.0F);
    const adx::project::Note& second = clip->notes[1];
    CHECK(second.pitch == 67);
    // Still held at the stop: it ends there.
    CHECK(std::abs(second.end().value - (adx::core::kPpq * 4)) <= 2);
    CHECK(clip->extrasFor(second.id) == nullptr);

    // One undo step takes it all back.
    REQUIRE(stack.undo(project));
    CHECK(project.findPatternByName("Take 1") == nullptr);
    rig.engine->core().setMidiInput(0, nullptr);
}
