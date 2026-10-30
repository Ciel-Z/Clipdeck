import QtQuick

// Stand-in for the source application icon (real icons come from
// SHGetFileInfo in the product); same size and corner rule as sourceBadge().
Rectangle {
    property real size: 30
    property string letter: "A"
    width: size
    height: size
    radius: Math.max(4, size * 0.20)
    gradient: Gradient {
        GradientStop { position: 0; color: "#3A3F4B" }
        GradientStop { position: 1; color: "#1D2027" }
    }
    border.color: Qt.rgba(1, 1, 1, 0.14)
    Text {
        anchors.centerIn: parent
        text: parent.letter
        color: "#7FB6FF"
        font { family: "Segoe UI"; pixelSize: parent.size * 0.5; weight: Font.Bold }
    }
}
