// Runs the shared data-layer checks under Qt 6 on Windows.
#include "history/file_reference_tests.h"
#include "history_repository.h"

#include <QCoreApplication>
#include <QApplication>
#include <QTemporaryDir>
#include <cstdio>

namespace {
int failures = 0;
void check(bool condition, const char *name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
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
    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
