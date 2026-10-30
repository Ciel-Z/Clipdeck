#include "win_global_hotkey.h"

#include <QCoreApplication>
#include <QSettings>
#include <QStringList>

#include <atomic>

#include <windows.h>

namespace {

std::atomic<int> nextHotkeyId{0xC11D};

QString keyName(quint32 key) {
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return QString(QChar(char16_t(key)));
    if (key >= VK_F1 && key <= VK_F24) return QStringLiteral("F%1").arg(key - VK_F1 + 1);
    switch (key) {
    case VK_LEFT: return QStringLiteral("←");
    case VK_RIGHT: return QStringLiteral("→");
    case VK_UP: return QStringLiteral("↑");
    case VK_DOWN: return QStringLiteral("↓");
    case VK_SPACE: return QStringLiteral("Space");
    case VK_RETURN: return QStringLiteral("Enter");
    case VK_TAB: return QStringLiteral("Tab");
    case VK_OEM_3: return QStringLiteral("`");
    case VK_OEM_MINUS: return QStringLiteral("-");
    case VK_OEM_PLUS: return QStringLiteral("=");
    default: return QStringLiteral("0x%1").arg(key, 2, 16, QLatin1Char('0'));
    }
}

quint32 keyFromName(const QString &name) {
    if (name.size() == 1) {
        const QChar c = name.at(0).toUpper();
        if ((c >= QLatin1Char('A') && c <= QLatin1Char('Z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))) {
            return quint32(c.unicode());
        }
        if (c == QLatin1Char('`')) return VK_OEM_3;
        if (c == QLatin1Char('-')) return VK_OEM_MINUS;
        if (c == QLatin1Char('=')) return VK_OEM_PLUS;
    }
    if (name.size() >= 2 && name.at(0).toUpper() == QLatin1Char('F')) {
        bool ok = false;
        const int number = name.mid(1).toInt(&ok);
        if (ok && number >= 1 && number <= 24) return quint32(VK_F1 + number - 1);
    }
    if (name == QStringLiteral("←")) return VK_LEFT;
    if (name == QStringLiteral("→")) return VK_RIGHT;
    if (name == QStringLiteral("↑")) return VK_UP;
    if (name == QStringLiteral("↓")) return VK_DOWN;
    const QString lower = name.toLower();
    if (lower == QLatin1String("left")) return VK_LEFT;
    if (lower == QLatin1String("right")) return VK_RIGHT;
    if (lower == QLatin1String("up")) return VK_UP;
    if (lower == QLatin1String("down")) return VK_DOWN;
    if (lower == QLatin1String("space")) return VK_SPACE;
    if (lower == QLatin1String("enter")) return VK_RETURN;
    if (lower == QLatin1String("tab")) return VK_TAB;
    if (lower.startsWith(QLatin1String("0x"))) {
        bool ok = false;
        const uint value = lower.mid(2).toUInt(&ok, 16);
        if (ok && value > 0 && value < 0xFF) return value;
    }
    return 0;
}

} // namespace

QString HotkeySpec::toString() const {
    QStringList parts;
    if (modifiers & Control) parts << QStringLiteral("Ctrl");
    if (modifiers & Alt) parts << QStringLiteral("Alt");
    if (modifiers & Shift) parts << QStringLiteral("Shift");
    if (modifiers & Win) parts << QStringLiteral("Win");
    parts << keyName(key);
    return parts.join(QLatin1Char('+'));
}

HotkeySpec HotkeySpec::parse(const QString &text) {
    HotkeySpec spec;
    spec.modifiers = 0;
    spec.key = 0;
    const QStringList parts = text.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    for (const QString &raw : parts) {
        const QString part = raw.trimmed().toLower();
        if (part == QLatin1String("ctrl") || part == QLatin1String("control")) spec.modifiers |= Control;
        else if (part == QLatin1String("alt")) spec.modifiers |= Alt;
        else if (part == QLatin1String("shift")) spec.modifiers |= Shift;
        else if (part == QLatin1String("win") || part == QLatin1String("meta")) spec.modifiers |= Win;
        else if (spec.key == 0) spec.key = keyFromName(raw.trimmed());
        else return HotkeySpec{0, 0};
    }
    return spec.isValid() ? spec : HotkeySpec{0, 0};
}

HotkeySpec HotkeySpec::load(const QString &name, const HotkeySpec &fallback) {
    QSettings settings;
    HotkeySpec spec;
    spec.modifiers = settings.value(QStringLiteral("shortcuts/%1/modifiers").arg(name), fallback.modifiers).toUInt();
    spec.key = settings.value(QStringLiteral("shortcuts/%1/key").arg(name), fallback.key).toUInt();
    return spec.isValid() ? spec : fallback;
}

void HotkeySpec::save(const QString &name) const {
    QSettings settings;
    settings.setValue(QStringLiteral("shortcuts/%1/modifiers").arg(name), modifiers);
    settings.setValue(QStringLiteral("shortcuts/%1/key").arg(name), key);
}

namespace {

quint32 virtualKeyFromQt(int qtKey) {
    if ((qtKey >= Qt::Key_A && qtKey <= Qt::Key_Z) || (qtKey >= Qt::Key_0 && qtKey <= Qt::Key_9)) return quint32(qtKey);
    if (qtKey >= Qt::Key_F1 && qtKey <= Qt::Key_F24) return quint32(VK_F1 + (qtKey - Qt::Key_F1));
    switch (qtKey) {
    case Qt::Key_Left: return VK_LEFT;
    case Qt::Key_Right: return VK_RIGHT;
    case Qt::Key_Up: return VK_UP;
    case Qt::Key_Down: return VK_DOWN;
    case Qt::Key_Space: return VK_SPACE;
    case Qt::Key_Return:
    case Qt::Key_Enter: return VK_RETURN;
    case Qt::Key_Tab: return VK_TAB;
    case Qt::Key_QuoteLeft: return VK_OEM_3;
    case Qt::Key_Minus: return VK_OEM_MINUS;
    case Qt::Key_Equal: return VK_OEM_PLUS;
    default: return 0;
    }
}

quint32 modifiersFromQt(int qtModifiers) {
    quint32 modifiers = 0;
    if (qtModifiers & Qt::ControlModifier) modifiers |= HotkeySpec::Control;
    if (qtModifiers & Qt::AltModifier) modifiers |= HotkeySpec::Alt;
    if (qtModifiers & Qt::ShiftModifier) modifiers |= HotkeySpec::Shift;
    if (qtModifiers & Qt::MetaModifier) modifiers |= HotkeySpec::Win;
    return modifiers;
}

} // namespace

HotkeySpec HotkeySpec::fromQtKey(int qtKey, int qtModifiers) {
    HotkeySpec spec{modifiersFromQt(qtModifiers), virtualKeyFromQt(qtKey)};
    return spec.isValid() ? spec : HotkeySpec{0, 0};
}

bool HotkeySpec::matchesQtKey(int qtKey, int qtModifiers) const {
    return isValid() && key == virtualKeyFromQt(qtKey) && modifiers == modifiersFromQt(qtModifiers);
}

WinGlobalHotkey::WinGlobalHotkey(QObject *parent) : QObject(parent), id_(nextHotkeyId++) {
    if (QCoreApplication *application = QCoreApplication::instance()) application->installNativeEventFilter(this);
}

WinGlobalHotkey::~WinGlobalHotkey() {
    unregisterHotkey();
    if (QCoreApplication *application = QCoreApplication::instance()) application->removeNativeEventFilter(this);
}

bool WinGlobalHotkey::registerHotkey(const HotkeySpec &spec) {
    unregisterHotkey();
    lastError_.clear();
    if (!spec.isValid()) {
        lastError_ = QStringLiteral("无效的快捷键");
        return false;
    }
    if (!RegisterHotKey(nullptr, id_, UINT(spec.modifiers) | MOD_NOREPEAT, UINT(spec.key))) {
        const DWORD error = GetLastError();
        lastError_ = error == ERROR_HOTKEY_ALREADY_REGISTERED
            ? QStringLiteral("%1 已被其他程序占用").arg(spec.toString())
            : QStringLiteral("注册 %1 失败 (%2)").arg(spec.toString()).arg(error);
        return false;
    }
    spec_ = spec;
    registered_ = true;
    return true;
}

void WinGlobalHotkey::unregisterHotkey() {
    if (!registered_) return;
    UnregisterHotKey(nullptr, id_);
    registered_ = false;
}

bool WinGlobalHotkey::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *) {
    if (eventType != "windows_generic_MSG") return false;
    const auto *msg = static_cast<const MSG *>(message);
    if (msg->message != WM_HOTKEY || int(msg->wParam) != id_) return false;
    Q_EMIT triggered();
    return true;
}
