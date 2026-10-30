#include "deck_backdrop.h"

#include <QColor>

#include <algorithm>
#include <vector>

namespace DeckBackdrop {

namespace {

// One horizontal + one vertical running-sum pass over ARGB32 pixels.
void boxBlur(QImage &image, int radius) {
    const int width = image.width();
    const int height = image.height();
    if (radius <= 0 || width == 0 || height == 0) return;
    std::vector<QRgb> line(size_t(std::max(width, height)));
    const auto blurLine = [radius](QRgb *pixels, int count, int stride, std::vector<QRgb> &scratch) {
        int r = 0, g = 0, b = 0;
        const int window = radius * 2 + 1;
        const auto at = [&](int index) { return pixels[std::clamp(index, 0, count - 1) * stride]; };
        for (int index = -radius; index <= radius; ++index) {
            const QRgb pixel = at(index);
            r += qRed(pixel); g += qGreen(pixel); b += qBlue(pixel);
        }
        for (int index = 0; index < count; ++index) {
            scratch[size_t(index)] = qRgb(r / window, g / window, b / window);
            const QRgb outgoing = at(index - radius);
            const QRgb incoming = at(index + radius + 1);
            r += qRed(incoming) - qRed(outgoing);
            g += qGreen(incoming) - qGreen(outgoing);
            b += qBlue(incoming) - qBlue(outgoing);
        }
        for (int index = 0; index < count; ++index) pixels[index * stride] = scratch[size_t(index)];
    };
    for (int y = 0; y < height; ++y) blurLine(reinterpret_cast<QRgb *>(image.scanLine(y)), width, 1, line);
    QRgb *first = reinterpret_cast<QRgb *>(image.bits());
    const int stride = int(image.bytesPerLine() / sizeof(QRgb));
    for (int x = 0; x < width; ++x) blurLine(first + x, height, stride, line);
}

void saturate(QImage &image, double factor) {
    for (int y = 0; y < image.height(); ++y) {
        QRgb *pixels = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x) {
            const QRgb pixel = pixels[x];
            const double gray = 0.299 * qRed(pixel) + 0.587 * qGreen(pixel) + 0.114 * qBlue(pixel);
            const auto channel = [&](int value) { return std::clamp(int(gray + (value - gray) * factor), 0, 255); };
            pixels[x] = qRgb(channel(qRed(pixel)), channel(qGreen(pixel)), channel(qBlue(pixel)));
        }
    }
}

} // namespace

QImage frost(const QImage &screen, int downscale, int radius, int passes, double saturation) {
    if (screen.isNull()) return QImage();
    const QSize small(std::max(1, screen.width() / std::max(1, downscale)),
                      std::max(1, screen.height() / std::max(1, downscale)));
    QImage image = screen.convertToFormat(QImage::Format_RGB32)
                       .scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    for (int pass = 0; pass < passes; ++pass) boxBlur(image, radius);
    if (saturation != 1.0) saturate(image, saturation);
    return image;
}

} // namespace DeckBackdrop
