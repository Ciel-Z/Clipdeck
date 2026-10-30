#include "global_hotkey.h"

#include "spike_log.h"

#include <windows.h>

namespace {
constexpr int HotkeyId = 0xC11D;
}

GlobalHotkey::GlobalHotkey(QObject *parent) : QObject(parent) {}

GlobalHotkey::~GlobalHotkey() { unregister(); }

QString GlobalHotkey::registerDefault() {
    unregister();
    struct Candidate {
        UINT modifiers;
        const char *name;
    };
    const Candidate candidates[] = {
        {MOD_ALT | MOD_SHIFT, "Alt+Shift+V"},
        {MOD_CONTROL | MOD_SHIFT, "Ctrl+Shift+V"},
    };
    for (const auto &candidate : candidates) {
        if (RegisterHotKey(nullptr, HotkeyId, candidate.modifiers | MOD_NOREPEAT, 'V')) {
            registered_ = true;
            spike::log(QStringLiteral("hotkey"), QStringLiteral("registered %1").arg(QLatin1String(candidate.name)));
            return QLatin1String(candidate.name);
        }
        spike::log(QStringLiteral("hotkey"), QStringLiteral("failed %1, error %2")
                                                 .arg(QLatin1String(candidate.name))
                                                 .arg(GetLastError()));
    }
    return QString();
}

void GlobalHotkey::unregister() {
    if (!registered_) return;
    UnregisterHotKey(nullptr, HotkeyId);
    registered_ = false;
}

bool GlobalHotkey::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *) {
    if (eventType != "windows_generic_MSG") return false;
    const auto *msg = static_cast<const MSG *>(message);
    if (msg->message != WM_HOTKEY || msg->wParam != HotkeyId) return false;
    emit triggered();
    return true;
}
