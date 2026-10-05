// Voice (phase_4.md §4.13), on the synthetic bank (VoicebankFixture.h). What needs
// Kasane Teto herself is tests/local/teto/, which runs only where ADX_TETO_DIR points at
// the extracted bank.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "engine/format/text/ShiftJis.h"
#include "engine/instruments/Factory.h"
#include "engine/instruments/voice/UtauResampler.h"
#include "engine/instruments/voice/VoiceInstrument.h"
#include "engine/instruments/voice/VoiceRenderCache.h"
#include "engine/instruments/voice/VoiceSetup.h"
#include "engine/instruments/voice/Voicebank.h"
#include "engine/instruments/voice/WorldAnalysis.h"
#include "engine/render/RenderHash.h"
#include "tests/cpp/instruments/InstrumentHarness.h"
#include "tests/cpp/instruments/VoicebankFixture.h"
#include "tests/cpp/render/RenderFixtures.h"

using adx::instruments::NoteRenderRequest;
using adx::instruments::OtoEntry;
using adx::instruments::Voicebank;
using Catch::Matchers::WithinAbs;

namespace {

std::shared_ptr<const Voicebank> bank() {
    return adx::instruments::loadVoicebank(adx::tests::syntheticVoicebank());
}

/// F0 of the steady middle of `x` at `rate`, from the autocorrelation peak within half
/// an octave of `expected` - so a peak at twice the period is not mistaken for it.
double pitchOf(const std::vector<float>& x, std::size_t from, std::size_t length, double rate,
               double expected) {
    double best = 0.0;
    std::size_t bestLag = 0;
    for (auto lag = static_cast<std::size_t>(rate / (expected * 1.41));
         lag < static_cast<std::size_t>(rate / (expected / 1.41)); ++lag) {
        double sum = 0.0;
        for (std::size_t i = from; i < from + length; ++i) {
            sum += static_cast<double>(x[i]) * x[i + lag];
        }
        if (sum > best) {
            best = sum;
            bestLag = lag;
        }
    }
    // Parabolic refinement around the peak.
    const auto at = [&](std::size_t lag) {
        double sum = 0.0;
        for (std::size_t i = from; i < from + length; ++i) {
            sum += static_cast<double>(x[i]) * x[i + lag];
        }
        return sum;
    };
    const double a = at(bestLag - 1);
    const double b = at(bestLag);
    const double c = at(bestLag + 1);
    const double shift = 0.5 * (a - c) / (a - (2.0 * b) + c);
    return rate / (static_cast<double>(bestLag) + shift);
}

NoteRenderRequest request(const std::string& lyric, double hz, double lengthMs,
                          const std::string& previous = {}) {
    NoteRenderRequest r;
    r.bank = bank();
    const OtoEntry* oto = adx::instruments::resolveAlias(*r.bank, lyric, previous);
    REQUIRE(oto != nullptr);
    r.oto = *oto;
    r.noteHz = hz;
    r.lengthMs = lengthMs;
    r.cutMs = lengthMs;
    r.cents.assign(static_cast<std::size_t>(lengthMs / 5.0) + 2, 0.0F);
    return r;
}

} // namespace

TEST_CASE("shiftjis_decodes_and_detects", "[voice][format]") {
    // 重音テト in cp932, and the same text already in UTF-8: both come out as UTF-8.
    const std::string sjis = "\x8F\x64\x89\xB9\x83\x65\x83\x67";
    CHECK(adx::format::detectEncoding(sjis) == adx::format::TextEncoding::ShiftJis);
    CHECK(adx::format::toUtf8(sjis) == "重音テト");
    CHECK(adx::format::toUtf8("重音テト") == "重音テト");
    CHECK(adx::format::toUtf8("\xEF\xBB\xBFoto") == "oto");
    CHECK(adx::format::shiftJisToUtf8("\xB1") == "ｱ"); // half-width katakana
}

TEST_CASE("oto_parse_shiftjis", "[voice]") {
    Voicebank loaded;
    std::vector<std::string> warnings;
    REQUIRE(loaded.load(adx::tests::syntheticVoicebank(), warnings));
    CHECK(loaded.name() == "合成テスト");
    REQUIRE(loaded.subBanks().size() == 2);
    // The VCV bank first in the search order, whatever the folders' order on disk.
    const OtoEntry* vcv = loaded.find("a い");
    REQUIRE(vcv != nullptr);
    CHECK(loaded.subBanks()[vcv->subBank].vcvAliases == 4);
    CHECK(vcv->offset == 302.5);
    CHECK(vcv->preutterance == 100.75);
    // A negative cutoff is measured from the offset.
    CHECK(vcv->endMs(2000.0) == 602.5);
    // One WAV, several aliases.
    const OtoEntry* plain = loaded.find("あ");
    const OtoEntry* rest = loaded.find("- あ");
    REQUIRE(plain != nullptr);
    REQUIRE(rest != nullptr);
    CHECK(plain->wav.filename().u8string() == u8"_あ.wav");
    // A bad line and a missing WAV are skipped with ADX4300, not fatal.
    CHECK(warnings.size() == 2);
    CHECK(std::ranges::all_of(warnings,
                              [](const std::string& w) { return w.starts_with("ADX4300"); }));
}

TEST_CASE("alias_resolution_cv_vcv", "[voice]") {
    const auto b = bank();
    const auto alias = [&](const char* lyric, const char* previous) {
        const OtoEntry* entry = adx::instruments::resolveAlias(*b, lyric, previous);
        return entry != nullptr ? entry->alias : std::string("(none)");
    };
    CHECK(alias("あ", "") == "- あ");
    CHECK(alias("い", "あ") == "a い");
    CHECK(alias("う", "い") == "i う");
    CHECK(alias("え", "う") == "u え");
    CHECK(alias("お", "え") == "e お");
    // No VCV for it: the plain CV alias. And romaji sings as kana.
    CHECK(alias("か", "あ") == "か");
    CHECK(alias("ka", "") == "- か");
    CHECK(alias("ぬ", "") == "(none)");
    CHECK(adx::instruments::vowelOf("きゃ") == "a");
    CHECK(adx::instruments::vowelOf("ン") == "n");
    CHECK(adx::instruments::vowelOf("ka") == "a");
}

TEST_CASE("frq_imported_as_f0", "[voice]") {
    const auto* const entry = bank()->find("あ");
    REQUIRE(entry != nullptr);
    CHECK(adx::instruments::frqPathFor(entry->wav).filename().u8string() == u8"_あ_wav.frq");
    const auto analysis = adx::instruments::analyseWav(entry->wav);
    REQUIRE(analysis != nullptr);
    CHECK(analysis->fromFrq);
    CHECK_THAT(analysis->f0[40], WithinAbs(adx::tests::kSyntheticF0, 1e-6));
}

TEST_CASE("world_analysis_deterministic", "[voice]") {
    const auto* const entry = bank()->find("い");
    REQUIRE(entry != nullptr);
    const auto analysis = adx::instruments::analyseWav(entry->wav);
    REQUIRE(analysis != nullptr);
    const std::vector<double> a =
        adx::instruments::harvestF0(analysis->samples, analysis->sampleRate);
    const std::vector<double> b =
        adx::instruments::harvestF0(analysis->samples, analysis->sampleRate);
    CHECK(a == b);
    // And Harvest agrees with the bank's .frq to within 30 cents on voiced frames.
    std::size_t agreed = 0;
    std::size_t voiced = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a[i] > 0.0 && analysis->f0[i] > 0.0) {
            ++voiced;
            agreed += std::abs(1200.0 * std::log2(a[i] / analysis->f0[i])) < 30.0 ? 1 : 0;
        }
    }
    REQUIRE(voiced > 50);
    CHECK(agreed >= voiced * 9 / 10);
}

TEST_CASE("resampler_pitch_accuracy", "[voice]") {
    // The synthetic voice is D#4; sung at A4, C4 and A3 its pitch is within 5 cents.
    for (const double hz : {440.0, 261.626, 220.0}) {
        INFO(hz << " Hz");
        const adx::instruments::NoteRender render =
            adx::instruments::renderNote(request("あ", hz, 500.0));
        REQUIRE(render.ok);
        const double measured = pitchOf(render.samples, render.lead + 9600, 4800,
                                        adx::instruments::kVoiceRenderRate, hz);
        CHECK(std::abs(1200.0 * std::log2(measured / hz)) < 5.0);
        for (const float s : render.samples) {
            REQUIRE(std::isfinite(s));
            REQUIRE(std::abs(s) <= 1.0F);
        }
    }
}

TEST_CASE("resampler_consonant_length_preserved", "[voice]") {
    // か at two lengths: the consonant - the first 60 ms - plays at its natural speed in
    // both, so the renders agree through it; only the vowel stretches.
    const auto shortNote = adx::instruments::renderNote(request("か", 311.127, 300.0));
    const auto longNote = adx::instruments::renderNote(request("か", 311.127, 900.0));
    REQUIRE(shortNote.ok);
    REQUIRE(longNote.ok);
    CHECK(shortNote.lead == longNote.lead);
    // WORLD places each pulse from the frames around it, so the two differ by a hair
    // near the consonant's end; a stretched consonant would decorrelate entirely.
    const std::size_t consonant = std::size_t{48} * 50; // 50 ms, inside the 60.5 ms consonant
    double ab = 0.0;
    double aa = 0.0;
    double bb = 0.0;
    for (std::size_t i = 0; i < consonant; ++i) {
        ab += static_cast<double>(shortNote.samples[i]) * longNote.samples[i];
        aa += static_cast<double>(shortNote.samples[i]) * shortNote.samples[i];
        bb += static_cast<double>(longNote.samples[i]) * longNote.samples[i];
    }
    CHECK(ab / std::sqrt(aa * bb) > 0.999);
    CHECK(longNote.samples.size() > shortNote.samples.size() * 2);
}

TEST_CASE("voice_cache_key_stable_and_sensitive", "[voice]") {
    const NoteRenderRequest base = request("あ", 440.0, 500.0);
    CHECK(adx::instruments::renderKey(base) ==
          adx::instruments::renderKey(request("あ", 440.0, 500.0)));
    std::vector<NoteRenderRequest> changed(9, base);
    changed[0].noteHz = 441.0;
    changed[1].lengthMs = 501.0;
    changed[2].cutMs = 400.0;
    changed[3].gender = 10.0;
    changed[4].breathiness = 60.0;
    changed[5].tuningCents = 5.0;
    changed[6].peakCompression = 0.0;
    changed[7].cents[3] = 1.0F;
    changed[8].oto = *bank()->find("* あ");
    for (const NoteRenderRequest& r : changed) {
        CHECK(adx::instruments::renderKey(r) != adx::instruments::renderKey(base));
    }
}

TEST_CASE("voice_note_not_ready_is_silent_not_blocking", "[voice][rt]") {
    const adx::instruments::VoiceClip pending; // never published
    adx::instruments::VoiceInstrument node{1, 8, adx::project::VoiceStealMode::OldestReleased};
    adx::rt::OwnedArray<adx::instruments::VoiceNoteRef> notes;
    notes.allocate(1);
    notes.view()[0] = adx::instruments::VoiceNoteRef{.noteId = 1, .clip = &pending};
    node.setNotes(std::move(notes), nullptr);
    node.prepare(adx::graph::PrepareInfo{.sampleRate = 48000, .maxBlockFrames = 2048});
    const adx::tests::InstrumentRun run = adx::tests::runInstrument(
        node, {adx::tests::noteOn(0, 1, 60)}, 48000, adx::tests::instrumentParams("voice"));
    CHECK(std::ranges::all_of(run.left, [](float s) { return s == 0.0F; }));
    CHECK(run.violations == 0);
    CHECK(node.latencySamples() == 19200); // the 0.4 s preroll, declared to PDC
}

namespace {

/// A voice channel singing あ い う え お on the synthetic bank.
adx::project::Project voiceProject() {
    std::vector<adx::tests::NoteSpec> specs;
    specs.reserve(5);
    for (std::int64_t n = 0; n < 5; ++n) {
        specs.push_back({.start = n * adx::core::kPpq, .length = adx::core::kPpq, .pitch = 63});
    }
    adx::project::Project project = adx::tests::toneProject(specs, 120.0);
    project.resources.baseDirectory = adx::tests::syntheticVoicebank().parent_path().string();
    adx::project::Channel& channel = project.channels.front();
    channel.instrument.type = "voice";
    channel.instrument.voicebank = "adx_voicebank_synth";
    adx::project::NoteClip& clip = project.patterns.front().noteClips.front();
    const std::array<const char*, 5> lyrics{"あ", "い", "う", "え", "お"};
    for (std::size_t n = 0; n < clip.notes.size(); ++n) {
        adx::project::NoteExtras extras;
        extras.note = clip.notes[n].id;
        extras.lyric = lyrics[n];
        static_cast<void>(clip.setExtras(extras));
    }
    return project;
}

} // namespace

TEST_CASE("voice_export_waits_for_cache", "[voice][render]") {
    const adx::project::Project project = voiceProject();
    adx::render::RenderStats stats;
    const std::vector<float> offline =
        adx::tests::renderFrames(project, 256, std::uint64_t{48000} * 3, stats);
    // The first note's vowel lands on the beat, after the PDC delay: sound by 0.5 s.
    double energy = 0.0;
    for (std::size_t i = 0; i < std::size_t{48000} * 2; ++i) {
        energy += static_cast<double>(offline[i]) * offline[i];
    }
    CHECK(energy > 1.0);
}

TEST_CASE("voice_offline_equals_realtime", "[voice][render]") {
    const adx::project::Project project = voiceProject();
    adx::render::RenderStats stats;
    const std::vector<float> offline =
        adx::tests::renderFrames(project, 256, std::uint64_t{48000} * 3, stats);
    // A session that rendered a moment ago: the cache is warm.
    adx::instruments::VoiceRenderCache::global().waitAll();
    const adx::tests::RealtimeCapture realtime =
        adx::tests::captureRealtime(project, 256, std::uint64_t{48000} * 3);
    CHECK(adx::render::hashSamples(offline) == adx::render::hashSamples(realtime.samples));
    CHECK(realtime.violations == 0);
}
