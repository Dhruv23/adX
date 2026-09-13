#pragma once

#include <cstddef>
#include <vector>

// UI-Refactor Phase 5: keeps the last `maxFrames` frames of log-magnitude
// bins (from SimpleFFT::ComputeLogMagnitudeSpectrum) so the WATERFALL panel
// can draw a receding layered-line spectrogram. UI-Thread-only. A plain
// fixed-capacity ring buffer -- the caller controls how often frames land
// (main.cpp pushes only every few rendered frames) so the retained history
// spans a few seconds rather than a few dozen milliseconds.
class SpectrogramHistory {
public:
    explicit SpectrogramHistory(size_t maxFrames = 30) : m_maxFrames(maxFrames), m_frames(maxFrames) {}

    // Pushes one frame of bins, evicting the oldest once at capacity.
    // Reuses each slot's existing std::vector capacity after warm-up, so
    // steady-state pushes don't allocate.
    void Push(const std::vector<float>& bins) {
        if (m_maxFrames == 0) return;
        m_frames[m_writeIndex] = bins;
        m_writeIndex = (m_writeIndex + 1) % m_maxFrames;
        if (m_count < m_maxFrames) m_count++;
    }

    size_t size() const { return m_count; }

    // ageFromNewest: 0 = most recently pushed frame, size()-1 = oldest
    // still retained. Caller must ensure ageFromNewest < size().
    const std::vector<float>& Get(size_t ageFromNewest) const {
        size_t idx = (m_writeIndex + m_maxFrames - 1 - ageFromNewest) % m_maxFrames;
        return m_frames[idx];
    }

private:
    size_t m_maxFrames;
    std::vector<std::vector<float>> m_frames;
    size_t m_writeIndex = 0;
    size_t m_count = 0;
};
