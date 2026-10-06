// The piano roll's QML: the C++ PianoRollItem filling the view, and the light overlays
// Python drives - a drag preview, the lyric cells and a hint line (phase_5.md 4.3).
//
// Pan and zoom never touch this file: they set PianoRollItem's view transform. The
// item's raw input is forwarded to `rollInput`, a Python object, which owns what a
// click means (tools.py).
import QtQuick
import Adx 1.0

Item {
    id: root

    // [x, y, width, height] rectangles of notes being dragged, in pixels.
    property var preview: []
    // [x, width, lyric, alias] per visible note, drawn in the lane strip.
    property var lyricCells: []
    property bool lyricsVisible: false
    property real laneTop: 0
    property string hint: ""

    // The lane strip's background; PianoRollItem draws its bars over it.
    Rectangle {
        x: 0
        y: root.laneTop
        width: root.width
        height: Math.max(0, root.height - root.laneTop)
        color: adxTheme.panel
        Rectangle {
            width: parent.width
            height: 1
            color: adxTheme.border
        }
    }

    PianoRollItem {
        id: roll
        objectName: "roll"
        anchors.fill: parent
        focus: true

        onPointerPressed: (x, y, button, modifiers) => rollInput.pressed(x, y, button, modifiers)
        onPointerMoved: (x, y, buttons, modifiers) => rollInput.moved(x, y, buttons, modifiers)
        onPointerReleased: (x, y, button, modifiers) => rollInput.released(x, y, button, modifiers)
        onPointerDoubleClicked: (x, y, modifiers) => rollInput.doubleClicked(x, y, modifiers)
        onHovered: (x, y, modifiers) => rollInput.hovered(x, y, modifiers)
        onWheeled: (x, y, dx, dy, modifiers) => rollInput.wheeled(x, y, dx, dy, modifiers)
        onKeyPressed: (key, modifiers, text) => rollInput.keyPressed(key, modifiers, text)
        onWidthChanged: rollInput.resized(width, height)
        onHeightChanged: rollInput.resized(width, height)
    }

    Repeater {
        model: root.preview
        delegate: Rectangle {
            required property var modelData
            x: modelData[0]
            y: modelData[1]
            width: Math.max(2, modelData[2])
            height: modelData[3]
            color: "transparent"
            border.color: adxTheme.noteSelected
            border.width: 1
            radius: 2
        }
    }

    Repeater {
        model: root.lyricsVisible ? root.lyricCells : []
        delegate: Column {
            required property var modelData
            x: modelData[0] + 2
            y: root.laneTop + 8
            width: Math.max(12, modelData[1] - 4)
            spacing: 2
            clip: true
            Text {
                text: modelData[2] === "" ? "·" : modelData[2]
                color: adxTheme.text
                font.pixelSize: 14
            }
            Text {
                text: modelData[3]
                color: adxTheme.textDim
                font.pixelSize: 10
            }
        }
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
