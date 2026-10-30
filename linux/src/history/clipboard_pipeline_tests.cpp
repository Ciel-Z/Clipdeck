#include "clipboard_pipeline_tests.h"
#include "clipboard_pipeline.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QMimeData>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>
#include <atomic>

namespace {
bool until(const std::function<bool()> &done, int timeout = 20000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done() && elapsed.elapsed() < timeout) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    return done();
}

ClipboardSnapshot textSnapshot(quint64 sequence, const QByteArray &text, qint64 time) {
    ClipboardSnapshot result;
    result.sequence = sequence;
    result.text = text;
    result.copiedAt = QDateTime::fromMSecsSinceEpoch(time);
    result.sourceWindow = quint32(sequence);
    return result;
}
}

QStringList clipboardPipelineSelfTestFailures() {
    QStringList failures;
    const auto require = [&](bool condition, const char *name) {
        if (!condition) failures.append(QString::fromLatin1(name));
    };
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    ClipboardSnapshotQueue queue(2, 12);
    queue.push(textSnapshot(1, "one", now));
    queue.push(textSnapshot(2, "two", now));
    require(queue.pop(false).sequence == 1, "queue-fifo-head");
    queue.push(textSnapshot(3, "three", now));
    require(queue.pop(true).sequence == 3, "queue-prioritizes-newest-tail");
    queue.push(textSnapshot(4, "four", now));
    queue.push(textSnapshot(5, "five", now));
    require(queue.size() == 2 && queue.pop(false).sequence == 4, "queue-bounds-evict-oldest");
    require(!queue.push(textSnapshot(6, QByteArray(13, 'x'), now)), "queue-bounds-oversize");
    ClipboardSnapshotQueue memoryQueue(99, 6);
    memoryQueue.push(textSnapshot(1, "aaaa", now));
    memoryQueue.push(textSnapshot(2, "bbbb", now));
    require(memoryQueue.size() == 1 && memoryQueue.byteCount() == 4
        && memoryQueue.pop(false).sequence == 2 && memoryQueue.byteCount() == 0, "queue-byte-budget");

    QTemporaryDir temporary;
    require(temporary.isValid(), "pipeline-test-directory");
    if (!temporary.isValid()) return failures;
    QApplication::clipboard()->setText(QStringLiteral("native-clipboard-must-not-change"));
    int clipboardWrites = 0;
    const auto clipboardConnection = QObject::connect(QApplication::clipboard(), &QClipboard::dataChanged,
                                                     [&] { ++clipboardWrites; });
    std::atomic<bool> resolvedOffUI{true};
    QThread *uiThread = QThread::currentThread();
    ClipboardPipeline pipeline(temporary.filePath(QStringLiteral("history.sqlite")), false,
        [&](quint32 source, quint32) {
            if (QThread::currentThread() == uiThread) resolvedOffUI = false;
            QThread::msleep(5); // deliberately slow enrichment must not block UI events
            SourceApplicationInfo info;
            info.identifier = QStringLiteral("source-%1").arg(source);
            info.name = info.identifier;
            return info;
        });
    int published = 0;
    quint64 lastSequence = 0;
    QStringList errors;
    QObject::connect(&pipeline, &ClipboardPipeline::recentChanged, [&](quint64 sequence) {
        if (sequence) ++published;
        lastSequence = sequence;
    });
    QObject::connect(&pipeline, &ClipboardPipeline::processingError, [&](const QString &error) { errors.append(error); });
    int heartbeat = 0;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, [&] { ++heartbeat; });
    timer.start(1);
    for (quint64 sequence = 1; sequence <= 12; ++sequence)
        pipeline.submit(textSnapshot(sequence, "history-" + QByteArray::number(sequence), now + sequence));
    auto duplicate = textSnapshot(13, "history-12", now + 20);
    duplicate.sourceWindow = 99;
    pipeline.submit(duplicate);
    pipeline.setDeckVisible(true);
    pipeline.start();
    const bool drained = until([&] { return published == 13 || !errors.isEmpty(); });
    require(drained && errors.isEmpty(), "pipeline-drains-capture-queue");
    require(resolvedOffUI && heartbeat > 10, "pipeline-enrichment-keeps-ui-responsive");
    const auto recent = pipeline.recentItems();
    require(recent.size() == 10, "pipeline-recent-cache-bounded-to-ten");
    require(!recent.isEmpty() && recent.first().text == QStringLiteral("history-12")
        && recent.first().copiedAt.toMSecsSinceEpoch() == now + 20
        && recent.first().copyCount == 2 && recent.first().sourceID == QStringLiteral("source-99"),
        "pipeline-tail-first-preserves-capture-time-and-source");
    bool historyReady = false;
    QVector<ClipboardItem> all;
    pipeline.requestHistory({}, {}, 100, 0, [&](QVector<ClipboardItem> items) {
        all = std::move(items); historyReady = true;
    });
    require(until([&] { return historyReady; }) && all.size() == 12, "pipeline-hash-deduplication");

    pipeline.setDeckVisible(false);
    auto poll = textSnapshot(14, "history-12", now + 21);
    poll.explicitChange = false;
    pipeline.submit(poll);
    auto largeText = textSnapshot(15, QByteArray(6000, 'a'), now + 22);
    largeText.html = "<pre>" + largeText.text + "</pre>";
    pipeline.submit(largeText);
    require(until([&] { return lastSequence == 15; }), "pipeline-normal-queue-consumed");
    require(pipeline.recentItems().size() == 10 && !pipeline.recentItems().first().payloadComplete
        && pipeline.recentItems().first().text.size() == 3000
        && pipeline.recentItems().first().textLength == 6000, "pipeline-text-summary-is-lightweight");
    bool fullReady = false;
    pipeline.requestItem(pipeline.recentItems().first().databaseID, [&](QVector<ClipboardItem> items) {
        require(items.size() == 1 && items.first().payloadComplete && items.first().text.size() == 6000
            && items.first().richTextHtml == QString::fromUtf8(largeText.html), "pipeline-full-text-payload");
        fullReady = true;
    });
    require(until([&] { return fullReady; }), "pipeline-full-text-response");

    ClipboardSnapshot image;
    image.sequence = 16;
    image.copiedAt = QDateTime::fromMSecsSinceEpoch(now + 23);
    QImage original(1800, 1200, QImage::Format_ARGB32);
    original.fill(QColor(25, 120, 220));
    QBuffer buffer(&image.png);
    buffer.open(QIODevice::WriteOnly);
    original.save(&buffer, "PNG");
    pipeline.submit(image);
    require(until([&] { return lastSequence == 16; }), "pipeline-image-response");
    const auto cachedImage = pipeline.recentItems().first();
    require(cachedImage.isImage() && !cachedImage.payloadComplete && cachedImage.imageData.isEmpty()
        && cachedImage.imagePixelSize == original.size() && cachedImage.image.width() < original.width(),
        "pipeline-image-cache-is-thumbnail-only");
    fullReady = false;
    pipeline.requestItem(cachedImage.databaseID, [&](QVector<ClipboardItem> items) {
        require(items.size() == 1 && items.first().image.size() == original.size()
            && items.first().imageData == image.png, "pipeline-full-resolution-image-payload");
        fullReady = true;
    });
    require(until([&] { return fullReady; }), "pipeline-full-image-response");
    historyReady = false;
    pipeline.requestHistory({}, {}, 100, 0, [&](QVector<ClipboardItem> items) {
        for (const auto &item : items) {
            if (item.text == QStringLiteral("history-12"))
                require(item.copyCount == 2, "pipeline-poll-does-not-recopy-unchanged-content");
        }
        historyReady = true;
    });
    require(until([&] { return historyReady; }), "pipeline-final-query-response");
    require(clipboardWrites == 0 && QApplication::clipboard()->text()
        == QStringLiteral("native-clipboard-must-not-change"), "pipeline-never-writes-native-clipboard");
    QObject::disconnect(clipboardConnection);

    if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
        ClipboardPipeline x11(temporary.filePath(QStringLiteral("x11.sqlite")), true,
                              [](quint32, quint32) { return SourceApplicationInfo{}; });
        x11.start();
        // This Qt process acts as a separate X11 selection owner from the
        // reader's X connection. Large payloads exercise the actual INCR path.
        const QString large = QStringLiteral("最新剪切内容\n") + QString(1024 * 1024, QLatin1Char('x'));
        const QString html = QStringLiteral("<pre>") + large + QStringLiteral("</pre>");
        auto *mime = new QMimeData;
        mime->setText(large);
        mime->setHtml(html);
        QApplication::clipboard()->setMimeData(mime);
        x11.captureChanged();
        const bool read = until([&] { return !x11.recentItems().isEmpty(); });
        require(read && x11.recentItems().first().textLength == large.size(), "x11-asynchronous-incr-text-snapshot");
        if (read) {
            bool ready = false;
            x11.requestItem(x11.recentItems().first().databaseID, [&](QVector<ClipboardItem> items) {
                require(items.size() == 1 && items.first().text == large && items.first().richTextHtml == html,
                        "x11-snapshot-preserves-complete-text-and-html");
                ready = true;
            });
            require(until([&] { return ready; }), "x11-full-payload-response");
        }
        require(QApplication::clipboard()->text() == large, "x11-reader-does-not-change-selection-owner");
        QApplication::clipboard()->setImage(original);
        x11.captureChanged();
        require(until([&] { return !x11.recentItems().isEmpty() && x11.recentItems().first().isImage(); }),
                "x11-asynchronous-png-snapshot");
        require(QApplication::clipboard()->image().size() == original.size(), "x11-reader-preserves-native-image-clipboard");
        const QString documentPath=temporary.filePath(QStringLiteral("document.epub"));
        QFile document(documentPath); document.open(QIODevice::WriteOnly); document.write("PK\003\004not an image"); document.close();
        auto *fileMime=new QMimeData;
        fileMime->setUrls({QUrl::fromLocalFile(documentPath)});
        fileMime->setImageData(original); // source application's incidental icon
        QApplication::clipboard()->setMimeData(fileMime);
        x11.captureChanged();
        require(until([&]{return !x11.recentItems().isEmpty() && x11.recentItems().first().isFiles();})
            && x11.recentItems().first().files.first().path==documentPath,"x11-file-uri-is-not-co-published-application-icon");
        const QString picturePath=temporary.filePath(QStringLiteral("actual-image.png"));
        QImage picture(140,90,QImage::Format_ARGB32);picture.fill(QColor(240,160,30));picture.save(picturePath,"PNG");
        auto *pictureMime=new QMimeData;
        pictureMime->setUrls({QUrl::fromLocalFile(picturePath)});pictureMime->setImageData(original);
        QApplication::clipboard()->setMimeData(pictureMime);x11.captureChanged();
        const bool imageReady=until([&]{return !x11.recentItems().isEmpty() && x11.recentItems().first().isImage();});
        require(imageReady && x11.recentItems().first().imagePixelSize==picture.size(),"x11-local-image-uses-actual-file-type-and-pixels");
        auto *gnomeMime=new QMimeData;
        const QByteArray gnomeData="copy\n"+QUrl::fromLocalFile(documentPath).toEncoded();
        gnomeMime->setData(QStringLiteral("x-special/gnome-copied-files"),gnomeData);
        QApplication::clipboard()->setMimeData(gnomeMime);x11.captureChanged();
        require(until([&]{return !x11.recentItems().isEmpty() && x11.recentItems().first().isFiles();}),"x11-native-file-manager-target-without-uri-target");
        require(QApplication::clipboard()->mimeData()->data(QStringLiteral("x-special/gnome-copied-files"))==gnomeData,
            "x11-file-capture-never-replaces-native-clipboard");
    }
    return failures;
}
