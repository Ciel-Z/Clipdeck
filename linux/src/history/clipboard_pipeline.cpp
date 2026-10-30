#include "clipboard_pipeline.h"
#include "capture_payload.h"
#ifndef CLIPDECK_NO_X11
#include "../platform/x11_clipboard_reader.h"
#endif
#include "../ui/display_scale.h"

#include <QBuffer>
#include <QCache>
#include <QCryptographicHash>
#include <QDebug>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QSettings>
#include <QWaitCondition>
#include <algorithm>

ClipboardSnapshotQueue::ClipboardSnapshotQueue(int capacity, qint64 bytes)
    : capacity_(std::max(1, capacity)), maximumBytes_(std::max<qint64>(1, bytes)) {}

bool ClipboardSnapshotQueue::push(ClipboardSnapshot snapshot) {
    if (snapshot.byteCount() > maximumBytes_) return false;
    while (!items_.empty() && (size() >= capacity_ || bytes_ + snapshot.byteCount() > maximumBytes_)) {
        qWarning() << "[ClipDeck][Capture] queue limit evicted oldest snapshot seq=" << items_.front().sequence;
        bytes_ -= items_.front().byteCount();
        items_.pop_front();
    }
    bytes_ += snapshot.byteCount();
    items_.push_back(std::move(snapshot));
    return true;
}

ClipboardSnapshot ClipboardSnapshotQueue::pop(bool newestFirst) {
    if (items_.empty()) return {};
    ClipboardSnapshot snapshot = std::move(newestFirst ? items_.back() : items_.front());
    if (newestFirst) items_.pop_back(); else items_.pop_front();
    bytes_ -= snapshot.byteCount();
    return snapshot;
}

class HistoryWorker final : public QThread {
public:
    HistoryWorker(ClipboardPipeline *owner, QString path, ClipboardPipeline::SourceResolver resolver)
        : owner_(owner), path_(std::move(path)), resolver_(std::move(resolver)) {}
    void enqueue(ClipboardSnapshot snapshot) {
        QMutexLocker locker(&mutex_);
        if (!queue_.push(std::move(snapshot))) report(QStringLiteral("Clipboard snapshot exceeds queue memory limit"));
        available_.wakeOne();
    }
    void prioritize(bool visible) {
        QMutexLocker locker(&mutex_);
        urgent_ = visible;
        available_.wakeOne();
    }
    struct Query {
        QString category;
        QString search;
        int limit = 100;
        int offset = 0;
        qint64 itemID = -1;
        ClipboardPipeline::ItemsCallback callback;
    };
    void query(Query query) {
        QMutexLocker locker(&mutex_);
        if (query.itemID >= 0 || query.offset > 0) itemQueries_.push_back(std::move(query));
        else historyQuery_ = std::make_unique<Query>(std::move(query));
        available_.wakeOne();
    }
    void recent() {
        QMutexLocker locker(&mutex_);
        refresh_ = true;
        available_.wakeOne();
    }
    void stop() {
        { QMutexLocker locker(&mutex_); stopping_ = true; available_.wakeOne(); }
        wait();
    }

protected:
    void run() override {
        // Both the database and every QJSEngine used for classification are
        // created and destroyed on this worker, never moved from the UI.
        HistoryRepository repository(path_);
        if (!repository.isOpen()) { report(repository.lastError()); return; }
        QCache<QByteArray, QImage> thumbnails(16 * 1024); // cost in KiB
        HistoryRepository::ItemTransform summarize = [&](ClipboardItem &item) {
            if (item.isImage()) {
                const QByteArray key = QCryptographicHash::hash(item.imageData, QCryptographicHash::Sha256);
                if (auto *cached = thumbnails.object(key)) item.image = *cached;
                else {
                    QBuffer buffer(&item.imageData); buffer.open(QIODevice::ReadOnly);
                    QImageReader reader(&buffer, "PNG");
                    const QSize original = reader.size();
                    const int side = qRound(420 * DisplayScale::factor());
                    if (original.isValid()) reader.setScaledSize(original.scaled(side, side, Qt::KeepAspectRatio));
                    item.image = reader.read();
                    const int cost = std::max(1, int(item.image.sizeInBytes() / 1024));
                    thumbnails.insert(key, new QImage(item.image), cost);
                }
                item.imageData.clear();
                item.payloadComplete = false;
            } else if (item.text.size() > 3000 || item.richTextHtml.size() > 64 * 1024) {
                item.text = item.text.left(3000);
                item.richTextHtml.clear();
                item.payloadComplete = false;
            }
        };
        auto publishRecent = [&](quint64 sequence) {
            // Transform each SQL row before appending: a batch of 100 image
            // cards must never retain 100 original PNG blobs in memory.
            auto items = repository.fetch(QString(), QString(), 10, 0, -1, false, summarize);
            QMetaObject::invokeMethod(owner_, [owner = owner_, items = std::move(items), sequence]() mutable {
                owner->publishRecent(std::move(items), sequence);
            }, Qt::QueuedConnection);
        };
        publishRecent(0);
        QByteArray lastFingerprint;
        quint64 lastObservedSequence = 0;
        int sinceQuery = 0;
        while (true) {
            ClipboardSnapshot snapshot;
            std::unique_ptr<Query> query;
            bool refresh = false;
            {
                QMutexLocker locker(&mutex_);
                while (!stopping_ && queue_.size() == 0 && itemQueries_.empty() && !historyQuery_ && !refresh_)
                    available_.wait(&mutex_);
                if (stopping_ && queue_.size() == 0) break;
                if (!itemQueries_.empty()) {
                    query = std::make_unique<Query>(std::move(itemQueries_.front()));
                    itemQueries_.pop_front();
                } else if (historyQuery_ && (sinceQuery >= 4 || queue_.size() == 0)) {
                    query = std::move(historyQuery_);
                    sinceQuery = 0;
                } else if (queue_.size()) { snapshot = queue_.pop(urgent_); ++sinceQuery; }
                else { refresh = refresh_; refresh_ = false; }
            }
            if (query) {
                const bool full = query->itemID >= 0;
                auto result = repository.fetch(query->category, query->search, query->limit,
                    query->offset, query->itemID, full,
                    full ? HistoryRepository::ItemTransform{} : summarize);
                QMetaObject::invokeMethod(owner_, [query = std::shared_ptr<Query>(std::move(query)),
                    result = std::move(result)]() mutable { query->callback(std::move(result)); }, Qt::QueuedConnection);
                continue;
            }
            if (refresh) { publishRecent(0); continue; }
            if (!snapshot.sequence) continue;
            const QByteArray fingerprint = QCryptographicHash::hash(!snapshot.uriList.isEmpty()
                ? QByteArrayLiteral("files:") + snapshot.uriList
                : !snapshot.png.isEmpty() ? QByteArrayLiteral("image:") + snapshot.png
                : QByteArrayLiteral("text:") + snapshot.text + '\0' + snapshot.html,
                QCryptographicHash::Sha256);
            if (snapshot.baseline) {
                if (snapshot.sequence >= lastObservedSequence) {
                    lastFingerprint = fingerprint;
                    lastObservedSequence = snapshot.sequence;
                }
                continue;
            }
            if (!snapshot.explicitChange && fingerprint == lastFingerprint) continue;
            if (snapshot.sequence >= lastObservedSequence) {
                lastFingerprint = fingerprint;
                lastObservedSequence = snapshot.sequence;
            }
            auto payload = payloadFromSnapshot(snapshot);
            if (!payload) continue;
            ClipboardItem item = std::move(*payload);
            const SourceApplicationInfo source = resolver_ ? resolver_(snapshot.sourceWindow, snapshot.ownerWindow)
                                                           : SourceApplicationInfo{};
            item.sourceID = source.identifier; item.sourceName = source.name;
            item.sourceIconData = source.iconData; item.sourceIconVariantsData = source.iconVariantsData;
            if (item.isText()) item.category = repository.classifyText(item.text, item.sourceID, item.sourceName, item.copiedAt);
            const ClipboardItem stored = repository.save(item);
            if (stored.databaseID < 0) { report(repository.lastError()); continue; }
            qInfo() << "[ClipDeck][Capture] stored seq=" << snapshot.sequence
                    << "id=" << stored.databaseID << "image=" << stored.isImage()
                    << "capturedAt=" << stored.copiedAt.toMSecsSinceEpoch();
            publishRecent(snapshot.sequence);
        }
    }

private:
    void report(QString message) {
        QMetaObject::invokeMethod(owner_, [owner = owner_, message] {
            Q_EMIT owner->processingError(message);
        }, Qt::QueuedConnection);
    }
    ClipboardPipeline *owner_;
    QString path_;
    ClipboardPipeline::SourceResolver resolver_;
    QMutex mutex_;
    QWaitCondition available_;
    ClipboardSnapshotQueue queue_;
    std::deque<Query> itemQueries_;
    std::unique_ptr<Query> historyQuery_;
    bool stopping_ = false;
    bool urgent_ = false;
    bool refresh_ = false;
};

ClipboardPipeline::ClipboardPipeline(QString path, bool useX11, SourceResolver resolver, QObject *parent)
    : QObject(parent), worker_(new HistoryWorker(this, std::move(path), std::move(resolver))) {
#ifndef CLIPDECK_NO_X11
    if (useX11) {
        reader_ = new X11ClipboardReader([this](ClipboardSnapshot snapshot) {
            worker_->enqueue(std::move(snapshot));
        });
        reader_->moveToThread(&readerThread_);
        QObject::connect(&readerThread_, &QThread::started, reader_, [this] { reader_->start(); });
        QObject::connect(&readerThread_, &QThread::finished, reader_, &QObject::deleteLater);
    }
#else
    // Snapshots come from a platform monitor through submit().
    Q_UNUSED(useX11)
#endif
}

ClipboardPipeline::~ClipboardPipeline() {
    if (reader_ && started_) { readerThread_.quit(); readerThread_.wait(); }
#ifndef CLIPDECK_NO_X11
    else if (reader_) delete reader_;
#endif
    if (started_) worker_->stop();
}

void ClipboardPipeline::start() {
    if (started_) return;
    started_ = true;
    worker_->start(QThread::LowPriority);
    if (reader_) { readerThread_.start(); notifyReader(false, false, true); }
}

void ClipboardPipeline::notifyReader(bool changed, bool ignore, bool baseline) {
    if (!reader_ || !started_) return;
    const quint64 sequence = ++nextSequence_;
    lastCaptureTime_ = std::max(QDateTime::currentMSecsSinceEpoch(), lastCaptureTime_ + 1);
    const auto time = QDateTime::fromMSecsSinceEpoch(lastCaptureTime_);
#ifndef CLIPDECK_NO_X11
    QMetaObject::invokeMethod(reader_, [reader = reader_, sequence, time, changed, ignore, baseline] {
        reader->capture(sequence, time, changed, ignore, baseline, !changed && !baseline);
    }, Qt::QueuedConnection);
#else
    Q_UNUSED(sequence) Q_UNUSED(time) Q_UNUSED(changed) Q_UNUSED(ignore) Q_UNUSED(baseline)
#endif
}

void ClipboardPipeline::captureChanged(bool ownHistoryWrite) { notifyReader(true, ownHistoryWrite); }
void ClipboardPipeline::pollClipboard() { notifyReader(false, false); }
void ClipboardPipeline::submit(ClipboardSnapshot snapshot) {
    if (!snapshot.sequence) snapshot.sequence = ++nextSequence_;
    else nextSequence_ = std::max(nextSequence_, snapshot.sequence);
    if (!snapshot.copiedAt.isValid()) snapshot.copiedAt = QDateTime::currentDateTime();
    worker_->enqueue(std::move(snapshot));
}
void ClipboardPipeline::setDeckVisible(bool visible) {
    deckVisible_ = visible;
    worker_->prioritize(visible);
    if (started_) worker_->setPriority(visible ? QThread::NormalPriority : QThread::LowPriority);
}
void ClipboardPipeline::requestHistory(QString category, QString search, int limit, int offset, ItemsCallback callback) {
    worker_->query({std::move(category), std::move(search), limit, offset, -1, std::move(callback)});
}
void ClipboardPipeline::requestItem(qint64 id, ItemsCallback callback) {
    worker_->query({{}, {}, 1, 0, id, std::move(callback)});
}
void ClipboardPipeline::refreshRecent() { worker_->recent(); }
void ClipboardPipeline::publishRecent(QVector<ClipboardItem> items, quint64 sequence) {
    recent_ = std::move(items);
    latestPublished_ = std::max(sequence, latestPublished_);
    Q_EMIT recentChanged(sequence);
}
