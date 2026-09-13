// What happens when realtime code breaks Rule 1.
//
// A violation cannot throw (RT code never throws), cannot print (that is a
// violation itself), and cannot allocate. So it records: an atomic increment and a
// store into a fixed array, and nothing else. The test suite reads the log
// afterwards and fails on a non-zero count.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

#include "engine/rt/RtConfig.h"

#if defined(_MSC_VER)
#    include <intrin.h>
#endif

/// The address in the function that called the one expanding this.
///
/// Used at the top of the allocator hook, where it names the code that allocated
/// rather than the hook itself - which is the whole difference between "something
/// allocated" and "Scheduler::dispatch allocated". It has to be a macro for that
/// reason: wrapped in a function, it would report the wrapper's caller.
///
/// One frame, not the four ViolationRecord has room for. Walking further needs a
/// platform backtrace API, and FINAL_PLAN's non-goals keep Win32 out of the engine
/// entirely - so the extra slots stay reserved and returnAddrCount stays 1.
#if defined(_MSC_VER)
#    define ADX_RETURN_ADDRESS() _ReturnAddress()
#elif defined(__GNUC__) || defined(__clang__)
#    define ADX_RETURN_ADDRESS() __builtin_return_address(0)
#else
#    define ADX_RETURN_ADDRESS() nullptr
#endif

namespace adx::rt {

enum class ViolationKind : std::uint8_t {
    Allocation,
    Deallocation,
    Lock,
    Throw,
    Blocking,
    Unbounded,
    Assert,
};

/// Number of distinct kinds, for the per-kind counter array.
inline constexpr std::size_t kViolationKindCount = 7;

[[nodiscard]] const char* toString(ViolationKind kind) noexcept;

/// One recorded violation. Trivially copyable and exactly one cache line, because
/// recording one is a single store on the audio thread.
struct ViolationRecord {
    ViolationKind kind{ViolationKind::Allocation};
    std::array<std::uint8_t, 3> reserved{};
    /// Bytes requested, for Allocation and Deallocation. Zero otherwise.
    std::uint32_t sizeBytes{};
    /// Order this violation was recorded in, across all kinds. Filled in by
    /// record(); callers leave it zero.
    std::uint64_t sequence{};
    /// A string literal, never owned and never freed.
    const char* file{nullptr};
    std::uint32_t line{};
    std::uint32_t returnAddrCount{};
    std::array<void*, 4> returnAddrs{};
};

static_assert(std::is_trivially_copyable_v<ViolationRecord>);
static_assert(sizeof(ViolationRecord) == 64, "one record is one cache line");

/// The process-wide violation log.
///
/// Fixed capacity, so recording never allocates and never grows. The counter is
/// exact; the stored records are the most recent kCapacity of them.
class ViolationLog {
public:
    static constexpr std::size_t kCapacity = 256;

    [[nodiscard]] static ViolationLog& instance() noexcept;

    /// Records a violation. Safe to call from the audio thread, from inside
    /// operator new, and from a signal-handler-like context: one relaxed
    /// fetch_add, one store, one more fetch_add.
    void record(ViolationRecord violation) noexcept;

    /// Total violations since the last reset, of every kind.
    [[nodiscard]] std::uint64_t count() const noexcept;

    /// Violations of one kind. This is what most tests assert on - a test for the
    /// lock check should not pass merely because something allocated.
    [[nodiscard]] std::uint64_t count(ViolationKind kind) const noexcept;

    /// Clears counters and records. Main thread only, between tests. Not safe
    /// against a concurrently running audio thread, and not meant to be.
    void reset() noexcept;

    /// The stored records, oldest first, up to kCapacity.
    ///
    /// A record is written as a plain 64-byte store while a reader may be copying
    /// it, so a snapshot taken while the audio thread is running can contain one
    /// torn entry. Acceptable for a diagnostic: count() is exact, and tests read
    /// the records only after stopping the stream.
    [[nodiscard]] std::span<const ViolationRecord> snapshot() const noexcept;

private:
    ViolationLog() noexcept = default;
    ~ViolationLog() = default;

public:
    ViolationLog(const ViolationLog&) = delete;
    ViolationLog& operator=(const ViolationLog&) = delete;
    ViolationLog(ViolationLog&&) = delete;
    ViolationLog& operator=(ViolationLog&&) = delete;

private:
    std::atomic<std::uint64_t> m_count{0};
    std::array<std::atomic<std::uint64_t>, kViolationKindCount> m_countByKind{};
    std::array<ViolationRecord, kCapacity> m_slots{};
};

/// Convenience for the common case: record `kind` with the caller's location.
///
/// Deliberately a macro so __FILE__ and __LINE__ name the violating line rather
/// than this header.
#define ADX_RECORD_VIOLATION(kindValue, bytes)                                                     \
    ::adx::rt::ViolationLog::instance().record(                                                    \
        ::adx::rt::ViolationRecord{.kind = (kindValue),                                            \
                                   .sizeBytes = (bytes),                                           \
                                   .file = __FILE__,                                               \
                                   .line = __LINE__,                                               \
                                   .returnAddrCount = 1,                                           \
                                   .returnAddrs = {ADX_RETURN_ADDRESS()}})

/// Routes failed ADX_ASSERTs inside an RT section into the log instead of calling
/// abort(), and leaves the aborting behaviour in place everywhere else.
///
/// This is the promise phase_0.md §4.3 made when it insisted ADX_ASSERT must not be
/// plain assert(): an assertion that fires mid-buffer should fail the test suite,
/// not kill the process holding the audio device. Installed by AudioThread; call it
/// directly in a test that asserts on assertion behaviour.
void installRtAssertHandler() noexcept;

} // namespace adx::rt
