import QtQuick
import ClipDeck.Native

// The script-editor component (mac: CodeMirror in ScriptEditorWebView) bound
// to settings.categoryScript: One Dark colours, line numbers, active line,
// Ctrl+/ comment, Tab / Shift+Tab indent, Ctrl+Enter test, Ctrl+Shift+F format.
Rectangle {
    id: editor
    property bool autofocus: false
    readonly property string monoFont: "Consolas"
    // TextEdit lays out at the font's own line height; the gutter and the
    // active line follow it so the numbers stay aligned.
    readonly property real lineHeight: textEdit.lineCount > 0 ? textEdit.contentHeight / textEdit.lineCount : 16
    color: Qt.rgba(0, 0, 0, 0.16)
    clip: true

    function focusEditor() { textEdit.forceActiveFocus() }

    Flickable {
        id: scroll
        anchors.fill: parent
        contentWidth: Math.max(width, gutter.width + textEdit.contentWidth + 24)
        contentHeight: Math.max(height, textEdit.contentHeight + 20)
        boundsBehavior: Flickable.StopAtBounds
        function ensureVisible(r) {
            const top = r.y + 10
            if (contentY > top) contentY = top
            else if (contentY + height < top + r.height + 10) contentY = top + r.height + 10 - height
            const left = r.x + textEdit.x
            if (contentX > left - gutter.width) contentX = Math.max(0, left - gutter.width - 8)
            else if (contentX + width < left + 16) contentX = left + 16 - width
        }

        // .cm-activeLine
        Rectangle {
            visible: textEdit.activeFocus
            x: 0
            y: textEdit.y + textEdit.cursorRectangle.y
            width: scroll.contentWidth
            height: editor.lineHeight
            color: Qt.rgba(1, 1, 1, 0.045)
        }

        Text {
            id: gutter
            x: 0
            y: textEdit.y
            width: Math.max(34, implicitWidth + 18)
            rightPadding: 10
            horizontalAlignment: Text.AlignRight
            text: {
                const count = Math.max(1, textEdit.lineCount)
                let lines = []
                for (let i = 1; i <= count; ++i) lines.push(i)
                return lines.join("\n")
            }
            color: Qt.rgba(1, 1, 1, 0.28)
            font { family: editor.monoFont; pixelSize: 13 }
            lineHeightMode: Text.FixedHeight
            lineHeight: editor.lineHeight
        }

        TextEdit {
            id: textEdit
            x: gutter.width + 6
            y: 10
            width: Math.max(implicitWidth, scroll.width - x - 12)
            // Fill the visible area so a click below the last line still focuses it.
            height: Math.max(contentHeight, scroll.height - 20)
            text: settings.categoryScript
            onTextChanged: if (text !== settings.categoryScript) settings.categoryScript = text
            textFormat: TextEdit.PlainText
            wrapMode: TextEdit.NoWrap
            selectByMouse: true
            persistentSelection: true
            color: "#E6E6E8"
            selectionColor: Qt.rgba(0.24, 0.47, 0.85, 0.55)
            selectedTextColor: "white"
            cursorDelegate: Rectangle { width: 2; color: "#528BFF" }
            font { family: editor.monoFont; pixelSize: 13 }
            onCursorRectangleChanged: scroll.ensureVisible(cursorRectangle)
            Keys.onPressed: (event) => editor.handleKey(event)
            Component.onCompleted: if (editor.autofocus) forceActiveFocus()
            SyntaxHighlight {
                document: textEdit.textDocument
                language: "javascript"
                palette: "editor"
            }
        }
    }

    // The wheel scrolls the code only. The page gets it only when the code
    // cannot move any further that way; otherwise the Flickable's leftover
    // delta also scrolled the settings page underneath.
    MouseArea {
        anchors.fill: scroll
        acceptedButtons: Qt.NoButton
        cursorShape: Qt.IBeamCursor
        onWheel: (wheel) => {
            const step = editor.lineHeight * 3 / 120
            let dx = wheel.pixelDelta.x !== 0 ? wheel.pixelDelta.x : wheel.angleDelta.x * step
            let dy = wheel.pixelDelta.y !== 0 ? wheel.pixelDelta.y : wheel.angleDelta.y * step
            // Shift + wheel scrolls sideways.
            if ((wheel.modifiers & Qt.ShiftModifier) && dx === 0) { dx = dy; dy = 0 }
            const x = Math.max(0, Math.min(scroll.contentWidth - scroll.width, scroll.contentX - dx))
            const y = Math.max(0, Math.min(scroll.contentHeight - scroll.height, scroll.contentY - dy))
            if (x === scroll.contentX && y === scroll.contentY) {
                wheel.accepted = false
                return
            }
            scroll.contentX = x
            scroll.contentY = y
            wheel.accepted = true
        }
    }

    OverlayScrollBar {
        id: verticalBar
        flickable: scroll
        orientation: Qt.Vertical
        endInset: horizontalBar.needed ? 10 : 0
    }
    OverlayScrollBar {
        id: horizontalBar
        flickable: scroll
        orientation: Qt.Horizontal
        endInset: verticalBar.needed ? 10 : 0
    }

    function handleKey(event) {
        const ctrl = (event.modifiers & Qt.ControlModifier) !== 0
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0
        if (ctrl && (event.key === Qt.Key_Return || event.key === Qt.Key_Enter)) { settings.testScript(); event.accepted = true }
        else if (ctrl && event.key === Qt.Key_Slash) { toggleComment(); event.accepted = true }
        else if (ctrl && shift && event.key === Qt.Key_F) { settings.formatScript(); event.accepted = true }
        else if (event.key === Qt.Key_Backtab || (event.key === Qt.Key_Tab && shift)) { outdent(); event.accepted = true }
        else if (event.key === Qt.Key_Tab && event.modifiers === Qt.NoModifier) {
            if (textEdit.selectionStart !== textEdit.selectionEnd && textEdit.selectedText.indexOf(" ") >= 0) indent()
            else textEdit.insert(textEdit.cursorPosition, "    ")
            event.accepted = true
        }
    }

    // The [start, end) range of whole lines touched by the selection.
    function selectedLines() {
        const text = textEdit.text
        const start = text.lastIndexOf("\n", textEdit.selectionStart - 1) + 1
        const last = textEdit.selectionEnd > textEdit.selectionStart ? textEdit.selectionEnd - 1 : textEdit.selectionEnd
        let end = text.indexOf("\n", Math.max(last, start))
        if (end < 0) end = text.length
        return { start: start, end: end, lines: text.substring(start, end).split("\n") }
    }

    function replaceLines(range, lines) {
        const text = textEdit.text
        const changed = lines.join("\n")
        settings.categoryScript = text.substring(0, range.start) + changed + text.substring(range.end)
        textEdit.select(range.start, range.start + changed.length)
    }

    function indent() {
        const range = selectedLines()
        replaceLines(range, range.lines.map(line => line.length ? "    " + line : line))
    }

    function outdent() {
        const range = selectedLines()
        replaceLines(range, range.lines.map(line => line.replace(/^( {1,4}|\t)/, "")))
    }

    function toggleComment() {
        const range = selectedLines()
        const commented = range.lines.every(line => line.trim() === "" || line.trim().startsWith("//"))
        replaceLines(range, range.lines.map(line => {
            if (line.trim() === "") return line
            if (commented) return line.replace(/^(\s*)\/\/ ?/, "$1")
            const indentText = line.match(/^\s*/)[0]
            return indentText + "// " + line.substring(indentText.length)
        }))
    }
}
