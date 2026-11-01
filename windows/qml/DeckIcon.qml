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
        case "doc":
            return "M " + p(14, 3) + " L " + p(6, 3) + " Q " + p(5, 3) + " " + p(5, 4)
                + " L " + p(5, 20) + " Q " + p(5, 21) + " " + p(6, 21) + " L " + p(18, 21)
                + " Q " + p(19, 21) + " " + p(19, 20) + " L " + p(19, 8) + " Z"
                + " M " + p(14, 3) + " L " + p(14, 8) + " L " + p(19, 8)
        case "clipboard":
            return "M " + p(9, 4) + " L " + p(6, 4) + " Q " + p(5, 4) + " " + p(5, 5)
                + " L " + p(5, 20) + " Q " + p(5, 21) + " " + p(6, 21) + " L " + p(18, 21)
                + " Q " + p(19, 21) + " " + p(19, 20) + " L " + p(19, 5) + " Q " + p(19, 4) + " " + p(18, 4)
                + " L " + p(15, 4)
                + " M " + p(9, 3) + " L " + p(15, 3) + " L " + p(15, 6) + " L " + p(9, 6) + " Z"
        case "sync":
            // Two arcs chasing each other (SF Symbols arrow.triangle.2.circlepath).
            return "M " + p(19, 12) + " A " + (7 * s) + " " + (7 * s) + " 0 0 0 " + p(7, 7)
                + " M " + p(7, 3.5) + " L " + p(7, 7.5) + " L " + p(11, 7.5)
                + " M " + p(5, 12) + " A " + (7 * s) + " " + (7 * s) + " 0 0 0 " + p(17, 17)
                + " M " + p(17, 20.5) + " L " + p(17, 16.5) + " L " + p(13, 16.5)
        case "copy":
            // Two overlapping sheets (SF Symbols doc.on.doc).
            return "M " + p(9, 8) + " L " + p(18, 8) + " Q " + p(20, 8) + " " + p(20, 10)
                + " L " + p(20, 19) + " Q " + p(20, 21) + " " + p(18, 21) + " L " + p(11, 21)
                + " Q " + p(9, 21) + " " + p(9, 19) + " Z"
                + " M " + p(15, 5) + " Q " + p(15, 3) + " " + p(13, 3) + " L " + p(6, 3)
                + " Q " + p(4, 3) + " " + p(4, 5) + " L " + p(4, 14) + " Q " + p(4, 16) + " " + p(6, 16)
        case "safari":
            return "M " + p(21, 12) + " A " + (9 * s) + " " + (9 * s) + " 0 1 1 " + p(3, 12)
                + " A " + (9 * s) + " " + (9 * s) + " 0 1 1 " + p(21, 12)
                + " M " + p(15.5, 8.5) + " L " + p(13.5, 13.5) + " L " + p(8.5, 15.5) + " L " + p(10.5, 10.5) + " Z"
        case "pencil":
            return "M " + p(4, 20) + " L " + p(5, 15.5) + " L " + p(16, 4.5) + " L " + p(19.5, 8)
                + " L " + p(8.5, 19) + " Z M " + p(13.5, 7) + " L " + p(17, 10.5)
        case "cursor":
            return "M " + p(9, 4) + " L " + p(15, 4) + " M " + p(9, 20) + " L " + p(15, 20)
                + " M " + p(12, 4) + " L " + p(12, 20)
        case "eye":
            return "M " + p(2, 12) + " Q " + p(12, 3) + " " + p(22, 12) + " Q " + p(12, 21) + " " + p(2, 12) + " Z"
                + " M " + p(15, 12) + " A " + (3 * s) + " " + (3 * s) + " 0 1 1 " + p(9, 12)
                + " A " + (3 * s) + " " + (3 * s) + " 0 1 1 " + p(15, 12)
        case "trash":
            return "M " + p(4, 6) + " L " + p(20, 6) + " M " + p(9, 6) + " L " + p(9, 4) + " L " + p(15, 4) + " L " + p(15, 6)
                + " M " + p(6, 6) + " L " + p(7, 20) + " L " + p(17, 20) + " L " + p(18, 6)
                + " M " + p(10, 10) + " L " + p(10, 16) + " M " + p(14, 10) + " L " + p(14, 16)
        case "rotate.left":
            return "M " + p(4, 5) + " L " + p(4, 10) + " L " + p(9, 10)
                + " M " + p(4.6, 9.6) + " A " + (8 * s) + " " + (8 * s) + " 0 1 1 " + p(5, 16)
        case "rotate.right":
            return "M " + p(20, 5) + " L " + p(20, 10) + " L " + p(15, 10)
                + " M " + p(19.4, 9.6) + " A " + (8 * s) + " " + (8 * s) + " 0 1 0 " + p(19, 16)
        case "gear":
            return "M " + p(15, 12) + " A " + (3 * s) + " " + (3 * s) + " 0 1 1 " + p(9, 12)
                + " A " + (3 * s) + " " + (3 * s) + " 0 1 1 " + p(15, 12)
                + " M " + p(12, 2.5) + " L " + p(12, 5) + " M " + p(12, 19) + " L " + p(12, 21.5)
                + " M " + p(2.5, 12) + " L " + p(5, 12) + " M " + p(19, 12) + " L " + p(21.5, 12)
                + " M " + p(5.3, 5.3) + " L " + p(7.1, 7.1) + " M " + p(16.9, 16.9) + " L " + p(18.7, 18.7)
                + " M " + p(5.3, 18.7) + " L " + p(7.1, 16.9) + " M " + p(16.9, 7.1) + " L " + p(18.7, 5.3)
                + " M " + p(18.5, 12) + " A " + (6.5 * s) + " " + (6.5 * s) + " 0 1 1 " + p(5.5, 12)
                + " A " + (6.5 * s) + " " + (6.5 * s) + " 0 1 1 " + p(18.5, 12)
        case "keyboard":
            return "M " + p(4, 6) + " L " + p(20, 6) + " Q " + p(22, 6) + " " + p(22, 8) + " L " + p(22, 16)
                + " Q " + p(22, 18) + " " + p(20, 18) + " L " + p(4, 18) + " Q " + p(2, 18) + " " + p(2, 16)
                + " L " + p(2, 8) + " Q " + p(2, 6) + " " + p(4, 6) + " Z"
                + " M " + p(6, 10) + " L " + p(6.1, 10) + " M " + p(10, 10) + " L " + p(10.1, 10)
                + " M " + p(14, 10) + " L " + p(14.1, 10) + " M " + p(18, 10) + " L " + p(18.1, 10)
                + " M " + p(8, 14.5) + " L " + p(16, 14.5)
        case "grid":
            return "M " + p(4, 4) + " L " + p(10, 4) + " L " + p(10, 10) + " L " + p(4, 10) + " Z"
                + " M " + p(14, 4) + " L " + p(20, 4) + " L " + p(20, 10) + " L " + p(14, 10) + " Z"
                + " M " + p(4, 14) + " L " + p(10, 14) + " L " + p(10, 20) + " L " + p(4, 20) + " Z"
                + " M " + p(14, 14) + " L " + p(20, 14) + " L " + p(20, 20) + " L " + p(14, 20) + " Z"
        case "drive":
            return "M " + p(3, 13) + " L " + p(6, 5) + " L " + p(18, 5) + " L " + p(21, 13)
                + " L " + p(21, 18) + " Q " + p(21, 19) + " " + p(20, 19) + " L " + p(4, 19)
                + " Q " + p(3, 19) + " " + p(3, 18) + " Z M " + p(3, 13) + " L " + p(21, 13)
                + " M " + p(16, 16) + " L " + p(16.1, 16)
        case "restore":
            return "M " + p(4, 4) + " L " + p(4, 9) + " L " + p(9, 9)
                + " M " + p(4.6, 8.6) + " A " + (8 * s) + " " + (8 * s) + " 0 1 1 " + p(4.3, 14)
        case "grip":
            return "M " + p(6, 8) + " L " + p(18, 8) + " M " + p(6, 12) + " L " + p(18, 12) + " M " + p(6, 16) + " L " + p(18, 16)
        case "ellipsis":
            return "M " + p(5, 12) + " L " + p(5.1, 12) + " M " + p(12, 12) + " L " + p(12.1, 12) + " M " + p(19, 12) + " L " + p(19.1, 12)
        case "chevron.updown":
            return "M " + p(7, 9) + " L " + p(12, 4.5) + " L " + p(17, 9)
                + " M " + p(7, 15) + " L " + p(12, 19.5) + " L " + p(17, 15)
        case "xmark":
            return "M " + p(7, 7) + " L " + p(17, 17) + " M " + p(17, 7) + " L " + p(7, 17)
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
