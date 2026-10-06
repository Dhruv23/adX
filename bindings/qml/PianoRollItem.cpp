#include "bindings/qml/PianoRollItem.h"

#include <algorithm>
#include <array>

#include <QtGui/QMatrix4x4>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtQuick/QSGGeometryNode>
#include <QtQuick/QSGNode>

#include "engine/geometry/ColorPalette.h"

namespace adx::quick {

/// The root node, holding pointers to the nodes updatePaintNode refreshes. The scene
/// graph owns every node; these are only ever touched on the render thread.
struct PianoRollItem::Nodes : QSGNode {
    QSGClipNode* noteClip{nullptr};
    QSGTransformNode* noteTransform{nullptr};
    QSGClipNode* laneClip{nullptr};
    QSGTransformNode* laneTransform{nullptr};
    std::array<QSGGeometryNode*, kLayerCount> layers{};
    QSGGeometryNode* playhead{nullptr};
    QSGGeometryNode* selection{nullptr};
};

PianoRollItem::PianoRollItem(QQuickItem* parent) : QQuickItem(parent), m_palette(defaultPalette()) {
    setFlag(ItemHasContents, true);
    setFlag(ItemIsFocusScope, true);
    setAcceptedMouseButtons(Qt::AllButtons);
    setAcceptHoverEvents(true);
    setActiveFocusOnTab(true);
}

void PianoRollItem::setView(double beatStart, double worldTop, double pixelsPerBeat,
                            double pixelsPerRow) {
    m_beatStart = beatStart;
    m_worldTop = worldTop;
    m_pixelsPerBeat = pixelsPerBeat;
    m_pixelsPerRow = pixelsPerRow;
    // The playhead is two pixels wide at any zoom, so its six vertices follow the zoom.
    m_playheadDirty = true;
    update();
}

void PianoRollItem::upload(int layer, qulonglong address, int vertexCount, qulonglong revision) {
    if (layer < 0 || layer >= kLayerCount) {
        return;
    }
    // NOLINTNEXTLINE(performance-no-int-to-ptr) - the address of a leased engine buffer.
    const auto* source = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(address));
    m_layers.at(static_cast<std::size_t>(layer)).upload(source, vertexCount, revision);
    update();
}

void PianoRollItem::setPalette(const QVariantList& colors) {
    for (int role = 0; role < colors.size(); ++role) {
        setRole(m_palette, role, colors.at(role).value<QColor>());
    }
    m_paletteDirty = true;
    m_playheadDirty = true;
    update();
}

void PianoRollItem::setSelectionRect(double x0, double y0, double x1, double y1, bool visible) {
    m_selection = {x0, y0, x1, y1};
    m_selectionVisible = visible;
    m_selectionDirty = true;
    update();
}

qulonglong PianoRollItem::layerRevision(int layer) const {
    return layer >= 0 && layer < kLayerCount ? m_layers.at(static_cast<std::size_t>(layer)).revision
                                             : 0;
}

int PianoRollItem::layerVertexCount(int layer) const {
    return layer >= 0 && layer < kLayerCount
               ? m_layers.at(static_cast<std::size_t>(layer)).vertexCount
               : 0;
}

QVariantList PianoRollItem::paintCounts() const {
    return {m_paintUpdates, m_geometryRefills, m_transformOnly};
}

void PianoRollItem::setPlayheadBeats(double beats) {
    if (beats == m_playheadBeats) {
        return;
    }
    m_playheadBeats = beats;
    m_playheadDirty = true;
    emit playheadChanged();
    update();
}

void PianoRollItem::setLaneHeight(double height) {
    if (height == m_laneHeight) {
        return;
    }
    m_laneHeight = height;
    emit laneHeightChanged();
    update();
}

void PianoRollItem::buildTree(QSGNode* rootNode) {
    auto* root = static_cast<Nodes*>(rootNode);
    root->noteClip = makeClipNode();
    root->noteTransform = new QSGTransformNode;
    root->noteClip->appendChildNode(root->noteTransform);
    root->appendChildNode(root->noteClip);
    root->laneClip = makeClipNode();
    root->laneTransform = new QSGTransformNode;
    root->laneClip->appendChildNode(root->laneTransform);
    root->appendChildNode(root->laneClip);

    for (int layer = 0; layer < kLayerCount; ++layer) {
        const bool lines = layer == kGrid || layer == kCurves;
        QSGGeometryNode* node =
            makeLayerNode(lines ? QSGGeometry::DrawLines : QSGGeometry::DrawTriangles);
        root->layers.at(static_cast<std::size_t>(layer)) = node;
        if (layer == kLanes) {
            root->laneTransform->appendChildNode(node);
        } else {
            root->noteTransform->appendChildNode(node);
        }
    }
    root->playhead = makeLayerNode(QSGGeometry::DrawTriangles);
    root->noteTransform->appendChildNode(root->playhead);
    root->selection = makeLayerNode(QSGGeometry::DrawTriangles);
    root->appendChildNode(root->selection);
}

void PianoRollItem::updateTransforms(Nodes& nodes) {
    const auto w = static_cast<float>(width());
    const auto h = static_cast<float>(height());
    const auto lane = static_cast<float>(std::clamp(m_laneHeight, 0.0, height()));
    setClipRect(nodes.noteClip, 0.0F, 0.0F, w, h - lane);
    setClipRect(nodes.laneClip, 0.0F, h - lane, w, lane);

    QMatrix4x4 notes;
    notes.scale(static_cast<float>(m_pixelsPerBeat), static_cast<float>(m_pixelsPerRow));
    notes.translate(static_cast<float>(-m_beatStart), static_cast<float>(-m_worldTop));
    if (nodes.noteTransform->matrix() != notes) {
        nodes.noteTransform->setMatrix(notes);
        nodes.noteTransform->markDirty(QSGNode::DirtyMatrix);
    }
    QMatrix4x4 lanes;
    lanes.translate(0.0F, h - lane);
    lanes.scale(static_cast<float>(m_pixelsPerBeat), lane);
    lanes.translate(static_cast<float>(-m_beatStart), 0.0F);
    if (nodes.laneTransform->matrix() != lanes) {
        nodes.laneTransform->setMatrix(lanes);
        nodes.laneTransform->markDirty(QSGNode::DirtyMatrix);
    }
}

QSGNode* PianoRollItem::updatePaintNode(QSGNode* old, UpdatePaintNodeData* /*data*/) {
    auto* nodes = static_cast<Nodes*>(old);
    if (nodes == nullptr) {
        nodes = new Nodes;
        buildTree(nodes);
    }
    ++m_paintUpdates;
    updateTransforms(*nodes);

    bool refilled = false;
    for (int layer = 0; layer < kLayerCount; ++layer) {
        adx::quick::Layer& staged = m_layers.at(static_cast<std::size_t>(layer));
        if (!staged.dirty && !m_paletteDirty) {
            continue;
        }
        fillLayerNode(nodes->layers.at(static_cast<std::size_t>(layer)), staged, m_palette);
        staged.dirty = false;
        refilled = true;
    }
    m_paletteDirty = false;
    if (refilled) {
        ++m_geometryRefills;
    } else {
        ++m_transformOnly;
    }

    if (m_playheadDirty) {
        adx::quick::Layer line;
        if (m_playheadBeats >= 0.0) {
            const float role = adx::geometry::roleValue(adx::geometry::ColorRole::kPlayhead);
            const auto x0 = static_cast<float>(m_playheadBeats);
            const auto x1 = x0 + static_cast<float>(2.0 / std::max(m_pixelsPerBeat, 1e-6));
            const std::array<float, 18> vertices{x0, 0.0F,   role, x1, 0.0F,   role,
                                                 x0, 128.0F, role, x1, 0.0F,   role,
                                                 x1, 128.0F, role, x0, 128.0F, role};
            line.upload(vertices.data(), 6, 0);
        }
        fillLayerNode(nodes->playhead, line, m_palette);
        m_playheadDirty = false;
    }
    if (m_selectionDirty) {
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

void PianoRollItem::mousePressEvent(QMouseEvent* event) {
    forceActiveFocus(Qt::MouseFocusReason);
    emit pointerPressed(event->position().x(), event->position().y(),
                        static_cast<int>(event->button()), static_cast<int>(event->modifiers()));
    event->accept();
}

void PianoRollItem::mouseMoveEvent(QMouseEvent* event) {
    emit pointerMoved(event->position().x(), event->position().y(),
                      static_cast<int>(event->buttons()), static_cast<int>(event->modifiers()));
    event->accept();
}

void PianoRollItem::mouseReleaseEvent(QMouseEvent* event) {
    emit pointerReleased(event->position().x(), event->position().y(),
                         static_cast<int>(event->button()), static_cast<int>(event->modifiers()));
    event->accept();
}

void PianoRollItem::mouseDoubleClickEvent(QMouseEvent* event) {
    emit pointerDoubleClicked(event->position().x(), event->position().y(),
                              static_cast<int>(event->modifiers()));
    event->accept();
}

void PianoRollItem::hoverMoveEvent(QHoverEvent* event) {
    emit hovered(event->position().x(), event->position().y(),
                 static_cast<int>(event->modifiers()));
}

void PianoRollItem::wheelEvent(QWheelEvent* event) {
    emit wheeled(event->position().x(), event->position().y(), event->angleDelta().x(),
                 event->angleDelta().y(), static_cast<int>(event->modifiers()));
    event->accept();
}

void PianoRollItem::keyPressEvent(QKeyEvent* event) {
    emit keyPressed(event->key(), static_cast<int>(event->modifiers()), event->text());
    event->accept();
}

} // namespace adx::quick
