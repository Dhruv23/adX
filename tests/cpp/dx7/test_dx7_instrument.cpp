// The dx7 channel node (phase_11.5.md §4.2, §5): dx7_no_alloc, dx7_fm1_rate,
// dx7_block_size_independent, and adX's additions to msfa (transpose, operator on/off,
// amplitude modulation, bend range, mono legato, sustain, part level and pan).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/instruments/dx7/Dx7Engine.h"
#include "engine/instruments/dx7/Dx7Params.h"
#include "engine/midi/dx7/Dx7Codec.h"
#include "engine/rt/AllocGuard.h"
#include "engine/rt/RtSection.h"
#include "engine/rt/Violation.h"
#include "tests/cpp/instruments/InstrumentHarness.h"

using adx::tests::instrumentParamIndex;
using adx::tests::instrumentParams;
using adx::tests::InstrumentRun;
using adx::tests::noteOff;
using adx::tests::noteOn;
using adx::tests::preparedInstrument;
using adx::tests::runInstrument;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr double kRate = 48000.0;

void set(std::vector<float>& params, const std::string& name, float value) {
    params[instrumentParamIndex("dx7", name)] = value;
}

/// Operator 1 alone, algorithm 32: a sine at the note's pitch.
std::vector<float> sineParams() {
    std::vector<float> p = instrumentParams("dx7");
    set(p, "algorithm", 32.0F);
    for (int op = 2; op <= 6; ++op) {
        set(p, "op" + std::to_string(op) + ".level", 0.0F);
    }
    return p;
}

/// Zero crossings per second, upward, over [from, to).
double frequency(const std::vector<float>& x, std::size_t from, std::size_t to) {
    std::size_t first = 0;
    std::size_t last = 0;
    std::size_t count = 0;
    for (std::size_t i = from + 1; i < to; ++i) {
        if (x[i - 1] < 0.0F && x[i] >= 0.0F) {
            if (count == 0) {
                first = i;
            }
            last = i;
            ++count;
        }
    }
    REQUIRE(count > 2);
    return static_cast<double>(count - 1) * kRate / static_cast<double>(last - first);
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        sum += static_cast<double>(x[i]) * x[i];
    }
    return std::sqrt(sum / static_cast<double>(to - from));
}

std::size_t onset(const std::vector<float>& x, float threshold = 1e-4F) {
    for (std::size_t i = 0; i < x.size(); ++i) {
        if (std::abs(x[i]) > threshold) {
            return i;
        }
    }
    return x.size();
}

std::vector<float> randomVoiceParams(std::size_t index) {
    const std::filesystem::path path =
        std::filesystem::path(ADX_REPO_ROOT) / "tests" / "fixtures" / "dx7" / "random.syx";
    std::ifstream in(path, std::ios::binary);
    const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>(in),
                                         std::istreambuf_iterator<char>()};
    const auto voices = adx::dx7::readVoices(file).voices;
    std::vector<float> p = instrumentParams("dx7");
    std::vector<float> voice(adx::dx7::kVoiceBytes);
    adx::dx7::paramsFromVoice(voices.at(index), voice);
    std::ranges::copy(
        voice, p.begin() + static_cast<std::ptrdiff_t>(instrumentParamIndex("dx7", "op6.eg.r1")));
    return p;
}

} // namespace

TEST_CASE("dx7_plays_a_sine_at_pitch", "[dx7]") {
    const InstrumentRun run =
        runInstrument(*preparedInstrument("dx7"), {noteOn(0, 1, 69, 127)}, 24000, sineParams());
    REQUIRE(run.violations == 0);
    CHECK_THAT(frequency(run.left, 4800, 24000), WithinRel(440.0, 0.002));
    CHECK(rms(run.left, 4800, 24000) > 0.05);
    // Mono instrument, centred: both sides the same.
    CHECK(run.left == run.right);
}

TEST_CASE("dx7_block_size_independent", "[dx7]") {
    // A chord, a gap of silence, and a later note on a random voice: the same output at
    // every buffer size (offline and live renders differ only in their buffer sizes).
    const std::vector<adx::graph::BlockEvent> events = {
        noteOn(100, 1, 60, 100),   noteOn(130, 2, 64, 90), noteOn(777, 3, 67, 80),
        noteOff(9000, 1, 60),      noteOff(9100, 2, 64),   noteOff(9300, 3, 67),
        noteOn(60001, 4, 72, 110), noteOff(70000, 4, 72),
    };
    for (const std::size_t v : {0U, 5U, 11U}) {
        const std::vector<float> params = randomVoiceParams(v);
        const InstrumentRun reference =
            runInstrument(*preparedInstrument("dx7"), events, 96000, params, 64);
        REQUIRE(rms(reference.left, 0, 96000) > 1e-4);
        for (const std::uint32_t block : {37U, 128U, 256U, 1000U, 1024U}) {
            INFO("voice " << v << ", block " << block);
            const InstrumentRun run =
                runInstrument(*preparedInstrument("dx7"), events, 96000, params, block);
            REQUIRE(run.violations == 0);
            REQUIRE(run.left == reference.left);
        }
        // FM-1 rate too.
        std::vector<float> fm1 = params;
        set(fm1, "fm1.rate", 1.0F);
        const InstrumentRun fm1Ref =
            runInstrument(*preparedInstrument("dx7"), events, 96000, fm1, 64);
        for (const std::uint32_t block : {37U, 1024U}) {
            REQUIRE(runInstrument(*preparedInstrument("dx7"), events, 96000, fm1, block).left ==
                    fm1Ref.left);
        }
    }
}

TEST_CASE("dx7_fm1_rate", "[dx7]") {
    // Rendered at 44 118 Hz inside, played at 48 kHz: the pitch is still the note's and
    // the onset within a sample of engine-rate mode.
    std::vector<float> params = sineParams();
    const InstrumentRun engineRate =
        runInstrument(*preparedInstrument("dx7"), {noteOn(1000, 1, 69, 127)}, 24000, params);
    set(params, "fm1.rate", 1.0F);
    const InstrumentRun fm1 =
        runInstrument(*preparedInstrument("dx7"), {noteOn(1000, 1, 69, 127)}, 24000, params);
    REQUIRE(fm1.violations == 0);
    CHECK_THAT(frequency(fm1.left, 4800, 24000), WithinRel(440.0, 0.002));
    const auto a = static_cast<long>(onset(engineRate.left));
    const auto b = static_cast<long>(onset(fm1.left));
    INFO("engine-rate onset " << a << ", FM-1-rate onset " << b);
    CHECK(std::abs(a - b) <= 1);
    CHECK_THAT(rms(fm1.left, 4800, 24000), WithinRel(rms(engineRate.left, 4800, 24000), 0.02));
}

TEST_CASE("dx7_no_alloc", "[dx7]") {
    // 64 voices, patch changes and controller moves on the audio thread.
    adx::dx7::Dx7Engine engine;
    engine.prepare(kRate);
    adx::dx7::Dx7Function function;
    function.polyphony = 64;
    engine.setFunction(function);
    adx::dx7::Dx7Voice voice = adx::dx7::Dx7Voice::init();
    std::vector<std::int32_t> block(64);
    auto& log = adx::rt::ViolationLog::instance();
    log.reset();
    {
        const adx::rt::ScopedRtSection section;
        for (int i = 0; i < 64; ++i) {
            engine.noteOn(36 + i, 100, static_cast<std::uint64_t>(i));
        }
        for (int b = 0; b < 400; ++b) {
            if (b % 10 == 0) {
                voice.bytes[adx::dx7::kAlgorithm] = static_cast<std::uint8_t>(b % 32);
                voice.bytes[adx::dx7::kLfoAmd] = static_cast<std::uint8_t>(b % 100);
                engine.setVoice(voice);
                engine.setController(0, b % 128);
                engine.setPitchBend((b * 41) % 0x4000);
            }
            engine.render(block.data());
        }
        REQUIRE(engine.liveVoices() == 64);
    }
    if (adx::rt::allocGuardCompiledIn()) {
        REQUIRE(log.count(adx::rt::ViolationKind::Allocation) == 0);
    }
    // And through the node, at 64 voices (the harness runs inside an RT section).
    std::vector<float> params = randomVoiceParams(3);
    set(params, "polyphony", 64.0F);
    std::vector<adx::graph::BlockEvent> events;
    events.reserve(64);
    for (std::uint32_t i = 0; i < 64; ++i) {
        events.push_back(noteOn(i * 7, i + 1, static_cast<std::uint8_t>(30 + i), 100));
    }
    const InstrumentRun run = runInstrument(*preparedInstrument("dx7", 64), events, 9600, params);
    REQUIRE(run.violations == 0);
}

TEST_CASE("dx7_additions_to_msfa", "[dx7]") {
    SECTION("transpose moves the note") {
        std::vector<float> p = sineParams();
        set(p, "transpose", 12.0F);
        const InstrumentRun run =
            runInstrument(*preparedInstrument("dx7"), {noteOn(0, 1, 57, 127)}, 24000, p);
        CHECK_THAT(frequency(run.left, 4800, 24000), WithinRel(440.0, 0.002));
    }
    SECTION("operator on/off silences a carrier") {
        std::vector<float> p = sineParams();
        set(p, "ops.on", 31.0F); // OP1 (bit 5) off
        const InstrumentRun run =
            runInstrument(*preparedInstrument("dx7"), {noteOn(0, 1, 69, 127)}, 9600, p);
        CHECK(rms(run.left, 0, 9600) == 0.0);
    }
    SECTION("bend range") {
        std::vector<float> p = sineParams();
        set(p, "bend.range", 12.0F);
        set(p, "ctl.bend", 8191.0F);
        const InstrumentRun run =
            runInstrument(*preparedInstrument("dx7"), {noteOn(0, 1, 57, 127)}, 24000, p);
        // Full bend up at 12 semitones: an octave above 220 Hz (8191/8192 of it).
        CHECK_THAT(frequency(run.left, 4800, 24000), WithinRel(440.0, 0.003));
    }
    SECTION("amplitude modulation") {
        std::vector<float> p = sineParams();
        set(p, "op1.ams", 3.0F);
        set(p, "lfo.amd", 99.0F);
        set(p, "lfo.speed", 70.0F);
        set(p, "lfo.wave", 3.0F); // square
        set(p, "lfo.pmd", 0.0F);
        const InstrumentRun run =
            runInstrument(*preparedInstrument("dx7"), {noteOn(0, 1, 69, 127)}, 48000, p);
        // The level moves: short-window RMS spans a wide range.
        double lo = 1e9;
        double hi = 0.0;
        for (std::size_t at = 4800; at + 480 < 48000; at += 480) {
            const double r = rms(run.left, at, at + 480);
            lo = std::min(lo, r);
            hi = std::max(hi, r);
        }
        CHECK(hi > 4.0 * lo);
        // AMS 0 with the same LFO: steady.
        set(p, "op1.ams", 0.0F);
        const InstrumentRun steady =
            runInstrument(*preparedInstrument("dx7"), {noteOn(0, 1, 69, 127)}, 48000, p);
        CHECK_THAT(rms(steady.left, 4800, 9600), WithinRel(rms(steady.left, 30000, 34800), 0.01));
    }
    SECTION("mono legato returns to the held note") {
        std::vector<float> p = sineParams();
        set(p, "mono", 1.0F);
        const InstrumentRun run = runInstrument(
            *preparedInstrument("dx7"),
            {noteOn(0, 1, 57, 127), noteOn(12000, 2, 69, 127), noteOff(24000, 2, 69)}, 36000, p);
        CHECK_THAT(frequency(run.left, 4800, 12000), WithinRel(220.0, 0.003));
        CHECK_THAT(frequency(run.left, 13000, 24000), WithinRel(440.0, 0.003));
        CHECK_THAT(frequency(run.left, 25000, 36000), WithinRel(220.0, 0.003));
        // Legato: no envelope retrigger, so no dip at the change.
        CHECK(rms(run.left, 12100, 12600) > 0.8 * rms(run.left, 11000, 11500));
    }
    SECTION("sustain holds a released note") {
        std::vector<float> p = sineParams();
        set(p, "op1.eg.r4", 60.0F);
        const InstrumentRun dry = runInstrument(
            *preparedInstrument("dx7"), {noteOn(0, 1, 69, 127), noteOff(4800, 1, 69)}, 24000, p);
        set(p, "ctl.sustain", 1.0F);
        const InstrumentRun held = runInstrument(
            *preparedInstrument("dx7"), {noteOn(0, 1, 69, 127), noteOff(4800, 1, 69)}, 24000, p);
        CHECK(rms(dry.left, 20000, 24000) < 0.01 * rms(held.left, 20000, 24000));
    }
    SECTION("part level and pan") {
        std::vector<float> p = sineParams();
        set(p, "part.pan", -64.0F);
        const InstrumentRun left =
            runInstrument(*preparedInstrument("dx7"), {noteOn(0, 1, 69, 127)}, 9600, p);
        CHECK(rms(left.right, 4800, 9600) < 1e-6);
        set(p, "part.pan", 0.0F);
        const InstrumentRun full =
            runInstrument(*preparedInstrument("dx7"), {noteOn(0, 1, 69, 127)}, 9600, p);
        set(p, "part.level", 64.0F);
        const InstrumentRun half =
            runInstrument(*preparedInstrument("dx7"), {noteOn(0, 1, 69, 127)}, 9600, p);
        CHECK_THAT(rms(half.left, 4800, 9600) / rms(full.left, 4800, 9600),
                   WithinAbs(64.0 / 127.0, 1e-3));
    }
    SECTION("a released note frees its voice") {
        const std::vector<float> p = sineParams();
        auto node = preparedInstrument("dx7");
        const InstrumentRun run =
            runInstrument(*node, {noteOn(0, 1, 69, 127), noteOff(2400, 1, 69)}, 96000, p);
        CHECK(rms(run.left, 90000, 96000) == 0.0);
    }
}
