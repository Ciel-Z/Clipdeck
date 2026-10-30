#pragma once
#include "../history/file_reference.h"
#include <QColor>
#include <QRectF>
class QPainter;
enum class DeckSymbol { History, Text, Image, Folder, Document, CodeDocument, ImageDocument,
                        Checkmark, Warning, WarningCircle };
void drawDeckSymbol(QPainter &painter, const QRectF &rect, DeckSymbol symbol, const QColor &color);
DeckSymbol fileSymbol(const ClipboardFileReference &file);
