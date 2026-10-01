// adx-thread: realtime
#include "engine/audio/CallbackCore.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "engine/core/Config.h"
#include "engine/rt/Denormal.h"
#include "engine/rt/RtSection.h"

namespace adx::audio {

void CallbackCore::setProcessStep(ProcessStep step, void* user) noexcept {
    m_processStep = step != nullptr ? step : &CallbackCore::noProcess;
    m_processUser = user;
}

void CallbackCore::setDeadline(std::uint32_t sampleRate, std::uint32_t blockFrames) noexcept {
    m_deadlineNs = sampleRate == 0
                       ? 0
                       : (static_cast<std::uint64_t>(blockFrames) * 1'000'000'000ULL) / sampleRate;
}

void CallbackCore::noProcess(void* /*user*/, float* /*out*/, const float* /*in*/,
                             std::uint32_t /*frames*/, std::uint32_t /*channels*/,
                             const StreamTime& /*time*/, rt::BlockArena& /*arena*/) noexcept {}

void CallbackCore::renderTrampoline(void* user, float* out, const float* in, std::uint32_t frames,
                                    const StreamTime& time) noexcept {
    static_cast<CallbackCore*>(user)->render(out, in, frames, time);
}

void CallbackCore::render(float* out, const float* in, std::uint32_t frames,
                          const StreamTime& time) noexcept {
    // steady_clock is QueryPerformanceCounter on Windows: a register read, no lock, no
    // allocation. It measures; it never feeds the audio, so it cannot cost the
    // offline-equals-realtime guarantee anything (phase_3.md §4.10 condition 6).
    const auto startedAt = std::chrono::steady_clock::now();
    {
        // The two guards, in the one place they are installed.
        const rt::ScopedRtSection rtSection;
        const rt::ScopedFlushDenormals denormals;

        ADX_ASSERT(frames <= rt::kMaxBlockFrames);
        if (frames > rt::kMaxBlockFrames) {
            // The assertion records and returns rather than aborting inside a callback,
            // so the overflow still has to be handled: render nothing rather than write
            // past the end of the caller's buffer.
            return;
        }

        m_arena.reset();

        const std::size_t sampleCount = static_cast<std::size_t>(frames) * m_outputChannels;
        // Cleared before the work, not after: a graph that does not fill every channel
        // then leaves silence rather than whatever the device buffer last held.
        std::fill_n(out, sampleCount, 0.0F);
        m_processStep(m_processUser, out, in, frames, m_outputChannels, time, m_arena);

        writeTaps(out, frames);
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - startedAt)
                             .count();
    const auto durationNs = static_cast<std::uint64_t>(elapsed < 0 ? 0 : elapsed);
    m_timingTap.write(CallbackTiming{.durationNs = static_cast<std::uint32_t>(
                                         std::min<std::uint64_t>(durationNs, 0xFFFFFFFFULL)),
                                     .frames = frames});
    if (durationNs > m_worstNs.load(std::memory_order_relaxed)) {
        m_worstNs.store(durationNs, std::memory_order_release);
    }
    if (m_deadlineNs != 0 && durationNs > m_deadlineNs) {
        m_overDeadline.fetch_add(1, std::memory_order_relaxed);
    }
}

void CallbackCore::writeTaps(const float* out, std::uint32_t frames) noexcept {
    // The taps run even over silence, so the meters and the scope are exercised on
    // every callback rather than first being wired up in Phase 6 - if writing them
    // were ever going to allocate, the gate would catch it.
    float peakLeft = 0.0F;
    float peakRight = 0.0F;
    double sumSquaresLeft = 0.0;
    double sumSquaresRight = 0.0;

    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        const std::size_t base = static_cast<std::size_t>(frame) * m_outputChannels;
        const float left = out[base];
        const float right = m_outputChannels > 1 ? out[base + 1] : left;

        m_scopeTap.write(rt::StereoFrame{.left = left, .right = right});

        peakLeft = std::max(peakLeft, std::abs(left));
        peakRight = std::max(peakRight, std::abs(right));
        sumSquaresLeft += static_cast<double>(left) * left;
        sumSquaresRight += static_cast<double>(right) * right;
    }

    const double inverseFrames = frames > 0 ? 1.0 / static_cast<double>(frames) : 0.0;
    m_levelTap.write(
        rt::LevelFrame{.peakLeft = peakLeft,
                       .peakRight = peakRight,
                       .rmsLeft = static_cast<float>(std::sqrt(sumSquaresLeft * inverseFrames)),
                       .rmsRight = static_cast<float>(std::sqrt(sumSquaresRight * inverseFrames))});
}

} // namespace adx::audio
