#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QString>

// Modifier bits are the Win32 MOD_* values; key is a virtual-key code.
struct HotkeySpec {
    enum Modifier : quint32 { Alt = 0x1, Control = 0x2, Shift = 0x4, Win = 0x8 };

    quint32 modifiers = Alt | Shift; // mac default is Cmd+Shift+V; Ctrl+Shift+V clashes with "paste as text"
    quint32 key = 'V';

    bool isValid() const { return key != 0 && (modifiers & (Alt | Control | Shift | Win)) != 0; }
    QString toString() const;
    static HotkeySpec parse(const QString &text); // invalid spec on failure
    bool operator==(const HotkeySpec &other) const { return modifiers == other.modifiers && key == other.key; }

    // shortcuts/<name>/* in QSettings, like AppPreferences.*KeyCode/*Modifiers on mac.
    static HotkeySpec load(const QString &name, const HotkeySpec &fallback);
    void save(const QString &name) const;
    // shortcuts/showDeck: the global "启动 ClipDeck" shortcut.
    static HotkeySpec load() { return load(QStringLiteral("showDeck"), HotkeySpec()); }
    void save() const { save(QStringLiteral("showDeck")); }

    // From a Qt key event (Qt::Key + Qt::KeyboardModifiers); invalid when the
    // key has no virtual-key equivalent or no modifier is held.
    static HotkeySpec fromQtKey(int qtKey, int qtModifiers);
    // Whether a Qt key event is this shortcut (the Deck's in-window shortcuts).
    bool matchesQtKey(int qtKey, int qtModifiers) const;
};

// Thread hot key (hwnd = nullptr): WM_HOTKEY arrives on the GUI thread queue,
// and the press grants this process the foreground right needed to focus Deck.
class WinGlobalHotkey final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    explicit WinGlobalHotkey(QObject *parent = nullptr);
    ~WinGlobalHotkey() override;

    bool registerHotkey(const HotkeySpec &spec);
    void unregisterHotkey();
    bool isRegistered() const { return registered_; }
    HotkeySpec current() const { return spec_; }
    QString lastError() const { return lastError_; }

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

Q_SIGNALS:
    void triggered();

private:
    HotkeySpec spec_;
    int id_;
    bool registered_ = false;
    QString lastError_;
};
