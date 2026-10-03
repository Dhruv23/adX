// One golden hash per instrument, per shipped preset and per effect at three settings
// (phase_4.md §6, golden_all_instruments and golden_all_effects).
//
// The corpus files exercise whole projects; these pin each unit on its own, so a DSP
// change anywhere is caught by name - "effect/Chorus/high moved" - rather than only as
// "suffocation.adx moved". Each unit is driven directly, without a graph, on a fixed
// phrase or a fixed signal, so the hashes depend on nothing but the unit.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "engine/preset/Preset.h"
#include "engine/preset/PresetLibrary.h"
#include "engine/project/TypeCatalog.h"
#include "engine/render/RenderHash.h"
#include "tests/cpp/Corpus.h"
#include "tests/cpp/effects/EffectHarness.h"
#include "tests/cpp/instruments/InstrumentHarness.h"
#include "tests/cpp/render/GoldenHashes.h"

namespace {

constexpr std::uint32_t kPhraseFrames = 36000;

/// A chord, a later note gliding up a tone, and every note released before the end so
/// the release stages are in the hash too.
adx::tests::InstrumentRun playPhrase(std::string_view type, std::vector<float> params) {
    const auto node = adx::tests::preparedInstrument(type);
    return adx::tests::runInstrument(
        *node,
        {adx::tests::noteOn(0, 1, 48, 100), adx::tests::noteOn(0, 2, 55, 80),
         adx::tests::noteOn(0, 3, 64, 64), adx::tests::noteOn(6000, 4, 72, 120),
         adx::tests::pitchGlide(9000, 4, 72, 200.0F, 6000), adx::tests::noteOff(18000, 1, 48),
         adx::tests::noteOff(18000, 2, 55), adx::tests::noteOff(18000, 3, 64),
         adx::tests::noteOff(21000, 4, 72)},
        kPhraseFrames, std::move(params));
}

std::string hashOf(const std::vector<float>& left, const std::vector<float>& right) {
    std::vector<float> interleaved;
    interleaved.reserve(left.size() * 2);
    for (std::size_t i = 0; i < left.size(); ++i) {
        interleaved.push_back(left[i]);
        interleaved.push_back(right[i]);
    }
    return adx::render::hashSamples(interleaved).hex();
}

bool audible(const std::vector<float>& samples) {
    return std::ranges::any_of(samples, [](float s) { return s != 0.0F; });
}

constexpr std::string_view kHeader =
    "# Golden hashes per unit (tests/cpp/render/test_golden_units.cpp): each instrument\n"
    "# and shipped preset playing one phrase, each effect at three settings on one\n"
    "# signal. 48 kHz, 64-frame blocks, FNV-1a 128 over interleaved floats.\n"
    "# Regenerate with ADX_UPDATE_GOLDEN=1, and only on purpose.\n";

} // namespace

TEST_CASE("golden_all_instruments", "[render][golden]") {
    std::map<std::string, std::string> actual;

    for (const adx::project::TypeInfo& type : adx::project::instrumentTypes()) {
        INFO(type.name);
        const adx::tests::InstrumentRun run =
            playPhrase(type.name, adx::tests::instrumentParams(type.name));
        CHECK(run.violations == 0);
        CHECK(audible(run.left));
        actual["instrument/" + std::string(type.name)] = hashOf(run.left, run.right);
    }

    adx::format::DiagnosticList diagnostics;
    adx::preset::PresetLibrary library;
    library.scan(adx::preset::packsDirectory(adx::tests::repoRoot()), diagnostics);
    for (const adx::preset::Preset& preset : library.all()) {
        if (preset.kind() != adx::project::TypeKind::Instrument) {
            continue;
        }
        INFO(preset.name);
        std::vector<float> params = adx::tests::instrumentParams(preset.type);
        for (const adx::project::ParamValue& param : preset.params) {
            const std::uint32_t index = adx::tests::instrumentParamIndex(preset.type, param.name);
            REQUIRE(index != adx::project::kNoParam);
            params[index] = static_cast<float>(param.value);
        }
        const adx::tests::InstrumentRun run = playPhrase(preset.type, std::move(params));
        CHECK(audible(run.left));
        actual["preset/" + preset.pack + "/" + preset.name] = hashOf(run.left, run.right);
    }

    // Names with spaces would break the file's two-column lines.
    std::map<std::string, std::string> keyed;
    for (auto& [name, hash] : actual) {
        std::string key = name;
        std::ranges::replace(key, ' ', '_');
        keyed[key] = hash;
    }
    adx::tests::checkGolden(adx::tests::goldenDirectory() / "instruments.txt", kHeader, keyed);
}

TEST_CASE("golden_all_effects", "[render][golden]") {
    // The signal: a tone over noise, with a sidechain key of a slower, louder tone so a
    // keyed effect has something to react to.
    adx::tests::StereoSignal in = adx::tests::noise(7U, 0.25F, 24000);
    const adx::tests::StereoSignal tone = adx::tests::sine(220.0, 0.35F, 24000);
    for (std::size_t i = 0; i < in.size(); ++i) {
        in.left[i] += tone.left[i];
        in.right[i] += tone.right[i];
    }
    const adx::tests::StereoSignal key = adx::tests::sine(3.0, 0.9F, 24000);

    std::map<std::string, std::string> actual;
    for (const adx::project::TypeInfo& type : adx::project::effectTypes()) {
        // default; every parameter a quarter of the way up its range; three quarters.
        for (const auto& [setting, position] :
             {std::pair<std::string_view, float>{"default", -1.0F},
              {"low", 0.25F},
              {"high", 0.75F}}) {
            INFO(type.name << " " << setting);
            std::vector<float> params = adx::tests::slotParams(type.name);
            if (position >= 0.0F) {
                for (const adx::project::ParamDescriptor& descriptor : type.params) {
                    if (descriptor.curve != adx::project::CurvePart::None) {
                        continue;
                    }
                    const std::uint32_t index =
                        adx::tests::slotParamIndex(type.name, descriptor.name);
                    params[index] =
                        descriptor.minimum + (position * (descriptor.maximum - descriptor.minimum));
                }
            }
            const auto node = adx::tests::preparedEffect(type.name);
            const adx::tests::EffectRun run =
                adx::tests::runEffect(*node, in, params, 64, {}, &key);
            CHECK(run.violations == 0);
            actual["effect/" + std::string(type.name) + "/" + std::string(setting)] =
                hashOf(run.out.left, run.out.right);
        }
    }
    adx::tests::checkGolden(adx::tests::goldenDirectory() / "effects.txt", kHeader, actual);
}
