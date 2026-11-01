#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QQuickImageProvider>

// image://deck/<key>: card thumbnails, source application icons, link icons
// and link previews. Filled on the GUI thread, read on QML loader threads.
class DeckImageProvider final : public QQuickImageProvider {
public:
    DeckImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;

    void insert(const QString &key, const QImage &image);
    void insertEncoded(const QString &key, const QByteArray &data);
    bool contains(const QString &key) const;
    QImage image(const QString &key) const;
    // Drops entries whose key is not in `keep` (items scrolled out of history).
    void retain(const QSet<QString> &keep);

private:
    mutable QMutex mutex_;
    QHash<QString, QImage> images_;
};
