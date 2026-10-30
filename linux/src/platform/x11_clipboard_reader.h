#pragma once

#include "../history/clipboard_snapshot.h"

#include <QObject>
#include <QHash>
#include <QSocketNotifier>
#include <QTimer>
#include <QVector>
#include <functional>
#include <xcb/xcb.h>

// Lives on a dedicated I/O thread. It never owns or writes CLIPBOARD.
class X11ClipboardReader final : public QObject {
public:
    explicit X11ClipboardReader(std::function<void(ClipboardSnapshot)> completed);
    ~X11ClipboardReader() override;
    void start();
    void capture(quint64 sequence, const QDateTime &time, bool explicitChange,
                 bool ignoreOwner = false, bool baseline = false, bool onlyIfIdle = false);

private:
    enum class Part { Targets, TimestampBefore, Uri, Text, Html, Png, TimestampAfter };
    xcb_atom_t atom(const char *name);
    void cancel();
    void drain();
    void request(Part part, xcb_atom_t target);
    void readProperty();
    void accept(QByteArray bytes, xcb_atom_t type, quint8 format);
    void next();
    void finish();

    std::function<void(ClipboardSnapshot)> completed_;
    xcb_connection_t *connection_ = nullptr;
    xcb_window_t root_ = XCB_NONE;
    xcb_window_t window_ = XCB_NONE;
    xcb_atom_t clipboard_ = XCB_NONE;
    xcb_atom_t property_ = XCB_NONE;
    xcb_atom_t targets_ = XCB_NONE;
    xcb_atom_t incr_ = XCB_NONE;
    xcb_atom_t utf8_ = XCB_NONE;
    xcb_atom_t plainUtf8_ = XCB_NONE;
    xcb_atom_t plain_ = XCB_NONE;
    xcb_atom_t html_ = XCB_NONE;
    xcb_atom_t png_ = XCB_NONE;
    xcb_atom_t uri_ = XCB_NONE;
    xcb_atom_t copiedFiles_ = XCB_NONE;
    xcb_atom_t activeWindow_ = XCB_NONE;
    xcb_atom_t target_ = XCB_NONE;
    xcb_atom_t timestamp_ = XCB_NONE;
    quint32 ownerTimestamp_ = 0;
    QVector<xcb_atom_t> offered_;
    ClipboardSnapshot snapshot_;
    Part part_ = Part::Targets;
    bool incremental_ = false;
    QByteArray chunks_;
    QSocketNotifier *notifier_ = nullptr;
    QTimer *timeout_ = nullptr;
    QTimer *retiredTimer_ = nullptr;
    QHash<xcb_window_t, qint64> retiredWindows_;
};
