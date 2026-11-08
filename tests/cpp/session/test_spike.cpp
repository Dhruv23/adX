// Phase 11's week-one spike (phase_11.md §3), kept as a permanent test.
//
// Two time sources at different positions, rates and loops drive one pattern each
// through the existing Scheduler, rendered 30 seconds offline. Each pattern's onsets land
// where they are computed by hand. Then the case the spike found the checkpoint did not
// cover by itself: one channel playing the arrangement *and* a launched clip at once.
// A graph node is bound to one source, so that needs the scheduler's session lanes.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "engine/graph/Scheduler.h"
#include "engine/graph/nodes/SumNode.h"
#include "engine/graph/nodes/TestToneNode.h"
#include "tests/cpp/graph/GraphFixtures.h"

using adx::core::kPpq;
using adx::core::Ticks;
using adx::project::EventKind;
using adx::project::ScheduledEvent;

namespace {

constexpr double kRate = 48000.0;

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

/// Samples at 120 bpm (a source's default tempo).
std::int64_t samples120(std::int64_t tick) {
    return std::llround(static_cast<double>(tick) * (60.0 / (120.0 * kPpq)) * kRate);
}

std::vector<std::size_t> onsets(const std::vector<float>& out, std::size_t channel) {
    std::vector<std::size_t> found;
    bool sounding = false;
    std::size_t silentRun = 1000;
    for (std::size_t frame = 0; frame < out.size() / 2; ++frame) {
        const bool now = out[(frame * 2) + channel] != 0.0F;
        if (now && !sounding && silentRun > 100) {
            found.push_back(frame);
        }
        if (now) {
            sounding = true;
            silentRun = 0;
        } else if (++silentRun > 100) {
            sounding = false;
        }
    }
    return found;
}

/// Where a channel sounds, as [start, end) runs - for diagnosing an onset mismatch.
std::string runs(const std::vector<float>& out, std::size_t channel) {
    std::string text;
    std::size_t start = 0;
    std::size_t last = 0;
    bool on = false;
    for (std::size_t f = 0; f < out.size() / 2; ++f) {
        const bool now = out[(f * 2) + channel] != 0.0F;
        if (now && !on) {
            if (f - last > 100 || last == 0) {
                if (start != 0 || last != 0) {
                    text += "[" + std::to_string(start) + "," + std::to_string(last) + ") ";
                }
                start = f;
            }
        }
        if (now) {
            last = f + 1;
        }
        on = now;
    }
    text += "[" + std::to_string(start) + "," + std::to_string(last) + ")";
    return text;
}

bool near(const std::vector<std::size_t>& got, const std::vector<std::size_t>& want,
          std::size_t slack) {
    if (got.size() != want.size()) {
        return false;
    }
    for (std::size_t i = 0; i < got.size(); ++i) {
        const auto d = static_cast<std::int64_t>(got[i]) - static_cast<std::int64_t>(want[i]);
        if (std::abs(d) > static_cast<std::int64_t>(slack)) {
            return false;
        }
    }
    return true;
}

/// Two testtone channels (ids 1 and 2, panned left and right) into one sum.
struct Rig {
    adx::tests::ManualRender rig;
    std::vector<std::uint32_t> channels{1, 2};
    adx::graph::NodeId a{};
    adx::graph::NodeId b{};

    Rig(const std::vector<std::int64_t>& aTicks, const std::vector<std::int64_t>& bTicks,
        std::uint32_t bSource) {
        adx::graph::Graph graph;
        a = graph.add(
            std::make_shared<adx::graph::TestToneNode>(1, 16, adx::project::VoiceStealMode::Oldest),
            "A");
        b = graph.add(
            std::make_shared<adx::graph::TestToneNode>(2, 16, adx::project::VoiceStealMode::Oldest),
            "B");
        const auto sum = graph.add(std::make_shared<adx::graph::SumNode>(), "sum");
        graph.connect(a, adx::graph::kPortPost, sum, adx::graph::kPortMain);
        graph.connect(b, adx::graph::kPortPost, sum, adx::graph::kPortMain);
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
        nodes[b.v].timeSource = bSource;
        nodes[b.v].paramBase = 4;
        nodes[b.v].paramCount = 4;
        rig.compile(graph);
        rig.snapshot.trackChannels = channels;
    }
};

} // namespace

TEST_CASE("spike_two_sources_render", "[session][checkpoint]") {
    SECTION("two sources: own position, own rate, own loop, 30 seconds") {
        // A: the arrangement, 120 bpm, from 0, no loop. B: a second source, started at
        // beat 2, looping beats 2-4, at 1.5 times speed.
        const std::vector<std::int64_t> aTicks{kPpq / 2, kPpq * 7, kPpq * 40, kPpq * 55};
        const std::vector<std::int64_t> bTicks{(kPpq * 2) + (kPpq / 4), (kPpq * 3) + (kPpq / 2)};
        Rig two{aTicks, bTicks, 1};
        const adx::transport::TimeSourceId second = two.rig.transport.acquire();
        REQUIRE(second.v == 1);
        adx::transport::TimeSource& b = two.rig.transport.get(second);
        b.requestSeek(Ticks{kPpq * 2});
        b.requestLoop(adx::transport::LoopRegion{
            .start = Ticks{kPpq * 2}, .end = Ticks{kPpq * 4}, .enabled = true});
        b.requestRate(1.5);
        b.requestState(adx::transport::PlayState::Playing);
        two.rig.transport.arrangement().requestState(adx::transport::PlayState::Playing);

        constexpr std::uint64_t kFrames = std::uint64_t{30} * 48000;
        const std::vector<float> out = two.rig.render(kFrames, 512);

        std::vector<std::size_t> wantA;
        wantA.reserve(aTicks.size());
        for (const std::int64_t tick : aTicks) {
            wantA.push_back(static_cast<std::size_t>(samples120(tick)));
        }
        // B: loop start 48000, loop length 48000 samples of its own timeline, covered 1.5
        // samples per frame. Pass n reaches event e at frame (n * 48000 + e - 48000) / 1.5.
        std::vector<std::size_t> wantB;
        for (std::int64_t pass = 0;; ++pass) {
            bool any = false;
            for (const std::int64_t tick : bTicks) {
                const auto along = static_cast<double>((pass * 48000) + samples120(tick) - 48000);
                const auto frame = static_cast<std::size_t>(std::ceil(along / 1.5));
                if (frame < kFrames) {
                    wantB.push_back(frame);
                    any = true;
                }
            }
            if (!any) {
                break;
            }
        }
        const std::vector<std::size_t> gotA = onsets(out, 0);
        const std::vector<std::size_t> gotB = onsets(out, 1);
        INFO("A " << gotA.size() << " onsets, B " << gotB.size() << " (want " << wantB.size()
                  << ")");
        CHECK(gotA == wantA);
        CHECK(near(gotB, wantB, 1));
        CHECK(wantB.size() == 90); // 45 passes of the loop, two notes each
    }

    SECTION("one channel: the arrangement and a launched clip at once") {
        // Channel A plays its arrangement notes on source 0 and, through a session lane,
        // a clip's notes on source 1 started at a mid-block frame. Both reach the one
        // channel at their own positions; a stop inside a block drops the clip's later
        // notes and leaves the arrangement's.
        const std::vector<std::int64_t> aTicks{kPpq, kPpq * 5};
        const std::vector<std::int64_t> clipTicks{0, kPpq * 2, kPpq * 4};
        Rig one{aTicks, {}, 0};
        // The track's events are a span: the vector behind them lives as long as it.
        const std::vector<ScheduledEvent> clipEvents = notesAt(clipTicks);
        const adx::project::EventTrack clip{.events = clipEvents, .lyrics = {}};
        adx::project::EventCursor cursor{};
        const adx::transport::TimeSourceId source = one.rig.transport.acquire();
        adx::transport::TimeSource& clipTime = one.rig.transport.get(source);
        one.rig.transport.arrangement().requestState(adx::transport::PlayState::Playing);
        std::vector<float> out = one.rig.render(12800, 512); // the clip not launched yet
        // Launched 300 frames into the block at 12800: its tick 0 lands on frame 13100.
        constexpr std::int64_t kLaunch = 13100;
        clipTime.requestSeekSamples(-300);
        clipTime.requestState(adx::transport::PlayState::Playing);
        std::array<adx::graph::SessionLane, 1> lanes{
            adx::graph::SessionLane{.channel = 1,
                                    .source = source.v,
                                    .track = &clip,
                                    .cursor = &cursor,
                                    .stopAt = adx::graph::SessionLane::kNoStop}};
        one.rig.scheduler.setSessionLanes(lanes);
        const std::vector<float> rest = one.rig.render(150000 - 12800, 512);
        out.insert(out.end(), rest.begin(), rest.end());

        std::vector<std::size_t> want{static_cast<std::size_t>(samples120(aTicks[0]))};
        for (const std::int64_t tick : clipTicks) {
            want.push_back(static_cast<std::size_t>(kLaunch + samples120(tick)));
        }
        want.push_back(static_cast<std::size_t>(samples120(aTicks[1])));
        std::ranges::sort(want);
        const std::vector<std::size_t> got = onsets(out, 0);
        INFO("got " << got.size() << " onsets on the one channel: " << runs(out, 0));
        CHECK(got == want);
        CHECK(one.rig.scheduler.stats().laneEvents == clipTicks.size() * 2);

        // Stop the clip 100 frames into a block, before its third note: that note never
        // plays; the arrangement's note after it still does.
        Rig again{aTicks, {}, 0};
        adx::project::EventCursor cursor2{};
        const adx::transport::TimeSourceId source2 = again.rig.transport.acquire();
        again.rig.transport.get(source2).requestState(adx::transport::PlayState::Playing);
        again.rig.transport.arrangement().requestState(adx::transport::PlayState::Playing);
        std::array<adx::graph::SessionLane, 1> live{
            adx::graph::SessionLane{.channel = 1,
                                    .source = source2.v,
                                    .track = &clip,
                                    .cursor = &cursor2,
                                    .stopAt = adx::graph::SessionLane::kNoStop}};
        again.rig.scheduler.setSessionLanes(live);
        // Note 3 is at frame 96000: the block from 95744 holds it, and a stop at offset 100
        // (frame 95844) comes before it.
        std::vector<float> first = again.rig.render(95744, 512);
        live[0].stopAt = 100;
        const std::vector<float> stopBlock = again.rig.render(512, 512);
        again.rig.transport.get(source2).requestState(adx::transport::PlayState::Stopped);
        live[0].stopAt = adx::graph::SessionLane::kNoStop;
        const std::vector<float> tail = again.rig.render(150000 - 96256, 512);
        first.insert(first.end(), stopBlock.begin(), stopBlock.end());
        first.insert(first.end(), tail.begin(), tail.end());
        const std::vector<std::size_t> stopped = onsets(first, 0);
        std::vector<std::size_t> expected{0, static_cast<std::size_t>(samples120(aTicks[0])),
                                          static_cast<std::size_t>(samples120(kPpq * 2)),
                                          static_cast<std::size_t>(samples120(aTicks[1]))};
        CHECK(stopped == expected);
    }
}
