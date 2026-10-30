#pragma once

#include "history/clipboard_pipeline.h"
#include "win_clipboard_monitor.h"

#include <QObject>

#include <memory>

// Windows capture chain: WM_CLIPBOARDUPDATE monitor -> ClipboardSnapshot ->
// the pipeline shared with Linux (payload, classification, SQLite on its own
// worker thread). Created and used on the GUI thread.
class WinCaptureService final : public QObject {
    Q_OBJECT

public:
    explicit WinCaptureService(const QString &databasePath, QObject *parent = nullptr);
    ~WinCaptureService() override;

    bool start();
    void stop();

    ClipboardPipeline *pipeline() { return &pipeline_; }
    WinClipboardMonitor *monitor() { return &monitor_; }

private:
    ClipboardPipeline pipeline_;
    WinClipboardMonitor monitor_;
    bool started_ = false;
};
