import QtQuick

// settingsRow(_:): 52 pt row, semibold title on the left, control on the right,
// and the inset divider that follows it (hidden on the last row).
Item {
    id: row
    property string title: ""
    property bool divider: true
    default property alias control: slot.data
    width: parent ? parent.width : 600
    height: 52

    Text {
        anchors.left: parent.left
        anchors.leftMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        text: row.title
        color: "white"
        font { family: "Segoe UI"; pixelSize: 14; weight: Font.DemiBold }
    }
    Item {
        id: slot
        anchors.right: parent.right
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        width: childrenRect.width
        height: childrenRect.height
    }
    Rectangle {
        visible: row.divider
        anchors.bottom: parent.bottom
        x: 16
        width: parent.width - 16
        height: 1
        color: Qt.rgba(1, 1, 1, 0.08)
    }
}
