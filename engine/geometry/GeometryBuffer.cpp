#include "engine/geometry/GeometryBuffer.h"

#include <stdexcept>

namespace adx::geometry {

std::span<float> GeometryBuffer::resize(std::size_t vertexCount, std::size_t floatsPerVertex) {
    if (m_leases > 0) {
        throw std::logic_error("GeometryBuffer::resize during an active GeometryLease: a view of "
                               "this buffer is still alive (phase_5.md 4.1)");
    }
    const std::size_t floats = vertexCount * floatsPerVertex;
    if (floats > m_storage.size()) {
        m_storage.resize(floats);
    }
    m_vertexCount = vertexCount;
    m_floatsPerVertex = floatsPerVertex;
    ++m_revision;
    return {m_storage.data(), floats};
}

std::span<float> GeometryBuffer::patch() {
    if (m_leases > 0) {
        throw std::logic_error("GeometryBuffer::patch during an active GeometryLease");
    }
    ++m_revision;
    return {m_storage.data(), m_vertexCount * m_floatsPerVertex};
}

bool GeometryLease::release() noexcept {
    if (!m_active) {
        return true;
    }
    m_active = false;
    m_buffer->releaseLease();
    return m_buffer->revision() == m_revision;
}

} // namespace adx::geometry
