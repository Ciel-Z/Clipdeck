import QtQuick

// SwiftUI Toggle (switch style) in the accent colour.
Item {
    id: toggle
    property bool checked: false
    signal toggled(bool checked)
    width: 40
    height: 24
    opacity: enabled ? 1 : 0.45

    Rectangle {
        anchors.fill: parent
        radius: height / 2
        color: toggle.checked ? "#0A84FF" : Qt.rgba(1, 1, 1, 0.16)
        Behavior on color { ColorAnimation { duration: 140 } }
        Rectangle {
            width: 20; height: 20; radius: 10
            y: 2
            x: toggle.checked ? parent.width - width - 2 : 2
            color: "white"
            Behavior on x { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
        }
    }
    MouseArea {
        anchors.fill: parent
        onClicked: toggle.toggled(!toggle.checked)
    }
}
