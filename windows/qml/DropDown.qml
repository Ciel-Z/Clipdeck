import QtQuick
import QtQuick.Effects

// Picker(.menu): shows the current option; the list opens over the window
// (parented to the window's content item so it is not clipped).
Rectangle {
    id: dropdown
    // [{ value, text }]
    property var options: []
    property var value
    signal activated(var value)

    width: 170
    height: 28
    radius: 6
    color: mouse.containsMouse && enabled ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(1, 1, 1, 0.10)
    border.color: Qt.rgba(1, 1, 1, 0.08)
    opacity: enabled ? 1 : 0.45

    function currentText() {
        for (const option of options) if (option.value === value) return option.text
        return ""
    }

    Text {
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.right: chevron.left
        anchors.verticalCenter: parent.verticalCenter
        text: dropdown.currentText()
        color: "white"
        elide: Text.ElideRight
        font { family: "Segoe UI"; pixelSize: 13 }
    }
    DeckIcon {
        id: chevron
        anchors.right: parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        width: 12; height: 12
        kind: "chevron.updown"
        color: Qt.rgba(1, 1, 1, 0.65)
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: popup.visible = true
    }

    Item {
        id: popup
        parent: dropdown.Window.contentItem
        anchors.fill: parent
        visible: false
        z: 1000

        MouseArea { anchors.fill: parent; onClicked: popup.visible = false }
        DropShadow { target: list; blur: 18; offsetY: 4 }

        Rectangle {
            id: list
            readonly property point origin: popup.visible ? dropdown.mapToItem(popup, 0, dropdown.height + 4) : Qt.point(0, 0)
            x: Math.min(origin.x, popup.width - width - 8)
            y: Math.min(origin.y, popup.height - height - 8)
            width: Math.max(dropdown.width, 150)
            height: optionColumn.implicitHeight + 10
            radius: 8
            color: "#2E2E32"
            border.color: Qt.rgba(1, 1, 1, 0.12)
            Column {
                id: optionColumn
                x: 5; y: 5
                width: parent.width - 10
                Repeater {
                    model: dropdown.options
                    delegate: Rectangle {
                        required property var modelData
                        width: optionColumn.width
                        height: 26
                        radius: 5
                        color: optionMouse.containsMouse ? "#0A64D6" : "transparent"
                        Text {
                            x: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.value === dropdown.value ? "✓" : ""
                            color: "white"
                            font { family: "Segoe UI"; pixelSize: 12; weight: Font.Bold }
                        }
                        Text {
                            x: 26
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.text
                            color: "white"
                            font { family: "Segoe UI"; pixelSize: 13 }
                        }
                        MouseArea {
                            id: optionMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            onClicked: {
                                popup.visible = false
                                dropdown.activated(modelData.value)
                            }
                        }
                    }
                }
            }
        }
    }
}
