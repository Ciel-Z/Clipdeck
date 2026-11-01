import QtQuick
import QtQuick.Shapes

// LAN sync in progress (docs/lan-sync-design.md §8): a grey arc whose head
// breaks off into a short dash and joins it again while the whole mark turns.
// Same geometry as SyncLoaderView (mac) and the Linux Deck painter: 24-unit
// grid, radius 7, stroke 2.6, main arc 155–245°, dash 22°, 1.8 s breathing,
// 1.1 s per turn.
Item {
    id: loader
    property bool running: false
    property real phase: 0 // 0…1 over one breathing cycle

    readonly property real unit: width / 24
    readonly property real breath: (1 - Math.cos(phase * 2 * Math.PI)) / 2 // 0 → 1 → 0
    readonly property real mainSweep: 245 - 90 * breath
    readonly property real gap: (245 - mainSweep) * 0.6

    width: 22
    height: 22
    opacity: running ? 1 : 0
    visible: opacity > 0
    Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

    NumberAnimation on phase {
        running: loader.visible
        from: 0; to: 1; duration: 1800; loops: Animation.Infinite
    }

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        RotationAnimator on rotation {
            running: loader.visible
            from: 0; to: 360; duration: 1100; loops: Animation.Infinite
        }
        ShapePath {
            fillColor: "transparent"
            strokeColor: "#D3D1C7"
            strokeWidth: 2.6 * loader.unit
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: 12 * loader.unit; centerY: 12 * loader.unit
                radiusX: 7 * loader.unit; radiusY: 7 * loader.unit
                startAngle: -90
                sweepAngle: loader.mainSweep
            }
        }
        ShapePath {
            fillColor: "transparent"
            strokeColor: "#888780"
            strokeWidth: 2.6 * loader.unit
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: 12 * loader.unit; centerY: 12 * loader.unit
                radiusX: 7 * loader.unit; radiusY: 7 * loader.unit
                startAngle: -90 + loader.mainSweep + loader.gap
                sweepAngle: 22
            }
        }
    }
}
