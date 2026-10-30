import QtQuick

// Capsule buttons of ContentEditorView (取消 white 7 %, 保存 accent) and the
// small bordered buttons of RenameItemView / 格式化.
Rectangle {
    id: button
    property string text: ""
    property bool accent: false
    property bool small: false
    signal clicked()

    width: small ? Math.max(52, label.implicitWidth + 22) : 68
    height: small ? 26 : 34
    radius: height / 2
    color: accent ? (mouse.pressed ? "#0068D6" : "#0A84FF")
                  : Qt.rgba(1, 1, 1, mouse.pressed ? 0.16 : mouse.containsMouse ? 0.11 : 0.07)
    border.color: small && !accent ? Qt.rgba(1, 1, 1, 0.14) : "transparent"

    Text {
        id: label
        anchors.centerIn: parent
        text: button.text
        color: "white"
        font { family: "Segoe UI"; pixelSize: button.small ? 12 : 13; weight: button.accent ? Font.DemiBold : Font.Medium }
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: button.clicked()
    }
}
