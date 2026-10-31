// Runs the shared data-layer checks under Qt 6 on Windows.
#include "history/file_reference_tests.h"
#include "history_repository.h"

#include <QCoreApplication>
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
    cardTypeTests();
    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
