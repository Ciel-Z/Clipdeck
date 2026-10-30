import QtQuick
import QtQuick.Effects
import QtQuick.Window

// The card .contextMenu: 粘贴 · 访问 · — · 编辑 · 重命名 · 预览 · — · 删除,
// drawn like the dark macOS menu. It never takes focus, so the Deck keeps
// the keyboard; Main routes ↑ ↓ Enter Esc here while it is open.
Window {
    id: menu

    property int itemIndex: -1
    property bool isLink: false
    property bool isFiles: false
    property int highlighted: -1
    readonly property real margin: 16
    readonly property real menuWidth: 214

    readonly property var entries: [
        { action: "paste", text: "粘贴", icon: "clipboard", shortcut: "Enter", visible: true, enabled: true },
        { action: "open", text: "访问", icon: "safari", shortcut: "Ctrl+O", visible: isLink, enabled: true },
        { separator: true, visible: true },
        { action: "edit", text: "编辑", icon: "pencil", shortcut: "Ctrl+E", visible: true, enabled: !isFiles },
        { action: "rename", text: "重命名", icon: "cursor", shortcut: "Ctrl+R", visible: true, enabled: true },
        { action: "preview", text: "预览", icon: "eye", shortcut: "Space", visible: true, enabled: true },
        { separator: true, visible: true },
        { action: "delete", text: "删除", icon: "trash", shortcut: "Ctrl+Backspace", visible: true, enabled: true, destructive: true }
    ].filter(entry => entry.visible)

    flags: Qt.FramelessWindowHint | Qt.Tool | Qt.WindowStaysOnTopHint | Qt.WindowDoesNotAcceptFocus
    color: "transparent"
    width: menuWidth + margin * 2
    height: column.implicitHeight + 10 + margin * 2
    visible: false

    function openAt(index, globalX, globalY, link, files) {
        itemIndex = index
        isLink = link
        isFiles = files
        highlighted = -1
        // Top-left at the cursor; flipped when it would leave the screen.
        const s = menu.screen
        let left = globalX - margin + 2
        let top = globalY - margin + 2
        if (left + width - margin > s.virtualX + s.width) left = globalX - width + margin
        if (top + height - margin > s.virtualY + s.height) top = globalY - height + margin
        x = left
        y = top
        visible = true
    }

    function close() { visible = false }

    function moveHighlight(step) {
        let next = highlighted
        for (let attempt = 0; attempt < entries.length; ++attempt) {
            next = (next + step + entries.length) % entries.length
            if (!entries[next].separator && entries[next].enabled) break
        }
        highlighted = next
    }

    function trigger(position) {
        const entry = entries[position]
        if (!entry || entry.separator || !entry.enabled) return
        close()
        switch (entry.action) {
        case "paste": deck.paste(itemIndex, false); break
        case "open": deck.openItem(itemIndex); break
        case "edit": deck.editItem(itemIndex); break
        case "rename": deck.renameItem(itemIndex); break
        case "preview": deck.previewItem(itemIndex); break
        case "delete": deck.deleteItem(itemIndex); break
        }
    }

    // Returns true when the key belonged to the open menu.
    function handleKey(event) {
        if (!visible) return false
        if (event.key === Qt.Key_Escape) close()
        else if (event.key === Qt.Key_Down) moveHighlight(1)
        else if (event.key === Qt.Key_Up) moveHighlight(-1)
        else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            if (highlighted >= 0) trigger(highlighted)
            else close()
        } else {
            close()
            return false
        }
        return true
    }

    Connections {
        target: deck
        function onDeckVisibleChanged() { if (!deck.deckVisible) menu.close() }
    }

    // The shadow is its own layer behind the panel: text rendered inside an
    // effect layer is resampled and looks soft.
    Rectangle {
        x: panel.x
        y: panel.y
        width: panel.width
        height: panel.height
        radius: panel.radius
        color: "black"
        layer.enabled: true
        layer.effect: MultiEffect {
            shadowEnabled: true
            shadowColor: Qt.rgba(0, 0, 0, 0.45)
            blurMax: 20
            shadowBlur: 1.0
            shadowVerticalOffset: 5
        }
    }

    Rectangle {
        id: panel
        x: menu.margin
        y: menu.margin
        width: menu.menuWidth
        height: column.implicitHeight + 10
        radius: 10
        color: Qt.rgba(0.17, 0.17, 0.19, 0.97)
        border.color: Qt.rgba(1, 1, 1, 0.12)

        Column {
            id: column
            x: 5
            y: 5
            width: parent.width - 10
            Repeater {
                model: menu.entries
                delegate: Item {
                    id: row
                    required property var modelData
                    required property int index
                    readonly property bool isHighlighted: menu.highlighted === index && !modelData.separator && modelData.enabled
                    width: column.width
                    height: modelData.separator ? 11 : 26

                    Rectangle {
                        visible: row.modelData.separator === true
                        anchors.verticalCenter: parent.verticalCenter
                        x: 9
                        width: parent.width - 18
                        height: 1
                        color: Qt.rgba(1, 1, 1, 0.12)
                    }
                    Rectangle {
                        anchors.fill: parent
                        visible: row.isHighlighted
                        radius: 5
                        color: "#0A64D6"
                    }
                    DeckIcon {
                        visible: !row.modelData.separator
                        x: 9
                        anchors.verticalCenter: parent.verticalCenter
                        width: 15; height: 15
                        kind: row.modelData.icon || ""
                        color: row.isHighlighted ? "white"
                             : row.modelData.destructive ? "#FF453A"
                             : Qt.rgba(1, 1, 1, row.modelData.enabled ? 0.88 : 0.32)
                    }
                    Text {
                        visible: !row.modelData.separator
                        x: 32
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.text || ""
                        color: row.isHighlighted ? "white"
                             : row.modelData.destructive ? "#FF453A"
                             : Qt.rgba(1, 1, 1, row.modelData.enabled ? 0.92 : 0.32)
                        font { family: "Segoe UI"; pixelSize: 13 }
                    }
                    Text {
                        visible: !row.modelData.separator
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.modelData.shortcut || ""
                        color: row.isHighlighted ? Qt.rgba(1, 1, 1, 0.85) : Qt.rgba(1, 1, 1, row.modelData.enabled ? 0.42 : 0.2)
                        font { family: "Segoe UI"; pixelSize: 12 }
                    }
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: !row.modelData.separator
                        onEntered: if (row.modelData.enabled) menu.highlighted = row.index
                        onExited: if (menu.highlighted === row.index) menu.highlighted = -1
                        onClicked: menu.trigger(row.index)
                    }
                }
            }
        }
    }
}
