#include "win_paste_service.h"

#include "win_clipboard_writer.h"
#include "win_process_util.h"

#include <QBuffer>
#include <QDebug>
#include <QSettings>
#include <QStringList>
#include <QTransform>

#include <vector>

namespace {

bool isKeyDown(int virtualKey) { return (GetAsyncKeyState(virtualKey) & 0x8000) != 0; }

} // namespace

WinPasteService::WinPasteService(QObject *parent) : QObject(parent) {
    poll_.setInterval(30);
    connect(&poll_, &QTimer::timeout, this, &WinPasteService::tick);
}

ClipboardItem WinPasteService::prepareForWrite(const ClipboardItem &item) {
    ClipboardItem prepared = item;
    if (prepared.isImage() && prepared.imageAngle % 360 != 0) {
        QImage image = prepared.image;
        if (image.isNull()) image.loadFromData(prepared.imageData, "PNG");
        if (!image.isNull()) {
            const int turns = ((prepared.imageAngle % 360) + 360) % 360 / 90;
            QTransform transform;
            transform.rotate(90 * turns); // clockwise, like ImageRotation.render
            prepared.image = image.transformed(transform, Qt::SmoothTransformation);
            prepared.imageData.clear(); // re-encoded from the rotated image
        }
    }
    return prepared;
}

bool WinPasteService::copy(const ClipboardItem &item, HWND owner) {
    const bool preserveFormatting = QSettings().value(QLatin1String(preserveFormattingKey), false).toBool();
    const auto written = WinClipboard::write(prepareForWrite(item), !preserveFormatting, owner);
    if (!written.ok) {
        qWarning() << "[ClipDeck][Copy]" << written.error;
        return false;
    }
    if (writeObserver_) writeObserver_(written.sequence);
    return true;
}

WinPasteService::Result WinPasteService::paste(const ClipboardItem &item, HWND target, HWND deck,
                                               bool forcePlainText, const std::function<void()> &dismissDeck) {
    cancel();
    const bool preserveFormatting = !forcePlainText
        && QSettings().value(QLatin1String(preserveFormattingKey), false).toBool();
    const auto written = WinClipboard::write(prepareForWrite(item), !preserveFormatting, deck);
    if (!written.ok) {
        qWarning() << "[ClipDeck][Paste]" << written.error;
        return Result::Failed;
    }
    sequence_ = written.sequence;
    if (writeObserver_) writeObserver_(written.sequence);

    if (dismissDeck) dismissDeck();
    target_ = target;
    deck_ = deck;

    const bool targetUsable = target && IsWindow(target)
        && (allowOwnProcessTarget_ || !WinProcess::belongsToCurrentProcess(target));
    if (!targetUsable) {
        qInfo() << "[ClipDeck][Paste] copied only: target unavailable";
        return Result::CopiedOnlyNoTarget;
    }

    // SendInput is silently dropped by UIPI for higher-integrity targets, so
    // decide up front (the macOS accessibility-not-trusted branch).
    DWORD targetProcess = 0;
    GetWindowThreadProcessId(target, &targetProcess);
    const long targetLevel = WinProcess::integrityLevel(targetProcess);
    const long ownLevel = WinProcess::integrityLevel(GetCurrentProcessId());
    if (targetLevel < 0 || targetLevel > ownLevel) {
        qInfo() << "[ClipDeck][Paste] copied only: target integrity"
                << WinProcess::integrityName(targetLevel) << "above own" << WinProcess::integrityName(ownLevel);
        return Result::CopiedOnlyElevatedTarget;
    }

    elapsed_.start();
    bringToFront(target);
    poll_.start();
    tick();
    return Result::Pasting;
}

void WinPasteService::cancel() {
    if (poll_.isActive()) finish(Outcome::Cancelled, QStringLiteral("superseded by a new paste"));
}

bool WinPasteService::bringToFront(HWND target) {
    HWND root = WinProcess::rootOwner(target);
    if (IsIconic(root)) ShowWindow(root, SW_RESTORE);
    if (SetForegroundWindow(root)) return true;

    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD currentThread = GetCurrentThreadId();
    AttachThreadInput(currentThread, foregroundThread, TRUE);
    const bool ok = SetForegroundWindow(root) != FALSE;
    AttachThreadInput(currentThread, foregroundThread, FALSE);
    return ok;
}

void WinPasteService::tick() {
    if (GetClipboardSequenceNumber() != sequence_) {
        finish(Outcome::Cancelled, QStringLiteral("clipboard changed before focus returned"));
        return;
    }
    if (!IsWindow(target_)) {
        finish(Outcome::Cancelled, QStringLiteral("target window closed"));
        return;
    }
    HWND foreground = GetForegroundWindow();
    const bool targetIsForeground = WinProcess::rootOwner(foreground) == WinProcess::rootOwner(target_);
    const bool deckHoldsKeyboard = deck_ && IsWindowVisible(deck_);
    if (canDeliverPaste(targetIsForeground, deckHoldsKeyboard)) {
        const QString keys = sendPasteKeystroke();
        Q_EMIT keystrokeSent();
        finish(Outcome::Pasted, QStringLiteral("focus ready after %1 ms; %2").arg(elapsed_.elapsed()).arg(keys));
        return;
    }
    if (elapsed_.elapsed() >= 2000) {
        finish(Outcome::TimedOut, QStringLiteral("focus not ready; foreground={%1} deckVisible=%2")
                                      .arg(WinProcess::describeWindow(foreground))
                                      .arg(deckHoldsKeyboard));
    }
}

QString WinPasteService::sendPasteKeystroke() {
    std::vector<INPUT> inputs;
    auto key = [&inputs](WORD virtualKey, bool up) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = virtualKey;
        input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        if (virtualKey == VK_LWIN || virtualKey == VK_RWIN || virtualKey == VK_RMENU || virtualKey == VK_RCONTROL) {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
        inputs.push_back(input);
    };

    // Ctrl goes down before any held modifier is released, so a lone Alt-up
    // cannot open the target's menu bar and a lone Win-up cannot open Start.
    // The global hot key (Alt+Shift+V) is usually still held at this point.
    const bool ctrlHeld = isKeyDown(VK_CONTROL);
    if (!ctrlHeld) key(VK_CONTROL, false);
    QStringList released;
    const struct { WORD key; const char *name; } modifiers[] = {
        {VK_LSHIFT, "LShift"}, {VK_RSHIFT, "RShift"}, {VK_LMENU, "LAlt"},
        {VK_RMENU, "RAlt"},    {VK_LWIN, "LWin"},     {VK_RWIN, "RWin"},
    };
    for (const auto &modifier : modifiers) {
        if (isKeyDown(modifier.key)) {
            key(modifier.key, true);
            released << QLatin1String(modifier.name);
        }
    }
    key('V', false);
    key('V', true);
    if (!ctrlHeld) key(VK_CONTROL, true);

    const UINT sent = SendInput(UINT(inputs.size()), inputs.data(), sizeof(INPUT));
    return QStringLiteral("SendInput %1/%2, ctrlHeld=%3, released=[%4]")
        .arg(sent).arg(inputs.size()).arg(ctrlHeld).arg(released.join(QLatin1Char(',')));
}

void WinPasteService::finish(Outcome outcome, const QString &detail) {
    poll_.stop();
    qInfo() << "[ClipDeck][Paste]" << int(outcome) << detail;
    Q_EMIT finished(outcome, detail);
}
