// A GeometryItem filling the view, plus text labels Python positions (clip names, track
// rows). The C++ item draws every vertex; this file only forwards its input to
// `viewInput`, a Python object that owns what a click means (phase_6.md 4.1).
import QtQuick
import Adx 1.0

Item {
    id: root

    // [x, y, width, text] per label, in pixels.
    property var labels: []
    property string hint: ""
    property var band: []

    GeometryItem {
        id: view
        objectName: "view"
        anchors.fill: parent
        focus: true

        onPointerPressed: (x, y, button, modifiers) => viewInput.pressed(x, y, button, modifiers)
        onPointerMoved: (x, y, buttons, modifiers) => viewInput.moved(x, y, buttons, modifiers)
        onPointerReleased: (x, y, button, modifiers) => viewInput.released(x, y, button, modifiers)
        onPointerDoubleClicked: (x, y, modifiers) => viewInput.doubleClicked(x, y, modifiers)
        onHovered: (x, y, modifiers) => viewInput.hovered(x, y, modifiers)
        onWheeled: (x, y, dx, dy, modifiers) => viewInput.wheeled(x, y, dx, dy, modifiers)
        onKeyPressed: (key, modifiers, text) => viewInput.keyPressed(key, modifiers, text)
        onWidthChanged: viewInput.resized(width, height)
        onHeightChanged: viewInput.resized(width, height)
    }

    Repeater {
        model: root.labels
        delegate: Text {
            required property var modelData
            x: modelData[0] + 3
            y: modelData[1] + 1
            width: Math.max(0, modelData[2] - 6)
            elide: Text.ElideRight
            text: modelData[3]
            color: adxTheme.text
            font.pixelSize: 11
        }
    }

    // The rubber band's outline while a selection is dragged out: [x0, y0, x1, y1],
    // or empty. The item draws its translucent fill; this makes its edge plain.
    Rectangle {
        visible: root.band.length === 4
        x: visible ? Math.min(root.band[0], root.band[2]) : 0
        y: visible ? Math.min(root.band[1], root.band[3]) : 0
        width: visible ? Math.abs(root.band[2] - root.band[0]) : 0
        height: visible ? Math.abs(root.band[3] - root.band[1]) : 0
        color: "transparent"
        border.color: adxTheme.accent
        border.width: 1
    }

    Text {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 6
        text: root.hint
        color: adxTheme.textDim
        font.pixelSize: 11
    }
}
