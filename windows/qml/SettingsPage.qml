import QtQuick

// A settings pane: 25 pt bold title, 13 pt secondary subtitle, then sections
// 24 pt apart; padding 38 × 26, at most maximumWidth wide and centred once the
// window is wider than that (a maximised window), scrolls vertically.
Flickable {
    id: page
    property string title: ""
    property string subtitle: ""
    property real maximumWidth: 760
    default property alias content: column.data

    contentWidth: width
    contentHeight: column.implicitHeight + 52
    clip: true
    boundsBehavior: Flickable.StopAtBounds

    Column {
        id: column
        x: Math.max(38, (page.width - width) / 2)
        y: 26
        width: Math.min(page.maximumWidth, page.width) - 76
        spacing: 24
        Column {
            spacing: 4
            Text {
                text: page.title
                color: "white"
                font { family: "Segoe UI"; pixelSize: 25; weight: Font.Bold }
            }
            Text {
                text: page.subtitle
                color: Qt.rgba(1, 1, 1, 0.55)
                font { family: "Segoe UI"; pixelSize: 13 }
            }
        }
    }
}
