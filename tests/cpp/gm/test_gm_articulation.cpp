// The gm voice's per-zone articulation and SF2's default modulators, on banks built in
// the test (phase_12.md §4.7.4, §5: zone_articulation_*, sf2_default_modulators), and
// the live controllers a gm channel answers.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/format/sfz/SfzParser.h"
#include "engine/format/soundfont/SoundFont.h"
#include "engine/instruments/gm/GmSetup.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/instruments/InstrumentHarness.h"

namespace fs = std::filesystem;
namespace sf = adx::format::soundfont;
using adx::instruments::GmBankView;
using adx::instruments::GmInstrument;
using adx::instruments::GmPresetView;
using adx::instruments::GmSampleView;
using adx::tests::noteOff;
using adx::tests::noteOn;

namespace {

constexpr float kRate = 48000.0F;

/// One second of 1.0, so a note's output is its envelope times its gains.
const std::vector<float>& dc() {
    static const std::vector<float> kSamples(48000, 1.0F);
    return kSamples;
}

/// A hand-made bank: program 0 and program 5 (6 dB down), each one looped DC zone.
struct DcBank {
    std::array<sf::BankZone, 2> zones{};
    std::array<GmSampleView, 1> samples{};
    std::array<GmPresetView, 2> presets{};

    explicit DcBank(const sf::BankEnvelope& volume) {
        samples[0] = GmSampleView{.data = dc().data(), .frames = 48000, .rate = 48000};
        for (sf::BankZone& zone : zones) {
            zone.sample = 0;
            zone.rootKey = 60;
            zone.start = 0;
            zone.end = 48000;
            zone.loopStart = 1000;
            zone.loopEnd = 47000;
            zone.loop = sf::BankLoop::Continuous;
            zone.volume = volume;
            zone.vibrato.hz = 0.0F;
            zone.modLfo.hz = 0.0F;
        }
        zones[1].attenuation = 6.0206F;
        presets[0] = GmPresetView{.bank = 0, .program = 0, .firstZone = 0, .zoneCount = 1};
        presets[1] = GmPresetView{.bank = 0, .program = 5, .firstZone = 1, .zoneCount = 1};
    }

    [[nodiscard]] GmBankView view() const {
        return GmBankView{.presets = presets, .zones = zones, .samples = samples};
    }
};

std::shared_ptr<GmInstrument> node(const DcBank& bank) {
    auto made = std::make_shared<GmInstrument>(1, 16, adx::project::VoiceStealMode::OldestReleased);
    made->setBank(bank.view(), nullptr);
    made->prepare(adx::graph::PrepareInfo{.sampleRate = 48000, .maxBlockFrames = 2048});
    return made;
}

std::shared_ptr<GmInstrument> tinyNode() {
    const fs::path path = adx::tests::repoRoot() / "tests" / "data" / "banks" / "tiny_gm.sf2";
    auto made = std::make_shared<GmInstrument>(1, 16, adx::project::VoiceStealMode::OldestReleased);
    std::string error;
    adx::instruments::setGmBank(*made, adx::instruments::GmBankCache::get(path, error), path);
    made->prepare(adx::graph::PrepareInfo{.sampleRate = 48000, .maxBlockFrames = 2048});
    return made;
}

adx::graph::BlockEvent controller(std::uint32_t offset, std::uint8_t control, std::uint8_t value,
                                  std::uint8_t part = 0) {
    adx::graph::BlockEvent event = noteOn(offset, 0, 0, 0);
    event.kind = adx::graph::BlockEventKind::Controller;
    event.noteId = 0;
    event.pitch = control;
    event.velocity = value;
    event.part = part;
    return event;
}

adx::graph::BlockEvent onPart(adx::graph::BlockEvent event, std::uint8_t part) {
    event.part = part;
    return event;
}

std::vector<float> params(std::initializer_list<std::pair<std::string_view, float>> values = {}) {
    std::vector<float> out = adx::tests::instrumentParams("gm");
    for (const auto& [name, value] : values) {
        out[adx::tests::instrumentParamIndex("gm", name)] = value;
    }
    return out;
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    }
    return std::sqrt(sum / static_cast<double>(to - from));
}

double db(double x) {
    return 20.0 * std::log10(std::max(x, 1e-12));
}

/// The frame at which `x` first satisfies `pred`, from `from`.
template<typename Pred>
std::size_t first(const std::vector<float>& x, std::size_t from, Pred pred) {
    for (std::size_t i = from; i < x.size(); ++i) {
        if (pred(x[i])) {
            return i;
        }
    }
    return x.size();
}

/// Cents of instantaneous pitch from zero crossings, one value per period.
std::vector<double> pitchTrack(const std::vector<float>& x, std::size_t from, std::size_t to) {
    std::vector<double> crossings;
    for (std::size_t i = from + 1; i < to; ++i) {
        if (x[i - 1] < 0.0F && x[i] >= 0.0F) {
            crossings.push_back(static_cast<double>(i - 1) +
                                (static_cast<double>(-x[i - 1]) / (x[i] - x[i - 1])));
        }
    }
    std::vector<double> cents;
    for (std::size_t i = 1; i < crossings.size(); ++i) {
        cents.push_back(1200.0 * std::log2(1.0 / (crossings[i] - crossings[i - 1])));
    }
    return cents;
}

double swing(const std::vector<double>& cents) {
    const auto [lo, hi] = std::ranges::minmax(cents);
    return hi - lo;
}

} // namespace

TEST_CASE("zone_articulation_dahdsr", "[gm][articulation]") {
    // Each stage's length and level as the zone's generators say, within 1 ms and 0.1 dB,
    // key-to-hold and key-to-decay scaling included.
    const sf::BankEnvelope shape{.delay = 0.01F,
                                 .attack = 0.02F,
                                 .hold = 0.03F,
                                 .decay = 0.4F,
                                 .sustain = 0.5F,
                                 .release = 0.2F,
                                 .keyToHold = 0.0F,
                                 .keyToDecay = 0.0F};
    const DcBank bank(shape);
    const auto run = adx::tests::runInstrument(
        *node(bank), {noteOn(0, 1, 60, 127), noteOff(24000, 1, 60)}, 48000, params());
    const std::vector<float>& x = run.left;
    const auto ms = [](std::size_t frames) { return static_cast<double>(frames) * 1000.0 / kRate; };
    // Delay: silent for 10 ms.
    const std::size_t sounding = first(x, 0, [](float v) { return v > 1e-6F; });
    CHECK(std::abs(ms(sounding) - 10.0) < 1.0);
    // Attack: linear to full in 20 ms.
    const std::size_t full = first(x, 0, [](float v) { return v >= 0.9999F; });
    CHECK(std::abs(ms(full - sounding) - 20.0) < 1.0);
    // Hold: full for 30 ms.
    const std::size_t falling = first(x, full, [](float v) { return v < 0.9999F; });
    CHECK(std::abs(ms(falling - full) - 30.0) < 1.0);
    // Decay: 96 dB per `decay` seconds, stopping at the sustain level (-6.02 dB).
    const std::size_t sustained = first(x, falling, [](float v) { return v <= 0.5001F; });
    const double expected = 400.0 * (6.0206 / 96.0);
    CHECK(std::abs(ms(sustained - falling) - expected) < 1.0);
    CHECK(std::abs(db(x[20000]) - db(0.5)) < 0.1);
    // Release: 96 dB per `release` seconds from where it was.
    CHECK(std::abs(db(x[24000 + 2400]) - (db(0.5) - (96.0 * 0.05 / 0.2))) < 0.1);

    // Key scaling: 100 timecents per key from 60 halves hold and decay an octave up.
    sf::BankEnvelope scaled = shape;
    scaled.keyToHold = 100.0F;
    scaled.keyToDecay = 100.0F;
    const DcBank keyed(scaled);
    const auto up = adx::tests::runInstrument(
        *node(keyed), {noteOn(0, 1, 72, 127), noteOff(24000, 1, 72)}, 48000, params());
    const std::size_t upFull = first(up.left, 0, [](float v) { return v >= 0.9999F; });
    const std::size_t upFalling = first(up.left, upFull, [](float v) { return v < 0.9999F; });
    const std::size_t upSustained = first(up.left, upFalling, [](float v) { return v <= 0.5001F; });
    CHECK(std::abs(ms(upFalling - upFull) - 15.0) < 1.0);
    CHECK(std::abs(ms(upSustained - upFalling) - (expected / 2.0)) < 1.0);
}

TEST_CASE("zone_articulation_inert", "[gm][articulation]") {
    // A zone whose articulation is the SF2 default - no delay, attack, hold or decay, full
    // sustain, no LFO, no filter - plays its sample exactly, at the zone's gain.
    const DcBank bank(sf::BankEnvelope{});
    const auto run =
        adx::tests::runInstrument(*node(bank), {noteOn(0, 1, 60, 127)}, 4800, params());
    // Its gain is the centre of the equal-power pan law: cos(pi/4) sqrt(2), 1 to a ulp.
    REQUIRE(std::abs(run.left[0] - 1.0F) < 1e-6F);
    for (std::size_t i = 0; i < 4800; ++i) {
        INFO(i);
        REQUIRE(run.left[i] == run.left[0]);
        REQUIRE(run.right[i] == run.left[0]);
    }
}

TEST_CASE("sf2_default_modulators", "[gm][soundfont]") {
    const DcBank bank(sf::BankEnvelope{});
    // Velocity to attenuation: GM's (velocity / 127) squared.
    const auto loud =
        adx::tests::runInstrument(*node(bank), {noteOn(0, 1, 60, 127)}, 4800, params());
    const auto soft =
        adx::tests::runInstrument(*node(bank), {noteOn(0, 1, 60, 64)}, 4800, params());
    CHECK(std::abs((rms(soft.left, 0, 4800) / rms(loud.left, 0, 4800)) -
                   std::pow(64.0 / 127.0, 2.0)) < 1e-4);

    // CC7 volume (square law, unity at 100), CC11 expression (unity at 127), CC10 pan.
    const auto vol = adx::tests::runInstrument(
        *node(bank), {controller(0, 7, 50), noteOn(0, 1, 60, 127)}, 4800, params());
    CHECK(std::abs(rms(vol.left, 0, 4800) - 0.25) < 1e-4);
    const auto expr = adx::tests::runInstrument(
        *node(bank), {controller(0, 11, 64), noteOn(0, 1, 60, 127)}, 4800, params());
    CHECK(std::abs(rms(expr.left, 0, 4800) - std::pow(64.0 / 127.0, 2.0)) < 1e-4);
    const auto pan = adx::tests::runInstrument(
        *node(bank), {controller(0, 10, 127), noteOn(0, 1, 60, 127)}, 4800, params());
    CHECK(rms(pan.left, 0, 4800) < 1e-6);
    CHECK(std::abs(rms(pan.right, 0, 4800) - std::sqrt(2.0)) < 1e-3);

    // CC1 adds up to 50 cents of vibrato to the zone's own (the fixture's sine: 5 Hz,
    // +-20 cents after 0.2 s).
    const auto still =
        adx::tests::runInstrument(*tinyNode(), {noteOn(0, 1, 69, 127)}, 48000, params());
    const auto wheel = adx::tests::runInstrument(
        *tinyNode(), {controller(0, 1, 127), noteOn(0, 1, 69, 127)}, 48000, params());
    const double own = swing(pitchTrack(still.left, 14400, 48000));
    const double moved = swing(pitchTrack(wheel.left, 14400, 48000));
    CHECK(std::abs(own - 40.0) < 6.0);
    CHECK(std::abs(moved - 140.0) < 12.0);
    // The parameter does the same as the live controller.
    const auto drawn = adx::tests::runInstrument(*tinyNode(), {noteOn(0, 1, 69, 127)}, 48000,
                                                 params({{"modWheel", 127.0F}}));
    CHECK(std::abs(swing(pitchTrack(drawn.left, 14400, 48000)) - moved) < 2.0);

    // Bend: a glide of +200 cents (a full wheel at the default 2-semitone range) moves
    // the pitch by exactly that.
    const auto flat =
        adx::tests::runInstrument(*tinyNode(), {noteOn(0, 1, 69, 127)}, 9600, params());
    const auto bent = adx::tests::runInstrument(
        *tinyNode(), {noteOn(0, 1, 69, 127), adx::tests::pitchGlide(0, 1, 69, 200.0F, 0)}, 9600,
        params());
    const auto mean = [](const std::vector<double>& c) {
        double sum = 0.0;
        for (const double v : c) {
            sum += v;
        }
        return sum / static_cast<double>(c.size());
    };
    CHECK(std::abs(mean(pitchTrack(bent.left, 960, 9600)) - mean(pitchTrack(flat.left, 960, 9600)) -
                   200.0) < 1.0);
}

TEST_CASE("gm_live_controllers", "[gm]") {
    const sf::BankEnvelope shape{.release = 0.05F};
    const DcBank bank(shape);
    // A live program change: the next note plays program 5 (6 dB down); the ringing
    // one keeps program 0.
    const auto run = adx::tests::runInstrument(*node(bank),
                                               {noteOn(0, 1, 60, 127),
                                                controller(2400, adx::graph::kProgramChange, 5),
                                                noteOn(4800, 2, 62, 127), noteOff(4800, 1, 60)},
                                               12000, params());
    CHECK(std::abs(run.left[2400] - 1.0F) < 1e-6F);
    CHECK(std::abs(db(run.left[11000]) - db(0.5)) < 0.05);

    // In part mode each part keeps its own: part 2's program change leaves part 1 alone.
    const auto parts =
        adx::tests::runInstrument(*node(bank),
                                  {controller(0, adx::graph::kProgramChange, 5, 1),
                                   noteOn(0, 1, 60, 127), onPart(noteOn(0, 2, 62, 127), 1)},
                                  4800, params({{"part", 1.0F}}));
    CHECK(std::abs(parts.left[2400] - 1.5F) < 1e-5F);

    // All Notes Off releases; Reset All Controllers undoes the wheel and expression.
    const auto off = adx::tests::runInstrument(
        *node(bank), {noteOn(0, 1, 60, 127), controller(2400, 123, 0)}, 9600, params());
    CHECK(std::abs(off.left[2300] - 1.0F) < 1e-6F);
    CHECK(off.left[9000] < 0.01F);
    const auto reset = adx::tests::runInstrument(
        *node(bank), {controller(0, 11, 64), controller(0, 121, 0), noteOn(0, 1, 60, 127)}, 4800,
        params());
    CHECK(std::abs(reset.left[2400] - 1.0F) < 1e-6F);
    CHECK(reset.violations == 0);
}

namespace {

fs::path ariaFolder() {
    return adx::tests::repoRoot() / "ARIAConverted" / "sf2" / "gm_sf2";
}

/// A note through gm on `bank`, left channel.
std::vector<float> play(const std::shared_ptr<const sf::Bank>& bank, std::uint8_t program,
                        std::uint8_t key, std::uint32_t frames) {
    auto made = std::make_shared<GmInstrument>(1, 8, adx::project::VoiceStealMode::OldestReleased);
    adx::instruments::setGmBank(*made, bank, ariaFolder());
    made->prepare(adx::graph::PrepareInfo{.sampleRate = 48000, .maxBlockFrames = 2048});
    return adx::tests::runInstrument(*made, {noteOn(0, 1, key, 110)}, frames,
                                     params({{"program", static_cast<float>(program)}}))
        .left;
}

/// The pitch of [from, from + 960) in cents from `hz`, by autocorrelation over lags
/// within a semitone of hz's period (parabolic between lags).
double centsNear(const std::vector<float>& x, std::size_t from, double hz) {
    const double period = 48000.0 / hz;
    const auto lo = static_cast<std::size_t>(period / 1.06);
    const auto hi = static_cast<std::size_t>(period * 1.06) + 1;
    std::vector<double> r(hi + 2, 0.0);
    for (std::size_t lag = lo - 1; lag <= hi + 1; ++lag) {
        for (std::size_t i = from; i < from + 960; ++i) {
            r[lag] += static_cast<double>(x[i]) * static_cast<double>(x[i + lag]);
        }
    }
    std::size_t best = lo;
    for (std::size_t lag = lo; lag <= hi; ++lag) {
        best = r[lag] > r[best] ? lag : best;
    }
    const double a = r[best - 1];
    const double b = r[best];
    const double c = r[best + 1];
    const double lag = static_cast<double>(best) + (0.5 * (a - c) / (a - (2.0 * b) + c));
    return 1200.0 * std::log2(period / lag);
}

/// RMS of a - b over [from, to), relative to a's own, in dB.
double differenceDb(const std::vector<float>& a, const std::vector<float>& b, std::size_t from,
                    std::size_t to) {
    std::vector<float> d(a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        d[i] = a[i] - b[i];
    }
    return db(rms(d, from, to)) - db(rms(a, from, to));
}

} // namespace

TEST_CASE("sfz_import_aria_gm", "[sfz][gm]") {
    // The user's converted Windows bank (local: ARIAConverted/ is not in the repository).
    std::error_code ec;
    if (!fs::is_directory(ariaFolder(), ec)) {
        SKIP("no ARIAConverted/ here");
    }
    // Every file imports with no opcode this build has never heard of.
    std::size_t files = 0;
    for (const auto& entry : fs::recursive_directory_iterator(ariaFolder())) {
        if (entry.path().extension() != ".sfz") {
            continue;
        }
        adx::format::sfz::Import import;
        REQUIRE(adx::format::sfz::loadSfz(entry.path(), import));
        INFO(entry.path().filename().string() << "\n" << adx::format::sfz::reportOf(import));
        CHECK(import.count(adx::format::sfz::NoteKind::Unknown) == 0);
        ++files;
    }
    CHECK(files == 235);

    // The folder is a gm bank: every file a program, and the programs cover the keys
    // the Windows bank they were converted from covers.
    auto bank = std::make_shared<sf::Bank>();
    std::string error;
    REQUIRE(sf::loadBank(ariaFolder(), *bank, error));
    CHECK(bank->presets.size() == 235);
    INFO([&] {
        std::string notes;
        for (const sf::BankNote& note : bank->notes) {
            notes += note.what + ": " + note.detail + "\n";
        }
        return notes;
    }());
    CHECK(bank->notes.empty());
    const fs::path dls = sf::systemGmBank();
    if (!dls.empty()) {
        sf::Bank windows;
        REQUIRE(sf::loadBank(dls, windows, error));
        std::size_t compared = 0;
        for (const sf::BankPreset& ours : bank->presets) {
            const sf::BankPreset* theirs = nullptr;
            for (const sf::BankPreset& p : windows.presets) {
                if (p.bank == ours.bank && p.program == ours.program) {
                    theirs = &p;
                }
            }
            if (theirs == nullptr) {
                continue;
            }
            const auto keys = [](const sf::BankPreset& p) {
                std::array<bool, 128> covered{};
                for (const sf::BankZone& z : p.zones) {
                    for (std::size_t k = z.keyLow; k <= z.keyHigh; ++k) {
                        covered.at(k) = true;
                    }
                }
                return covered;
            };
            INFO("bank " << ours.bank << " program " << static_cast<int>(ours.program) << " "
                         << ours.name);
            CHECK(keys(ours) == keys(*theirs));
            ++compared;
        }
        CHECK(compared >= 128);
    }

    // The Reed Organ's eg06 pitch dive and the Viola's lfo07 vibrato reach the sound:
    // the same note with each depth zeroed differs where the modulator acts.
    const auto without = [&](std::uint8_t program, bool envelope) {
        auto copy = std::make_shared<sf::Bank>(*bank);
        for (sf::BankPreset& p : copy->presets) {
            if (p.bank == 0 && p.program == program) {
                for (sf::BankZone& z : p.zones) {
                    (envelope ? z.modToPitch : z.vibrato.toPitch) = 0.0F;
                }
            }
        }
        return std::shared_ptr<const sf::Bank>(copy);
    };
    const sf::BankPreset* organ = bank->find(0, 20);
    REQUIRE(organ != nullptr);
    REQUIRE(!organ->zones.empty());
    CHECK(organ->zones.front().modToPitch == -95.0F);
    const std::vector<float> dive = play(bank, 20, 60, 24000);
    const std::vector<float> flat = play(without(20, true), 20, 60, 24000);
    const double c4 = 440.0 * std::pow(2.0, -9.0 / 12.0);
    // In the first milliseconds the dive holds the note tens of cents flat; by 200 ms it
    // has settled to within a few cents of the note without it.
    CHECK(centsNear(dive, 240, c4) - centsNear(flat, 240, c4) < -20.0);
    CHECK(std::abs(centsNear(dive, 9600, c4) - centsNear(flat, 9600, c4)) < 3.0);

    const sf::BankPreset* viola = bank->find(0, 41);
    REQUIRE(viola != nullptr);
    CHECK(viola->zones.front().vibrato.toPitch == 10.0F);
    const std::vector<float> vibrato = play(bank, 41, 62, 48000);
    const std::vector<float> still = play(without(41, false), 41, 62, 48000);
    // Before lfo07_delay (0.31 s) the two are the same note; after it, one swings.
    CHECK(differenceDb(vibrato, still, 0, 12000) < -60.0);
    const double d4 = 440.0 * std::pow(2.0, -7.0 / 12.0);
    const auto swingOf = [&](const std::vector<float>& x) {
        double lo = 1e9;
        double hi = -1e9;
        for (std::size_t at = 19200; at + 2000 < 48000; at += 480) {
            const double c = centsNear(x, at, d4);
            lo = std::min(lo, c);
            hi = std::max(hi, c);
        }
        return hi - lo;
    };
    CHECK(swingOf(vibrato) > swingOf(still) + 10.0);
}
