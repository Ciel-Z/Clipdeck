#pragma once
#include "history/file_reference.h"

#include <QByteArray>
#include <QDateTime>
#include <QImage>
#include <QSize>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVector>
#include <functional>

enum class ClipboardPayloadType {
    Text,
    Image,
    Files,
};

struct ClipboardItem {
    // In-memory identity (mac ClipboardItem.id). Survives save() so a transient
    // capture can be reconciled with its persisted row.
    QUuid id = QUuid::createUuid();
    qint64 databaseID = -1;
    QString title;
    ClipboardPayloadType type = ClipboardPayloadType::Text;
    QString text;
    QString richTextHtml;
    QByteArray linkIconData;
    QByteArray linkPreviewData;
    bool hasFetchedLinkPreview = false;
    QByteArray imageData;
    QImage image;
    QVector<ClipboardFileReference> files;
    QSize imagePixelSize;
    bool payloadComplete = true;
    qint64 textLength = 0;
    int imageAngle = 0;
    QString sourceID;
    QString sourceName;
    QByteArray sourceIconData;
    QByteArray sourceIconVariantsData;
    QImage sourceIcon;
    QString category;
    QString categoryIcon;
    QString previewLanguage;
    QDateTime copiedAt;
    int copyCount = 1;

    bool isText() const { return type == ClipboardPayloadType::Text; }
    bool isImage() const { return type == ClipboardPayloadType::Image; }
    bool isFiles() const { return type == ClipboardPayloadType::Files; }
};

struct ClipboardCategory {
    qint64 databaseID = -1;
    QString name;
    QString icon;
    QString script;
    QString previewLanguage;
    int sortOrder = 0;
};

class HistoryRepository final {
public:
    using ItemTransform = std::function<void(ClipboardItem &)>;
    explicit HistoryRepository(const QString &databasePath = QString());
    ~HistoryRepository();

    bool isOpen() const;
    QString lastError() const;
    QString databasePath() const { return databasePath_; }
    ClipboardItem save(const ClipboardItem &item);
    QVector<ClipboardItem> fetch(const QString &category = QString(),
                                 const QString &search = QString(),
                                 int limit = 300,
                                 int offset = 0,
                                 qint64 itemID = -1,
                                 bool decodeImages = true,
                                 const ItemTransform &prepare = {}) const;
    bool updateTitle(qint64 id, const QString &title);
    bool updateText(qint64 id, const QString &text, const QString &category);
    bool updateImage(qint64 id, const QByteArray &pngData, const QImage &image);
    bool updateImageAngle(qint64 id, int angle);
    bool updateLinkMetadata(qint64 id, const QString &title, const QByteArray &iconData,
                            const QByteArray &previewData);
    bool remove(qint64 id);
    int clearExpired(int retentionDays);
    int clearExpiredAndReclaim(int retentionDays);
    int clearAll();
    qint64 databaseByteCount() const;

    QVector<ClipboardCategory> categories() const;
    bool createCategory(const QString &name, const QString &icon, const QString &script,
                        const QString &previewLanguage);
    bool updateCategory(qint64 id, const QString &name, const QString &icon,
                        const QString &script, const QString &previewLanguage);
    bool removeCategory(qint64 id);
    bool reorderCategories(const QVector<qint64> &ids);
    bool restoreDefaultCategories();
    QString classifyText(const QString &text, const QString &sourceID = QString(),
                         const QString &sourceName = QString(),
                         const QDateTime &copiedAt = QDateTime()) const;
    QString categoryScriptError(const QString &script) const;
    bool categoryScriptMatches(const QString &script, const QString &text,
                               const QString &previewLanguage,
                               QString *error = nullptr,
                               QStringList *consoleOutput = nullptr) const;
    static QString detectedCodeLanguage(const QString &text, int *score = nullptr);

    static QStringList presetCategories();
    // A category's colour is stored as "#RRGGBB" in its icon field. Older rows
    // still hold a glyph there; they take one of the six defaults by position,
    // which is how the Deck has always coloured them.
    static QStringList categoryColorPalette();
    static QString categoryColor(const QString &icon, int position);
    // How a category's cards are shown. Text cards come in four types: plain
    // text, code ("code": the language is detected), link ("url") and colour
    // ("color"), stored in preview_language. Rows written before this may name
    // a language ("json", "sql", …); they count as code and keep it.
    struct DisplayChoice {
        QString value;
        QString label;
        QString detail;
    };
    static QVector<DisplayChoice> cardTypeChoices();
    // "text", "code", "url" or "color" for a stored preview_language value.
    static QString cardTypeOf(const QString &previewLanguage);
    // The preview_language to store when the card type changes; a category
    // that already is code keeps its value.
    static QString previewLanguageForCardType(const QString &cardType, const QString &current);

private:
    bool initialize();
    bool collapseRecentDuplicates();
    static QByteArray contentHash(const ClipboardItem &item);
    static ClipboardItem itemFromQuery(const class QSqlQuery &query, bool decodeImages = true);
    bool execute(const QString &sql) const;
    static QString classifyBuiltinText(const QString &text);

    QString connectionName_;
    QString databasePath_;
    mutable QString lastError_;
    QSqlDatabase database_;
    bool ready_ = false;
};
