// A zoomable waveform's scene-graph item (phase_5.md §4.5).
//
// One triangle strip of min/max pairs, built by WaveformGeometry in world space
// (seconds, amplitude) and placed by a transform, so a pan or zoom inside the built
// range moves a matrix and rebuilds nothing. Reused by Phase 6's playlist clips.
#pragma once

#include <QtCore/QVariantList>
#include <QtQuick/QQuickItem>

#include "bindings/qml/SceneLayers.h"

class QSGTransformNode;
class QSGGeometryNode;

namespace adx::quick {

class WaveformItem : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(
        double playheadSeconds READ playheadSeconds WRITE setPlayheadSeconds NOTIFY playheadChanged)

public:
    explicit WaveformItem(QQuickItem* parent = nullptr);

    /// World seconds at the left edge, and pixels per second.
    Q_INVOKABLE void setView(double secondsStart, double pixelsPerSecond);
    Q_INVOKABLE void upload(qulonglong address, int vertexCount, qulonglong revision);
    Q_INVOKABLE void setPalette(const QVariantList& colors);
    Q_INVOKABLE [[nodiscard]] qulonglong revision() const;
    /// [paint-node updates, geometry refills].
    Q_INVOKABLE [[nodiscard]] QVariantList paintCounts() const;

    [[nodiscard]] double playheadSeconds() const noexcept {
        return m_playheadSeconds;
    }
    void setPlayheadSeconds(double seconds);

signals:
    void playheadChanged();
    void pointerPressed(double x, double y, int button, int modifiers);
    void pointerMoved(double x, double y, int buttons, int modifiers);
    void pointerReleased(double x, double y, int button, int modifiers);
    void wheeled(double x, double y, double deltaX, double deltaY, int modifiers);

protected:
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData* data) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;

private:
    struct Nodes;

    Layer m_strip;
    Palette m_palette;
    bool m_paletteDirty{true};
    bool m_playheadDirty{true};
    double m_secondsStart{0.0};
    double m_pixelsPerSecond{100.0};
    double m_playheadSeconds{-1.0};
    int m_paintUpdates{0};
    int m_geometryRefills{0};
};

} // namespace adx::quick
