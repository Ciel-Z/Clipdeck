import QtQuick

// linkIcon(size:): white rounded tile with the site icon, or a grey compass
// when no favicon has been fetched yet.
Rectangle {
    id: tile
    property real size: 42
    property string iconUrl: ""
    width: size
    height: size
    radius: size * 0.23
    color: Qt.rgba(1, 1, 1, 0.94)
    border.color: Qt.rgba(1, 1, 1, 0.18)

    Image {
        anchors.fill: parent
        anchors.margins: tile.size * 0.13
        visible: tile.iconUrl !== ""
        source: tile.iconUrl
        fillMode: Image.PreserveAspectFit
        smooth: true
        asynchronous: true
    }

    Rectangle {
        visible: tile.iconUrl === ""
        anchors.centerIn: parent
        width: tile.size * 0.55
        height: width
        radius: width / 2
        color: "transparent"
        border.color: Qt.rgba(0, 0, 0, 0.22)
        border.width: Math.max(1, tile.size * 0.04)
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
