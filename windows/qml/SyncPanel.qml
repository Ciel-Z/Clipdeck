import QtQuick
import QtQuick.Window

// 同步面板 (docs/lan-sync-design.md §8): this device, paired devices and nearby
// devices, with pairing inline. Opens next to the tray; hides when it loses
// focus unless a pairing code is on screen.
Window {
    id: panel
    width: 380
    height: Math.min(Screen.desktopAvailableHeight - 24, content.implicitHeight + 32)
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    color: "#252528"
    visible: false
    title: "局域网同步"

    readonly property string uiFont: "Segoe UI"
    readonly property color secondary: Qt.rgba(1, 1, 1, 0.55)
    readonly property var pairing: sync.pairing
    readonly property bool incoming: pairing.active === true && pairing.role === "incoming"

    property double openedAt: 0

    function openNearTray() {
        openedAt = Date.now()
        const area = Qt.rect(Screen.virtualX, Screen.virtualY, Screen.desktopAvailableWidth, Screen.desktopAvailableHeight)
        x = area.x + area.width - width - 12
        y = area.y + area.height - height - 12
        show()
        raise()
        requestActivate()
        settings.decorateSheet(panel)
        if (sync.nearby.length === 0) sync.searchNearby()
    }

    onHeightChanged: if (visible) y = Screen.virtualY + Screen.desktopAvailableHeight - height - 12
    onActiveChanged: {
        // A code shown on this device must stay up while the other side types it.
        // Focus can bounce while the window first appears; ignore that moment.
        if (!active && visible && Date.now() - openedAt > 600 && !incoming
            && !(pairing.active === true && pairing.stage === "code")) hide()
    }

    Connections {
        target: deck
        function onSyncPanelRequested() { panel.openNearTray() }
    }

    Column {
        id: content
        x: 16; y: 16
        width: parent.width - 32
        spacing: 14

        // MARK: Header
        Item {
            width: parent.width
            height: 26
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "局域网同步"
                color: "white"
                font { family: panel.uiFont; pixelSize: 16; weight: Font.Bold }
            }
            Text {
                anchors.right: closeButton.left
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: sync.statusText
                color: panel.secondary
                font { family: panel.uiFont; pixelSize: 12 }
            }
            Item {
                id: closeButton
                width: 22; height: 22
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                DeckIcon { anchors.centerIn: parent; width: 14; height: 14; kind: "xmark"; color: Qt.rgba(1, 1, 1, 0.6) }
                MouseArea { anchors.fill: parent; onClicked: panel.hide() }
            }
        }

        Text {
            visible: sync.networkHint !== "" || !sync.available
            width: parent.width
            wrapMode: Text.Wrap
            text: sync.available ? sync.networkHint : "同步服务无法启动（端口被占用或网络不可用）"
            color: "#FF9F0A"
            font { family: panel.uiFont; pixelSize: 12 }
        }

        // MARK: Incoming pairing
        Rectangle {
            visible: panel.incoming
            width: parent.width
            height: incomingColumn.implicitHeight + 28
            radius: 10
            color: Qt.rgba(0.04, 0.52, 1, 0.16)
            border.color: Qt.rgba(0.04, 0.52, 1, 0.45)
            Column {
                id: incomingColumn
                x: 14; y: 14
                width: parent.width - 28
                spacing: 8
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: (panel.pairing.hostname || "一台设备") + "（" + (panel.pairing.address || "") + "）请求配对"
                    color: "white"
                    font { family: panel.uiFont; pixelSize: 13; weight: Font.DemiBold }
                }
                Text {
                    visible: panel.pairing.stage === "code" || panel.pairing.stage === "verifying"
                    text: (panel.pairing.code || "").replace(/(\d{3})(\d{3})/, "$1 $2")
                    color: "white"
                    font { family: "Consolas"; pixelSize: 30; weight: Font.Bold; letterSpacing: 2 }
                }
                Text {
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: panel.pairing.stage === "done" ? "配对成功"
                        : panel.pairing.stage === "failed" ? panel.pairing.error
                        : panel.pairing.stage === "finishing" ? "正在完成配对…"
                        : "请在对方设备上输入此配对码，2 分钟内有效"
                    color: panel.pairing.stage === "failed" ? "#FF6961" : panel.secondary
                    font { family: panel.uiFont; pixelSize: 12 }
                }
                TextButton {
                    visible: panel.pairing.stage === "code" || panel.pairing.stage === "failed"
                    text: panel.pairing.stage === "failed" ? "关闭" : "拒绝"
                    onClicked: panel.pairing.stage === "failed" ? sync.cancelPairing() : sync.rejectPairing()
                }
            }
        }

        // MARK: This device
        Column {
            width: parent.width
            spacing: 6
            Text { text: "本机"; color: panel.secondary; font { family: panel.uiFont; pixelSize: 12; weight: Font.DemiBold } }
            Rectangle {
                width: parent.width
                height: 92
                radius: 10
                color: Qt.rgba(1, 1, 1, 0.05)
                Item {
                    x: 12; y: 6
                    width: parent.width - 24
                    height: 38
                    TextInput {
                        id: nameInput
                        anchors.left: parent.left
                        anchors.right: pencil.left
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        text: sync.deviceName
                        color: "white"
                        selectionColor: "#0A84FF"
                        clip: true
                        font { family: panel.uiFont; pixelSize: 14; weight: Font.DemiBold }
                        onEditingFinished: sync.deviceName = text
                        Keys.onEscapePressed: { text = sync.deviceName; focus = false }
                    }
                    DeckIcon {
                        id: pencil
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14; height: 14
                        kind: "pencil"
                        color: Qt.rgba(1, 1, 1, 0.5)
                        MouseArea { anchors.fill: parent; anchors.margins: -6; onClicked: { nameInput.forceActiveFocus(); nameInput.selectAll() } }
                    }
                }
                Rectangle { x: 12; y: 46; width: parent.width - 24; height: 1; color: Qt.rgba(1, 1, 1, 0.08) }
                Item {
                    x: 12; y: 48
                    width: parent.width - 24
                    height: 40
                    Column {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 1
                        Text {
                            text: sync.visible ? "局域网可见 · " + Math.floor(sync.visibleSecondsLeft / 60) + ":"
                                                 + ("0" + sync.visibleSecondsLeft % 60).slice(-2)
                                               : "局域网可见"
                            color: "white"
                            font { family: panel.uiFont; pixelSize: 13 }
                        }
                        Text {
                            text: sync.visible ? "本次标识 " + sync.visibilityTag : "开启后 3 分钟内可被附近设备发现"
                            color: panel.secondary
                            font { family: panel.uiFont; pixelSize: 11 }
                        }
                    }
                    ToggleSwitch {
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        checked: sync.visible
                        enabled: sync.available
                        onToggled: (checked) => sync.visible = checked
                    }
                }
            }
        }

        // MARK: My devices
        Column {
            visible: sync.devices.length > 0
            width: parent.width
            spacing: 6
            Text { text: "我的设备"; color: panel.secondary; font { family: panel.uiFont; pixelSize: 12; weight: Font.DemiBold } }
            Rectangle {
                width: parent.width
                height: deviceColumn.implicitHeight
                radius: 10
                color: Qt.rgba(1, 1, 1, 0.05)
                Column {
                    id: deviceColumn
                    width: parent.width
                    Repeater {
                        model: sync.devices
                        delegate: Item {
                            required property var modelData
                            width: deviceColumn.width
                            height: 46
                            HoverHandler { id: deviceHover }
                            Rectangle {
                                id: stateDot
                                x: 14
                                anchors.verticalCenter: parent.verticalCenter
                                width: 9; height: 9; radius: 4.5
                                color: modelData.state === "offline" ? "transparent" : "#30D158"
                                border.width: modelData.state === "offline" ? 1.5 : 0
                                border.color: Qt.rgba(1, 1, 1, 0.45)
                                RotationAnimator on rotation {
                                    running: modelData.state === "syncing"
                                    from: 0; to: 360; duration: 900; loops: Animation.Infinite
                                }
                            }
                            Column {
                                anchors.left: stateDot.right
                                anchors.leftMargin: 10
                                anchors.right: actions.left
                                anchors.verticalCenter: parent.verticalCenter
                                Text {
                                    width: parent.width
                                    elide: Text.ElideRight
                                    text: modelData.hostname + (modelData.address ? " · " + modelData.address : "")
                                    color: "white"
                                    font { family: panel.uiFont; pixelSize: 13 }
                                }
                                Text {
                                    text: modelData.state === "syncing" ? "同步中" : modelData.lastSeen
                                    color: panel.secondary
                                    font { family: panel.uiFont; pixelSize: 11 }
                                }
                            }
                            Row {
                                id: actions
                                anchors.right: parent.right
                                anchors.rightMargin: 10
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6
                                visible: deviceHover.hovered
                                TextButton { text: "立即同步"; enabled: !sync.paused; onClicked: sync.syncNow() }
                                TextButton { text: "取消配对"; destructive: true; onClicked: sync.unpair(modelData.deviceId) }
                            }
                        }
                    }
                }
            }
        }

        // MARK: Nearby devices
        Column {
            width: parent.width
            spacing: 6
            Item {
                width: parent.width
                height: 18
                Text { text: "附近的设备"; color: panel.secondary; font { family: panel.uiFont; pixelSize: 12; weight: Font.DemiBold } }
                Row {
                    anchors.right: parent.right
                    spacing: 5
                    DeckIcon {
                        width: 13; height: 13
                        kind: "sync"
                        color: Qt.rgba(1, 1, 1, 0.6)
                        anchors.verticalCenter: parent.verticalCenter
                        RotationAnimator on rotation {
                            running: sync.searching
                            from: 0; to: 360; duration: 900; loops: Animation.Infinite
                        }
                    }
                    Text {
                        text: sync.searching ? "正在查找…" : "刷新"
                        color: Qt.rgba(1, 1, 1, 0.7)
                        font { family: panel.uiFont; pixelSize: 12 }
                        MouseArea { anchors.fill: parent; anchors.margins: -6; onClicked: sync.searchNearby() }
                    }
                }
            }
            Rectangle {
                width: parent.width
                height: Math.max(52, nearbyColumn.implicitHeight)
                radius: 10
                color: Qt.rgba(1, 1, 1, 0.05)
                Text {
                    visible: sync.nearby.length === 0
                    anchors.centerIn: parent
                    width: parent.width - 28
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: sync.searching ? "正在查找…" : "没有找到。请在另一台设备上开启「局域网可见」后刷新"
                    color: panel.secondary
                    font { family: panel.uiFont; pixelSize: 12 }
                }
                Column {
                    id: nearbyColumn
                    width: parent.width
                    Repeater {
                        model: sync.nearby
                        delegate: Column {
                            id: nearbyRow
                            required property var modelData
                            readonly property bool pairingThis: panel.pairing.active === true
                                && panel.pairing.role === "outgoing" && panel.pairing.key === modelData.key
                            width: nearbyColumn.width
                            Item {
                                width: parent.width
                                height: 46
                                Column {
                                    x: 14
                                    anchors.right: pairButton.left
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    Text {
                                        width: parent.width
                                        elide: Text.ElideRight
                                        text: modelData.hostname + " · " + modelData.address
                                        color: "white"
                                        font { family: panel.uiFont; pixelSize: 13 }
                                    }
                                    Text {
                                        text: "标识 " + modelData.tag
                                        color: panel.secondary
                                        font { family: panel.uiFont; pixelSize: 11 }
                                    }
                                }
                                TextButton {
                                    id: pairButton
                                    anchors.right: parent.right
                                    anchors.rightMargin: 10
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: !nearbyRow.pairingThis
                                    enabled: modelData.pairable && panel.pairing.active !== true
                                    accent: true
                                    text: "配对"
                                    onClicked: sync.pair(modelData.key)
                                }
                            }
                            // Inline pairing for this device.
                            Item {
                                visible: nearbyRow.pairingThis
                                width: parent.width
                                height: visible ? pairingColumn.implicitHeight + 12 : 0
                                Column {
                                    id: pairingColumn
                                    x: 14
                                    width: parent.width - 28
                                    spacing: 6
                                    Text {
                                        visible: panel.pairing.stage === "connecting" || panel.pairing.stage === "verifying"
                                                 || panel.pairing.stage === "finishing" || panel.pairing.stage === "done"
                                        text: panel.pairing.stage === "done" ? "配对成功"
                                            : panel.pairing.stage === "connecting" ? "正在连接…" : "正在验证…"
                                        color: panel.secondary
                                        font { family: panel.uiFont; pixelSize: 12 }
                                    }
                                    Row {
                                        visible: panel.pairing.stage === "code"
                                        spacing: 8
                                        Rectangle {
                                            width: 120; height: 28; radius: 6
                                            color: Qt.rgba(0, 0, 0, 0.3)
                                            border.color: codeInput.activeFocus ? "#0A84FF" : Qt.rgba(1, 1, 1, 0.15)
                                            TextInput {
                                                id: codeInput
                                                anchors.fill: parent
                                                anchors.leftMargin: 10
                                                verticalAlignment: TextInput.AlignVCenter
                                                color: "white"
                                                maximumLength: 6
                                                inputMethodHints: Qt.ImhDigitsOnly
                                                validator: RegularExpressionValidator { regularExpression: /\d{0,6}/ }
                                                font { family: "Consolas"; pixelSize: 16; letterSpacing: 2 }
                                                onVisibleChanged: if (visible) { text = ""; forceActiveFocus() }
                                                onAccepted: if (text.length === 6) sync.submitPairingCode(text)
                                                Text {
                                                    visible: codeInput.text.length === 0
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    text: "配对码"
                                                    color: Qt.rgba(1, 1, 1, 0.3)
                                                    font { family: panel.uiFont; pixelSize: 13 }
                                                }
                                            }
                                        }
                                        TextButton {
                                            text: "确定"
                                            accent: true
                                            enabled: codeInput.text.length === 6
                                            onClicked: sync.submitPairingCode(codeInput.text)
                                        }
                                        TextButton { text: "取消"; onClicked: sync.cancelPairing() }
                                    }
                                    Text {
                                        visible: panel.pairing.stage === "code" && panel.pairing.error !== ""
                                        text: panel.pairing.error + "，还可尝试 " + panel.pairing.attemptsLeft + " 次"
                                        color: "#FF6961"
                                        font { family: panel.uiFont; pixelSize: 12 }
                                    }
                                    Row {
                                        visible: panel.pairing.stage === "failed"
                                        spacing: 8
                                        Text {
                                            text: panel.pairing.error
                                            color: "#FF6961"
                                            anchors.verticalCenter: parent.verticalCenter
                                            font { family: panel.uiFont; pixelSize: 12 }
                                        }
                                        TextButton { text: "关闭"; onClicked: sync.cancelPairing() }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // MARK: Pause
        Item {
            width: parent.width
            height: 30
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "暂停同步"
                color: "white"
                font { family: panel.uiFont; pixelSize: 13 }
            }
            ToggleSwitch {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                checked: sync.paused
                enabled: sync.available
                onToggled: (checked) => sync.paused = checked
            }
        }
    }
}
