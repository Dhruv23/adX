// adx-thread: main
//
// Mipmapped min/max peaks for audio clips (phase_5.md §4.5).
//
// PORTED from _archive/src-cpp/src/ClipPeakCache.cpp, whose structure FINAL_PLAN §3.1
// credits: tiered peaks, keyed on what changes a clip's shape. What changed on port:
//
//   - the key gains `sourceOffset` and `gain` (Phase 8's slip editing and clip gain -
//     a stale cache there is a visibly wrong waveform);
//   - tiers are powers of 4, from 1 to 65,536 samples per peak, so every zoom level has
//     a tier within 4x of it. Tier 0 *is* the source - one sample per peak - and reads
//     it rather than copying it; the archive's tiers doubled from an adaptive base;
//   - peaks are computed on a worker thread, coarsest tier first, so a view draws the
//     coarse tier while the fine ones are still coming and opening a project never
//     blocks on analysis. A path-only request decodes on the worker too;
//   - peaks are stored as int16, rounded outward (min down, max up): a 10-minute stereo
//     file costs ~38 MB of peaks instead of ~77 MB as floats, and the drawn envelope never
//     under-reaches the audio.
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "engine/format/audio/SampleBuffer.h"

namespace adx::geometry {

/// What a clip's waveform shape depends on. Two clips with equal keys share peaks.
struct PeakKey {
    std::string path;
    float pitchSemitones{0.0F};
    float stretch{1.0F};
    bool reversed{false};
    std::int64_t sourceOffset{0};
    float gain{1.0F};

    [[nodiscard]] friend bool operator==(const PeakKey&, const PeakKey&) = default;
};

inline constexpr int kPeakTiers = 9;

/// Samples one peak of `tier` spans: 4^tier, so 1 .. 65,536.
[[nodiscard]] constexpr std::uint64_t samplesPerPeak(int tier) noexcept {
    return std::uint64_t{1} << (2U * static_cast<unsigned>(tier));
}

/// The tier a view showing `samplesPerColumn` source samples per pixel column should
/// read: the coarsest whose peaks are no wider than a column, so it is within 4x.
[[nodiscard]] int idealTier(double samplesPerColumn) noexcept;

/// Quantisation to the stored int16 scale, rounded outward. Public so tests can build
/// the brute-force reference the same way.
[[nodiscard]] std::int16_t quantiseDown(float value) noexcept;
[[nodiscard]] std::int16_t quantiseUp(float value) noexcept;

class ClipPeaks {
public:
    /// Over a buffer that is already decoded.
    ClipPeaks(std::shared_ptr<const format::SampleBuffer> source, float gain);
    /// Over a file the worker will decode first.
    ClipPeaks(std::string path, float gain);

    /// Whether the source has been decoded; frames() and sampleRate() are 0 until then.
    [[nodiscard]] bool sourceReady() const noexcept;
    [[nodiscard]] bool tierReady(int tier) const noexcept;
    /// Every tier is ready (or decoding failed: see error()).
    [[nodiscard]] bool complete() const noexcept;
    [[nodiscard]] bool failed() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept {
        return m_error;
    }
    [[nodiscard]] std::uint64_t frames() const noexcept;
    [[nodiscard]] std::uint32_t sampleRate() const noexcept;
    /// A count that changes whenever a tier becomes ready; a view polls it.
    [[nodiscard]] std::uint32_t progress() const noexcept;

    /// idealTier(), or the coarsest ready tier while that one is still being computed.
    /// -1 when nothing is ready.
    [[nodiscard]] int chooseTier(double samplesPerColumn) const noexcept;

    /// Quantised min and max over source frames [begin, end) read from `tier`, both
    /// channels folded together. The tier must be ready.
    [[nodiscard]] std::pair<std::int16_t, std::int16_t> range(int tier, std::uint64_t begin,
                                                              std::uint64_t end) const noexcept;

    /// Worker side: decode if needed, then fill the tiers coarsest first. `cancel` is
    /// checked between blocks.
    void compute(const std::atomic<bool>& cancel);

private:
    void allocateTiers();
    void scanTier(int tier, const std::atomic<bool>& cancel);
    void mergeTier(int tier);
    void publish(int tier) noexcept;
    [[nodiscard]] std::pair<std::int16_t, std::int16_t>
    sourcePeak(std::uint64_t frame) const noexcept;

    std::shared_ptr<const format::SampleBuffer> m_source;
    std::string m_path;
    float m_gain;
    std::string m_error;
    std::uint64_t m_frames{0};
    std::uint32_t m_sampleRate{0};
    /// Index 0 is unused: tier 0 reads the source.
    std::array<std::vector<std::int16_t>, kPeakTiers> m_min;
    std::array<std::vector<std::int16_t>, kPeakTiers> m_max;
    /// Bit t: tier t is ready. Bit 30: source decoded. Bit 31: decoding failed.
    std::atomic<std::uint32_t> m_ready{0};
    std::atomic<std::uint32_t> m_progress{0};
};

class ClipPeakCache {
public:
    ClipPeakCache();
    ~ClipPeakCache();
    ClipPeakCache(const ClipPeakCache&) = delete;
    ClipPeakCache& operator=(const ClipPeakCache&) = delete;
    ClipPeakCache(ClipPeakCache&&) = delete;
    ClipPeakCache& operator=(ClipPeakCache&&) = delete;

    /// The peaks for `key` over `source`, queued for the worker if this is the first
    /// request - or if `source` is not the buffer the cached peaks were built from,
    /// which is how a clip re-decoded in place is caught (the archive's sourcePtr).
    /// Never waits for the computation.
    std::shared_ptr<const ClipPeaks> request(const PeakKey& key,
                                             std::shared_ptr<const format::SampleBuffer> source);
    /// The same, decoding key.path on the worker.
    std::shared_ptr<const ClipPeaks> request(const PeakKey& key);

    [[nodiscard]] std::size_t size() const noexcept {
        return m_entries.size();
    }
    void clear();
    /// Blocks until the worker has nothing queued or running. For tests.
    void waitIdle();

private:
    struct Entry {
        PeakKey key;
        const format::SampleBuffer* source{nullptr};
        std::shared_ptr<ClipPeaks> peaks;
    };

    void enqueue(std::shared_ptr<ClipPeaks> peaks);
    void run();

    std::vector<Entry> m_entries;

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::condition_variable m_idle;
    std::deque<std::shared_ptr<ClipPeaks>> m_queue;
    bool m_busy{false};
    bool m_stop{false};
    std::atomic<bool> m_cancel{false};
    std::thread m_worker;
};

} // namespace adx::geometry
