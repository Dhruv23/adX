// adx-thread: main
//
// Writing vertices into a GeometryBuffer's span: quads as two triangles, line segments
// as vertex pairs (the item's *_LINES layers). For the Phase 10 analyzers; a writer that
// runs off the end of its span stops writing rather than overrun, and count() says how
// many vertices it actually wrote, so a builder can size generously and trim.
#pragma once

#include <cstddef>
#include <span>

#include "engine/geometry/ColorPalette.h"
#include "engine/geometry/GeometryBuffer.h"

namespace adx::geometry {

class VertexWriter {
public:
    explicit VertexWriter(std::span<float> out) noexcept : m_out(out) {}

    void vertex(float x, float y, ColorRole role) noexcept {
        if (m_at + kFloatsPerVertex > m_out.size()) {
            return;
        }
        m_out[m_at++] = x;
        m_out[m_at++] = y;
        m_out[m_at++] = roleValue(role);
    }
    void quad(float x0, float y0, float x1, float y1, ColorRole role) noexcept {
        vertex(x0, y0, role);
        vertex(x1, y0, role);
        vertex(x1, y1, role);
        vertex(x0, y0, role);
        vertex(x1, y1, role);
        vertex(x0, y1, role);
    }
    void line(float x0, float y0, float x1, float y1, ColorRole role) noexcept {
        vertex(x0, y0, role);
        vertex(x1, y1, role);
    }
    [[nodiscard]] std::size_t count() const noexcept {
        return m_at / kFloatsPerVertex;
    }

private:
    std::span<float> m_out;
    std::size_t m_at{0};
};

/// Sizes `buffer` for `capacity` vertices, lets `write` fill it, then trims it to what
/// was written. Allocation-free once the buffer has grown to `capacity`.
template<class Write>
void writeVertices(GeometryBuffer& buffer, std::size_t capacity, Write&& write) {
    VertexWriter writer(buffer.resize(capacity, kFloatsPerVertex));
    write(writer);
    const std::size_t written = writer.count();
    if (written != capacity) {
        (void)buffer.resize(written, kFloatsPerVertex);
    }
}

} // namespace adx::geometry
