import QtQuick
import QtQuick.Effects
import QtQuick.Shapes

// Port of ClipboardCardView (HistoryPanelView.swift): text, link, color,
// image and file cards with expanded / compact headers.
Item {
    id: card

    property string kind: "text"
    property string title: ""
    property string categoryLabel: ""
    property string time: ""
    property string text: ""
    property var characterCount: 0
    property bool monospaced: false
    property string imageUrl: ""
    property string imageSizeText: ""
    property int imageAngle: 0
    property var files: []
    property string sourceIconUrl: ""
    property string sourceName: ""
    property string linkTitle: ""
    property string linkHost: ""
    property string linkIconUrl: ""
    property string linkPreviewUrl: ""
    property var colorValue
    property string colorText: ""
    property bool immersive: false

    property real side: 300
    property bool selected: false
    property int quickNumber: 0
    property bool shadowsEnabled: true

    readonly property bool expanded: side >= 180
    readonly property real radius: expanded ? 18 : 15
    readonly property real headerHeight: Math.min(58, Math.max(48, side * 0.20))
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
        layer.enabled: true
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
            sourceComponent: card.kind === "color" ? colorBody
                : card.kind === "image" ? imageBody
                : card.kind === "link" ? linkBody
                : card.kind === "files" ? filesBody
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
                    GradientStop {
                        position: 0
                        color: card.kind === "files" ? "#246BB9" : card.selected ? "#032459" : "#459EED"
                    }
                    GradientStop {
                        position: 1
                        color: card.kind === "files" ? "#2161AD" : card.selected ? "#054299" : "#2178E0"
                    }
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
                anchors.right: headerIcon.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 1
                Row {
                    spacing: 7
                    width: parent.width
                    Text {
                        id: headerTitle
                        text: card.title
                        color: "white"
                        font { family: card.uiFont; pixelSize: 14; weight: Font.Bold }
                        elide: Text.ElideRight
                        width: Math.min(implicitWidth, parent.width * 0.65)
                    }
                    Text {
                        visible: card.categoryLabel !== ""
                        text: card.categoryLabel
                        color: Qt.rgba(1, 1, 1, 0.72)
                        font { family: card.uiFont; pixelSize: 12; weight: Font.DemiBold }
                        topPadding: 2
                        elide: Text.ElideRight
                        width: Math.min(implicitWidth, parent.width - headerTitle.width - 7)
                    }
                }
                Text {
                    text: card.time
                    color: Qt.rgba(1, 1, 1, 0.78)
                    font { family: card.uiFont; pointSize: 11.5 * 0.75; weight: Font.DemiBold }
                }
            }

            Item {
                id: headerIcon
                width: card.kind === "files" ? 30 : card.headerHeight - 8
                height: width
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                DeckIcon {
                    visible: card.kind === "files"
                    anchors.fill: parent
                    kind: "folder"
                    color: "white"
                }
                SourceBadge {
                    visible: card.kind !== "files"
                    size: parent.width
                    iconUrl: card.sourceIconUrl
                    name: card.sourceName
                }
            }
        }

        Row {
            id: compactHeader
            visible: !card.expanded
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: 11
            anchors.rightMargin: 11 + 18 + 6
            anchors.topMargin: 10
            spacing: 5
            layer.enabled: card.immersive && card.shadowsEnabled
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: Qt.rgba(0, 0, 0, 0.55)
                blurMax: 6
                shadowBlur: 1.0
                shadowVerticalOffset: 1
            }
            Text {
                text: card.title
                color: Qt.rgba(1, 1, 1, 0.90)
                font { family: card.uiFont; pointSize: 11.5 * 0.75; weight: Font.Bold }
                elide: Text.ElideRight
                width: Math.min(implicitWidth, compactHeader.width * 0.45)
            }
            Text {
                visible: card.categoryLabel !== ""
                text: card.categoryLabel
                color: Qt.rgba(1, 1, 1, 0.70)
                font { family: card.uiFont; pointSize: 11.5 * 0.75; weight: Font.DemiBold }
                elide: Text.ElideRight
                width: Math.min(implicitWidth, compactHeader.width * 0.3)
            }
            Text {
                text: card.time
                color: Qt.rgba(1, 1, 1, 0.62)
                font { family: card.uiFont; pointSize: 11.5 * 0.75; weight: Font.DemiBold }
            }
        }
        SourceBadge {
            visible: !card.expanded
            size: 18
            iconUrl: card.sourceIconUrl
            name: card.sourceName
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
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 9
        width: Math.max(25, numberLabel.implicitWidth + 10)
        height: 25
        radius: 7
        color: Qt.rgba(0, 0, 0, card.selected ? 0.66 : 0.48)
        opacity: card.quickNumber > 0 ? 1 : 0
        scale: card.quickNumber > 0 ? 1 : 0.88
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: 100; easing.type: Easing.OutCubic } }
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
                text: card.text
                textFormat: Text.PlainText
                color: Qt.rgba(1, 1, 1, 0.90)
                font { family: card.monospaced ? card.monoFont : card.uiFont; pointSize: 13.5 * 0.75 }
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
                text: card.characterCount + " 个字符"
                color: Qt.rgba(1, 1, 1, 0.52)
                font { family: card.uiFont; pixelSize: 11; weight: Font.Medium }
            }
        }
    }

    Component {
        id: colorBody
        Rectangle {
            color: card.colorValue !== undefined ? card.colorValue : "black"
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
                    text: card.colorText
                    color: "white"
                    font { family: card.monoFont; pixelSize: 13; weight: Font.Bold }
                }
            }
        }
    }

    Component {
        id: imageBody
        Item {
            Checkerboard { anchors.fill: parent }
            RotatedImage {
                anchors.fill: parent
                anchors.margins: 8
                source: card.imageUrl
                angle: card.imageAngle
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
                    text: card.imageSizeText
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
            Image {
                id: preview
                anchors.fill: parent
                visible: card.linkPreviewUrl !== ""
                source: card.linkPreviewUrl
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
            }
            LinkIcon {
                visible: card.linkPreviewUrl === ""
                size: card.expanded ? Math.min(58, Math.max(42, card.side * 0.25)) : 36
                iconUrl: card.linkIconUrl
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                anchors.verticalCenterOffset: card.expanded ? -parent.footerHeight / 2 : 9
            }
            Rectangle {
                visible: card.expanded
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: parent.footerHeight
                color: Qt.rgba(0, 0, 0, preview.visible ? 0.55 : 0.0)
                Row {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 10
                    LinkIcon { size: 32; iconUrl: card.linkIconUrl; anchors.verticalCenter: parent.verticalCenter }
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 2
                        width: parent.width - 42
                        Text {
                            width: parent.width
                            text: card.linkTitle
                            color: Qt.rgba(1, 1, 1, 0.92)
                            font { family: card.uiFont; pointSize: 13.5 * 0.75; weight: Font.DemiBold }
                            maximumLineCount: 2
                            wrapMode: Text.Wrap
                            elide: Text.ElideRight
                        }
                        Text {
                            width: parent.width
                            text: card.linkHost
                            color: Qt.rgba(1, 1, 1, 0.50)
                            font { family: card.uiFont; pointSize: 11.5 * 0.75; weight: Font.Medium }
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }
    }

    Component {
        id: filesBody
        // FileReferenceList(compact: true)
        Item {
            id: fileList
            readonly property int count: card.files ? card.files.length : 0
            readonly property real available: Math.max(0, height - (card.expanded ? 24 : 54))
            readonly property bool overflow: count * 30 - 4 > available
            readonly property int visibleRows: Math.max(0, Math.min(count, Math.floor((available - (overflow ? 22 : 0) + 4) / 30)))

            // Single file: large icon, name and folder.
            Column {
                visible: fileList.count === 1
                anchors.centerIn: parent
                anchors.verticalCenterOffset: card.expanded ? 0 : 12
                width: parent.width - 28
                spacing: 6
                DeckIcon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: card.expanded ? 46 : 32
                    height: width
                    kind: fileList.count === 1 && card.files[0].isDirectory ? "folder" : "doc"
                    color: Qt.rgba(1, 1, 1, 0.86)
                }
                Text {
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: fileList.count === 1 ? card.files[0].name : ""
                    color: Qt.rgba(1, 1, 1, 0.92)
                    font { family: card.uiFont; pixelSize: 13; weight: Font.DemiBold }
                    elide: Text.ElideMiddle
                }
                Text {
                    visible: card.expanded
                    width: parent.width
                    horizontalAlignment: Text.AlignHCenter
                    text: fileList.count === 1 ? card.files[0].path : ""
                    color: Qt.rgba(1, 1, 1, 0.50)
                    font { family: card.uiFont; pixelSize: 11 }
                    elide: Text.ElideMiddle
                }
            }

            // Several files: as many 30 px rows as fit, then "还有 N 项".
            Column {
                visible: fileList.count > 1
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: 14
                anchors.rightMargin: 14
                anchors.topMargin: card.expanded ? 12 : 42
                spacing: 4
                Repeater {
                    model: fileList.visibleRows
                    delegate: Row {
                        required property int index
                        height: 26
                        spacing: 8
                        DeckIcon {
                            width: 18; height: 18
                            anchors.verticalCenter: parent.verticalCenter
                            kind: card.files[index].isDirectory ? "folder" : "doc"
                            color: Qt.rgba(1, 1, 1, 0.78)
                        }
                        Text {
                            width: fileList.width - 28 - 26
                            anchors.verticalCenter: parent.verticalCenter
                            text: card.files[index].name
                            color: Qt.rgba(1, 1, 1, 0.90)
                            font { family: card.uiFont; pixelSize: 13; weight: Font.Medium }
                            elide: Text.ElideMiddle
                        }
                    }
                }
                Text {
                    visible: fileList.overflow && fileList.count > fileList.visibleRows
                    text: "还有 " + (fileList.count - fileList.visibleRows) + " 项"
                    color: Qt.rgba(1, 1, 1, 0.52)
                    font { family: card.uiFont; pointSize: 11.5 * 0.75; weight: Font.Medium }
                }
            }
        }
    }
}
