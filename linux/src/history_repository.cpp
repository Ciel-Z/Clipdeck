#include "history_repository.h"
#include "history/hybrid_logical_clock.h"

#include <QtGlobal>
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
#define QT_STRING_SKIP_EMPTY Qt::SkipEmptyParts
#else
#define QT_STRING_SKIP_EMPTY QString::SkipEmptyParts
#endif

#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJSEngine>
#include <QJSValue>
#include <QRegularExpression>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <cstdlib>
#include <utility>

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

bool isDDEGrandSearchSource(const QString &sourceID, const QString &sourceName) {
    const QString identity = (sourceID + QLatin1Char(' ') + sourceName).toLower();
    return identity.contains(QStringLiteral("dde-grand-search"))
        || identity.contains(QStringLiteral("dde grand search"));
}

struct CategoryPreset {
    QString name;
    QString icon;
    QString language;
    QString script;
};

const QVector<CategoryPreset> &defaultCategoryPresets() {
    static const QVector<CategoryPreset> presets = {
        {QStringLiteral("JSON"), QStringLiteral("{}"), QStringLiteral("json"), QString::fromUtf8(R"JS(function classify(item) {
    const text = item.text.trim();
    function parse(source) {
        try {
            return JSON.parse(source);
        } catch (_) {
            return undefined;
        }
    }
    function isContainer(value) {
        return value !== null && typeof value === "object";
    }
    // Empty containers and one-number lists ("{}", "[]", "[1]") read as
    // placeholders or citations far more often than as data.
    function isData(value) {
        if (!isContainer(value)) return false;
        if (Array.isArray(value)) {
            return value.length > 1 || (value.length === 1 && isContainer(value[0]));
        }
        return Object.keys(value).length > 0;
    }

    const first = text.charAt(0);
    const last = text.charAt(text.length - 1);
    if ((first === "{" && last === "}") || (first === "[" && last === "]")) {
        if (isData(parse(text))) return true;
    }

    // JSON Lines (log lines, exports): every non-empty line is an object or array.
    const lines = text.split(/\r?\n/).filter(function (line) {
        return line.trim() !== "";
    });
    return lines.length >= 2 && lines.every(function (line) {
        return isContainer(parse(line.trim()));
    });
})JS")},
        {QStringLiteral("URL"), QStringLiteral("↗"), QStringLiteral("url"), QString::fromUtf8(R"JS(function classify(item) {
    // One http(s) link whose host is a real name, IPv4/IPv6 address or
    // localhost: what the Deck shows as a link card.
    return /^https?:\/\/(?:[^\s\/?#@]+@)?(?:\[[0-9a-f:.]+\]|[^\s\/?#:.@-][^\s\/?#:@]*)(?::\d{1,5})?(?:[\/?#]\S*)?$/i
        .test(item.text.trim());
})JS")},
        {QStringLiteral("SQL"), QStringLiteral("◫"), QStringLiteral("sql"), QString::fromUtf8(R"JS(function classify(item) {
    // Leading comments and an opening parenthesis do not change the statement.
    const text = item.text.trim()
        .replace(/^(?:\s+|--[^\n]*(?:\n|$)|\/\*[\s\S]*?\*\/)+/, "")
        .replace(/^\(\s*/, "");
    if (text.length < 6) return false;

    // A table or column name, never an English article or pronoun, so that
    // "Delete from the list" or "Insert into your document" stay text.
    const name = "(?!(?:the|a|an|this|that|these|those|my|your|our|their|it|here|there)\\b)[\\w$.`\"\\[\\]]+";
    const statements = [
        new RegExp("^select\\s+(?:distinct\\s+|top\\s+\\d+\\s+)?(?!from\\b)[\\s\\S]+?\\bfrom\\s+" + name, "i"),
        /^select\s+(?:\d+(?:\.\d+)?|'[^']*'|@@?\w+|\w+\s*\([^()]*\)|\*)\s*(?:as\s+\w+\s*)?(?:,[^\n]*)?;?$/i,
        new RegExp("^(?:insert(?:\\s+ignore)?|replace|upsert)\\s+into\\s+" + name, "i"),
        new RegExp("^update\\s+" + name + "\\s+set\\s+[\\w$.`\"\\[\\]]+\\s*=", "i"),
        new RegExp("^delete\\s+from\\s+" + name, "i"),
        new RegExp("^(?:create|alter|drop)\\s+(?:or\\s+replace\\s+|temp(?:orary)?\\s+|unique\\s+|materialized\\s+)*"
            + "(?:table|view|index|database|schema|function|procedure|trigger|sequence|type|extension|user|role)\\s+"
            + "(?:if\\s+(?:not\\s+)?exists\\s+)?" + name
            + "\\s*(?:\\(|;|$|\\n|\\s(?:as|like|on|add|drop|alter|rename|modify|owner|set|cascade|restrict)\\b)", "i"),
        /^with\s+(?:recursive\s+)?[\w"`]+\s*(?:\([^)]*\))?\s+as\s*\(/i,
        new RegExp("^truncate\\s+(?:table\\s+)?" + name, "i"),
        /^merge\s+into\s+\S+/i,
        /^explain\s+(?:analyze\s+|query\s+plan\s+)?(?:select|insert|update|delete|with)\b/i,
        /^(?:grant|revoke)\s+[\w\s,]+?\s+on\s+\S+\s+(?:to|from)\s+\S+/i,
        /^show\s+(?:full\s+)?(?:tables|databases|schemas|columns|indexes|index|create\s+table|processlist|variables|grants)\b/i,
        // "Describe yourself" / "Use Chrome" are sentences: these short MySQL
        // commands count only when written like commands.
        /^(?:desc|describe|use)\s+[\w$.`"]+\s*;$/i,
        /^(?:DESC|DESCRIBE|USE)\s+[\w$.`"]+$/
    ];
    if (!statements.some(function (pattern) { return pattern.test(text); })) return false;

    // Prose that happens to start like SQL: everyday words outside quotes.
    const bare = text.replace(/'(?:[^']|'')*'|"(?:[^"]|"")*"/g, "''");
    const prose = bare.match(/\b(?:the|you|your|please|we|our|my|me|this|these|those|will|should|would|could|can|then|just)\b/gi);
    if (prose && prose.length >= 2) return false;
    // A sentence ending needs some SQL punctuation or clause to back it up.
    if (/[.!?。！？]$/.test(bare)) return /[*=(),;]|\b(?:where|join|group\s+by|order\s+by|limit|values)\b/i.test(bare);
    return true;
})JS")},
        {QStringLiteral("Markdown"), QStringLiteral("M↓"), QStringLiteral("markdown"), QString::fromUtf8(R"JS(function classify(item) {
    const text = item.text.replace(/\r\n?/g, "\n");
    function count(pattern) {
        const matches = text.match(pattern);
        return matches ? matches.length : 0;
    }

    // Structures only Markdown uses: one is enough.
    const strong = [
        /(?:^|\n)(`{3,}|~{3,})[^\n]*\n[\s\S]*?\n\1[ \t]*(?:\n|$)/,                  // fenced code block
        /^(`{3,}|~{3,})[\w+#.-]*\n/,                                                  // fence opening a copy
        /(?:^|\n)[^\n]*\|[^\n]*\n[ \t]*\|?[ \t]*:?-{3,}:?[ \t]*(?:\|[ \t]*:?-{3,}:?[ \t]*)+\|?[ \t]*(?:\n|$)/, // table
        /!?\[[^\]\n]+\]\((?:https?:\/\/|mailto:|\/|\.{1,2}\/|#)[^)\s]*(?:\s+"[^"\n]*")?\)/, // [link](url), ![image](src)
        /(?:^|\n)#{2,6}[ \t]+\S/,                                                     // ## heading
        /(?:^|\n)[ \t]{0,3}[-*+][ \t]+\[[ xX]\][ \t]+\S/                              // - [ ] task
    ];
    if (strong.some(function (pattern) { return pattern.test(text); })) return true;

    // Shapes plain notes, shell comments or code also use: two different
    // kinds are needed. A lone "# comment" line or one "- item" stays text.
    const weak = [
        /(?:^|\n)#[ \t]+\S/.test(text),                                               // # heading
        count(/(?:^|\n)[ \t]{0,3}[-*+][ \t]+\S/g) >= 2,                               // bullet list
        count(/(?:^|\n)[ \t]{0,3}\d{1,3}[.)][ \t]+\S/g) >= 2,                         // numbered list
        /(?:^|\n)>[ \t]?\S/.test(text),                                               // > quote
        /(?:^|[\s(])(\*\*|__)(?=\S)[^\n]*?\S\1(?=[\s).,:;!?，。；：！？]|$)/.test(text), // **bold**
        /(?:^|[^`])`[^`\n]+`(?!`)/.test(text),                                        // `code`
        /(?:^|\n)[ \t]*(?:-{3,}|\*{3,}|_{3,})[ \t]*(?:\n|$)/.test(text)               // --- rule
    ];
    return weak.filter(Boolean).length >= 2;
})JS")},
        {QStringLiteral("代码"), QStringLiteral("</>"), QStringLiteral("code"), QString::fromUtf8(R"JS(function classify(item) {
    return item.codeEngineMatched === true;
})JS")},
        {QStringLiteral("色彩"), QStringLiteral("◉"), QStringLiteral("color"), QString::fromUtf8(R"JS(function classify(item) {
    const text = item.text.trim();
    const hashHex = /^#(?:[0-9a-f]{3}|[0-9a-f]{4}|[0-9a-f]{6}|[0-9a-f]{8})$/i;
    const bareHex = /^(?=[0-9a-f]*[a-f])(?:[0-9a-f]{6}|[0-9a-f]{8})$/i;
    if (hashHex.test(text) || bareHex.test(text)) {
        return true;
    }

    const match = /^rgba?\((.*)\)$/i.exec(text);
    if (!match) {
        return false;
    }

    const channels = match[1].split(",").map(function(value) {
        return value.trim();
    });
    if (channels.length !== 3 && channels.length !== 4) {
        return false;
    }

    const numericChannel = /^\d+(?:\.\d+)?%?$/;
    function validColorChannel(value) {
        return numericChannel.test(value) && (value.endsWith("%")
            ? Number(value.slice(0, -1)) >= 0 && Number(value.slice(0, -1)) <= 100
            : Number(value) >= 0 && Number(value) <= 255);
    }
    function validAlphaChannel(value) {
        return numericChannel.test(value) && (value.endsWith("%")
            ? Number(value.slice(0, -1)) >= 0 && Number(value.slice(0, -1)) <= 100
            : Number(value) >= 0 && Number(value) <= 1);
    }

    return channels.slice(0, 3).every(validColorChannel)
        && (channels.length === 3 || validAlphaChannel(channels[3]));
})JS")}
    };
    return presets;
}

const QMap<QString, QString> &previousFormattedDefaultScripts() {
    static const QMap<QString, QString> scripts = {
        {QStringLiteral("JSON"), QString::fromUtf8(R"JS(function classify(item) {
    const text = item.text.trim();
    const isContainer =
        (text.startsWith("{") && text.endsWith("}")) ||
        (text.startsWith("[") && text.endsWith("]"));
    if (!isContainer) return false;
    try {
        const value = JSON.parse(text);
        return value !== null && typeof value === "object";
    } catch (_) {
        return false;
    }
})JS")},
        {QStringLiteral("URL"), QString::fromUtf8(R"JS(function classify(item) {
    return /^https?:\/\/\S+$/i.test(item.text.trim());
})JS")},
        {QStringLiteral("SQL"), QString::fromUtf8(R"JS(function classify(item) {
    return /^(select|insert|update|delete|create|alter|drop)\b/i.test(
        item.text.trim()
    );
})JS")},
        {QStringLiteral("Markdown"), QString::fromUtf8(R"JS(function classify(item) {
    return /(^|\n)(#{1,6} |[-*] |```)/.test(item.text);
})JS")},
    };
    return scripts;
}

QString legacyLinuxDefaultScript(const QString &name) {
    static const QMap<QString, QString> scripts = {
        {QStringLiteral("JSON"), QStringLiteral("function classify(item){const t=item.text.trim();if(!((t.startsWith('{')&&t.endsWith('}'))||(t.startsWith('[')&&t.endsWith(']'))))return false;try{const v=JSON.parse(t);return v!==null&&typeof v==='object';}catch(e){return false;}}")},
        {QStringLiteral("URL"), QStringLiteral("function classify(item){return /^https?:\\/\\/\\S+$/i.test(item.text.trim());}")},
        {QStringLiteral("SQL"), QStringLiteral("function classify(item){return /^(select|insert|update|delete|create|alter|drop)\\b/i.test(item.text.trim());}")},
        {QStringLiteral("Markdown"), QStringLiteral("function classify(item){return /(^|\\n)(#{1,6} |[-*] |```)/.test(item.text);}")},
        {QStringLiteral("代码"), QStringLiteral("function classify(item){return item.codeEngineMatched===true;}")},
        {QStringLiteral("色彩"), QStringLiteral("function classify(item){var t=item.text.trim();var hash=/^#(?:[0-9a-f]{3}|[0-9a-f]{4}|[0-9a-f]{6}|[0-9a-f]{8})$/i;var bare=/^(?=[0-9a-f]*[a-f])(?:[0-9a-f]{6}|[0-9a-f]{8})$/i;if(hash.test(t)||bare.test(t))return true;var m=/^rgba?\\((.*)\\)$/i.exec(t);if(!m)return false;var c=m[1].split(',').map(function(v){return v.trim();});if(c.length!==3&&c.length!==4)return false;var n=/^\\d+(?:\\.\\d+)?%?$/;function channel(v,max){if(!n.test(v))return false;var percent=v.charAt(v.length-1)==='%';var number=Number(percent?v.slice(0,-1):v);return number>=0&&number<=(percent?100:max);}for(var i=0;i<3;i++){if(!channel(c[i],255))return false;}return c.length===3||channel(c[3],1);}")}
    };
    return scripts.value(name);
}

} // namespace

HistoryRepository::HistoryRepository(const QString &databasePath)
    : connectionName_(QStringLiteral("clipdeck-%1").arg(QUuid::createUuid().toString())) {
    if (databasePath.isEmpty()) {
        const QString dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dataDirectory);
        databasePath_ = QDir(dataDirectory).filePath(QStringLiteral("history.sqlite"));
    } else {
        QDir().mkpath(QFileInfo(databasePath).absolutePath());
        databasePath_ = databasePath;
    }
    database_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    database_.setDatabaseName(databasePath_);
    if (!database_.open()) {
        lastError_ = database_.lastError().text();
        return;
    }
    ready_ = initialize();
    if (!ready_) {
        const QString firstError = lastError_;
        ready_ = initialize();
        if (!ready_ && lastError_.isEmpty()) lastError_ = firstError;
    }
}

HistoryRepository::~HistoryRepository() {
    if (database_.isOpen()) {
        database_.close();
    }
    database_ = QSqlDatabase();
    QSqlDatabase::removeDatabase(connectionName_);
}

bool HistoryRepository::isOpen() const {
    return database_.isOpen() && ready_;
}

QString HistoryRepository::lastError() const {
    return lastError_;
}

bool HistoryRepository::initialize() {
    if (!execute(QStringLiteral("PRAGMA busy_timeout = 2000"))
        || !execute(QStringLiteral("PRAGMA foreign_keys = ON"))
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
        "link_icon_data BLOB,"
        "link_preview_data BLOB,"
        "link_preview_fetched INTEGER NOT NULL DEFAULT 0,"
        "image_data BLOB,"
        "image_width INTEGER,"
        "image_height INTEGER,"
        "image_angle INTEGER NOT NULL DEFAULT 0,"
        "content_hash BLOB NOT NULL,"
        "source_id TEXT,"
        "source_name TEXT NOT NULL,"
        "source_icon BLOB,"
        "source_icon_variants BLOB,"
        "category TEXT,"
        "copied_at INTEGER NOT NULL,"
        "last_copied_at INTEGER NOT NULL,"
        "copy_count INTEGER NOT NULL DEFAULT 1,"
        "updated_at INTEGER NOT NULL)"))
        && execute(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS application_icon ("
            "source_id TEXT PRIMARY KEY,"
            "source_name TEXT NOT NULL,"
            "source_icon BLOB,"
            "source_icon_variants BLOB,"
            "updated_at INTEGER NOT NULL)"))
        && execute(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_content_last_copied "
            "ON content(last_copied_at DESC)"))
        && execute(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_content_category "
            "ON content(category)"))
        && execute(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_content_hash_recent "
            "ON content(content_hash,last_copied_at DESC)"))
        && execute(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS category ("
            "id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "name TEXT NOT NULL UNIQUE,"
            "icon TEXT,"
            "script TEXT NOT NULL,"
            "preview_language TEXT,"
            "sort_order INTEGER NOT NULL DEFAULT 0)"))
        && [this] {
            QSqlQuery columns(database_);
            if (!columns.exec(QStringLiteral("PRAGMA table_info(content)"))) return false;
            QSet<QString> names;
            while (columns.next()) names.insert(columns.value(1).toString());
            columns.finish();
            if (!names.contains(QStringLiteral("rich_text_html"))
                && !execute(QStringLiteral("ALTER TABLE content ADD COLUMN rich_text_html TEXT"))) {
                return false;
            }
            if (!names.contains(QStringLiteral("source_icon"))
                && !execute(QStringLiteral("ALTER TABLE content ADD COLUMN source_icon BLOB"))) {
                return false;
            }
            if (!names.contains(QStringLiteral("source_icon_variants"))
                && !execute(QStringLiteral(
                    "ALTER TABLE content ADD COLUMN source_icon_variants BLOB"))) {
                return false;
            }
            if (!names.contains(QStringLiteral("link_icon_data"))
                && !execute(QStringLiteral("ALTER TABLE content ADD COLUMN link_icon_data BLOB"))) {
                return false;
            }
            if (!names.contains(QStringLiteral("link_preview_data"))
                && !execute(QStringLiteral("ALTER TABLE content ADD COLUMN link_preview_data BLOB"))) {
                return false;
            }
            if (!names.contains(QStringLiteral("link_preview_fetched"))
                && !execute(QStringLiteral(
                    "ALTER TABLE content ADD COLUMN link_preview_fetched INTEGER NOT NULL DEFAULT 0"))) {
                return false;
            }
            if (!names.contains(QStringLiteral("image_angle"))
                && !execute(QStringLiteral(
                    "ALTER TABLE content ADD COLUMN image_angle INTEGER NOT NULL DEFAULT 0"))) {
                return false;
            }
            QSqlQuery tableDefinition(database_);
            tableDefinition.prepare(QStringLiteral(
                "SELECT sql FROM sqlite_master WHERE type='table' AND name='content'"));
            if (!tableDefinition.exec() || !tableDefinition.next()) return false;
            const QString contentSQL = tableDefinition.value(0).toString();
            tableDefinition.finish();
            if (contentSQL.contains(QStringLiteral("content_hash BLOB NOT NULL UNIQUE"),
                                    Qt::CaseInsensitive)) {
                if (!database_.transaction()) return false;
                QSqlQuery migration(database_);
                const QStringList statements = {
                    QStringLiteral(
                        "CREATE TABLE content_v2 ("
                        "id INTEGER PRIMARY KEY AUTOINCREMENT,title TEXT NOT NULL,content_type TEXT NOT NULL,"
                        "plain_text TEXT,rich_text_html TEXT,link_icon_data BLOB,link_preview_data BLOB,"
                        "link_preview_fetched INTEGER NOT NULL DEFAULT 0,image_data BLOB,image_width INTEGER,image_height INTEGER,"
                        "image_angle INTEGER NOT NULL DEFAULT 0,"
                        "content_hash BLOB NOT NULL,source_id TEXT,source_name TEXT NOT NULL,source_icon BLOB,"
                        "source_icon_variants BLOB,category TEXT,"
                        "copied_at INTEGER NOT NULL,last_copied_at INTEGER NOT NULL,copy_count INTEGER NOT NULL DEFAULT 1,"
                        "updated_at INTEGER NOT NULL)"),
                    QStringLiteral(
                        "INSERT INTO content_v2(id,title,content_type,plain_text,rich_text_html,link_icon_data,link_preview_data,"
                        "link_preview_fetched,image_data,image_width,image_height,image_angle,"
                        "content_hash,source_id,source_name,source_icon,source_icon_variants,category,"
                        "copied_at,last_copied_at,copy_count,updated_at) "
                        "SELECT id,title,content_type,plain_text,rich_text_html,link_icon_data,link_preview_data,"
                        "link_preview_fetched,image_data,image_width,image_height,image_angle,"
                        "content_hash,source_id,source_name,source_icon,source_icon_variants,category,"
                        "copied_at,last_copied_at,copy_count,updated_at FROM content"),
                    QStringLiteral("DROP TABLE content"),
                    QStringLiteral("ALTER TABLE content_v2 RENAME TO content")
                };
                for (const QString &statement : statements) {
                    if (!migration.exec(statement)) {
                        lastError_ = migration.lastError().text();
                        database_.rollback();
                        return false;
                    }
                }
                if (!database_.commit()) return false;
                if (!execute(QStringLiteral(
                        "CREATE INDEX IF NOT EXISTS idx_content_last_copied ON content(last_copied_at DESC)"))
                    || !execute(QStringLiteral(
                        "CREATE INDEX IF NOT EXISTS idx_content_category ON content(category)"))
                    || !execute(QStringLiteral(
                        "CREATE INDEX IF NOT EXISTS idx_content_hash_recent ON content(content_hash,last_copied_at DESC)"))) {
                    return false;
                }
            }
            QSqlQuery fileColumn(database_);
            if (!fileColumn.exec(QStringLiteral("PRAGMA table_info(content)"))) return false;
            bool hasFiles = false;
            while (fileColumn.next()) hasFiles = hasFiles || fileColumn.value(1).toString() == QStringLiteral("files_json");
            fileColumn.finish();
            if (!hasFiles && !execute(QStringLiteral("ALTER TABLE content ADD COLUMN files_json BLOB"))) return false;
            QSqlQuery categoryColumns(database_);
            if (!categoryColumns.exec(QStringLiteral("PRAGMA table_info(category)"))) return false;
            QSet<QString> categoryNames;
            while (categoryColumns.next()) categoryNames.insert(categoryColumns.value(1).toString());
            if (!categoryNames.contains(QStringLiteral("icon"))
                && !execute(QStringLiteral("ALTER TABLE category ADD COLUMN icon TEXT"))) return false;
            if (!categoryNames.contains(QStringLiteral("preview_language"))
                && !execute(QStringLiteral("ALTER TABLE category ADD COLUMN preview_language TEXT"))) return false;
            QSqlQuery count(database_);
            if (!count.exec(QStringLiteral("SELECT COUNT(*) FROM category")) || !count.next()) return false;
            const bool hasCategories = count.value(0).toInt() > 0;
            const QVector<CategoryPreset> &presets = defaultCategoryPresets();
            for (int index = 0; index < presets.size(); ++index) {
                QSqlQuery query(database_);
                if (hasCategories) {
                    query.prepare(QStringLiteral(
                        "UPDATE category SET icon=COALESCE(icon,?),preview_language=COALESCE(preview_language,?),"
                        "script=CASE WHEN script IN (?,?) THEN ? ELSE script END WHERE name=?"));
                    query.addBindValue(presets[index].icon);
                    query.addBindValue(presets[index].language);
                    // Only untouched defaults move to the current rule; edited
                    // scripts are the user's and stay as they are.
                    const QString legacy = legacyLinuxDefaultScript(presets[index].name);
                    query.addBindValue(legacy);
                    query.addBindValue(previousFormattedDefaultScripts().value(presets[index].name, legacy));
                    query.addBindValue(presets[index].script);
                    query.addBindValue(presets[index].name);
                } else {
                    query.prepare(QStringLiteral("INSERT INTO category(name,icon,script,preview_language,sort_order) VALUES(?,?,?,?,?)"));
                    query.addBindValue(presets[index].name);
                    query.addBindValue(presets[index].icon);
                    query.addBindValue(presets[index].script);
                    query.addBindValue(presets[index].language);
                    query.addBindValue(index);
                }
                if (!query.exec()) return false;
            }
            return migrateSyncColumns() && collapseRecentDuplicates();
        }();
}

bool HistoryRepository::migrateSyncColumns() {
    QSqlQuery columns(database_);
    if (!columns.exec(QStringLiteral("PRAGMA table_info(content)"))) return false;
    QSet<QString> names;
    while (columns.next()) names.insert(columns.value(1).toString());
    columns.finish();
    if (!names.contains(QStringLiteral("sync_id"))
        && !execute(QStringLiteral("ALTER TABLE content ADD COLUMN sync_id TEXT"))) return false;
    if (!names.contains(QStringLiteral("origin_device"))
        && !execute(QStringLiteral("ALTER TABLE content ADD COLUMN origin_device TEXT"))) return false;
    if (!names.contains(QStringLiteral("updated_hlc"))
        && !execute(QStringLiteral(
            "ALTER TABLE content ADD COLUMN updated_hlc INTEGER NOT NULL DEFAULT 0"))) return false;
    if (!execute(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS sync_deleted ("
            "sync_id TEXT PRIMARY KEY,"
            "deleted_at INTEGER NOT NULL)"))) return false;
    if (!execute(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS sync_peer ("
            "device_id TEXT PRIMARY KEY,"
            "hostname TEXT NOT NULL,"
            "public_key BLOB NOT NULL,"
            "addresses TEXT,"
            "last_seen INTEGER NOT NULL DEFAULT 0,"
            "sent_cursor INTEGER NOT NULL DEFAULT 0)"))) return false;

    // Rows from before sync: give each an identity, and a clock value taken
    // from its last change so their order is kept.
    QSqlQuery missing(database_);
    if (!missing.exec(QStringLiteral("SELECT id,updated_at FROM content WHERE sync_id IS NULL OR updated_hlc=0")))
        return false;
    QVector<QPair<qint64, qint64>> rows;
    while (missing.next()) rows.append({missing.value(0).toLongLong(), missing.value(1).toLongLong()});
    missing.finish();
    if (!rows.isEmpty()) {
        if (!database_.transaction()) return false;
        QSqlQuery fill(database_);
        fill.prepare(QStringLiteral(
            "UPDATE content SET sync_id=COALESCE(sync_id,?),"
            "updated_hlc=CASE WHEN updated_hlc=0 THEN ? ELSE updated_hlc END WHERE id=?"));
        for (const auto &row : rows) {
            fill.bindValue(0, QUuid::createUuid().toString(QUuid::WithoutBraces));
            fill.bindValue(1, QVariant::fromValue<qint64>(qint64(HybridLogicalClock::make(row.second))));
            fill.bindValue(2, row.first);
            if (!fill.exec()) {
                lastError_ = fill.lastError().text();
                database_.rollback();
                return false;
            }
        }
        if (!database_.commit()) return false;
    }
    if (!execute(QStringLiteral(
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_content_sync_id ON content(sync_id)"))
        || !execute(QStringLiteral(
            "CREATE INDEX IF NOT EXISTS idx_content_updated_hlc ON content(updated_hlc)"))) return false;

    QSqlQuery latest(database_);
    if (latest.exec(QStringLiteral("SELECT MAX(updated_hlc) FROM content")) && latest.next())
        HybridLogicalClock::shared().observe(quint64(latest.value(0).toLongLong()));
    pruneDeletedRecords();
    return true;
}

void HistoryRepository::pruneDeletedRecords() {
    QSqlQuery prune(database_);
    prune.prepare(QStringLiteral("DELETE FROM sync_deleted WHERE deleted_at<?"));
    prune.addBindValue(QDateTime::currentMSecsSinceEpoch() - kDeletedRecordRetentionMs);
    if (!prune.exec()) lastError_ = prune.lastError().text();
}

bool HistoryRepository::collapseRecentDuplicates() {
    constexpr qint64 kDuplicateWindowMs = 24LL * 60 * 60 * 1000;
    struct Row {
        qint64 id = -1;
        QByteArray hash;
        qint64 lastCopiedAt = 0;
        int copyCount = 1;
        QString sourceID;
        QString sourceName;
        QByteArray sourceIcon;
        QByteArray sourceIconVariants;
    };

    QVector<Row> rows;
    QSqlQuery select(database_);
    if (!select.exec(QStringLiteral(
            "SELECT id,content_hash,last_copied_at,copy_count,source_id,source_name,source_icon,source_icon_variants "
            "FROM content ORDER BY last_copied_at DESC,id DESC"))) {
        lastError_ = select.lastError().text();
        return false;
    }
    while (select.next()) {
        Row row;
        row.id = select.value(0).toLongLong();
        row.hash = select.value(1).toByteArray();
        row.lastCopiedAt = select.value(2).toLongLong();
        row.copyCount = std::max(1, select.value(3).toInt());
        row.sourceID = select.value(4).toString();
        row.sourceName = select.value(5).toString();
        row.sourceIcon = select.value(6).toByteArray();
        row.sourceIconVariants = select.value(7).toByteArray();
        rows.append(std::move(row));
    }
    if (rows.size() < 2) return true;

    QHash<QByteArray, int> latestGroupByHash;
    QHash<int, QVector<qint64>> duplicateIDsByKeptIndex;
    for (int index = 0; index < rows.size(); ++index) {
        const int keptIndex = latestGroupByHash.value(rows[index].hash, -1);
        if (keptIndex >= 0
            && rows[keptIndex].lastCopiedAt - rows[index].lastCopiedAt <= kDuplicateWindowMs) {
            Row &kept = rows[keptIndex];
            const Row &duplicate = rows[index];
            kept.copyCount += duplicate.copyCount;
            if (isDDEGrandSearchSource(kept.sourceID, kept.sourceName)
                && !isDDEGrandSearchSource(duplicate.sourceID, duplicate.sourceName)
                && !duplicate.sourceID.isEmpty()) {
                kept.sourceID = duplicate.sourceID;
                kept.sourceName = duplicate.sourceName;
                kept.sourceIcon = duplicate.sourceIcon;
                kept.sourceIconVariants = duplicate.sourceIconVariants;
            }
            duplicateIDsByKeptIndex[keptIndex].append(duplicate.id);
        } else {
            latestGroupByHash.insert(rows[index].hash, index);
        }
    }
    if (duplicateIDsByKeptIndex.isEmpty()) return true;

    if (!database_.transaction()) return false;
    for (auto group = duplicateIDsByKeptIndex.cbegin();
         group != duplicateIDsByKeptIndex.cend(); ++group) {
            const Row &kept = rows[group.key()];
            QSqlQuery update(database_);
            update.prepare(QStringLiteral(
                "UPDATE content SET copy_count=?,source_id=?,source_name=?,source_icon=?,"
                "source_icon_variants=?,updated_at=? WHERE id=?"));
            update.addBindValue(kept.copyCount);
            update.addBindValue(kept.sourceID);
            update.addBindValue(kept.sourceName);
            update.addBindValue(kept.sourceIcon);
            update.addBindValue(kept.sourceIconVariants);
            update.addBindValue(QDateTime::currentMSecsSinceEpoch());
            update.addBindValue(kept.id);
            if (!update.exec()) {
                lastError_ = update.lastError().text();
                database_.rollback();
                return false;
            }
            QSqlQuery remove(database_);
            remove.prepare(QStringLiteral("DELETE FROM content WHERE id=?"));
            for (const qint64 duplicateID : group.value()) {
                remove.bindValue(0, duplicateID);
                if (!remove.exec()) {
                    lastError_ = remove.lastError().text();
                    database_.rollback();
                    return false;
                }
            }
            qInfo() << "[ClipDeck][History] collapsed" << group.value().size()
                    << "duplicate(s) from the 24-hour window into" << kept.id;
    }
    if (!database_.commit()) {
        lastError_ = database_.lastError().text();
        return false;
    }
    return true;
}

ClipboardItem HistoryRepository::save(const ClipboardItem &input) {
    ClipboardItem item = input;
    if (!isOpen()) {
        return item;
    }
    const QByteArray hash = contentHash(item);
    const qint64 now = milliseconds(item.copiedAt.isValid() ? item.copiedAt : QDateTime::currentDateTime());

    if (!item.sourceID.isEmpty()
        && (!item.sourceIconData.isEmpty() || !item.sourceIconVariantsData.isEmpty())) {
        QSqlQuery applicationIcon(database_);
        applicationIcon.prepare(QStringLiteral(
            "INSERT INTO application_icon(source_id,source_name,source_icon,source_icon_variants,updated_at) "
            "VALUES(?,?,?,?,?) ON CONFLICT(source_id) DO UPDATE SET "
            "source_name=excluded.source_name,"
            "source_icon=CASE WHEN length(excluded.source_icon)>0 THEN excluded.source_icon ELSE source_icon END,"
            "source_icon_variants=CASE WHEN length(excluded.source_icon_variants)>0 "
            "THEN excluded.source_icon_variants ELSE source_icon_variants END,"
            "updated_at=excluded.updated_at"));
        applicationIcon.addBindValue(item.sourceID);
        applicationIcon.addBindValue(item.sourceName.isEmpty() ? QStringLiteral("Unknown") : item.sourceName);
        applicationIcon.addBindValue(item.sourceIconData);
        applicationIcon.addBindValue(item.sourceIconVariantsData);
        applicationIcon.addBindValue(now);
        if (!applicationIcon.exec()) lastError_ = applicationIcon.lastError().text();
    }

    constexpr qint64 kDuplicateWindowMs = 24LL * 60 * 60 * 1000;
    const qint64 duplicateCutoff = now - kDuplicateWindowMs;

    QSqlQuery existing(database_);
    existing.prepare(QStringLiteral(
        "SELECT id,copy_count,copied_at,title,category,source_id,source_name,last_copied_at,sync_id,origin_device "
        "FROM content "
        "WHERE content_hash=? AND last_copied_at>=? AND last_copied_at<=? "
        "ORDER BY last_copied_at DESC,id DESC LIMIT 1"));
    existing.addBindValue(hash);
    existing.addBindValue(duplicateCutoff);
    existing.addBindValue(now + kDuplicateWindowMs);
    if (existing.exec() && existing.next()) {
        item.databaseID = existing.value(0).toLongLong();
        item.copyCount = existing.value(1).toInt() + 1;
        item.title = existing.value(3).toString();
        item.category = existing.value(4).toString();
        const QString existingSourceID = existing.value(5).toString();
        const QString existingSourceName = existing.value(6).toString();
        const qint64 existingTime = existing.value(7).toLongLong();
        item.syncID = existing.value(8).toString();
        item.originDevice = existing.value(9).toString();
        // Copying it again is a change that syncs (the item re-enters the window).
        item.updatedHlc = HybridLogicalClock::shared().now();
        const bool preserveExistingSource = now < existingTime || (isDDEGrandSearchSource(item.sourceID, item.sourceName)
            && !existingSourceID.isEmpty()
            && !isDDEGrandSearchSource(existingSourceID, existingSourceName));
        QSqlQuery update(database_);
        update.prepare(preserveExistingSource
            ? QStringLiteral(
                "UPDATE content SET last_copied_at=?,copy_count=?,updated_at=?,updated_hlc=? WHERE id=?")
            : QStringLiteral(
                "UPDATE content SET source_id=?,source_name=?,source_icon=?,source_icon_variants=?,last_copied_at=?,"
                "copy_count=?,updated_at=?,updated_hlc=? WHERE id=?"));
        if (!preserveExistingSource) {
            update.addBindValue(item.sourceID);
            update.addBindValue(item.sourceName);
            update.addBindValue(item.sourceIconData);
            update.addBindValue(QVariant()); // Stored once in application_icon.
        } else {
            item.sourceID = existingSourceID;
            item.sourceName = existingSourceName;
        }
        update.addBindValue(std::max(now, existingTime));
        update.addBindValue(item.copyCount);
        update.addBindValue(now);
        update.addBindValue(QVariant::fromValue<qint64>(qint64(item.updatedHlc)));
        update.addBindValue(item.databaseID);
        if (!update.exec()) {
            lastError_ = update.lastError().text();
        }
        item.copiedAt = QDateTime::fromMSecsSinceEpoch(std::max(now, existingTime));
        return item;
    }

    QSqlQuery insert(database_);
    insert.prepare(QStringLiteral(
        "INSERT INTO content(title,content_type,plain_text,rich_text_html,link_icon_data,link_preview_data,"
        "link_preview_fetched,image_data,image_width,image_height,image_angle,"
        "content_hash,source_id,source_name,source_icon,source_icon_variants,category,"
        "copied_at,last_copied_at,copy_count,updated_at,files_json,sync_id,origin_device,updated_hlc) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"));
    if (item.syncID.isEmpty()) item.syncID = QUuid::createUuid().toString(QUuid::WithoutBraces);
    item.updatedHlc = HybridLogicalClock::shared().now();
    insert.addBindValue(item.title.isEmpty()
        ? (item.isImage() ? QStringLiteral("图片") : QStringLiteral("文本"))
        : item.title);
    insert.addBindValue(item.isFiles() ? QStringLiteral("files")
        : item.isImage() ? QStringLiteral("image") : QStringLiteral("text"));
    insert.addBindValue(item.isFiles() ? FileReferences::searchableText(item.files)
        : item.isText() ? QVariant(item.text) : QVariant());
    insert.addBindValue(item.isText() ? item.richTextHtml : QVariant());
    insert.addBindValue(item.linkIconData);
    insert.addBindValue(item.linkPreviewData);
    insert.addBindValue(item.hasFetchedLinkPreview ? 1 : 0);
    insert.addBindValue(item.isImage() ? item.imageData : QVariant());
    insert.addBindValue(item.isImage() ? item.image.width() : QVariant());
    insert.addBindValue(item.isImage() ? item.image.height() : QVariant());
    insert.addBindValue(item.isImage() ? ((item.imageAngle % 360) + 360) % 360 : 0);
    insert.addBindValue(hash);
    insert.addBindValue(item.sourceID);
    insert.addBindValue(item.sourceName.isEmpty() ? QStringLiteral("Unknown") : item.sourceName);
    insert.addBindValue(item.sourceIconData);
    insert.addBindValue(QVariant()); // Stored once in application_icon.
    insert.addBindValue(item.category);
    insert.addBindValue(now);
    insert.addBindValue(now);
    insert.addBindValue(1);
    insert.addBindValue(now);
    insert.addBindValue(item.isFiles() ? QVariant(FileReferences::encode(item.files)) : QVariant());
    insert.addBindValue(item.syncID);
    insert.addBindValue(item.originDevice.isEmpty() ? QVariant() : QVariant(item.originDevice));
    insert.addBindValue(QVariant::fromValue<qint64>(qint64(item.updatedHlc)));
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
    int limit,
    int offset,
    qint64 itemID,
    bool decodeImages,
    const ItemTransform &prepare) const {
    QVector<ClipboardItem> result;
    if (!isOpen()) {
        return result;
    }

    QString sql = QStringLiteral(
        "SELECT content.id,content.title,content.content_type,content.plain_text,content.rich_text_html,"
        "content.link_icon_data,content.link_preview_data,content.link_preview_fetched,"
        "content.image_data,content.image_width,content.image_height,"
        "content.source_id,content.source_name,"
        "COALESCE(application_icon.source_icon,content.source_icon),"
        "COALESCE(application_icon.source_icon_variants,content.source_icon_variants),"
        "content.category,content.last_copied_at,content.copy_count,"
        "category.preview_language,category.icon,content.image_angle,content.files_json,"
        "content.sync_id,content.origin_device,content.updated_hlc FROM content "
        "LEFT JOIN application_icon ON application_icon.source_id=content.source_id "
        "LEFT JOIN category ON category.name=content.category WHERE 1=1");
    if (!category.isEmpty()) {
        sql += category == QStringLiteral("文本")
            ? QStringLiteral(" AND content.content_type='text'")
            : category == QStringLiteral("图片")
                ? QStringLiteral(" AND content.content_type='image'")
                : category == QStringLiteral("文件") ? QStringLiteral(" AND content.content_type='files'")
                : QStringLiteral(" AND content.category=?");
    }
    if (!search.trimmed().isEmpty()) {
        sql += QStringLiteral(" AND (content.title LIKE ? ESCAPE '\\' OR content.plain_text LIKE ? ESCAPE '\\')");
    }
    if (itemID >= 0) sql += QStringLiteral(" AND content.id=?");
    sql += QStringLiteral(" ORDER BY content.last_copied_at DESC,content.id DESC LIMIT ? OFFSET ?");

    QSqlQuery query(database_);
    query.prepare(sql);
    if (!category.isEmpty() && category != QStringLiteral("文本") && category != QStringLiteral("图片")
        && category != QStringLiteral("文件")) {
        query.addBindValue(category);
    }
    if (!search.trimmed().isEmpty()) {
        const QString pattern = QStringLiteral("%%1%").arg(escapedLike(search.trimmed()));
        query.addBindValue(pattern);
        query.addBindValue(pattern);
    }
    if (itemID >= 0) query.addBindValue(itemID);
    query.addBindValue(std::max(1, limit));
    query.addBindValue(std::max(0, offset));
    if (!query.exec()) {
        lastError_ = query.lastError().text();
        return result;
    }
    while (query.next()) {
        ClipboardItem item = itemFromQuery(query, decodeImages);
        if (prepare) prepare(item);
        result.append(std::move(item));
    }
    return result;
}

bool HistoryRepository::updateTitle(qint64 id, const QString &title) {
    QSqlQuery query(database_);
    QString value=title.trimmed();
    if(value.isEmpty()) {
        QSqlQuery original(database_);
        original.prepare(QStringLiteral("SELECT content_type,files_json FROM content WHERE id=?"));
        original.addBindValue(id);
        if(!original.exec() || !original.next()) return false;
        if(original.value(0).toString()==QStringLiteral("files")) {
            const auto files=FileReferences::decode(original.value(1).toByteArray());
            value=files.size()==1?files.first().name:QStringLiteral("文件 · %1 项").arg(files.size());
        } else value=original.value(0).toString()==QStringLiteral("image")?QStringLiteral("图片"):QStringLiteral("文本");
    }
    query.prepare(QStringLiteral(
        "UPDATE content SET updated_hlc=CASE WHEN title IS NOT ? THEN ? ELSE updated_hlc END,title=?,updated_at=? "
        "WHERE id=?"));
    query.addBindValue(value);
    query.addBindValue(QVariant::fromValue<qint64>(qint64(HybridLogicalClock::shared().now())));
    query.addBindValue(value);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(id);
    return query.exec();
}

bool HistoryRepository::updateText(qint64 id, const QString &text, const QString &category) {
    ClipboardItem hashItem;
    hashItem.text = text;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "UPDATE content SET updated_hlc=CASE WHEN plain_text IS NOT ? THEN ? ELSE updated_hlc END,"
        "plain_text=?,rich_text_html=CASE WHEN plain_text IS NOT ? THEN NULL ELSE rich_text_html END,"
        "content_hash=?,category=?,updated_at=? WHERE id=?"));
    query.addBindValue(text);
    query.addBindValue(QVariant::fromValue<qint64>(qint64(HybridLogicalClock::shared().now())));
    query.addBindValue(text);
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
        "UPDATE content SET image_data=?,image_width=?,image_height=?,image_angle=0,content_hash=?,updated_at=?,"
        "updated_hlc=? WHERE id=?"));
    query.addBindValue(pngData);
    query.addBindValue(image.width());
    query.addBindValue(image.height());
    query.addBindValue(contentHash(hashItem));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(QVariant::fromValue<qint64>(qint64(HybridLogicalClock::shared().now())));
    query.addBindValue(id);
    return query.exec();
}

bool HistoryRepository::updateImageAngle(qint64 id, int angle) {
    angle = ((angle % 360) + 360) % 360;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "UPDATE content SET updated_hlc=CASE WHEN image_angle IS NOT ? THEN ? ELSE updated_hlc END,"
        "image_angle=?,updated_at=? WHERE id=?"));
    query.addBindValue(angle);
    query.addBindValue(QVariant::fromValue<qint64>(qint64(HybridLogicalClock::shared().now())));
    query.addBindValue(angle);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(id);
    return query.exec();
}

bool HistoryRepository::updateLinkMetadata(qint64 id, const QString &title,
                                           const QByteArray &iconData,
                                           const QByteArray &previewData) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "UPDATE content SET title=CASE WHEN title='文本' OR title='链接' THEN ? ELSE title END,"
        "link_icon_data=CASE WHEN length(?)>0 THEN ? ELSE link_icon_data END,"
        "link_preview_data=CASE WHEN length(?)>0 THEN ? ELSE link_preview_data END,"
        "link_preview_fetched=1,updated_at=? WHERE id=?"));
    query.addBindValue(title.trimmed().isEmpty() ? QStringLiteral("链接") : title.trimmed());
    query.addBindValue(iconData);
    query.addBindValue(iconData);
    query.addBindValue(previewData);
    query.addBindValue(previewData);
    query.addBindValue(QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(id);
    if (query.exec()) return true;
    lastError_ = query.lastError().text();
    return false;
}

bool HistoryRepository::touch(qint64 id) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "UPDATE content SET last_copied_at=MAX(last_copied_at,?),copy_count=copy_count+1,updated_at=?,"
        "updated_hlc=? WHERE id=?"));
    query.addBindValue(now);
    query.addBindValue(now);
    query.addBindValue(QVariant::fromValue<qint64>(qint64(HybridLogicalClock::shared().now())));
    query.addBindValue(id);
    if (query.exec()) return query.numRowsAffected() > 0;
    lastError_ = query.lastError().text();
    return false;
}

bool HistoryRepository::remove(qint64 id) {
    if (!database_.transaction()) return false;
    QSqlQuery remember(database_);
    remember.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO sync_deleted(sync_id,deleted_at) "
        "SELECT sync_id,? FROM content WHERE id=? AND sync_id IS NOT NULL"));
    remember.addBindValue(QDateTime::currentMSecsSinceEpoch());
    remember.addBindValue(id);
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("DELETE FROM content WHERE id=?"));
    query.addBindValue(id);
    if (!remember.exec() || !query.exec()) {
        lastError_ = remember.lastError().isValid() ? remember.lastError().text() : query.lastError().text();
        database_.rollback();
        return false;
    }
    return database_.commit();
}

QVector<ClipboardItem> HistoryRepository::syncChanges(quint64 afterHlc, qint64 windowMs, int limit) const {
    QVector<ClipboardItem> result;
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "SELECT content.id,content.title,content.content_type,content.plain_text,content.rich_text_html,"
        "content.link_icon_data,content.link_preview_data,content.link_preview_fetched,"
        "content.image_data,content.image_width,content.image_height,"
        "content.source_id,content.source_name,NULL,NULL,content.category,content.last_copied_at,content.copy_count,"
        "NULL,NULL,content.image_angle,content.files_json,"
        "content.sync_id,content.origin_device,content.updated_hlc FROM content "
        "WHERE content.content_type IN ('text','image') AND content.updated_hlc>? AND content.last_copied_at>=? "
        "ORDER BY content.updated_hlc ASC LIMIT ?"));
    query.addBindValue(QVariant::fromValue<qint64>(qint64(afterHlc)));
    query.addBindValue(QDateTime::currentMSecsSinceEpoch() - windowMs);
    query.addBindValue(std::max(1, limit));
    if (!query.exec()) {
        lastError_ = query.lastError().text();
        return result;
    }
    while (query.next()) result.append(itemFromQuery(query, false));
    return result;
}

HistoryRepository::ImportResult HistoryRepository::importRemoteItem(const ClipboardItem &remote) {
    if (remote.syncID.isEmpty() || remote.isFiles()) return ImportResult::Failed;
    if (wasDeleted(remote.syncID)) return ImportResult::Skipped;
    HybridLogicalClock::shared().receive(remote.updatedHlc);
    const qint64 copiedAt = milliseconds(remote.copiedAt.isValid() ? remote.copiedAt : QDateTime::currentDateTime());

    QSqlQuery existing(database_);
    existing.prepare(QStringLiteral("SELECT id,updated_hlc FROM content WHERE sync_id=?"));
    existing.addBindValue(remote.syncID);
    if (existing.exec() && existing.next()) {
        const qint64 id = existing.value(0).toLongLong();
        if (remote.updatedHlc <= quint64(existing.value(1).toLongLong())) return ImportResult::Skipped;
        existing.finish();
        // Last writer wins for the fields that sync: content, title, rotation.
        ClipboardItem hashItem = remote;
        QSqlQuery update(database_);
        update.prepare(QStringLiteral(
            "UPDATE content SET title=?,plain_text=?,rich_text_html=?,image_data=?,image_width=?,image_height=?,"
            "image_angle=?,content_hash=?,category=?,last_copied_at=MAX(last_copied_at,?),updated_hlc=?,updated_at=? "
            "WHERE id=?"));
        update.addBindValue(remote.title);
        update.addBindValue(remote.isText() ? QVariant(remote.text) : QVariant());
        update.addBindValue(remote.isText() && !remote.richTextHtml.isEmpty() ? QVariant(remote.richTextHtml) : QVariant());
        update.addBindValue(remote.isImage() ? QVariant(remote.imageData) : QVariant());
        update.addBindValue(remote.isImage() ? QVariant(remote.image.width()) : QVariant());
        update.addBindValue(remote.isImage() ? QVariant(remote.image.height()) : QVariant());
        update.addBindValue(((remote.imageAngle % 360) + 360) % 360);
        update.addBindValue(contentHash(hashItem));
        update.addBindValue(remote.isText() ? classifyText(remote.text) : QString());
        update.addBindValue(copiedAt);
        update.addBindValue(QVariant::fromValue<qint64>(qint64(remote.updatedHlc)));
        update.addBindValue(QDateTime::currentMSecsSinceEpoch());
        update.addBindValue(id);
        if (!update.exec()) {
            lastError_ = update.lastError().text();
            return ImportResult::Failed;
        }
        return ImportResult::Updated;
    }
    existing.finish();

    // The same content copied on both devices: one card, the local identity.
    QSqlQuery sameContent(database_);
    sameContent.prepare(QStringLiteral(
        "SELECT id,updated_hlc FROM content WHERE content_hash=? ORDER BY last_copied_at DESC LIMIT 1"));
    sameContent.addBindValue(contentHash(remote));
    if (sameContent.exec() && sameContent.next()) {
        const qint64 id = sameContent.value(0).toLongLong();
        const quint64 hlc = std::max(remote.updatedHlc, quint64(sameContent.value(1).toLongLong()));
        sameContent.finish();
        QSqlQuery merge(database_);
        merge.prepare(QStringLiteral(
            "UPDATE content SET last_copied_at=MAX(last_copied_at,?),updated_hlc=?,updated_at=? WHERE id=?"));
        merge.addBindValue(copiedAt);
        merge.addBindValue(QVariant::fromValue<qint64>(qint64(hlc)));
        merge.addBindValue(QDateTime::currentMSecsSinceEpoch());
        merge.addBindValue(id);
        return merge.exec() ? ImportResult::Merged : ImportResult::Failed;
    }
    sameContent.finish();

    QSqlQuery insert(database_);
    insert.prepare(QStringLiteral(
        "INSERT INTO content(title,content_type,plain_text,rich_text_html,image_data,image_width,image_height,"
        "image_angle,content_hash,source_id,source_name,category,copied_at,last_copied_at,copy_count,updated_at,"
        "sync_id,origin_device,updated_hlc) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,1,?,?,?,?)"));
    insert.addBindValue(remote.title.isEmpty() ? (remote.isImage() ? QStringLiteral("图片") : QStringLiteral("文本"))
                                               : remote.title);
    insert.addBindValue(remote.isImage() ? QStringLiteral("image") : QStringLiteral("text"));
    insert.addBindValue(remote.isText() ? QVariant(remote.text) : QVariant());
    insert.addBindValue(remote.isText() && !remote.richTextHtml.isEmpty() ? QVariant(remote.richTextHtml) : QVariant());
    insert.addBindValue(remote.isImage() ? QVariant(remote.imageData) : QVariant());
    insert.addBindValue(remote.isImage() ? QVariant(remote.image.width()) : QVariant());
    insert.addBindValue(remote.isImage() ? QVariant(remote.image.height()) : QVariant());
    insert.addBindValue(((remote.imageAngle % 360) + 360) % 360);
    insert.addBindValue(contentHash(remote));
    insert.addBindValue(QStringLiteral("clipdeck-sync:") + remote.originDevice);
    insert.addBindValue(remote.sourceName.isEmpty() ? QStringLiteral("ClipDeck") : remote.sourceName);
    insert.addBindValue(remote.isText() ? classifyText(remote.text) : QString());
    insert.addBindValue(copiedAt);
    insert.addBindValue(copiedAt);
    insert.addBindValue(QDateTime::currentMSecsSinceEpoch());
    insert.addBindValue(remote.syncID);
    insert.addBindValue(remote.originDevice.isEmpty() ? QVariant() : QVariant(remote.originDevice));
    insert.addBindValue(QVariant::fromValue<qint64>(qint64(remote.updatedHlc)));
    if (!insert.exec()) {
        lastError_ = insert.lastError().text();
        return ImportResult::Failed;
    }
    return ImportResult::Inserted;
}

QVector<SyncPeer> HistoryRepository::syncPeers() const {
    QVector<SyncPeer> peers;
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral(
            "SELECT device_id,hostname,public_key,addresses,last_seen,sent_cursor FROM sync_peer ORDER BY hostname"))) {
        lastError_ = query.lastError().text();
        return peers;
    }
    while (query.next()) {
        SyncPeer peer;
        peer.deviceID = query.value(0).toString();
        peer.hostname = query.value(1).toString();
        peer.publicKey = query.value(2).toByteArray();
        // Qt 5.11 (UOS) has no Qt::SkipEmptyParts: drop empty entries by hand.
        for (const QString &address : query.value(3).toString().split(QLatin1Char(',')))
            if (!address.isEmpty()) peer.addresses.append(address);
        peer.lastSeenMs = query.value(4).toLongLong();
        peer.sentCursor = quint64(query.value(5).toLongLong());
        peers.append(peer);
    }
    return peers;
}

bool HistoryRepository::saveSyncPeer(const SyncPeer &peer) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "INSERT INTO sync_peer(device_id,hostname,public_key,addresses,last_seen,sent_cursor) VALUES(?,?,?,?,?,?) "
        "ON CONFLICT(device_id) DO UPDATE SET hostname=excluded.hostname,public_key=excluded.public_key,"
        "addresses=excluded.addresses,last_seen=excluded.last_seen,sent_cursor=excluded.sent_cursor"));
    query.addBindValue(peer.deviceID);
    query.addBindValue(peer.hostname);
    query.addBindValue(peer.publicKey);
    query.addBindValue(peer.addresses.join(QLatin1Char(',')));
    query.addBindValue(peer.lastSeenMs);
    query.addBindValue(QVariant::fromValue<qint64>(qint64(peer.sentCursor)));
    if (query.exec()) return true;
    lastError_ = query.lastError().text();
    return false;
}

bool HistoryRepository::removeSyncPeer(const QString &deviceID) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("DELETE FROM sync_peer WHERE device_id=?"));
    query.addBindValue(deviceID);
    return query.exec();
}

bool HistoryRepository::wasDeleted(const QString &syncID) const {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("SELECT 1 FROM sync_deleted WHERE sync_id=?"));
    query.addBindValue(syncID);
    return query.exec() && query.next();
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
    // Expired items are older than the 24-hour sync window, so they need no
    // deletion record; this is just a good moment to drop old records.
    pruneDeletedRecords();
    return query.numRowsAffected();
}

int HistoryRepository::clearExpiredAndReclaim(int retentionDays) {
    const int removed = clearExpired(retentionDays);
    execute(QStringLiteral("PRAGMA wal_checkpoint(TRUNCATE)"));
    execute(QStringLiteral("VACUUM"));
    return removed;
}

int HistoryRepository::clearAll() {
    // Clearing is a local deletion too: remember what was there, so peers do
    // not refill the history from the last 24 hours.
    QSqlQuery remember(database_);
    remember.prepare(QStringLiteral(
        "INSERT OR REPLACE INTO sync_deleted(sync_id,deleted_at) "
        "SELECT sync_id,? FROM content WHERE sync_id IS NOT NULL"));
    remember.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!remember.exec()) lastError_ = remember.lastError().text();
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

QVector<ClipboardCategory> HistoryRepository::categories() const {
    QVector<ClipboardCategory> result;
    QSqlQuery query(database_);
    if (!query.exec(QStringLiteral("SELECT id,name,icon,script,preview_language,sort_order FROM category ORDER BY sort_order,id"))) {
        lastError_ = query.lastError().text();
        return result;
    }
    while (query.next()) {
        ClipboardCategory category;
        category.databaseID = query.value(0).toLongLong();
        category.name = query.value(1).toString();
        category.icon = query.value(2).toString();
        category.script = query.value(3).toString();
        category.previewLanguage = query.value(4).toString();
        category.sortOrder = query.value(5).toInt();
        result.append(category);
    }
    return result;
}

bool HistoryRepository::createCategory(const QString &name, const QString &icon,
                                       const QString &script, const QString &previewLanguage) {
    QSqlQuery query(database_);
    query.prepare(QStringLiteral(
        "INSERT INTO category(name,icon,script,preview_language,sort_order) "
        "VALUES(?,?,?,?,COALESCE((SELECT MAX(sort_order)+1 FROM category),0))"));
    query.addBindValue(name.trimmed());
    query.addBindValue(icon);
    query.addBindValue(script);
    query.addBindValue(previewLanguage);
    if (query.exec()) return true;
    lastError_ = query.lastError().text();
    return false;
}

bool HistoryRepository::updateCategory(qint64 id, const QString &name, const QString &icon,
                                       const QString &script, const QString &previewLanguage) {
    QSqlQuery old(database_);
    old.prepare(QStringLiteral("SELECT name FROM category WHERE id=?"));
    old.addBindValue(id);
    if (!old.exec() || !old.next()) return false;
    const QString oldName = old.value(0).toString();
    if (!database_.transaction()) return false;
    QSqlQuery update(database_);
    update.prepare(QStringLiteral("UPDATE category SET name=?,icon=?,script=?,preview_language=? WHERE id=?"));
    update.addBindValue(name.trimmed());
    update.addBindValue(icon);
    update.addBindValue(script);
    update.addBindValue(previewLanguage);
    update.addBindValue(id);
    QSqlQuery content(database_);
    content.prepare(QStringLiteral("UPDATE content SET category=? WHERE category=?"));
    content.addBindValue(name.trimmed());
    content.addBindValue(oldName);
    if (update.exec() && content.exec() && database_.commit()) return true;
    database_.rollback();
    lastError_ = update.lastError().text();
    return false;
}

bool HistoryRepository::removeCategory(qint64 id) {
    QSqlQuery old(database_);
    old.prepare(QStringLiteral("SELECT name FROM category WHERE id=?"));
    old.addBindValue(id);
    if (!old.exec() || !old.next() || !database_.transaction()) return false;
    QSqlQuery clear(database_);
    clear.prepare(QStringLiteral("UPDATE content SET category=NULL WHERE category=?"));
    clear.addBindValue(old.value(0).toString());
    QSqlQuery query(database_);
    query.prepare(QStringLiteral("DELETE FROM category WHERE id=?"));
    query.addBindValue(id);
    if (clear.exec() && query.exec() && database_.commit()) return true;
    database_.rollback();
    lastError_ = query.lastError().text();
    return false;
}

bool HistoryRepository::reorderCategories(const QVector<qint64> &ids) {
    if (!database_.transaction()) return false;
    for (int index = 0; index < ids.size(); ++index) {
        QSqlQuery query(database_);
        query.prepare(QStringLiteral("UPDATE category SET sort_order=? WHERE id=?"));
        query.addBindValue(index);
        query.addBindValue(ids[index]);
        if (!query.exec()) {
            lastError_ = query.lastError().text();
            database_.rollback();
            return false;
        }
    }
    return database_.commit();
}

bool HistoryRepository::restoreDefaultCategories() {
    if (!database_.transaction()) return false;
    int nextOrder = 0;
    QSqlQuery maximum(database_);
    if (maximum.exec(QStringLiteral("SELECT COALESCE(MAX(sort_order)+1,0) FROM category"))
        && maximum.next()) {
        nextOrder = maximum.value(0).toInt();
    }
    for (const CategoryPreset &preset : defaultCategoryPresets()) {
        QSqlQuery existing(database_);
        existing.prepare(QStringLiteral("SELECT id FROM category WHERE name=? LIMIT 1"));
        existing.addBindValue(preset.name);
        if (!existing.exec()) {
            lastError_ = existing.lastError().text();
            database_.rollback();
            return false;
        }
        QSqlQuery query(database_);
        if (existing.next()) {
            query.prepare(QStringLiteral(
                "UPDATE category SET icon=?,script=?,preview_language=? WHERE id=?"));
            query.addBindValue(preset.icon);
            query.addBindValue(preset.script);
            query.addBindValue(preset.language);
            query.addBindValue(existing.value(0).toLongLong());
        } else {
            query.prepare(QStringLiteral(
                "INSERT INTO category(name,icon,script,preview_language,sort_order) VALUES(?,?,?,?,?)"));
            query.addBindValue(preset.name);
            query.addBindValue(preset.icon);
            query.addBindValue(preset.script);
            query.addBindValue(preset.language);
            query.addBindValue(nextOrder++);
        }
        if (!query.exec()) {
            lastError_ = query.lastError().text();
            database_.rollback();
            return false;
        }
    }
    return database_.commit();
}

QString HistoryRepository::classifyText(const QString &text, const QString &sourceID,
                                        const QString &sourceName,
                                        const QDateTime &copiedAt) const {
    for (const ClipboardCategory &category : categories()) {
        QJSEngine engine;
        const QJSValue evaluation = engine.evaluate(category.script, category.name);
        if (evaluation.isError()) continue;
        QJSValue classify = engine.globalObject().property(QStringLiteral("classify"));
        if (!classify.isCallable()) continue;
        QJSValue item = engine.newObject();
        item.setProperty(QStringLiteral("text"), text);
        item.setProperty(QStringLiteral("textLength"), int(text.size()));
        QJSValue application = engine.newObject();
        application.setProperty(QStringLiteral("bundleId"), sourceID);
        application.setProperty(QStringLiteral("name"), sourceName);
        item.setProperty(QStringLiteral("sourceApplication"), application);
        item.setProperty(QStringLiteral("copiedAt"),
                         static_cast<double>((copiedAt.isValid() ? copiedAt
                             : QDateTime::currentDateTime()).toMSecsSinceEpoch()));
        if (category.previewLanguage.compare(QStringLiteral("code"), Qt::CaseInsensitive) == 0) {
            int score = 0;
            const QString language = detectedCodeLanguage(text, &score);
            item.setProperty(QStringLiteral("codeEngineMatched"), !language.isEmpty());
            item.setProperty(QStringLiteral("detectedLanguage"), language);
            item.setProperty(QStringLiteral("codeScore"), score);
        }
        const QJSValue result = classify.call(QJSValueList{item});
        if (!result.isError() && result.isBool() && result.toBool()) return category.name;
    }
    return QString();
}

QString HistoryRepository::categoryScriptError(const QString &script) const {
    QJSEngine engine;
    const QJSValue evaluation = engine.evaluate(script, QStringLiteral("category-script"));
    if (evaluation.isError()) {
        return QStringLiteral("第 %1 行：%2")
            .arg(evaluation.property(QStringLiteral("lineNumber")).toInt())
            .arg(evaluation.toString());
    }
    if (!engine.globalObject().property(QStringLiteral("classify")).isCallable()) {
        return QStringLiteral("脚本必须定义 classify(item) 函数");
    }
    return QString();
}

bool HistoryRepository::categoryScriptMatches(const QString &script, const QString &text,
                                              const QString &previewLanguage,
                                              QString *error,
                                              QStringList *consoleOutput) const {
    if (error) error->clear();
    if (consoleOutput) consoleOutput->clear();
    QJSEngine engine;
    engine.evaluate(QStringLiteral(
        "var __clipDeckConsole=[];"
        "function __clipDeckConsoleText(value){"
        " if(typeof value==='string') return value;"
        " try { var json=JSON.stringify(value); if(json!==undefined) return json; } catch (_) {}"
        " return String(value);"
        "}"
        "function __clipDeckConsolePush(level,args){"
        " var values=[]; for(var i=0;i<args.length;i++) values.push(__clipDeckConsoleText(args[i]));"
        " __clipDeckConsole.push(level+'  '+values.join(' '));"
        "}"
        "var console={"
        " log:function(){__clipDeckConsolePush('LOG',arguments);},"
        " info:function(){__clipDeckConsolePush('INFO',arguments);},"
        " warn:function(){__clipDeckConsolePush('WARN',arguments);},"
        " error:function(){__clipDeckConsolePush('ERROR',arguments);},"
        " debug:function(){__clipDeckConsolePush('DEBUG',arguments);}"
        "};"));
    const auto collectConsole = [&] {
        if (!consoleOutput) return;
        const QJSValue entries = engine.globalObject().property(QStringLiteral("__clipDeckConsole"));
        const int length = entries.property(QStringLiteral("length")).toInt();
        for (int index = 0; index < length; ++index) {
            consoleOutput->append(entries.property(index).toString());
        }
    };
    const QJSValue evaluation = engine.evaluate(script, QStringLiteral("category-script"));
    if (evaluation.isError()) {
        collectConsole();
        if (error) {
            *error = QStringLiteral("第 %1 行：%2")
                .arg(evaluation.property(QStringLiteral("lineNumber")).toInt())
                .arg(evaluation.toString());
        }
        return false;
    }
    QJSValue classify = engine.globalObject().property(QStringLiteral("classify"));
    if (!classify.isCallable()) {
        collectConsole();
        if (error) *error = QStringLiteral("脚本必须定义 classify(item) 函数");
        return false;
    }
    QJSValue item = engine.newObject();
    item.setProperty(QStringLiteral("text"), text);
    item.setProperty(QStringLiteral("textLength"), int(text.size()));
    QJSValue application = engine.newObject();
    application.setProperty(QStringLiteral("bundleId"), QStringLiteral("app.clipdeck.script-test"));
    application.setProperty(QStringLiteral("name"), QStringLiteral("脚本测试"));
    item.setProperty(QStringLiteral("sourceApplication"), application);
    item.setProperty(QStringLiteral("copiedAt"),
                     static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
    if (previewLanguage.compare(QStringLiteral("code"), Qt::CaseInsensitive) == 0) {
        int score = 0;
        const QString language = detectedCodeLanguage(text, &score);
        item.setProperty(QStringLiteral("codeEngineMatched"), !language.isEmpty());
        item.setProperty(QStringLiteral("detectedLanguage"), language);
        item.setProperty(QStringLiteral("codeScore"), score);
    }
    const QJSValue result = classify.call(QJSValueList{item});
    collectConsole();
    if (result.isError()) {
        if (error) *error = result.toString();
        return false;
    }
    return result.toBool();
}

QString HistoryRepository::detectedCodeLanguage(const QString &source, int *score) {
    if (score) *score = 0;
    // This is a heuristic, not the stored payload or the input to user JS.
    // Bound regex work so a megabyte-long clipboard line cannot monopolize
    // the capture worker while newer cuts wait behind it.
    const QString text = source.left(32768).trimmed();
    if (text.size() < 4) return QString();
    if (!text.contains(QLatin1Char('\n')) && text.size() >= 24) {
        const int structural = text.count(QRegularExpression(QStringLiteral("[{}\\[\\]();=<>$#@]")));
        const int words = text.split(QRegularExpression(QStringLiteral("\\s+")), QT_STRING_SKIP_EMPTY).size();
        if (words >= 5 && structural == 0) return QString();
    }
    struct Signal { QString pattern; int weight; };
    struct Rule { QString language; int minimum; QVector<Signal> signals; };
    static const QVector<Rule> rules = {
        {QStringLiteral("swift"), 5, {
            {QStringLiteral("\\bimport\\s+(Foundation|SwiftUI|AppKit|UIKit|Combine|PackageDescription)\\b"), 7},
            {QStringLiteral("(^|\\n)\\s*@(?:State|Binding|ObservedObject|StateObject|Environment|MainActor|ViewBuilder)\\b"), 6},
            {QStringLiteral("\\b(?:protocol|extension|actor|struct|enum)\\s+[A-Za-z_]\\w*"), 5},
            {QStringLiteral("\\bfunc\\s+[A-Za-z_]\\w*\\s*\\([^)]*:[^)]*\\)"), 6},
            {QStringLiteral("\\bvar\\s+body\\s*:\\s*some\\s+View\\b"), 7},
            {QStringLiteral("\\b(?:guard|if)\\s+let\\s+\\w+[^\\n{]*\\{"), 5}
        }},
        {QStringLiteral("objectivec"), 6, {
            {QStringLiteral("@(?:interface|implementation|protocol)\\s+\\w+"), 8},
            {QStringLiteral("#import\\s+[<\"](?:Foundation|UIKit|AppKit)/"), 8}
        }},
        {QStringLiteral("java"), 6, {
            {QStringLiteral("\\b(?:public|protected|private)\\s+(?:abstract\\s+|final\\s+)?(?:class|interface|enum|record)\\s+\\w+[^\\n{]*\\{"), 8},
            {QStringLiteral("\\bpackage\\s+[A-Za-z_]\\w*(?:\\.[A-Za-z_]\\w*)*\\s*;"), 7},
            {QStringLiteral("\\bSystem\\.(?:out|err)\\.\\w+\\s*\\("), 7}
        }},
        {QStringLiteral("kotlin"), 6, {
            {QStringLiteral("\\bfun\\s+\\w+\\s*\\([^)]*\\)\\s*(?::\\s*[\\w<>,?.]+)?\\s*[={]"), 7},
            {QStringLiteral("\\b(?:data|sealed|enum)\\s+class\\s+\\w+"), 7},
            {QStringLiteral("\\bcompanion\\s+object\\b|\\bwhen\\s*\\([^)]*\\)\\s*\\{"), 6}
        }},
        {QStringLiteral("python"), 6, {
            {QStringLiteral("(^|\\n)\\s*(?:async\\s+)?def\\s+\\w+\\s*\\([^)]*\\)\\s*(?:->[^:]+)?\\s*:"), 8},
            {QStringLiteral("(^|\\n)\\s*class\\s+\\w+(?:\\([^)]*\\))?\\s*:"), 8},
            {QStringLiteral("(^|\\n)\\s*from\\s+[\\w.]+\\s+import\\s+"), 6},
            {QStringLiteral("if\\s+__name__\\s*==\\s*['\"]__main__['\"]\\s*:"), 8}
        }},
        {QStringLiteral("typescript"), 6, {
            {QStringLiteral("\\binterface\\s+\\w+(?:\\s+extends\\s+\\w+)?\\s*\\{"), 7},
            {QStringLiteral("\\btype\\s+\\w+(?:<[^>]+>)?\\s*="), 7},
            {QStringLiteral("\\b(?:const|let|var)\\s+\\w+\\s*:\\s*(?:[A-Za-z_]|\\{|\\[)"), 6}
        }},
        {QStringLiteral("javascript"), 5, {
            {QStringLiteral("\\b(?:async\\s+)?function\\s*\\w*\\s*\\([^)]*\\)\\s*\\{"), 7},
            {QStringLiteral("\\b(?:const|let|var)\\s+[A-Za-z_$]\\w*\\s*="), 5},
            {QStringLiteral("(?:\\([^)]*\\)|[A-Za-z_$]\\w*)\\s*=>"), 6},
            {QStringLiteral("\\b(?:document|window|console)\\.\\w+\\s*\\("), 6}
        }},
        {QStringLiteral("cpp"), 6, {
            {QStringLiteral("^\\s*#include\\s*[<\"][^>\"]+[>\"]"), 8},
            {QStringLiteral("\\bstd::\\w+|\\b(?:cout|cin)\\s*(?:<<|>> )"), 7},
            {QStringLiteral("\\btemplate\\s*<[^>]+>"), 7}
        }},
        {QStringLiteral("csharp"), 6, {
            {QStringLiteral("\\busing\\s+System(?:\\.\\w+)*\\s*;"), 8},
            {QStringLiteral("\\bConsole\\.Write(?:Line)?\\s*\\("), 7}
        }},
        {QStringLiteral("go"), 6, {
            {QStringLiteral("^\\s*package\\s+\\w+\\s*$"), 7},
            {QStringLiteral("\\bfunc\\s+(?:\\([^)]*\\)\\s*)?\\w+\\s*\\([^)]*\\)"), 7}
        }},
        {QStringLiteral("rust"), 6, {
            {QStringLiteral("\\bfn\\s+\\w+\\s*\\([^)]*\\)\\s*(?:->[^\\{]+)?\\{"), 8},
            {QStringLiteral("\\bimpl(?:<[^>]+>)?\\s+\\w+"), 7},
            {QStringLiteral("\\blet\\s+mut\\s+\\w+"), 6}
        }},
        {QStringLiteral("php"), 6, {
            {QStringLiteral("<\\?php\\b"), 9},
            {QStringLiteral("\\$[A-Za-z_]\\w*\\s*(?:=|->)"), 6}
        }},
        {QStringLiteral("ruby"), 6, {
            {QStringLiteral("(^|\\n)\\s*require(?:_relative)?\\s+['\"][^'\"]+['\"]"), 6},
            {QStringLiteral("(^|\\n)\\s*(?:class|def)\\s+\\w+"), 8}
        }},
        {QStringLiteral("bash"), 6, {
            {QStringLiteral("^\\s*#!\\s*/(?:usr/bin/env\\s+)?(?:ba|z|k)?sh\\b"), 9},
            {QStringLiteral("(^|\\n)\\s*(?:sudo|git|curl|wget|ssh|scp|rsync|brew|npm|yarn|pnpm|docker|kubectl)\\s+[-\\w]"), 6},
            {QStringLiteral("\\$\\{[A-Za-z_]\\w*(?::-[^}]*)?\\}"), 6}
        }},
        {QStringLiteral("xml"), 6, {
            {QStringLiteral("<!DOCTYPE\\s+html\\b|<\\?xml\\s+version="), 9},
            {QStringLiteral("<(?:html|head|body|script|style|div|span|template|svg)\\b[^>]*>"), 6}
        }},
        {QStringLiteral("css"), 6, {
            {QStringLiteral("(?:^|\\n)\\s*(?:[.#][\\w-]+|[a-z][\\w-]*)\\s*\\{[^}]*[\\w-]+\\s*:[^;}]+"), 8},
            {QStringLiteral("@(?:media|supports|keyframes|font-face)\\b[^\\{]*\\{"), 7}
        }}
    };
    const auto options = QRegularExpression::CaseInsensitiveOption
        | QRegularExpression::MultilineOption | QRegularExpression::DotMatchesEverythingOption;
    for (const Rule &rule : rules) {
        int total = 0;
        for (const Signal &signal : rule.signals) {
            if (QRegularExpression(signal.pattern, options).match(text).hasMatch()) total += signal.weight;
        }
        if (total >= rule.minimum) {
            if (score) *score = total;
            return rule.language;
        }
    }
    return QString();
}

QString HistoryRepository::classifyBuiltinText(const QString &input) {
    const QString text = input.trimmed();
    static const QRegularExpression url(QStringLiteral("^https?://\\S+$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression sql(QStringLiteral("^(select|insert|update|delete|create|alter|drop)\\b"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression markdown(QStringLiteral("(^|\\n)(#{1,6} |[-*] |```)") );
    static const QRegularExpression color(QStringLiteral("^(#(?:[0-9a-f]{3}|[0-9a-f]{4}|[0-9a-f]{6}|[0-9a-f]{8})|(?=[0-9a-f]*[a-f])(?:[0-9a-f]{6}|[0-9a-f]{8})|rgba?\\(.*\\))$"), QRegularExpression::CaseInsensitiveOption);
    if (url.match(text).hasMatch()) return QStringLiteral("URL");
    if ((text.startsWith(QLatin1Char('{')) && text.endsWith(QLatin1Char('}')))
        || (text.startsWith(QLatin1Char('[')) && text.endsWith(QLatin1Char(']')))) {
        return QStringLiteral("JSON");
    }
    if (sql.match(text).hasMatch()) return QStringLiteral("SQL");
    if (markdown.match(text).hasMatch()) return QStringLiteral("Markdown");
    if (color.match(text).hasMatch()) return QStringLiteral("色彩");
    if (!detectedCodeLanguage(text).isEmpty()) return QStringLiteral("代码");
    return QString();
}

QStringList HistoryRepository::presetCategories() {
    return {QStringLiteral("JSON"), QStringLiteral("URL"), QStringLiteral("SQL"),
            QStringLiteral("Markdown"), QStringLiteral("代码"), QStringLiteral("色彩")};
}

QStringList HistoryRepository::categoryColorPalette() {
    return {QStringLiteral("#BF5AF2"), QStringLiteral("#64D2FF"), QStringLiteral("#30D158"),
            QStringLiteral("#FF375F"), QStringLiteral("#5E5CE6"), QStringLiteral("#66D4CF")};
}

QString HistoryRepository::categoryColor(const QString &icon, int position) {
    static const QRegularExpression hex(QStringLiteral("^#[0-9A-Fa-f]{6}$"));
    if (hex.match(icon).hasMatch()) return icon.toUpper();
    const QStringList palette = categoryColorPalette();
    return palette.at(std::abs(position) % palette.size());
}

QVector<HistoryRepository::DisplayChoice> HistoryRepository::cardTypeChoices() {
    return {
        {QStringLiteral("text"), QStringLiteral("纯文本"), QStringLiteral("按原样显示文字")},
        {QStringLiteral("code"), QStringLiteral("代码"), QStringLiteral("自动识别语言并着色")},
        {QStringLiteral("url"), QStringLiteral("链接"), QStringLiteral("显示网页标题和缩略图")},
        {QStringLiteral("color"), QStringLiteral("色彩"), QStringLiteral("显示为对应颜色的色块")},
    };
}

QString HistoryRepository::cardTypeOf(const QString &previewLanguage) {
    const QString value = previewLanguage.trimmed().toLower();
    if (value.isEmpty() || value == QLatin1String("text")) return QStringLiteral("text");
    if (value == QLatin1String("url") || value == QLatin1String("color")) return value;
    return QStringLiteral("code");
}

QString HistoryRepository::previewLanguageForCardType(const QString &cardType, const QString &current) {
    if (cardType == QLatin1String("code"))
        return cardTypeOf(current) == QLatin1String("code") ? current : QStringLiteral("code");
    if (cardType == QLatin1String("url") || cardType == QLatin1String("color")) return cardType;
    return QStringLiteral("text");
}

QByteArray HistoryRepository::contentHash(const ClipboardItem &item) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(item.isFiles() ? QByteArrayLiteral("files:")
        : item.isImage() ? QByteArrayLiteral("image:") : QByteArrayLiteral("text:"));
    hash.addData(item.isFiles() ? FileReferences::identity(item.files)
        : item.isImage() ? item.imageData : item.text.toUtf8());
    return hash.result();
}

ClipboardItem HistoryRepository::itemFromQuery(const QSqlQuery &query, bool decodeImages) {
    ClipboardItem item;
    item.databaseID = query.value(0).toLongLong();
    item.title = query.value(1).toString();
    item.type = query.value(2).toString() == QStringLiteral("files") ? ClipboardPayloadType::Files
        : query.value(2).toString() == QStringLiteral("image")
        ? ClipboardPayloadType::Image : ClipboardPayloadType::Text;
    item.text = query.value(3).toString();
    item.richTextHtml = query.value(4).toString();
    item.linkIconData = query.value(5).toByteArray();
    item.linkPreviewData = query.value(6).toByteArray();
    item.hasFetchedLinkPreview = query.value(7).toBool();
    item.imageData = query.value(8).toByteArray();
    item.imagePixelSize = QSize(query.value(9).toInt(), query.value(10).toInt());
    item.textLength = item.text.size();
    if (item.isImage() && decodeImages) {
        item.image.loadFromData(item.imageData, "PNG");
    }
    item.sourceID = query.value(11).toString();
    item.sourceName = query.value(12).toString();
    item.sourceIconData = query.value(13).toByteArray();
    if (!item.sourceIconData.isEmpty()) item.sourceIcon.loadFromData(item.sourceIconData, "PNG");
    item.sourceIconVariantsData = query.value(14).toByteArray();
    item.category = query.value(15).toString();
    item.copiedAt = QDateTime::fromMSecsSinceEpoch(query.value(16).toLongLong());
    item.copyCount = query.value(17).toInt();
    item.previewLanguage = query.value(18).toString();
    item.categoryIcon = query.value(19).toString();
    item.imageAngle = ((query.value(20).toInt() % 360) + 360) % 360;
    if (item.isFiles()) {
        item.files = FileReferences::decode(query.value(21).toByteArray());
        item.text.clear();
    }
    item.syncID = query.value(22).toString();
    item.originDevice = query.value(23).toString();
    item.updatedHlc = quint64(query.value(24).toLongLong());
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
