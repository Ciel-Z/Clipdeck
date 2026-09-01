#include "x11_global_shortcut.h"

#include <QByteArray>
#include <QDebug>
#include <QGuiApplication>
#include <QX11Info>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>

#include <utility>

KeySym keySymForQtKey(int key) {
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35) return XK_F1 + (key - Qt::Key_F1);
    if (key >= Qt::Key_A && key <= Qt::Key_Z) return XK_A + (key - Qt::Key_A);
    if (key >= Qt::Key_0 && key <= Qt::Key_9) return XK_0 + (key - Qt::Key_0);
    switch (key) {
    case Qt::Key_Escape: return XK_Escape;
    case Qt::Key_Tab: return XK_Tab;
    case Qt::Key_Backtab: return XK_ISO_Left_Tab;
    case Qt::Key_Backspace: return XK_BackSpace;
    case Qt::Key_Return: return XK_Return;
    case Qt::Key_Enter: return XK_KP_Enter;
    case Qt::Key_Insert: return XK_Insert;
    case Qt::Key_Delete: return XK_Delete;
    case Qt::Key_Pause: return XK_Pause;
    case Qt::Key_Print: return XK_Print;
    case Qt::Key_Home: return XK_Home;
    case Qt::Key_End: return XK_End;
    case Qt::Key_Left: return XK_Left;
    case Qt::Key_Up: return XK_Up;
    case Qt::Key_Right: return XK_Right;
    case Qt::Key_Down: return XK_Down;
    case Qt::Key_PageUp: return XK_Page_Up;
    case Qt::Key_PageDown: return XK_Page_Down;
    case Qt::Key_Space: return XK_space;
    default: break;
    }
    if (key >= 0x21 && key <= 0x7e) {
        const QByteArray name(1, static_cast<char>(key));
        return XStringToKeysym(name.constData());
    }
    return NoSymbol;
}

GlobalShortcutFilter::GlobalShortcutFilter(std::function<void()> callback,
                                           NativeKeyCallback nativeKeyCallback,
                                           ShortcutProvider shortcutProvider)
    : callback_(std::move(callback)),
      nativeKeyCallback_(std::move(nativeKeyCallback)),
      shortcutProvider_(std::move(shortcutProvider)),
      display_(QX11Info::display()) {
    if (QGuiApplication::platformName() != QStringLiteral("xcb") || !display_) {
        display_ = nullptr;
        return;
    }
    for (int index = 0; index < ScreenCount(display_); ++index) {
        roots_.push_back(RootWindow(display_, index));
    }
    Bool detectable = False;
    XkbSetDetectableAutoRepeat(display_, True, &detectable);
    detectableAutoRepeat_ = detectable == True;
    configure();
}

GlobalShortcutFilter::~GlobalShortcutFilter() {
    unregisterShortcut();
}

void GlobalShortcutFilter::suspend() {
    unregisterShortcut();
}

void GlobalShortcutFilter::configure() {
    if (!display_ || !shortcutProvider_) return;
    unregisterShortcut();
    const QKeySequence sequence = shortcutProvider_();
    if (sequence.isEmpty()) return;
    const int value = sequence[0];
    modifiers_ = 0;
    if (value & Qt::SHIFT) modifiers_ |= ShiftMask;
    if (value & Qt::CTRL) modifiers_ |= ControlMask;
    if (value & Qt::ALT) modifiers_ |= Mod1Mask;
    if (value & Qt::META) modifiers_ |= Mod4Mask;
    const int key = value & ~Qt::KeyboardModifierMask;
    const KeySym symbol = keySymForQtKey(key);
    keycode_ = symbol == NoSymbol ? 0 : XKeysymToKeycode(display_, symbol);
    if (keycode_ == 0) {
        qWarning().noquote() << "[ClipDeck][Shortcut] unsupported sequence:"
                             << sequence.toString(QKeySequence::PortableText);
        return;
    }
    ignoredLockMask_ = LockMask | modifierMaskForKeySym(XK_Num_Lock)
        | modifierMaskForKeySym(XK_Scroll_Lock);
    for (const Window root : roots_) {
        for (const unsigned int locks : ignoredModifierVariants()) {
            XGrabKey(display_, keycode_, modifiers_ | locks, root, True,
                     GrabModeAsync, GrabModeAsync);
        }
    }
    XSync(display_, False);
    qInfo().noquote() << "[ClipDeck][Shortcut] registered"
                      << sequence.toString(QKeySequence::PortableText)
                      << "keycode=" << keycode_ << "modifiers=" << modifiers_
                      << "ignoredLocks=" << ignoredLockMask_
                      << "detectableAutoRepeat=" << detectableAutoRepeat_;
}

bool GlobalShortcutFilter::nativeEventFilter(const QByteArray &eventType, void *message, long *) {
    if (eventType != QByteArrayLiteral("xcb_generic_event_t")) return false;
    const auto *event = static_cast<xcb_generic_event_t *>(message);
    const uint8_t responseType = event->response_type & 0x7f;
    if (responseType == XCB_KEY_RELEASE) {
        const auto *keyEvent = reinterpret_cast<const xcb_key_release_event_t *>(event);
        if (keyEvent->detail == keycode_) {
            if (detectableAutoRepeat_) shortcutPressed_ = false;
            else lastReleaseTime_ = keyEvent->time;
        }
        return false;
    }
    if (responseType != XCB_KEY_PRESS) return false;
    const auto *keyEvent = reinterpret_cast<const xcb_key_press_event_t *>(event);
    if (nativeKeyCallback_ && nativeKeyCallback_(keyEvent)) return true;
    const unsigned int relevantState = keyEvent->state
        & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
    if (keyEvent->detail != keycode_ || relevantState != modifiers_) return false;
    if (!detectableAutoRepeat_) {
        if (lastReleaseTime_ == keyEvent->time && shortcutPressed_) return true;
        if (lastReleaseTime_ != 0 && lastReleaseTime_ != keyEvent->time) shortcutPressed_ = false;
    }
    if (shortcutPressed_) return true;
    shortcutPressed_ = true;
    callback_();
    return true;
}

unsigned int GlobalShortcutFilter::modifierMaskForKeySym(KeySym symbol) const {
    if (!display_) return 0;
    const KeyCode target = XKeysymToKeycode(display_, symbol);
    if (target == 0) return 0;
    XModifierKeymap *mapping = XGetModifierMapping(display_);
    if (!mapping) return 0;
    unsigned int result = 0;
    for (int modifier = 0; modifier < 8; ++modifier) {
        for (int slot = 0; slot < mapping->max_keypermod; ++slot) {
            if (mapping->modifiermap[modifier * mapping->max_keypermod + slot] == target) {
                result |= 1u << modifier;
            }
        }
    }
    XFreeModifiermap(mapping);
    return result;
}

std::vector<unsigned int> GlobalShortcutFilter::ignoredModifierVariants() const {
    std::vector<unsigned int> variants;
    unsigned int subset = ignoredLockMask_;
    while (true) {
        variants.push_back(subset);
        if (subset == 0) break;
        subset = (subset - 1) & ignoredLockMask_;
    }
    return variants;
}

void GlobalShortcutFilter::unregisterShortcut() {
    if (!display_ || keycode_ == 0) return;
    for (const Window root : roots_) {
        for (const unsigned int locks : ignoredModifierVariants()) {
            XUngrabKey(display_, keycode_, modifiers_ | locks, root);
        }
    }
    XSync(display_, False);
    keycode_ = 0;
    shortcutPressed_ = false;
    lastReleaseTime_ = 0;
}
