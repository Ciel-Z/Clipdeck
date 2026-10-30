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

    // Windows icons fill their whole canvas while macOS icons keep the icon
    // grid margin (824 of 1024); inset to the same visible size.
    Image {
        anchors.centerIn: parent
        width: Math.round(badge.size * 0.84)
        height: width
        visible: badge.iconUrl !== ""
        source: badge.iconUrl
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
        asynchronous: true
    }

    Rectangle {
        anchors.centerIn: parent
        width: Math.round(badge.size * 0.84)
        height: width
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
