// SysEx transport (phase_11.5.md §4.4, §5): the assembler, the queue, and an input that
// keeps SysEx off the audio path.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "engine/midi/MidiInput.h"
#include "engine/midi/sysex/SysexAssembler.h"
#include "engine/midi/sysex/SysexQueue.h"
#include "engine/rt/AllocGuard.h"
#include "engine/rt/RtSection.h"
#include "engine/rt/Violation.h"

using adx::midi::AudioClock;
using adx::midi::BlockMidi;
using adx::midi::MidiInput;
using adx::midi::SysexAssembler;
using adx::midi::SysexMessage;
using adx::midi::SysexQueue;

using Messages = std::vector<std::vector<std::uint8_t>>;

namespace {

std::vector<std::uint8_t> sysex(std::size_t payload, std::uint8_t seed = 1) {
    std::vector<std::uint8_t> out{0xF0, 0x43, 0x00};
    for (std::size_t i = 0; i < payload; ++i) {
        out.push_back(static_cast<std::uint8_t>((seed + (i * 7)) & 0x7F));
    }
    out.push_back(0xF7);
    return out;
}

std::vector<std::vector<std::uint8_t>> drain(SysexQueue& queue) {
    std::vector<std::vector<std::uint8_t>> out;
    auto message = std::make_unique<SysexMessage>();
    while (queue.pop(*message)) {
        const auto view = message->view();
        out.emplace_back(view.begin(), view.end());
    }
    return out;
}

} // namespace

TEST_CASE("sysex_assembler", "[midi][sysex]") {
    auto queue = std::make_unique<SysexQueue>();
    SysexAssembler assembler(*queue, 1000);

    SECTION("a whole message passes through") {
        const auto bank = sysex(4096 + 2); // a DX7 bank's size
        REQUIRE(assembler.feed(0, bank) == bank.size());
        REQUIRE(drain(*queue) == Messages{bank});
        REQUIRE(assembler.stats().completed == 1);
    }
    SECTION("fragments are joined, realtime bytes inside are skipped") {
        const auto voice = sysex(155 + 2);
        const std::span<const std::uint8_t> all(voice);
        REQUIRE(assembler.feed(0, all.first(10)) == 10);
        const std::array<std::uint8_t, 1> clock{0xF8};
        REQUIRE(assembler.feed(1, clock) == 0); // a clock tick mid-message: the caller's
        REQUIRE(assembler.active());
        REQUIRE(assembler.feed(2, all.subspan(10, 50)) == 50);
        REQUIRE(assembler.feed(3, all.subspan(60)) == voice.size() - 60);
        REQUIRE(drain(*queue) == Messages{voice});
    }
    SECTION("over-size is dropped and counted") {
        const auto huge = sysex(SysexMessage::kMaxBytes + 10);
        assembler.feed(0, huge);
        REQUIRE(drain(*queue).empty());
        REQUIRE(assembler.stats().oversize == 1);
        // And the next one is fine.
        const auto small = sysex(4);
        assembler.feed(1, small);
        REQUIRE(drain(*queue) == Messages{small});
    }
    SECTION("a new status byte interrupts and discards") {
        const auto voice = sysex(100);
        assembler.feed(0, std::span<const std::uint8_t>(voice).first(20));
        const std::array<std::uint8_t, 3> note{0x90, 60, 100};
        REQUIRE(assembler.feed(1, note) == 0); // the caller plays the note
        REQUIRE_FALSE(assembler.active());
        REQUIRE(assembler.stats().interrupted == 1);
        // The tail that follows is not a message.
        assembler.feed(2, std::span<const std::uint8_t>(voice).subspan(20));
        REQUIRE(drain(*queue).empty());
        // F0 before F7 restarts.
        assembler.feed(3, std::span<const std::uint8_t>(voice).first(20));
        const auto next = sysex(5);
        assembler.feed(4, next);
        REQUIRE(assembler.stats().interrupted == 2);
        REQUIRE(drain(*queue) == Messages{next});
    }
    SECTION("a message left open times out") {
        const auto voice = sysex(40);
        assembler.feed(0, std::span<const std::uint8_t>(voice).first(10));
        assembler.poll(5000);
        REQUIRE_FALSE(assembler.active());
        REQUIRE(assembler.stats().timedOut == 1);
    }
}

TEST_CASE("sysex_queue_no_alloc", "[midi][sysex]") {
    auto queue = std::make_unique<SysexQueue>();
    SysexAssembler assembler(*queue);
    const auto bank = sysex(4098);
    const auto voice = sysex(157);
    auto& log = adx::rt::ViolationLog::instance();
    log.reset();
    {
        const adx::rt::ScopedRtSection section;
        for (int i = 0; i < 100; ++i) {
            const auto& message = (i % 3) == 0 ? bank : voice;
            const std::span<const std::uint8_t> all(message);
            assembler.feed(i, all.first(7));
            assembler.feed(i, all.subspan(7));
        }
    }
    if (adx::rt::allocGuardCompiledIn()) {
        REQUIRE(log.count(adx::rt::ViolationKind::Allocation) == 0);
    }
    // 16 slots: the rest were dropped and counted, not grown into.
    REQUIRE(drain(*queue).size() == SysexQueue::kSlots);
    REQUIRE(queue->droppedCount() == 100 - SysexQueue::kSlots);
    REQUIRE(queue->push(voice));
}

TEST_CASE("midi_input_keeps_sysex", "[midi][sysex]") {
    AudioClock clock;
    clock.publish(0, 0);
    MidiInput input(clock, 48000, 256, 0);
    auto queue = std::make_unique<SysexQueue>();
    SysexAssembler assembler(*queue);

    const auto dump = sysex(155);
    const std::array<std::uint8_t, 3> note{0x90, 60, 100};
    // Without an assembler SysEx is dropped, as before.
    input.onMessage(0, dump);
    input.onMessage(0, note);
    std::array<BlockMidi, 16> out{};
    REQUIRE(input.drain(0, 1 << 20, out) == 1);
    REQUIRE(drain(*queue).empty());

    input.attachSysex(&assembler);
    input.onMessage(1, dump);
    input.onMessage(1, note);
    input.onMessage(1, std::span<const std::uint8_t>(dump).first(30));
    input.onMessage(1, std::span<const std::uint8_t>(dump).subspan(30));
    // Only the note reaches the channel side; both dumps reach the queue whole.
    const std::size_t n = input.drain(0, 1 << 20, out);
    REQUIRE(n == 1);
    REQUIRE(out[0].event.status == 0x90);
    REQUIRE(drain(*queue) == Messages{dump, dump});
}
