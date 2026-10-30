import QtQuick

// sourceBadge(size:): the application icon, or a neutral tile with the
// first letter of its name when the icon could not be read.
Item {
    id: badge
    property real size: 30
    property string iconUrl: ""
    property string name: ""
    width: size
    height: size

    Image {
        anchors.fill: parent
        visible: badge.iconUrl !== ""
        source: badge.iconUrl
        sourceSize: Qt.size(badge.size * 2, badge.size * 2)
        fillMode: Image.PreserveAspectFit
        smooth: true
        asynchronous: true
    }

    Rectangle {
        anchors.fill: parent
        visible: badge.iconUrl === ""
        radius: Math.max(4, badge.size * 0.20)
        gradient: Gradient {
            GradientStop { position: 0; color: "#3A3F4B" }
            GradientStop { position: 1; color: "#1D2027" }
        }
        border.color: Qt.rgba(1, 1, 1, 0.14)
        Text {
            anchors.centerIn: parent
            text: badge.name.length > 0 ? badge.name.charAt(0).toUpperCase() : "?"
            color: "#7FB6FF"
            font { family: "Segoe UI"; pixelSize: badge.size * 0.5; weight: Font.Bold }
        }
    }
}
