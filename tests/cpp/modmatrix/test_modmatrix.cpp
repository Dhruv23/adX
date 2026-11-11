// The per-voice modulation layer (phase_12.md §4.3, §5): the VA's own LFO routings as
// matrix slots, the pitch envelope, mono and legato, tempo-synced LFOs, and every
// source into every destination an instrument reads.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "engine/dsp/ModMatrix.h"
#include "engine/project/TypeCatalog.h"
#include "tests/cpp/instruments/InstrumentHarness.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::tests::instrumentParamIndex;
using adx::tests::instrumentParams;
using adx::tests::noteOff;
using adx::tests::noteOn;
using adx::tests::preparedInstrument;
using adx::tests::runInstrument;

namespace {

constexpr double kRate = 48000.0;

void set(std::vector<float>& params, std::string_view type, const std::string& name, float value) {
    const std::uint32_t index = instrumentParamIndex(type, name);
    REQUIRE(index != adx::project::kNoParam);
    params[index] = value;
}

/// A VA playing one clean sine at full level from the first frame.
std::vector<float> sineVa() {
    std::vector<float> params = instrumentParams("va");
    set(params, "va", "osc1.wave", 0.0F);
    set(params, "va", "env.attack", 0.0F);
    set(params, "va", "env.sustain", 1.0F);
    set(params, "va", "level", 0.0F);
    return params;
}

/// Rising zero crossings of `x`, in fractional frames.
std::vector<double> risingCrossings(const std::vector<float>& x) {
    std::vector<double> out;
    for (std::size_t i = 1; i < x.size(); ++i) {
        if (x[i - 1] < 0.0F && x[i] >= 0.0F) {
            const double a = x[i - 1];
            const double b = x[i];
            out.push_back(static_cast<double>(i - 1) + (-a / (b - a)));
        }
    }
    return out;
}

/// Mean frequency over [from, to) frames, from the rising crossings inside it.
double frequencyIn(const std::vector<float>& x, std::size_t from, std::size_t to) {
    const std::vector<float> part(x.begin() + static_cast<std::ptrdiff_t>(from),
                                  x.begin() + static_cast<std::ptrdiff_t>(to));
    const std::vector<double> crossings = risingCrossings(part);
    REQUIRE(crossings.size() >= 2);
    return kRate * static_cast<double>(crossings.size() - 1) /
           (crossings.back() - crossings.front());
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    }
    return std::sqrt(sum / static_cast<double>(to - from));
}

double hzOf(double note) {
    return 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
}

} // namespace

TEST_CASE("va_lfo_target_equals_matrix_slot", "[modmatrix][va]") {
    // lfo1.target/depth is sugar for one slot: the two spellings render bit for bit.
    struct Case {
        float target;
        adx::dsp::ModDest dest;
        float scale;
    };
    // NOLINTBEGIN(modernize-use-designated-initializers) - a table reads as a table.
    const std::vector<Case> cases{{1, adx::dsp::ModDest::Pitch, 0.25F},
                                  {2, adx::dsp::ModDest::Cutoff, 0.5F},
                                  {3, adx::dsp::ModDest::Level, 1.0F},
                                  {4, adx::dsp::ModDest::Pan, 1.0F},
                                  {5, adx::dsp::ModDest::PulseWidth, 1.0F}};
    // NOLINTEND(modernize-use-designated-initializers)
    for (const Case& c : cases) {
        INFO("target " << c.target);
        std::vector<float> base = instrumentParams("va");
        set(base, "va", "osc1.wave", 4.0F); // pulse, so the width matters
        set(base, "va", "filter.cutoff", 1500.0F);
        set(base, "va", "lfo1.rate", 6.0F);

        std::vector<float> legacy = base;
        set(legacy, "va", "lfo1.target", c.target);
        set(legacy, "va", "lfo1.depth", 0.5F);

        std::vector<float> slot = base;
        set(slot, "va", "mod.1.source", static_cast<float>(adx::dsp::ModSource::Lfo1));
        set(slot, "va", "mod.1.dest", static_cast<float>(c.dest));
        set(slot, "va", "mod.1.amount", 0.5F * c.scale);

        const std::vector<adx::graph::BlockEvent> events{noteOn(0, 1, 57), noteOff(20000, 1, 57)};
        const auto plain = runInstrument(*preparedInstrument("va"), events, 30000, base);
        const auto a = runInstrument(*preparedInstrument("va"), events, 30000, legacy);
        const auto b = runInstrument(*preparedInstrument("va"), events, 30000, slot);
        CHECK(a.left == b.left);
        CHECK(a.right == b.right);
        CHECK(a.left != plain.left);
    }
}

TEST_CASE("pitch_env_exact", "[modmatrix][va]") {
    // A sine that starts 24 semitones high and falls for 0.1 s. Its rising zero
    // crossings are compared with those of the same phase integrated from the formula:
    // amount * 0.01^(t / time). The filter delays everything by one constant time,
    // taken from the settled tail. A cent of pitch error held for 100 ms at 1 kHz would
    // move a crossing by ~58 us; they must sit within 4.
    constexpr double kAmount = 24.0;
    constexpr double kTime = 0.1;
    constexpr int kNote = 69;
    for (const float curve : {0.0F, 1.0F}) {
        INFO("curve " << curve);
        std::vector<float> params = sineVa();
        set(params, "va", "pitchEnv.amount", static_cast<float>(kAmount));
        set(params, "va", "pitchEnv.time", static_cast<float>(kTime));
        set(params, "va", "pitchEnv.curve", curve);
        const auto run =
            runInstrument(*preparedInstrument("va"), {noteOn(0, 1, kNote)}, 24000, params);

        std::vector<double> expected;
        const double frames = kTime * kRate;
        const double factor = std::pow(0.01, 1.0 / frames);
        double offset = kAmount;
        double phase = 0.0;
        for (int n = 0; n < 24000; ++n) {
            // The envelope's float value is what the oscillator is given.
            const double hz = hzOf(kNote + static_cast<double>(static_cast<float>(offset)));
            const double next = phase + (hz / kRate);
            if (std::floor(next) > std::floor(phase)) {
                expected.push_back(static_cast<double>(n) +
                                   ((std::floor(next) - phase) / (next - phase)));
            }
            phase = next;
            if (curve >= 0.5F) {
                offset = std::max(0.0, offset - (kAmount / frames));
            } else {
                offset *= factor;
                if (offset < 1e-5) {
                    offset = 0.0;
                }
            }
        }
        const std::vector<double> measured = risingCrossings(run.left);
        REQUIRE(measured.size() + 2 >= expected.size());
        const std::size_t count = std::min(measured.size(), expected.size());
        // The sine starts on a crossing the detector cannot see: align on the tail.
        const std::ptrdiff_t shift =
            std::llround((measured[count - 1] - expected[count - 1]) * hzOf(kNote) / kRate);
        std::vector<double> lag;
        for (std::size_t k = count - 40; k < count; ++k) {
            lag.push_back(
                measured[k] -
                expected[static_cast<std::size_t>(static_cast<std::ptrdiff_t>(k) + shift)]);
        }
        std::ranges::sort(lag);
        const double delay = lag[lag.size() / 2];
        double worst = 0.0;
        std::size_t compared = 0;
        for (std::size_t k = 0; k < count; ++k) {
            const auto e = static_cast<std::ptrdiff_t>(k) + shift;
            if (e < 0 || static_cast<std::size_t>(e) >= expected.size() ||
                measured[k] < 0.005 * kRate) {
                continue;
            }
            worst = std::max(worst,
                             std::abs(measured[k] - expected[static_cast<std::size_t>(e)] - delay));
            ++compared;
        }
        CHECK(compared > 100);
        CHECK(worst / kRate < 4e-6);
    }
}

TEST_CASE("legato_no_retrigger", "[modmatrix][va]") {
    // The envelope falls to a low sustain; a second note starts while the first is
    // held. Poly starts a new voice. Mono restarts the envelope on the same voice.
    // Legato leaves the envelope alone and only moves the pitch, gliding.
    const auto play = [](float mode) {
        std::vector<float> params = sineVa();
        set(params, "va", "env.decay", 0.1F);
        set(params, "va", "env.sustain", 0.1F);
        set(params, "va", "env.release", 0.01F);
        set(params, "va", "glide", 0.1F);
        set(params, "va", "voice.mode", mode);
        return runInstrument(
            *preparedInstrument("va"),
            {noteOn(0, 1, 57), noteOn(24000, 2, 69), noteOff(36000, 1, 57), noteOff(40000, 2, 69)},
            48000, params);
    };
    const auto poly = play(0.0F);
    const auto mono = play(1.0F);
    const auto legato = play(2.0F);

    const std::size_t block = 24000 / 64;
    CHECK(poly.sounding[block + 2] == 2);
    CHECK(mono.sounding[block + 2] == 1);
    CHECK(legato.sounding[block + 2] == 1);

    // Just before the second note every mode sits at the sustain level.
    const double held = rms(legato.left, 23000, 24000);
    CHECK(rms(mono.left, 23000, 24000) == held);
    // Mono: the envelope is back at the top. Legato: it never moved.
    CHECK(rms(mono.left, 24050, 24500) > 4.0 * held);
    CHECK(rms(legato.left, 24050, 24500) < 1.2 * held);
    CHECK(rms(legato.left, 24050, 24500) > 0.8 * held);

    // The glide: halfway through its 0.1 s the pitch is between the two notes, and
    // once it is over the voice is on the new one.
    const double mid = frequencyIn(legato.left, 26000, 26800);
    CHECK(mid > hzOf(58.0));
    CHECK(mid < hzOf(68.0));
    const double settled = frequencyIn(legato.left, 30000, 34000);
    CHECK(std::abs(1200.0 * std::log2(settled / hzOf(69.0))) < 1.0);

    // The first note's note-off no longer owns the voice; the second's releases it.
    CHECK(legato.sounding[(38000 / 64)] == 1);
    CHECK(rms(legato.left, 47000, 48000) < 0.01 * held);
}

TEST_CASE("glide_mode_always", "[modmatrix][va]") {
    // Two notes that do not overlap: no glide by default, a glide with glide.mode=1.
    const auto play = [](float mode) {
        std::vector<float> params = sineVa();
        set(params, "va", "env.release", 0.001F);
        set(params, "va", "glide", 0.2F);
        set(params, "va", "glide.mode", mode);
        return runInstrument(*preparedInstrument("va"),
                             {noteOn(0, 1, 57), noteOff(8000, 1, 57), noteOn(12000, 2, 69)}, 24000,
                             params);
    };
    CHECK(std::abs(1200.0 * std::log2(frequencyIn(play(0.0F).left, 12100, 14000) / hzOf(69.0))) <
          2.0);
    CHECK(frequencyIn(play(1.0F).left, 12100, 14000) < hzOf(65.0));
}

TEST_CASE("lfo_delay_and_fade", "[modmatrix][va]") {
    // Vibrato that arrives late: nothing for 0.25 s, then a fade in over 0.25 s.
    std::vector<float> params = sineVa();
    set(params, "va", "lfo1.rate", 6.0F);
    set(params, "va", "lfo1.target", 1.0F);
    set(params, "va", "lfo1.depth", 0.2F);
    set(params, "va", "lfo1.delay", 0.25F);
    set(params, "va", "lfo1.fade", 0.25F);
    const auto late = runInstrument(*preparedInstrument("va"), {noteOn(0, 1, 69)}, 48000, params);
    const std::vector<float> none = sineVa();
    const auto plain = runInstrument(*preparedInstrument("va"), {noteOn(0, 1, 69)}, 48000, none);
    // Identical until the delay is over, different after.
    CHECK(std::equal(late.left.begin(), late.left.begin() + 11900, plain.left.begin()));
    CHECK_FALSE(std::equal(late.left.begin() + 30000, late.left.begin() + 40000,
                           plain.left.begin() + 30000));
}

TEST_CASE("lfo_sync_tempo_change", "[modmatrix][va]") {
    // A quarter-note tremolo through a tempo ramp from 90 to 150 bpm. The gain's
    // minimum is three quarters of the way through each beat, and it stays there:
    // the LFO follows the tempo map's beats, not a rate in Hz.
    const std::string text =
        "[PROJECT]\nADX_VERSION=2\n\n[TEMPO]\n0:0:0 90 ramp\n2:0:0 150\n\n"
        "[CHANNEL Lead]\nINSTRUMENT=va\nOUTPUT=insert.1\n"
        "PARAM osc1.wave=0\nPARAM env.attack=0\nPARAM env.sustain=1\n"
        "PARAM lfo1.target=3\nPARAM lfo1.depth=1\nPARAM lfo1.sync=8\n"
        "\n[PATTERN P]\nLENGTH=3:0:0\nNOTES Lead\n  A5 0:0:0 3:0:0 100\n\n"
        "[PLAYLIST]\nTRACK 1\n  PATTERN P 0:0:0\n\n[MIXER]\nINSERT 1 name=\"Master\"\n";
    const auto loaded = adx::tests::loadText(text);
    adx::render::RenderStats stats;
    const double end = loaded->project.tempo.secondsAt(adx::core::Ticks{adx::core::kPpq * 4 * 3});
    const auto frames = static_cast<std::uint64_t>(end * kRate);
    const std::vector<float> stereo = adx::tests::renderFrames(loaded->project, 256, frames, stats);

    // The envelope: mean absolute value over 4 ms.
    constexpr std::size_t kWindow = 192;
    std::vector<double> envelope(frames / kWindow, 0.0);
    for (std::size_t w = 0; w < envelope.size(); ++w) {
        for (std::size_t i = 0; i < kWindow; ++i) {
            envelope[w] += std::abs(stereo[2 * ((w * kWindow) + i)]);
        }
    }
    int checked = 0;
    for (int beat = 0; beat < 11; ++beat) {
        const auto tick = static_cast<std::int64_t>((beat + 0.75) * adx::core::kPpq);
        const double at = loaded->project.tempo.secondsAt(adx::core::Ticks{tick}) * kRate;
        // The quietest window within half a beat either side.
        const auto centre = static_cast<std::size_t>(at / kWindow);
        const auto reach = static_cast<std::size_t>(0.2 * kRate / kWindow);
        std::size_t quietest = centre;
        for (std::size_t w = centre > reach ? centre - reach : 0;
             w < std::min(envelope.size(), centre + reach); ++w) {
            if (envelope[w] < envelope[quietest]) {
                quietest = w;
            }
        }
        INFO("beat " << beat);
        CHECK(std::abs(((static_cast<double>(quietest) + 0.5) * kWindow) - at) < 0.006 * kRate);
        ++checked;
    }
    CHECK(checked == 11);
}

TEST_CASE("modmatrix_each_source_dest", "[modmatrix]") {
    // Every source into every destination a type reads moves that type's output, and a
    // destination the type has no use for leaves it bit for bit alone.
    using adx::dsp::ModDest;
    struct Type {
        std::string name;
        std::vector<std::pair<std::string, float>> setup;
        std::vector<ModDest> reads;
        ModDest ignores;
    };
    // NOLINTBEGIN(modernize-use-designated-initializers) - a table reads as a table.
    const std::vector<Type> types{
        {"va",
         {{"osc1.wave", 4.0F},
          {"osc2.level", 0.5F},
          {"unison.voices", 3.0F},
          {"filter.cutoff", 900.0F},
          {"filter.resonance", 0.3F},
          {"filter.drive", 1.0F}},
         {ModDest::Pitch, ModDest::Osc1Pitch, ModDest::Osc2Pitch, ModDest::PulseWidth,
          ModDest::Cutoff, ModDest::Resonance, ModDest::Level, ModDest::Pan, ModDest::Drive,
          ModDest::UnisonDetune},
         ModDest::FmOp1},
        {"wavetable",
         {{"unison.voices", 3.0F},
          {"filter.cutoff", 900.0F},
          {"filter.resonance", 0.3F},
          {"position", 0.3F}},
         {ModDest::Pitch, ModDest::Cutoff, ModDest::Resonance, ModDest::Level, ModDest::Pan,
          ModDest::UnisonDetune, ModDest::WtPosition},
         ModDest::PulseWidth},
        {"fm",
         {{"algorithm", 5.0F},
          {"feedback", 0.3F},
          {"op2.level", 0.4F},
          {"op4.level", 0.4F},
          {"op6.level", 0.4F},
          {"op3.level", 0.5F},
          {"op5.level", 0.5F}},
         {ModDest::Pitch, ModDest::Level, ModDest::Pan, ModDest::FmOp1, ModDest::FmOp2,
          ModDest::FmOp3, ModDest::FmOp4, ModDest::FmOp5, ModDest::FmOp6, ModDest::FmFeedback},
         ModDest::Cutoff},
        {"additive",
         {{"resfilter.cutoff", 900.0F}, {"resfilter.resonance", 0.3F}},
         {ModDest::Pitch, ModDest::Cutoff, ModDest::Resonance, ModDest::Level, ModDest::Pan},
         ModDest::WtPosition},
    };
    // NOLINTEND(modernize-use-designated-initializers)
    const std::vector<adx::graph::BlockEvent> events{noteOn(0, 1, 52, 100), noteOff(9000, 1, 52)};
    for (const Type& type : types) {
        std::vector<float> base = instrumentParams(type.name);
        for (const auto& [name, value] : type.setup) {
            set(base, type.name, name, value);
        }
        // Something for each source to be: a wheel, pressure, and a pitch envelope too
        // small to hear but not zero.
        set(base, type.name, "mod.wheel", 0.5F);
        set(base, type.name, "mod.pressure", 0.5F);
        set(base, type.name, "pitchEnv.amount", 0.02F);
        set(base, type.name, "modEnv2.sustain", 0.5F);
        const auto plain = runInstrument(*preparedInstrument(type.name), events, 12000, base);
        const auto with = [&](int source, ModDest dest) {
            std::vector<float> params = base;
            set(params, type.name, "mod.3.source", static_cast<float>(source));
            set(params, type.name, "mod.3.dest", static_cast<float>(dest));
            set(params, type.name, "mod.3.amount", 0.6F);
            return runInstrument(*preparedInstrument(type.name), events, 12000, params);
        };
        for (int source = 1; source < static_cast<int>(adx::dsp::kModSourceCount); ++source) {
            for (const ModDest dest : type.reads) {
                INFO(type.name << " source " << source << " dest " << static_cast<int>(dest));
                const auto run = with(source, dest);
                CHECK(run.violations == 0);
                CHECK((run.left != plain.left || run.right != plain.right));
            }
            INFO(type.name << " source " << source << " ignored dest");
            const auto run = with(source, type.ignores);
            CHECK(run.left == plain.left);
            CHECK(run.right == plain.right);
        }
    }
}
