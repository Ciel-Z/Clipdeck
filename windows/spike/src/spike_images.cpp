#include "spike_images.h"

#include <QLinearGradient>
#include <QPainter>
#include <QRadialGradient>

SpikeImageProvider::SpikeImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage SpikeImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
    QImage image;
    if (id == QLatin1String("checker")) {
        // Base (0.10, 0.10, 0.11) with white 5.5% / black 12% pre-composited.
        image = QImage(24, 24, QImage::Format_RGB32);
        const QColor light(38, 38, 41);
        const QColor dark(22, 22, 25);
        QPainter painter(&image);
        painter.fillRect(0, 0, 12, 12, light);
        painter.fillRect(12, 12, 12, 12, light);
        painter.fillRect(12, 0, 12, 12, dark);
        painter.fillRect(0, 12, 12, 12, dark);
    } else {
        const int seed = id.section(QLatin1Char('/'), 1).toInt();
        const QSize photoSize = (seed % 3 == 0) ? QSize(1200, 1600) : QSize(1920, 1080);
        image = QImage(photoSize, QImage::Format_ARGB32_Premultiplied);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        QLinearGradient sky(0, 0, 0, photoSize.height());
        sky.setColorAt(0, QColor::fromHsv((seed * 37) % 360, 120, 230));
        sky.setColorAt(1, QColor::fromHsv((seed * 37 + 60) % 360, 160, 90));
        painter.fillRect(image.rect(), sky);
        QRadialGradient sun(photoSize.width() * 0.7, photoSize.height() * 0.3, photoSize.width() * 0.25);
        sun.setColorAt(0, QColor(255, 240, 200, 230));
        sun.setColorAt(1, QColor(255, 240, 200, 0));
        painter.fillRect(image.rect(), sun);
        if (seed % 4 == 1) {
            // A transparent cut-out so the checkerboard shows through.
            painter.setCompositionMode(QPainter::CompositionMode_Clear);
            painter.drawEllipse(QPointF(photoSize.width() * 0.3, photoSize.height() * 0.6),
                                photoSize.width() * 0.18, photoSize.width() * 0.18);
        }
    }
    if (size) *size = image.size();
    if (requestedSize.isValid() && !requestedSize.isEmpty() && id != QLatin1String("checker")) {
        image = image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return image;
}
