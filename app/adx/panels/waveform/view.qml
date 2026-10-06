// The waveform view's QML: one C++ WaveformItem; input goes to `waveInput` (Python).
import QtQuick
import Adx 1.0

Item {
    id: root

    WaveformItem {
        id: wave
        objectName: "wave"
        anchors.fill: parent

        onWheeled: (x, y, dx, dy, modifiers) => waveInput.wheeled(x, y, dx, dy, modifiers)
        onPointerPressed: (x, y, button, modifiers) => waveInput.pressed(x, y, button, modifiers)
        onPointerMoved: (x, y, buttons, modifiers) => waveInput.moved(x, y, buttons, modifiers)
        onWidthChanged: waveInput.resized(width, height)
        onHeightChanged: waveInput.resized(width, height)
    }
}
