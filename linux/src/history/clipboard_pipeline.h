#pragma once

#include "clipboard_snapshot.h"
#include "../history_repository.h"
#include "../platform/application_source.h"

#include <QObject>
#include <QThread>
#include <deque>
#include <functional>
#include <memory>

// Bounded FIFO in normal use; Deck presentation may consume the newest tail
// first. Persistence uses capture timestamps, never completion timestamps.
class ClipboardSnapshotQueue {
public:
    explicit ClipboardSnapshotQueue(int capacity = 128, qint64 bytes = 64 * 1024 * 1024);
    bool push(ClipboardSnapshot snapshot);
    ClipboardSnapshot pop(bool newestFirst);
    int size() const { return int(items_.size()); }
    qint64 byteCount() const { return bytes_; }
private:
    std::deque<ClipboardSnapshot> items_;
    int capacity_;
    qint64 maximumBytes_;
    qint64 bytes_ = 0;
};

class HistoryWorker;
class X11ClipboardReader;

class ClipboardPipeline final : public QObject {
    Q_OBJECT
public:
    using ItemsCallback = std::function<void(QVector<ClipboardItem>)>;
    using SourceResolver = std::function<SourceApplicationInfo(quint32, quint32)>;
    explicit ClipboardPipeline(QString databasePath, bool useX11 = true,
                               SourceResolver resolver = sourceApplicationForWindows,
                               QObject *parent = nullptr);
    ~ClipboardPipeline() override;
    void start();
    void captureChanged(bool ownHistoryWrite = false);
    void pollClipboard();
    void submit(ClipboardSnapshot snapshot);
    void setDeckVisible(bool visible);
    void requestHistory(QString category, QString search, int limit, int offset, ItemsCallback callback);
    void requestItem(qint64 id, ItemsCallback callback);
    void refreshRecent();
    const QVector<ClipboardItem> &recentItems() const { return recent_; }

Q_SIGNALS:
    void recentChanged(quint64 captureSequence);
    void processingError(const QString &message);

private:
    friend class HistoryWorker;
    void publishRecent(QVector<ClipboardItem> items, quint64 sequence);
    void notifyReader(bool explicitChange, bool ignore, bool baseline = false);
    std::unique_ptr<HistoryWorker> worker_;
    QThread readerThread_;
    X11ClipboardReader *reader_ = nullptr;
    QVector<ClipboardItem> recent_;
    quint64 nextSequence_ = 0;
    quint64 latestPublished_ = 0;
    qint64 lastCaptureTime_ = 0;
    bool started_ = false;
    bool deckVisible_ = false;
};
