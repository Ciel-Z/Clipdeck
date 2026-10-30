#pragma once

#include <QQuickImageProvider>

// "image://spike/checker" — one 24x24 tile of the 12 px card checkerboard.
// "image://spike/photo/<n>" — synthetic photos so image cards pay a real
// decode-and-scale cost in the scrolling benchmark.
class SpikeImageProvider final : public QQuickImageProvider {
public:
    SpikeImageProvider();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
