import QtQuick
import QtQuick.Effects

// A shadow drawn behind `target` as a separate layer, so text in the target
// is rendered directly instead of being resampled through an effect.
Rectangle {
    id: shadow
    property Item target
    property real blur: 20
    property real offsetY: 5
    property real strength: 0.45
    x: target ? target.x : 0
    y: target ? target.y : 0
    width: target ? target.width : 0
    height: target ? target.height : 0
    radius: target && target.radius !== undefined ? target.radius : 0
    color: "black"
    visible: target ? target.visible : false
    z: target ? target.z - 1 : 0
    layer.enabled: true
    layer.effect: MultiEffect {
        shadowEnabled: true
        shadowColor: Qt.rgba(0, 0, 0, shadow.strength)
        blurMax: shadow.blur
        shadowBlur: 1.0
        shadowVerticalOffset: shadow.offsetY
    }
}
