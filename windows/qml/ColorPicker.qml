import QtQuick

// The custom-colour picker: a saturation/brightness square, a hue strip and a
// hex field, in a popover over the window (no system dialog, like DropDown).
// Every change is reported at once through picked(hex).
Item {
    id: picker
    property color color: "#BF5AF2"
    signal picked(string hex)

    parent: Window.contentItem
    anchors.fill: parent
    visible: false
    z: 1000

    property real hue: 0
    property real saturation: 1
    property real brightness: 1
    property point origin

    function open(anchor) {
        const c = Qt.color(picker.color)
        if (c.hsvHue >= 0) hue = c.hsvHue
        saturation = c.hsvSaturation
        brightness = c.hsvValue
        hexInput.text = hexOf(c)
        const point = anchor.mapToItem(picker, anchor.width, anchor.height + 8)
        origin = Qt.point(point.x - panel.width, point.y)
        visible = true
    }

    function hexOf(c) { return c.toString().toUpperCase() }

    function emitCurrent() {
        const hex = hexOf(Qt.hsva(hue, saturation, brightness, 1))
        if (!hexInput.activeFocus) hexInput.text = hex
        picked(hex)
    }

    MouseArea { anchors.fill: parent; onClicked: picker.visible = false }
    DropShadow { target: panel; blur: 18; offsetY: 4 }

    Rectangle {
        id: panel
        x: Math.max(8, Math.min(picker.origin.x, picker.width - width - 8))
        y: picker.origin.y + height + 8 > picker.height ? picker.origin.y - height - 40 : picker.origin.y
        width: 240
        height: content.implicitHeight + 24
        radius: 10
        color: "#2E2E32"
        border.color: Qt.rgba(1, 1, 1, 0.12)
        // Clicks inside the panel do not close it.
        MouseArea { anchors.fill: parent }

        Column {
            id: content
            x: 12; y: 12
            width: parent.width - 24
            spacing: 12

            // Saturation (x) and brightness (y) for the current hue.
            Rectangle {
                id: field
                width: parent.width
                height: 150
                radius: 6
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: "white" }
                    GradientStop { position: 1; color: Qt.hsva(picker.hue, 1, 1, 1) }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: 6
                    gradient: Gradient {
                        GradientStop { position: 0; color: "transparent" }
                        GradientStop { position: 1; color: "black" }
                    }
                }
                Rectangle {
                    width: 14; height: 14; radius: 7
                    x: picker.saturation * field.width - width / 2
                    y: (1 - picker.brightness) * field.height - height / 2
                    color: Qt.hsva(picker.hue, picker.saturation, picker.brightness, 1)
                    border.width: 2
                    border.color: "white"
                }
                MouseArea {
                    anchors.fill: parent
                    preventStealing: true
                    function pick(mouse) {
                        picker.saturation = Math.max(0, Math.min(1, mouse.x / field.width))
                        picker.brightness = 1 - Math.max(0, Math.min(1, mouse.y / field.height))
                        picker.emitCurrent()
                    }
                    onPressed: (mouse) => pick(mouse)
                    onPositionChanged: (mouse) => pick(mouse)
                }
            }

            Rectangle {
                id: hueStrip
                width: parent.width
                height: 12
                radius: 6
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0 / 6; color: "#FF0000" }
                    GradientStop { position: 1 / 6; color: "#FFFF00" }
                    GradientStop { position: 2 / 6; color: "#00FF00" }
                    GradientStop { position: 3 / 6; color: "#00FFFF" }
                    GradientStop { position: 4 / 6; color: "#0000FF" }
                    GradientStop { position: 5 / 6; color: "#FF00FF" }
                    GradientStop { position: 6 / 6; color: "#FF0000" }
                }
                Rectangle {
                    width: 16; height: 16; radius: 8
                    anchors.verticalCenter: parent.verticalCenter
                    x: picker.hue * hueStrip.width - width / 2
                    color: Qt.hsva(picker.hue, 1, 1, 1)
                    border.width: 2
                    border.color: "white"
                }
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -4
                    preventStealing: true
                    function pick(mouse) {
                        picker.hue = Math.max(0, Math.min(0.999, (mouse.x - 4) / hueStrip.width))
                        picker.emitCurrent()
                    }
                    onPressed: (mouse) => pick(mouse)
                    onPositionChanged: (mouse) => pick(mouse)
                }
            }

            Row {
                spacing: 8
                Rectangle {
                    width: 28; height: 28; radius: 6
                    color: Qt.hsva(picker.hue, picker.saturation, picker.brightness, 1)
                    border.color: Qt.rgba(1, 1, 1, 0.15)
                }
                Rectangle {
                    width: content.width - 36
                    height: 28
                    radius: 6
                    color: Qt.rgba(0, 0, 0, 0.25)
                    border.color: hexInput.activeFocus ? "#0A84FF" : Qt.rgba(1, 1, 1, 0.12)
                    TextInput {
                        id: hexInput
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        verticalAlignment: TextInput.AlignVCenter
                        color: "white"
                        selectionColor: "#0A84FF"
                        selectByMouse: true
                        font { family: "Consolas"; pixelSize: 13 }
                        maximumLength: 7
                        validator: RegularExpressionValidator { regularExpression: /#?[0-9A-Fa-f]{0,6}/ }
                        onTextEdited: {
                            const value = text.startsWith("#") ? text : "#" + text
                            if (!/^#[0-9A-Fa-f]{6}$/.test(value)) return
                            const c = Qt.color(value)
                            if (c.hsvHue >= 0) picker.hue = c.hsvHue
                            picker.saturation = c.hsvSaturation
                            picker.brightness = c.hsvValue
                            picker.picked(value.toUpperCase())
                        }
                        Keys.onReturnPressed: picker.visible = false
                        Keys.onEscapePressed: picker.visible = false
                    }
                }
            }
        }
    }
}
