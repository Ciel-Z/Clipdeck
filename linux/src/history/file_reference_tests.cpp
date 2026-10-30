#include "file_reference_tests.h"
#include "capture_payload.h"
#include "file_paste_controller.h"
#include "../ui/file_card_view.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QMimeData>
#include <QTemporaryDir>
#include <QThread>
#include <QScrollBar>
#include <QPainter>
#include <algorithm>

namespace {
bool waitFor(const std::function<bool()> &done, int timeout=3000) {
    QElapsedTimer clock; clock.start();
    while (!done() && clock.elapsed()<timeout) { QApplication::processEvents(QEventLoop::AllEvents,5); QThread::msleep(1); }
    return done();
}
}

QStringList fileReferenceSelfTestFailures() {
    QStringList failures;
    const auto require=[&](bool good,const char *name) { if(!good) failures.append(QString::fromLatin1(name)); };
    QTemporaryDir temporary;
    if (!temporary.isValid()) return {QStringLiteral("file-test-directory")};
    const auto create=[&](const QString &name,const QByteArray &bytes) {
        const QString path=temporary.filePath(name); QFile file(path);
        if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size()) failures.append(QStringLiteral("file-fixture-write"));
        return path;
    };
    const QString document=create(QStringLiteral("报告 #1.epub"),QByteArray("PK\003\004epub document, not an image"));
    const QString pdf=create(QStringLiteral("第二份报告.pdf"),QByteArrayLiteral("%PDF-1.4\ndocument"));
    const QString directory=temporary.filePath(QStringLiteral("文件夹")); QDir().mkpath(directory);
    QImage image(120,80,QImage::Format_ARGB32); image.fill(QColor(25,120,220));
    QByteArray png; QBuffer pngBuffer(&png); pngBuffer.open(QIODevice::WriteOnly); image.save(&pngBuffer,"PNG");
    const QString picture=create(QStringLiteral("真实图片.epub"),png);
    ClipboardSnapshot snapshot;
    snapshot.uriList=QUrl::fromLocalFile(picture).toEncoded();
    snapshot.copiedAt=QDateTime::currentDateTime();
    auto payload=payloadFromSnapshot(snapshot);
    require(payload && payload->isImage() && payload->image.size()==image.size(),"file-actual-image-content-recognized-before-category");
    snapshot.uriList=QUrl::fromLocalFile(document).toEncoded(); snapshot.png=png;
    payload=payloadFromSnapshot(snapshot);
    require(payload && payload->isFiles() && payload->imageData.isEmpty(),"file-document-never-captures-attached-application-icon");
    snapshot.uriList=QUrl::fromLocalFile(document).toEncoded()+"\r\n"+QUrl::fromLocalFile(pdf).toEncoded()
        +"\r\n"+QUrl::fromLocalFile(directory).toEncoded();
    payload=payloadFromSnapshot(snapshot);
    require(payload && payload->isFiles() && payload->files.size()==3 && payload->files.last().isDirectory,"file-multi-file-and-directory-snapshot");
    if(!payload) return failures;
    require(FileReferences::urls("copy\n"+snapshot.uriList+"\n"+QUrl::fromLocalFile(pdf).toEncoded()).size()==3,
            "file-uri-encoding-and-gnome-duplicates");
    ClipboardItem item=*payload;
    item.sourceName=QStringLiteral("Files");
    HistoryRepository repository(temporary.filePath(QStringLiteral("history.sqlite")));
    const auto stored=repository.save(item);
    auto changed=item; std::reverse(changed.files.begin(),changed.files.end()); changed.files.first().name=QStringLiteral("changed metadata");
    changed.copiedAt=changed.copiedAt.addMSecs(1);
    const auto duplicate=repository.save(changed);
    require(stored.databaseID>=0 && stored.databaseID==duplicate.databaseID && duplicate.copyCount==2,
            "file-sha256-order-independent-metadata-independent-dedup");
    require(repository.fetch(QStringLiteral("文件"),QStringLiteral("第二份")).size()==1,"file-name-and-path-search");
    const auto fetched=repository.fetch(QStringLiteral("文件"));
    require(fetched.size()==1 && fetched.first().isFiles() && fetched.first().files.size()==3,"file-payload-persists-roundtrip");
    require(FileReferences::identity(FileReferences::decode(FileReferences::encode(item.files)))==FileReferences::identity(item.files),
            "file-reference-json-roundtrip");
    std::unique_ptr<QMimeData> mime(FileReferences::mimeData(item.files));
    require(mime->urls().size()==3 && mime->data(QStringLiteral("x-special/gnome-copied-files")).startsWith("copy\n")
        && !mime->hasImage(),"file-paste-restores-native-url-bundle-not-text-or-icon");
    QFile::remove(document);
    require(repository.fetch(QStringLiteral("文件")).size()==1,"file-missing-reference-is-not-hidden-or-deleted");
    QApplication::clipboard()->setText(QStringLiteral("file-preflight-must-preserve-native-clipboard"));
    FilePasteController controller;
    item.databaseID=stored.databaseID;
    bool valid=true;
    int ready=0;
    ClipboardItem prepared;
    controller.prepare(item,[&]{return valid;},[&](ClipboardItem result){++ready;prepared=std::move(result);});
    require(waitFor([&]{return controller.feedback().phase!=FilePasteFeedback::Checking;}),"file-preflight-completes");
    require(ready==0 && controller.feedback().phase==FilePasteFeedback::Partial
        && controller.feedback().available.size()==2 && controller.feedback().unavailable.size()==1,"file-partial-requires-confirmation");
    require(QApplication::clipboard()->text()==QStringLiteral("file-preflight-must-preserve-native-clipboard"),"file-preflight-never-writes-clipboard");
    controller.prepare(item,[&]{return valid;},[&](ClipboardItem result){++ready;prepared=std::move(result);});
    require(waitFor([&]{return ready==1;}) && prepared.files.size()==2,"file-partial-confirmation-uses-only-rechecked-available-files");
    controller.cancel();
    auto unavailable=item; unavailable.files={item.files.first()};
    controller.prepare(unavailable,[]{return true;},[&](ClipboardItem){++ready;});
    require(waitFor([&]{return controller.feedback().phase==FilePasteFeedback::Unavailable;}) && ready==1,"file-all-unavailable-preserves-history-and-clipboard");
    FilePasteController slow(nullptr,[](const ClipboardFileReference &){QThread::msleep(90);return true;},20);
    slow.prepare(item,[]{return true;},[&](ClipboardItem){++ready;});
    require(waitFor([&]{return slow.feedback().phase==FilePasteFeedback::Unavailable;}),"file-preflight-timeout");
    QElapsedTimer elapsed;elapsed.start();waitFor([&]{return elapsed.elapsed()>320;});
    require(ready==1,"file-preflight-late-result-cannot-paste");
    FilePasteController cancelled(nullptr,[](const ClipboardFileReference &){QThread::msleep(20);return true;});
    cancelled.prepare(item,[&]{return valid;},[&](ClipboardItem){++ready;});
    valid=false;
    require(waitFor([&]{return cancelled.feedback().phase==FilePasteFeedback::Idle;}) && ready==1,"file-preflight-selection-or-clipboard-revision-cancellation");

    QVector<ClipboardFileReference> many;
    for(int i=0;i<12;++i) many.append({QStringLiteral("/example/报告-%1.pdf").arg(i+1),QStringLiteral("报告-%1.pdf").arg(i+1),QStringLiteral("application/pdf"),false});
    FilePreviewWidget preview(many); preview.resize(560,302);preview.show();QApplication::processEvents();
    require(preview.width()==560,"file-preview-fixed-width");
    preview.scrollBy(100);
    const QString renders=qEnvironmentVariable("CLIPDECK_RENDER_DIRECTORY");
    if(!renders.isEmpty()) {
        QDir().mkpath(renders);
        for(int height:{168,269,338}) {
            QImage canvas(QSize(338,height),QImage::Format_ARGB32_Premultiplied);canvas.fill(QColor(19,19,21));
            QPainter painter(&canvas);paintFileCard(painter,canvas.rect(),many,true);painter.end();
            require(canvas.save(QDir(renders).filePath(QStringLiteral("files-height-%1.png").arg(height))),"file-card-render");
        }
        preview.grab().save(QDir(renders).filePath(QStringLiteral("files-preview.png")));
    }
    preview.hide();
    return failures;
}
