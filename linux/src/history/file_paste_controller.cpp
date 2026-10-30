#include "file_paste_controller.h"
#include <QDateTime>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <thread>

struct FilePasteController::Job {
    QMutex mutex;
    bool done = false;
    qint64 deadline = 0;
    QVector<ClipboardFileReference> available, unavailable;
};

FilePasteController::FilePasteController(QObject *parent, Checker checker, int timeoutMs)
    : QObject(parent), checker_(std::move(checker)), timeoutMs_(timeoutMs),
      activeJobs_(std::make_shared<std::atomic<int>>(0)) {
    if (!checker_) checker_ = [](const ClipboardFileReference &file) {
        const QFileInfo info(file.path);
        return info.exists() && info.isReadable();
    };
    timer_.setInterval(16);
    QObject::connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
}

void FilePasteController::cancel() {
    timer_.stop();
    job_.reset();
    ready_ = {};
    stillCurrent_ = {};
    feedback_ = {};
    if (changed) changed();
}

void FilePasteController::prepare(ClipboardItem item, std::function<bool()> current,
                                std::function<void(ClipboardItem)> ready) {
    if (feedback_.itemID == item.databaseID && feedback_.phase == FilePasteFeedback::Checking) return;
    const auto files = feedback_.itemID == item.databaseID && feedback_.phase == FilePasteFeedback::Partial
        ? feedback_.available : item.files;
    cancel();
    feedback_.itemID = item.databaseID;
    if (activeJobs_->load() >= 4) {
        feedback_.phase = FilePasteFeedback::Unavailable;
        feedback_.message = QStringLiteral("正在等待磁盘或网络挂载响应，请稍后重试");
        if (changed) changed();
        return;
    }
    item_ = std::move(item);
    stillCurrent_ = std::move(current);
    ready_ = std::move(ready);
    feedback_.phase = FilePasteFeedback::Checking;
    job_ = std::make_shared<Job>();
    job_->deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs_;
    const auto job = job_;
    const auto check = checker_;
    const auto active = activeJobs_;
    ++*active;
    // No QObject or UI pointer is captured. A disconnected mount may remain
    // blocked beyond the UI timeout; late completion cannot access the Deck.
    std::thread([job, files, check, active] {
        QVector<ClipboardFileReference> available, unavailable;
        for (const auto &file : files) {
            bool readable = false;
            try { readable = check(file); } catch (...) { }
            (readable ? available : unavailable).append(file);
        }
        { QMutexLocker lock(&job->mutex);
          job->available = std::move(available); job->unavailable = std::move(unavailable); job->done = true; }
        --*active;
    }).detach();
    timer_.start();
    if (changed) changed();
}

void FilePasteController::tick() {
    if (!job_ || !stillCurrent_ || !stillCurrent_()) { cancel(); return; }
    if (QDateTime::currentMSecsSinceEpoch() >= job_->deadline) {
        timer_.stop(); job_.reset(); ready_ = {};
        feedback_.phase = FilePasteFeedback::Unavailable;
        feedback_.message = QStringLiteral("检查超时，请确认磁盘或网络挂载后重试");
        if (changed) changed();
        return;
    }
    { QMutexLocker lock(&job_->mutex);
      if (!job_->done) { if(changed) changed(); return; }
      feedback_.available = job_->available;
      feedback_.unavailable = job_->unavailable; }
    timer_.stop(); job_.reset();
    if (feedback_.available.isEmpty()) {
        feedback_.phase = FilePasteFeedback::Unavailable;
        feedback_.message = QStringLiteral("无法粘贴，源文件不可访问");
    } else if (!feedback_.unavailable.isEmpty()) {
        feedback_.phase = FilePasteFeedback::Partial;
    } else {
        auto callback = std::move(ready_);
        auto item = std::move(item_);
        item.files = feedback_.available;
        cancel();
        if (callback) callback(std::move(item));
        return;
    }
    if (changed) changed();
}
