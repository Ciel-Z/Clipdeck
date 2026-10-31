import QtQuick
import QtQuick.Window

// ExpandedJavaScriptEditor (sheet): its own window over the settings window,
// drawn by us (no system dialog chrome). The script editor is on the left;
// rule test, console output and the item fields on the right. 取消 restores
// the script the sheet opened with; 保存并完成 (Ctrl+S) saves and closes.
Window {
    id: sheet
    width: 1040
    height: 680
    minimumWidth: 900
    minimumHeight: 620
    visible: false
    flags: Qt.Dialog | Qt.FramelessWindowHint
    modality: Qt.WindowModal
    color: "#232326"
    title: settings.categoryName + " · JavaScript 分类规则"

    readonly property string uiFont: "Segoe UI"
    readonly property string monoFont: "Consolas"
    readonly property color secondary: Qt.rgba(1, 1, 1, 0.55)
    readonly property color divider: Qt.rgba(1, 1, 1, 0.08)
    property string initialScript: ""

    function open() {
        initialScript = settings.categoryScript
        const owner = sheet.transientParent
        if (owner) {
            x = Math.round(owner.x + (owner.width - width) / 2)
            y = Math.round(owner.y + Math.max(0, (owner.height - height) / 2))
        }
        show()
        settings.decorateSheet(sheet)
        requestActivate()
        editor.focusEditor()
    }
    function cancel() {
        settings.categoryScript = initialScript
        close()
    }
    function saveAndClose() {
        settings.saveCategory()
        close()
    }

    Shortcut { sequence: "Ctrl+S"; onActivated: sheet.saveAndClose() }
    Shortcut { sequence: "Escape"; onActivated: sheet.cancel() }

    // Header bar: drag to move the window.
    Rectangle {
        id: header
        width: parent.width
        height: 62
        color: "#2B2B2F"
        DragHandler {
            target: null
            onActiveChanged: if (active) sheet.startSystemMove()
        }
        Column {
            x: 18
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Text {
                text: sheet.title
                color: "white"
                font { family: sheet.uiFont; pointSize: 17 * 0.75; weight: Font.DemiBold }
            }
            Text {
                text: "编辑分类规则并使用右侧样本验证结果"
                color: sheet.secondary
                font { family: sheet.uiFont; pointSize: 11.5 * 0.75 }
            }
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            TextButton { height: 28; text: "取消"; onClicked: sheet.cancel() }
            TextButton { height: 28; accent: true; text: "保存并完成"; onClicked: sheet.saveAndClose() }
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: sheet.divider }
    }

    // Left: 分类脚本
    Item {
        id: scriptPane
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: sidePane.left

        Item {
            id: scriptHeader
            width: parent.width
            height: 42
            Row {
                x: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6
                Text {
                    text: "{ }"
                    color: sheet.secondary
                    font { family: sheet.monoFont; pointSize: 12 * 0.75; weight: Font.Bold }
                }
                Text {
                    text: "分类脚本"
                    color: sheet.secondary
                    font { family: sheet.uiFont; pointSize: 12.5 * 0.75; weight: Font.DemiBold }
                }
            }
            TextButton {
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: "格式化"
                onClicked: settings.formatScript()
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: sheet.divider }
        }
        ScriptEditor {
            id: editor
            anchors.top: scriptHeader.bottom
            anchors.bottom: scriptFooter.top
            width: parent.width
            color: Qt.rgba(0, 0, 0, 0.22)
        }
        Item {
            id: scriptFooter
            anchors.bottom: parent.bottom
            width: parent.width
            height: 36
            readonly property string script: settings.categoryScript
            Rectangle { width: parent.width; height: 1; color: sheet.divider }
            Text {
                x: 14
                anchors.verticalCenter: parent.verticalCenter
                text: "Ctrl+/ 注释  ·  Tab / Shift+Tab 缩进  ·  Ctrl+Shift+F 格式化"
                color: sheet.secondary
                font { family: sheet.uiFont; pointSize: 11.5 * 0.75; weight: Font.Medium }
            }
            Text {
                anchors.right: parent.right
                anchors.rightMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                text: Math.max(1, parent.script.split("\n").length) + " 行  ·  " + Array.from(parent.script).length + " 个字符"
                color: sheet.secondary
                font { family: sheet.uiFont; pointSize: 11.5 * 0.75; weight: Font.Medium }
            }
        }
    }

    // Right: 规则测试 / 控制台输出 / item 可用字段
    Item {
        id: sidePane
        anchors.top: header.bottom
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        width: Math.min(360, Math.max(270, sheet.width * 0.3))
        clip: true
        Rectangle { width: 1; height: parent.height; color: sheet.divider }

        Column {
            x: 1
            width: parent.width - 1
            Item {
                width: parent.width
                height: 42
                Text {
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: "规则测试"
                    color: "white"
                    font { family: sheet.uiFont; pointSize: 13 * 0.75; weight: Font.DemiBold }
                }
                TextButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: "运行测试"
                    onClicked: settings.testScript()
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: sheet.divider }
            }
            Item {
                width: parent.width
                height: 176
                TextEdit {
                    id: testEditor
                    x: 12; y: 10
                    width: parent.width - 24
                    height: parent.height - 20
                    text: settings.testText
                    onTextChanged: if (text !== settings.testText) settings.testText = text
                    wrapMode: TextEdit.Wrap
                    clip: true
                    selectByMouse: true
                    color: "white"
                    selectionColor: "#0A84FF"
                    font { family: sheet.monoFont; pointSize: 12.5 * 0.75 }
                    Text {
                        visible: testEditor.text.length === 0 && !testEditor.activeFocus
                        text: "输入一段示例文本"
                        color: Qt.rgba(1, 1, 1, 0.3)
                        font: testEditor.font
                    }
                    Keys.onPressed: (event) => {
                        if ((event.modifiers & Qt.ControlModifier) && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                            settings.testScript(); event.accepted = true
                        }
                    }
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: sheet.divider }
            }
            Item {
                width: parent.width
                height: 36
                Text {
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: "控制台输出"
                    color: "white"
                    font { family: sheet.uiFont; pointSize: 12.5 * 0.75; weight: Font.DemiBold }
                }
            }
            Rectangle {
                width: parent.width
                height: 124
                color: Qt.rgba(0, 0, 0, 0.2)
                clip: true
                Flickable {
                    anchors.fill: parent
                    anchors.margins: 12
                    contentHeight: output.implicitHeight
                    boundsBehavior: Flickable.StopAtBounds
                    Column {
                        id: output
                        width: parent.width
                        spacing: 7
                        Repeater {
                            model: settings.testConsole
                            delegate: Row {
                                required property var modelData
                                spacing: 8
                                Text {
                                    width: 38
                                    text: modelData.level.toUpperCase()
                                    color: modelData.level === "warn" ? "#FF9F0A" : modelData.level === "error" ? "#FF453A"
                                         : modelData.level === "info" ? "#0A84FF" : modelData.level === "debug" ? "#BF5AF2"
                                         : Qt.rgba(1, 1, 1, 0.55)
                                    font { family: sheet.monoFont; pointSize: 9.5 * 0.75; weight: Font.Bold }
                                }
                                Text {
                                    width: output.width - 46
                                    wrapMode: Text.Wrap
                                    text: modelData.message === "" ? "（空）" : modelData.message
                                    color: "white"
                                    font { family: sheet.monoFont; pointSize: 11.5 * 0.75 }
                                }
                            }
                        }
                        Text {
                            visible: settings.testResult === "" && settings.testConsole.length === 0
                            text: "运行测试后在这里查看结果和 console 输出"
                            color: Qt.rgba(1, 1, 1, 0.32)
                            wrapMode: Text.Wrap
                            width: parent.width
                            font { family: sheet.uiFont; pointSize: 11.5 * 0.75 }
                        }
                        Text {
                            visible: settings.testResult !== ""
                            text: (settings.testResult.startsWith("错误") ? "✕  " : settings.testResult.indexOf("true") >= 0 ? "✓  " : "–  ")
                                  + settings.testResult
                            color: settings.testResult.startsWith("错误") ? "#FF453A" : sheet.secondary
                            font { family: sheet.uiFont; pixelSize: 12; weight: Font.DemiBold }
                        }
                    }
                }
            }
            Rectangle { width: parent.width; height: 1; color: sheet.divider }
            Item {
                width: parent.width
                height: 42
                Text {
                    x: 14
                    anchors.verticalCenter: parent.verticalCenter
                    text: "item 可用字段"
                    color: "white"
                    font { family: sheet.uiFont; pointSize: 13 * 0.75; weight: Font.DemiBold }
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: sheet.divider }
            }
            Column {
                x: 14
                topPadding: 14
                width: parent.width - 28
                spacing: 12
                Repeater {
                    model: {
                        let fields = [
                            { name: "text", detail: "复制的文本内容" },
                            { name: "textLength", detail: "文本字符数" },
                            { name: "sourceApplication.bundleId", detail: "来源应用标识" },
                            { name: "sourceApplication.name", detail: "来源应用名称" },
                            { name: "copiedAt", detail: "毫秒时间戳" }
                        ]
                        if (settings.categoryPreviewLanguage === "code") {
                            fields.push({ name: "detectedLanguage", detail: "识别的代码语言" })
                            fields.push({ name: "codeScore", detail: "代码识别分数" })
                            fields.push({ name: "codeEngineMatched", detail: "是否识别为代码" })
                        }
                        return fields
                    }
                    // JavaScriptFieldRow
                    delegate: Column {
                        required property var modelData
                        spacing: 2
                        Text {
                            text: modelData.name
                            color: "#E5C07B"
                            font { family: sheet.monoFont; pointSize: 12 * 0.75; weight: Font.DemiBold }
                        }
                        Text {
                            text: modelData.detail
                            color: sheet.secondary
                            font { family: sheet.uiFont; pointSize: 11.5 * 0.75 }
                        }
                    }
                }
            }
        }
    }

    // A frameless window still resizes from its edges and corners.
    Repeater {
        model: [
            { edges: Qt.LeftEdge, cursor: Qt.SizeHorCursor },
            { edges: Qt.RightEdge, cursor: Qt.SizeHorCursor },
            { edges: Qt.TopEdge, cursor: Qt.SizeVerCursor },
            { edges: Qt.BottomEdge, cursor: Qt.SizeVerCursor },
            { edges: Qt.LeftEdge | Qt.TopEdge, cursor: Qt.SizeFDiagCursor },
            { edges: Qt.RightEdge | Qt.BottomEdge, cursor: Qt.SizeFDiagCursor },
            { edges: Qt.RightEdge | Qt.TopEdge, cursor: Qt.SizeBDiagCursor },
            { edges: Qt.LeftEdge | Qt.BottomEdge, cursor: Qt.SizeBDiagCursor }
        ]
        delegate: MouseArea {
            required property var modelData
            readonly property int grip: 6
            readonly property bool atLeft: modelData.edges & Qt.LeftEdge
            readonly property bool atRight: modelData.edges & Qt.RightEdge
            readonly property bool atTop: modelData.edges & Qt.TopEdge
            readonly property bool atBottom: modelData.edges & Qt.BottomEdge
            x: atRight ? sheet.width - grip : 0
            y: atBottom ? sheet.height - grip : 0
            width: atLeft || atRight ? grip : sheet.width
            height: atTop || atBottom ? grip : sheet.height
            z: 10
            cursorShape: modelData.cursor
            onPressed: sheet.startSystemResize(modelData.edges)
        }
    }
}
