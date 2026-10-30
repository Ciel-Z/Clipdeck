import QtQuick

// ImageCheckerboard: 12 px tiles, white 5.5 % / black 12 % on (0.10, 0.10, 0.11).
Canvas {
    onWidthChanged: requestPaint()
    onHeightChanged: requestPaint()
    onPaint: {
        const ctx = getContext("2d")
        ctx.fillStyle = Qt.rgba(0.10, 0.10, 0.11, 1)
        ctx.fillRect(0, 0, width, height)
        for (let y = 0; y < height; y += 12) {
            for (let x = 0; x < width; x += 12) {
                ctx.fillStyle = ((x / 12) + (y / 12)) % 2 === 0 ? Qt.rgba(1, 1, 1, 0.055) : Qt.rgba(0, 0, 0, 0.12)
                ctx.fillRect(x, y, 12, 12)
            }
        }
    }
}
