// Phase 4 tranche A0: slides and pitch curves as glide events, automation as a
// per-frame function of position, clip envelopes, and the arpeggiator.
//
// phase_4.md §4.0's tests. The theme they share: none of this may depend on the block
// size, and none of it may depend on where playback started.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

#include "engine/core/Curve.h"
#include "engine/graph/nodes/ChannelNode.h"
#include "engine/project/CurveCompile.h"
#include "engine/project/EventCompile.h"
#include "engine/render/RenderHash.h"
#include "tests/cpp/graph/GraphFixtures.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::core::kPpq;
using adx::graph::BlockEvent;
using adx::graph::BlockEventKind;

namespace {

/// Records the pitch the base hands its voices, frame by frame.
class PitchProbe final : public adx::graph::ChannelNode {
public:
    using ChannelNode::ChannelNode;
    std::vector<float> cents;

    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "probe";
    }

protected:
    void startVoice(adx::graph::Voice& /*voice*/, const BlockEvent& /*event*/,
                    const adx::graph::VoiceRender& /*render*/) noexcept override {}
    // A test probe that records into a vector; bad_alloc there ends the run, as it should.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    bool renderVoice(adx::graph::Voice& voice, std::span<float> left, std::span<float> right,
                     const adx::graph::VoiceRender& render) noexcept override {
        std::ranges::fill(left, 0.0F);
        std::ranges::fill(right, 0.0F);
        cents.insert(cents.end(), render.pitchCents.begin(), render.pitchCents.end());
        return voice.phase == adx::graph::VoicePhase::Active;
    }
    [[nodiscard]] float
    portamentoSeconds(std::span<const float> /*params*/) const noexcept override {
        return portamento;
    }

public:
    float portamento{0.0F};
};

BlockEvent event(BlockEventKind kind, std::uint32_t offset, std::uint32_t noteId,
                 std::uint8_t pitch, float value = 0.0F, std::uint32_t duration = 0) {
    return BlockEvent{.offset = offset,
                      .noteId = noteId,
                      .instance = 1,
                      .timeSource = 0,
                      .endTick = 1'000'000,
                      .value = value,
                      .duration = duration,
                      .kind = kind,
                      .pitch = pitch,
                      .velocity = 100};
}

std::vector<float> channelParams() {
    // Volume, pan, audible, pitch.
    return {1.0F, 0.0F, 1.0F, 0.0F};
}

adx::render::RenderHash hashOf(const std::vector<float>& samples) {
    return adx::render::hashSamples(samples);
}

std::vector<float> renderText(const std::string& text, std::uint32_t block, std::uint64_t frames) {
    const auto loaded = adx::tests::loadText(text);
    adx::render::RenderStats stats;
    return adx::tests::renderFrames(loaded->project, block, frames, stats);
}

double rms(const std::vector<float>& stereo, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        const double s = stereo[i * 2];
        sum += s * s;
    }
    return std::sqrt(sum / static_cast<double>(to - from));
}

const std::string kHeader = "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 120\n\n"
                            "[CHANNEL Tone]\nINSTRUMENT=testtone\nOUTPUT=insert.1\n\n";

} // namespace

TEST_CASE("slide_reaches_target_exactly", "[graph][a0]") {
    PitchProbe probe{1, 4, adx::project::VoiceStealMode::OldestReleased};
    probe.prepare(adx::graph::PrepareInfo{});
    static_cast<void>(
        adx::tests::runChannelNode(probe,
                                   {event(BlockEventKind::NoteOn, 0, 7, 69),
                                    event(BlockEventKind::PitchGlide, 100, 7, 69, 1200.0F, 1000)},
                                   2048, channelParams()));
    REQUIRE(probe.cents.size() == 2048);
    CHECK(probe.cents[99] == 0.0F);
    // Linear in cents, arriving on the frame the duration says and staying there.
    CHECK_THAT(probe.cents[600], Catch::Matchers::WithinAbs(600.0, 1.5));
    CHECK(probe.cents[1100] == 1200.0F);
    CHECK(probe.cents[2047] == 1200.0F);
    for (std::size_t i = 101; i < 1100; ++i) {
        REQUIRE(probe.cents[i] >= probe.cents[i - 1]);
    }

    // A glide for a voice that does not exist moves nothing.
    PitchProbe idle{2, 4, adx::project::VoiceStealMode::OldestReleased};
    idle.prepare(adx::graph::PrepareInfo{});
    static_cast<void>(
        adx::tests::runChannelNode(idle,
                                   {event(BlockEventKind::NoteOn, 0, 7, 69),
                                    event(BlockEventKind::PitchGlide, 10, 8, 69, 1200.0F, 10)},
                                   256, channelParams()));
    CHECK(idle.cents.back() == 0.0F);
}

TEST_CASE("portamento_overridden_by_explicit_slide", "[graph][a0]") {
    PitchProbe probe{1, 4, adx::project::VoiceStealMode::OldestReleased};
    probe.portamento = 0.01F; // 480 frames
    probe.prepare(adx::graph::PrepareInfo{});
    // A held, then B a fifth up while A is still held: legato, so B glides up from A.
    // At frame 300 an explicit slide takes over from wherever the portamento got to.
    static_cast<void>(adx::tests::runChannelNode(
        probe,
        {event(BlockEventKind::NoteOn, 0, 1, 60), event(BlockEventKind::NoteOn, 100, 2, 67),
         event(BlockEventKind::PitchGlide, 300, 2, 67, 500.0F, 200)},
        1024, channelParams()));
    // Voice B's frames are the second voice's; the probe interleaves voices per
    // segment, so read B's pitch by locating where it started at -700 cents.
    const auto start = std::ranges::find(probe.cents, -700.0F);
    REQUIRE(start != probe.cents.end());
    // Not a re-trigger at B's own pitch: B begins a fifth below itself.
    CHECK(*start == -700.0F);
    // And it ends at the slide's target, not at 0 where portamento would have.
    CHECK(probe.cents.back() == 500.0F);
}

TEST_CASE("slide_block_size_independent", "[graph][a0]") {
    const std::string text =
        kHeader + "[PATTERN P]\nLENGTH=2:0:0\nNOTES Tone\n"
                  "  A4 0:0:0 0:2:0 100 slide=7st@0:0:1920+0:1:0~smooth\n"
                  "  C5 0:2:0 0:2:0 100 "
                  "bend=0st@0:0:0|-130c@0:0:2400~exponential(0.6)|50c@0:1:0~bezier(0.2,0.9,0.4,1)\n"
                  "  E5 1:0:0 0:2:0 100 slide=-12st@0:0:0+0:0:3000 bend=0c@0:0:0|35c@0:1:0\n"
                  "\n[PLAYLIST]\nTRACK 1\n  PATTERN P 0:0:0\n\n[MIXER]\nINSERT 1 name=\"Master\"\n";
    const std::uint64_t frames = std::uint64_t{48000} * 4;
    const std::vector<float> reference = renderText(text, 64, frames);
    CHECK(hashOf(renderText(text, 256, frames)) == hashOf(reference));
    CHECK(hashOf(renderText(text, 1024, frames)) == hashOf(reference));

    // And the slides do something: the same notes without them render differently.
    std::string plain = text;
    for (const char* key : {" slide=7st@0:0:1920+0:1:0~smooth"}) {
        plain.erase(plain.find(key), std::string(key).size());
    }
    CHECK_FALSE(hashOf(renderText(plain, 64, frames)) == hashOf(reference));
}

TEST_CASE("slide_seek_midway_matches_continuous", "[graph][a0]") {
    // A note whose slide is half done when a seek lands exactly on the next note. The
    // second note's slide, rendered after the seek, matches the continuous render
    // sample for sample: glides are events on the timeline, not state replayed from
    // the song's start.
    const std::string text =
        kHeader + "[PATTERN P]\nLENGTH=4:0:0\nNOTES Tone\n"
                  "  A4 0:0:0 1:0:0 100 slide=12st@0:2:0+1:0:0\n"
                  "  E5 2:0:0 2:0:0 100 slide=-5st@0:1:0+1:0:0~logarithmic\n"
                  "\n[PLAYLIST]\nTRACK 1\n  PATTERN P 0:0:0\n\n[MIXER]\nINSERT 1 name=\"Master\"\n";
    const auto loaded = adx::tests::loadText(text);
    const std::uint64_t seekFrame = std::uint64_t{48000} * 4; // bar 2 at 120 bpm
    const std::uint64_t tail = std::uint64_t{48000} * 4;

    adx::tests::OfflineRig const continuous{256};
    REQUIRE(continuous.engine->setProject(loaded->project, 1).rebuilt);
    continuous.engine->seek(adx::core::Ticks{0});
    continuous.engine->play();
    const std::vector<float> whole = continuous.render(seekFrame + tail);

    adx::tests::OfflineRig const seeked{256};
    REQUIRE(seeked.engine->setProject(loaded->project, 1).rebuilt);
    seeked.engine->seek(adx::core::Ticks{std::int64_t{2} * 4 * kPpq});
    seeked.engine->play();
    const std::vector<float> after = seeked.render(tail);

    // The first note released at bar 1 and its 50 ms release is long over by bar 2.
    const std::vector<float> fromSeek(whole.begin() + static_cast<std::ptrdiff_t>(seekFrame * 2),
                                      whole.end());
    CHECK(hashOf(after) == hashOf(fromSeek));
}

TEST_CASE("automation_ramp_matches_curve_evaluate", "[graph][a0]") {
    using adx::core::Curve;
    using adx::core::CurveKind;
    const std::vector<Curve> curves{
        Curve{.kind = CurveKind::Linear},
        Curve{.kind = CurveKind::Exponential, .tension = 0.5F},
        Curve{.kind = CurveKind::Logarithmic, .tension = -0.3F},
        Curve{.kind = CurveKind::Smooth},
        Curve{.kind = CurveKind::Bezier, .c1x = 0.1F, .c1y = 0.0F, .c2x = 0.4F, .c2y = 1.0F},
        Curve{.kind = CurveKind::Exponential, .tension = 0.0F},
    };
    // A parameter ranging 20..20000 (a cutoff), swept from 200 to 9000 over two bars.
    constexpr float kLow = 20.0F;
    constexpr float kHigh = 20000.0F;
    const float tolerance = adx::project::automationTolerance(kLow, kHigh);
    for (const Curve& curve : curves) {
        INFO("curve kind " << static_cast<int>(curve.kind) << " tension " << curve.tension);
        const std::vector<adx::project::AutomationPoint> points{
            {.tick = 1000, .value = 200.0F, .curve = curve},
            {.tick = 1000 + (8 * kPpq), .value = 9000.0F, .curve = {}},
        };
        std::vector<adx::project::Knot> knots;
        adx::project::compileBreakpoints(knots, points, tolerance);
        CHECK(knots.size() <= static_cast<std::size_t>(adx::project::kMaxCurvePieces) + 1);
        for (std::int64_t tick = 0; tick < 1000 + (9 * kPpq); tick += 37) {
            const float exact = [&] {
                if (tick <= points[0].tick) {
                    return points[0].value;
                }
                if (tick >= points[1].tick) {
                    return points[1].value;
                }
                const auto t =
                    static_cast<float>(static_cast<double>(tick - points[0].tick) /
                                       static_cast<double>(points[1].tick - points[0].tick));
                return adx::core::interpolate(curve, points[0].value, points[1].value, t);
            }();
            const float compiled = adx::project::knotValueAt(knots.data(), knots.size(), tick);
            REQUIRE(std::abs(compiled - exact) <= tolerance * 1.01F);
        }
    }

    // Step holds the start value to the end, then jumps; hold jumps at the start.
    std::vector<adx::project::Knot> step;
    adx::project::compileBreakpoints(
        step,
        std::vector<adx::project::AutomationPoint>{
            {.tick = 0, .value = 1.0F, .curve = Curve{.kind = CurveKind::Step}},
            {.tick = 100, .value = 5.0F, .curve = {}}},
        0.001F);
    CHECK(adx::project::knotValueAt(step.data(), step.size(), 99) == 1.0F);
    CHECK(adx::project::knotValueAt(step.data(), step.size(), 100) == 5.0F);
}

TEST_CASE("automation_sweep_block_size_independent", "[graph][a0]") {
    const std::string text =
        kHeader +
        "[PATTERN P]\nLENGTH=4:0:0\nNOTES Tone\n  A4 0:0:0 4:0:0 100\n  E5 1:0:0 2:0:0 90\n"
        "AUTOMATION channel.Tone.volume\n  0:0:0 0.05\n  3:2:0 1.6 exponential(0.7)\n"
        "AUTOMATION channel.Tone.pan\n  0:1:0 -1 smooth\n  2:0:0 1 bezier(0.1,0.9,0.3,1)\n  3:0:0 "
        "0\n"
        "\n[PLAYLIST]\nTRACK 1\n  PATTERN P 0:0:0\n\n[MIXER]\nINSERT 1 name=\"Master\"\n";
    const std::uint64_t frames = std::uint64_t{48000} * 8;
    const std::vector<float> reference = renderText(text, 64, frames);
    CHECK(hashOf(renderText(text, 512, frames)) == hashOf(reference));
    CHECK(hashOf(renderText(text, 1024, frames)) == hashOf(reference));

    // Per frame, not per block: within one 1024-frame block of the sweep, consecutive
    // frames' gains differ - there is no staircase.
    const std::vector<float> coarse = renderText(text, 1024, frames);
    const std::size_t at = std::size_t{48000} * 3; // mid-sweep, a note sounding
    double steps = 0.0;
    for (std::size_t i = at; i < at + 1024; ++i) {
        steps +=
            std::abs(std::abs(coarse[i * 2]) - std::abs(coarse[(i - 1) * 2])) > 0.0 ? 1.0 : 0.0;
    }
    CHECK(steps > 1000.0);
}

TEST_CASE("automation_seek_midramp_continuous", "[graph][a0]") {
    // Seek into the middle of a volume ramp: the first frame after the seek has the
    // ramp's value at that frame, not the ramp's start value - and every frame after
    // it matches the continuous render exactly.
    const std::string text =
        kHeader + "[PATTERN P]\nLENGTH=4:0:0\nNOTES Tone\n  A4 1:2:0 2:0:0 100\n"
                  "AUTOMATION channel.Tone.volume\n  0:0:0 0.1\n  4:0:0 1.9 smooth\n"
                  "\n[PLAYLIST]\nTRACK 1\n  PATTERN P 0:0:0\n\n[MIXER]\nINSERT 1 name=\"Master\"\n";
    const auto loaded = adx::tests::loadText(text);
    const std::uint64_t seekFrame = std::uint64_t{48000} * 3; // 1:2:0 at 120 bpm

    adx::tests::OfflineRig const continuous{512};
    REQUIRE(continuous.engine->setProject(loaded->project, 1).rebuilt);
    continuous.engine->seek(adx::core::Ticks{0});
    continuous.engine->play();
    const std::vector<float> whole = continuous.render(seekFrame + 48000);

    adx::tests::OfflineRig const seeked{512};
    REQUIRE(seeked.engine->setProject(loaded->project, 1).rebuilt);
    seeked.engine->seek(adx::core::Ticks{6 * kPpq});
    seeked.engine->play();
    const std::vector<float> after = seeked.render(48000);

    const std::vector<float> fromSeek(whole.begin() + static_cast<std::ptrdiff_t>(seekFrame * 2),
                                      whole.end());
    CHECK(hashOf(after) == hashOf(fromSeek));
}

TEST_CASE("clip_envelope_applies_only_within_item", "[graph][a0]") {
    // A sustained tone; a second track holds an empty pattern from bar 1 to bar 2 whose
    // envelope pulls the channel's volume to a quarter. Before and after the item the
    // channel is at its own volume.
    const std::string text =
        kHeader + "[PATTERN Hold]\nLENGTH=4:0:0\nNOTES Tone\n  A4 0:0:0 4:0:0 100\n"
                  "\n[PATTERN Shape]\nLENGTH=1:0:0\n"
                  "\n[PLAYLIST]\nTRACK 1\n  PATTERN Hold 0:0:0\nTRACK 2\n  PATTERN Shape 1:0:0\n"
                  "    ENVELOPE channel.Tone.volume\n      0:0:0 0.25\n"
                  "\n[MIXER]\nINSERT 1 name=\"Master\"\n";
    const std::vector<float> out = renderText(text, 256, std::uint64_t{48000} * 8);
    constexpr std::size_t kBar = std::size_t{48000} * 2;
    const double before = rms(out, kBar / 2, kBar - 1000);
    const double inside = rms(out, kBar + 1000, (2 * kBar) - 1000);
    const double after = rms(out, (2 * kBar) + 1000, (3 * kBar) - 1000);
    CHECK_THAT(inside / before, Catch::Matchers::WithinRel(0.25, 1e-4));
    CHECK_THAT(after / before, Catch::Matchers::WithinRel(1.0, 1e-4));
}

TEST_CASE("arpeggiator_plays_the_held_chord_on_the_grid", "[graph][a0]") {
    using adx::project::EventKind;
    using adx::project::ScheduledEvent;
    // C E G held for one bar, plus a later single note: arp up, 1/16, one octave.
    std::vector<ScheduledEvent> events;
    const auto note = [&](std::int64_t on, std::int64_t off, std::uint8_t pitch) {
        events.push_back(ScheduledEvent{.tick = on,
                                        .endTick = off,
                                        .noteId = pitch,
                                        .instance = 1,
                                        .value = 0.0F,
                                        .kind = EventKind::NoteOn,
                                        .pitch = pitch,
                                        .velocity = 90});
        events.push_back(ScheduledEvent{.tick = off,
                                        .endTick = off,
                                        .noteId = pitch,
                                        .instance = 1,
                                        .value = 0.0F,
                                        .kind = EventKind::NoteOff,
                                        .pitch = pitch,
                                        .velocity = 0});
    };
    note(0, 4 * kPpq, 60);
    note(0, 4 * kPpq, 64);
    note(0, 4 * kPpq, 67);
    std::ranges::sort(events, adx::project::eventLess);

    adx::project::ArpSettings arp;
    arp.mode = adx::project::ArpMode::Up;
    arp.rate = adx::core::Rational{.numerator = 1, .denominator = 16};
    arp.octaves = 2;
    arp.gate = 0.5F;
    adx::project::arpeggiate(events, arp);

    std::vector<std::uint8_t> pitches;
    for (const ScheduledEvent& e : events) {
        if (e.kind == EventKind::NoteOn) {
            CHECK(e.tick % (kPpq / 4) == 0);
            CHECK(e.endTick - e.tick == kPpq / 8);
            pitches.push_back(e.pitch);
        }
    }
    const std::vector<std::uint8_t> expected{60, 64, 67, 72, 76, 79, 60, 64,
                                             67, 72, 76, 79, 60, 64, 67, 72};
    CHECK(pitches == expected);

    // And through the whole pipeline: a channel's ARP line makes the onsets land on the
    // sixteenth grid, sample-exactly.
    const std::string text =
        kHeader.substr(0, kHeader.size() - 1) + "ARP mode=up rate=1/16 octaves=1 gate=0.5\n\n" +
        "[PATTERN P]\nLENGTH=1:0:0\nNOTES Tone\n  C4 0:0:0 1:0:0 100\n  E4 0:0:0 1:0:0 100\n"
        "\n[PLAYLIST]\nTRACK 1\n  PATTERN P 0:0:0\n\n[MIXER]\nINSERT 1 name=\"Master\"\n";
    const std::vector<float> out = renderText(text, 256, std::uint64_t{48000} * 2);
    // A sixteenth at 120 bpm is 6000 frames; each arp note is 3000 frames long with a
    // 50 ms release, so there is silence before every onset.
    for (std::size_t step = 1; step < 8; ++step) {
        const std::size_t expectedFrame = step * 6000;
        CHECK(adx::tests::firstSoundFrom(out, expectedFrame - 400) == expectedFrame);
    }
}
