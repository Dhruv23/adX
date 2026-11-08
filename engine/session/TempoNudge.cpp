// adx-thread: realtime
#include "engine/session/TempoNudge.h"

#include <algorithm>
#include <cmath>

namespace adx::session {

double TapTempo::tap(double seconds) noexcept {
    if (m_count > 0 && seconds - m_times.at((m_count - 1) % kTaps) > 2.0) {
        m_count = 0; // a pause: a new count
    }
    m_times.at(m_count % kTaps) = seconds;
    ++m_count;
    const std::size_t taps = std::min(m_count, kTaps);
    if (taps < 2) {
        return 0.0;
    }
    // The intervals among the kept taps, oldest first.
    std::array<double, kTaps - 1> intervals{};
    const std::size_t first = m_count - taps;
    for (std::size_t i = 0; i + 1 < taps; ++i) {
        intervals.at(i) = m_times.at((first + i + 1) % kTaps) - m_times.at((first + i) % kTaps);
    }
    const std::size_t n = taps - 1;
    // The median, then the mean of the intervals within 15 % of it: one bad tap is out.
    std::array<double, kTaps - 1> sorted = intervals;
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(n));
    const double median =
        (n % 2 == 1) ? sorted.at(n / 2) : 0.5 * (sorted.at((n / 2) - 1) + sorted.at(n / 2));
    double sum = 0.0;
    std::size_t kept = 0;
    for (std::size_t i = 0; i < n; ++i) {
        if (std::abs(intervals.at(i) - median) <= 0.15 * median) {
            sum += intervals.at(i);
            ++kept;
        }
    }
    const double interval = kept > 0 ? sum / static_cast<double>(kept) : median;
    return interval > 0.0 ? 60.0 / interval : 0.0;
}

} // namespace adx::session
