import QtQuick

// categoryWorkspace: the category list (210, drag to reorder) and the editor
// (basic info, JavaScript rule, rule test).
Item {
    id: workspace
    property var confirmDialog
    readonly property string uiFont: "Segoe UI"
    readonly property string monoFont: "Consolas"
    readonly property color secondary: Qt.rgba(1, 1, 1, 0.55)

    // "保存更改" carries ⌘S on mac for the whole pane, not just the script editor.
    Shortcut {
        sequence: "Ctrl+S"
        enabled: settings.selectedCategoryId >= 0
        onActivated: settings.saveCategory()
    }

    // MARK: List
    Item {
        id: listPane
        width: 210
        height: parent.height

        Item {
            id: listHeader
            width: parent.width
            height: 62
            Text {
                x: 16
                anchors.verticalCenter: parent.verticalCenter
                text: "分类"
                color: "white"
                font { family: workspace.uiFont; pixelSize: 18; weight: Font.Bold }
            }
            Row {
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                IconButton {
                    width: 28; height: 28
                    icon: "restore"
                    tooltip: "还原预制分类"
                    onClicked: workspace.confirmDialog.ask("还原预制分类？",
                        "缺失的预制分类会被重新创建，同名预制分类的图标、规则和预览语言会恢复默认；其他自建分类会保留。",
                        "还原预制分类", false, () => settings.restorePresets())
                }
                IconButton { width: 28; height: 28; icon: "plus"; tooltip: "新增分类"; onClicked: settings.addCategory() }
            }
        }

        // Local copy of settings.categories so a drag can move rows without
        // the model reset that would destroy the row being dragged.
        ListModel { id: categoryModel }
        function syncModel() {
            if (categoryList.dragging) return
            categoryModel.clear()
            for (const category of settings.categories)
                categoryModel.append({ categoryId: category.id, name: category.name, icon: category.icon || "#" })
        }
        Connections {
            target: settings
            function onCategoriesChanged() { listPane.syncModel() }
        }
        Component.onCompleted: syncModel()

        ListView {
            id: categoryList
            anchors.top: listHeader.bottom
            anchors.bottom: parent.bottom
            x: 10
            width: parent.width - 20
            spacing: 5
            clip: true
            model: categoryModel
            property bool dragging: false
            moveDisplaced: Transition { NumberAnimation { properties: "y"; duration: 120; easing.type: Easing.OutCubic } }
            delegate: Rectangle {
                id: row
                required property var categoryId
                required property string name
                required property string icon
                required property int index
                readonly property bool selected: settings.selectedCategoryId === categoryId
                width: categoryList.width
                height: 40
                radius: 9
                color: selected ? Qt.rgba(1, 1, 1, 0.12) : "transparent"
                opacity: categoryList.dragging && gripMouse.pressed ? 0.6 : 1

                MouseArea { anchors.fill: parent; onClicked: settings.selectCategory(row.categoryId) }
                Text {
                    id: iconText
                    x: 11
                    width: 20
                    anchors.verticalCenter: parent.verticalCenter
                    horizontalAlignment: Text.AlignHCenter
                    text: row.icon
                    color: Qt.rgba(1, 1, 1, row.selected ? 0.95 : 0.55)
                    font { family: workspace.uiFont; pixelSize: 13; weight: Font.DemiBold }
                }
                Text {
                    anchors.left: iconText.right
                    anchors.leftMargin: 9
                    anchors.right: grip.left
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.name
                    elide: Text.ElideRight
                    color: Qt.rgba(1, 1, 1, row.selected ? 0.95 : 0.55)
                    font { family: workspace.uiFont; pixelSize: 14; weight: Font.DemiBold }
                }
                // Drag handle: rows move live, the order is saved on release.
                DeckIcon {
                    id: grip
                    anchors.right: menuButton.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 16; height: 16
                    kind: "grip"
                    color: Qt.rgba(1, 1, 1, 0.3)
                    MouseArea {
                        id: gripMouse
                        anchors.fill: parent
                        anchors.margins: -6
                        cursorShape: Qt.SizeVerCursor
                        // Otherwise the ListView (a Flickable) steals the drag after a few pixels.
                        preventStealing: true
                        onPressed: categoryList.dragging = true
                        onPositionChanged: (mouse) => {
                            if (!pressed) return
                            const point = mapToItem(categoryList.contentItem, mouse.x, mouse.y)
                            const target = categoryList.indexAt(10, point.y)
                            if (target >= 0 && target !== row.index) categoryModel.move(row.index, target, 1)
                        }
                        onReleased: {
                            categoryList.dragging = false
                            const order = []
                            for (let index = 0; index < categoryModel.count; ++index) order.push(categoryModel.get(index).categoryId)
                            settings.reorderCategories(order)
                        }
                    }
                }
                Item {
                    id: menuButton
                    anchors.right: parent.right
                    anchors.rightMargin: 5
                    width: 24; height: 30
                    anchors.verticalCenter: parent.verticalCenter
                    DeckIcon { anchors.centerIn: parent; width: 16; height: 16; kind: "ellipsis"; color: Qt.rgba(1, 1, 1, 0.6) }
                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            const id = row.categoryId
                            workspace.confirmDialog.ask("删除分类？",
                                "确定删除“" + row.name + "”吗？已有历史内容会保留，但会失去这个分类。",
                                "删除", true, () => settings.deleteCategory(id))
                        }
                    }
                }
            }
        }
    }
    Rectangle { x: listPane.width; width: 1; height: parent.height; color: Qt.rgba(1, 1, 1, 0.08) }

    // MARK: Editor
    Text {
        visible: settings.selectedCategoryId < 0
        anchors.centerIn: editor
        text: "选择一个分类"
        color: workspace.secondary
        font { family: workspace.uiFont; pixelSize: 17; weight: Font.DemiBold }
    }

    SettingsPage {
        id: editor
        visible: settings.selectedCategoryId >= 0
        x: listPane.width + 1
        width: parent.width - x
        height: parent.height
        maximumWidth: 820
        title: "编辑分类"
        subtitle: "调整名称、内容预览方式和自动分类规则"

        SettingsSection {
            title: "基本信息"
            SettingsRow {
                title: "分类名称"
                Rectangle {
                    width: 270; height: 28; radius: 6
                    color: Qt.rgba(0, 0, 0, 0.25)
                    border.color: nameInput.activeFocus ? "#0A84FF" : Qt.rgba(1, 1, 1, 0.12)
                    TextInput {
                        id: nameInput
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        verticalAlignment: TextInput.AlignVCenter
                        clip: true
                        color: "white"
                        selectionColor: "#0A84FF"
                        font { family: workspace.uiFont; pixelSize: 13 }
                        text: settings.categoryName
                        onTextEdited: settings.categoryName = text
                    }
                }
            }
            SettingsRow {
                title: "预览语言"
                divider: false
                DropDown {
                    options: settings.previewLanguages().map(language => ({ value: language, text: language }))
                    value: settings.categoryPreviewLanguage
                    onActivated: (value) => settings.categoryPreviewLanguage = value
                }
            }
        }

        SettingsSection {
            title: "JavaScript 分类规则"
            Item {
                width: parent.width
                height: 48
                Text {
                    x: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: "函数需返回 true 或 false，首个匹配的分类生效"
                    color: workspace.secondary
                    font { family: workspace.uiFont; pointSize: 12.5 * 0.75 }
                }
                TextButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: "格式化"
                    onClicked: settings.formatScript()
                }
                Rectangle { anchors.bottom: parent.bottom; x: 16; width: parent.width - 16; height: 1; color: Qt.rgba(1, 1, 1, 0.08) }
            }
            Flickable {
                id: scriptScroll
                width: parent.width
                height: 280
                contentHeight: scriptEditor.paintedHeight + 24
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                function ensureVisible(r) {
                    if (contentY >= r.y) contentY = r.y
                    else if (contentY + height <= r.y + r.height + 24) contentY = r.y + r.height + 24 - height
                }
                TextEdit {
                    id: scriptEditor
                    x: 16; y: 12
                    width: scriptScroll.width - 32
                    // Fill the visible area so a click below the last line still focuses it.
                    height: Math.max(paintedHeight, scriptScroll.height - 24)
                    text: settings.categoryScript
                    onTextChanged: if (text !== settings.categoryScript) settings.categoryScript = text
                    textFormat: TextEdit.PlainText
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    color: "white"
                    selectionColor: "#0A84FF"
                    font { family: workspace.monoFont; pixelSize: 13 }
                    onCursorRectangleChanged: scriptScroll.ensureVisible(cursorRectangle)
                    Keys.onPressed: (event) => workspace.handleScriptKey(event)
                }
            }
            Rectangle { x: 16; width: parent.width - 16; height: 1; color: Qt.rgba(1, 1, 1, 0.08) }
            Item {
                width: parent.width
                height: 36
                readonly property string script: settings.categoryScript
                Text {
                    x: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Ctrl+/ 注释  ·  Tab 缩进"
                    color: Qt.rgba(1, 1, 1, 0.32)
                    font { family: workspace.uiFont; pointSize: 11.5 * 0.75; weight: Font.Medium }
                }
                Text {
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: Math.max(1, parent.script.split("\n").length) + " 行  ·  " + Array.from(parent.script).length + " 个字符"
                    color: Qt.rgba(1, 1, 1, 0.32)
                    font { family: workspace.uiFont; pointSize: 11.5 * 0.75; weight: Font.Medium }
                }
            }
        }

        SettingsSection {
            title: "规则测试"
            Item {
                width: parent.width
                height: 106
                TextEdit {
                    id: testEditor
                    x: 14; y: 10
                    width: parent.width - 28
                    height: 86
                    text: settings.testText
                    onTextChanged: if (text !== settings.testText) settings.testText = text
                    wrapMode: TextEdit.Wrap
                    clip: true
                    selectByMouse: true
                    color: "white"
                    selectionColor: "#0A84FF"
                    font { family: workspace.monoFont; pixelSize: 13 }
                    Text {
                        visible: testEditor.text.length === 0 && !testEditor.activeFocus
                        text: "输入一段示例文本，测试脚本是否匹配"
                        color: Qt.rgba(1, 1, 1, 0.3)
                        font: testEditor.font
                    }
                    Keys.onPressed: (event) => {
                        if ((event.modifiers & Qt.ControlModifier) && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) {
                            settings.testScript(); event.accepted = true
                        }
                    }
                }
                Rectangle { anchors.bottom: parent.bottom; x: 16; width: parent.width - 16; height: 1; color: Qt.rgba(1, 1, 1, 0.08) }
            }
            Item {
                width: parent.width
                height: 50
                TextButton {
                    x: 16
                    anchors.verticalCenter: parent.verticalCenter
                    text: "测试脚本"
                    onClicked: settings.testScript()
                }
            }
            // ScriptTestOutputView
            Rectangle {
                visible: settings.testResult !== "" || settings.testConsole.length > 0
                width: parent.width
                height: visible ? Math.min(150, output.implicitHeight + 24) : 0
                color: Qt.rgba(0, 0, 0, 0.18)
                clip: true
                Column {
                    id: output
                    x: 12; y: 12
                    width: parent.width - 24
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
                                font { family: workspace.monoFont; pointSize: 9.5 * 0.75; weight: Font.Bold }
                            }
                            Text {
                                width: output.width - 46
                                wrapMode: Text.Wrap
                                text: modelData.message === "" ? "（空）" : modelData.message
                                color: "white"
                                font { family: workspace.monoFont; pointSize: 11.5 * 0.75 }
                            }
                        }
                    }
                    Rectangle { visible: settings.testResult !== ""; width: parent.width; height: 1; color: Qt.rgba(1, 1, 1, 0.1) }
                    Text {
                        visible: settings.testResult !== ""
                        text: (settings.testResult.startsWith("错误") ? "✕  " : settings.testResult.indexOf("true") >= 0 ? "✓  " : "–  ")
                              + settings.testResult
                        color: settings.testResult.startsWith("错误") ? "#FF453A" : workspace.secondary
                        font { family: workspace.uiFont; pixelSize: 12; weight: Font.DemiBold }
                    }
                }
            }
        }

        Text {
            visible: settings.categoryError !== ""
            width: parent.width
            wrapMode: Text.Wrap
            text: "最近错误：" + settings.categoryError
            color: "#FF453A"
            font { family: workspace.uiFont; pixelSize: 11 }
        }

        Item {
            width: parent.width
            height: 34
            PillButton {
                anchors.right: parent.right
                width: 96
                accent: true
                text: "保存更改"
                onClicked: settings.saveCategory()
            }
        }
    }

    // Ctrl+S saves, Ctrl+Enter tests, Tab indents, Ctrl+/ toggles "// ".
    function handleScriptKey(event) {
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        if (ctrl && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) { settings.testScript(); event.accepted = true }
        else if (ctrl && event.key === Qt.Key_Slash) { toggleComment(); event.accepted = true }
        else if (event.key === Qt.Key_Tab && event.modifiers === Qt.NoModifier) { scriptEditor.insert(scriptEditor.cursorPosition, "    "); event.accepted = true }
    }

    function toggleComment() {
        const text = scriptEditor.text
        const start = text.lastIndexOf("\n", scriptEditor.selectionStart - 1) + 1
        let end = text.indexOf("\n", Math.max(scriptEditor.selectionEnd - (scriptEditor.selectionEnd > scriptEditor.selectionStart ? 1 : 0), start))
        if (end < 0) end = text.length
        const lines = text.substring(start, end).split("\n")
        const commented = lines.every(line => line.trim() === "" || line.trim().startsWith("//"))
        const changed = lines.map(line => {
            if (line.trim() === "") return line
            if (commented) return line.replace(/^(\s*)\/\/ ?/, "$1")
            const indent = line.match(/^\s*/)[0]
            return indent + "// " + line.substring(indent.length)
        }).join("\n")
        const cursor = scriptEditor.cursorPosition
        settings.categoryScript = text.substring(0, start) + changed + text.substring(end)
        scriptEditor.cursorPosition = Math.min(cursor, scriptEditor.length)
    }
}
