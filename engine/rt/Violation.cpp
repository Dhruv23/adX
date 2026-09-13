#include "engine/rt/Violation.h"

#include "engine/core/Config.h"
#include "engine/rt/RtSection.h"

namespace adx::rt {

const char* toString(ViolationKind kind) noexcept {
    switch (kind) {
    case ViolationKind::Allocation:
        return "allocation";
    case ViolationKind::Deallocation:
        return "deallocation";
    case ViolationKind::Lock:
        return "lock";
    case ViolationKind::Throw:
        return "throw";
    case ViolationKind::Blocking:
        return "blocking call";
    case ViolationKind::Unbounded:
        return "unbounded work";
    case ViolationKind::Assert:
        return "failed assertion";
    }
    return "unknown";
}

ViolationLog& ViolationLog::instance() noexcept {
    // Function-local static rather than a namespace-scope object: it must be
    // constructed before the first allocation the process makes, because operator
    // new records into it, and a namespace-scope object's construction order
    // relative to other translation units is not something to bet the allocator
    // hook on. The type is trivially constructible, so there is no lock here in
    // practice - MSVC emits a one-time initialisation check, not a mutex.
    static ViolationLog log;
    return log;
}

void ViolationLog::record(ViolationRecord violation) noexcept {
    const std::uint64_t sequence = m_count.fetch_add(1, std::memory_order_relaxed);
    violation.sequence = sequence;

    // & (kCapacity - 1) rather than %: kCapacity is a power of two, and this runs
    // on the audio thread inside operator new.
    m_slots[sequence & (kCapacity - 1)] = violation;

    const auto kindIndex = static_cast<std::size_t>(violation.kind);
    if (kindIndex < kViolationKindCount) {
        m_countByKind[kindIndex].fetch_add(1, std::memory_order_relaxed);
    }
}

std::uint64_t ViolationLog::count() const noexcept {
    return m_count.load(std::memory_order_acquire);
}

std::uint64_t ViolationLog::count(ViolationKind kind) const noexcept {
    const auto kindIndex = static_cast<std::size_t>(kind);
    if (kindIndex >= kViolationKindCount) {
        return 0;
    }
    return m_countByKind[kindIndex].load(std::memory_order_acquire);
}

void ViolationLog::reset() noexcept {
    for (auto& perKind : m_countByKind) {
        perKind.store(0, std::memory_order_relaxed);
    }
    m_slots = {};
    m_count.store(0, std::memory_order_release);
}

std::span<const ViolationRecord> ViolationLog::snapshot() const noexcept {
    const std::uint64_t total = count();
    const auto stored = static_cast<std::size_t>(total < kCapacity ? total : kCapacity);
    return std::span<const ViolationRecord>{m_slots.data(), stored};
}

namespace {

void recordOrAbortOnAssert(const char* expression, const char* file, int line) noexcept {
    if (!inRtSection()) {
        core::abortOnAssertFailure(expression, file, line);
    }

    // Inside an audio callback. Record and return: aborting here would take down
    // the process mid-buffer, and the test suite cannot report on a process that
    // is gone. ADX_RECORD_VIOLATION is not used because the useful location is the
    // assertion's, which was passed in, not this function's.
    ViolationLog::instance().record(ViolationRecord{.kind = ViolationKind::Assert,
                                                    .file = file,
                                                    .line = static_cast<std::uint32_t>(line),
                                                    .returnAddrCount = 1,
                                                    .returnAddrs = {ADX_RETURN_ADDRESS()}});
}

} // namespace

void installRtAssertHandler() noexcept {
    core::setAssertHandler(&recordOrAbortOnAssert);
}

} // namespace adx::rt
