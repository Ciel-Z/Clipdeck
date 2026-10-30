import QtQuick

// KeyboardShortcutRecorder: click, then press the new combination. Esc or
// clicking elsewhere cancels; a lone modifier keeps waiting.
Rectangle {
    id: recorder
    property string name: ""       // showDeck | nextCategory | previousCategory
    property string shortcut: ""   // current text, e.g. "Alt+Shift+V"
    readonly property bool recording: activeFocus

    width: 190
    height: 34
    radius: 8
    color: recording ? Qt.rgba(0.04, 0.52, 1, 0.16) : Qt.rgba(1, 1, 1, 0.08)
    border.color: recording ? "#0A84FF" : Qt.rgba(1, 1, 1, 0.10)
    border.width: recording ? 1.5 : 1

    onRecordingChanged: settings.setRecording(recording)

    Text {
        anchors.centerIn: parent
        text: recorder.recording ? "按下快捷键…" : recorder.shortcut
        color: recorder.recording ? Qt.rgba(1, 1, 1, 0.7) : "white"
        font { family: "Segoe UI"; pixelSize: 14; weight: Font.DemiBold }
    }
    MouseArea {
        anchors.fill: parent
        onClicked: recorder.forceActiveFocus()
    }
    Keys.onPressed: (event) => {
        event.accepted = true
        if (event.key === Qt.Key_Escape) { recorder.focus = false; return }
        if ([Qt.Key_Control, Qt.Key_Shift, Qt.Key_Alt, Qt.Key_Meta].indexOf(event.key) >= 0) return
        if (settings.recordShortcut(recorder.name, event.key, event.modifiers)) recorder.focus = false
    }
}
