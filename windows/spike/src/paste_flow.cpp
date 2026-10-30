#include "paste_flow.h"

#include "spike_log.h"
#include "win32_util.h"

#include <QMetaEnum>
#include <QStringList>

#include <cstring>

#include <vector>

namespace {

QByteArray toRtf(const QString &text) {
    // Whole text in bold so "preserve formatting" is visible in WordPad/Word.
    QByteArray rtf = "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0 Segoe UI;}}\\f0{\\b ";
    for (const QChar character : text) {
        const ushort code = character.unicode();
        if (character == QLatin1Char('\\') || character == QLatin1Char('{') || character == QLatin1Char('}')) {
            rtf += '\\';
            rtf += char(code);
        } else if (character == QLatin1Char('\n')) {
            rtf += "\\par ";
        } else if (code < 0x80) {
            rtf += char(code);
        } else {
            rtf += "\\u" + QByteArray::number(short(code)) + '?';
        }
    }
    rtf += "}}";
    return rtf;
}

bool setClipboardBytes(UINT format, const void *bytes, size_t size) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!memory) return false;
    void *target = GlobalLock(memory);
    memcpy(target, bytes, size);
    GlobalUnlock(memory);
    if (SetClipboardData(format, memory)) return true;
    GlobalFree(memory);
    return false;
}

bool isKeyDown(int virtualKey) { return (GetAsyncKeyState(virtualKey) & 0x8000) != 0; }

} // namespace

PasteFlow::PasteFlow(QObject *parent) : QObject(parent) {
    poll_.setInterval(30);
    connect(&poll_, &QTimer::timeout, this, &PasteFlow::tick);
}

void PasteFlow::paste(const QString &text, bool plainText, const QString &trigger,
                      HWND target, HWND deck, const std::function<void()> &hideDeck) {
    poll_.stop();
    trigger_ = trigger;
    target_ = target;
    deck_ = deck;
    elapsed_.start();

    spike::log(QStringLiteral("paste"), QStringLiteral("trigger=%1 plain=%2 target={%3}")
                                            .arg(trigger, plainText ? QStringLiteral("yes") : QStringLiteral("no"),
                                                 spike::describeWindow(target)));

    if (!writeClipboard(text, plainText, deck)) {
        hideDeck();
        finish(Outcome::Failed, QStringLiteral("clipboard write failed, error %1").arg(GetLastError()));
        return;
    }
    sequence_ = GetClipboardSequenceNumber();

    if (!target || !IsWindow(target)) {
        hideDeck();
        finish(Outcome::CopiedOnlyNoTarget, QStringLiteral("no target window recorded"));
        return;
    }

    // SendInput is silently dropped by UIPI for higher-integrity targets, so
    // this has to be decided up front (the macOS accessibility-denied branch).
    DWORD targetProcess = 0;
    GetWindowThreadProcessId(target, &targetProcess);
    const long targetLevel = spike::integrityLevel(targetProcess);
    const long ownLevel = spike::integrityLevel(GetCurrentProcessId());
    if (targetLevel > ownLevel || targetLevel < 0) {
        hideDeck();
        finish(Outcome::CopiedOnlyElevatedTarget,
               QStringLiteral("target integrity %1 > own %2")
                   .arg(spike::integrityName(targetLevel), spike::integrityName(ownLevel)));
        return;
    }

    const bool focused = bringToFront(target);
    hideDeck();
    spike::log(QStringLiteral("paste"), QStringLiteral("SetForegroundWindow(target)=%1 after %2 ms")
                                            .arg(focused ? QStringLiteral("ok") : QStringLiteral("refused"))
                                            .arg(elapsed_.elapsed()));
    poll_.start();
    tick();
}

bool PasteFlow::writeClipboard(const QString &text, bool plainText, HWND owner) {
    bool opened = false;
    for (int attempt = 0; attempt < 5 && !opened; ++attempt) {
        opened = OpenClipboard(owner) != FALSE;
        if (!opened) Sleep(10);
    }
    if (!opened) return false;
    EmptyClipboard();
    const std::wstring wide = text.toStdWString();
    bool ok = setClipboardBytes(CF_UNICODETEXT, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
    if (ok && !plainText) {
        const QByteArray rtf = toRtf(text);
        ok = setClipboardBytes(RegisterClipboardFormatW(L"Rich Text Format"), rtf.constData(), size_t(rtf.size()) + 1);
    }
    CloseClipboard();
    return ok;
}

bool PasteFlow::bringToFront(HWND target) {
    HWND root = spike::rootOwner(target);
    if (IsIconic(root)) ShowWindow(root, SW_RESTORE);
    if (SetForegroundWindow(root)) return true;

    const DWORD foregroundThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const DWORD currentThread = GetCurrentThreadId();
    AttachThreadInput(currentThread, foregroundThread, TRUE);
    const bool ok = SetForegroundWindow(root) != FALSE;
    AttachThreadInput(currentThread, foregroundThread, FALSE);
    spike::log(QStringLiteral("paste"), QStringLiteral("AttachThreadInput fallback=%1").arg(ok));
    return ok;
}

void PasteFlow::tick() {
    if (GetClipboardSequenceNumber() != sequence_) {
        finish(Outcome::Cancelled, QStringLiteral("clipboard changed before focus returned"));
        return;
    }
    HWND foreground = GetForegroundWindow();
    const bool targetIsFront = spike::rootOwner(foreground) == spike::rootOwner(target_);
    const bool deckVisible = deck_ && IsWindowVisible(deck_);
    if (targetIsFront && !deckVisible) {
        const QString keys = sendPasteKeystroke();
        finish(Outcome::Pasted, QStringLiteral("focus ready after %1 ms; %2").arg(elapsed_.elapsed()).arg(keys));
        return;
    }
    if (elapsed_.elapsed() >= 2000) {
        finish(Outcome::TimedOut, QStringLiteral("focus not ready; foreground={%1} deckVisible=%2")
                                      .arg(spike::describeWindow(foreground))
                                      .arg(deckVisible));
    }
}

QString PasteFlow::sendPasteKeystroke() {
    std::vector<INPUT> inputs;
    auto key = [&inputs](WORD virtualKey, bool up) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = virtualKey;
        input.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        if (virtualKey == VK_LWIN || virtualKey == VK_RWIN || virtualKey == VK_RMENU
            || virtualKey == VK_RCONTROL) {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }
        inputs.push_back(input);
    };

    // Ctrl goes down before any held modifier is released, so a lone Alt-up
    // cannot open the target's menu bar and a lone Win-up cannot open Start.
    const bool ctrlHeld = isKeyDown(VK_CONTROL);
    if (!ctrlHeld) key(VK_CONTROL, false);

    QStringList released;
    const struct {
        WORD key;
        const char *name;
    } modifiers[] = {
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
        .arg(sent)
        .arg(inputs.size())
        .arg(ctrlHeld)
        .arg(released.join(QLatin1Char(',')));
}

void PasteFlow::finish(Outcome outcome, const QString &detail) {
    poll_.stop();
    spike::log(QStringLiteral("paste"), QStringLiteral("result=%1 trigger=%2 %3")
                                            .arg(QLatin1String(QMetaEnum::fromType<Outcome>().valueToKey(int(outcome))),
                                                 trigger_, detail));
    emit finished(outcome, detail);
}
