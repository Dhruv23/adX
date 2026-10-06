#include "bindings/qml/WaveformItem.h"

#include <array>

#include <QtGui/QMatrix4x4>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtQuick/QSGGeometryNode>
#include <QtQuick/QSGNode>

#include "engine/geometry/ColorPalette.h"

namespace adx::quick {

struct WaveformItem::Nodes : QSGNode {
    QSGClipNode* clip{nullptr};
    QSGTransformNode* transform{nullptr};
    QSGGeometryNode* strip{nullptr};
    QSGTransformNode* playheadTransform{nullptr};
    QSGGeometryNode* playhead{nullptr};
};

WaveformItem::WaveformItem(QQuickItem* parent) : QQuickItem(parent), m_palette(defaultPalette()) {
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::AllButtons);
}

void WaveformItem::setView(double secondsStart, double pixelsPerSecond) {
    m_secondsStart = secondsStart;
    m_pixelsPerSecond = pixelsPerSecond;
    update();
}

void WaveformItem::upload(qulonglong address, int vertexCount, qulonglong revision) {
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - the address of a leased engine buffer.
    m_strip.upload(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address)), vertexCount,
                   revision);
    update();
}

void WaveformItem::setPalette(const QVariantList& colors) {
    for (int role = 0; role < colors.size(); ++role) {
        setRole(m_palette, role, colors.at(role).value<QColor>());
    }
    m_paletteDirty = true;
    m_playheadDirty = true;
    update();
}

qulonglong WaveformItem::revision() const {
    return m_strip.revision;
}

QVariantList WaveformItem::paintCounts() const {
    return {m_paintUpdates, m_geometryRefills};
}

void WaveformItem::setPlayheadSeconds(double seconds) {
    if (seconds == m_playheadSeconds) {
        return;
    }
    m_playheadSeconds = seconds;
    m_playheadDirty = true;
    emit playheadChanged();
    update();
}

QSGNode* WaveformItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData* /*data*/) {
    auto* nodes = static_cast<Nodes*>(old);
    if (nodes == nullptr) {
        nodes = new Nodes;
        nodes->clip = makeClipNode();
        nodes->transform = new QSGTransformNode;
        nodes->strip = makeLayerNode(QSGGeometry::DrawTriangleStrip);
        nodes->transform->appendChildNode(nodes->strip);
        nodes->clip->appendChildNode(nodes->transform);
        nodes->playhead = makeLayerNode(QSGGeometry::DrawLines);
        nodes->transform->appendChildNode(nodes->playhead);
        nodes->appendChildNode(nodes->clip);
    }
    ++m_paintUpdates;
    const auto w = static_cast<float>(width());
    const auto h = static_cast<float>(height());
    setClipRect(nodes->clip, 0.0F, 0.0F, w, h);

    // x: seconds to pixels; y: amplitude +1 at the top, -1 at the bottom, 5% margin.
    QMatrix4x4 matrix;
    matrix.translate(0.0F, h * 0.5F);
    matrix.scale(static_cast<float>(m_pixelsPerSecond), -h * 0.475F);
    matrix.translate(static_cast<float>(-m_secondsStart), 0.0F);
    if (nodes->transform->matrix() != matrix) {
        nodes->transform->setMatrix(matrix);
        nodes->transform->markDirty(QSGNode::DirtyMatrix);
    }
    if (m_strip.dirty || m_paletteDirty) {
        fillLayerNode(nodes->strip, m_strip, m_palette);
        m_strip.dirty = false;
        ++m_geometryRefills;
    }
    m_paletteDirty = false;
    if (m_playheadDirty) {
        Layer line;
        if (m_playheadSeconds >= 0.0) {
            const float role = adx::geometry::roleValue(adx::geometry::ColorRole::kPlayhead);
            const auto x = static_cast<float>(m_playheadSeconds);
            const std::array<float, 6> vertices{x, -1.05F, role, x, 1.05F, role};
            line.upload(vertices.data(), 2, 0);
        }
        fillLayerNode(nodes->playhead, line, m_palette);
        m_playheadDirty = false;
    }
    return nodes;
}

void WaveformItem::mousePressEvent(QMouseEvent* event) {
    emit pointerPressed(event->position().x(), event->position().y(),
                        static_cast<int>(event->button()), static_cast<int>(event->modifiers()));
    event->accept();
}

void WaveformItem::mouseMoveEvent(QMouseEvent* event) {
    emit pointerMoved(event->position().x(), event->position().y(),
                      static_cast<int>(event->buttons()), static_cast<int>(event->modifiers()));
    event->accept();
}

void WaveformItem::mouseReleaseEvent(QMouseEvent* event) {
    emit pointerReleased(event->position().x(), event->position().y(),
                         static_cast<int>(event->button()), static_cast<int>(event->modifiers()));
    event->accept();
}

void WaveformItem::wheelEvent(QWheelEvent* event) {
    emit wheeled(event->position().x(), event->position().y(), event->angleDelta().x(),
                 event->angleDelta().y(), static_cast<int>(event->modifiers()));
    event->accept();
}

} // namespace adx::quick
