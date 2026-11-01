import QtQuick

// The image on an image card. Close to the card's shape (within 1.34×, so 4:3,
// 3:4 and squarish images) it fills the card and loses a little at the
// edges; otherwise the whole image is fitted edge to edge and the bands beside
// it show a dimmed, softened copy of the same image instead of empty space.
// The checkerboard sits only behind the image, for its transparent parts.
// Same rule as the Linux and mac cards; the preview window still shows it all.
Item {
    id: frame
    property string source: ""
    property int angle: 0
    readonly property bool quarter: (angle / 90) % 2 !== 0
    readonly property real imageAspect: {
        // sourceSize is the image's own size; implicitWidth follows fillMode
        // and would loop through `fills`.
        const w = picture.sourceSize.width, h = picture.sourceSize.height
        if (w <= 0 || h <= 0) return width / Math.max(1, height)
        return quarter ? h / w : w / h
    }
    readonly property real frameAspect: width / Math.max(1, height)
    readonly property bool fills: Math.max(imageAspect / frameAspect, frameAspect / imageAspect) <= 1.34

    clip: true

    // A 16 px copy scaled up: a soft backdrop without a blur pass.
    Image {
        visible: !frame.fills
        anchors.centerIn: parent
        width: frame.quarter ? frame.height : frame.width
        height: frame.quarter ? frame.width : frame.height
        rotation: frame.angle
        source: frame.source
        sourceSize: Qt.size(16, 16)
        fillMode: Image.PreserveAspectCrop
        smooth: true
        asynchronous: true
    }
    Rectangle {
        visible: !frame.fills
        anchors.fill: parent
        color: Qt.rgba(0.07, 0.07, 0.08, 0.55)
    }

    Item {
        anchors.centerIn: parent
        width: frame.quarter ? frame.height : frame.width
        height: frame.quarter ? frame.width : frame.height
        rotation: frame.angle
        Behavior on rotation { RotationAnimation { duration: 160; direction: RotationAnimation.Shortest } }

        Checkerboard {
            anchors.centerIn: parent
            width: frame.fills ? parent.width : picture.paintedWidth
            height: frame.fills ? parent.height : picture.paintedHeight
        }
        Image {
            id: picture
            anchors.fill: parent
            source: frame.source
            fillMode: frame.fills ? Image.PreserveAspectCrop : Image.PreserveAspectFit
            asynchronous: true
            smooth: true
            mipmap: true
        }
    }
}
