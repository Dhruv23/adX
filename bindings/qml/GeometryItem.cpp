#include "bindings/qml/GeometryItem.h"

#include <algorithm>

#include <QtGui/QMatrix4x4>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtQuick/QSGGeometryNode>
#include <QtQuick/QSGNode>

#include "engine/geometry/ColorPalette.h"

namespace adx::quick {

/// The root node and the nodes updatePaintNode refreshes. Render thread only.
struct GeometryItem::Nodes : QSGNode {
    QSGClipNode* clip{nullptr};
    QSGTransformNode* world{nullptr};
    std::array<QSGGeometryNode*, kMaxLayers> layers{};
    QSGGeometryNode* playhead{nullptr};
    QSGGeometryNode* selection{nullptr};
    int built{-1};
};

GeometryItem::GeometryItem(QQuickItem* parent) : QQuickItem(parent), m_palette(defaultPalette()) {
    setFlag(ItemHasContents, true);
    setFlag(ItemIsFocusScope, true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setActiveFocusOnTab(true);
}

void GeometryItem::setLayers(const QVariantList& modes) {
    m_layerCount = std::min(static_cast<int>(modes.size()), kMaxLayers);
    for (int i = 0; i < m_layerCount; ++i) {
        m_modes.at(static_cast<std::size_t>(i)) = std::clamp(modes.at(i).toInt(), 0, 3);
    }
    m_treeDirty = true;
    update();
}

void GeometryItem::setView(double originX, double originY, double scaleX, double scaleY) {
    m_originX = originX;
    m_originY = originY;
    m_scaleX = scaleX;
    m_scaleY = scaleY;
    m_playheadDirty = true;
    update();
}

void GeometryItem::upload(int layer, qulonglong address, int vertexCount, qulonglong revision) {
    if (layer < 0 || layer >= kMaxLayers) {
        return;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - the address of a leased engine buffer.
    const auto* source = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address));
    m_layers.at(static_cast<std::size_t>(layer)).upload(source, vertexCount, revision);
    update();
}

void GeometryItem::setPalette(const QVariantList& colors) {
    for (int role = 0; role < colors.size(); ++role) {
        setRole(m_palette, role, colors.at(role).value<QColor>());
    }
    m_paletteDirty = true;
    m_playheadDirty = true;
    update();
}

void GeometryItem::setSelectionRect(double x0, double y0, double x1, double y1, bool visible) {
    m_selection = {x0, y0, x1, y1};
    m_selectionVisible = visible;
    m_selectionDirty = true;
    update();
}

int GeometryItem::layerVertexCount(int layer) const {
    return layer >= 0 && layer < kMaxLayers
               ? m_layers.at(static_cast<std::size_t>(layer)).vertexCount
               : 0;
}

qulonglong GeometryItem::layerRevision(int layer) const {
    return layer >= 0 && layer < kMaxLayers ? m_layers.at(static_cast<std::size_t>(layer)).revision
                                            : 0;
}

QVariantList GeometryItem::paintCounts() const {
    return {m_paintUpdates, m_geometryRefills, m_transformOnly};
}

void GeometryItem::setPlayheadX(double x) {
    if (x == m_playheadX) {
        return;
    }
    m_playheadX = x;
    m_playheadDirty = true;
    emit playheadChanged();
    update();
}

void GeometryItem::buildTree(Nodes& nodes) const {
    nodes.removeAllChildNodes();
    // removeAllChildNodes does not delete; the old subtree goes with its clip node.
    delete nodes.clip;
    nodes.clip = makeClipNode();
    nodes.world = new QSGTransformNode;
    nodes.clip->appendChildNode(nodes.world);
    nodes.appendChildNode(nodes.clip);
    nodes.layers.fill(nullptr);
    for (int i = 0; i < m_layerCount; ++i) {
        const int mode = m_modes.at(static_cast<std::size_t>(i));
        QSGGeometryNode* node =
            makeLayerNode((mode & 1) != 0 ? QSGGeometry::DrawLines : QSGGeometry::DrawTriangles);
        nodes.layers.at(static_cast<std::size_t>(i)) = node;
        ((mode & 2) != 0 ? static_cast<QSGNode*>(nodes.clip) : nodes.world)->appendChildNode(node);
    }
    nodes.playhead = makeLayerNode(QSGGeometry::DrawTriangles);
    nodes.clip->appendChildNode(nodes.playhead);
    nodes.selection = makeLayerNode(QSGGeometry::DrawTriangles);
    nodes.clip->appendChildNode(nodes.selection);
    nodes.built = m_layerCount;
}

QSGNode* GeometryItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData* /*data*/) {
    auto* nodes = static_cast<Nodes*>(old);
    bool rebuilt = false;
    if (nodes == nullptr) {
        nodes = new Nodes;
    }
    if (m_treeDirty || nodes->built < 0) {
        buildTree(*nodes);
        m_treeDirty = false;
        rebuilt = true;
    }
    ++m_paintUpdates;
    const auto w = static_cast<float>(width());
    const auto h = static_cast<float>(height());
    setClipRect(nodes->clip, 0.0F, 0.0F, w, h);

    QMatrix4x4 view;
    view.scale(static_cast<float>(m_scaleX), static_cast<float>(m_scaleY));
    view.translate(static_cast<float>(-m_originX), static_cast<float>(-m_originY));
    if (nodes->world->matrix() != view) {
        nodes->world->setMatrix(view);
        nodes->world->markDirty(QSGNode::DirtyMatrix);
    }

    bool refilled = false;
    for (int i = 0; i < m_layerCount; ++i) {
        Layer& staged = m_layers.at(static_cast<std::size_t>(i));
        if (!staged.dirty && !m_paletteDirty && !rebuilt) {
            continue;
        }
        fillLayerNode(nodes->layers.at(static_cast<std::size_t>(i)), staged, m_palette);
        staged.dirty = false;
        refilled = true;
    }
    m_paletteDirty = false;
    if (refilled) {
        ++m_geometryRefills;
    } else {
        ++m_transformOnly;
    }

    if (m_playheadDirty || rebuilt) {
        Layer line;
        if (m_playheadX >= 0.0) {
            const float role = adx::geometry::roleValue(adx::geometry::ColorRole::kPlayhead);
            const auto x0 = static_cast<float>((m_playheadX - m_originX) * m_scaleX);
            const float x1 = x0 + 2.0F;
            const std::array<float, 18> vertices{x0, 0.0F, role, x1, 0.0F, role, x0, h, role,
                                                 x1, 0.0F, role, x1, h,    role, x0, h, role};
            line.upload(vertices.data(), 6, 0);
        }
        fillLayerNode(nodes->playhead, line, m_palette);
        m_playheadDirty = false;
    }
    if (m_selectionDirty || rebuilt) {
        if (m_selectionVisible) {
            fillRect(
                nodes->selection, static_cast<float>(m_selection[0]),
                static_cast<float>(m_selection[1]), static_cast<float>(m_selection[2]),
                static_cast<float>(m_selection[3]),
                m_palette.at(static_cast<std::size_t>(adx::geometry::ColorRole::kSelectionRect)));
        } else {
            nodes->selection->geometry()->allocate(0);
            nodes->selection->markDirty(QSGNode::DirtyGeometry);
        }
        m_selectionDirty = false;
    }
    return nodes;
}

void GeometryItem::mousePressEvent(QMouseEvent* event) {
    forceActiveFocus(Qt::MouseFocusReason);
    emit pointerPressed(event->position().x(), event->position().y(),
                        static_cast<int>(event->button()), static_cast<int>(event->modifiers()));
    event->accept();
}

void GeometryItem::mouseMoveEvent(QMouseEvent* event) {
    emit pointerMoved(event->position().x(), event->position().y(),
                      static_cast<int>(event->buttons()), static_cast<int>(event->modifiers()));
    event->accept();
}

void GeometryItem::mouseReleaseEvent(QMouseEvent* event) {
    emit pointerReleased(event->position().x(), event->position().y(),
                         static_cast<int>(event->button()), static_cast<int>(event->modifiers()));
    event->accept();
}

void GeometryItem::mouseDoubleClickEvent(QMouseEvent* event) {
    emit pointerDoubleClicked(event->position().x(), event->position().y(),
                              static_cast<int>(event->modifiers()));
    event->accept();
}

void GeometryItem::hoverMoveEvent(QHoverEvent* event) {
    emit hovered(event->position().x(), event->position().y(),
                 static_cast<int>(event->modifiers()));
}

void GeometryItem::wheelEvent(QWheelEvent* event) {
    emit wheeled(event->position().x(), event->position().y(), event->angleDelta().x(),
                 event->angleDelta().y(), static_cast<int>(event->modifiers()));
    event->accept();
}

void GeometryItem::keyPressEvent(QKeyEvent* event) {
    emit keyPressed(event->key(), static_cast<int>(event->modifiers()), event->text());
    event->accept();
}

} // namespace adx::quick
