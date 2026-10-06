// The part of every adX scene-graph item that is the same: staged vertices, a palette,
// and geometry nodes filled from them (phase_5.md §4.3).
//
// Vertices arrive from Python as the engine's (x, y, role) floats - one memcpy into a
// staging vector on the GUI thread. updatePaintNode, on the render thread while the GUI
// thread waits, converts them once into QSGGeometry::ColoredPoint2D with the palette.
// Nothing here ever runs Python: the render thread never takes the GIL.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <QtGui/QColor>
#include <QtQuick/QSGGeometryNode>
#include <QtQuick/QSGNode>

#include "engine/geometry/ColorPalette.h"

namespace adx::quick {

/// RGBA per colour role, resolved by the theme.
using Palette = std::array<QSGGeometry::ColoredPoint2D, adx::geometry::kColorRoleCount>;

/// The default palette: dark theme, in case Python never sets one.
[[nodiscard]] Palette defaultPalette();

/// Sets entry `role` from a colour. Out-of-range roles are ignored.
void setRole(Palette& palette, int role, const QColor& color);

/// One layer: what Python last uploaded, and whether its node needs refilling.
struct Layer {
    std::vector<float> staging;
    std::uint64_t revision{0};
    int vertexCount{0};
    bool dirty{false};

    /// Copies `vertexCount` vertices of three floats from `address`. One memcpy.
    void upload(const void* address, int vertexCount, std::uint64_t revision);
};

/// A geometry node with a vertex-colour material that owns both.
[[nodiscard]] QSGGeometryNode* makeLayerNode(unsigned int drawingMode);

/// Refills `node` from `layer` through `palette`, and marks it dirty. Returns true when
/// it did anything (a geometry rebuild, which tests count).
bool fillLayerNode(QSGGeometryNode* node, const Layer& layer, const Palette& palette);

/// A rectangle in pixel space as two triangles, in the given colour.
void fillRect(QSGGeometryNode* node, float x0, float y0, float x1, float y1,
              const QSGGeometry::ColoredPoint2D& color);

/// A clip node whose clip is the rectangle (x, y, w, h).
[[nodiscard]] QSGClipNode* makeClipNode();
void setClipRect(QSGClipNode* node, float x, float y, float w, float h);

} // namespace adx::quick
