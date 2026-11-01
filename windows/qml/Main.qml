import QtQuick
import QtQuick.Effects
import QtQuick.Window

// Port of HistoryPanelView (toolbar, category entries, card strip, empty
// state) and the PanelController key routing.
Window {
    id: root
    flags: Qt.FramelessWindowHint | Qt.Tool | Qt.WindowStaysOnTopHint
    color: "transparent"
    visible: false
    title: "ClipDeck"

    readonly property real cardSide: Math.max(166, cards.height - 24)
    readonly property string uiFont: "Segoe UI"

    onActiveChanged: if (!active) deck.setQuickPasteVisible(false)

    // Global rect of the card a popover points at; re-evaluated while the
    // strip scrolls or the Deck moves.
    readonly property rect popoverAnchor: {
        const index = deck.popover.index !== undefined ? deck.popover.index : -1
        const dependencies = [cards.contentX, root.x, root.y, root.width, root.height, root.cardSide, deck.deckHeight]
        const item = index >= 0 ? cards.itemAtIndex(index) : null
        if (!item) return Qt.rect(root.x + root.width / 2 - 1, root.y + root.height - deck.deckHeight + 60, 2, 2)
        const origin = item.mapToGlobal(0, 0)
        return Qt.rect(origin.x, origin.y, item.width, item.height)
    }

    PopoverWindow { mode: "preview"; anchorRect: root.popoverAnchor }
    PopoverWindow { mode: "editor"; anchorRect: root.popoverAnchor }
    ContextMenuWindow { id: contextMenu }

    Connections {
        target: deck
        function onDeckShown() {
            deckRoot.forceActiveFocus()
            root.reveal(false)
        }
        function onRevealRequested() { root.reveal(true) }
        function onSelectionChanged() { root.reveal(true) }
        function onSearchChanged() {
            if (searchInput.text !== deck.searchQuery) searchInput.text = deck.searchQuery
            if (deck.searchEditing && !searchInput.activeFocus) searchInput.forceActiveFocus()
            else if (!deck.searchEditing && searchInput.activeFocus) deckRoot.forceActiveFocus()
        }
    }

    Item {
        id: deckRoot
        // The window is as tall as the Deck can get; the Deck is its bottom part.
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: deck.deckHeight
        focus: true

        // Material (DeckMaterialView): the frosted screen behind the Deck,
        // clipped to 24 pt corners, then a light tint, black 14 % (mac 20 %,
        // whose material already lets less through) and
        // white 6 % → clear → black 6 %. With 背景模糊 off (tray menu, as on
        // Linux) the live desktop shows through black 37 % instead.
        Item {
            id: material
            visible: deck.backdropBlur
            anchors.fill: parent
            layer.enabled: true
            layer.effect: MultiEffect {
                maskEnabled: true
                maskSource: materialMask
                maskThresholdMin: 0.5
                maskSpreadAtMin: 1.0
            }
            Image {
                // Screen-aligned: the capture covers the whole window.
                y: deckRoot.height - root.height
                width: root.width
                height: root.height
                source: deck.backdropUrl
                smooth: true
                cache: false
            }
            // underWindowBackground lets much of the desktop through; this
            // tint only evens out the brightest wallpapers.
            Rectangle {
                anchors.fill: parent
                color: Qt.rgba(0.118, 0.118, 0.125, 0.22)
            }
        }
        Rectangle {
            id: materialMask
            anchors.fill: parent
            radius: 24
            visible: false
            layer.enabled: true
            layer.smooth: true
        }
        Rectangle {
            anchors.fill: parent
            radius: 24
            color: Qt.rgba(0, 0, 0, deck.backdropBlur ? 0.14 : 0.37)
        }
        Rectangle {
            anchors.fill: parent
            radius: 24
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.rgba(1, 1, 1, 0.06) }
                GradientStop { position: 0.5; color: "transparent" }
                GradientStop { position: 1; color: Qt.rgba(0, 0, 0, 0.06) }
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

            // LAN sync in progress (docs/lan-sync-design.md §8): shown in the
            // top-left corner only while items are sent or received.
            SyncLoader {
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                running: deck.syncing
            }

            Row {
                id: toolbarCenter
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: -27
                spacing: 14

                // Search: magnifier button, or the 230×34 capsule while searching.
                Item {
                    width: deck.searchActive ? 230 : 28
                    height: 34
                    anchors.verticalCenter: parent.verticalCenter
                    DeckIcon {
                        visible: !deck.searchActive
                        anchors.centerIn: parent
                        width: 22; height: 22
                        kind: "search"
                        color: Qt.rgba(1, 1, 1, 0.92)
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -6
                            onClicked: deck.beginSearch()
                        }
                    }
                    Rectangle {
                        visible: deck.searchActive
                        anchors.fill: parent
                        radius: height / 2
                        color: Qt.rgba(1, 1, 1, 0.14)
                        DeckIcon {
                            id: fieldIcon
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            width: 16; height: 16
                            kind: "search"
                            color: Qt.rgba(1, 1, 1, 0.62)
                        }
                        TextInput {
                            id: searchInput
                            anchors.left: fieldIcon.right
                            anchors.leftMargin: 7
                            anchors.right: clearButton.left
                            anchors.rightMargin: 4
                            anchors.verticalCenter: parent.verticalCenter
                            color: "white"
                            selectionColor: "#0A84FF"
                            clip: true
                            font { family: root.uiFont; pixelSize: 14; weight: Font.Medium }
                            onTextEdited: deck.searchQuery = text
                            onActiveFocusChanged: deck.setSearchFieldFocused(activeFocus)
                            Text {
                                visible: searchInput.text.length === 0 && !searchInput.inputMethodComposing
                                text: "搜索文本"
                                color: Qt.rgba(1, 1, 1, 0.42)
                                font: searchInput.font
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Keys.onPressed: (event) => root.handleSearchKey(event)
                            TapHandler { onTapped: deck.setSearchFieldFocused(true) }
                        }
                        Item {
                            id: clearButton
                            visible: searchInput.text.length > 0
                            width: 22; height: 22
                            anchors.right: parent.right
                            anchors.rightMargin: 7
                            anchors.verticalCenter: parent.verticalCenter
                            Rectangle {
                                anchors.centerIn: parent
                                width: 16; height: 16; radius: 8
                                color: Qt.rgba(1, 1, 1, 0.48)
                                DeckIcon { anchors.centerIn: parent; width: 14; height: 14; kind: "xmark"; color: "#2a2a2c" }
                            }
                            MouseArea { anchors.fill: parent; onClicked: deck.endSearch() }
                        }
                    }
                }

                // Fixed entries: History / 文本 / 图片 / 文件.
                Row {
                    spacing: 4
                    anchors.verticalCenter: parent.verticalCenter
                    Repeater {
                        model: [
                            { key: "all", icon: "history", label: "History" },
                            { key: "text", icon: "text", label: "文本" },
                            { key: "images", icon: "image", label: "图片" },
                            { key: "files", icon: "folder", label: "文件" }
                        ]
                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool selected: deck.selectedCategoryKey === modelData.key
                            readonly property bool showsLabel: selected || modelData.key === "all"
                            height: 35
                            width: entryRow.implicitWidth + 20
                            radius: 9
                            color: selected ? Qt.rgba(1, 1, 1, 0.16) : "transparent"
                            Row {
                                id: entryRow
                                anchors.centerIn: parent
                                spacing: 6
                                DeckIcon {
                                    kind: modelData.icon
                                    width: 19; height: 19
                                    color: Qt.rgba(1, 1, 1, selected ? 1 : 0.65)
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                Text {
                                    visible: showsLabel
                                    text: modelData.label
                                    color: Qt.rgba(1, 1, 1, selected ? 1 : 0.65)
                                    font { family: root.uiFont; pixelSize: 13; weight: Font.DemiBold }
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }
                            MouseArea { anchors.fill: parent; onClicked: deck.selectCategoryKey(modelData.key) }
                        }
                    }
                }

                Rectangle {
                    visible: deck.categories.length > 0
                    width: 1; height: 22
                    color: Qt.rgba(1, 1, 1, 0.18)
                    anchors.verticalCenter: parent.verticalCenter
                }

                // Automatic categories: 96×31 capsules, horizontally scrollable.
                Flickable {
                    id: categoryStrip
                    visible: deck.categories.length > 0
                    width: Math.max(240, Math.min(760, categoryRow.implicitWidth))
                    height: 31
                    anchors.verticalCenter: parent.verticalCenter
                    contentWidth: categoryRow.implicitWidth
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    Row {
                        id: categoryRow
                        spacing: 12
                        Repeater {
                            model: deck.categories
                            delegate: Rectangle {
                                required property var modelData
                                readonly property bool selected: deck.selectedCategoryKey === modelData.key
                                width: 96; height: 31
                                radius: height / 2
                                color: selected ? Qt.rgba(1, 1, 1, 0.14) : "transparent"
                                Row {
                                    anchors.centerIn: parent
                                    spacing: 7
                                    Rectangle {
                                        width: 11; height: 11; radius: 5.5
                                        color: modelData.color
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                    Text {
                                        text: modelData.name
                                        color: Qt.rgba(1, 1, 1, 0.92)
                                        font { family: root.uiFont; pixelSize: 13; weight: Font.DemiBold }
                                        width: Math.min(implicitWidth, 64)
                                        elide: Text.ElideRight
                                    }
                                }
                                MouseArea { anchors.fill: parent; onClicked: deck.selectCategoryKey(modelData.key) }
                            }
                        }
                    }
                }

                DeckIcon {
                    kind: "plus"
                    width: 22; height: 22
                    color: Qt.rgba(1, 1, 1, 0.92)
                    anchors.verticalCenter: parent.verticalCenter
                    MouseArea { anchors.fill: parent; anchors.margins: -6; onClicked: deck.openSettings("categories") }
                }
            }

            // "…" → settings, category page.
            Item {
                width: 38; height: 38
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                Row {
                    anchors.centerIn: parent
                    spacing: 4
                    Repeater {
                        model: 3
                        Rectangle { width: 5; height: 5; radius: 2.5; color: Qt.rgba(1, 1, 1, 0.82) }
                    }
                }
                MouseArea { anchors.fill: parent; onClicked: deck.openSettings("categories") }
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
            model: deck.model
            delegate: Card {
                id: cardDelegate
                required property int index
                required kind
                required title
                required categoryLabel
                required time
                required text
                required highlightedText
                required characterCount
                required monospaced
                required imageUrl
                required imageSizeText
                required imageAngle
                required files
                required sourceIconUrl
                required sourceName
                required fromOtherDevice
                required linkTitle
                required linkHost
                required linkIconUrl
                required linkPreviewUrl
                required colorValue
                required colorText
                required immersive

                side: root.cardSide
                selected: index === deck.selectedIndex
                quickNumber: deck.quickPasteVisible && index < 9 ? index + 1 : 0
                shadowsEnabled: !deck.resizing

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: (mouse) => {
                        if (mouse.button === Qt.RightButton) {
                            const point = mapToGlobal(mouse.x, mouse.y)
                            contextMenu.openAt(cardDelegate.index, point.x, point.y,
                                               cardDelegate.kind === "link", cardDelegate.kind === "files")
                            return
                        }
                        contextMenu.close()
                        deck.select(cardDelegate.index)
                    }
                    onDoubleClicked: (mouse) => {
                        if (mouse.button === Qt.LeftButton)
                            deck.paste(cardDelegate.index, (mouse.modifiers & deck.plainTextModifier) !== 0)
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

        // Empty state.
        Rectangle {
            visible: cards.count === 0
            anchors.centerIn: cards
            width: emptyColumn.implicitWidth + 56
            height: emptyColumn.implicitHeight + 68
            radius: 22
            color: Qt.rgba(1, 1, 1, 0.10)
            Column {
                id: emptyColumn
                anchors.centerIn: parent
                spacing: 6
                DeckIcon {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 30; height: 30
                    kind: deck.searchActive ? "search" : "history"
                    color: Qt.rgba(1, 1, 1, 0.72)
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: deck.searchActive ? "没有找到匹配内容" : "Copy something to get started"
                    color: "white"
                    font { family: root.uiFont; pixelSize: 16; weight: Font.DemiBold }
                }
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: deck.searchActive ? "尝试其他关键词" : "复制的文本、图片和文件会显示在这里"
                    color: Qt.rgba(1, 1, 1, 0.55)
                    font { family: root.uiFont; pointSize: 12.5 * 0.75 }
                }
            }
        }

        // File paste feedback (FilePasteController): checking / unavailable / partial.
        Rectangle {
            visible: deck.feedbackMessage !== ""
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 18
            width: feedbackText.implicitWidth + 32
            height: 36
            radius: 18
            color: Qt.rgba(0, 0, 0, 0.72)
            Text {
                id: feedbackText
                anchors.centerIn: parent
                text: deck.feedbackMessage
                color: "white"
                font { family: root.uiFont; pixelSize: 13; weight: Font.DemiBold }
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
            onPressed: (mouse) => deck.beginResize(mapToGlobal(mouse.x, mouse.y).y)
            onPositionChanged: (mouse) => { if (pressed) deck.updateResize(mapToGlobal(mouse.x, mouse.y).y) }
            onReleased: deck.endResize()
            Rectangle {
                visible: resizeArea.pressed
                anchors.horizontalCenter: parent.horizontalCenter
                y: 5
                width: 44; height: 3; radius: 1.5
                color: Qt.rgba(1, 1, 1, 0.32)
            }
        }

        Keys.onPressed: (event) => root.handleKey(event, false)
        Keys.onReleased: (event) => {
            if (event.key === deck.quickPasteModifierKey) deck.setQuickPasteVisible(false)
        }
    }

    function clampContentX(value) {
        const minimum = cards.originX - cards.leftMargin
        const maximum = Math.max(minimum, cards.originX + cards.contentWidth + cards.rightMargin - cards.width)
        return Math.min(Math.max(value, minimum), maximum)
    }

    // revealSelection(): centre the selected card (0.18 s ease-out).
    function reveal(animated) {
        const index = deck.selectedIndex
        if (index < 0 || cards.count === 0) return
        const itemX = cards.originX + index * (cardSide + cards.spacing)
        const target = clampContentX(itemX + cardSide / 2 - cards.width / 2)
        cards.scrollTarget = target
        scrollAnimation.stop()
        if (!animated || deck.resizing) {
            cards.contentX = target
            return
        }
        scrollAnimation.duration = 180
        scrollAnimation.to = target
        scrollAnimation.start()
    }

    // Keys while the search field has focus: the field edits text, the
    // Deck keeps arrows, Enter, Escape and Ctrl shortcuts.
    function handleSearchKey(event) {
        const modifiers = event.modifiers & (Qt.ControlModifier | Qt.ShiftModifier | Qt.AltModifier | Qt.MetaModifier)
        if (event.key === Qt.Key_Backspace && modifiers === 0) {
            // Let the field delete first; collapse once it is empty (not while composing).
            Qt.callLater(() => { if (!searchInput.inputMethodComposing) deck.collapseEmptySearch() })
            return
        }
        if (event.key === Qt.Key_Delete && modifiers === 0) return
        if (event.key === Qt.Key_Home || event.key === Qt.Key_End) return
        if (modifiers === Qt.ControlModifier && (event.key === Qt.Key_A || event.key === Qt.Key_C || event.key === Qt.Key_X)) return
        root.handleKey(event, true)
    }

    // 1…9 on the number row or keypad. Scan codes keep Shift+1 ("!") working
    // when Shift is the configured quick-paste modifier.
    function quickPasteIndex(event) {
        if (event.key >= Qt.Key_1 && event.key <= Qt.Key_9) return event.key - Qt.Key_1
        const rowCodes = [0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A]
        const padCodes = [0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49]
        let index = rowCodes.indexOf(event.nativeScanCode)
        if (index < 0) index = padCodes.indexOf(event.nativeScanCode)
        return index
    }

    function handleKey(event, fromSearchField) {
        const modifiers = event.modifiers & (Qt.ControlModifier | Qt.ShiftModifier | Qt.AltModifier | Qt.MetaModifier)
        const ctrl = modifiers === Qt.ControlModifier
        if (contextMenu.handleKey(event)) {
            event.accepted = true
            return
        }
        let handled = true
        const categoryDirection = deck.categoryShortcutDirection(event.key, modifiers)
        if (event.key === deck.quickPasteModifierKey) {
            deck.setQuickPasteVisible(true)
            handled = false
        } else if (ctrl && event.key === Qt.Key_Q) {
            deck.quit()
        } else if (event.key === Qt.Key_Escape) {
            deck.escape()
        } else if ((event.modifiers & deck.quickPasteModifier) && root.quickPasteIndex(event) >= 0) {
            deck.quickPaste(root.quickPasteIndex(event))
        } else if (categoryDirection !== 0) {
            deck.moveCategory(categoryDirection)
        } else if (ctrl && event.key === Qt.Key_O) {
            deck.openSelection()
        } else if (ctrl && event.key === Qt.Key_C && !fromSearchField && !deck.searchEditing) {
            deck.copySelection()
        } else if (ctrl && event.key === Qt.Key_E) {
            deck.editSelection()
        } else if (ctrl && event.key === Qt.Key_R) {
            deck.renameSelection()
        } else if (modifiers === 0 && (event.key === Qt.Key_Up || event.key === Qt.Key_Down)
                   && deck.popover.kind === "preview") {
            deck.scrollPreview(event.key === Qt.Key_Down ? 1 : -1)
        } else if (modifiers === 0 && event.key === Qt.Key_Space && !fromSearchField && !deck.searchEditing) {
            deck.togglePreview()
        } else if (ctrl && event.key === Qt.Key_V && deck.searchActive) {
            deck.pasteClipboardIntoSearch()
        } else if (ctrl && event.key === Qt.Key_F) {
            deck.beginSearch()
        } else if (ctrl && event.key === Qt.Key_Comma) {
            deck.openSettings("general")
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            deck.pasteSelection()
        } else if (event.key === Qt.Key_Backspace) {
            if (deck.searchActive) {
                if (ctrl) deck.deleteSelection()
                else if (modifiers === 0) deck.resumeSearchAndDeleteBackward()
            } else if (ctrl) {
                deck.deleteSelection()
            }
        } else if (event.key === Qt.Key_Delete && modifiers === 0) {
            deck.deleteSelection()
        } else if (modifiers === 0 && (event.key === Qt.Key_Left || event.key === Qt.Key_Right)) {
            deck.moveSelection(event.key === Qt.Key_Left ? -1 : 1)
        } else if (!fromSearchField && (event.modifiers & (Qt.ControlModifier | Qt.AltModifier)) === 0
                   && event.key !== Qt.Key_Space && event.text.length > 0 && event.text.charCodeAt(0) >= 0x20) {
            // Typing anywhere starts a search (typeToSearchCharacters).
            deck.typeToSearch(event.text)
        } else {
            handled = false
        }
        event.accepted = handled
    }
}
