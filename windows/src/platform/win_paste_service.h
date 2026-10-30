#pragma once

#include "history_repository.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <functional>

#include <windows.h>

// Port of mac PasteService: write the item to the clipboard, dismiss the Deck,
// return focus to the window recorded before the Deck opened, wait until it is
// in front (30 ms polling, 2 s limit) and send exactly one Ctrl+V.
class WinPasteService final : public QObject {
    Q_OBJECT

public:
    static constexpr const char *preserveFormattingKey = "paste/preserveFormatting";

    // Result of paste(): what happened synchronously.
    enum class Result {
        Pasting,                  // keystroke follows once the target has focus (mac .pasted)
        CopiedOnlyElevatedTarget, // UIPI would drop SendInput (mac .copiedOnly(.accessibilityPermission))
        CopiedOnlyNoTarget,       // no usable target window (mac .copiedOnly(.targetUnavailable))
        Failed,
    };
    Q_ENUM(Result)

    // How an asynchronous Pasting attempt ended.
    enum class Outcome { Pasted, Cancelled, TimedOut };
    Q_ENUM(Outcome)

    // The target must be in front and the Deck must no longer hold the keyboard.
    static bool canDeliverPaste(bool targetIsForeground, bool deckHoldsKeyboard) {
        return targetIsForeground && !deckHoldsKeyboard;
    }

    explicit WinPasteService(QObject *parent = nullptr);

    // Called with the clipboard sequence number of every write, so the
    // monitor can skip it.
    void setWriteObserver(std::function<void(quint32)> observer) { writeObserver_ = std::move(observer); }
    // Tests paste into a window of their own process; the product never does.
    void setAllowOwnProcessTarget(bool allow) { allowOwnProcessTarget_ = allow; }

    Result paste(const ClipboardItem &item, HWND target, HWND deck, bool forcePlainText,
                 const std::function<void()> &dismissDeck);
    bool isPending() const { return poll_.isActive(); }
    void cancel();

    // The item as it is written: plain text unless formatting is preserved,
    // images rotated by their stored angle.
    static ClipboardItem prepareForWrite(const ClipboardItem &item);

Q_SIGNALS:
    void keystrokeSent();
    void finished(WinPasteService::Outcome outcome, const QString &detail);

private:
    bool bringToFront(HWND target);
    void tick();
    void finish(Outcome outcome, const QString &detail);
    static QString sendPasteKeystroke();

    QTimer poll_;
    QElapsedTimer elapsed_;
    HWND target_ = nullptr;
    HWND deck_ = nullptr;
    DWORD sequence_ = 0;
    bool allowOwnProcessTarget_ = false;
    std::function<void(quint32)> writeObserver_;
};
