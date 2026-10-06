// The piano roll's scene-graph item (phase_5.md §4.3).
//
// C++ rather than Python because updatePaintNode() runs on Qt's render thread, and a
// Python item would take the GIL on every frame - the UI-side twin of Rule 1.
//
// Node tree:
//
//   root
//    +- clip (the note area)
//    |   +- QSGTransformNode   <- pan/zoom: the ONLY thing a pan touches
//    |       +- rows    triangles   +- grid   lines   +- ghosts  triangles
//    |       +- notes   triangles   +- curves lines   +- playhead (its own 6-vertex node)
//    +- clip (the lane strip)
//    |   +- QSGTransformNode   <- same x, lane y
//    |       +- lanes   triangles
//    +- selection rectangle, in pixels
//
// The playhead is its own node, moved by a 60 Hz timer: it must never dirty the notes,
// which would be a 60 fps rebuild of every note vertex - exactly the failure this
// architecture exists to avoid. paintCounts() lets tests see that it does not.
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

class PianoRollItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(
        double playheadBeats READ playheadBeats WRITE setPlayheadBeats NOTIFY playheadChanged)
    Q_PROPERTY(double laneHeight READ laneHeight WRITE setLaneHeight NOTIFY laneHeightChanged)

public:
    /// Layers, in the order PianoRollGeometry::buffer() numbers them.
    enum Layer : int { kRows = 0, kGrid, kGhosts, kNotes, kLanes, kCurves, kLayerCount };

    explicit PianoRollItem(QQuickItem* parent = nullptr);

    /// The view transform: world (beats, 128 - pitch) to pixels. Changing it moves a
    /// matrix and nothing else.
    Q_INVOKABLE void setView(double beatStart, double worldTop, double pixelsPerBeat,
                             double pixelsPerRow);
    /// Copies one layer's vertices from engine memory (a GeometryLease's address), one
    /// memcpy. Call it inside the lease's `with` block.
    Q_INVOKABLE void upload(int layer, qulonglong address, int vertexCount, qulonglong revision);
    /// One colour per ColorRole, as QColor or "#rrggbb[aa]". Recolours every layer
    /// without any new geometry from the engine.
    Q_INVOKABLE void setPalette(const QVariantList& colors);
    Q_INVOKABLE void setSelectionRect(double x0, double y0, double x1, double y1, bool visible);
    Q_INVOKABLE [[nodiscard]] qulonglong layerRevision(int layer) const;
    Q_INVOKABLE [[nodiscard]] int layerVertexCount(int layer) const;
    /// [paint-node updates, geometry refills, transform-only updates] since creation.
    Q_INVOKABLE [[nodiscard]] QVariantList paintCounts() const;

    [[nodiscard]] double playheadBeats() const noexcept {
        return m_playheadBeats;
    }
    void setPlayheadBeats(double beats);
    [[nodiscard]] double laneHeight() const noexcept {
        return m_laneHeight;
    }
    void setLaneHeight(double height);

signals:
    void playheadChanged();
    void laneHeightChanged();
    /// Raw input, for Python's tools: positions in item pixels, Qt button/modifier ints.
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
    static void buildTree(QSGNode* root);
    void updateTransforms(Nodes& nodes);

    std::array<adx::quick::Layer, kLayerCount> m_layers;
    Palette m_palette;
    bool m_paletteDirty{true};
    double m_beatStart{0.0};
    double m_worldTop{44.0};
    double m_pixelsPerBeat{40.0};
    double m_pixelsPerRow{12.0};
    double m_playheadBeats{-1.0};
    double m_laneHeight{0.0};
    std::array<double, 4> m_selection{};
    bool m_selectionVisible{false};
    bool m_selectionDirty{false};
    bool m_playheadDirty{true};
    int m_paintUpdates{0};
    int m_geometryRefills{0};
    int m_transformOnly{0};
};

} // namespace adx::quick
