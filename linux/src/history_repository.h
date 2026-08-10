#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QSqlDatabase>
#include <QString>
#include <QVector>

enum class ClipboardPayloadType {
    Text,
    Image,
};

struct ClipboardItem {
    qint64 databaseID = -1;
    QString title;
    ClipboardPayloadType type = ClipboardPayloadType::Text;
    QString text;
    QString richTextHtml;
    QByteArray imageData;
    QImage image;
    QString sourceID;
    QString sourceName;
    QByteArray sourceIconData;
    QImage sourceIcon;
    QString category;
    QDateTime copiedAt;
    int copyCount = 1;

    bool isText() const { return type == ClipboardPayloadType::Text; }
    bool isImage() const { return type == ClipboardPayloadType::Image; }
};

class HistoryRepository final {
public:
    HistoryRepository();
    ~HistoryRepository();

    bool isOpen() const;
    QString lastError() const;
    ClipboardItem save(const ClipboardItem &item);
    QVector<ClipboardItem> fetch(const QString &category = QString(),
                                 const QString &search = QString(),
                                 int limit = 300) const;
    bool updateTitle(qint64 id, const QString &title);
    bool updateText(qint64 id, const QString &text, const QString &category);
    bool updateImage(qint64 id, const QByteArray &pngData, const QImage &image);
    bool remove(qint64 id);
    int clearExpired(int retentionDays);
    int clearAll();
    qint64 databaseByteCount() const;

    static QString classifyText(const QString &text);
    static QStringList presetCategories();

private:
    bool initialize();
    static QByteArray contentHash(const ClipboardItem &item);
    static ClipboardItem itemFromQuery(const class QSqlQuery &query);
    bool execute(const QString &sql) const;

    QString connectionName_;
    QString databasePath_;
    mutable QString lastError_;
    QSqlDatabase database_;
};
