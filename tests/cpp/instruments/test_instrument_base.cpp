// The instrument base's contract, on every instrument type the catalog lists
// (phase_4.md §4.2, §6): one table-driven harness for note-on, release and steal; no
// allocation in process(); exact silence when idle; and pitch that sums in cents.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "engine/project/TypeCatalog.h"
#include "engine/rt/AllocGuard.h"
#include "tests/cpp/instruments/InstrumentHarness.h"

using adx::tests::instrumentParamIndex;
using adx::tests::instrumentParams;
using adx::tests::InstrumentRun;
using adx::tests::noteOff;
using adx::tests::noteOn;
using adx::tests::preparedInstrument;
using adx::tests::runInstrument;

namespace {

std::vector<std::string> allInstrumentTypes() {
    std::vector<std::string> types;
    for (const adx::project::TypeInfo& info : adx::project::instrumentTypes()) {
        types.emplace_back(info.name);
    }
    return types;
}

bool silent(const InstrumentRun& run, std::size_t from, std::size_t to) {
    for (std::size_t i = from; i < to; ++i) {
        if (run.left[i] != 0.0F || run.right[i] != 0.0F) {
            return false;
        }
    }
    return true;
}

float peak(const InstrumentRun& run, std::size_t from, std::size_t to) {
    float p = 0.0F;
    for (std::size_t i = from; i < to; ++i) {
        p = std::max({p, std::abs(run.left[i]), std::abs(run.right[i])});
    }
    return p;
}

} // namespace

TEST_CASE("instrument_voice_lifecycle", "[instruments]") {
    for (const std::string& type : allInstrumentTypes()) {
        INFO(type);
        // One note: it sounds, it releases on note-off, and its voice is freed once the
        // release has run, after which the output is exactly zero.
        {
            const auto node = preparedInstrument(type);
            const InstrumentRun run = runInstrument(*node, {noteOn(0, 1, 69), noteOff(9600, 1, 69)},
                                                    48000 * 3, instrumentParams(type));
            CHECK(peak(run, 0, 9600) > 0.01F);
            CHECK(run.sounding.front() == 1);
            CHECK(run.sounding.back() == 0);
            CHECK(silent(run, (std::size_t{48000} * 3) - 4800, std::size_t{48000} * 3));
        }
        // Polyphony is a ceiling: three notes on a two-voice channel never sound more
        // than two voices, and the third note still plays (by stealing).
        {
            const auto node = preparedInstrument(type, 2);
            const InstrumentRun run =
                runInstrument(*node,
                              {noteOn(0, 1, 60), noteOn(480, 2, 64), noteOn(960, 3, 67),
                               noteOff(24000, 1, 60), noteOff(24000, 2, 64), noteOff(24000, 3, 67)},
                              48000 * 3, instrumentParams(type));
            CHECK(*std::ranges::max_element(run.sounding) <= 2);
            CHECK(peak(run, 4800, 24000) > 0.01F);
            CHECK(run.sounding.back() == 0);
        }
        // A note-off for a note that is not playing changes nothing.
        {
            const auto node = preparedInstrument(type);
            const InstrumentRun run =
                runInstrument(*node, {noteOff(0, 9, 69)}, 4800, instrumentParams(type));
            CHECK(silent(run, 0, 4800));
            CHECK(run.sounding.back() == 0);
        }
    }
}

TEST_CASE("instrument_no_alloc_in_process", "[instruments][rt]") {
    if (!adx::rt::allocGuardCompiledIn()) {
        SUCCEED("allocator hook not compiled in (Release)");
        return;
    }
    for (const std::string& type : allInstrumentTypes()) {
        INFO(type);
        // A chord, a steal, a glide and every release - the paths that might be tempted
        // to allocate - all inside the realtime section.
        const auto node = preparedInstrument(type, 4);
        const InstrumentRun run = runInstrument(
            *node,
            {noteOn(0, 1, 60), noteOn(10, 2, 64), noteOn(20, 3, 67), noteOn(30, 4, 71),
             noteOn(4800, 5, 72), adx::tests::pitchGlide(5000, 5, 72, 200.0F, 2400),
             noteOff(24000, 1, 60), noteOff(24000, 2, 64), noteOff(24000, 3, 67),
             noteOff(24000, 4, 71), noteOff(24000, 5, 72)},
            48000 * 2, instrumentParams(type), 128);
        CHECK(run.violations == 0);
    }
}

TEST_CASE("instrument_silence_when_idle", "[instruments]") {
    // phase_4.md §6: with no voices the output is exactly 0.0 - not -120 dB of denormal
    // hiss from a filter left ringing - both before any note and after the last one.
    for (const std::string& type : allInstrumentTypes()) {
        INFO(type);
        const auto node = preparedInstrument(type);
        const InstrumentRun idle = runInstrument(*node, {}, 48000, instrumentParams(type));
        CHECK(silent(idle, 0, 48000));
        const InstrumentRun after = runInstrument(*node, {noteOn(0, 1, 40), noteOff(2400, 1, 40)},
                                                  48000 * 4, instrumentParams(type));
        CHECK(after.sounding.back() == 0);
        CHECK(silent(after, std::size_t{48000} * 3, std::size_t{48000} * 4));
    }
}

TEST_CASE("slide_plus_vibrato_sums_in_cents", "[instruments]") {
    // phase_4.md §4.2: base pitch + slide + vibrato sum in cents. An octave slide (a jump)
    // under a 50-cent, 5 Hz vibrato: the pitch averages to the octave and swings 50
    // cents either side of it. The Additive's default patch is one harmonic - a sine -
    // so its frequency can be read from zero crossings.
    const std::string type = "additive";
    std::vector<float> params = instrumentParams(type);
    params[instrumentParamIndex(type, "vibrato.rate")] = 5.0F;
    params[instrumentParamIndex(type, "vibrato.depthCents")] = 50.0F;
    params[instrumentParamIndex(type, "env.attack")] = 0.0F;
    params[instrumentParamIndex(type, "env.sustain")] = 1.0F;
    const auto node = preparedInstrument(type);
    constexpr std::uint32_t kFrames = 48000 * 2;
    const InstrumentRun run = runInstrument(
        *node, {noteOn(0, 1, 57), adx::tests::pitchGlide(0, 1, 57, 1200.0F, 0)}, kFrames, params);

    // Instantaneous frequency from rising zero crossings, interpolated.
    std::vector<double> frequency;
    double last = -1.0;
    // Exactly nine vibrato cycles (5 Hz at 48 kHz is 9600 frames), so the mean is not
    // biased by a partial one.
    constexpr std::uint32_t kFrom = 4800;
    constexpr std::uint32_t kTo = kFrom + (9 * 9600);
    for (std::uint32_t i = kFrom; i < kTo; ++i) {
        const float a = run.left[i - 1];
        const float b = run.left[i];
        if (a < 0.0F && b >= 0.0F) {
            const double crossing = static_cast<double>(i - 1) + (a / (a - b));
            if (last >= 0.0) {
                frequency.push_back(48000.0 / (crossing - last));
            }
            last = crossing;
        }
    }
    REQUIRE(frequency.size() > 700); // 440 Hz for 1.8 s
    double logSum = 0.0;
    for (const double f : frequency) {
        logSum += std::log2(f / 440.0);
    }
    const double meanCents = 1200.0 * logSum / static_cast<double>(frequency.size());
    const auto [low, high] = std::ranges::minmax_element(frequency);
    const double lowCents = 1200.0 * std::log2(*low / 440.0);
    const double highCents = 1200.0 * std::log2(*high / 440.0);
    // A3 (220 Hz) + 1200 cents = 440 Hz is the centre.
    CHECK_THAT(meanCents, Catch::Matchers::WithinAbs(0.0, 2.0));
    CHECK_THAT(highCents, Catch::Matchers::WithinAbs(50.0, 3.0));
    CHECK_THAT(lowCents, Catch::Matchers::WithinAbs(-50.0, 3.0));
}
