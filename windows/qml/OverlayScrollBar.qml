import QtQuick

// Overlay scroller (macOS style): a translucent thumb laid over the content's
// edge. It shows while the content moves or the pointer is on it, fades out
// after, and the thumb can be dragged. Hidden when everything already fits.
Item {
    id: bar
    property Flickable flickable
    property int orientation: Qt.Vertical
    readonly property bool vertical: orientation === Qt.Vertical
    readonly property real viewSize: vertical ? flickable.height : flickable.width
    readonly property real contentSize: vertical ? flickable.contentHeight : flickable.contentWidth
    readonly property real position: vertical ? flickable.contentY : flickable.contentX
    readonly property bool needed: contentSize > viewSize + 1
    readonly property bool active: needed && (recent.running || hover.containsMouse || hover.pressed)
    // Leaves room for the other bar in the corner they share.
    property real endInset: 0

    width: vertical ? 12 : flickable.width - endInset
    height: vertical ? flickable.height - endInset : 12
    x: vertical ? flickable.x + flickable.width - width : flickable.x
    y: vertical ? flickable.y : flickable.y + flickable.height - height
    z: 10
    visible: needed
    opacity: active ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: active ? 90 : 320 } }

    // Either direction of movement shows both bars, as on macOS.
    Connections {
        target: bar.flickable
        function onContentXChanged() { recent.restart() }
        function onContentYChanged() { recent.restart() }
    }
    Timer { id: recent; interval: 900 }

    readonly property real track: vertical ? height : width
    readonly property real thumbLength: needed ? Math.max(28, track * viewSize / contentSize) : track
    readonly property real thumbOffset: needed
        ? (track - thumbLength) * Math.max(0, Math.min(1, position / (contentSize - viewSize))) : 0

    Rectangle {
        property real thickness: hover.containsMouse || hover.pressed ? 8 : 6
        x: bar.vertical ? bar.width - thickness - 3 : bar.thumbOffset
        y: bar.vertical ? bar.thumbOffset : bar.height - thickness - 3
        width: bar.vertical ? thickness : bar.thumbLength
        height: bar.vertical ? bar.thumbLength : thickness
        radius: thickness / 2
        color: Qt.rgba(1, 1, 1, hover.pressed ? 0.55 : 0.38)
        Behavior on thickness { NumberAnimation { duration: 90 } }
    }

    MouseArea {
        id: hover
        anchors.fill: parent
        hoverEnabled: true
        preventStealing: true
        property real grabOffset: 0
        function scrollTo(along) {
            const ratio = (along - grabOffset) / Math.max(1, bar.track - bar.thumbLength)
            const target = Math.max(0, Math.min(1, ratio)) * (bar.contentSize - bar.viewSize)
            if (bar.vertical) bar.flickable.contentY = target
            else bar.flickable.contentX = target
        }
        onPressed: (mouse) => {
            const along = bar.vertical ? mouse.y : mouse.x
            const onThumb = along >= bar.thumbOffset && along <= bar.thumbOffset + bar.thumbLength
            // Pressing the track jumps there with the thumb centred under the pointer.
            grabOffset = onThumb ? along - bar.thumbOffset : bar.thumbLength / 2
            if (!onThumb) scrollTo(along)
        }
        onPositionChanged: (mouse) => { if (pressed) scrollTo(bar.vertical ? mouse.y : mouse.x) }
    }
}
