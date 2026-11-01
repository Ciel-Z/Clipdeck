#include "deck_image_provider.h"

#include <QMutexLocker>
#include <QSet>

QImage DeckImageProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize) {
    // A "?rev" suffix only busts QML's cache; it is not part of the key.
    const QString key = id.section(QLatin1Char('?'), 0, 0);
    QImage image;
    {
        QMutexLocker locker(&mutex_);
        image = images_.value(key);
    }
    if (size) *size = image.size();
    if (!image.isNull() && requestedSize.isValid()
        && (image.width() > requestedSize.width() || image.height() > requestedSize.height())) {
        image = image.scaled(requestedSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return image;
}

void DeckImageProvider::insert(const QString &key, const QImage &image) {
    if (image.isNull()) return;
    QMutexLocker locker(&mutex_);
    images_.insert(key, image);
}

void DeckImageProvider::insertEncoded(const QString &key, const QByteArray &data) {
    if (data.isEmpty() || contains(key)) return;
    QImage image;
    if (image.loadFromData(data)) insert(key, image);
}

QImage DeckImageProvider::image(const QString &key) const {
    QMutexLocker locker(&mutex_);
    return images_.value(key);
}

bool DeckImageProvider::contains(const QString &key) const {
    QMutexLocker locker(&mutex_);
    return images_.contains(key);
}

void DeckImageProvider::retain(const QSet<QString> &keep) {
    QMutexLocker locker(&mutex_);
    for (auto it = images_.begin(); it != images_.end();) {
        if (keep.contains(it.key())) ++it;
        else it = images_.erase(it);
    }
}
