import QtQuick

// The default macOS push button in settings (bordered, grey), with red text
// for role: .destructive.
Rectangle {
    id: button
    property string text: ""
    property bool destructive: false
    signal clicked()

    width: Math.max(72, label.implicitWidth + 24)
    height: 26
    radius: 6
    color: mouse.pressed ? Qt.rgba(1, 1, 1, 0.20) : mouse.containsMouse ? Qt.rgba(1, 1, 1, 0.15) : Qt.rgba(1, 1, 1, 0.11)
    border.color: Qt.rgba(1, 1, 1, 0.06)
    opacity: enabled ? 1 : 0.45

    Text {
        id: label
        anchors.centerIn: parent
        text: button.text
        color: button.destructive ? "#FF6B61" : "white"
        font { family: "Segoe UI"; pixelSize: 13 }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: button.clicked()
    }
}
