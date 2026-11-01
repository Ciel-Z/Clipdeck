#pragma once

#include "history_repository.h"

#include <QAbstractListModel>
#include <QDateTime>

class DeckImageProvider;

// HistoryStore.displayedItems as a QML list model. Each row carries what
// ClipboardCardView derives from its item (title, kind, relative time …).
class DeckItemModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        KindRole,
        TitleRole,
        CategoryLabelRole,
        TimeRole,
        TextRole,
        CharacterCountRole,
        MonospacedRole,
        ImageUrlRole,
        ImageSizeTextRole,
        ImageAngleRole,
        FilesRole,
        SourceIconUrlRole,
        SourceNameRole,
        LinkTitleRole,
        LinkHostRole,
        LinkIconUrlRole,
        LinkPreviewUrlRole,
        ColorRole,
        ColorTextRole,
        ImmersiveRole,
        HighlightedTextRole,
        FromOtherDeviceRole,
    };

    explicit DeckItemModel(DeckImageProvider *images, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Keeps rows in place when only their content changed, so the strip
    // does not jump; otherwise resets.
    void setItems(const QVector<ClipboardItem> &items);
    const QVector<ClipboardItem> &items() const { return items_; }
    // This device's LAN sync ID: items whose origin is another device get the sync badge.
    void setLocalDeviceId(const QString &deviceId) { localDeviceId_ = deviceId; }
    int indexOf(const QUuid &id) const;
    // Relative times ("3 分钟") age while the Deck is open.
    void refreshTimes();

private:
    void registerImages(const ClipboardItem &item);

    DeckImageProvider *images_;
    QVector<ClipboardItem> items_;
    QString localDeviceId_;
    QDateTime now_ = QDateTime::currentDateTime();
    // Highlighted card HTML by item id + text hash (mac SyntaxHighlighter cache).
    mutable QHash<QString, QString> highlighted_;
};
