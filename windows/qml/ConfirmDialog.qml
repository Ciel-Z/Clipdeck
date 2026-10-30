import QtQuick
import QtQuick.Effects

// NSAlert / confirmationDialog drawn in the window. Buttons keep the macOS
// order: 取消 on the left, the action on the right (red when destructive).
Item {
    id: dialog
    property string title: ""
    property string message: ""
    property string confirmText: "确定"
    property bool destructive: false
    property var onConfirm: null
    anchors.fill: parent
    visible: false
    z: 2000

    function ask(titleText, messageText, confirmLabel, isDestructive, action) {
        title = titleText
        message = messageText
        confirmText = confirmLabel
        destructive = isDestructive
        onConfirm = action
        visible = true
        forceActiveFocus()
    }
    function close() { visible = false }

    Keys.onEscapePressed: close()
    Keys.onReturnPressed: { const action = onConfirm; close(); if (action) action() }

    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.45)
        MouseArea { anchors.fill: parent }
    }
    DropShadow { target: panel; blur: 30; offsetY: 8; strength: 0.5 }
    Rectangle {
        id: panel
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: 380
        height: content.implicitHeight + 44
        radius: 12
        color: "#2E2E32"
        border.color: Qt.rgba(1, 1, 1, 0.12)

        Column {
            id: content
            x: 22; y: 22
            width: parent.width - 44
            spacing: 10
            Image {
                source: "qrc:/icons/icon-64.png"
                width: 46; height: 46
                smooth: true
            }
            Text {
                width: parent.width
                text: dialog.title
                color: "white"
                wrapMode: Text.Wrap
                font { family: "Segoe UI"; pixelSize: 14; weight: Font.Bold }
            }
            Text {
                width: parent.width
                text: dialog.message
                color: Qt.rgba(1, 1, 1, 0.75)
                wrapMode: Text.Wrap
                lineHeight: 1.15
                font { family: "Segoe UI"; pixelSize: 12 }
            }
            Item { width: 1; height: 6 }
            Row {
                anchors.right: parent.right
                spacing: 8
                Rectangle {
                    width: Math.max(80, cancelLabel.implicitWidth + 28); height: 28; radius: 6
                    color: cancelMouse.containsMouse ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(1, 1, 1, 0.12)
                    Text { id: cancelLabel; anchors.centerIn: parent; text: "取消"; color: "white"; font { family: "Segoe UI"; pixelSize: 13 } }
                    MouseArea { id: cancelMouse; anchors.fill: parent; hoverEnabled: true; onClicked: dialog.close() }
                }
                Rectangle {
                    width: Math.max(80, confirmLabel.implicitWidth + 28); height: 28; radius: 6
                    color: dialog.destructive ? (confirmMouse.containsMouse ? Qt.rgba(1, 0.27, 0.23, 0.30) : Qt.rgba(1, 0.27, 0.23, 0.22))
                                              : (confirmMouse.containsMouse ? "#2B95FF" : "#0A84FF")
                    Text {
                        id: confirmLabel
                        anchors.centerIn: parent
                        text: dialog.confirmText
                        color: dialog.destructive ? "#FF6B61" : "white"
                        font { family: "Segoe UI"; pixelSize: 13; weight: Font.DemiBold }
                    }
                    MouseArea {
                        id: confirmMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: { const action = dialog.onConfirm; dialog.close(); if (action) action() }
                    }
                }
            }
        }
    }
}
