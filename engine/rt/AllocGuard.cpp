#include "engine/rt/AllocGuard.h"

// NOLINTBEGIN(portability-restrict-system-includes)
// The realtime ban list bans <new> and <cstdlib> because realtime code must not
// allocate. This is the file that implements allocation, so it is the one place those
// headers belong - and the ban being in force everywhere else is what makes that
// true rather than merely claimed.
#include <cstdlib>
#include <new>
// NOLINTEND(portability-restrict-system-includes)

#if defined(_MSC_VER)
#    include <malloc.h>
#endif

#include "engine/rt/RtSection.h"
#include "engine/rt/Violation.h"

#if ADX_ENABLE_RT_GUARD

// NOLINTBEGIN(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
// The realtime ban list forbids malloc, free and raw ownership under engine/rt. This
// file is the allocator: forbidding them here would be forbidding it from existing.
// The exemption covers this file and no other, which is why it is a bounded region
// rather than a widened rule.

namespace {

/// Records an allocation of `bytes` if the calling thread is mid-callback.
///
/// `returnAddress` is captured by the caller, not here: taken inside this function it
/// would name the replacement operator rather than the code that allocated, and
/// "Scheduler::dispatch allocated" is the only version of this diagnostic that is
/// worth anything.
void noteIfRealtime(adx::rt::ViolationKind kind, std::size_t bytes, void* returnAddress) noexcept {
    if (!adx::rt::inRtSection()) {
        return;
    }
    adx::rt::ViolationLog::instance().record(
        adx::rt::ViolationRecord{.kind = kind,
                                 .sizeBytes = static_cast<std::uint32_t>(bytes),
                                 .file = __FILE__,
                                 .line = __LINE__,
                                 .returnAddrCount = 1,
                                 .returnAddrs = {returnAddress}});
}

void* allocateBytes(std::size_t bytes) noexcept {
    // Zero-size allocations must still return a distinct pointer.
    return std::malloc(bytes == 0 ? 1 : bytes);
}

void* allocateAligned(std::size_t bytes, std::size_t alignment) noexcept {
    const std::size_t request = bytes == 0 ? alignment : bytes;
#    if defined(_MSC_VER)
    return _aligned_malloc(request, alignment);
#    else
    // aligned_alloc requires a size that is a multiple of the alignment.
    const std::size_t rounded = ((request + alignment - 1) / alignment) * alignment;
    return std::aligned_alloc(alignment, rounded);
#    endif
}

void freeAligned(void* ptr) noexcept {
#    if defined(_MSC_VER)
    _aligned_free(ptr);
#    else
    std::free(ptr);
#    endif
}

} // namespace

// ---------------------------------------------------------------------------------
// All eight replaceable allocation forms, and the matching deallocations.
//
// Every one of them, because a missing form is a hole in the coverage that looks
// exactly like safety: an over-aligned type or a nothrow new would route straight
// past the hook and the suite would stay green. Aligned allocations use the aligned
// CRT entry points and must be released through the aligned deletes - mixing
// free() with _aligned_malloc is undefined behaviour.
// ---------------------------------------------------------------------------------

void* operator new(std::size_t bytes) {
    noteIfRealtime(adx::rt::ViolationKind::Allocation, bytes, ADX_RETURN_ADDRESS());
    void* ptr = allocateBytes(bytes);
    if (ptr == nullptr) {
        throw std::bad_alloc{};
    }
    return ptr;
}

void* operator new[](std::size_t bytes) {
    noteIfRealtime(adx::rt::ViolationKind::Allocation, bytes, ADX_RETURN_ADDRESS());
    void* ptr = allocateBytes(bytes);
    if (ptr == nullptr) {
        throw std::bad_alloc{};
    }
    return ptr;
}

void* operator new(std::size_t bytes, const std::nothrow_t& /*tag*/) noexcept {
    noteIfRealtime(adx::rt::ViolationKind::Allocation, bytes, ADX_RETURN_ADDRESS());
    return allocateBytes(bytes);
}

void* operator new[](std::size_t bytes, const std::nothrow_t& /*tag*/) noexcept {
    noteIfRealtime(adx::rt::ViolationKind::Allocation, bytes, ADX_RETURN_ADDRESS());
    return allocateBytes(bytes);
}

void* operator new(std::size_t bytes, std::align_val_t alignment) {
    noteIfRealtime(adx::rt::ViolationKind::Allocation, bytes, ADX_RETURN_ADDRESS());
    void* ptr = allocateAligned(bytes, static_cast<std::size_t>(alignment));
    if (ptr == nullptr) {
        throw std::bad_alloc{};
    }
    return ptr;
}

void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    noteIfRealtime(adx::rt::ViolationKind::Allocation, bytes, ADX_RETURN_ADDRESS());
    void* ptr = allocateAligned(bytes, static_cast<std::size_t>(alignment));
    if (ptr == nullptr) {
        throw std::bad_alloc{};
    }
    return ptr;
}

void* operator new(std::size_t bytes, std::align_val_t alignment,
                   const std::nothrow_t& /*tag*/) noexcept {
    noteIfRealtime(adx::rt::ViolationKind::Allocation, bytes, ADX_RETURN_ADDRESS());
    return allocateAligned(bytes, static_cast<std::size_t>(alignment));
}

void* operator new[](std::size_t bytes, std::align_val_t alignment,
                     const std::nothrow_t& /*tag*/) noexcept {
    noteIfRealtime(adx::rt::ViolationKind::Allocation, bytes, ADX_RETURN_ADDRESS());
    return allocateAligned(bytes, static_cast<std::size_t>(alignment));
}

void operator delete(void* ptr) noexcept {
    noteIfRealtime(adx::rt::ViolationKind::Deallocation, 0, ADX_RETURN_ADDRESS());
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept {
    noteIfRealtime(adx::rt::ViolationKind::Deallocation, 0, ADX_RETURN_ADDRESS());
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t bytes) noexcept {
    noteIfRealtime(adx::rt::ViolationKind::Deallocation, bytes, ADX_RETURN_ADDRESS());
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t bytes) noexcept {
    noteIfRealtime(adx::rt::ViolationKind::Deallocation, bytes, ADX_RETURN_ADDRESS());
    std::free(ptr);
}

void operator delete(void* ptr, const std::nothrow_t& /*tag*/) noexcept {
    noteIfRealtime(adx::rt::ViolationKind::Deallocation, 0, ADX_RETURN_ADDRESS());
    std::free(ptr);
}

void operator delete[](void* ptr, const std::nothrow_t& /*tag*/) noexcept {
    noteIfRealtime(adx::rt::ViolationKind::Deallocation, 0, ADX_RETURN_ADDRESS());
    std::free(ptr);
}

void operator delete(void* ptr, std::align_val_t /*alignment*/) noexcept {
    noteIfRealtime(adx::rt::ViolationKind::Deallocation, 0, ADX_RETURN_ADDRESS());
    freeAligned(ptr);
}

void operator delete[](void* ptr, std::align_val_t /*alignment*/) noexcept {
    noteIfRealtime(adx::rt::ViolationKind::Deallocation, 0, ADX_RETURN_ADDRESS());
    freeAligned(ptr);
}

void operator delete(void* ptr, std::size_t bytes, std::align_val_t /*alignment*/) noexcept {
    noteIfRealtime(adx::rt::ViolationKind::Deallocation, bytes, ADX_RETURN_ADDRESS());
    freeAligned(ptr);
}

void operator delete[](void* ptr, std::size_t bytes, std::align_val_t /*alignment*/) noexcept {
    noteIfRealtime(adx::rt::ViolationKind::Deallocation, bytes, ADX_RETURN_ADDRESS());
    freeAligned(ptr);
}

void operator delete(void* ptr, std::align_val_t /*alignment*/,
                     const std::nothrow_t& /*tag*/) noexcept {
    noteIfRealtime(adx::rt::ViolationKind::Deallocation, 0, ADX_RETURN_ADDRESS());
    freeAligned(ptr);
}

void operator delete[](void* ptr, std::align_val_t /*alignment*/,
                       const std::nothrow_t& /*tag*/) noexcept {
    noteIfRealtime(adx::rt::ViolationKind::Deallocation, 0, ADX_RETURN_ADDRESS());
    freeAligned(ptr);
}

// NOLINTEND(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)

#endif // ADX_ENABLE_RT_GUARD

namespace adx::rt {

bool allocGuardLinked() noexcept {
#if ADX_ENABLE_RT_GUARD
    return true;
#else
    return false;
#endif
}

void installRtGuards() noexcept {
    // Referencing allocGuardLinked() is what drags this object file - and with it the
    // operator new replacements - into the final binary. Not a formality: see the
    // comment on allocGuardLinked() in the header.
    static_cast<void>(allocGuardLinked());
    installRtAssertHandler();
}

} // namespace adx::rt

// What this does not catch, stated plainly rather than left to be discovered:
//
//   - A dependency statically linked against a different CRT allocates through its
//     own malloc and never reaches these operators.
//   - OS-level allocations behind a Win32 call are invisible here.
//
// The mitigations are the clang-tidy ban list (phase_0.md §4.6) and the rule that no
// Win32 call appears below the binding layer. Neither is as good as detection, which
// is why both are enforced rather than encouraged.
//
// phase_1.md §3.3 also specifies _CrtSetAllocHook as a second mechanism, to catch
// malloc/realloc/free from C dependencies (RtAudio's internals, and later miniaudio,
// shine, libFLAC) that never route through operator new. It is deliberately not here:
// _CrtSetAllocHook exists only in the debug CRT, and the RT gate has to hold in
// RelWithDebInfo too, so adding it would buy coverage in the one configuration that
// needs it least while implying coverage in the one that needs it most. Tracked as an
// open issue for the phase that first links a C dependency into the callback path.
