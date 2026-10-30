#pragma once
#include "../history_repository.h"
#include <QObject>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>

struct FilePasteFeedback {
    enum Phase { Idle, Checking, Unavailable, Partial } phase = Idle;
    qint64 itemID = -1;
    QString message;
    QVector<ClipboardFileReference> available;
    QVector<ClipboardFileReference> unavailable;
};

class FilePasteController final : public QObject {
public:
    using Checker = std::function<bool(const ClipboardFileReference &)>;
    explicit FilePasteController(QObject *parent = nullptr, Checker checker = {}, int timeoutMs = 2000);
    void prepare(ClipboardItem item, std::function<bool()> stillCurrent,
                 std::function<void(ClipboardItem)> ready);
    void cancel();
    const FilePasteFeedback &feedback() const { return feedback_; }
    std::function<void()> changed;

private:
    struct Job;
    void tick();
    QTimer timer_;
    Checker checker_;
    int timeoutMs_;
    std::shared_ptr<Job> job_;
    std::shared_ptr<std::atomic<int>> activeJobs_;
    FilePasteFeedback feedback_;
    std::function<bool()> stillCurrent_;
    std::function<void(ClipboardItem)> ready_;
    ClipboardItem item_;
};
