// The clip state machine, launch quantize and follow actions (phase_11.md §4.2, §4.3,
// §4.1): state_machine_transitions, launch_quantize_exact's grid half, follow_actions,
// follow_random_seeded.
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <set>

#include "engine/core/Time.h"
#include "engine/session/ClipState.h"
#include "engine/session/FollowAction.h"
#include "engine/session/LaunchQuantize.h"
#include "engine/session/TempoNudge.h"

using adx::core::kPpq;
using namespace adx::session;

TEST_CASE("state_machine_transitions", "[session][state]") {
    using S = ClipState;
    using T = ClipTrigger;
    struct Row {
        S from;
        T trigger;
        bool legato;
        S next;
        bool start;
        bool stop;
        bool follow;
    };
    // Every state x every trigger; Retriggered x Boundary both ways for legato.
    const auto row = [](S from, T trigger, bool legato, S next, bool start, bool stop,
                        bool follow) {
        return Row{.from = from,
                   .trigger = trigger,
                   .legato = legato,
                   .next = next,
                   .start = start,
                   .stop = stop,
                   .follow = follow};
    };
    const std::array<Row, 21> table{
        row(S::Stopped, T::Launch, false, S::Queued, false, false, false),
        row(S::Stopped, T::Stop, false, S::Stopped, false, false, false),
        row(S::Stopped, T::Boundary, false, S::Stopped, false, false, false),
        row(S::Stopped, T::End, false, S::Stopped, false, false, false),
        row(S::Queued, T::Launch, false, S::Queued, false, false, false),
        row(S::Queued, T::Stop, false, S::Stopped, false, false, false),
        row(S::Queued, T::Boundary, false, S::Playing, true, false, false),
        row(S::Queued, T::End, false, S::Queued, false, false, false),
        row(S::Playing, T::Launch, false, S::Retriggered, false, false, false),
        row(S::Playing, T::Stop, false, S::QueuedStop, false, false, false),
        row(S::Playing, T::Boundary, false, S::Playing, false, false, false),
        row(S::Playing, T::End, false, S::Stopped, false, true, true),
        row(S::QueuedStop, T::Launch, false, S::Retriggered, false, false, false),
        row(S::QueuedStop, T::Stop, false, S::QueuedStop, false, false, false),
        row(S::QueuedStop, T::Boundary, false, S::Stopped, false, true, false),
        row(S::QueuedStop, T::End, false, S::Stopped, false, true, false),
        row(S::Retriggered, T::Launch, false, S::Retriggered, false, false, false),
        row(S::Retriggered, T::Stop, false, S::QueuedStop, false, false, false),
        row(S::Retriggered, T::Boundary, false, S::Playing, true, false, false),
        row(S::Retriggered, T::Boundary, true, S::Playing, false, false, false),
        row(S::Retriggered, T::End, false, S::Retriggered, false, false, false),
    };
    for (const Row& entry : table) {
        INFO(toString(entry.from) << " + trigger " << static_cast<int>(entry.trigger)
                                  << (entry.legato ? " (legato)" : ""));
        const ClipStep got = step(entry.from, entry.trigger, entry.legato);
        CHECK(got.next == entry.next);
        CHECK(got.start == entry.start);
        CHECK(got.stop == entry.stop);
        CHECK(got.follow == entry.follow);
    }
    CHECK(pending(S::Queued));
    CHECK(pending(S::QueuedStop));
    CHECK(pending(S::Retriggered));
    CHECK_FALSE(pending(S::Playing));
    CHECK(sounding(S::QueuedStop));
    CHECK_FALSE(sounding(S::Queued));
}

TEST_CASE("launch_quantize_grid", "[session][quantize]") {
    const std::int64_t bar = kPpq * 4;
    CHECK(nextBoundary(1, LaunchQuantize::Bar, bar) == bar);
    CHECK(nextBoundary(bar, LaunchQuantize::Bar, bar) == bar); // at a boundary: that one
    CHECK(nextBoundary(bar + 1, LaunchQuantize::TwoBars, bar) == bar * 2);
    CHECK(nextBoundary((bar * 2) + 1, LaunchQuantize::TwoBars, bar) == bar * 4);
    CHECK(nextBoundary(5, LaunchQuantize::Quarter, bar) == kPpq);
    CHECK(nextBoundary(kPpq + 1, LaunchQuantize::Half, bar) == kPpq * 2);
    CHECK(nextBoundary((bar * 3) + 7, LaunchQuantize::FourBars, bar) == bar * 4);
    CHECK(nextBoundary(bar * 9, LaunchQuantize::EightBars, bar) == bar * 16);
    CHECK(nextBoundary(1234, LaunchQuantize::Instant, bar) == 1234);
    // A grid from a meter change at tick 1000, in 7/8 (a bar of 3.5 beats).
    const std::int64_t seven = kPpq * 7 / 2;
    CHECK(nextBoundary(1001, LaunchQuantize::Bar, seven, 1000) == 1000 + seven);
    CHECK(nextBoundary(-5, LaunchQuantize::Bar, bar) == 0);
    // Names round-trip.
    for (int q = 0; q <= static_cast<int>(LaunchQuantize::EightBars); ++q) {
        LaunchQuantize parsed{};
        REQUIRE(parseQuantize(toString(static_cast<LaunchQuantize>(q)), parsed));
        CHECK(parsed == static_cast<LaunchQuantize>(q));
    }
    LaunchQuantize ignored{};
    CHECK_FALSE(parseQuantize("3bar", ignored));
}

TEST_CASE("follow_actions", "[session][follow]") {
    using K = FollowResult::Kind;
    CHECK(follow(FollowAction::None, 2, 5, 1, 1, 0).kind == K::Continue);
    CHECK(follow(FollowAction::Stop, 2, 5, 1, 1, 0).kind == K::Stop);
    CHECK(follow(FollowAction::Next, 2, 5, 1, 1, 0).row == 3);
    CHECK(follow(FollowAction::Next, 4, 5, 1, 1, 0).row == 0); // wraps
    CHECK(follow(FollowAction::Previous, 0, 5, 1, 1, 0).row == 4);
    CHECK(follow(FollowAction::Previous, 3, 5, 1, 1, 0).row == 2);
    CHECK(follow(FollowAction::First, 3, 5, 1, 1, 0).row == 0);
    // RepeatN: three passes, then stop.
    CHECK(follow(FollowAction::RepeatN, 1, 5, 1, 3, 0).kind == K::Launch);
    CHECK(follow(FollowAction::RepeatN, 1, 5, 1, 3, 0).row == 1);
    CHECK(follow(FollowAction::RepeatN, 1, 5, 2, 3, 0).kind == K::Launch);
    CHECK(follow(FollowAction::RepeatN, 1, 5, 3, 3, 0).kind == K::Stop);
    CHECK(follow(FollowAction::Next, 0, 0, 1, 1, 0).kind == K::Stop); // no rows at all
    for (int a = 0; a <= static_cast<int>(FollowAction::RepeatN); ++a) {
        FollowAction parsed{};
        REQUIRE(parseFollow(toString(static_cast<FollowAction>(a)), parsed));
        CHECK(parsed == static_cast<FollowAction>(a));
    }
}

TEST_CASE("follow_random_seeded", "[session][follow]") {
    // The same seed, row and pass always pick the same row; it is never the clip's own;
    // over many passes every other row comes up.
    std::set<std::uint32_t> seen;
    for (std::uint32_t pass = 1; pass <= 200; ++pass) {
        const FollowResult a = follow(FollowAction::Random, 2, 6, pass, 1, 0xC0FFEE);
        const FollowResult b = follow(FollowAction::Random, 2, 6, pass, 1, 0xC0FFEE);
        REQUIRE(a.kind == FollowResult::Kind::Launch);
        CHECK(a.row == b.row);
        CHECK(a.row != 2);
        CHECK(a.row < 6);
        seen.insert(a.row);
    }
    CHECK(seen == std::set<std::uint32_t>{0, 1, 3, 4, 5});
    // Another seed is another sequence.
    std::uint32_t differ = 0;
    for (std::uint32_t pass = 1; pass <= 50; ++pass) {
        differ += follow(FollowAction::Random, 2, 6, pass, 1, 1).row !=
                          follow(FollowAction::Random, 2, 6, pass, 1, 2).row
                      ? 1U
                      : 0U;
    }
    CHECK(differ > 10);
    CHECK(follow(FollowAction::Random, 0, 1, 1, 1, 7).row == 0); // a lone row
}

TEST_CASE("tap_tempo_outliers", "[session][tempo]") {
    // Taps at 128 bpm; one of them 60 ms late. The tempo stays within 2 %.
    adx::session::TapTempo tap;
    const double beat = 60.0 / 128.0;
    double bpm = 0.0;
    for (int i = 0; i < 5; ++i) {
        const double late = i == 3 ? 0.06 : 0.0;
        bpm = tap.tap(10.0 + (i * beat) + late);
    }
    INFO("tapped " << bpm);
    CHECK(std::abs(bpm - 128.0) / 128.0 < 0.02);
    // Two taps already say a tempo; a long pause starts a new count.
    adx::session::TapTempo fresh;
    CHECK(fresh.tap(0.0) == 0.0);
    CHECK(std::abs(fresh.tap(0.5) - 120.0) < 1e-9);
    CHECK(fresh.tap(10.0) == 0.0);
}

TEST_CASE("tempo_nudge_rate", "[session][tempo]") {
    // A nudge is computed from the base each time: after a hundred, none is left over.
    double rate = 1.0;
    for (int i = 0; i < 100; ++i) {
        rate = adx::session::nudgedRate(1.0, i % 2 == 0 ? 1 : -1);
        CHECK(rate != 1.0);
    }
    rate = adx::session::nudgedRate(1.0, 0);
    CHECK(rate == 1.0);
    CHECK(adx::session::nudgedRate(1.25, 1) == 1.25 * 1.04);
    CHECK(adx::session::nudgedRate(1.0, -1, 0.1) == 0.9);
}
