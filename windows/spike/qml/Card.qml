import QtQuick
import QtQuick.Effects
import QtQuick.Shapes

// Port of ClipboardCardView (HistoryPanelView.swift), reduced to the content
// kinds that stress rendering: text, code, link, color and image.
Item {
    id: card

    required property var entry
    property real side: 300
    property bool selected: false
    property int quickNumber: 0
    property bool shadowsEnabled: true
    property bool masksEnabled: true

    readonly property bool expanded: side >= 180
    readonly property real radius: expanded ? 18 : 15
    readonly property real headerHeight: Math.min(58, Math.max(48, side * 0.20))
    readonly property bool immersive: entry.kind === "image" || entry.kind === "color"
    readonly property string uiFont: "Segoe UI"
    readonly property string monoFont: "Consolas"

    width: side
    height: side

    Rectangle {
        id: base
        anchors.fill: parent
        radius: card.radius
        color: "#131315"
        layer.enabled: card.shadowsEnabled
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: card.selected ? Qt.rgba(0, 0.48, 1, 0.24) : Qt.rgba(0, 0, 0, 0.28)
            blurMax: 18
            shadowBlur: 1.0
            shadowVerticalOffset: 4
        }
    }

    Rectangle {
        id: maskShape
        anchors.fill: parent
        radius: card.radius
        visible: false
        layer.enabled: true
        layer.smooth: true
    }

    Item {
        id: content
        anchors.fill: parent
        clip: !card.masksEnabled
        layer.enabled: card.masksEnabled
        layer.smooth: true
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: maskShape
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }

        Loader {
            id: body
            anchors.fill: parent
            anchors.topMargin: card.expanded && !card.immersive ? card.headerHeight : 0
            sourceComponent: card.entry.kind === "color" ? colorBody
                : card.entry.kind === "image" ? imageBody
                : card.entry.kind === "link" ? linkBody
                : textBody
        }

        Shape {
            id: headerBackground
            visible: card.expanded && !card.immersive
            width: card.width
            height: card.headerHeight
            ShapePath {
                strokeWidth: -1
                fillGradient: LinearGradient {
                    x1: 0; y1: 0
                    x2: card.width; y2: card.headerHeight
                    GradientStop { position: 0; color: card.selected ? "#032459" : "#459EED" }
                    GradientStop { position: 1; color: card.selected ? "#054299" : "#2178E0" }
                }
                startX: 0; startY: 0
                PathLine { x: card.width; y: 0 }
                PathLine { x: card.width; y: card.headerHeight }
                PathLine { x: 0; y: card.headerHeight }
                PathLine { x: 0; y: 0 }
            }
        }

        Item {
            id: expandedHeader
            visible: card.expanded
            width: card.width
            height: card.headerHeight
            layer.enabled: card.immersive && card.shadowsEnabled
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: Qt.rgba(0, 0, 0, 0.42)
                blurMax: 6
                shadowBlur: 1.0
                shadowVerticalOffset: 1
            }

            Column {
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.right: badge.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1
                Row {
                    spacing: 7
                    width: parent.width
                    Text {
                        text: card.entry.title
                        color: "white"
                        font { family: card.uiFont; pixelSize: 14; weight: Font.Bold }
                        elide: Text.ElideRight
                        width: Math.min(implicitWidth, parent.width * 0.6)
                    }
                    Text {
                        text: card.entry.category
                        color: Qt.rgba(1, 1, 1, 0.72)
                        font { family: card.uiFont; pixelSize: 12; weight: Font.DemiBold }
                        topPadding: 2
                    }
                }
                Text {
                    text: card.entry.time
                    color: Qt.rgba(1, 1, 1, 0.78)
                    font { family: card.uiFont; pixelSize: 11.5; weight: Font.DemiBold }
                }
            }

            SourceBadge {
                id: badge
                size: card.headerHeight - 8
                letter: card.entry.source
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Row {
            id: compactHeader
            visible: !card.expanded
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: 11
            anchors.rightMargin: 11
            anchors.topMargin: 10
            spacing: 5
            Text {
                text: card.entry.title
                color: Qt.rgba(1, 1, 1, 0.90)
                font { family: card.uiFont; pixelSize: 11.5; weight: Font.Bold }
                elide: Text.ElideRight
                width: Math.min(implicitWidth, compactHeader.width * 0.4)
            }
            Text {
                text: card.entry.category
                color: Qt.rgba(1, 1, 1, 0.70)
                font { family: card.uiFont; pixelSize: 11.5; weight: Font.DemiBold }
            }
            Text {
                text: card.entry.time
                color: Qt.rgba(1, 1, 1, 0.62)
                font { family: card.uiFont; pixelSize: 11.5; weight: Font.DemiBold }
            }
        }
        SourceBadge {
            visible: !card.expanded
            size: 18
            letter: card.entry.source
            anchors.right: parent.right
            anchors.rightMargin: 11
            anchors.top: parent.top
            anchors.topMargin: 9
        }
    }

    Rectangle {
        anchors.fill: parent
        radius: card.radius
        color: "transparent"
        border.color: card.selected ? "#0A8FFF" : Qt.rgba(1, 1, 1, 0.09)
        border.width: card.selected ? 4 : 1
        Behavior on border.width { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
    }

    Rectangle {
        visible: card.quickNumber > 0
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 9
        width: Math.max(25, numberLabel.implicitWidth + 10)
        height: 25
        radius: 7
        color: Qt.rgba(0, 0, 0, card.selected ? 0.66 : 0.48)
        Text {
            id: numberLabel
            anchors.centerIn: parent
            text: card.quickNumber
            color: Qt.rgba(1, 1, 1, 0.94)
            font { family: card.uiFont; pixelSize: 14; weight: Font.Bold }
        }
    }

    Component {
        id: textBody
        Item {
            Text {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                anchors.topMargin: card.expanded ? 13 : 37
                anchors.bottomMargin: card.expanded ? 34 : 12
                text: card.entry.styled
                textFormat: card.entry.mono ? Text.StyledText : Text.PlainText
                color: Qt.rgba(1, 1, 1, 0.90)
                font { family: card.entry.mono ? card.monoFont : card.uiFont; pixelSize: 13.5 }
                lineHeightMode: Text.FixedHeight
                lineHeight: 20
                wrapMode: Text.WrapAnywhere
                elide: Text.ElideRight
                clip: true
            }
            Text {
                visible: card.expanded
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 10
                text: card.entry.plain.length + " 个字符"
                color: Qt.rgba(1, 1, 1, 0.52)
                font { family: card.uiFont; pixelSize: 11; weight: Font.Medium }
            }
        }
    }

    Component {
        id: colorBody
        Rectangle {
            color: card.entry.plain
            Rectangle {
                visible: card.expanded
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 12
                width: colorLabel.implicitWidth + 20
                height: colorLabel.implicitHeight + 12
                radius: height / 2
                color: Qt.rgba(0, 0, 0, 0.48)
                Text {
                    id: colorLabel
                    anchors.centerIn: parent
                    text: card.entry.plain.replace("#", "").toUpperCase()
                    color: "white"
                    font { family: card.monoFont; pixelSize: 13; weight: Font.Bold }
                }
            }
        }
    }

    Component {
        id: imageBody
        Item {
            Image {
                anchors.fill: parent
                source: "image://spike/checker"
                fillMode: Image.Tile
                smooth: false
            }
            Image {
                anchors.fill: parent
                anchors.margins: 8
                source: "image://spike/photo/" + card.entry.seed
                sourceSize: Qt.size(card.side * 2, card.side * 2)
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                cache: true
            }
            Rectangle {
                visible: card.expanded
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 9
                width: sizeLabel.implicitWidth + 18
                height: sizeLabel.implicitHeight + 10
                radius: height / 2
                color: Qt.rgba(0, 0, 0, 0.58)
                Text {
                    id: sizeLabel
                    anchors.centerIn: parent
                    text: card.entry.plain
                    color: Qt.rgba(1, 1, 1, 0.82)
                    font { family: card.monoFont; pixelSize: 11; weight: Font.DemiBold }
                }
            }
        }
    }

    Component {
        id: linkBody
        Item {
            readonly property real footerHeight: Math.min(88, Math.max(72, card.side * 0.23))
            LinkIcon {
                size: card.expanded ? Math.min(58, Math.max(42, card.side * 0.25)) : 36
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: card.expanded ? -parent.footerHeight / 2 : 9
            }
            Row {
                visible: card.expanded
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 12
                height: parent.footerHeight - 20
                spacing: 10
                LinkIcon { size: 32; anchors.verticalCenter: parent.verticalCenter }
                Column {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    width: parent.width - 42
                    Text {
                        width: parent.width
                        text: card.entry.title
                        color: Qt.rgba(1, 1, 1, 0.92)
                        font { family: card.uiFont; pixelSize: 13.5; weight: Font.DemiBold }
                        maximumLineCount: 2
                        wrapMode: Text.Wrap
                        elide: Text.ElideRight
                    }
                    Text {
                        text: card.entry.host
                        color: Qt.rgba(1, 1, 1, 0.50)
                        font { family: card.uiFont; pixelSize: 11.5; weight: Font.Medium }
                    }
                }
            }
        }
    }
}
