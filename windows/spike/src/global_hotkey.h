#pragma once

#include <QAbstractNativeEventFilter>
#include <QObject>
#include <QString>

// Thread hot key (hwnd = nullptr): WM_HOTKEY arrives on the GUI thread queue
// and grants this process the foreground right needed to focus the Deck.
class GlobalHotkey final : public QObject, public QAbstractNativeEventFilter {
    Q_OBJECT

public:
    explicit GlobalHotkey(QObject *parent = nullptr);
    ~GlobalHotkey() override;

    // Tries Alt+Shift+V first, then Ctrl+Shift+V so both candidates in the
    // plan can be compared on the same machine.
    QString registerDefault();
    void unregister();

    bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

signals:
    void triggered();

private:
    bool registered_ = false;
};
