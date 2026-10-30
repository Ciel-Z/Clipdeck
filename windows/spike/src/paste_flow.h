#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>

#include <windows.h>

// Port of PasteService.pasteAsSoonAsActive (mac/Sources/ClipDeck/Services/
// PasteService.swift): write the clipboard, return focus to the target, poll
// every 30 ms for up to 2 s, and send exactly one paste keystroke.
class PasteFlow final : public QObject {
    Q_OBJECT

public:
    enum class Outcome { Pasted, CopiedOnlyElevatedTarget, CopiedOnlyNoTarget, Cancelled, TimedOut, Failed };
    Q_ENUM(Outcome)

    explicit PasteFlow(QObject *parent = nullptr);

    bool isRunning() const { return poll_.isActive(); }

    // hideDeck is invoked after the target has been asked to take focus.
    void paste(const QString &text, bool plainText, const QString &trigger,
               HWND target, HWND deck, const std::function<void()> &hideDeck);

signals:
    void finished(PasteFlow::Outcome outcome, const QString &detail);

private:
    bool writeClipboard(const QString &text, bool plainText, HWND owner);
    bool bringToFront(HWND target);
    QString sendPasteKeystroke();
    void finish(Outcome outcome, const QString &detail);
    void tick();

    QTimer poll_;
    QElapsedTimer elapsed_;
    HWND target_ = nullptr;
    HWND deck_ = nullptr;
    DWORD sequence_ = 0;
    QString trigger_;
};
