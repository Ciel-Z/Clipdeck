import QtQuick
import QtQuick.Shapes

// Port of DeckNavigationIcon (HistoryPanelView.swift): 24-unit outline paths,
// scaled to the item size while the stroke stays 1.7 px, as in SwiftUI.
Shape {
    id: icon
    property string kind: "history"
    property color color: "white"
    readonly property real s: width / 24
    // Qt 6.5 has no curve renderer; MSAA on the layer gives smooth strokes.
    layer.enabled: true
    layer.samples: 4

    ShapePath {
        strokeColor: icon.color
        strokeWidth: 1.7
        fillColor: "transparent"
        capStyle: ShapePath.RoundCap
        joinStyle: ShapePath.RoundJoin
        PathSvg { path: icon.pathFor(icon.kind, icon.s) }
    }

    function pathFor(kind, s) {
        function p(x, y) { return (x * s).toFixed(2) + " " + (y * s).toFixed(2) }
        switch (kind) {
        case "history":
            // Arc from 200° to 160° (y-down) leaves the gap where the arrow sits.
            return "M " + p(3.543, 8.922) + " A " + (9 * s) + " " + (9 * s) + " 0 1 1 " + p(3.543, 15.078)
                + " M " + p(3, 3) + " L " + p(3, 8) + " L " + p(8, 8)
                + " M " + p(12, 7) + " L " + p(12, 12) + " L " + p(16, 14)
        case "text":
            return "M " + p(4, 7) + " L " + p(4, 4) + " L " + p(20, 4) + " L " + p(20, 7)
                + " M " + p(12, 4) + " L " + p(12, 20) + " M " + p(8, 20) + " L " + p(16, 20)
        case "image":
            return "M " + p(5, 3) + " L " + p(19, 3) + " Q " + p(21, 3) + " " + p(21, 5)
                + " L " + p(21, 19) + " Q " + p(21, 21) + " " + p(19, 21)
                + " L " + p(5, 21) + " Q " + p(3, 21) + " " + p(3, 19)
                + " L " + p(3, 5) + " Q " + p(3, 3) + " " + p(5, 3) + " Z"
                + " M " + p(10, 8.5) + " A " + (1.5 * s) + " " + (1.5 * s) + " 0 1 1 " + p(7, 8.5)
                + " A " + (1.5 * s) + " " + (1.5 * s) + " 0 1 1 " + p(10, 8.5)
                + " M " + p(21, 15) + " L " + p(16, 10) + " L " + p(5, 21)
        case "folder":
            return "M " + p(20, 20) + " L " + p(4, 20) + " Q " + p(2, 20) + " " + p(2, 18)
                + " L " + p(2, 6) + " Q " + p(2, 4) + " " + p(4, 4)
                + " L " + p(9, 4) + " L " + p(11, 7) + " L " + p(20, 7)
                + " Q " + p(22, 7) + " " + p(22, 9) + " L " + p(22, 18)
                + " Q " + p(22, 20) + " " + p(20, 20) + " Z"
        case "search":
            return "M " + p(17, 10.5) + " A " + (6.5 * s) + " " + (6.5 * s) + " 0 1 1 " + p(4, 10.5)
                + " A " + (6.5 * s) + " " + (6.5 * s) + " 0 1 1 " + p(17, 10.5)
                + " M " + p(15.2, 15.2) + " L " + p(20.5, 20.5)
        case "plus":
            return "M " + p(12, 4) + " L " + p(12, 20) + " M " + p(4, 12) + " L " + p(20, 12)
        case "chevron.left":
            return "M " + p(15, 5) + " L " + p(8, 12) + " L " + p(15, 19)
        case "chevron.right":
            return "M " + p(9, 5) + " L " + p(16, 12) + " L " + p(9, 19)
        }
        return ""
    }
}
