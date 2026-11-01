import QtQuick
import QtQuick.Shapes

// sourceBadge(size:): the application icon, or a neutral tile with the
// first letter of its name when the icon could not be read. Items synced from
// another device show a teal two-way-arrow tile instead (their icon does not
// travel; the source application is still named in the details), and file
// cards an amber folder tile from the same family.
Item {
    id: badge
    property real size: 30
    property string iconUrl: ""
    property string name: ""
    property bool fromOtherDevice: false
    property bool isFiles: false
    readonly property bool glyphTile: fromOtherDevice || isFiles
    width: size
    height: size

    // Windows icons fill their whole canvas while macOS icons keep the icon
    // grid margin (824 of 1024); inset to the same visible size.
    Image {
        anchors.centerIn: parent
        width: Math.round(badge.size * 0.84)
        height: width
        visible: badge.iconUrl !== "" && !badge.glyphTile
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
        visible: badge.iconUrl === "" && !badge.glyphTile
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

    // Same geometry on mac and Linux: the tile of the letter fallback, a white
    // glyph on a 24-unit grid; small badges get a denser, bolder glyph.
    Rectangle {
        id: glyphTile
        anchors.centerIn: parent
        width: Math.round(badge.size * 0.84)
        height: width
        visible: badge.glyphTile
        radius: Math.max(4, badge.size * 0.20)
        color: badge.isFiles ? "#E5A43A" : "#1D9E75"
        readonly property bool small: width < 20
        Shape {
            anchors.centerIn: parent
            width: 24; height: 24
            scale: glyphTile.width * (glyphTile.small ? 0.86 : badge.isFiles ? 0.75 : 0.66) / 24
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                fillColor: "transparent"
                strokeColor: "white"
                strokeWidth: glyphTile.small ? 2.9 : badge.isFiles ? 2.4 : 2.3
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                // Folder with its tab line, or two arrows passing each other.
                PathSvg {
                    path: badge.isFiles
                        ? "M3.5 8V17.5Q3.5 19 5 19H19Q20.5 19 20.5 17.5V9.5Q20.5 8 19 8H11.5L9.8 5.8Q9.4 5.3 8.8 5.3H5Q3.5 5.3 3.5 6.8ZM3.5 9.5H20.5"
                        : "M5 8.5H18M14.5 5L18 8.5L14.5 12M19 15.5H6M9.5 12L6 15.5L9.5 19"
                }
            }
        }
    }
}
