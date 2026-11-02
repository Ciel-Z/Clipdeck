import QtQuick
import ClipDeck.Native

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
        enabled: settings.selectedCategoryId >= 0 && !expandedEditor.visible
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
                        "缺失的预制分类会被重新创建，同名预制分类的颜色、规则和卡片显示方式会恢复默认；其他自建分类会保留。",
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
                categoryModel.append({ categoryId: category.id, name: category.name, dotColor: category.color })
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
                required property string dotColor
                required property int index
                readonly property bool selected: settings.selectedCategoryId === categoryId
                width: categoryList.width
                height: 40
                radius: 9
                color: selected ? Qt.rgba(1, 1, 1, 0.12) : "transparent"
                opacity: categoryList.dragging && gripMouse.pressed ? 0.6 : 1

                MouseArea { anchors.fill: parent; onClicked: settings.selectCategory(row.categoryId) }
                // The same dot the category's capsule shows in the Deck.
                Item {
                    id: dot
                    x: 11
                    width: 20; height: 20
                    anchors.verticalCenter: parent.verticalCenter
                    Rectangle {
                        anchors.centerIn: parent
                        width: 11; height: 11; radius: 5.5
                        color: row.dotColor
                    }
                }
                Text {
                    anchors.left: dot.right
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

    // Title and "保存更改", fixed above the scrolling editor: at the end of
    // the long page the button was out of sight. Enabled only while the
    // editor differs from the stored category.
    Item {
        id: editorHeader
        visible: editor.visible
        x: editor.x
        width: editor.width
        height: 26 + headerText.implicitHeight + 14
        readonly property real columnWidth: Math.min(editor.maximumWidth, width) - 76
        Column {
            id: headerText
            x: Math.max(38, (editorHeader.width - editorHeader.columnWidth) / 2)
            y: 26
            spacing: 4
            Text {
                text: "编辑分类"
                color: "white"
                font { family: workspace.uiFont; pixelSize: 25; weight: Font.Bold }
            }
            Text {
                text: "调整名称、颜色、卡片显示方式和自动分类规则"
                color: workspace.secondary
                font { family: workspace.uiFont; pixelSize: 13 }
            }
        }
        PillButton {
            x: headerText.x + editorHeader.columnWidth - width
            anchors.verticalCenter: headerText.verticalCenter
            width: 96
            accent: true
            text: "保存更改"
            enabled: settings.categoryHasUnsavedChanges
            opacity: enabled ? 1 : 0.4
            onClicked: settings.saveCategory()
        }
    }

    SettingsPage {
        id: editor
        visible: settings.selectedCategoryId >= 0
        x: listPane.width + 1
        y: editorHeader.height
        width: parent.width - x
        height: parent.height - y
        // Wider than the other panes: the script editor uses the room.
        maximumWidth: 1120
        showsHeader: false
        topPadding: 4

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
                title: "颜色"
                subtitle: "列表和 Deck 分类栏中显示的圆点颜色"
                divider: false
                Row {
                    spacing: 10
                    Repeater {
                        model: settings.categoryColors()
                        delegate: Item {
                            required property string modelData
                            readonly property bool chosen: settings.categoryColor.toUpperCase() === modelData.toUpperCase()
                            width: 24; height: 24
                            Rectangle {
                                anchors.fill: parent
                                radius: width / 2
                                color: "transparent"
                                border.width: 2
                                border.color: parent.chosen ? "white" : "transparent"
                            }
                            Rectangle {
                                anchors.centerIn: parent
                                width: 16; height: 16; radius: 8
                                color: modelData
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: settings.categoryColor = modelData
                            }
                        }
                    }
                    // Custom colour: a hue wheel until a colour outside the
                    // presets is chosen, then that colour.
                    Item {
                        id: customSwatch
                        readonly property bool chosen: settings.categoryColors()
                            .every(preset => preset.toUpperCase() !== settings.categoryColor.toUpperCase())
                        width: 24; height: 24
                        Rectangle {
                            anchors.fill: parent
                            radius: width / 2
                            color: "transparent"
                            border.width: 2
                            border.color: customSwatch.chosen ? "white" : "transparent"
                        }
                        Canvas {
                            id: wheel
                            anchors.centerIn: parent
                            width: 16; height: 16
                            visible: !customSwatch.chosen
                            onPaint: {
                                const context = getContext("2d")
                                const gradient = context.createConicalGradient(8, 8, 0)
                                const stops = ["#FF0000", "#FF00FF", "#0000FF", "#00FFFF", "#00FF00", "#FFFF00", "#FF0000"]
                                stops.forEach((stop, index) => gradient.addColorStop(index / 6, stop))
                                context.fillStyle = gradient
                                context.beginPath()
                                context.arc(8, 8, 8, 0, Math.PI * 2)
                                context.fill()
                            }
                        }
                        Rectangle {
                            anchors.centerIn: parent
                            width: 16; height: 16; radius: 8
                            visible: customSwatch.chosen
                            color: settings.categoryColor
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                colorPicker.color = settings.categoryColor
                                colorPicker.open(customSwatch)
                            }
                        }
                    }
                }
            }
        }

        SettingsSection {
            title: "卡片显示"
            SettingsRow {
                id: cardTypeRow
                readonly property var choices: settings.cardTypes()
                title: "卡片类型"
                subtitle: {
                    for (const choice of choices) if (choice.value === settings.categoryCardType) return choice.detail
                    return ""
                }
                divider: false
                SegmentedControl {
                    options: cardTypeRow.choices
                    value: settings.categoryCardType
                    onActivated: (value) => settings.categoryCardType = value
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
                Row {
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    TextButton {
                        text: "格式化"
                        onClicked: settings.formatScript()
                    }
                    TextButton {
                        text: "⤢  展开编辑"
                        onClicked: expandedEditor.open()
                    }
                }
                Rectangle { anchors.bottom: parent.bottom; x: 16; width: parent.width - 16; height: 1; color: Qt.rgba(1, 1, 1, 0.08) }
            }
            ScriptEditor {
                id: scriptEditor
                width: parent.width
                // Grows with a taller (maximised) window; 280 at the default size.
                height: Math.round(Math.max(280, Math.min(760, editor.height - 420)))
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

    }

    // Nested in the pane, so its transient parent is the settings window.
    ExpandedScriptEditor { id: expandedEditor }

    ColorPicker {
        id: colorPicker
        onPicked: (hex) => settings.categoryColor = hex
    }
}
