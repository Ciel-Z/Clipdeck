import QtQuick

// retentionSlider: 1–6 天 · 1–3 周 · 1–11 个月 · 1 年 · 永久 on a 0…1 track
// with major ticks at 天/周/月/年/永久. Dragging shows a draft value; the
// change is requested on release (shortening asks for confirmation).
Item {
    id: slider
    property int days: 30
    property int draftDays: -1
    signal changeRequested(int days)
    height: 57 + 23

    readonly property var options: {
        const list = []
        for (let day = 1; day <= 6; ++day)
            list.push({ days: day, title: day + "天", position: (day - 1) / 6 * 0.25, major: day === 1 })
        list.push({ days: 7, title: "1周", position: 0.25, major: true })
        for (let week = 2; week <= 3; ++week)
            list.push({ days: week * 7, title: week + "周", position: 0.25 + (week - 1) / 3 * 0.25, major: false })
        list.push({ days: 30, title: "1个月", position: 0.5, major: true })
        for (let month = 2; month <= 11; ++month)
            list.push({ days: month * 30, title: month + "个月", position: 0.5 + (month - 1) / 11 * 0.25, major: false })
        list.push({ days: 365, title: "1年", position: 0.75, major: true })
        list.push({ days: 0, title: "永久", position: 1, major: true })
        return list
    }

    function indexFor(value) {
        if (value === 0) return options.length - 1
        let best = 0
        for (let index = 0; index < options.length - 1; ++index)
            if (Math.abs(options[index].days - value) < Math.abs(options[best].days - value)) best = index
        return best
    }
    function title(value) {
        for (const option of options) if (option.days === value) return option.title
        return value + "天"
    }

    readonly property int shownIndex: indexFor(draftDays >= 0 ? draftDays : days)
    readonly property real usable: Math.max(0, width - 20)

    Text {
        text: slider.options[slider.shownIndex].title
        color: "white"
        font { family: "Segoe UI"; pixelSize: 13; weight: Font.DemiBold }
        x: Math.min(Math.max(10 + slider.usable * slider.options[slider.shownIndex].position, 28), slider.width - 28) - width / 2
        y: 0
    }

    // Track
    Rectangle {
        id: track
        x: 10
        y: 32
        width: slider.usable
        height: 4
        radius: 2
        color: Qt.rgba(1, 1, 1, 0.16)
        Rectangle {
            width: slider.usable * slider.options[slider.shownIndex].position
            height: parent.height
            radius: 2
            color: "#0A84FF"
        }
    }
    Repeater {
        model: slider.options
        delegate: Rectangle {
            required property var modelData
            width: modelData.major ? 3.5 : 2
            height: width
            radius: width / 2
            color: Qt.rgba(1, 1, 1, modelData.major ? 0.45 : 0.22)
            x: 10 + slider.usable * modelData.position - width / 2
            y: 51 - height / 2
        }
    }
    Rectangle {
        id: thumb
        width: 20; height: 20; radius: 10
        color: "white"
        border.color: Qt.rgba(0, 0, 0, 0.2)
        x: 10 + slider.usable * slider.options[slider.shownIndex].position - width / 2
        y: track.y + track.height / 2 - height / 2
    }
    MouseArea {
        anchors.left: parent.left
        anchors.right: parent.right
        y: 18
        height: 34
        function pick(mouseX) {
            const value = Math.max(0, Math.min(1, (mouseX - 10) / Math.max(1, slider.usable)))
            let best = 0
            for (let index = 1; index < slider.options.length; ++index)
                if (Math.abs(slider.options[index].position - value) < Math.abs(slider.options[best].position - value)) best = index
            slider.draftDays = slider.options[best].days
        }
        onPressed: (mouse) => pick(mouse.x)
        onPositionChanged: (mouse) => { if (pressed) pick(mouse.x) }
        onReleased: {
            const chosen = slider.draftDays
            slider.draftDays = -1
            if (chosen >= 0 && chosen !== slider.days) slider.changeRequested(chosen)
        }
    }

    // Major labels: 天 · 周 · 月 · 年 · 永久
    Repeater {
        model: [{ title: "天", position: 0 }, { title: "周", position: 0.25 }, { title: "月", position: 0.5 },
                { title: "年", position: 0.75 }, { title: "永久", position: 1 }]
        delegate: Text {
            required property var modelData
            text: modelData.title
            color: Qt.rgba(1, 1, 1, 0.55)
            font { family: "Segoe UI"; pixelSize: 13 }
            x: 10 + slider.usable * modelData.position - width / 2
            y: 62
        }
    }
}
