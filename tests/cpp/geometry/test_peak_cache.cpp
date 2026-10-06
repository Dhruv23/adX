// ClipPeakCache: invalidation, tier choice, exactness, and never blocking (phase_5.md
// §4.5, §5).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <random>

#include "engine/geometry/ClipPeakCache.h"
#include "engine/geometry/WaveformGeometry.h"

using namespace adx::geometry;
using adx::format::kSampleGuardFrames;
using adx::format::SampleBuffer;

namespace {

std::shared_ptr<SampleBuffer> noise(std::uint64_t frames, bool stereo, std::uint32_t seed) {
    auto buffer = std::make_shared<SampleBuffer>();
    buffer->sampleRate = 48000;
    buffer->sourceChannels = stereo ? 2 : 1;
    buffer->frames = frames;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> sample(-1.0F, 1.0F);
    buffer->left.assign(frames + (2 * kSampleGuardFrames), 0.0F);
    for (std::uint64_t f = 0; f < frames; ++f) {
        buffer->left[kSampleGuardFrames + f] = sample(rng) * 0.9F;
    }
    if (stereo) {
        buffer->right.assign(frames + (2 * kSampleGuardFrames), 0.0F);
        for (std::uint64_t f = 0; f < frames; ++f) {
            buffer->right[kSampleGuardFrames + f] = sample(rng) * 0.5F;
        }
    }
    return buffer;
}

std::pair<std::int16_t, std::int16_t> brute(const SampleBuffer& buffer, float gain,
                                            std::uint64_t begin, std::uint64_t end) {
    std::int16_t lo = 32767;
    std::int16_t hi = -32768;
    const auto l = buffer.leftAudio();
    const auto r = buffer.rightAudio();
    for (std::uint64_t f = begin; f < std::min(end, buffer.frames); ++f) {
        lo = std::min(lo, quantiseDown(std::min(l[f], r[f]) * gain));
        hi = std::max(hi, quantiseUp(std::max(l[f], r[f]) * gain));
    }
    return {lo, hi};
}

} // namespace

TEST_CASE("peak_cache_invalidation", "[geometry]") {
    ClipPeakCache cache;
    const auto source = noise(10000, true, 1);
    const PeakKey key{.path = "a.wav"};
    const auto first = cache.request(key, source);
    REQUIRE(cache.request(key, source) == first); // changing nothing does not

    PeakKey changed = key;
    changed.path = "b.wav";
    REQUIRE(cache.request(changed, source) != first);
    changed = key;
    changed.pitchSemitones = 1.0F;
    REQUIRE(cache.request(changed, source) != first);
    changed = key;
    changed.stretch = 2.0F;
    REQUIRE(cache.request(changed, source) != first);
    changed = key;
    changed.reversed = true;
    REQUIRE(cache.request(changed, source) != first);
    changed = key;
    changed.sourceOffset = 480;
    REQUIRE(cache.request(changed, source) != first);
    changed = key;
    changed.gain = 0.5F;
    REQUIRE(cache.request(changed, source) != first);
    // The same key over a re-decoded buffer is stale too.
    REQUIRE(cache.request(key, noise(10000, true, 1)) != first);
    cache.waitIdle();
}

TEST_CASE("peak_cache_tier_selection", "[geometry]") {
    for (double spc = 0.5; spc < 1.0e7; spc *= 1.37) {
        const int tier = idealTier(spc);
        const auto width = static_cast<double>(samplesPerPeak(tier));
        REQUIRE(width <= std::max(spc, 1.0));
        // Within 4x of the request, except past the coarsest tier.
        if (tier < kPeakTiers - 1) {
            REQUIRE(spc < width * 4.0);
        }
    }
    REQUIRE(samplesPerPeak(0) == 1);
    REQUIRE(samplesPerPeak(kPeakTiers - 1) == 65536);
}

TEST_CASE("peak_cache_matches_bruteforce", "[geometry]") {
    ClipPeakCache cache;
    const auto source = noise(300001, true, 9);
    const auto peaks = cache.request(PeakKey{.path = "x", .gain = 0.8F}, source);
    cache.waitIdle();
    REQUIRE(peaks->complete());
    std::mt19937 rng(5);
    std::uniform_int_distribution<std::uint64_t> frame(0, source->frames);
    for (int tier = 0; tier < kPeakTiers; ++tier) {
        const std::uint64_t width = samplesPerPeak(tier);
        for (int i = 0; i < 50; ++i) {
            // Peak-aligned ranges, which is what a tier can answer exactly.
            const std::uint64_t a = (frame(rng) / width) * width;
            const std::uint64_t b = std::min(source->frames, a + (width * (1 + (frame(rng) % 7))));
            if (a >= b) {
                continue;
            }
            REQUIRE(peaks->range(tier, a, b) == brute(*source, 0.8F, a, b));
        }
    }
}

TEST_CASE("peak_cache_never_blocks", "[geometry]") {
    ClipPeakCache cache;
    // Ten minutes at 48 kHz.
    const auto source = noise(48000ULL * 600ULL, false, 2);
    const auto begin = std::chrono::steady_clock::now();
    const auto peaks = cache.request(PeakKey{.path = "ten-minutes"}, source);
    WaveformGeometry wave;
    wave.build(*peaks, WaveView{.frameStart = 0.0,
                                .frameEnd = static_cast<double>(source->frames),
                                .columns = 1000});
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    // Requesting and drawing the not-yet-analysed file took no analysis time.
    REQUIRE(elapsed < std::chrono::milliseconds(50));
    REQUIRE(wave.strip().vertexCount() == 2000);
    cache.waitIdle();
    REQUIRE(peaks->complete());
    wave.build(*peaks, WaveView{.frameStart = 0.0,
                                .frameEnd = static_cast<double>(source->frames),
                                .columns = 1000});
    REQUIRE(wave.tier() == idealTier(static_cast<double>(source->frames) / 1000.0));
}

TEST_CASE("waveform_every_zoom_is_o_pixels", "[geometry]") {
    ClipPeakCache cache;
    const auto source = noise(48000ULL * 60ULL, true, 4);
    const auto peaks = cache.request(PeakKey{.path = "minute"}, source);
    cache.waitIdle();
    WaveformGeometry wave;
    for (double span = 64.0; span <= static_cast<double>(source->frames); span *= 4.0) {
        wave.build(*peaks,
                   WaveView{.frameStart = 1000.0, .frameEnd = 1000.0 + span, .columns = 800});
        REQUIRE(wave.strip().vertexCount() == 1600);
        REQUIRE(wave.tier() == idealTier(span / 800.0));
        // Every column's min is at or below its max.
        const auto floats = wave.strip().floats();
        for (std::size_t v = 0; v < floats.size(); v += 6) {
            REQUIRE(floats[v + 4] <= floats[v + 1]);
        }
    }
}

TEST_CASE("peak_cache_decodes_on_the_worker", "[geometry]") {
    ClipPeakCache cache;
    const auto peaks = cache.request(PeakKey{.path = "does/not/exist.wav"});
    cache.waitIdle();
    REQUIRE(peaks->failed());
    REQUIRE(peaks->complete());
    REQUIRE_FALSE(peaks->error().empty());
    REQUIRE(peaks->chooseTier(10.0) == -1);
}
