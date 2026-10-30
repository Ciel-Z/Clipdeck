import QtQuick
import QtQuick.Effects
import QtQuick.Shapes
import QtQuick.Window

// SwiftUI .popover(arrowEdge: .bottom) for a card: an owned frameless window
// above the card with an arrow pointing down at it. mode "preview" never takes
// focus (the Deck keeps the keyboard, like the mac key panel); mode "editor"
// hosts ContentEditorView / RenameItemView and does.
Window {
    id: popup

    property string mode: "preview"
    property rect anchorRect: Qt.rect(0, 0, 0, 0)
    readonly property var info: deck.popover
    readonly property string kind: info.kind !== undefined ? info.kind : ""
    readonly property bool active: mode === "preview" ? kind === "preview" : (kind === "edit" || kind === "rename")
    readonly property string uiFont: "Segoe UI"
    readonly property string monoFont: "Consolas"

    readonly property real margin: 22        // room for the shadow
    readonly property real arrowHeight: 11
    readonly property real arrowWidth: 24
    readonly property real bubbleRadius: 12
    readonly property real contentWidth: kind === "rename" ? 380 : kind === "edit" ? 560 : (info.previewWidth || 560)
    readonly property real contentHeight: kind === "rename" ? renameContent.implicitHeight
        : kind === "edit" ? 430 : (info.previewHeight || 360)

    flags: Qt.FramelessWindowHint | Qt.Tool | Qt.WindowStaysOnTopHint
           | (mode === "preview" ? Qt.WindowDoesNotAcceptFocus : 0)
    color: "transparent"
    width: contentWidth + margin * 2
    height: contentHeight + arrowHeight + margin * 2
    visible: active && deck.deckVisible

    // Centred over the card, kept on screen; the arrow tracks the card centre.
    readonly property real anchorCenter: anchorRect.x + anchorRect.width / 2
    x: Math.max(screen.virtualX + 4, Math.min(anchorCenter - width / 2, screen.virtualX + screen.width - width - 4))
    y: anchorRect.y - height + margin - 2
    readonly property real arrowX: Math.max(bubbleRadius + arrowWidth / 2,
                                            Math.min(anchorCenter - x - margin, contentWidth - bubbleRadius - arrowWidth / 2))

    onVisibleChanged: {
        if (visible && mode === "editor") {
            requestActivate()
            focusTimer.restart()
        } else if (!visible && mode === "editor") {
            // After the native hide: activating earlier leaves the hidden
            // popover as the foreground window and the Deck without keys.
            restoreTimer.restart()
        }
    }
    Timer {
        id: restoreTimer
        interval: 20
        onTriggered: deck.activateDeck()
    }
    onKindChanged: if (active && mode === "editor") focusTimer.restart()
    Timer {
        id: focusTimer
        interval: 30
        onTriggered: {
            // Activating a popover that has closed meanwhile would make the
            // hidden window foreground and swallow the Deck's keys.
            if (!popup.active || !popup.visible) return
            // The first show creates the native window; activate once it exists.
            popup.requestActivate()
            if (popup.kind === "rename") { titleInput.forceActiveFocus(); titleInput.selectAll() }
            else if (popup.kind === "edit" && popup.info.payload === "text") textEditor.forceActiveFocus()
            else editorRoot.forceActiveFocus()
        }
    }

    // Shadow of the bubble, drawn as its own layer so the text in the bubble
    // is not resampled through an effect.
    Shape {
        x: popup.margin
        y: popup.margin
        width: popup.contentWidth
        height: popup.contentHeight + popup.arrowHeight
        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: Qt.rgba(0, 0, 0, 0.45)
            blurMax: 24
            shadowBlur: 1.0
            shadowVerticalOffset: 6
        }
        ShapePath {
            strokeWidth: -1
            fillColor: "black"
            PathSvg { path: popup.bubblePath() }
        }
    }

    Item {
        id: bubble
        x: popup.margin
        y: popup.margin
        width: popup.contentWidth
        height: popup.contentHeight + popup.arrowHeight

        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: Qt.rgba(1, 1, 1, 0.12)
                strokeWidth: 1
                fillGradient: LinearGradient {
                    x1: 0; y1: 0; x2: 0; y2: popup.contentHeight
                    GradientStop { position: 0; color: popup.kind === "edit" ? "#2E2E36" : "#2C2C30" }
                    GradientStop { position: 1; color: popup.kind === "edit" ? "#212126" : "#26262A" }
                }
                PathSvg { path: popup.bubblePath() }
            }
        }

        Item {
            id: contentArea
            width: popup.contentWidth
            height: popup.contentHeight
            clip: true

            Loader {
                anchors.fill: parent
                active: popup.kind === "preview"
                sourceComponent: previewContent
            }
            Item {
                id: editorRoot
                anchors.fill: parent
                visible: popup.kind === "edit"
                focus: popup.kind === "edit" && popup.info.payload !== "text"
                Keys.onPressed: (event) => popup.handleEditorKey(event)
            }
        }
    }

    function bubblePath() {
        const w = contentWidth, h = contentHeight, r = bubbleRadius
        const ax = arrowX, aw = arrowWidth / 2, ah = arrowHeight
        return "M " + r + " 0 L " + (w - r) + " 0 Q " + w + " 0 " + w + " " + r
            + " L " + w + " " + (h - r) + " Q " + w + " " + h + " " + (w - r) + " " + h
            + " L " + (ax + aw) + " " + h + " L " + ax + " " + (h + ah) + " L " + (ax - aw) + " " + h
            + " L " + r + " " + h + " Q 0 " + h + " 0 " + (h - r)
            + " L 0 " + r + " Q 0 0 " + r + " 0 Z"
    }

    // MARK: Preview (ContentPreviewView)

    Component {
        id: previewContent
        Item {
            id: previewRoot
            readonly property bool isFiles: popup.info.payload === "files"
            Rectangle {
                anchors.fill: parent
                visible: previewRoot.isFiles
                radius: popup.bubbleRadius
                color: "#131315"
            }
            Item {
                id: previewHeader
                width: parent.width
                height: previewRoot.isFiles ? 52 : 58
                Text {
                    id: categoryText
                    anchors.left: parent.left
                    anchors.leftMargin: 22
                    anchors.verticalCenter: parent.verticalCenter
                    text: popup.info.categoryName || ""
                    color: "white"
                    font { family: popup.uiFont; pixelSize: previewRoot.isFiles ? 15 : 18; weight: Font.DemiBold }
                }
                Text {
                    anchors.left: categoryText.right
                    anchors.leftMargin: 12
                    anchors.baseline: categoryText.baseline
                    text: popup.info.sourceName || ""
                    color: Qt.rgba(1, 1, 1, 0.55)
                    font { family: popup.uiFont; pixelSize: 13; weight: Font.Medium }
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 22
                    anchors.verticalCenter: parent.verticalCenter
                    text: popup.info.dateText || ""
                    color: Qt.rgba(1, 1, 1, 0.55)
                    font { family: popup.uiFont; pixelSize: 11 }
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Qt.rgba(1, 1, 1, 0.10) }
            }

            Flickable {
                id: previewScroll
                visible: popup.info.payload === "text"
                anchors.top: previewHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                contentHeight: previewText.paintedHeight + 36
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                TextEdit {
                    id: previewText
                    x: 20; y: 18
                    width: previewScroll.width - 40
                    readOnly: true
                    selectByMouse: true
                    text: popup.info.text || ""
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.Wrap
                    color: Qt.rgba(1, 1, 1, 0.92)
                    selectionColor: "#0A84FF"
                    font { family: popup.info.monospaced ? popup.monoFont : popup.uiFont; pixelSize: 14 }
                }
                Connections {
                    target: deck
                    function onPreviewScrollRequested(direction) {
                        previewScroll.contentY = Math.max(0, Math.min(previewScroll.contentHeight - previewScroll.height,
                                                                      previewScroll.contentY + direction * 60))
                    }
                }
                WheelHandler {
                    onWheel: (event) => {
                        previewScroll.contentY = Math.max(0, Math.min(previewScroll.contentHeight - previewScroll.height,
                                                                      previewScroll.contentY - event.angleDelta.y / 120 * 44))
                    }
                }
            }
            Text {
                visible: popup.info.payload === "text" && popup.info.loading === true
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 12
                text: "正在载入完整内容…"
                color: Qt.rgba(1, 1, 1, 0.45)
                font { family: popup.uiFont; pixelSize: 11 }
            }

            Column {
                visible: popup.info.payload === "image"
                anchors.top: previewHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 24
                spacing: 12
                RotatedImage {
                    width: parent.width
                    height: parent.height - 12 - sizeCaption.height
                    source: popup.info.imageUrl || ""
                    angle: popup.info.imageAngle || 0
                }
                Text {
                    id: sizeCaption
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: popup.dimensions(popup.info.imageAngle || 0)
                    color: Qt.rgba(1, 1, 1, 0.55)
                    font { family: popup.monoFont; pixelSize: 11 }
                }
            }

            FileRows {
                visible: previewRoot.isFiles
                anchors.top: previewHeader.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                files: popup.info.files || []
            }
        }
    }

    // MARK: Editor (ContentEditorView)

    property string editText: ""
    property int editAngle: 0
    onInfoChanged: {
        // Read info.kind here: the `kind` binding may not have re-evaluated yet.
        const current = info.kind !== undefined ? info.kind : ""
        const id = info.itemId !== undefined ? info.itemId : ""
        if (current === "edit" && id !== editingId) {
            editingId = id
            editText = info.text || ""
            editAngle = info.imageAngle || 0
        } else if (current === "edit" && editText === "" && info.text) {
            editText = info.text // full text arrived after the summary
        }
        if (current === "rename" && id !== renamingId) {
            renamingId = id
            titleInput.text = info.title || ""
        }
        if (current !== "edit") editingId = ""
        if (current !== "rename") renamingId = ""
    }
    property string editingId: ""
    property string renamingId: ""

    function dimensions(angle) {
        const w = info.imageWidth || 0, h = info.imageHeight || 0
        return (angle % 180 === 0) ? (w + " × " + h) : (h + " × " + w)
    }

    function saveEditor() {
        if (info.payload === "text") deck.saveText(editText)
        else if (info.payload === "image") deck.saveImageAngle(editAngle)
    }

    function handleEditorKey(event) {
        const ctrl = (event.modifiers & Qt.ControlModifier) && !(event.modifiers & (Qt.AltModifier | Qt.ShiftModifier))
        if (event.key === Qt.Key_Escape) { deck.escape(); event.accepted = true }
        else if (ctrl && event.key === Qt.Key_S) { saveEditor(); event.accepted = true }
        else if (ctrl && event.key === Qt.Key_BracketLeft && info.payload === "image") { editAngle = (editAngle + 270) % 360; event.accepted = true }
        else if (ctrl && event.key === Qt.Key_BracketRight && info.payload === "image") { editAngle = (editAngle + 90) % 360; event.accepted = true }
        else if (ctrl && event.key === Qt.Key_Q) { deck.quit(); event.accepted = true }
    }

    Item {
        parent: editorRoot
        anchors.fill: parent

        // Toolbar: 取消 · title / rotate buttons · 保存
        Rectangle {
            id: editorToolbar
            width: parent.width
            height: 62
            color: Qt.rgba(1, 1, 1, 0.035)
            radius: popup.bubbleRadius
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Qt.rgba(1, 1, 1, 0.08) }

            Row {
                visible: popup.info.payload !== "image"
                anchors.centerIn: parent
                spacing: 10
                Text {
                    text: "编辑文本"
                    color: Qt.rgba(1, 1, 1, 0.76)
                    font { family: popup.uiFont; pixelSize: 15; weight: Font.DemiBold }
                    anchors.verticalCenter: parent.verticalCenter
                }
                PillButton {
                    visible: popup.info.editableLanguage === "json"
                    text: "格式化"
                    small: true
                    anchors.verticalCenter: parent.verticalCenter
                    onClicked: {
                        const formatted = deck.formatJson(popup.editText)
                        if (formatted !== "") popup.editText = formatted
                    }
                }
            }
            Row {
                visible: popup.info.payload === "image"
                anchors.centerIn: parent
                spacing: 22
                IconButton { icon: "rotate.left"; tooltip: "向左旋转"; onClicked: popup.editAngle = (popup.editAngle + 270) % 360 }
                IconButton { icon: "rotate.right"; tooltip: "向右旋转"; onClicked: popup.editAngle = (popup.editAngle + 90) % 360 }
            }
            PillButton {
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                text: "取消"
                onClicked: deck.escape()
            }
            PillButton {
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                text: "保存"
                accent: true
                onClicked: popup.saveEditor()
            }
        }

        // Text editor
        Item {
            visible: popup.info.payload === "text"
            anchors.top: editorToolbar.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            Rectangle {
                id: editorFrame
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                anchors.topMargin: 12
                anchors.bottomMargin: 8 + 44
                radius: 12
                color: Qt.rgba(0, 0, 0, 0.56)
                border.color: Qt.rgba(1, 1, 1, 0.09)
                Flickable {
                    id: editorScroll
                    anchors.fill: parent
                    anchors.margins: 16
                    contentHeight: textEditor.paintedHeight
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    function ensureVisible(r) {
                        if (contentY >= r.y) contentY = r.y
                        else if (contentY + height <= r.y + r.height) contentY = r.y + r.height - height
                    }
                    TextEdit {
                        id: textEditor
                        width: editorScroll.width
                        text: popup.editText
                        onTextChanged: if (text !== popup.editText) popup.editText = text
                        wrapMode: TextEdit.Wrap
                        textFormat: TextEdit.PlainText
                        selectByMouse: true
                        persistentSelection: true
                        color: "white"
                        selectionColor: "#0A84FF"
                        font { family: popup.info.monospaced ? popup.monoFont : popup.uiFont; pixelSize: popup.info.monospaced ? 15 : 17 }
                        onCursorRectangleChanged: editorScroll.ensureVisible(cursorRectangle)
                        Keys.onPressed: (event) => popup.handleEditorKey(event)
                    }
                }
            }
            Rectangle {
                anchors.bottom: parent.bottom
                width: parent.width
                height: 44
                color: Qt.rgba(1, 1, 1, 0.025)
                radius: popup.bubbleRadius
                Text {
                    anchors.left: parent.left
                    anchors.leftMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    readonly property string value: popup.editText
                    text: Array.from(value).length + " 个字符  ·  "
                          + value.split(/\s+/).filter(word => word.length > 0).length + " 个单词  ·  "
                          + Math.max(1, value.split("\n").length) + " 行"
                    color: Qt.rgba(1, 1, 1, 0.50)
                    font { family: popup.uiFont; pixelSize: 12; weight: Font.Medium }
                }
            }
        }

        // Image editor
        Column {
            visible: popup.info.payload === "image"
            anchors.top: editorToolbar.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.topMargin: 12
            spacing: 10
            Rectangle {
                x: 12
                width: parent.width - 24
                height: parent.height - 12 - 10 - dimensionsLabel.height - 12
                radius: 12
                color: "#1A1A1C"
                clip: true
                border.color: Qt.rgba(1, 1, 1, 0.10)
                Checkerboard { anchors.fill: parent; anchors.margins: 1 }
                RotatedImage {
                    anchors.fill: parent
                    anchors.margins: 12
                    source: popup.info.imageUrl || ""
                    angle: popup.editAngle
                }
            }
            Text {
                id: dimensionsLabel
                x: 18
                text: popup.dimensions(popup.editAngle)
                color: Qt.rgba(1, 1, 1, 0.56)
                font { family: popup.monoFont; pixelSize: 13; weight: Font.DemiBold }
            }
        }
    }

    // MARK: Rename (RenameItemView)

    Column {
        id: renameContent
        parent: contentArea
        visible: popup.kind === "rename"
        x: 18
        y: 18
        width: popup.contentWidth - 36
        spacing: 14
        bottomPadding: 18
        Text {
            text: "重命名"
            color: "white"
            font { family: popup.uiFont; pixelSize: 15; weight: Font.DemiBold }
        }
        Rectangle {
            width: parent.width
            height: 38
            radius: 9
            color: Qt.rgba(1, 1, 1, 0.08)
            TextInput {
                id: titleInput
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                verticalAlignment: TextInput.AlignVCenter
                color: "white"
                selectionColor: "#0A84FF"
                clip: true
                font { family: popup.uiFont; pixelSize: 16 }
                Text {
                    visible: titleInput.text.length === 0 && !titleInput.inputMethodComposing
                    text: "标题"
                    color: Qt.rgba(1, 1, 1, 0.35)
                    font: titleInput.font
                    anchors.verticalCenter: parent.verticalCenter
                }
                onAccepted: deck.saveRename(text)
                Keys.onPressed: (event) => {
                    const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
                    if (event.key === Qt.Key_Escape) { deck.escape(); event.accepted = true }
                    else if (ctrl && event.key === Qt.Key_S) { deck.saveRename(text); event.accepted = true }
                }
            }
        }
        Item {
            width: parent.width
            height: 28
            Text {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                text: "空标题将恢复为默认名称"
                color: Qt.rgba(1, 1, 1, 0.50)
                font { family: popup.uiFont; pixelSize: 11 }
            }
            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8
                PillButton { text: "取消"; small: true; onClicked: deck.escape() }
                PillButton { text: "保存"; small: true; accent: true; onClicked: deck.saveRename(titleInput.text) }
            }
        }
    }
}
