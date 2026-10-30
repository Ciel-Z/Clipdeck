import QtQuick

// editorIconButton: 34×34 round hit area, 17 pt glyph at 88 %.
Item {
    id: button
    property string icon: ""
    property string tooltip: ""
    signal clicked()
    width: 34
    height: 34

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: Qt.rgba(1, 1, 1, mouse.pressed ? 0.14 : mouse.containsMouse ? 0.08 : 0)
    }
    DeckIcon {
        anchors.centerIn: parent
        width: 20; height: 20
        kind: button.icon
        color: Qt.rgba(1, 1, 1, 0.88)
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: button.clicked()
    }
}
