import QtQuick

// settingsSection(_:): bold title above a rounded group (radius 14,
// controlBackgroundColor, white 4.5 % hairline).
Column {
    id: section
    property string title: ""
    default property alias content: body.data
    width: parent ? parent.width : 600
    spacing: 10

    Text {
        text: section.title
        color: "white"
        font { family: "Segoe UI"; pixelSize: 16; weight: Font.Bold }
    }
    Rectangle {
        width: parent.width
        height: body.implicitHeight
        radius: 14
        color: "#2A2A2D"
        border.color: Qt.rgba(1, 1, 1, 0.045)
        clip: true
        Column {
            id: body
            width: parent.width
        }
    }
}
