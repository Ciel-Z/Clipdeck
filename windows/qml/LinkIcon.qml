import QtQuick

// linkIcon(size:): white rounded tile with the site icon. Without a favicon,
// a flat coloured tile with the site's initial, the colour picked from the
// host so the same site always looks the same (the glyph-tile family of the
// sync and folder tiles).
Rectangle {
    id: tile
    property real size: 42
    property string iconUrl: ""
    property string host: ""

    readonly property string site: host.replace(/^www\./i, "")
    readonly property var palette: ["#3B7DD8", "#1D9E75", "#7F5AD6", "#D8613B", "#C98A1B", "#C2457A"]
    readonly property color siteColor: {
        let hash = 0
        for (let i = 0; i < site.length; ++i) hash = (hash * 31 + site.charCodeAt(i)) >>> 0
        return palette[hash % palette.length]
    }

    width: size
    height: size
    radius: size * 0.23
    color: iconUrl !== "" ? Qt.rgba(1, 1, 1, 0.94) : siteColor
    border.color: iconUrl !== "" ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

    Image {
        anchors.fill: parent
        anchors.margins: tile.size * 0.13
        visible: tile.iconUrl !== ""
        source: tile.iconUrl
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
        asynchronous: true
    }

    Text {
        visible: tile.iconUrl === ""
        anchors.centerIn: parent
        text: tile.site.length > 0 ? tile.site.charAt(0).toUpperCase() : "#"
        color: "white"
        font { family: "Segoe UI"; pixelSize: tile.size * 0.52; weight: Font.DemiBold }
    }
}
