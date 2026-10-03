// The effect base's contract, checked on every effect type the catalog lists
// (phase_4.md §4.2, §6): bypass is click-free, wet/dry follows the effect's law, a
// clone is independent of its original, equivalence follows type and configuration,
// declared latency is the measured latency, mix 0 is the dry signal exactly, and
// process() never allocates.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/dsp/Noise.h"
#include "engine/effects/Effect.h"
#include "engine/effects/Factory.h"
#include "engine/project/Mixer.h"
#include "engine/project/TypeCatalog.h"
#include "engine/rt/AllocGuard.h"
#include "tests/cpp/effects/EffectHarness.h"

using adx::tests::EffectRun;
using adx::tests::preparedEffect;
using adx::tests::runEffect;
using adx::tests::slotParams;
using adx::tests::StereoSignal;

namespace {

std::vector<std::string> allEffectTypes() {
    std::vector<std::string> types;
    for (const adx::project::TypeInfo& info : adx::project::effectTypes()) {
        types.emplace_back(info.name);
    }
    return types;
}

/// The largest sample-to-sample step in [from, to) of either channel.
float largestStep(const StereoSignal& signal, std::size_t from, std::size_t to) {
    float step = 0.0F;
    for (std::size_t i = std::max<std::size_t>(from, 1); i < std::min(to, signal.size()); ++i) {
        step = std::max(step, std::abs(signal.left[i] - signal.left[i - 1]));
        step = std::max(step, std::abs(signal.right[i] - signal.right[i - 1]));
    }
    return step;
}

double energy(const std::vector<float>& x, std::size_t from, std::size_t to) {
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) {
        sum += static_cast<double>(x[i]) * x[i];
    }
    return sum;
}

/// An effect whose wet output is noise uncorrelated with its input: the case the
/// equal-power law is for.
class UncorrelatedEffect final : public adx::effects::Effect {
public:
    [[nodiscard]] std::string_view typeName() const noexcept override {
        return "UncorrelatedTest";
    }

protected:
    void prepareEffect(const adx::graph::PrepareInfo& /*info*/) override {}
    void resetEffect() noexcept override {
        m_noise.reseed(0xC0FFEEU);
    }
    void processWet(std::span<const float> inLeft, std::span<const float> /*inRight*/,
                    std::span<float> outLeft, std::span<float> outRight,
                    const adx::effects::EffectContext& context) noexcept override {
        for (std::uint32_t i = 0; i < context.frames; ++i) {
            // Unit-variance-matched to the input: white noise of the same amplitude.
            outLeft[i] = m_noise.next() * m_amplitude;
            outRight[i] = outLeft[i];
            (void)inLeft;
        }
    }

private:
    adx::dsp::WhiteNoise m_noise{0xC0FFEEU};
    float m_amplitude{0.5F};
};

} // namespace

TEST_CASE("effect_bypass_is_click_free", "[effects]") {
    // phase_4.md §6: no sample-to-sample discontinuity > 0.01 on a bypass toggle. An
    // effect's own output may already step by more than that (a bit crusher is all
    // steps), so the toggle may add at most 0.01 to the largest step either the wet
    // or the dry path has across the same window.
    constexpr std::size_t kFrames = 48000;
    constexpr std::size_t kOn = 12000;  // bypass engages
    constexpr std::size_t kOff = 30000; // and releases
    const StereoSignal in = adx::tests::sine(50.0, 0.5F, kFrames);

    for (const std::string& type : allEffectTypes()) {
        INFO(type);
        const auto toggled = [&](std::uint64_t frame, std::vector<float>& params) {
            params[1] = (frame >= kOn && frame < kOff) ? 1.0F : 0.0F;
        };
        const EffectRun wet = runEffect(*preparedEffect(type), in, slotParams(type, 1.0F, false));
        const EffectRun dry = runEffect(*preparedEffect(type), in, slotParams(type, 1.0F, true));
        const EffectRun run = runEffect(*preparedEffect(type), in, slotParams(type), 64, toggled);
        for (const std::size_t at : {kOn, kOff}) {
            const std::size_t from = at - 2;
            const std::size_t to = at + 400; // the 5 ms ramp is 240 frames
            const float allowed =
                std::max(largestStep(wet.out, from, to), largestStep(dry.out, from, to)) + 0.01F;
            INFO("toggle at frame " << at);
            CHECK(largestStep(run.out, from, to) <= allowed);
        }
    }
}

TEST_CASE("effect_wetdry_equal_power", "[effects]") {
    // The law itself: dry^2 + wet^2 = 1 everywhere, exact at the ends.
    for (int step = 0; step <= 20; ++step) {
        const float mix = static_cast<float>(step) / 20.0F;
        const adx::effects::MixGains gains = adx::effects::equalPowerMix(mix);
        CHECK_THAT((gains.dry * gains.dry) + (gains.wet * gains.wet),
                   Catch::Matchers::WithinAbs(1.0, 1e-6));
    }
    CHECK(adx::effects::equalPowerMix(0.0F).dry == 1.0F);
    CHECK(adx::effects::equalPowerMix(0.0F).wet == 0.0F);
    CHECK(adx::effects::equalPowerMix(1.0F).dry == 0.0F);
    CHECK(adx::effects::equalPowerMix(1.0F).wet == 1.0F);

    // And through the base, on a wet signal uncorrelated with the dry - the case the
    // law is for: the output's energy stays that of either path across the crossfade.
    constexpr std::size_t kFrames = 96000;
    const StereoSignal in = adx::tests::noise(7U, 0.5F, kFrames);
    double reference = 0.0;
    for (int step = 0; step <= 10; ++step) {
        const float mix = static_cast<float>(step) / 10.0F;
        UncorrelatedEffect effect;
        effect.prepare(adx::graph::PrepareInfo{.sampleRate = adx::tests::kTestRate});
        const EffectRun run = runEffect(effect, in, std::vector<float>{mix, 0.0F});
        const double e = energy(run.out.left, 0, kFrames);
        if (step == 0) {
            reference = e;
        }
        INFO("mix " << mix);
        // White noise of equal amplitude, 96000 frames: within 0.2 dB.
        CHECK(std::abs(10.0 * std::log10(e / reference)) < 0.2);
    }
}

TEST_CASE("effect_clone_independence", "[effects]") {
    // A clone is a fresh instance with pristine DSP state (AudioEffect.h's clone()):
    // processing it neither perturbs the original nor inherits the original's tail.
    constexpr std::size_t kFrames = 24000;
    const StereoSignal in = adx::tests::noise(11U, 0.3F, kFrames);
    for (const std::string& type : allEffectTypes()) {
        INFO(type);
        // The original, uninterrupted.
        const EffectRun alone = runEffect(*preparedEffect(type), in, slotParams(type));

        // The original again, with a clone made from it and run in between its two halves.
        const std::shared_ptr<adx::graph::SlotNode> original = preparedEffect(type);
        const StereoSignal first{
            .left = std::vector<float>(in.left.begin(), in.left.begin() + (kFrames / 2)),
            .right = std::vector<float>(in.right.begin(), in.right.begin() + (kFrames / 2))};
        const StereoSignal second{
            .left = std::vector<float>(in.left.begin() + (kFrames / 2), in.left.end()),
            .right = std::vector<float>(in.right.begin() + (kFrames / 2), in.right.end())};
        const EffectRun a = runEffect(*original, first, slotParams(type));
        std::shared_ptr<adx::graph::SlotNode> copy = adx::effects::clone(*original);
        REQUIRE(copy != nullptr);
        CHECK(copy->typeName() == original->typeName());
        copy->prepare(adx::graph::PrepareInfo{.sampleRate = adx::tests::kTestRate});
        const EffectRun cloned = runEffect(*copy, in, slotParams(type));
        const EffectRun b = runEffect(*original, second, slotParams(type));

        // The original's output is exactly what it would have been without the clone.
        for (std::size_t i = 0; i < kFrames / 2; ++i) {
            REQUIRE(a.out.left[i] == alone.out.left[i]);
            REQUIRE(b.out.left[i] == alone.out.left[i + (kFrames / 2)]);
            REQUIRE(b.out.right[i] == alone.out.right[i + (kFrames / 2)]);
        }
        // And the clone started from silence: identical to a fresh instance.
        CHECK(cloned.out.left == alone.out.left);
        CHECK(cloned.out.right == alone.out.right);
    }
}

TEST_CASE("effect_is_equivalent", "[effects]") {
    for (const std::string& type : allEffectTypes()) {
        INFO(type);
        const auto a = std::dynamic_pointer_cast<adx::effects::Effect>(preparedEffect(type));
        const auto b = std::dynamic_pointer_cast<adx::effects::Effect>(preparedEffect(type));
        REQUIRE(a != nullptr);
        CHECK(a->isEquivalent(*b));
        for (const std::string& other : allEffectTypes()) {
            if (other != type) {
                const auto c =
                    std::dynamic_pointer_cast<adx::effects::Effect>(preparedEffect(other));
                CHECK_FALSE(a->isEquivalent(*c));
            }
        }
    }

    // Structural configuration counts; a parameter does not (it lives in the snapshot).
    adx::project::Slot limiter;
    limiter.type = "Limiter";
    limiter.params = {adx::project::SlotParam{.name = "lookahead", .value = 5.0}};
    const auto node = adx::effects::makeEffect(limiter);
    CHECK(adx::effects::configMatches(*node, limiter));
    adx::project::Slot ceiling = limiter;
    ceiling.params.push_back(adx::project::SlotParam{.name = "ceiling", .value = -6.0});
    CHECK(adx::effects::configMatches(*node, ceiling));
    adx::project::Slot longer = limiter;
    longer.params[0].value = 10.0;
    CHECK_FALSE(adx::effects::configMatches(*node, longer));
    adx::project::Slot other = limiter;
    other.type = "Compressor";
    CHECK_FALSE(adx::effects::configMatches(*node, other));
}

TEST_CASE("effect_declares_latency", "[effects]") {
    // phase_4.md §6: a lookahead effect's declared latency is its measured impulse
    // delay, exactly. The impulse is small enough that no dynamics stage acts on it,
    // so what comes out is the input, moved by the latency.
    constexpr std::size_t kFrames = 8192;
    constexpr std::size_t kAt = 1000;
    for (const char* type : {"Compressor", "Limiter", "Gate"}) {
        INFO(type);
        for (const float lookahead : {0.0F, 1.5F, 5.0F, 10.0F}) {
            INFO("lookahead " << lookahead << " ms");
            adx::project::Slot slot;
            slot.type = type;
            slot.params = {adx::project::SlotParam{.name = "lookahead", .value = lookahead}};
            const std::shared_ptr<adx::graph::SlotNode> node = adx::effects::makeEffect(slot);
            node->prepare(adx::graph::PrepareInfo{.sampleRate = adx::tests::kTestRate});
            // At least the lookahead; the Limiter adds its true-peak detector's delay.
            const std::uint32_t declared = node->latencySamples();
            const long lookaheadFrames = std::lround(lookahead * 0.001F * adx::tests::kTestRate);
            CHECK(std::cmp_greater_equal(declared, lookaheadFrames));

            // A gate opens on signal; give it one above its threshold. The rest see a
            // quiet impulse.
            const float amplitude = std::string_view(type) == "Gate" ? 0.9F : 0.01F;
            const std::vector<float> params = slotParams(type);
            const EffectRun run =
                runEffect(*node, adx::tests::impulse(kAt, amplitude, kFrames), params);
            std::size_t peak = 0;
            for (std::size_t i = 1; i < kFrames; ++i) {
                if (std::abs(run.out.left[i]) > std::abs(run.out.left[peak])) {
                    peak = i;
                }
            }
            CHECK(peak == kAt + declared);
        }
    }
}

TEST_CASE("effect_mix_zero_is_the_dry_signal", "[effects]") {
    // Mix 0 is the input bit for bit, moved by the declared latency: the dry path is
    // delayed with the wet so the two stay aligned (Effect.h).
    constexpr std::size_t kFrames = 9600;
    const StereoSignal in = adx::tests::noise(3U, 0.4F, kFrames);
    for (const std::string& type : allEffectTypes()) {
        INFO(type);
        const std::shared_ptr<adx::graph::SlotNode> node = preparedEffect(type);
        const std::size_t latency = node->latencySamples();
        const EffectRun run = runEffect(*node, in, slotParams(type, 0.0F));
        for (std::size_t i = latency; i < kFrames; ++i) {
            REQUIRE(run.out.left[i] == in.left[i - latency]);
            REQUIRE(run.out.right[i] == in.right[i - latency]);
        }
    }
}

TEST_CASE("effect_no_alloc_in_process", "[effects][rt]") {
    if (!adx::rt::allocGuardCompiledIn()) {
        SUCCEED("allocator hook not compiled in (Release)");
        return;
    }
    const StereoSignal in = adx::tests::noise(5U, 0.5F, 48000);
    for (const std::string& type : allEffectTypes()) {
        INFO(type);
        const EffectRun run = runEffect(*preparedEffect(type), in, slotParams(type), 256);
        CHECK(run.violations == 0);
    }
}
