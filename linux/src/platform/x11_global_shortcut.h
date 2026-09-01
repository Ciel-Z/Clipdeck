#pragma once

#include <QAbstractNativeEventFilter>
#include <QByteArray>
#include <QKeySequence>

#include <xcb/xcb.h>

#include <functional>
#include <vector>

typedef unsigned long KeySym;
typedef unsigned long Window;
typedef struct _XDisplay Display;

KeySym keySymForQtKey(int key);

class GlobalShortcutFilter final : public QAbstractNativeEventFilter {
public:
    using ShortcutProvider = std::function<QKeySequence()>;
    using NativeKeyCallback = std::function<bool(const xcb_key_press_event_t *)>;

    GlobalShortcutFilter(std::function<void()> callback,
                         NativeKeyCallback nativeKeyCallback,
                         ShortcutProvider shortcutProvider);
    ~GlobalShortcutFilter() override;

    void suspend();
    void configure();
    bool nativeEventFilter(const QByteArray &eventType, void *message, long *result) override;

private:
    unsigned int modifierMaskForKeySym(KeySym symbol) const;
    std::vector<unsigned int> ignoredModifierVariants() const;
    void unregisterShortcut();

    std::function<void()> callback_;
    NativeKeyCallback nativeKeyCallback_;
    ShortcutProvider shortcutProvider_;
    Display *display_ = nullptr;
    int keycode_ = 0;
    unsigned int modifiers_ = 0;
    unsigned int ignoredLockMask_ = 1u << 1;
    bool shortcutPressed_ = false;
    bool detectableAutoRepeat_ = false;
    xcb_timestamp_t lastReleaseTime_ = 0;
    std::vector<Window> roots_;
};
