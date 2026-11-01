// Runs the shared data-layer checks under Qt 6 on Windows.
#include "history/file_reference_tests.h"
#include "history_repository.h"
#include "history/hybrid_logical_clock.h"

#include <QCoreApplication>
#include <QThread>
#include <QApplication>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <cstdio>

namespace {
int failures = 0;
void check(bool condition, const char *name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

// The default categories run in their real order (JSON, URL, SQL, Markdown,
// 代码, 色彩), so each sample also checks that no earlier rule steals it.
// "!X" means only "not X": code or plain text are both fine there.
void defaultRuleTests(const HistoryRepository &repository) {
    const QList<QPair<QString, QString>> samples = {
        // JSON: data, JSON Lines; not placeholders, citations or code.
        {QStringLiteral("{\"name\": \"张三\", \"age\": 30}"), QStringLiteral("JSON")},
        {QStringLiteral("[1, 2, 3]"), QStringLiteral("JSON")},
        {QStringLiteral("{\"level\":\"info\"}\n{\"level\":\"warn\"}"), QStringLiteral("JSON")},
        {QStringLiteral("{}"), QStringLiteral("!JSON")},
        {QStringLiteral("[1]"), QStringLiteral("!JSON")},
        {QStringLiteral("[TODO] fix login"), QStringLiteral("!JSON")},
        // URL: one link with a real host.
        {QStringLiteral("https://example.com/a?b=1"), QStringLiteral("URL")},
        {QStringLiteral("http://localhost:8080"), QStringLiteral("URL")},
        {QStringLiteral("https://"), QStringLiteral("!URL")},
        {QStringLiteral("see https://example.com"), QStringLiteral("!URL")},
        // SQL: statements, including CTEs and leading comments; not prose.
        {QStringLiteral("SELECT * FROM users WHERE id = 1;"), QStringLiteral("SQL")},
        {QStringLiteral("select name from users"), QStringLiteral("SQL")},
        {QStringLiteral("-- latest\nSELECT id FROM orders LIMIT 10"), QStringLiteral("SQL")},
        {QStringLiteral("WITH r AS (SELECT * FROM t) SELECT * FROM r"), QStringLiteral("SQL")},
        {QStringLiteral("UPDATE users SET name = 'b' WHERE id = 2"), QStringLiteral("SQL")},
        {QStringLiteral("CREATE TABLE IF NOT EXISTS t (id int)"), QStringLiteral("SQL")},
        {QStringLiteral("SHOW TABLES"), QStringLiteral("SQL")},
        {QStringLiteral("Select the files you want to upload."), QStringLiteral("!SQL")},
        {QStringLiteral("Select all files from the folder and copy them"), QStringLiteral("!SQL")},
        {QStringLiteral("Update: the new release is out"), QStringLiteral("!SQL")},
        {QStringLiteral("Delete from the list"), QStringLiteral("!SQL")},
        {QStringLiteral("Create a new account"), QStringLiteral("!SQL")},
        {QStringLiteral("Drop me a line when you're free"), QStringLiteral("!SQL")},
        {QStringLiteral("Describe yourself"), QStringLiteral("!SQL")},
        // Markdown: one strong structure or two weaker ones.
        {QStringLiteral("## Install\nRun the script."), QStringLiteral("Markdown")},
        {QStringLiteral("| a | b |\n|---|---|\n| 1 | 2 |"), QStringLiteral("Markdown")},
        {QStringLiteral("See [the docs](https://example.com/docs)."), QStringLiteral("Markdown")},
        {QStringLiteral("- [ ] write tests\n- [x] ship"), QStringLiteral("Markdown")},
        {QStringLiteral("1. 打开设置\n2. 点击 **关于**"), QStringLiteral("Markdown")},
        {QStringLiteral("# install deps\nnpm install"), QStringLiteral("!Markdown")},
        {QStringLiteral("- a\n- b"), QStringLiteral("!Markdown")},
        {QStringLiteral("def __init__(self):\n    # set up\n    pass"), QStringLiteral("!Markdown")},
        // Colour is unchanged and still reached after the rules above.
        {QStringLiteral("#ff8800"), QStringLiteral("色彩")},
    };
    for (const auto &sample : samples) {
        const QString category = repository.classifyText(sample.first);
        const bool negative = sample.second.startsWith(QLatin1Char('!'));
        const bool ok = negative ? category != sample.second.mid(1) : category == sample.second;
        if (!ok) {
            std::printf("  %s -> '%s' (expected %s)\n", qPrintable(QString(sample.first).replace(QLatin1Char('\n'), QStringLiteral("\\n"))),
                        qPrintable(category), qPrintable(sample.second));
        }
        check(ok, qPrintable(QStringLiteral("default-rule %1 %2").arg(sample.second, sample.first.left(24).simplified())));
    }
}

// An untouched old default moves to the new rule on start; an edited rule stays.
void categoryColorTests() {
    const QStringList palette = HistoryRepository::categoryColorPalette();
    check(palette.size() == 6, "category-palette-has-six-colours");
    check(HistoryRepository::categoryColor(QStringLiteral("#30d158"), 0) == QStringLiteral("#30D158"),
          "stored-category-colour-wins");
    check(HistoryRepository::categoryColor(QStringLiteral("{}"), 7) == palette.at(1),
          "glyph-icon-falls-back-by-position");
    check(HistoryRepository::categoryColor(QString(), 0) == palette.at(0), "empty-icon-falls-back");
}

void cardTypeTests() {
    check(HistoryRepository::cardTypeOf(QString()) == QStringLiteral("text"), "empty-display-is-plain-text");
    check(HistoryRepository::cardTypeOf(QStringLiteral("url")) == QStringLiteral("url"), "url-display-is-link");
    check(HistoryRepository::cardTypeOf(QStringLiteral("json")) == QStringLiteral("code"), "json-display-is-code");
    check(HistoryRepository::previewLanguageForCardType(QStringLiteral("code"), QStringLiteral("sql")) == QStringLiteral("sql"),
          "switching-to-code-keeps-language");
    check(HistoryRepository::previewLanguageForCardType(QStringLiteral("code"), QStringLiteral("color")) == QStringLiteral("code"),
          "switching-to-code-detects-language");
    check(HistoryRepository::previewLanguageForCardType(QStringLiteral("text"), QStringLiteral("json")) == QStringLiteral("text"),
          "switching-to-text-drops-language");
}

void hybridClockTests() {
    qint64 wall = 1'000'000;
    HybridLogicalClock clock([&wall] { return wall; });
    const quint64 first = clock.now();
    check(HybridLogicalClock::milliseconds(first) == wall && HybridLogicalClock::counter(first) == 0,
          "hlc-reads-as-wall-time");
    const quint64 sameMillisecond = clock.now();
    check(sameMillisecond == first + 1, "hlc-counts-within-a-millisecond");
    wall -= 5000; // the system clock steps back
    check(clock.now() > sameMillisecond, "hlc-never-goes-backwards");
    const quint64 remote = HybridLogicalClock::make(wall + 60'000, 7); // a peer a minute ahead
    const quint64 received = clock.receive(remote);
    check(received > remote, "hlc-receive-moves-past-the-peer");
    check(clock.now() > received, "hlc-later-local-change-orders-after-the-peer");
    wall += 120'000;
    check(HybridLogicalClock::counter(clock.now()) == 0, "hlc-returns-to-wall-time-when-it-catches-up");
    HybridLogicalClock full([] { return qint64(5); });
    full.observe(HybridLogicalClock::make(5, 0xFFFF));
    const quint64 carried = full.now();
    check(HybridLogicalClock::milliseconds(carried) == 6 && HybridLogicalClock::counter(carried) == 0,
          "hlc-full-counter-carries-into-milliseconds");
}

void syncBookkeepingTests() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("sync.sqlite"));

    // A database from before sync: the original content table, two rows.
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("presync"));
        database.setDatabaseName(path);
        database.open();
        QSqlQuery query(database);
        query.exec(QStringLiteral(
            "CREATE TABLE content (id INTEGER PRIMARY KEY AUTOINCREMENT,title TEXT NOT NULL,content_type TEXT NOT NULL,"
            "plain_text TEXT,image_data BLOB,image_width INTEGER,image_height INTEGER,content_hash BLOB NOT NULL,"
            "source_id TEXT,source_name TEXT NOT NULL,category TEXT,copied_at INTEGER NOT NULL,"
            "last_copied_at INTEGER NOT NULL,copy_count INTEGER NOT NULL DEFAULT 1,updated_at INTEGER NOT NULL)"));
        query.exec(QStringLiteral(
            "INSERT INTO content(title,content_type,plain_text,content_hash,source_name,copied_at,last_copied_at,updated_at) "
            "VALUES('文本','text','older','h1','Notepad',1000,1000,1000),"
            "('文本','text','newer','h2','Notepad',2000,2000,2000)"));
        database.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("presync"));

    HistoryRepository repository(path);
    check(repository.isOpen(), "presync-database-opens");
    const QVector<ClipboardItem> migrated = repository.fetch();
    check(migrated.size() == 2 && !migrated[0].syncID.isEmpty() && !migrated[1].syncID.isEmpty()
              && migrated[0].syncID != migrated[1].syncID,
          "presync-rows-get-distinct-sync-ids");
    check(migrated.size() == 2 && HybridLogicalClock::milliseconds(migrated[0].updatedHlc) == 2000
              && HybridLogicalClock::milliseconds(migrated[1].updatedHlc) == 1000,
          "presync-rows-keep-their-order-in-the-clock");
    const QString keptID = migrated.isEmpty() ? QString() : migrated[0].syncID;
    {
        HistoryRepository reopened(path);
        check(reopened.fetch().value(0).syncID == keptID, "sync-ids-survive-reopening");
    }

    ClipboardItem item;
    item.text = QStringLiteral("sync me");
    item.sourceName = QStringLiteral("Notepad");
    item.copiedAt = QDateTime::currentDateTime();
    const ClipboardItem saved = repository.save(item);
    check(!saved.syncID.isEmpty() && saved.originDevice.isEmpty(), "new-item-gets-a-sync-id-from-this-device");
    check(saved.updatedHlc > migrated.value(0).updatedHlc, "new-item-orders-after-older-changes");

    const ClipboardItem copiedAgain = repository.save(item);
    check(copiedAgain.databaseID == saved.databaseID && copiedAgain.syncID == saved.syncID,
          "copying-again-keeps-the-sync-id");
    check(copiedAgain.updatedHlc > saved.updatedHlc, "copying-again-moves-the-clock");

    auto stored = [&repository](qint64 id) {
        return repository.fetch(QString(), QString(), 1, 0, id).value(0);
    };
    quint64 before = stored(saved.databaseID).updatedHlc;
    repository.updateTitle(saved.databaseID, QStringLiteral("renamed"));
    check(stored(saved.databaseID).updatedHlc > before, "rename-moves-the-clock");
    before = stored(saved.databaseID).updatedHlc;
    repository.updateText(saved.databaseID, QStringLiteral("edited"), QString());
    check(stored(saved.databaseID).updatedHlc > before, "edit-moves-the-clock");
    before = stored(saved.databaseID).updatedHlc;
    repository.updateLinkMetadata(saved.databaseID, QStringLiteral("Title"), QByteArray(), QByteArray());
    check(stored(saved.databaseID).updatedHlc == before, "link-metadata-does-not-sync");

    const ClipboardItem older = stored(migrated.value(1).databaseID);
    QThread::msleep(2); // rows saved above may share this millisecond and win the tie on id
    check(repository.touch(older.databaseID), "touch-finds-the-row");
    const ClipboardItem touched = stored(older.databaseID);
    check(touched.updatedHlc > before && touched.copiedAt > older.copiedAt
              && touched.copyCount == older.copyCount + 1,
          "touch-counts-as-copying-again");
    check(repository.fetch().value(0).databaseID == older.databaseID, "touched-item-moves-to-the-front");

    check(!repository.wasDeleted(touched.syncID), "live-item-is-not-deleted");
    check(repository.remove(touched.databaseID) && repository.wasDeleted(touched.syncID),
          "delete-remembers-the-sync-id");
    const QString lastID = stored(saved.databaseID).syncID;
    repository.clearAll();
    check(repository.wasDeleted(lastID), "clear-all-remembers-the-sync-ids");

    // Records older than the retention are dropped when the database opens.
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("aged"));
        database.setDatabaseName(path);
        database.open();
        QSqlQuery query(database);
        query.prepare(QStringLiteral("UPDATE sync_deleted SET deleted_at=? WHERE sync_id=?"));
        query.addBindValue(QDateTime::currentMSecsSinceEpoch() - HistoryRepository::kDeletedRecordRetentionMs - 1000);
        query.addBindValue(lastID);
        query.exec();
        database.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("aged"));
    HistoryRepository aged(path);
    check(!aged.wasDeleted(lastID) && aged.wasDeleted(touched.syncID), "old-deletion-records-are-pruned");
}

void upgradeTests() {
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("upgrade.sqlite"));
    { HistoryRepository seed(path); }
    const QString oldSql = QStringLiteral("function classify(item) {\n"
                                          "    return /^(select|insert|update|delete|create|alter|drop)\\b/i.test(\n"
                                          "        item.text.trim()\n"
                                          "    );\n"
                                          "}");
    const QString edited = QStringLiteral("function classify(item) { return item.text === 'mine'; }");
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("upgrade"));
        database.setDatabaseName(path);
        database.open();
        QSqlQuery query(database);
        query.prepare(QStringLiteral("UPDATE category SET script=? WHERE name=?"));
        query.addBindValue(oldSql);
        query.addBindValue(QStringLiteral("SQL"));
        query.exec();
        query.prepare(QStringLiteral("UPDATE category SET script=? WHERE name=?"));
        query.addBindValue(edited);
        query.addBindValue(QStringLiteral("Markdown"));
        query.exec();
        database.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("upgrade"));
    HistoryRepository reopened(path);
    QString sqlScript, markdownScript;
    for (const ClipboardCategory &category : reopened.categories()) {
        if (category.name == QStringLiteral("SQL")) sqlScript = category.script;
        if (category.name == QStringLiteral("Markdown")) markdownScript = category.script;
    }
    check(sqlScript != oldSql && sqlScript.contains(QStringLiteral("statements")), "old-default-rule-is-upgraded");
    check(markdownScript == edited, "edited-rule-is-kept");
    check(reopened.classifyText(QStringLiteral("Select the files you want to upload.")) != QStringLiteral("SQL"),
          "upgraded-rule-applies");
}
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ClipDeckTests"));
    QCoreApplication::setApplicationName(QStringLiteral("core-tests"));

    for (const QString &failure : fileReferenceSelfTestFailures()) {
        std::printf("FAIL file-reference: %s\n", qPrintable(failure));
        ++failures;
    }
    check(failures == 0, "file-reference-self-test");

    QTemporaryDir directory;
    HistoryRepository repository(directory.filePath(QStringLiteral("history.sqlite")));
    check(repository.isOpen(), "repository-opens");
    if (!repository.isOpen()) {
        std::printf("error: %s\n", qPrintable(repository.lastError()));
        return 1;
    }
    check(!repository.categories().isEmpty(), "default-categories-seeded");

    ClipboardItem item;
    item.text = QStringLiteral("https://example.com/path");
    item.title = item.text;
    item.sourceID = QStringLiteral("C:/Windows/notepad.exe");
    item.sourceName = QStringLiteral("Notepad");
    item.copiedAt = QDateTime::currentDateTime();
    item.category = repository.classifyText(item.text);
    check(item.category == QStringLiteral("链接") || !item.category.isEmpty(), "url-is-classified");
    const ClipboardItem saved = repository.save(item);
    check(saved.databaseID > 0, "save-assigns-id");
    const ClipboardItem again = repository.save(item);
    check(repository.lastError().isEmpty(), "duplicate-save-has-no-error");
    check(again.databaseID == saved.databaseID, "duplicate-reuses-row");
    check(repository.fetch().size() == 1, "duplicate-within-24h-collapses");
    check(repository.fetch().first().copyCount == 2, "duplicate-increments-copy-count");

    ClipboardItem other;
    other.text = QStringLiteral("hello clipdeck");
    other.title = other.text;
    other.sourceID = item.sourceID;
    other.sourceName = item.sourceName;
    other.copiedAt = QDateTime::currentDateTime();
    repository.save(other);
    check(repository.fetch(QString(), QStringLiteral("clipdeck")).size() == 1, "search-finds-matching-item");
    check(repository.remove(saved.databaseID), "remove-item");
    check(repository.fetch().size() == 1, "remove-leaves-other-items");

    QString error;
    check(repository.categoryScriptMatches(QStringLiteral("function classify(item){return item.text.indexOf('a')>=0;}"),
                                           QStringLiteral("banana"), QString(), &error),
          "category-script-runs-in-qjsengine");

    defaultRuleTests(repository);
    upgradeTests();
    categoryColorTests();
    hybridClockTests();
    syncBookkeepingTests();
    cardTypeTests();
    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
