// midiout (phase_11.5.md §4.6): a channel's notes leave as MIDI on the output in its
// `port` slot, on its channel, stamped with the stream frame they belong to; every note
// it starts is ended, by its note-off or a release; no output, no messages.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <vector>

#include "engine/midi/LiveOutputs.h"
#include "engine/midi/MidiOutput.h"
#include "tests/cpp/instruments/InstrumentHarness.h"

using adx::tests::instrumentParamIndex;
using adx::tests::instrumentParams;
using adx::tests::noteOff;
using adx::tests::noteOn;
using adx::tests::preparedInstrument;
using adx::tests::runInstrument;

namespace {

struct Sent {
    std::uint64_t frame;
    std::uint8_t status;
    std::uint8_t key;
    std::uint8_t velocity;
};

std::vector<Sent> drain(adx::midi::MidiOutput& out) {
    std::vector<Sent> sent;
    adx::midi::OutMessage m;
    while (out.pop(m)) {
        sent.push_back({.frame = m.streamFrame,
                        .status = m.bytes[0],
                        .key = m.bytes[1],
                        .velocity = m.bytes[2]});
    }
    return sent;
}

} // namespace

TEST_CASE("midiout_sends_notes", "[instruments][midiout]") {
    auto output = std::make_unique<adx::midi::MidiOutput>();
    auto& live = adx::midi::liveOutputs();
    live.outputs.at(2).store(output.get());
    live.blockFrame.store(100000);
    std::vector<float> params = instrumentParams("midiout");
    params[instrumentParamIndex("midiout", "port")] = 2.0F;
    params[instrumentParamIndex("midiout", "channel")] = 4.0F;

    const auto run = runInstrument(
        *preparedInstrument("midiout"),
        {noteOn(10, 1, 60, 99), noteOn(20, 2, 64, 0), noteOff(30, 1, 60)}, 64, params);
    REQUIRE(run.violations == 0);
    for (const float s : run.left) {
        REQUIRE(s == 0.0F); // no sound of its own
    }
    const auto sent = drain(*output);
    REQUIRE(sent.size() == 3);
    CHECK(sent[0].status == 0x93); // channel 4
    CHECK(sent[0].key == 60);
    CHECK(sent[0].velocity == 99);
    CHECK(sent[0].frame == 100010);
    CHECK(sent[1].velocity == 1); // a velocity-0 note still sounds, quietly
    CHECK(sent[2].status == 0x83);
    CHECK(sent[2].frame == 100030);

    // A release ends what is still held.
    auto node = preparedInstrument("midiout");
    static_cast<void>(runInstrument(*node, {noteOn(0, 7, 50)}, 64, params));
    REQUIRE(drain(*output).size() == 1);
    adx::graph::BlockEvent all = noteOn(5, 0, 0);
    all.kind = adx::graph::BlockEventKind::ReleaseAll;
    all.timeSource = 0;
    static_cast<void>(runInstrument(*node, {all}, 64, params));
    const auto released = drain(*output);
    REQUIRE(released.size() == 1);
    CHECK(released[0].status == 0x83);
    CHECK(released[0].key == 50);

    // No output in its slot: nothing is sent, nothing breaks.
    live.outputs.at(2).store(nullptr);
    static_cast<void>(
        runInstrument(*preparedInstrument("midiout"), {noteOn(0, 1, 60)}, 64, params));
    REQUIRE(drain(*output).empty());
}
