// THE CHECKPOINT GATE (phase_3.md §2, FINAL_PLAN §7 Phase 3).
//
// Two time sources, at different positions, different tempos and different loop
// regions, driven through one Scheduler in the same blocks. Each channel produces its
// own hand-computed onsets, and neither affects the other. If this passes, Phase 11's
// clip launching is an array element, not a transport rewrite.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

#include "engine/graph/nodes/SumNode.h"
#include "engine/graph/nodes/TestToneNode.h"
#include "tests/cpp/graph/GraphFixtures.h"

using adx::core::kPpq;
using adx::core::Ticks;
using adx::graph::kPortMain;
using adx::graph::kPortPost;
using adx::project::EventKind;
using adx::project::ScheduledEvent;

namespace {

std::vector<ScheduledEvent> notesAt(const std::vector<std::int64_t>& ticks) {
    std::vector<ScheduledEvent> events;
    std::uint32_t id = 1;
    for (const std::int64_t tick : ticks) {
        const std::int64_t off = tick + (kPpq / 16);
        events.push_back(ScheduledEvent{.tick = tick,
                                        .endTick = off,
                                        .noteId = id,
                                        .instance = 1,
                                        .kind = EventKind::NoteOn,
                                        .pitch = 60,
                                        .velocity = 100});
        events.push_back(ScheduledEvent{.tick = off,
                                        .endTick = off,
                                        .noteId = id,
                                        .instance = 1,
                                        .kind = EventKind::NoteOff,
                                        .pitch = 60,
                                        .velocity = 0});
        ++id;
    }
    std::ranges::sort(events, [](const ScheduledEvent& a, const ScheduledEvent& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.kind < b.kind;
    });
    return events;
}

std::int64_t samplesAt(std::int64_t tick, double bpm) {
    return std::llround((static_cast<double>(tick) * (60.0 / (bpm * kPpq))) * 48000.0);
}

/// Every frame at which a channel goes from silence to sound.
std::vector<std::size_t> onsets(const std::vector<float>& out, std::size_t channel) {
    std::vector<std::size_t> found;
    bool sounding = false;
    std::size_t silentRun = 0;
    for (std::size_t frame = 0; frame < out.size() / 2; ++frame) {
        const bool now = out[(frame * 2) + channel] != 0.0F;
        if (now && !sounding && silentRun > 100) {
            found.push_back(frame);
        }
        if (now) {
            sounding = true;
            silentRun = 0;
        } else {
            ++silentRun;
            if (silentRun > 100) {
                sounding = false;
            }
        }
    }
    return found;
}

struct TwoSourceRig {
    adx::tests::ManualRender rig;
    std::vector<adx::core::TempoEvent> tempo90{
        adx::core::TempoEvent{.at = Ticks{0}, .bpm = 90.0, .ramp = false}};
    std::vector<double> cum90{0.0};
    adx::transport::TimeSourceId second;

    /// Channel A on source 0 (the arrangement, 120 bpm), panned left; channel B on a
    /// second source at 90 bpm, panned right. Both into one sum.
    TwoSourceRig(const std::vector<std::int64_t>& aTicks, const std::vector<std::int64_t>& bTicks,
                 bool bRolls) {
        adx::graph::Graph graph;
        const auto a = graph.add(
            std::make_shared<adx::graph::TestToneNode>(1, 16, adx::project::VoiceStealMode::Oldest),
            "A");
        const auto b = graph.add(
            std::make_shared<adx::graph::TestToneNode>(2, 16, adx::project::VoiceStealMode::Oldest),
            "B");
        const auto sum = graph.add(std::make_shared<adx::graph::SumNode>(), "sum");
        graph.connect(a, kPortPost, sum, kPortMain);
        graph.connect(b, kPortPost, sum, kPortMain);
        graph.setOutput(sum);
        for (auto& node : graph.nodes()) {
            node.node->prepare(adx::graph::PrepareInfo{});
        }

        rig.events = {notesAt(aTicks), notesAt(bTicks)};
        rig.params = {1.0F, -1.0F, 1.0F, 0.0F, 1.0F, 1.0F, 1.0F, 0.0F};
        auto& nodes = graph.nodes();
        nodes[a.v].eventTrack = 0;
        nodes[a.v].timeSource = 0;
        nodes[a.v].paramBase = 0;
        nodes[a.v].paramCount = 4;
        nodes[b.v].eventTrack = 1;
        nodes[b.v].paramBase = 4;
        nodes[b.v].paramCount = 4;

        second = rig.transport.acquire();
        REQUIRE(second.v == 1);
        nodes[b.v].timeSource = second.v;
        rig.compile(graph);

        adx::core::computeCumulativeSeconds(tempo90, cum90);
        auto& b90 = rig.transport.get(second);
        b90.bindTempo(adx::core::TempoView{.events = tempo90, .cumSeconds = cum90}, 48000);
        b90.requestSeek(Ticks{kPpq * 2}); // a different position
        if (bRolls) {
            b90.requestState(adx::transport::PlayState::Playing);
        }
        rig.transport.arrangement().requestState(adx::transport::PlayState::Playing);
    }
};

} // namespace

TEST_CASE("two_time_sources_independent", "[transport][checkpoint]") {
    const std::vector<std::int64_t> aTicks{1000, 5000};
    const std::vector<std::int64_t> bTicks{9000, 12000, 20000};
    const std::int64_t bStart = samplesAt(kPpq * 2, 90.0);

    // Hand-computed. A: 120 bpm from tick 0. B: 90 bpm, starting at tick 7680.
    const std::vector<std::size_t> expectA{static_cast<std::size_t>(samplesAt(1000, 120.0)),
                                           static_cast<std::size_t>(samplesAt(5000, 120.0))};
    std::vector<std::size_t> expectB;
    expectB.reserve(bTicks.size());
    for (const std::int64_t tick : bTicks) {
        expectB.push_back(static_cast<std::size_t>(samplesAt(tick, 90.0) - bStart));
    }

    SECTION("neither loops") {
        TwoSourceRig two{aTicks, bTicks, true};
        const std::vector<float> out = two.rig.render(150000, 512);
        CHECK(onsets(out, 0) == expectA);
        CHECK(onsets(out, 1) == expectB);

        // Neither affects the other: A rendered with B's source stopped is identical.
        TwoSourceRig alone{aTicks, bTicks, false};
        const std::vector<float> aOnly = alone.rig.render(150000, 512);
        for (std::size_t frame = 0; frame < 150000; ++frame) {
            REQUIRE(aOnly[frame * 2] == out[frame * 2]);
            REQUIRE(aOnly[(frame * 2) + 1] == 0.0F);
        }
    }

    SECTION("one loops and the other does not") {
        TwoSourceRig two{aTicks, bTicks, true};
        // A loops its first two beats (48000 samples at 120 bpm); B plays straight on.
        two.rig.transport.arrangement().requestLoop(
            adx::transport::LoopRegion{.start = Ticks{0}, .end = Ticks{kPpq * 2}, .enabled = true});
        const std::vector<float> out = two.rig.render(150000, 512);

        std::vector<std::size_t> loopedA;
        for (std::size_t pass = 0; pass < 4; ++pass) {
            for (const std::size_t onset : expectA) {
                if (onset + (pass * 48000) < 150000) {
                    loopedA.push_back(onset + (pass * 48000));
                }
            }
        }
        CHECK(onsets(out, 0) == loopedA);
        CHECK(onsets(out, 1) == expectB);
        // And the arrangement's loop did not become the other source's loop.
        CHECK_FALSE(two.rig.transport.get(two.second).loop().active());
    }
}
