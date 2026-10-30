#include "deck_symbols.h"
#include <QFileInfo>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>

void drawDeckSymbol(QPainter &painter, const QRectF &rect, DeckSymbol symbol, const QColor &color) {
    // Preserve the macOS DeckNavigationIcon / FileReferenceTypeIcon 24-point
    // path geometry; no platform font glyphs or native theme icons.
    QPainterPath path;
    switch (symbol) {
    case DeckSymbol::History:
        path.arcMoveTo(QRectF(3, 3, 18, 18), -200);
        path.arcTo(QRectF(3, 3, 18, 18), -200, -320);
        path.moveTo(3, 3); path.lineTo(3, 8); path.lineTo(8, 8);
        path.moveTo(12, 7); path.lineTo(12, 12); path.lineTo(16, 14);
        break;
    case DeckSymbol::Text:
        path.moveTo(4, 7); path.lineTo(4, 4); path.lineTo(20, 4); path.lineTo(20, 7);
        path.moveTo(12, 4); path.lineTo(12, 20); path.moveTo(8, 20); path.lineTo(16, 20);
        break;
    case DeckSymbol::Image:
        path.addRoundedRect(QRectF(3, 3, 18, 18), 2, 2); path.addEllipse(QRectF(7, 7, 3, 3));
        path.moveTo(21, 15); path.lineTo(16, 10); path.lineTo(5, 21);
        break;
    case DeckSymbol::Folder:
        path.moveTo(20, 20); path.lineTo(4, 20); path.quadTo(2, 20, 2, 18);
        path.lineTo(2, 6); path.quadTo(2, 4, 4, 4); path.lineTo(9, 4);
        path.lineTo(11, 7); path.lineTo(20, 7); path.quadTo(22, 7, 22, 9);
        path.lineTo(22, 18); path.quadTo(22, 20, 20, 20); path.closeSubpath();
        break;
    case DeckSymbol::Checkmark:
        path.moveTo(5,12);path.lineTo(10,17);path.lineTo(20,5);break;
    case DeckSymbol::Warning:
        path.moveTo(10.5,3.5);path.quadTo(12,1,13.5,3.5);path.lineTo(22,19);
        path.quadTo(23,21,20.5,21);path.lineTo(3.5,21);path.quadTo(1,21,2,19);path.closeSubpath();
        path.moveTo(12,8);path.lineTo(12,13);path.addEllipse(QRectF(11.5,16.5,1,1));break;
    case DeckSymbol::WarningCircle:
        path.addEllipse(QRectF(3,3,18,18));path.moveTo(12,7);path.lineTo(12,13);
        path.addEllipse(QRectF(11.5,16,1,1));break;
    default:
        path.moveTo(14, 2); path.lineTo(6, 2); path.quadTo(4, 2, 4, 4);
        path.lineTo(4, 20); path.quadTo(4, 22, 6, 22); path.lineTo(18, 22);
        path.quadTo(20, 22, 20, 20); path.lineTo(20, 8); path.closeSubpath();
        path.moveTo(14, 2); path.lineTo(14, 8); path.lineTo(20, 8);
        if (symbol == DeckSymbol::ImageDocument) {
            path.addEllipse(QRectF(7, 11, 2, 2));
            path.moveTo(6, 20); path.lineTo(13, 14); path.lineTo(19, 19);
        } else if (symbol == DeckSymbol::CodeDocument) {
            path.moveTo(10, 12); path.lineTo(7, 15); path.lineTo(10, 18);
            path.moveTo(14, 12); path.lineTo(17, 15); path.lineTo(14, 18);
        } else {
            path.moveTo(8, 12); path.lineTo(16, 12); path.moveTo(8, 16); path.lineTo(16, 16);
        }
    }
    QTransform transform;
    transform.translate(rect.x(), rect.y()); transform.scale(rect.width() / 24, rect.height() / 24);
    painter.save(); painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush); painter.setPen(QPen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(transform.map(path)); painter.restore();
}

DeckSymbol fileSymbol(const ClipboardFileReference &file) {
    if (file.isDirectory) return DeckSymbol::Folder;
    if (file.mimeType.startsWith(QStringLiteral("image/"))) return DeckSymbol::ImageDocument;
    const QString suffix = QFileInfo(file.name).suffix().toLower();
    if (QStringList{"py", "js", "ts", "swift", "java", "sh", "cpp", "c", "go", "rs"}.contains(suffix))
        return DeckSymbol::CodeDocument;
    return DeckSymbol::Document;
}
