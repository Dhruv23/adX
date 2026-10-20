// The scene-graph item every Phase 6 hot surface draws with: the playlist, the channel
// rack's step grid, automation lanes, the mixer's meters (phase_6.md §4.1-§4.5).
//
// The piano roll's pipeline (PianoRollItem.h), made generic: N layers of engine vertices
// (x, y, colour role), each drawn as triangles or lines, each either in world space -
// through one transform, so a pan or zoom moves a matrix and rebuilds nothing - or in
// pixel space. The playhead is its own node, positioned from a world x, so moving it
// never dirties a layer. Input is forwarded as raw signals for Python's tools.
//
// Node tree:
//
//   root
//    +- clip (the item's rectangle)
//        +- QSGTransformNode   <- the view: world layers
//        +- pixel layers
//        +- playhead, selection rectangle (pixels)
#pragma once

#include <array>
#include <cstdint>

#include <QtCore/QVariantList>
#include <QtQuick/QQuickItem>

#include "bindings/qml/SceneLayers.h"

class QSGTransformNode;
class QSGClipNode;
class QSGGeometryNode;

namespace adx::quick {

class GeometryItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(double playheadX READ playheadX WRITE setPlayheadX NOTIFY playheadChanged)

public:
    static constexpr int kMaxLayers = 8;

    /// A layer's drawing: bit 0 lines (else triangles), bit 1 pixel space (else world).
    enum LayerMode : int {
        kWorldTriangles = 0,
        kWorldLines = 1,
        kPixelTriangles = 2,
        kPixelLines = 3
    };

    explicit GeometryItem(QQuickItem* parent = nullptr);

    /// One LayerMode per layer, in draw order. Rebuilds the node tree on the next paint.
    Q_INVOKABLE void setLayers(const QVariantList& modes);
    /// World to pixels: px = (x - originX) * scaleX, py = (y - originY) * scaleY. Moving
    /// it changes a matrix and nothing else.
    Q_INVOKABLE void setView(double originX, double originY, double scaleX, double scaleY);
    /// Copies one layer's vertices from engine memory (a GeometryLease's address).
    Q_INVOKABLE void upload(int layer, qulonglong address, int vertexCount, qulonglong revision);
    Q_INVOKABLE void setPalette(const QVariantList& colors);
    Q_INVOKABLE void setSelectionRect(double x0, double y0, double x1, double y1, bool visible);
    Q_INVOKABLE [[nodiscard]] int layerVertexCount(int layer) const;
    Q_INVOKABLE [[nodiscard]] qulonglong layerRevision(int layer) const;
    /// [paint-node updates, geometry refills, transform-only updates] since creation.
    Q_INVOKABLE [[nodiscard]] QVariantList paintCounts() const;

    [[nodiscard]] double playheadX() const noexcept {
        return m_playheadX;
    }
    /// A world x; negative hides the playhead.
    void setPlayheadX(double x);

signals:
    void playheadChanged();
    void pointerPressed(double x, double y, int button, int modifiers);
    void pointerMoved(double x, double y, int buttons, int modifiers);
    void pointerReleased(double x, double y, int button, int modifiers);
    void pointerDoubleClicked(double x, double y, int modifiers);
    void hovered(double x, double y, int modifiers);
    void wheeled(double x, double y, double deltaX, double deltaY, int modifiers);
    void keyPressed(int key, int modifiers, const QString& text);

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void hoverMoveEvent(QHoverEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    struct Nodes;
    void buildTree(Nodes& nodes) const;

    std::array<Layer, kMaxLayers> m_layers;
    std::array<int, kMaxLayers> m_modes{};
    int m_layerCount{0};
    bool m_treeDirty{true};
    Palette m_palette;
    bool m_paletteDirty{true};
    double m_originX{0.0};
    double m_originY{0.0};
    double m_scaleX{1.0};
    double m_scaleY{1.0};
    double m_playheadX{-1.0};
    bool m_playheadDirty{true};
    std::array<double, 4> m_selection{};
    bool m_selectionVisible{false};
    bool m_selectionDirty{false};
    int m_paintUpdates{0};
    int m_geometryRefills{0};
    int m_transformOnly{0};
};

} // namespace adx::quick
