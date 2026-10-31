import QtQuick

// The default macOS push button in settings (bordered, grey), with red text
// for role: .destructive, or blue for .borderedProminent (accent).
Rectangle {
    id: button
    property string text: ""
    property bool destructive: false
    property bool accent: false
    signal clicked()

    width: Math.max(72, label.implicitWidth + 24)
    height: 26
    radius: 6
    color: accent ? (mouse.pressed ? "#0068D6" : mouse.containsMouse ? "#2B95FF" : "#0A84FF")
         : mouse.pressed ? Qt.rgba(1, 1, 1, 0.20) : mouse.containsMouse ? Qt.rgba(1, 1, 1, 0.15) : Qt.rgba(1, 1, 1, 0.11)
    border.color: accent ? "transparent" : Qt.rgba(1, 1, 1, 0.06)
    opacity: enabled ? 1 : 0.45

    Text {
        id: label
        anchors.centerIn: parent
        text: button.text
        color: button.destructive ? "#FF6B61" : "white"
        font { family: "Segoe UI"; pixelSize: 13; weight: button.accent ? Font.DemiBold : Font.Normal }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: button.clicked()
    }
}
