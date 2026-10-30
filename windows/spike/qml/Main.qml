import QtQuick
import QtQuick.Effects
import QtQuick.Window

Window {
    id: root
    flags: Qt.FramelessWindowHint | Qt.Tool | Qt.WindowStaysOnTopHint
    color: "transparent"
    visible: false
    title: "ClipDeck Spike"

    property bool showHud: true
    property bool shadowsEnabled: true
    property bool masksEnabled: true
    property bool quickNumbers: false
    property int selectedIndex: 0
    property string benchmarkResult: ""
    readonly property var entries: buildEntries(100)
    readonly property real cardSide: Math.max(166, cards.height - 24)
    readonly property bool solidMode: spike.effectMode === 4
    readonly property bool squareMode: spike.effectMode === 3
    readonly property string uiFont: "Segoe UI"

    onActiveChanged: if (!active) quickNumbers = false

    Connections {
        target: spike
        function onDeckShown() {
            deck.forceActiveFocus()
            root.reveal(root.selectedIndex, false)
            if (spike.selfTest) selfTestTimer.start()
        }
        function onGraphicsApiChanged() {
            // The software renderer has no shader effects.
            if (spike.graphicsApi === "software") {
                root.shadowsEnabled = false
                root.masksEnabled = false
            }
        }
    }

    Item {
        id: deck
        anchors.fill: parent
        anchors.topMargin: spike.shadowMarginTop
        anchors.leftMargin: spike.shadowMarginSide
        anchors.rightMargin: spike.shadowMarginSide
        focus: true

        // Container: material (OS) + black 20% + white 6% → clear → black 6%.
        Item {
            id: background
            anchors.fill: parent
            layer.enabled: root.solidMode
            layer.effect: MultiEffect {
                shadowEnabled: true
                shadowColor: Qt.rgba(0, 0, 0, 0.45)
                blurMax: 24
                shadowBlur: 1.0
                shadowVerticalOffset: 0
            }
            Rectangle {
                anchors.fill: parent
                radius: root.squareMode ? 0 : 24
                color: root.solidMode ? Qt.rgba(0.118, 0.118, 0.125, 0.97) : Qt.rgba(0, 0, 0, 0.20)
            }
            Rectangle {
                anchors.fill: parent
                radius: root.squareMode ? 0 : 24
                gradient: Gradient {
                    GradientStop { position: 0; color: Qt.rgba(1, 1, 1, 0.06) }
                    GradientStop { position: 0.5; color: "transparent" }
                    GradientStop { position: 1; color: Qt.rgba(0, 0, 0, 0.06) }
                }
            }
        }

        Item {
            id: toolbar
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.topMargin: 7
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            height: 51

            Row {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: -27
                spacing: 20

                DeckIcon { kind: "search"; width: 22; height: 22; color: Qt.rgba(1, 1, 1, 0.92); anchors.verticalCenter: parent.verticalCenter }

                Row {
                    spacing: 7
                    anchors.verticalCenter: parent.verticalCenter
                    Row {
                        spacing: 4
                        anchors.verticalCenter: parent.verticalCenter
                        Repeater {
                            model: [
                                { kind: "history", label: "History" },
                                { kind: "text", label: "文本" },
                                { kind: "image", label: "图片" },
                                { kind: "folder", label: "文件" }
                            ]
                            delegate: Rectangle {
                                required property var modelData
                                required property int index
                                readonly property bool selected: index === 0
                                height: 35
                                width: typeRow.implicitWidth + 20
                                radius: 9
                                color: selected ? Qt.rgba(1, 1, 1, 0.16) : "transparent"
                                Row {
                                    id: typeRow
                                    anchors.centerIn: parent
                                    spacing: 6
                                    DeckIcon {
                                        kind: modelData.kind
                                        width: 19; height: 19
                                        color: Qt.rgba(1, 1, 1, selected ? 1 : 0.65)
                                    }
                                    Text {
                                        visible: selected
                                        text: modelData.label
                                        color: "white"
                                        font { family: root.uiFont; pixelSize: 13; weight: Font.DemiBold }
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                            }
                        }
                    }
                    Rectangle { width: 1; height: 22; color: Qt.rgba(1, 1, 1, 0.18); anchors.verticalCenter: parent.verticalCenter }
                    Item { width: 7; height: 1 }
                    Row {
                        spacing: 12
                        anchors.verticalCenter: parent.verticalCenter
                        Repeater {
                            model: [
                                { name: "URL", color: "#BF5AF2" }, { name: "JSON", color: "#64D2FF" },
                                { name: "SQL", color: "#30D158" }, { name: "Markdown", color: "#FF375F" },
                                { name: "代码", color: "#5E5CE6" }, { name: "色彩", color: "#66D4CF" }
                            ]
                            delegate: Item {
                                required property var modelData
                                width: 96; height: 31
                                Row {
                                    anchors.centerIn: parent
                                    spacing: 7
                                    Rectangle { width: 11; height: 11; radius: 5.5; color: modelData.color; anchors.verticalCenter: parent.verticalCenter }
                                    Text {
                                        text: modelData.name
                                        color: Qt.rgba(1, 1, 1, 0.92)
                                        font { family: root.uiFont; pixelSize: 13; weight: Font.DemiBold }
                                        width: Math.min(implicitWidth, 70)
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                        }
                    }
                }

                DeckIcon { kind: "plus"; width: 22; height: 22; color: Qt.rgba(1, 1, 1, 0.92); anchors.verticalCenter: parent.verticalCenter }
            }

            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4
                Repeater {
                    model: 3
                    Rectangle { width: 5; height: 5; radius: 2.5; color: Qt.rgba(1, 1, 1, 0.82) }
                }
            }
        }

        ListView {
            id: cards
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: toolbar.bottom
            anchors.bottom: parent.bottom
            orientation: ListView.Horizontal
            spacing: 24
            leftMargin: 22
            rightMargin: 22
            topMargin: 12
            boundsBehavior: Flickable.StopAtBounds
            cacheBuffer: root.cardSide * 2
            model: root.entries
            delegate: Card {
                required property var modelData
                required property int index
                entry: modelData
                side: root.cardSide
                selected: index === root.selectedIndex
                quickNumber: root.quickNumbers && index < 9 ? index + 1 : 0
                shadowsEnabled: root.shadowsEnabled && !resizeArea.pressed
                masksEnabled: root.masksEnabled

                MouseArea {
                    anchors.fill: parent
                    onClicked: root.selectedIndex = index
                    onDoubleClicked: (mouse) => {
                        root.selectedIndex = index
                        root.paste(index, (mouse.modifiers & Qt.ShiftModifier) !== 0, "double-click")
                    }
                }
            }

            NumberAnimation {
                id: scrollAnimation
                target: cards
                property: "contentX"
                easing.type: Easing.BezierSpline
                easing.bezierCurve: [0, 0, 0.58, 1, 1, 1]
            }
            property real scrollTarget: 0

            // Vertical wheel scrolls the strip (PanelController.routeVerticalWheelToCards).
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                onWheel: (wheel) => {
                    const vertical = Math.abs(wheel.angleDelta.y) >= Math.abs(wheel.angleDelta.x)
                    const raw = vertical ? wheel.angleDelta.y : wheel.angleDelta.x
                    if (raw === 0) return
                    // Notched wheels report multiples of 120; precision touchpads don't.
                    const notched = raw % 120 === 0
                    const delta = notched ? -(raw / 120) * 44 : -raw
                    const start = scrollAnimation.running ? cards.scrollTarget : cards.contentX
                    cards.scrollTarget = root.clampContentX(start + delta)
                    scrollAnimation.stop()
                    scrollAnimation.duration = notched ? 140 : 80
                    scrollAnimation.to = cards.scrollTarget
                    scrollAnimation.start()
                    wheel.accepted = true
                }
            }
        }

        // Top 18 DIP resize strip (DeckResizeHandle).
        MouseArea {
            id: resizeArea
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 18
            cursorShape: Qt.SizeVerCursor
            onPressed: (mouse) => spike.beginResize(mapToGlobal(mouse.x, mouse.y).y)
            onPositionChanged: (mouse) => { if (pressed) spike.updateResize(mapToGlobal(mouse.x, mouse.y).y) }
            onReleased: {
                spike.endResize()
                root.reveal(root.selectedIndex, false)
            }
            Rectangle {
                visible: resizeArea.pressed
                anchors.horizontalCenter: parent.horizontalCenter
                y: 5
                width: 44; height: 3; radius: 1.5
                color: Qt.rgba(1, 1, 1, 0.32)
            }
        }

        Keys.onPressed: (event) => root.handleKey(event)
        Keys.onReleased: (event) => {
            if (event.key === Qt.Key_Control) root.quickNumbers = false
        }

        Rectangle {
            id: hud
            visible: root.showHud
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 14
            width: hudText.implicitWidth + 16
            height: hudText.implicitHeight + 12
            radius: 8
            color: Qt.rgba(0, 0, 0, 0.72)
            Text {
                id: hudText
                anchors.centerIn: parent
                color: "#E8E8E8"
                font { family: "Consolas"; pixelSize: 11 }
                text: [
                    spike.effectName + "  tint α=" + spike.tintAlpha + "  rhi=" + spike.graphicsApi,
                    "fps=" + frames.fps.toFixed(0) + "  card=" + root.cardSide.toFixed(0) + "  shadows=" + root.shadowsEnabled
                        + "  masks=" + root.masksEnabled + "  keepAcrylicOnResize=" + spike.keepAcrylicDuringResize
                        + "  pinned=" + spike.pinned,
                    "hotkey=" + spike.hotkeyName + "  " + spike.status,
                    root.benchmarkResult,
                    "F1-F4 effect  +/- tint  F5 shadows  F6 masks  F7 resize-acrylic  F8 pin  F9 benchmark  F10 HUD"
                ].filter(line => line.length > 0).join("\n")
            }
        }
    }

    FrameAnimation {
        id: frames
        running: root.visible
        property real fps: 0
        property var samples: []
        property bool recording: false
        onTriggered: {
            if (smoothFrameTime > 0) fps = 1 / smoothFrameTime
            if (recording) samples.push(frameTime * 1000)
        }
    }

    SequentialAnimation {
        id: benchmark
        property int legDuration: spike.selfTest ? 1500 : 6000
        ScriptAction { script: { frames.samples = []; frames.recording = true; cards.contentX = root.clampContentX(-1e9) } }
        NumberAnimation { target: cards; property: "contentX"; to: root.clampContentX(1e9); duration: benchmark.legDuration; easing.type: Easing.InOutQuad }
        NumberAnimation { target: cards; property: "contentX"; to: root.clampContentX(-1e9); duration: benchmark.legDuration; easing.type: Easing.InOutQuad }
        ScriptAction { script: root.finishBenchmark() }
    }

    Timer {
        id: selfTestTimer
        interval: 600
        onTriggered: benchmark.start()
    }

    function finishBenchmark() {
        frames.recording = false
        const samples = frames.samples.slice(1).sort((a, b) => a - b)
        if (samples.length === 0) {
            benchmarkResult = "benchmark: no frames"
        } else {
            const total = samples.reduce((sum, value) => sum + value, 0)
            const p99 = samples[Math.min(samples.length - 1, Math.floor(samples.length * 0.99))]
            const slow = samples.filter(value => value > 20).length
            benchmarkResult = "benchmark: frames=" + samples.length
                + " avg=" + (1000 * samples.length / total).toFixed(1) + "fps"
                + " p99=" + p99.toFixed(1) + "ms max=" + samples[samples.length - 1].toFixed(1) + "ms"
                + " >20ms=" + slow
                + " card=" + cardSide.toFixed(0) + " shadows=" + shadowsEnabled + " masks=" + masksEnabled
                + " rhi=" + spike.graphicsApi
        }
        spike.log("benchmark", benchmarkResult)
        if (spike.selfTest) spike.finishSelfTest(benchmarkResult)
    }

    function clampContentX(value) {
        const minimum = cards.originX - cards.leftMargin
        const maximum = Math.max(minimum, cards.originX + cards.contentWidth + cards.rightMargin - cards.width)
        return Math.min(Math.max(value, minimum), maximum)
    }

    function reveal(index, animated) {
        const itemX = cards.originX + index * (cardSide + cards.spacing)
        const target = clampContentX(itemX + cardSide / 2 - cards.width / 2)
        cards.scrollTarget = target
        scrollAnimation.stop()
        if (!animated) {
            cards.contentX = target
            return
        }
        scrollAnimation.duration = 180
        scrollAnimation.to = target
        scrollAnimation.start()
    }

    function select(index) {
        selectedIndex = Math.max(0, Math.min(entries.length - 1, index))
        reveal(selectedIndex, true)
    }

    function paste(index, plainText, trigger) {
        quickNumbers = false
        spike.paste(entries[index].plain, plainText, trigger)
    }

    function handleKey(event) {
        const modifiers = event.modifiers & (Qt.ControlModifier | Qt.ShiftModifier | Qt.AltModifier | Qt.MetaModifier)
        const ctrl = modifiers === Qt.ControlModifier
        let handled = true
        if (event.key === Qt.Key_Control) {
            quickNumbers = true
            handled = false
        } else if (event.key >= Qt.Key_F1 && event.key <= Qt.Key_F4) {
            spike.effectMode = event.key - Qt.Key_F1 + 1
        } else if (event.key === Qt.Key_F5) {
            shadowsEnabled = !shadowsEnabled
        } else if (event.key === Qt.Key_F6) {
            masksEnabled = !masksEnabled
        } else if (event.key === Qt.Key_F7) {
            spike.keepAcrylicDuringResize = !spike.keepAcrylicDuringResize
        } else if (event.key === Qt.Key_F8) {
            spike.pinned = !spike.pinned
        } else if (event.key === Qt.Key_F9) {
            benchmark.restart()
        } else if (event.key === Qt.Key_F10) {
            showHud = !showHud
        } else if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) {
            spike.adjustTint(16)
        } else if (event.key === Qt.Key_Minus) {
            spike.adjustTint(-16)
        } else if (event.key === Qt.Key_Escape) {
            spike.hideDeck("escape")
        } else if (ctrl && event.key === Qt.Key_Q) {
            spike.quit()
        } else if (ctrl && event.key >= Qt.Key_1 && event.key <= Qt.Key_9) {
            const index = event.key - Qt.Key_1
            if (index < entries.length) {
                selectedIndex = index
                paste(index, false, "ctrl+" + (index + 1))
            }
        } else if ((event.key === Qt.Key_Left || event.key === Qt.Key_Right) && ctrl) {
            spike.log("keys", "category " + (event.key === Qt.Key_Left ? "previous" : "next"))
        } else if ((event.key === Qt.Key_Left || event.key === Qt.Key_Right) && modifiers === 0) {
            select(selectedIndex + (event.key === Qt.Key_Left ? -1 : 1))
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            paste(selectedIndex, (modifiers & Qt.ShiftModifier) !== 0, "enter")
        } else {
            handled = false
        }
        event.accepted = handled
    }

    function buildEntries(count) {
        const times = ["现在", "3 分钟", "7 分钟", "15 分钟", "25 分钟", "40 分钟", "2 小时", "1 天"]
        const sources = ["C", "W", "E", "V", "N", "T"]
        const kw = "#BF5AF2", str = "#FF9F0A", num = "#40C8E0", type = "#66D4CF"
        const templates = [
            { kind: "text", title: "发布说明", category: "", mono: false,
              plain: "ClipDeck 让复制、查找和粘贴保持顺畅。\n所有示例内容均为虚拟数据。" },
            { kind: "code", title: "剪贴板模型", category: "Swift", mono: true,
              plain: "struct ClipItem {\n    let title: String\n    let copiedAt: Date\n}",
              styled: "<pre><font color='" + kw + "'>struct</font> ClipItem {\n    <font color='" + kw + "'>let</font> title: <font color='" + type + "'>String</font>\n    <font color='" + kw + "'>let</font> copiedAt: <font color='" + type + "'>Date</font>\n}</pre>" },
            { kind: "link", title: "产品指南", category: "URL", mono: false, host: "example.com",
              plain: "https://example.com/guide" },
            { kind: "code", title: "主题配置", category: "JSON", mono: true,
              plain: "{\n  \"theme\": \"dark\",\n  \"sync\": false\n}",
              styled: "<pre>{\n  <font color='#64D2FF'>\"theme\"</font>: <font color='" + str + "'>\"dark\"</font>,\n  <font color='#64D2FF'>\"sync\"</font>: <font color='" + kw + "'>false</font>\n}</pre>" },
            { kind: "color", title: "品牌色", category: "色彩", mono: false, plain: "#F54927" },
            { kind: "image", title: "图片", category: "", mono: false, plain: "1920 × 1080" },
            { kind: "code", title: "查询", category: "SQL", mono: true,
              plain: "SELECT id, title\nFROM content\nWHERE copy_count > 3\nORDER BY last_copied_at DESC\nLIMIT 20;",
              styled: "<pre><font color='" + kw + "'>SELECT</font> id, title\n<font color='" + kw + "'>FROM</font> content\n<font color='" + kw + "'>WHERE</font> copy_count &gt; <font color='" + num + "'>3</font>\n<font color='" + kw + "'>ORDER BY</font> last_copied_at <font color='" + kw + "'>DESC</font>\n<font color='" + kw + "'>LIMIT</font> <font color='" + num + "'>20</font>;</pre>" },
            { kind: "text", title: "项目计划", category: "Markdown", mono: false,
              plain: "# 今日计划\n\n- 整理灵感\n- 完成设计\n- 发布版本" }
        ]
        const result = []
        for (let index = 0; index < count; ++index) {
            const template = templates[index % templates.length]
            const entry = Object.assign({}, template)
            entry.styled = template.styled || template.plain
            entry.time = times[Math.min(times.length - 1, Math.floor(index / 4))]
            entry.source = sources[index % sources.length]
            entry.seed = index
            if (entry.kind === "image") entry.plain = index % 3 === 0 ? "1200 × 1600" : "1920 × 1080"
            if (!entry.host) entry.host = ""
            result.push(entry)
        }
        return result
    }
}
