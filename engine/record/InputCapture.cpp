// adx-thread: realtime
#include "engine/record/InputCapture.h"

#include <algorithm>

namespace adx::record {

void InputCapture::capture(const float* in, std::uint32_t frames, std::uint32_t channels,
                           std::int64_t position, bool rolling) noexcept {
    if (!rolling || !m_armed.load(std::memory_order_acquire) || overran()) {
        return;
    }
    // Room for the whole block, or none of it: a block split by a full ring would be
    // a take with a hole in it. From this side the free space only grows while we look,
    // so the check is conservative, never wrong.
    const std::size_t samples = static_cast<std::size_t>(frames) * 2;
    if (kCaptureRingSamples - m_ring.sizeApprox() < samples) {
        m_overruns.fetch_add(samples, std::memory_order_acq_rel);
        return;
    }
    if (!m_started.load(std::memory_order_relaxed)) {
        m_firstFrameAt.store(position, std::memory_order_relaxed);
        m_started.store(true, std::memory_order_release);
    }
    for (std::uint32_t f = 0; f < frames; ++f) {
        float left = 0.0F;
        float right = 0.0F;
        if (in != nullptr && channels > 0) {
            left = in[static_cast<std::size_t>(f) * channels];
            right = channels > 1 ? in[(static_cast<std::size_t>(f) * channels) + 1] : left;
        }
        static_cast<void>(m_ring.tryPush(left));
        static_cast<void>(m_ring.tryPush(right));
    }
    m_frames.fetch_add(frames, std::memory_order_acq_rel);
}

std::size_t InputCapture::drain(float* out, std::size_t max) noexcept {
    // Whole frames only. The audio thread pushes a frame's left and right one at a
    // time, so popping whatever is there can catch it between the two - and an odd
    // count leaves every later drain a sample out, left and right swapped for the rest
    // of the take. From this side the size only grows while we look, so rounding it
    // down to a frame is never more than is there.
    const std::size_t take = std::min(max, m_ring.sizeApprox()) & ~std::size_t{1};
    std::size_t taken = 0;
    while (taken < take && m_ring.tryPop(out[taken])) {
        ++taken;
    }
    return taken;
}

} // namespace adx::record
