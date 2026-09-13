// Flush-to-zero and denormals-are-zero for the duration of the audio callback.
//
// A reverb or filter tail decaying into denormal range costs on the order of 100x
// per operation on some microarchitectures, and it shows up as a dropout under
// exactly the conditions where a dropout is least acceptable - a quiet passage after
// a loud one. Iteration one had no denormal handling at all.
//
// FTZ/DAZ change results, so this interacts with the bit-identical
// offline-vs-realtime guarantee (phase_0.md §4.1): it must be applied *identically*
// in both. That is why OfflineBackend installs it too, and why it is an RAII type
// installed at one place - AudioThread::render - rather than a process-wide setting
// somebody can forget to match.
#pragma once

#include <cstdint>

#include "engine/rt/RtConfig.h"

#if defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)
#    define ADX_HAS_SSE_DENORMAL_CONTROL 1
#    include <xmmintrin.h>
#    if defined(_MSC_VER)
#        include <pmmintrin.h>
#    endif
#else
#    define ADX_HAS_SSE_DENORMAL_CONTROL 0
#endif

namespace adx::rt {

/// Sets FTZ and DAZ on construction and restores the previous MXCSR on destruction.
///
/// The whole control word is saved and restored rather than just the two bits: the
/// rounding mode and exception mask belong to whoever set them, and a host that
/// eventually loads adX as nothing (adX is never a plugin - FINAL_PLAN non-goals)
/// still shares the thread with RtAudio's own code.
class ScopedFlushDenormals {
public:
    ScopedFlushDenormals() noexcept {
#if ADX_HAS_SSE_DENORMAL_CONTROL
        m_savedCsr = _mm_getcsr();
        _mm_setcsr(m_savedCsr | _MM_FLUSH_ZERO_ON | _MM_DENORMALS_ZERO_ON);
#endif
    }

    ~ScopedFlushDenormals() noexcept {
#if ADX_HAS_SSE_DENORMAL_CONTROL
        _mm_setcsr(m_savedCsr);
#endif
    }

    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals(ScopedFlushDenormals&&) = delete;
    ScopedFlushDenormals& operator=(ScopedFlushDenormals&&) = delete;

    /// The control word as it was before this guard. Exposed so a test can assert
    /// the bits were actually changed and actually restored.
    [[nodiscard]] std::uint32_t savedControlWord() const noexcept {
        return m_savedCsr;
    }

private:
    std::uint32_t m_savedCsr{0};
};

/// True when denormals are currently flushed on this thread. For tests.
[[nodiscard]] inline bool denormalsAreFlushed() noexcept {
#if ADX_HAS_SSE_DENORMAL_CONTROL
    const std::uint32_t csr = _mm_getcsr();
    return (csr & _MM_FLUSH_ZERO_MASK) == _MM_FLUSH_ZERO_ON &&
           (csr & _MM_DENORMALS_ZERO_MASK) == _MM_DENORMALS_ZERO_ON;
#else
    return false;
#endif
}

} // namespace adx::rt
