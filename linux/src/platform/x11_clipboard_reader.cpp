#include "x11_clipboard_reader.h"

#include <QDebug>
#include <QUrl>
#include <cstdlib>
#include <cstring>

namespace {
constexpr int maximumTransferBytes = 32 * 1024 * 1024;
}

X11ClipboardReader::X11ClipboardReader(std::function<void(ClipboardSnapshot)> completed)
    : completed_(std::move(completed)) {}

X11ClipboardReader::~X11ClipboardReader() {
    if (connection_) xcb_disconnect(connection_);
}

xcb_atom_t X11ClipboardReader::atom(const char *name) {
    const auto cookie = xcb_intern_atom(connection_, false, std::strlen(name), name);
    auto *reply = xcb_intern_atom_reply(connection_, cookie, nullptr);
    const auto result = reply ? reply->atom : xcb_atom_t(XCB_NONE);
    std::free(reply);
    return result;
}

void X11ClipboardReader::start() {
    int screenIndex = 0;
    connection_ = xcb_connect(nullptr, &screenIndex);
    if (!connection_ || xcb_connection_has_error(connection_)) {
        qWarning() << "[ClipDeck][Capture] X11 reader unavailable";
        return;
    }
    auto screen = xcb_setup_roots_iterator(xcb_get_setup(connection_));
    while (screenIndex-- > 0 && screen.rem) xcb_screen_next(&screen);
    if (!screen.rem) return;
    root_ = screen.data->root;
    clipboard_ = atom("CLIPBOARD"); property_ = atom("_CLIPDECK_CAPTURE");
    targets_ = atom("TARGETS"); incr_ = atom("INCR"); utf8_ = atom("UTF8_STRING");
    plainUtf8_ = atom("text/plain;charset=utf-8"); plain_ = atom("text/plain");
    html_ = atom("text/html"); png_ = atom("image/png"); uri_ = atom("text/uri-list");
    activeWindow_ = atom("_NET_ACTIVE_WINDOW");
    timestamp_ = atom("TIMESTAMP");
    notifier_ = new QSocketNotifier(xcb_get_file_descriptor(connection_), QSocketNotifier::Read, this);
    QObject::connect(notifier_, &QSocketNotifier::activated, this, [this](int) { drain(); });
    timeout_ = new QTimer(this);
    timeout_->setSingleShot(true);
    QObject::connect(timeout_, &QTimer::timeout, this, [this] {
        qWarning() << "[ClipDeck][Capture] selection transfer timed out seq=" << snapshot_.sequence;
        cancel();
    });
    retiredTimer_ = new QTimer(this);
    retiredTimer_->setInterval(1000);
    QObject::connect(retiredTimer_, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto it = retiredWindows_.begin(); it != retiredWindows_.end();) {
            if (it.value() <= now) {
                xcb_destroy_window(connection_, it.key());
                it = retiredWindows_.erase(it);
            } else ++it;
        }
        xcb_flush(connection_);
        if (retiredWindows_.isEmpty()) retiredTimer_->stop();
    });
}

void X11ClipboardReader::cancel() {
    if (timeout_) timeout_->stop();
    if (connection_ && window_) {
        // Ownership can change before the previous owner handles our request.
        // Keep its requestor alive briefly, draining/discarding late transfers
        // instead of causing BadWindow errors inside the source application.
        if (retiredWindows_.size() >= 256) {
            auto oldest = retiredWindows_.begin();
            for (auto it = retiredWindows_.begin(); it != retiredWindows_.end(); ++it)
                if (it.value() < oldest.value()) oldest = it;
            xcb_destroy_window(connection_, oldest.key());
            retiredWindows_.erase(oldest);
        }
        retiredWindows_.insert(window_, QDateTime::currentMSecsSinceEpoch() + 4000);
        retiredTimer_->start();
    }
    window_ = XCB_NONE;
    chunks_.clear();
    incremental_ = false;
    offered_.clear();
    ownerTimestamp_ = 0;
}

void X11ClipboardReader::capture(quint64 sequence, const QDateTime &time, bool explicitChange,
                                 bool ignoreOwner, bool baseline, bool onlyIfIdle) {
    if (!connection_ || !root_) return;
    if (onlyIfIdle && window_) return;
    cancel();
    // The UI already identified this as our own explicit history write. Do
    // not inspect/ignore a potentially newer owner's window on this thread.
    if (ignoreOwner) return;
    auto *owner = xcb_get_selection_owner_reply(connection_,
        xcb_get_selection_owner(connection_, clipboard_), nullptr);
    const xcb_window_t ownerWindow = owner ? owner->owner : xcb_window_t(XCB_NONE);
    std::free(owner);
    if (!ownerWindow) return;
    snapshot_ = {};
    snapshot_.sequence = sequence;
    snapshot_.copiedAt = time;
    snapshot_.ownerWindow = ownerWindow;
    snapshot_.explicitChange = explicitChange;
    snapshot_.baseline = baseline;
    auto *active = xcb_get_property_reply(connection_,
        xcb_get_property(connection_, false, root_, activeWindow_, XCB_ATOM_WINDOW, 0, 1), nullptr);
    if (active && active->format == 32 && xcb_get_property_value_length(active) >= 4) {
        std::memcpy(&snapshot_.sourceWindow, xcb_get_property_value(active), 4);
    }
    std::free(active);
    window_ = xcb_generate_id(connection_);
    const quint32 mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_create_window(connection_, XCB_COPY_FROM_PARENT, window_, root_, 0, 0, 1, 1, 0,
                      XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &mask);
    timeout_->start(4000);
    request(Part::Targets, targets_);
}

void X11ClipboardReader::request(Part part, xcb_atom_t target) {
    part_ = part;
    target_ = target;
    incremental_ = false;
    chunks_.clear();
    // CurrentTime avoids reusing a GUI event timestamp from an earlier owner.
    xcb_convert_selection(connection_, window_, clipboard_, target, property_, XCB_CURRENT_TIME);
    xcb_flush(connection_);
    drain();
}

void X11ClipboardReader::drain() {
    if (!connection_) return;
    while (auto *event = xcb_poll_for_event(connection_)) {
        const int type = event->response_type & 0x7f;
        if (type == XCB_SELECTION_NOTIFY) {
            auto *selection = reinterpret_cast<xcb_selection_notify_event_t *>(event);
            if (retiredWindows_.contains(selection->requestor) && selection->property != XCB_NONE) {
                xcb_delete_property(connection_, selection->requestor, selection->property);
                xcb_flush(connection_);
            }
            if (window_ && selection->requestor == window_ && selection->target == target_) {
                if (selection->property == XCB_NONE) {
                    if (part_ == Part::Targets) {
                        offered_ = {utf8_, XCB_ATOM_STRING};
                        request(Part::Text, utf8_);
                    } else if (part_ == Part::Text && target_ == utf8_) {
                        request(Part::Text, XCB_ATOM_STRING);
                    } else if (part_ == Part::TimestampAfter) { cancel(); }
                    else { next(); }
                } else { readProperty(); }
            }
        } else if (type == XCB_PROPERTY_NOTIFY) {
            auto *property = reinterpret_cast<xcb_property_notify_event_t *>(event);
            if (retiredWindows_.contains(property->window)
                && property->atom == property_ && property->state == XCB_PROPERTY_NEW_VALUE) {
                xcb_delete_property(connection_, property->window, property_);
                xcb_flush(connection_);
            }
            if (window_ && property->window == window_ && property->atom == property_
                    && property->state == XCB_PROPERTY_NEW_VALUE && incremental_) readProperty();
        }
        std::free(event);
    }
}

void X11ClipboardReader::readProperty() {
    auto *reply = xcb_get_property_reply(connection_, xcb_get_property(connection_, true,
        window_, property_, XCB_GET_PROPERTY_TYPE_ANY, 0, maximumTransferBytes / 4 + 1), nullptr);
    if (!reply) { cancel(); return; }
    const auto type = reply->type;
    const auto format = reply->format;
    const int length = xcb_get_property_value_length(reply);
    const bool tooLarge = reply->bytes_after > 0 || length > maximumTransferBytes;
    QByteArray data;
    if (!tooLarge) data = QByteArray(static_cast<const char *>(xcb_get_property_value(reply)), length);
    std::free(reply);
    xcb_flush(connection_);
    if (tooLarge) { cancel(); return; }
    if (type == incr_) {
        incremental_ = true;
        return;
    }
    if (incremental_) {
        if (!data.isEmpty()) {
            if (chunks_.size() > maximumTransferBytes - data.size()) { cancel(); return; }
            chunks_ += data;
            return;
        }
        data = std::move(chunks_);
        incremental_ = false;
    }
    accept(std::move(data), type, format);
}

void X11ClipboardReader::accept(QByteArray bytes, xcb_atom_t, quint8 format) {
    if (part_ == Part::Targets) {
        if (format != 32 || bytes.size() % 4 || bytes.size() > 64 * 1024) { cancel(); return; }
        offered_.resize(bytes.size() / 4);
        std::memcpy(offered_.data(), bytes.constData(), bytes.size());
        if (offered_.contains(timestamp_)) request(Part::TimestampBefore, timestamp_);
        else next();
        return;
    }
    if (part_ == Part::TimestampBefore || part_ == Part::TimestampAfter) {
        quint32 timestamp = 0;
        if (format == 32 && bytes.size() == 4) std::memcpy(&timestamp, bytes.constData(), 4);
        if (part_ == Part::TimestampAfter && (!timestamp || timestamp != ownerTimestamp_)) {
            // Same owner window can serve a new cut while HTML/INCR is still
            // transferring. Never combine payloads from different revisions.
            cancel();
            return;
        }
        if (part_ == Part::TimestampBefore) ownerTimestamp_ = timestamp;
    } else if (part_ == Part::Uri) {
        for (const auto &line : bytes.split('\n')) {
            if (QUrl::fromEncoded(line.trimmed()).isLocalFile()) { cancel(); return; }
        }
    } else if (part_ == Part::Text) {
        snapshot_.latin1 = target_ == XCB_ATOM_STRING;
        snapshot_.text = std::move(bytes);
    } else if (part_ == Part::Html) { snapshot_.html = std::move(bytes); }
    else if (part_ == Part::Png) { snapshot_.png = std::move(bytes); }
    next();
}

void X11ClipboardReader::next() {
    if (part_ == Part::Targets || part_ == Part::TimestampBefore) {
        if (offered_.contains(uri_)) { request(Part::Uri, uri_); return; }
        if (offered_.contains(png_)) { request(Part::Png, png_); return; }
        request(Part::Text, offered_.contains(utf8_) ? utf8_
            : offered_.contains(plainUtf8_) ? plainUtf8_
            : offered_.contains(plain_) ? plain_ : xcb_atom_t(XCB_ATOM_STRING));
        return;
    }
    if (part_ == Part::Uri) {
        if (offered_.contains(png_)) { request(Part::Png, png_); return; }
        request(Part::Text, offered_.contains(utf8_) ? utf8_
            : offered_.contains(plainUtf8_) ? plainUtf8_
            : offered_.contains(plain_) ? plain_ : xcb_atom_t(XCB_ATOM_STRING));
        return;
    }
    if (part_ == Part::Png && snapshot_.png.isEmpty()) {
        request(Part::Text, utf8_); return;
    }
    if (part_ == Part::Text && !snapshot_.text.isEmpty() && offered_.contains(html_)) {
        request(Part::Html, html_); return;
    }
    finish();
}

void X11ClipboardReader::finish() {
    if (ownerTimestamp_ && part_ != Part::TimestampAfter) {
        request(Part::TimestampAfter, timestamp_);
        return;
    }
    auto *owner = xcb_get_selection_owner_reply(connection_,
        xcb_get_selection_owner(connection_, clipboard_), nullptr);
    const bool current = owner && owner->owner == snapshot_.ownerWindow;
    std::free(owner);
    ClipboardSnapshot completed = std::move(snapshot_);
    cancel();
    if (current && (!completed.text.isEmpty() || !completed.png.isEmpty())) {
        completed_(std::move(completed));
    }
}
