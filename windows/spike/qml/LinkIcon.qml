import QtQuick

// linkIcon(size:) without a fetched favicon: white rounded tile, grey compass.
Rectangle {
    property real size: 42
    width: size
    height: size
    radius: size * 0.23
    color: Qt.rgba(1, 1, 1, 0.94)
    border.color: Qt.rgba(1, 1, 1, 0.18)
    Rectangle {
        anchors.centerIn: parent
        width: parent.size * 0.55
        height: width
        radius: width / 2
        color: "transparent"
        border.color: Qt.rgba(0, 0, 0, 0.22)
        border.width: Math.max(1, parent.size * 0.04)
        Rectangle {
            anchors.centerIn: parent
            width: parent.width * 0.16
            height: parent.height * 0.7
            rotation: 45
            radius: width / 2
            color: Qt.rgba(0, 0, 0, 0.22)
        }
    }
}
