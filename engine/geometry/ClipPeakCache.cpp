#include "engine/geometry/ClipPeakCache.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "engine/format/audio/AudioFileLoader.h"

namespace adx::geometry {
namespace {

constexpr float kScale = 32767.0F;
constexpr std::uint32_t kSourceBit = 1U << 30U;
constexpr std::uint32_t kFailedBit = 1U << 31U;
constexpr std::uint32_t kAllTiers = (1U << static_cast<unsigned>(kPeakTiers)) - 1U;
/// Frames between checks of the cancel flag.
constexpr std::uint64_t kCancelStride = std::uint64_t{1} << 20U;

[[nodiscard]] std::uint64_t peaksIn(std::uint64_t frames, int tier) noexcept {
    const std::uint64_t width = samplesPerPeak(tier);
    return (frames + width - 1) / width;
}

} // namespace

int idealTier(double samplesPerColumn) noexcept {
    int tier = 0;
    while (tier + 1 < kPeakTiers &&
           static_cast<double>(samplesPerPeak(tier + 1)) <= samplesPerColumn) {
        ++tier;
    }
    return tier;
}

std::int16_t quantiseDown(float value) noexcept {
    const float scaled = std::floor(std::clamp(value, -1.0F, 1.0F) * kScale);
    return static_cast<std::int16_t>(scaled);
}

std::int16_t quantiseUp(float value) noexcept {
    const float scaled = std::ceil(std::clamp(value, -1.0F, 1.0F) * kScale);
    return static_cast<std::int16_t>(scaled);
}

// --- ClipPeaks ---------------------------------------------------------------

ClipPeaks::ClipPeaks(std::shared_ptr<const format::SampleBuffer> source, float gain)
    : m_source(std::move(source)), m_gain(gain) {
    m_frames = m_source->frames;
    m_sampleRate = m_source->sampleRate;
    // Tier 0 is the source itself, ready the moment it exists.
    m_ready.store(kSourceBit | 1U, std::memory_order_release);
    m_progress.store(1, std::memory_order_release);
}

ClipPeaks::ClipPeaks(std::string path, float gain) : m_path(std::move(path)), m_gain(gain) {}

bool ClipPeaks::sourceReady() const noexcept {
    return (m_ready.load(std::memory_order_acquire) & kSourceBit) != 0U;
}

bool ClipPeaks::tierReady(int tier) const noexcept {
    return tier >= 0 && tier < kPeakTiers &&
           (m_ready.load(std::memory_order_acquire) & (1U << static_cast<unsigned>(tier))) != 0U;
}

bool ClipPeaks::complete() const noexcept {
    const std::uint32_t ready = m_ready.load(std::memory_order_acquire);
    return (ready & kFailedBit) != 0U || (ready & kAllTiers) == kAllTiers;
}

bool ClipPeaks::failed() const noexcept {
    return (m_ready.load(std::memory_order_acquire) & kFailedBit) != 0U;
}

std::uint64_t ClipPeaks::frames() const noexcept {
    return sourceReady() ? m_frames : 0;
}

std::uint32_t ClipPeaks::sampleRate() const noexcept {
    return sourceReady() ? m_sampleRate : 0;
}

std::uint32_t ClipPeaks::progress() const noexcept {
    return m_progress.load(std::memory_order_acquire);
}

int ClipPeaks::chooseTier(double samplesPerColumn) const noexcept {
    const int ideal = idealTier(samplesPerColumn);
    if (tierReady(ideal)) {
        return ideal;
    }
    // The finest ready tier coarser than the ideal one: never finer, which would scan
    // more peaks per column than the budget allows.
    for (int tier = ideal + 1; tier < kPeakTiers; ++tier) {
        if (tierReady(tier)) {
            return tier;
        }
    }
    return -1;
}

std::pair<std::int16_t, std::int16_t> ClipPeaks::sourcePeak(std::uint64_t frame) const noexcept {
    const format::SampleView view = m_source->view();
    const float l = view.left[frame] * m_gain;
    const float r = view.right[frame] * m_gain;
    return {quantiseDown(std::min(l, r)), quantiseUp(std::max(l, r))};
}

std::pair<std::int16_t, std::int16_t> ClipPeaks::range(int tier, std::uint64_t begin,
                                                       std::uint64_t end) const noexcept {
    std::int16_t lo = std::numeric_limits<std::int16_t>::max();
    std::int16_t hi = std::numeric_limits<std::int16_t>::min();
    end = std::min(end, m_frames);
    if (begin >= end) {
        return {0, 0};
    }
    if (tier == 0) {
        for (std::uint64_t f = begin; f < end; ++f) {
            const auto [a, b] = sourcePeak(f);
            lo = std::min(lo, a);
            hi = std::max(hi, b);
        }
        return {lo, hi};
    }
    const std::uint64_t width = samplesPerPeak(tier);
    const auto& mins = m_min[static_cast<std::size_t>(tier)];
    const auto& maxs = m_max[static_cast<std::size_t>(tier)];
    const std::uint64_t last = std::min<std::uint64_t>((end + width - 1) / width, mins.size());
    for (std::uint64_t i = begin / width; i < last; ++i) {
        lo = std::min(lo, mins[i]);
        hi = std::max(hi, maxs[i]);
    }
    return {lo, hi};
}

void ClipPeaks::publish(int tier) noexcept {
    m_ready.fetch_or(1U << static_cast<unsigned>(tier), std::memory_order_acq_rel);
    m_progress.fetch_add(1, std::memory_order_acq_rel);
}

void ClipPeaks::allocateTiers() {
    for (int tier = 1; tier < kPeakTiers; ++tier) {
        const auto count = static_cast<std::size_t>(peaksIn(m_frames, tier));
        m_min[static_cast<std::size_t>(tier)].assign(count, 0);
        m_max[static_cast<std::size_t>(tier)].assign(count, 0);
    }
}

void ClipPeaks::compute(const std::atomic<bool>& cancel) {
    if (!m_source) {
        auto decoded = std::make_shared<format::SampleBuffer>();
        if (!format::decodeFile(m_path, *decoded, m_error)) {
            m_ready.fetch_or(kFailedBit, std::memory_order_acq_rel);
            m_progress.fetch_add(1, std::memory_order_acq_rel);
            return;
        }
        m_source = std::move(decoded);
        m_frames = m_source->frames;
        m_sampleRate = m_source->sampleRate;
        allocateTiers();
        m_ready.fetch_or(kSourceBit | 1U, std::memory_order_acq_rel);
        m_progress.fetch_add(1, std::memory_order_acq_rel);
    } else {
        allocateTiers();
    }
    // Coarsest first: one pass over the source gives a view something to draw at every
    // zoom, then the finest tier, from which the rest merge four peaks into one.
    scanTier(kPeakTiers - 1, cancel);
    scanTier(1, cancel);
    for (int tier = 2;
         tier < kPeakTiers - 1 && tierReady(tier - 1) && !cancel.load(std::memory_order_relaxed);
         ++tier) {
        mergeTier(tier);
        publish(tier);
    }
}

void ClipPeaks::scanTier(int tier, const std::atomic<bool>& cancel) {
    const std::uint64_t width = samplesPerPeak(tier);
    auto& mins = m_min[static_cast<std::size_t>(tier)];
    auto& maxs = m_max[static_cast<std::size_t>(tier)];
    for (std::size_t i = 0; i < mins.size(); ++i) {
        if ((i * width) % kCancelStride == 0 && cancel.load(std::memory_order_relaxed)) {
            return;
        }
        std::int16_t lo = std::numeric_limits<std::int16_t>::max();
        std::int16_t hi = std::numeric_limits<std::int16_t>::min();
        const std::uint64_t end = std::min(m_frames, (i + 1) * width);
        for (std::uint64_t f = i * width; f < end; ++f) {
            const auto [a, b] = sourcePeak(f);
            lo = std::min(lo, a);
            hi = std::max(hi, b);
        }
        mins[i] = lo;
        maxs[i] = hi;
    }
    publish(tier);
}

void ClipPeaks::mergeTier(int tier) {
    const auto& srcMin = m_min[static_cast<std::size_t>(tier - 1)];
    const auto& srcMax = m_max[static_cast<std::size_t>(tier - 1)];
    auto& mins = m_min[static_cast<std::size_t>(tier)];
    auto& maxs = m_max[static_cast<std::size_t>(tier)];
    for (std::size_t i = 0; i < mins.size(); ++i) {
        const std::size_t a = i * 4;
        const std::size_t b = std::min(srcMin.size(), a + 4);
        mins[i] = *std::min_element(srcMin.begin() + static_cast<std::ptrdiff_t>(a),
                                    srcMin.begin() + static_cast<std::ptrdiff_t>(b));
        maxs[i] = *std::max_element(srcMax.begin() + static_cast<std::ptrdiff_t>(a),
                                    srcMax.begin() + static_cast<std::ptrdiff_t>(b));
    }
}

// --- ClipPeakCache -----------------------------------------------------------

ClipPeakCache::ClipPeakCache() : m_worker([this] { run(); }) {}

ClipPeakCache::~ClipPeakCache() {
    {
        const std::scoped_lock lock(m_mutex);
        m_stop = true;
        m_cancel.store(true, std::memory_order_relaxed);
    }
    m_wake.notify_all();
    m_worker.join();
}

std::shared_ptr<const ClipPeaks>
ClipPeakCache::request(const PeakKey& key, std::shared_ptr<const format::SampleBuffer> source) {
    const format::SampleBuffer* raw = source.get();
    for (const Entry& entry : m_entries) {
        if (entry.key == key && entry.source == raw) {
            return entry.peaks;
        }
    }
    std::erase_if(m_entries, [&key](const Entry& entry) { return entry.key == key; });
    auto peaks = std::make_shared<ClipPeaks>(std::move(source), key.gain);
    m_entries.push_back(Entry{.key = key, .source = raw, .peaks = peaks});
    enqueue(peaks);
    return peaks;
}

std::shared_ptr<const ClipPeaks> ClipPeakCache::request(const PeakKey& key) {
    for (const Entry& entry : m_entries) {
        if (entry.key == key && entry.source == nullptr) {
            return entry.peaks;
        }
    }
    auto peaks = std::make_shared<ClipPeaks>(key.path, key.gain);
    m_entries.push_back(Entry{.key = key, .source = nullptr, .peaks = peaks});
    enqueue(peaks);
    return peaks;
}

void ClipPeakCache::clear() {
    m_entries.clear();
}

void ClipPeakCache::enqueue(std::shared_ptr<ClipPeaks> peaks) {
    {
        const std::scoped_lock lock(m_mutex);
        m_queue.push_back(std::move(peaks));
    }
    m_wake.notify_one();
}

void ClipPeakCache::waitIdle() {
    std::unique_lock lock(m_mutex);
    m_idle.wait(lock, [this] { return m_queue.empty() && !m_busy; });
}

void ClipPeakCache::run() {
    for (;;) {
        std::shared_ptr<ClipPeaks> job;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
            if (m_stop) {
                return;
            }
            job = std::move(m_queue.front());
            m_queue.pop_front();
            m_busy = true;
        }
        job->compute(m_cancel);
        {
            const std::scoped_lock lock(m_mutex);
            m_busy = false;
        }
        m_idle.notify_all();
    }
}

} // namespace adx::geometry
