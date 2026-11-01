#pragma once

#include <QImage>
#include <QTransform>

#include <algorithm>
#include <cmath>

// What sits under the floating header of an image card, so the header can
// adapt instead of always laying a dark gradient over the image:
//   Light - a calm, bright top (white page, light screenshot): dark header text.
//   Dark  - a calm, dark top, or the dimmed backdrop of a fitted wide image:
//           white text with its usual soft shadow.
//   Mixed - a busy top (photos, colourful screenshots): white text over a short,
//           faint gradient.
// Cards are square. The layout follows the card image rule (CardImage.qml):
// within 1.34x of square the image fills the card, otherwise it is fitted over
// a dimmed copy of itself, so the header then sits on that dark backdrop.
// Shared by the Windows model and the Linux painter; mac mirrors it in Swift.
namespace HeaderTone {

enum Tone { Dark, Light, Mixed };

inline Tone forImage(const QImage &source, int angle) {
    if (source.isNull()) return Dark;
    QImage small = source.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation)
                       .convertToFormat(QImage::Format_ARGB32);
    if (angle % 360 != 0) small = small.transformed(QTransform().rotate(angle));
    const double aspect = small.width() / double(std::max(1, small.height()));
    if (std::max(aspect, 1 / aspect) > 1.34) return Dark;

    // The top quarter of the square the card shows.
    const int side = std::min(small.width(), small.height());
    const int left = (small.width() - side) / 2;
    const int top = (small.height() - side) / 2;
    const int bandHeight = std::max(1, int(std::lround(side * 0.24)));
    double sum = 0, sumSquares = 0;
    int count = 0;
    for (int y = top; y < top + bandHeight; ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(small.constScanLine(y));
        for (int x = left; x < left + side; ++x) {
            const QRgb pixel = row[x];
            // Transparent parts show the dark checkerboard.
            const double alpha = qAlpha(pixel) / 255.0;
            const double luminance = (0.2126 * qRed(pixel) + 0.7152 * qGreen(pixel) + 0.0722 * qBlue(pixel)) / 255.0;
            const double shown = luminance * alpha + 0.11 * (1 - alpha);
            sum += shown;
            sumSquares += shown * shown;
            ++count;
        }
    }
    if (count == 0) return Dark;
    const double mean = sum / count;
    const double spread = std::sqrt(std::max(0.0, sumSquares / count - mean * mean));
    if (mean > 0.68 && spread < 0.20) return Light;
    if (mean < 0.45 && spread < 0.22) return Dark;
    return Mixed;
}

} // namespace HeaderTone
