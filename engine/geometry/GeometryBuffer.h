// adx-thread: main
//
// C++-owned vertex storage that Python reads without copying (phase_5.md §4.1).
//
// Everything in engine/geometry/ is main-thread code by contract. Nothing here runs on
// the audio thread, nothing here includes pybind11 (FINAL_PLAN §2.2 Rule 1, checked by
// `tools/lint.py rules`), and nothing here is touched by the Qt render thread: the
// scene-graph items copy what they need while the GUI thread is blocked.
//
// A vertex is `floatsPerVertex` floats; every builder in this directory uses three -
// x and y in the builder's world space, then a ColorRole as a float.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace adx::geometry {

/// Floats per vertex for every builder in this directory: x, y, colour role.
inline constexpr std::size_t kFloatsPerVertex = 3;

class GeometryBuffer {
public:
    GeometryBuffer() = default;

    /// Sets the size and returns the storage to write. Main thread only. Grows by
    /// reallocation and never shrinks its capacity, so steady-state rebuilds allocate
    /// nothing. Throws std::logic_error during an active lease: a numpy view of this
    /// storage is alive, and reallocating would leave it dangling.
    std::span<float> resize(std::size_t vertexCount, std::size_t floatsPerVertex);

    /// Writable access for in-place patches - a selection change recolours vertices
    /// without moving them. Bumps the revision; throws during a lease, as resize does.
    std::span<float> patch();

    [[nodiscard]] const float* data() const noexcept {
        return m_storage.data();
    }
    [[nodiscard]] std::size_t vertexCount() const noexcept {
        return m_vertexCount;
    }
    [[nodiscard]] std::size_t floatsPerVertex() const noexcept {
        return m_floatsPerVertex;
    }
    [[nodiscard]] std::span<const float> floats() const noexcept {
        return {m_storage.data(), m_vertexCount * m_floatsPerVertex};
    }

    /// Bumped on every content change, never on a read. Python compares it to decide
    /// whether to re-upload; the scene-graph item compares it to decide whether to
    /// mark its node dirty.
    [[nodiscard]] std::uint64_t revision() const noexcept {
        return m_revision;
    }

    /// Lease bookkeeping, for GeometryLease. Counted rather than flagged so two
    /// leases on one buffer nest.
    void acquireLease() noexcept {
        ++m_leases;
    }
    void releaseLease() noexcept {
        --m_leases;
    }
    [[nodiscard]] int leases() const noexcept {
        return m_leases;
    }

private:
    std::vector<float> m_storage;
    std::size_t m_vertexCount{0};
    std::size_t m_floatsPerVertex{kFloatsPerVertex};
    std::uint64_t m_revision{0};
    int m_leases{0};
};

/// Pins a buffer for as long as it lives: resize() and patch() throw meanwhile. The
/// Python `with` block over a geometry view is one of these.
class GeometryLease {
public:
    explicit GeometryLease(GeometryBuffer& buffer) noexcept
        : m_buffer(&buffer), m_revision(buffer.revision()) {
        m_buffer->acquireLease();
    }
    ~GeometryLease() {
        release();
    }
    GeometryLease(const GeometryLease&) = delete;
    GeometryLease& operator=(const GeometryLease&) = delete;
    GeometryLease(GeometryLease&&) = delete;
    GeometryLease& operator=(GeometryLease&&) = delete;

    /// Ends the lease early. Returns whether the buffer's revision is what it was when
    /// the lease began - false means something changed it behind the lease's back.
    bool release() noexcept;

    [[nodiscard]] const GeometryBuffer& buffer() const noexcept {
        return *m_buffer;
    }
    [[nodiscard]] bool active() const noexcept {
        return m_active;
    }

private:
    GeometryBuffer* m_buffer;
    std::uint64_t m_revision;
    bool m_active{true};
};

} // namespace adx::geometry
