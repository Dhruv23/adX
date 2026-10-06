#include "bindings/qml/SceneLayers.h"

#include <algorithm>
#include <cstring>

#include <QtQuick/QSGFlatColorMaterial>
#include <QtQuick/QSGVertexColorMaterial>

#include "engine/geometry/GeometryBuffer.h"

namespace adx::quick {
namespace {

QSGGeometry::ColoredPoint2D rgba(int r, int g, int b, int a = 255) {
    QSGGeometry::ColoredPoint2D point{};
    // Premultiplied, which QSGVertexColorMaterial expects.
    point.set(0.0F, 0.0F, static_cast<unsigned char>(r * a / 255),
              static_cast<unsigned char>(g * a / 255), static_cast<unsigned char>(b * a / 255),
              static_cast<unsigned char>(a));
    return point;
}

} // namespace

Palette defaultPalette() {
    using adx::geometry::ColorRole;
    Palette palette{};
    const auto at = [&palette](ColorRole role) -> QSGGeometry::ColoredPoint2D& {
        return palette.at(static_cast<std::size_t>(role));
    };
    at(ColorRole::kBackground) = rgba(30, 32, 36);
    at(ColorRole::kRowWhite) = rgba(44, 47, 53);
    at(ColorRole::kRowBlack) = rgba(36, 38, 43);
    at(ColorRole::kRowInScale) = rgba(50, 56, 66);
    at(ColorRole::kRowRoot) = rgba(62, 70, 86);
    at(ColorRole::kGridSub) = rgba(56, 59, 66);
    at(ColorRole::kGridBeat) = rgba(72, 76, 85);
    at(ColorRole::kGridBar) = rgba(105, 110, 122);
    at(ColorRole::kNote) = rgba(96, 178, 240);
    at(ColorRole::kNoteSelected) = rgba(255, 196, 92);
    at(ColorRole::kNoteMuted) = rgba(96, 104, 116);
    at(ColorRole::kGhost) = rgba(140, 150, 170, 70);
    at(ColorRole::kDensity) = rgba(96, 178, 240, 200);
    at(ColorRole::kLane) = rgba(96, 178, 240);
    at(ColorRole::kLaneSelected) = rgba(255, 196, 92);
    at(ColorRole::kCurve) = rgba(255, 120, 160);
    at(ColorRole::kPlayhead) = rgba(255, 90, 80);
    at(ColorRole::kWaveFill) = rgba(120, 200, 150);
    at(ColorRole::kWavePending) = rgba(120, 128, 140);
    at(ColorRole::kSelectionRect) = rgba(255, 196, 92, 60);
    return palette;
}

void setRole(Palette& palette, int role, const QColor& color) {
    if (role < 0 || static_cast<std::size_t>(role) >= palette.size()) {
        return;
    }
    palette.at(static_cast<std::size_t>(role)) =
        rgba(color.red(), color.green(), color.blue(), color.alpha());
}

void Layer::upload(const void* address, int count, std::uint64_t newRevision) {
    const auto floats =
        static_cast<std::size_t>(std::max(0, count)) * adx::geometry::kFloatsPerVertex;
    staging.resize(floats);
    if (floats > 0 && address != nullptr) {
        std::memcpy(staging.data(), address, floats * sizeof(float));
    }
    vertexCount = std::max(0, count);
    revision = newRevision;
    dirty = true;
}

QSGGeometryNode* makeLayerNode(unsigned int drawingMode) {
    auto* node = new QSGGeometryNode;
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0);
    geometry->setDrawingMode(drawingMode);
    geometry->setLineWidth(1.0F);
    node->setGeometry(geometry);
    node->setFlag(QSGNode::OwnsGeometry);
    auto* material = new QSGVertexColorMaterial;
    node->setMaterial(material);
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}

bool fillLayerNode(QSGGeometryNode* node, const Layer& layer, const Palette& palette) {
    QSGGeometry* geometry = node->geometry();
    geometry->allocate(layer.vertexCount);
    QSGGeometry::ColoredPoint2D* out = geometry->vertexDataAsColoredPoint2D();
    const float* in = layer.staging.data();
    const auto last = static_cast<float>(palette.size() - 1);
    for (int v = 0; v < layer.vertexCount; ++v) {
        const float* vertex = in + (static_cast<std::ptrdiff_t>(v) * 3);
        const auto role = static_cast<std::size_t>(std::clamp(vertex[2], 0.0F, last));
        out[v] = palette.at(role);
        out[v].x = vertex[0];
        out[v].y = vertex[1];
    }
    node->markDirty(QSGNode::DirtyGeometry | QSGNode::DirtyMaterial);
    return true;
}

void fillRect(QSGGeometryNode* node, float x0, float y0, float x1, float y1,
              const QSGGeometry::ColoredPoint2D& color) {
    QSGGeometry* geometry = node->geometry();
    geometry->allocate(6);
    QSGGeometry::ColoredPoint2D* out = geometry->vertexDataAsColoredPoint2D();
    const std::array<std::array<float, 2>, 6> corners{
        {{x0, y0}, {x1, y0}, {x0, y1}, {x1, y0}, {x1, y1}, {x0, y1}}};
    for (std::size_t i = 0; i < corners.size(); ++i) {
        out[i] = color;
        out[i].x = corners.at(i)[0];
        out[i].y = corners.at(i)[1];
    }
    node->markDirty(QSGNode::DirtyGeometry);
}

QSGClipNode* makeClipNode() {
    auto* clip = new QSGClipNode;
    clip->setIsRectangular(true);
    auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_Point2D(), 4);
    geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
    clip->setGeometry(geometry);
    clip->setFlag(QSGNode::OwnsGeometry);
    return clip;
}

void setClipRect(QSGClipNode* node, float x, float y, float w, float h) {
    const QRectF rect(x, y, w, h);
    if (node->clipRect() == rect) {
        return;
    }
    node->setClipRect(rect);
    QSGGeometry::updateRectGeometry(node->geometry(), rect);
    node->markDirty(QSGNode::DirtyGeometry);
}

} // namespace adx::quick
