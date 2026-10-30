import QtQuick
import QtQuick.Window

// Port of CategoryManagementView: sidebar (190) + 通用 / 图片 / 键盘快捷键 /
// 分类 / 数据. Windows title bar (dark), contents drawn like macOS.
Window {
    id: window
    title: "ClipDeck 设置"
    width: 980
    height: 700
    minimumWidth: 920
    minimumHeight: 640
    color: "#1E1E20"
    visible: false

    readonly property string uiFont: "Segoe UI"
    readonly property color secondary: Qt.rgba(1, 1, 1, 0.55)

    Connections {
        target: settings
        function onOpenRequested() {
            window.showNormal()
            window.raise()
            window.requestActivate()
        }
    }

    // MARK: Sidebar
    Rectangle {
        id: sidebar
        width: 190
        height: parent.height
        color: "#18181A"
        Text {
            x: 20; y: 22
            text: "设置"
            color: "white"
            font { family: window.uiFont; pixelSize: 22; weight: Font.Bold }
        }
        Column {
            x: 12; y: 22 + 30 + 14
            width: parent.width - 24
            spacing: 5
            Repeater {
                model: [
                    { pane: "general", title: "通用", icon: "gear" },
                    { pane: "images", title: "图片", icon: "image" },
                    { pane: "keyboard", title: "键盘快捷键", icon: "keyboard" },
                    { pane: "categories", title: "分类", icon: "grid" },
                    { pane: "data", title: "数据", icon: "drive" }
                ]
                delegate: Rectangle {
                    required property var modelData
                    readonly property bool selected: settings.pane === modelData.pane
                    width: parent.width
                    height: 42
                    radius: 10
                    color: selected ? Qt.rgba(1, 1, 1, 0.12) : "transparent"
                    DeckIcon {
                        x: 13
                        anchors.verticalCenter: parent.verticalCenter
                        width: 20; height: 20
                        kind: modelData.icon
                        color: Qt.rgba(1, 1, 1, parent.selected ? 0.95 : 0.55)
                    }
                    Text {
                        x: 13 + 22 + 11
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.title
                        color: Qt.rgba(1, 1, 1, parent.selected ? 0.95 : 0.55)
                        font { family: window.uiFont; pixelSize: 15; weight: Font.DemiBold }
                    }
                    MouseArea { anchors.fill: parent; onClicked: settings.pane = modelData.pane }
                }
            }
        }
    }
    Rectangle { x: sidebar.width; width: 1; height: parent.height; color: Qt.rgba(1, 1, 1, 0.08) }

    Loader {
        x: sidebar.width + 1
        width: parent.width - x
        height: parent.height
        sourceComponent: settings.pane === "images" ? imagesPane
            : settings.pane === "keyboard" ? keyboardPane
            : settings.pane === "categories" ? categoriesPane
            : settings.pane === "data" ? dataPane
            : generalPane
    }

    ConfirmDialog { id: confirm }

    // MARK: 通用
    Component {
        id: generalPane
        SettingsPage {
            title: "通用"
            subtitle: "设置 ClipDeck 的默认行为"

            SettingsSection {
                title: "粘贴"
                SettingsRow {
                    title: "默认粘贴格式"
                    DropDown {
                        options: [{ value: false, text: "纯文本" }, { value: true, text: "保留原格式" }]
                        value: settings.preserveFormatting
                        onActivated: (value) => settings.preserveFormatting = value
                    }
                }
                Item {
                    width: parent.width
                    height: Math.max(56, hint.implicitHeight + 24)
                    Text {
                        id: hint
                        x: 16
                        width: parent.width - 32
                        anchors.verticalCenter: parent.verticalCenter
                        wrapMode: Text.Wrap
                        text: settings.preserveFormatting
                              ? "存在富文本（HTML）时同时写入；没有富文本时自动使用纯文本。"
                              : "始终以纯文本粘贴，避免携带字体、颜色和段落样式。"
                        color: window.secondary
                        font { family: window.uiFont; pointSize: 12.5 * 0.75 }
                    }
                }
            }

            SettingsSection {
                title: "启动"
                SettingsRow {
                    title: "登录时自动启动"
                    divider: settings.launchStatus !== ""
                    ToggleSwitch {
                        checked: settings.launchAtLogin
                        onToggled: (checked) => settings.setLaunchAtLogin(checked)
                    }
                }
                Item {
                    visible: settings.launchStatus !== ""
                    width: parent.width
                    height: visible ? 56 : 0
                    Text {
                        x: 16
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width - 32 - (openStartup.visible ? openStartup.width + 12 : 0)
                        wrapMode: Text.Wrap
                        text: settings.launchStatus
                        color: settings.launchStatusIsError ? "#FF453A" : window.secondary
                        font { family: window.uiFont; pointSize: 12.5 * 0.75 }
                    }
                    PillButton {
                        id: openStartup
                        visible: settings.launchNeedsSystemSettings
                        anchors.right: parent.right
                        anchors.rightMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        small: true
                        text: "打开启动应用设置"
                        onClicked: settings.openStartupAppsSettings()
                    }
                }
            }

            SettingsSection {
                title: "保留历史"
                Item {
                    width: parent.width
                    height: retention.height + 30
                    RetentionSlider {
                        id: retention
                        x: 16; y: 15
                        width: parent.width - 32
                        days: settings.retentionDays
                        onChangeRequested: (days) => {
                            const shorter = settings.retentionDays === 0 ? days !== 0
                                                                         : days !== 0 && days < settings.retentionDays
                            if (!shorter) { settings.applyRetention(days); return }
                            confirm.ask("缩短历史保留期限？",
                                        "将永久删除最近复制时间早于" + retention.title(days) + "的历史项目，然后应用新期限。此操作无法撤销。",
                                        "删除并应用", true, () => settings.applyRetention(days))
                        }
                    }
                    Rectangle { anchors.bottom: parent.bottom; x: 16; width: parent.width - 16; height: 1; color: Qt.rgba(1, 1, 1, 0.08) }
                }
                Item {
                    width: parent.width
                    height: 56
                    Text {
                        x: 16
                        anchors.verticalCenter: parent.verticalCenter
                        text: "期限以项目最近一次复制时间为起点"
                        color: window.secondary
                        font { family: window.uiFont; pointSize: 12.5 * 0.75 }
                    }
                    TextButton {
                        anchors.right: parent.right
                        anchors.rightMargin: 16
                        anchors.verticalCenter: parent.verticalCenter
                        text: "删除历史…"
                        destructive: true
                        onClicked: window.confirmClearAll()
                    }
                }
            }
        }
    }

    function confirmClearAll() {
        confirm.ask("清空全部历史？", "所有文本和图片历史都将永久删除，此操作无法撤销。", "清空全部历史", true,
                    () => settings.clearAllHistory())
    }

    // MARK: 图片
    Component {
        id: imagesPane
        SettingsPage {
            title: "图片"
            subtitle: "控制图片剪贴板内容的记录方式"
            SettingsSection {
                title: "图片历史"
                SettingsRow {
                    title: "记录图片"
                    ToggleSwitch {
                        checked: settings.recordsImages
                        onToggled: (checked) => settings.recordsImages = checked
                    }
                }
                SettingsRow {
                    title: "最大图片大小"
                    divider: false
                    DropDown {
                        width: 130
                        enabled: settings.recordsImages
                        options: [1, 2, 5, 10, 20].map(size => ({ value: size, text: size + " MB" }))
                        value: settings.maximumImageMegabytes
                        onActivated: (value) => settings.maximumImageMegabytes = value
                    }
                }
            }
            Text {
                width: parent.width
                wrapMode: Text.Wrap
                text: "PNG 超过限制时会被忽略；其他格式（如 BMP/DIB）会先转为 PNG，再按转换后的大小判断是否保存。"
                color: window.secondary
                font { family: window.uiFont; pointSize: 12.5 * 0.75 }
            }
        }
    }

    // MARK: 键盘快捷键
    Component {
        id: keyboardPane
        SettingsPage {
            title: "键盘快捷键"
            subtitle: "点击快捷键区域，然后按下新的组合键"
            SettingsSection {
                title: "全局"
                SettingsRow {
                    title: "启动 ClipDeck"
                    divider: settings.shortcutError !== ""
                    ShortcutRecorder { name: "showDeck"; shortcut: settings.showDeckShortcut }
                }
                Item {
                    visible: settings.shortcutError !== ""
                    width: parent.width
                    height: visible ? 44 : 0
                    Text {
                        x: 16
                        anchors.verticalCenter: parent.verticalCenter
                        text: settings.shortcutError
                        color: "#FF453A"
                        font { family: window.uiFont; pointSize: 12.5 * 0.75 }
                    }
                }
            }
            SettingsSection {
                title: "Deck"
                SettingsRow {
                    title: "显示下一个类型"
                    ShortcutRecorder { name: "nextCategory"; shortcut: settings.nextCategoryShortcut }
                }
                SettingsRow {
                    title: "显示上一个类型"
                    ShortcutRecorder { name: "previousCategory"; shortcut: settings.previousCategoryShortcut }
                }
                SettingsRow {
                    title: "快速粘贴"
                    Row {
                        spacing: 9
                        DropDown {
                            width: 145
                            options: [{ value: "ctrl", text: "Ctrl" }, { value: "shift", text: "Shift" }, { value: "alt", text: "Alt" }]
                            value: settings.quickPasteModifier
                            onActivated: (value) => settings.quickPasteModifier = value
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "+ 1…9"
                            color: window.secondary
                            font { family: window.uiFont; pixelSize: 14; weight: Font.DemiBold }
                        }
                    }
                }
                SettingsRow {
                    title: "纯文本模式"
                    divider: false
                    DropDown {
                        width: 145
                        options: [{ value: "ctrl", text: "Ctrl" }, { value: "shift", text: "Shift" }, { value: "alt", text: "Alt" }]
                        value: settings.plainTextModifier
                        onActivated: (value) => settings.plainTextModifier = value
                    }
                }
            }
            Item {
                width: parent.width
                height: 30
                PillButton {
                    anchors.right: parent.right
                    small: true
                    text: "将快捷方式重置为默认…"
                    onClicked: confirm.ask("重置键盘快捷键？", "启动 ClipDeck、类型切换、快速粘贴和纯文本模式都将恢复为默认快捷键。",
                                           "恢复默认快捷键", true, () => settings.resetShortcuts())
                }
            }
        }
    }

    // MARK: 数据
    Component {
        id: dataPane
        SettingsPage {
            title: "数据"
            subtitle: "查看数据库占用并管理本地历史"
            SettingsSection {
                title: "数据库"
                SettingsRow {
                    title: "当前占用"
                    Text { text: settings.databaseSize; color: window.secondary; font { family: window.uiFont; pixelSize: 14 } }
                }
                SettingsRow {
                    title: "过期缓存"
                    PillButton {
                        small: true
                        enabled: !settings.maintaining
                        opacity: enabled ? 1 : 0.45
                        text: "清空缓存…"
                        onClicked: confirm.ask("清空缓存？",
                                               "只删除超过当前保留期限的内容，并执行 WAL checkpoint 和 VACUUM；有效期内历史不受影响。",
                                               "清理过期内容并回收空间", false, () => settings.clearCache())
                    }
                }
                SettingsRow {
                    title: "全部历史"
                    divider: false
                    TextButton {
                        enabled: !settings.maintaining
                        text: "清空全部历史…"
                        destructive: true
                        onClicked: window.confirmClearAll()
                    }
                }
            }
            Text {
                visible: settings.maintenanceStatus !== ""
                text: settings.maintenanceStatus
                color: window.secondary
                font { family: window.uiFont; pointSize: 12.5 * 0.75; weight: Font.Medium }
            }
        }
    }

    // MARK: 分类
    Component {
        id: categoriesPane
        CategorySettings { confirmDialog: confirm }
    }
}
