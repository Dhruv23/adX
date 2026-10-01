// Loop wraps: not a seek. Voices sustain through them and cursors do not re-search.

#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "tests/cpp/render/RenderFixtures.h"

using adx::core::kPpq;
using adx::core::Ticks;
using adx::tests::NoteSpec;
using adx::tests::OfflineRig;

namespace {

// 120 bpm at 48 kHz: one beat is 24000 frames. The loop is beats 4..8.
constexpr std::size_t kBeat = 24000;
constexpr std::size_t kLoopStart = 4 * kBeat;
constexpr std::size_t kLoopEnd = 8 * kBeat;

adx::project::Project loopProject() {
    // N1 straddles the loop start (beats 3..5); N2 straddles the loop end (7..9).
    return adx::tests::toneProject({NoteSpec{.start = 3 * kPpq, .length = 2 * kPpq, .pitch = 36},
                                    NoteSpec{.start = 7 * kPpq, .length = 2 * kPpq, .pitch = 38}});
}

void playLooped(OfflineRig& rig) {
    REQUIRE(rig.engine->setProject(loopProject(), 1).rebuilt);
    rig.engine->setLoop(adx::transport::LoopRegion{
        .start = Ticks{4 * kPpq}, .end = Ticks{8 * kPpq}, .enabled = true});
    rig.engine->play();
}

} // namespace

TEST_CASE("loop_wrap_sustains_voices", "[transport][loop]") {
    OfflineRig rig{256};
    playLooped(rig);
    // Pass one runs 0..8 beats; pass two starts at output frame kLoopEnd, at beat 4.
    const std::vector<float> out = rig.render(kLoopEnd + (4 * kBeat));

    // Not cut: nothing anywhere jumps, including at the wrap.
    float largest = 0.0F;
    for (std::size_t frame = 1; frame < out.size() / 2; ++frame) {
        largest = std::max(largest, std::abs(out[frame * 2] - out[(frame - 1) * 2]));
    }
    INFO("largest step " << largest);
    CHECK(largest < 0.01F);

    // N2 is still sounding right after the wrap: it is released there, not stopped.
    CHECK(std::abs(out[(kLoopEnd + 20) * 2]) > 0.0F);

    // Not retriggered: in pass one, beats 4..5 carry N1's tail end; in pass two, N1's
    // note-on is behind the loop start and nothing plays until N2 comes round again.
    CHECK(std::abs(out[(kLoopStart + kBeat / 2) * 2]) > 0.0F);
    for (std::size_t frame = kLoopEnd + 4800; frame < kLoopEnd + (3 * kBeat) - 10; ++frame) {
        REQUIRE(out[frame * 2] == 0.0F);
    }
    // And N2 does come round again, on the exact sample.
    CHECK(adx::tests::firstSoundFrom(out, kLoopEnd + 4800) == kLoopEnd + (3 * kBeat));
}

TEST_CASE("scheduler_no_rescan_on_loop_wrap", "[transport][loop][scheduler]") {
    OfflineRig rig{256};
    playLooped(rig);
    // Up to just before the first wrap: the initial search, and the loop-start search.
    static_cast<void>(rig.render(kLoopEnd - 1000));
    const std::uint64_t searches = rig.stats().cursorSearches;
    REQUIRE(searches > 0);

    // Five wraps.
    static_cast<void>(rig.render(5 * (kLoopEnd - kLoopStart)));
    CHECK(rig.stats().cursorSearches == searches);
}
