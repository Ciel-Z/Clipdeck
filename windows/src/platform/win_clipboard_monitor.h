#pragma once

#include "history/clipboard_snapshot.h"

#include <QThread>

#include <atomic>
#include <functional>

#include <windows.h>

// Listens for WM_CLIPBOARDUPDATE on a message-only window owned by its own
// thread and hands owned ClipboardSnapshots to a sink (called on that thread).
// Replaces mac ClipboardMonitor's 0.2 s changeCount poll with the OS event.
class WinClipboardMonitor final : public QThread {
    Q_OBJECT

public:
    using Sink = std::function<void(ClipboardSnapshot)>;

    explicit WinClipboardMonitor(Sink sink, QObject *parent = nullptr);
    ~WinClipboardMonitor() override;

    // Starts the thread and blocks until the listener is installed. The
    // current clipboard is delivered once as a baseline snapshot (not recorded).
    bool begin();
    void end();

    // Skip changes that belong to this process (default). Tests turn it off.
    void setIgnoreOwnProcess(bool ignore) { ignoreOwnProcess_ = ignore; }
    // The clipboard sequence number of a write ClipDeck just made.
    void suppressSequence(quint32 sequence) { suppressedSequence_ = sequence; }

protected:
    void run() override;

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void capture(bool baseline);

    Sink sink_;
    HWND window_ = nullptr;
    std::atomic<bool> ready_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> ignoreOwnProcess_{true};
    std::atomic<quint32> suppressedSequence_{0};
};
