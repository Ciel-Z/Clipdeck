#include "linux_icons.h"
#include "display_scale.h"

#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>

#include <algorithm>

const QImage &shadowFreeClipDeckIcon() {
    static const QImage icon = [] {
        QImage image(256, 256, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath tile;
        tile.addRoundedRect(QRectF(22, 22, 212, 212), 52, 52);
        QLinearGradient gradient(22, 22, 234, 234);
        gradient.setColorAt(0, QColor(66, 190, 255));
        gradient.setColorAt(1, QColor(18, 91, 220));
        painter.fillPath(tile, gradient);
        painter.setPen(QPen(QColor(7, 36, 83), 15, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        painter.drawPolyline(QPolygonF({QPointF(100, 86), QPointF(62, 128), QPointF(100, 170)}));
        painter.drawPolyline(QPolygonF({QPointF(156, 86), QPointF(194, 128), QPointF(156, 170)}));
        painter.drawLine(QPointF(144, 69), QPointF(112, 187));
        return image;
    }();
    return icon;
}

QIcon transparentTrayIcon(qreal dpr) {
    // UOS displays tray artwork literally, so draw only a monochrome glyph.
    dpr = std::max<qreal>(1.0, dpr);
    QPixmap pixmap(DisplayScale::rasterSize(QSize(22, 22), dpr));
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(dpr);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(235, 235, 235), 2.1, Qt::SolidLine,
                        Qt::RoundCap, Qt::RoundJoin));
    painter.drawPolyline(QPolygonF({QPointF(8.5, 6), QPointF(4.5, 11), QPointF(8.5, 16)}));
    painter.drawPolyline(QPolygonF({QPointF(13.5, 6), QPointF(17.5, 11), QPointF(13.5, 16)}));
    painter.drawLine(QPointF(12.5, 4.5), QPointF(9.5, 17.5));
    painter.end();
    return QIcon(pixmap);
}
