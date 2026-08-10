#include "history_repository.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

namespace {

qint64 milliseconds(const QDateTime &dateTime) {
    return dateTime.toMSecsSinceEpoch();
}

QString escapedLike(QString value) {
    value.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    value.replace(QStringLiteral("%"), QStringLiteral("\\%"));
    value.replace(QStringLiteral("_"), QStringLiteral("\\_"));
    return value;
}

} // namespace

HistoryRepository::HistoryRepository()
    : connectionName_(QStringLiteral("clipdeck-%1").arg(QUuid::createUuid().toString())) {
    const QString dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDirectory);
    databasePath_ = QDir(dataDirectory).filePath(QStringLiteral("history.sqlite"));
    database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    database_.setDatabaseName(databasePath_);
    if (!database_.open()) {
        lastError_ = database_.lastError().text();
        return;
    }
    initialize();
}

HistoryRepository::~HistoryRepository() {
    if (database_.isOpen()) {
        database_.close();
    }
    database_ = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName_);
}

bool HistoryRepository::isOpen() const {
    return database_.isOpen();
}

QString HistoryRepository::lastError() const {
    return lastError_;
}

bool HistoryRepository::initialize() {
    if (!execute(QStringLiteral("PRAGMA foreign_keys = ON"))
        || !execute(QStringLiteral("PRAGMA journal_mode = WAL"))
        || !execute(QStringLiteral("PRAGMA synchronous = NORMAL"))) {
        return false;
    }
    return execute(QStringLiteral(
        "CREATE TABLE IF NOT EXISTS content ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "title TEXT NOT NULL,"
        "content_type TEXT NOT NULL,"
        "plain_text TEXT,"
        "rich_text_html TEXT,"
        "image_data BLOB,"
        "image_width INTEGER,"
        "image_height INTEGER,"
        "content_hash BLOB NOT NULL UNIQUE,"
        "source_id TEXT,"
        "source_name TEXT NOT NULL,"
        "source_icon BLOB,"
        "category TEXT,"
        "copied_at INTEGER NOT NULL,"
        "last_copied_at INTEGER NOT NULL,"
        "copy_count INTEGER NOT NULL DEFAULT 1,"
        "updated_at INTEGER NOT NULL)"))
        && execute(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_content_last_copied "
            "ON content(last_copied_at DESC)"))
        && execute(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_content_category "
            "ON content(category)"))
        && [this] {
            QSqlQuery columns(database_);
            if (!columns.exec(QStringLiteral("PRAGMA table_info(content)"))) return false;
            QSet<QString> names;
            while (columns.next()) names.insert(columns.value(1).toString());
            if (!names.contains(QStringLiteral("rich_text_html"))
                && !execute(QStringLiteral("ALTER TABLE content ADD COLUMN rich_text_html TEXT"))) {
                return false;
            }
            if (!names.contains(QStringLiteral("source_icon"))
                && !execute(QStringLiteral("ALTER TABLE content ADD COLUMN source_icon BLOB"))) {
                return false;
            }
            return true;
        }();
}

ClipboardItem HistoryRepository::save(const ClipboardItem &input) {
    ClipboardItem item = input;
    if (!database_.isOpen()) {
        return item;
    }
    const QByteArray hash = contentHash(item);
    const qint64 now = milliseconds(item.copiedAt.isValid() ? item.copiedAt : QDateTime::currentDateTime());

    QSqlQuery existing(database_);
    existing.prepare(QStringLiteral("SELECT id, copy_count, copied_at FROM content WHERE content_hash = ?"));
    existing.addBindValue(hash);
    if (existing.exec() && existing.next()) {
        item.databaseID = existing.value(0).toLongLong();
        item.copyCount = existing.value(1).toInt() + 1;
        QSqlQuery update(database_);
        update.prepare(QStringLiteral(
            "UPDATE content SET title=?, rich_text_html=?, source_id=?, source_name=?, source_icon=?, category=?, "
            "last_copied_at=?, copy_count=?, updated_at=? WHERE id=?"));
        update.addBindValue(item.title);
        update.addBindValue(item.richTextHtml);
        update.addBindValue(item.sourceID);
        update.addBindValue(item.sourceName);
        update.addBindValue(item.sourceIconData);
        update.addBindValue(item.category);
        update.addBindValue(now);
        update.addBindValue(item.copyCount);
        update.addBindValue(now);
        update.addBindValue(item.databaseID);
        if (!update.exec()) {
            lastError_ = update.lastError().text();
        }
        item.copiedAt = QDateTime::fromMSecsSinceEpoch(now);
        return item;
    }

    QSqlQuery insert(database_);
    insert.prepare(QStringLiteral(
        "INSERT INTO content(title,content_type,plain_text,rich_text_html,image_data,image_width,image_height,"
        "content_hash,source_id,source_name,source_icon,category,copied_at,last_copied_at,copy_count,updated_at) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    insert.addBindValue(item.title.isEmpty()
        ? (item.isImage() ? QStringLiteral("图片") : QStringLiteral("文本"))
        : item.title);
    insert.addBindValue(item.isImage() ? QStringLiteral("image") : QStringLiteral("text"));
    insert.addBindValue(item.isText() ? item.text : QVariant());
    insert.addBindValue(item.isText() ? item.richTextHtml : QVariant());
    insert.addBindValue(item.isImage() ? item.imageData : QVariant());
    insert.addBindValue(item.isImage() ? item.image.width() : QVariant());
    insert.addBindValue(item.isImage() ? item.image.height() : QVariant());
    insert.addBindValue(hash);
    insert.addBindValue(item.sourceID);
    insert.addBindValue(item.sourceName.isEmpty() ? QStringLiteral("Unknown") : item.sourceName);
    insert.addBindValue(item.sourceIconData);
    insert.addBindValue(item.category);
    insert.addBindValue(now);
    insert.addBindValue(now);
    insert.addBindValue(1);
    insert.addBindValue(now);
    if (!insert.exec()) {
        lastError_ = insert.lastError().text();
        return item;
    }
    item.databaseID = insert.lastInsertId().toLongLong();
    item.copiedAt = QDateTime::fromMSecsSinceEpoch(now);
    item.copyCount = 1;
    return item;
}

QVector<ClipboardItem> HistoryRepository::fetch(
    const QString &category,
    const QString &search,
    int limit) const {
    QVector<ClipboardItem> result;
    if (!database_.isOpen()) {
        return result;
    }

    QString sql = QStringLiteral(
        "SELECT id,title,content_type,plain_text,rich_text_html,image_data,image_width,image_height,"
        "source_id,source_name,source_icon,category,last_copied_at,copy_count FROM content WHERE 1=1");
    if (!category.isEmpty()) {
        sql += category == QStringLiteral("文本")
            ? QStringLiteral(" AND content_type='text'")
            : category == QStringLiteral("图片")
                ? QStringLiteral(" AND content_type='image'")
                : QStringLiteral(" AND category=?");
    }
    if (!search.trimmed().isEmpty()) {
        sql += QStringLiteral(" AND (title LIKE ? ESCAPE '\\' OR plain_text LIKE ? ESCAPE '\\')");
    }
    sql += QStringLiteral(" ORDER BY last_copied_at DESC LIMIT ?");

    QSqlQuery query(database_);
    query.prepare(sql);
    if (!category.isEmpty() && category != QStringLiteral("文本") && category != QStringLiteral("图片")) {
        query.addBindValue(category);
    }
    if (!search.trimmed().isEmpty()) {
        const QString pattern = QStringLiteral("%%1%").arg(escapedLike(search.trimmed()));
        query.addBindValue(pattern);
        query.addBindValue(pattern);
    }
    query.addBindValue(std::max(1, limit));
    if (!query.exec()) {
        lastError_ = query.lastError().text();
        return result;
    }
    while (query.next()) {
        result.append(itemFromQuery(query));
    }
    return result;
}

bool HistoryRepository::updateTitle(qint64 id, const QString &title) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("UPDATE content SET title=?,updated_at=? WHERE id=?"));
    query.addBindValue(title.trimmed().isEmpty() ? QStringLiteral("文本") : title.trimmed());
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(id);
    return query.exec();
}

bool HistoryRepository::updateText(qint64 id, const QString &text, const QString &category) {
    ClipboardItem hashItem;
    hashItem.text = text;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "UPDATE content SET plain_text=?,rich_text_html=NULL,content_hash=?,category=?,updated_at=? WHERE id=?"));
    query.addBindValue(text);
    query.addBindValue(contentHash(hashItem));
    query.addBindValue(category);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(id);
    return query.exec();
}

bool HistoryRepository::updateImage(qint64 id, const QByteArray &pngData, const QImage &image) {
    ClipboardItem hashItem;
    hashItem.type = ClipboardPayloadType::Image;
    hashItem.imageData = pngData;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "UPDATE content SET image_data=?,image_width=?,image_height=?,content_hash=?,updated_at=? WHERE id=?"));
    query.addBindValue(pngData);
    query.addBindValue(image.width());
    query.addBindValue(image.height());
    query.addBindValue(contentHash(hashItem));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(id);
    return query.exec();
}

bool HistoryRepository::remove(qint64 id) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("DELETE FROM content WHERE id=?"));
    query.addBindValue(id);
    return query.exec();
}

int HistoryRepository::clearExpired(int retentionDays) {
    if (retentionDays <= 0) {
        return 0;
    }
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("DELETE FROM content WHERE last_copied_at < ?"));
    query.addBindValue(QDateTime::currentDateTime().addDays(-retentionDays).toMSecsSinceEpoch());
    if (!query.exec()) {
        lastError_ = query.lastError().text();
        return 0;
    }
    return query.numRowsAffected();
}

int HistoryRepository::clearAll() {
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("DELETE FROM content"))) {
        lastError_ = query.lastError().text();
        return 0;
    }
    const int removed = query.numRowsAffected();
    execute(QStringLiteral("VACUUM"));
    return removed;
}

qint64 HistoryRepository::databaseByteCount() const {
    qint64 total = 0;
    for (const QString &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        total += QFileInfo(databasePath_ + suffix).size();
    }
    return total;
}

QString HistoryRepository::classifyText(const QString &input) {
    const QString text = input.trimmed();
    static const QRegularExpression url(QStringLiteral("^https?://\\S+$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression sql(QStringLiteral("^(select|insert|update|delete|create|alter|drop)\\b"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression markdown(QStringLiteral("(^|\\n)(#{1,6} |[-*] |```)") );
    static const QRegularExpression code(QStringLiteral("\\b(function|const|let|var|class|public|private|import|export|def|func)\\b|=>|[{};]"));
    static const QRegularExpression color(QStringLiteral("^(#(?:[0-9a-f]{3}|[0-9a-f]{4}|[0-9a-f]{6}|[0-9a-f]{8})|(?=[0-9a-f]*[a-f])(?:[0-9a-f]{6}|[0-9a-f]{8})|rgba?\\(.*\\))$"), QRegularExpression::CaseInsensitiveOption);
    if (url.match(text).hasMatch()) return QStringLiteral("URL");
    if ((text.startsWith(QLatin1Char('{')) && text.endsWith(QLatin1Char('}')))
        || (text.startsWith(QLatin1Char('[')) && text.endsWith(QLatin1Char(']')))) {
        return QStringLiteral("JSON");
    }
    if (sql.match(text).hasMatch()) return QStringLiteral("SQL");
    if (markdown.match(text).hasMatch()) return QStringLiteral("Markdown");
    if (color.match(text).hasMatch()) return QStringLiteral("色彩");
    if (code.match(text).hasMatch()) return QStringLiteral("代码");
    return QString();
}

QStringList HistoryRepository::presetCategories() {
    return {QStringLiteral("JSON"), QStringLiteral("URL"), QStringLiteral("SQL"),
            QStringLiteral("Markdown"), QStringLiteral("代码"), QStringLiteral("色彩")};
}

QByteArray HistoryRepository::contentHash(const ClipboardItem &item) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(item.isImage() ? QByteArrayLiteral("image:") : QByteArrayLiteral("text:"));
    hash.addData(item.isImage() ? item.imageData : item.text.toUtf8());
    return hash.result();
}

ClipboardItem HistoryRepository::itemFromQuery(const QSqlQuery &query) {
    ClipboardItem item;
    item.databaseID = query.value(0).toLongLong();
    item.title = query.value(1).toString();
    item.type = query.value(2).toString() == QStringLiteral("image")
        ? ClipboardPayloadType::Image : ClipboardPayloadType::Text;
    item.text = query.value(3).toString();
    item.richTextHtml = query.value(4).toString();
    item.imageData = query.value(5).toByteArray();
    if (item.isImage()) {
        item.image.loadFromData(item.imageData, "PNG");
    }
    item.sourceID = query.value(8).toString();
    item.sourceName = query.value(9).toString();
    item.sourceIconData = query.value(10).toByteArray();
    if (!item.sourceIconData.isEmpty()) item.sourceIcon.loadFromData(item.sourceIconData, "PNG");
    item.category = query.value(11).toString();
    item.copiedAt = QDateTime::fromMSecsSinceEpoch(query.value(12).toLongLong());
    item.copyCount = query.value(13).toInt();
    return item;
}

bool HistoryRepository::execute(const QString &sql) const {
    QSqlQuery query(database_);
    if (query.exec(sql)) {
        return true;
    }
    lastError_ = query.lastError().text();
    return false;
}
