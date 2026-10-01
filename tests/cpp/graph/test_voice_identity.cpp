// Voices: exact identity, per-channel pools, a defined steal order, and no clicks.

#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "engine/graph/VoicePool.h"
#include "engine/graph/nodes/TestToneNode.h"
#include "tests/cpp/graph/GraphFixtures.h"

using adx::graph::BlockEvent;
using adx::graph::BlockEventKind;
using adx::graph::TestToneNode;
using adx::graph::Voice;
using adx::graph::VoiceKey;
using adx::graph::VoicePhase;
using adx::graph::VoicePool;
using adx::project::VoiceStealMode;

namespace {

BlockEvent noteOn(std::uint32_t offset, std::uint32_t noteId, std::uint8_t pitch) {
    return BlockEvent{.offset = offset,
                      .noteId = noteId,
                      .instance = 1,
                      .timeSource = 0,
                      .endTick = 1'000'000,
                      .kind = BlockEventKind::NoteOn,
                      .pitch = pitch,
                      .velocity = 100};
}

BlockEvent noteOff(std::uint32_t offset, std::uint32_t noteId, std::uint8_t pitch) {
    BlockEvent event = noteOn(offset, noteId, pitch);
    event.kind = BlockEventKind::NoteOff;
    return event;
}

const Voice* voiceFor(VoicePool& pool, std::uint32_t noteId) {
    for (const Voice& voice : pool.voices()) {
        if (voice.phase != VoicePhase::Free && voice.key.noteId == noteId) {
            return &voice;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("voice_identity_cross_channel", "[graph][voices]") {
    // Iteration one's §3.3.4 defect, reproduced as a test: two channels play MIDI 60,
    // and a note-off on channel A must release A's voice and nothing else. The archived
    // engine matched note-offs by pitch and released both.
    TestToneNode a{1, 16, VoiceStealMode::OldestReleased};
    TestToneNode b{2, 16, VoiceStealMode::OldestReleased};
    a.prepare(adx::graph::PrepareInfo{});
    b.prepare(adx::graph::PrepareInfo{});

    // Same pitch, and even the same note id: identity is (channel, note, placement).
    static_cast<void>(
        adx::tests::runChannelNode(a, {noteOn(0, 7, 60), noteOff(1000, 7, 60)}, 2048));
    static_cast<void>(adx::tests::runChannelNode(b, {noteOn(0, 7, 60)}, 2048));

    const Voice* inA = voiceFor(a.pool(), 7);
    const Voice* inB = voiceFor(b.pool(), 7);
    REQUIRE(inA != nullptr);
    REQUIRE(inB != nullptr);
    CHECK(inA->phase == VoicePhase::Released);
    CHECK(inB->phase == VoicePhase::Active);

    // And within one channel: two notes on the same pitch are two voices, and a
    // note-off releases the one it names.
    TestToneNode c{3, 16, VoiceStealMode::OldestReleased};
    c.prepare(adx::graph::PrepareInfo{});
    static_cast<void>(adx::tests::runChannelNode(
        c, {noteOn(0, 10, 60), noteOn(10, 11, 60), noteOff(500, 10, 60)}, 1024));
    CHECK(voiceFor(c.pool(), 10)->phase == VoicePhase::Released);
    CHECK(voiceFor(c.pool(), 11)->phase == VoicePhase::Active);
}

TEST_CASE("voice_pool_per_channel_isolation", "[graph][voices]") {
    // A 64-voice pad at full polyphony, and a kick. The pad owns its pool; the kick
    // owns its own. Iteration one had one 64-voice pool for everything, and a busy pad
    // stole the kick (FINAL_PLAN §3.3.6).
    TestToneNode pad{1, 64, VoiceStealMode::OldestReleased};
    TestToneNode kick{2, 1, VoiceStealMode::OldestReleased};
    pad.prepare(adx::graph::PrepareInfo{});
    kick.prepare(adx::graph::PrepareInfo{});

    std::vector<BlockEvent> chord;
    chord.reserve(64);
    for (std::uint32_t n = 0; n < 64; ++n) {
        chord.push_back(noteOn(0, 100 + n, static_cast<std::uint8_t>(40 + n)));
    }
    static_cast<void>(adx::tests::runChannelNode(pad, chord, 256));
    REQUIRE(pad.pool().soundingCount() == 64);

    static_cast<void>(adx::tests::runChannelNode(kick, {noteOn(0, 1, 36)}, 256));
    CHECK(kick.pool().soundingCount() == 1);
    CHECK(voiceFor(kick.pool(), 1)->phase == VoicePhase::Active);
    // Nothing the kick did touched the pad.
    CHECK(pad.pool().soundingCount() == 64);
    for (std::uint32_t n = 0; n < 64; ++n) {
        CHECK(voiceFor(pad.pool(), 100 + n)->phase == VoicePhase::Active);
    }
}

TEST_CASE("voice_steal_order", "[graph][voices]") {
    // free -> oldest released -> quietest -> oldest, one row per rule.
    std::vector<Voice> storage(VoicePool::storageFor(3));
    const auto fresh = [&storage] { return VoicePool{3, storage, 96}; };
    const auto key = [](std::uint32_t id) {
        return VoiceKey{.channelId = 1, .noteId = id, .instance = 1};
    };
    const auto start = [&](VoicePool& pool, std::uint32_t id, float level) {
        Voice* voice = pool.allocate(key(id), VoiceStealMode::OldestReleased);
        REQUIRE(voice != nullptr);
        voice->level = level;
        voice->startedAt = pool.clock();
        pool.advanceClock(10);
        return voice;
    };

    SECTION("a free voice is used before anything is stolen") {
        VoicePool pool = fresh();
        start(pool, 1, 1.0F);
        start(pool, 2, 1.0F);
        start(pool, 3, 1.0F);
        CHECK(pool.soundingCount() == 3);
        for (std::uint32_t id = 1; id <= 3; ++id) {
            CHECK(voiceFor(pool, id)->phase == VoicePhase::Active);
        }
    }
    SECTION("then the oldest released voice, even when a louder one is released too") {
        VoicePool pool = fresh();
        Voice* oldest = start(pool, 1, 0.9F);
        start(pool, 2, 0.1F);
        Voice* younger = start(pool, 3, 0.9F);
        VoicePool::release(*oldest);
        VoicePool::release(*younger);
        start(pool, 4, 1.0F);
        CHECK(voiceFor(pool, 1)->phase == VoicePhase::Stolen);
        CHECK(voiceFor(pool, 3)->phase == VoicePhase::Released);
        CHECK(voiceFor(pool, 2)->phase == VoicePhase::Active);
    }
    SECTION("with nothing released, the quietest") {
        VoicePool pool = fresh();
        start(pool, 1, 0.8F);
        start(pool, 2, 0.2F);
        start(pool, 3, 0.5F);
        start(pool, 4, 1.0F);
        CHECK(voiceFor(pool, 2)->phase == VoicePhase::Stolen);
        CHECK(voiceFor(pool, 1)->phase == VoicePhase::Active);
        CHECK(voiceFor(pool, 3)->phase == VoicePhase::Active);
    }
    SECTION("and among equally quiet voices, the oldest") {
        VoicePool pool = fresh();
        start(pool, 1, 0.5F);
        start(pool, 2, 0.5F);
        start(pool, 3, 0.5F);
        start(pool, 4, 1.0F);
        CHECK(voiceFor(pool, 1)->phase == VoicePhase::Stolen);
        CHECK(voiceFor(pool, 2)->phase == VoicePhase::Active);
    }
    SECTION("VoiceStealMode::None drops the new note instead") {
        VoicePool pool = fresh();
        start(pool, 1, 0.5F);
        start(pool, 2, 0.5F);
        start(pool, 3, 0.5F);
        CHECK(pool.allocate(key(4), VoiceStealMode::None) == nullptr);
        CHECK(pool.soundingCount() == 3);
    }
}

TEST_CASE("voice_steal_ramps", "[graph][voices]") {
    // Polyphony 1, a low sustained note, then a second note that must steal it. The
    // stolen voice fades over 2 ms instead of stopping dead; the output never jumps.
    // A hard cut of a note at this level would jump by up to ~0.16.
    TestToneNode node{1, 1, VoiceStealMode::OldestReleased};
    node.prepare(adx::graph::PrepareInfo{});
    const std::vector<float> out =
        adx::tests::runChannelNode(node, {noteOn(0, 1, 36), noteOn(4801, 2, 38)}, 9600);

    float largest = 0.0F;
    for (std::size_t frame = 1; frame < 9600; ++frame) {
        largest = std::max(largest, std::abs(out[frame * 2] - out[(frame - 1) * 2]));
    }
    INFO("largest sample-to-sample step: " << largest);
    CHECK(largest < 0.05F);
    // The steal happened: one voice sounding, and it is the new note.
    CHECK(node.pool().soundingCount() == 1);
    CHECK(voiceFor(node.pool(), 2) != nullptr);
    CHECK(voiceFor(node.pool(), 1) == nullptr); // faded out and freed
}
