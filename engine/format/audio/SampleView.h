// What the audio thread sees of a decoded sample: planar pointers and a length, behind
// a handle that says whether decoding has finished.
//
// The realtime half of the sample pool (SamplePool.h is the main-thread half). A
// sampler node holds `const SampleHandle*` pointers; the pool owns the buffers they
// point into and keeps them alive for as long as any node pins them (phase_4.md §4.11:
// "a node holds a borrowed span valid for the snapshot's lifetime, and the snapshot
// pins the sample"). Decoding is asynchronous, so a handle may still be loading when
// the node first plays: the node reads `ready()` and plays silence until it is true
// - it never waits.
#pragma once

#include <atomic>
#include <cstdint>

namespace adx::format {

struct SampleView {
    const float* left{nullptr};
    /// Equal to `left` for a mono sample: one buffer, read twice.
    const float* right{nullptr};
    std::uint64_t frames{0};
    std::uint32_t sampleRate{0};

    [[nodiscard]] bool empty() const noexcept {
        return frames == 0 || left == nullptr;
    }
};

enum class SampleState : std::uint8_t { Loading, Ready, Failed };

class SampleHandle {
public:
    /// Audio thread. True once the view is filled in; the acquire pairs with the
    /// decoder's release in publish(), so the view's contents are visible after it.
    [[nodiscard]] bool ready() const noexcept {
        return m_state.load(std::memory_order_acquire) == SampleState::Ready;
    }
    [[nodiscard]] SampleState state() const noexcept {
        return m_state.load(std::memory_order_acquire);
    }
    /// Valid only when ready().
    [[nodiscard]] const SampleView& view() const noexcept {
        return m_view;
    }

    /// Decoder thread. Fills the view, then makes it visible.
    void publish(const SampleView& view) noexcept {
        m_view = view;
        m_state.store(SampleState::Ready, std::memory_order_release);
    }
    void fail() noexcept {
        m_state.store(SampleState::Failed, std::memory_order_release);
    }

private:
    SampleView m_view;
    std::atomic<SampleState> m_state{SampleState::Loading};
};

} // namespace adx::format
