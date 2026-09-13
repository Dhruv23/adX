#include "ClipPeakCache.h"
#include <unordered_map>
#include <string>
#include <algorithm>

namespace {

struct CacheEntry {
    ClipPeaks peaks;
    const std::vector<float>* sourcePtr = nullptr; // detects ReprocessClip re-decoding in place
};

std::string MakeKey(const AudioClip& clip) {
    std::string key;
    key.reserve(clip.filePath.size() + 32);
    key += clip.filePath;
    key += '|';
    key += std::to_string(clip.pitchShiftSemitones);
    key += '|';
    key += std::to_string(clip.timeStretchFactor);
    key += '|';
    key += clip.reversed ? '1' : '0';
    return key;
}

ClipPeaks BuildPeaks(const AudioClip& clip) {
    ClipPeaks peaks;
    const auto& pcm = *clip.pcmData;
    unsigned int channels = std::max(1u, clip.channels);
    size_t frameCount = pcm.size() / channels;
    peaks.frameCount = frameCount;
    if (frameCount == 0) return peaks;

    // Tier 0: aim for a few thousand buckets across the whole clip, so even
    // a multi-minute clip builds/scans quickly while a fully zoomed-in view
    // still reads as a real waveform rather than blocky steps.
    constexpr size_t kTargetBaseBuckets = 4096;
    int samplesPerBucket = static_cast<int>(
        std::max<size_t>(1, (frameCount + kTargetBaseBuckets - 1) / kTargetBaseBuckets));

    ClipPeaks::Tier base;
    base.samplesPerBucket = samplesPerBucket;
    size_t baseBucketCount = (frameCount + samplesPerBucket - 1) / static_cast<size_t>(samplesPerBucket);
    base.mins.resize(baseBucketCount);
    base.maxs.resize(baseBucketCount);
    for (size_t b = 0; b < baseBucketCount; ++b) {
        size_t frameStart = b * static_cast<size_t>(samplesPerBucket);
        size_t frameEnd = std::min(frameCount, frameStart + static_cast<size_t>(samplesPerBucket));
        float mn = 1.0f, mx = -1.0f;
        for (size_t f = frameStart; f < frameEnd; ++f) {
            for (unsigned int c = 0; c < channels; ++c) {
                float s = pcm[f * channels + c];
                mn = std::min(mn, s);
                mx = std::max(mx, s);
            }
        }
        base.mins[b] = mn;
        base.maxs[b] = mx;
    }
    peaks.tiers.push_back(std::move(base));

    // Coarser mip tiers: merge adjacent bucket pairs (min-of-mins/max-of-
    // maxs) until further halving would leave only a handful of buckets —
    // enough zoom-out levels that the renderer is never scanning more than
    // a couple of buckets per on-screen pixel column at any zoom.
    while (peaks.tiers.back().mins.size() > 8 && peaks.tiers.size() < 12) {
        const ClipPeaks::Tier& prev = peaks.tiers.back();
        ClipPeaks::Tier next;
        next.samplesPerBucket = prev.samplesPerBucket * 2;
        size_t nextCount = (prev.mins.size() + 1) / 2;
        next.mins.resize(nextCount);
        next.maxs.resize(nextCount);
        for (size_t b = 0; b < nextCount; ++b) {
            size_t a0 = b * 2;
            size_t a1 = std::min(prev.mins.size() - 1, a0 + 1);
            next.mins[b] = std::min(prev.mins[a0], prev.mins[a1]);
            next.maxs[b] = std::max(prev.maxs[a0], prev.maxs[a1]);
        }
        peaks.tiers.push_back(std::move(next));
    }

    return peaks;
}

std::unordered_map<std::string, CacheEntry> g_cache;

} // namespace

const ClipPeaks* GetOrBuildClipPeaks(const AudioClip& clip) {
    if (!clip.pcmData || clip.pcmData->empty()) return nullptr;

    std::string key = MakeKey(clip);
    auto it = g_cache.find(key);
    if (it != g_cache.end() && it->second.sourcePtr == clip.pcmData.get()) {
        return &it->second.peaks;
    }

    CacheEntry& stored = g_cache[key];
    stored.peaks = BuildPeaks(clip);
    stored.sourcePtr = clip.pcmData.get();
    return &stored.peaks;
}

void ClearClipPeakCache() {
    g_cache.clear();
}
