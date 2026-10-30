import QtQuick

// FileReferenceList(compact: false): one row per file with icon, name and folder.
ListView {
    id: list
    property var files: []
    model: files
    clip: true
    topMargin: 10
    bottomMargin: 10
    boundsBehavior: Flickable.StopAtBounds
    delegate: Item {
        required property var modelData
        width: list.width
        height: 46
        DeckIcon {
            id: fileIcon
            x: 22
            width: 24; height: 24
            anchors.verticalCenter: parent.verticalCenter
            kind: modelData.isDirectory ? "folder" : "doc"
            color: Qt.rgba(1, 1, 1, 0.82)
        }
        Column {
            anchors.left: fileIcon.right
            anchors.leftMargin: 12
            anchors.right: parent.right
            anchors.rightMargin: 22
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Text {
                width: parent.width
                text: modelData.name
                color: Qt.rgba(1, 1, 1, 0.92)
                font { family: "Segoe UI"; pixelSize: 13; weight: Font.DemiBold }
                elide: Text.ElideMiddle
            }
            Text {
                width: parent.width
                text: modelData.path
                color: Qt.rgba(1, 1, 1, 0.48)
                font { family: "Segoe UI"; pixelSize: 11 }
                elide: Text.ElideMiddle
            }
        }
    }
}
