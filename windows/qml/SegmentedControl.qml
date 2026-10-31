import QtQuick

// Picker(.segmented): equal-width segments in a rounded track, the chosen one
// raised. options: [{ value, text }].
Rectangle {
    id: control
    property var options: []
    property var value
    signal activated(var value)

    readonly property real segmentWidth: 72
    width: segmentWidth * options.length + 4
    height: 28
    radius: 7
    color: Qt.rgba(1, 1, 1, 0.08)
    border.color: Qt.rgba(1, 1, 1, 0.06)

    Row {
        x: 2; y: 2
        Repeater {
            model: control.options
            delegate: Rectangle {
                required property var modelData
                readonly property bool chosen: modelData.value === control.value
                width: control.segmentWidth
                height: control.height - 4
                radius: 5
                color: chosen ? Qt.rgba(1, 1, 1, 0.20) : mouse.containsMouse ? Qt.rgba(1, 1, 1, 0.06) : "transparent"
                Text {
                    anchors.centerIn: parent
                    text: modelData.text
                    color: Qt.rgba(1, 1, 1, chosen ? 1 : 0.7)
                    font { family: "Segoe UI"; pixelSize: 13; weight: chosen ? Font.DemiBold : Font.Normal }
                }
                MouseArea {
                    id: mouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: control.activated(modelData.value)
                }
            }
        }
    }
}
