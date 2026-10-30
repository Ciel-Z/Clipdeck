import QtQuick

// RotatedImageView: fits the image into the frame after rotating it by a
// multiple of 90°, swapping the frame for quarter turns.
Item {
    id: frame
    property string source: ""
    property int angle: 0
    readonly property bool quarter: (angle / 90) % 2 !== 0

    Image {
        anchors.centerIn: parent
        width: frame.quarter ? frame.height : frame.width
        height: frame.quarter ? frame.width : frame.height
        rotation: frame.angle
        source: frame.source
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        smooth: true
        mipmap: true
        Behavior on rotation { RotationAnimation { duration: 160; direction: RotationAnimation.Shortest } }
    }
}
