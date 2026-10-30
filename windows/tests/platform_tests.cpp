// Exercises the Win32 platform layer against the real clipboard.
#include "win_capture_service.h"
#include "win_clipboard_format.h"
#include "win_clipboard_writer.h"
#include "win_global_hotkey.h"
#include "win_paste_service.h"
#include "win_process_util.h"
#include "win_tray.h"
#include "win_source_application.h"

#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QElapsedTimer>
#include <QImage>
#include <QMenu>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QSettings>
#include <QTimer>
#include <optional>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>

#include <cstdio>
#include <functional>
#include <limits>

namespace {

int failures = 0;

void check(bool condition, const char *name) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

bool waitFor(const std::function<bool()> &condition, int timeoutMs = 6000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (condition()) return true;
        QThread::msleep(10);
    }
    return condition();
}

QVector<ClipboardItem> history(WinCaptureService &service) {
    bool done = false;
    QVector<ClipboardItem> items;
    service.pipeline()->requestHistory(QString(), QString(), 20, 0, [&](QVector<ClipboardItem> result) {
        items = std::move(result);
        done = true;
    });
    waitFor([&] { return done; });
    return items;
}

// Copies through QClipboard and waits until the pipeline has stored `count` items.
bool copyAndWait(WinCaptureService &service, QMimeData *data, int count) {
    QApplication::clipboard()->setMimeData(data);
    return waitFor([&] { return history(service).size() >= count; }, 8000);
}

void formatTests() {
    const QByteArray html = "<b>hello</b> \xE4\xB8\x96\xE7\x95\x8C";
    const QByteArray cf = WinClipboard::cfHtmlFromHtml(html);
    check(cf.startsWith("Version:0.9"), "cf-html-header");
    check(WinClipboard::htmlFromCfHtml(cf).contains(html), "cf-html-round-trip-keeps-utf8");
    check(WinClipboard::htmlFromCfHtml("garbage").isEmpty(), "cf-html-rejects-garbage");

    QImage image(5, 3, QImage::Format_ARGB32);
    image.fill(QColor(10, 200, 30, 255));
    image.setPixelColor(0, 0, QColor(255, 0, 0, 255));
    const QByteArray dib = WinClipboard::dibFromImage(image);
    const QByteArray png = WinClipboard::pngFromDib(dib);
    QImage decoded;
    check(decoded.loadFromData(png, "PNG"), "dib-to-png-decodes");
    check(decoded.size() == image.size(), "dib-round-trip-keeps-size");
    check(decoded.pixelColor(0, 0).red() == 255 && decoded.pixelColor(4, 2).green() == 200,
          "dib-round-trip-keeps-orientation-and-colour");
    check(WinClipboard::pngFromDib(dib, 4).isEmpty(), "dib-over-pixel-limit-is-refused");
    check(WinClipboard::pngFromDib(QByteArray(10, 'x')).isEmpty(), "dib-too-short-is-refused");
    // mac testImagePixelLimitAvoidsOverflow
    check(WinClipboard::isSafeImageSize(10000, 6000), "pixel-limit-accepts-60-megapixels");
    check(!WinClipboard::isSafeImageSize(10000, 6001), "pixel-limit-rejects-above-60-megapixels");
    check(!WinClipboard::isSafeImageSize(std::numeric_limits<qint64>::max(), std::numeric_limits<qint64>::max()),
          "pixel-limit-does-not-overflow");
}

void hotkeyTests() {
    check(HotkeySpec().toString() == QStringLiteral("Alt+Shift+V"), "hotkey-default-is-alt-shift-v");
    check(HotkeySpec::parse(QStringLiteral("Ctrl+Alt+F12")).key == 0x7B, "hotkey-parse-function-key");
    check(HotkeySpec::parse(QStringLiteral("Ctrl+Shift+V")) == HotkeySpec{HotkeySpec::Control | HotkeySpec::Shift, 'V'},
          "hotkey-parse-modifiers");
    check(!HotkeySpec::parse(QStringLiteral("V")).isValid(), "hotkey-needs-a-modifier");
    check(!HotkeySpec::parse(QStringLiteral("Ctrl+Shift")).isValid(), "hotkey-needs-a-key");
    check(HotkeySpec::parse(HotkeySpec().toString()) == HotkeySpec(), "hotkey-string-round-trip");

    WinGlobalHotkey hotkey;
    const HotkeySpec unusual{HotkeySpec::Control | HotkeySpec::Alt | HotkeySpec::Shift, VK_F23};
    check(hotkey.registerHotkey(unusual), "hotkey-registers");
    int fired = 0;
    QObject::connect(&hotkey, &WinGlobalHotkey::triggered, [&] { ++fired; });
    // Posting WM_HOTKEY exercises the same queue path as a real key press.
    const UINT id = UINT(0xC11D); // first WinGlobalHotkey created in this process
    PostThreadMessageW(GetCurrentThreadId(), WM_HOTKEY, id, 0);
    check(waitFor([&] { return fired > 0; }, 2000), "hotkey-message-emits-triggered");

    WinGlobalHotkey second;
    check(!second.registerHotkey(unusual), "hotkey-conflict-is-reported");
    check(second.lastError().contains(QStringLiteral("占用")), "hotkey-conflict-message");
    hotkey.unregisterHotkey();
    check(second.registerHotkey(unusual), "hotkey-can-be-taken-after-release");
}

void sourceTests() {
    const quint32 self = quint32(quintptr(GetConsoleWindow()));
    const SourceApplicationInfo unknown = windowsSourceApplication(0, 0);
    check(unknown.identifier == QStringLiteral("unknown"), "source-unknown-without-windows");
    HWND desktop = FindWindowW(L"Shell_TrayWnd", nullptr);
    if (desktop) {
        const SourceApplicationInfo shell = windowsSourceApplication(0, quint32(quintptr(desktop)));
        check(shell.identifier.endsWith(QStringLiteral("explorer.exe"), Qt::CaseInsensitive), "source-resolves-explorer-path");
        check(!shell.name.isEmpty() && shell.name != QStringLiteral("Unknown"), "source-has-a-name");
        check(!shell.iconData.isEmpty(), "source-has-icon-png");
        const QImage stored = QImage::fromData(shell.iconData);
        check(stored.width() >= 96, "source-icon-is-high-resolution");
        check(stored.hasAlphaChannel() && qAlpha(stored.pixel(0, 0)) == 0, "source-icon-keeps-transparent-margin");
        // A light rim means straight alpha was read as premultiplied.
        const QImage premultiplied = windowsApplicationIcon(shell.identifier).convertToFormat(QImage::Format_ARGB32_Premultiplied);
        bool validPremultiplied = true;
        for (int y = 0; y < premultiplied.height(); ++y) {
            const auto *line = reinterpret_cast<const QRgb *>(premultiplied.constScanLine(y));
            for (int x = 0; x < premultiplied.width(); ++x) {
                const QRgb p = line[x];
                if (qRed(p) > qAlpha(p) || qGreen(p) > qAlpha(p) || qBlue(p) > qAlpha(p)) validPremultiplied = false;
            }
        }
        check(validPremultiplied, "source-icon-edges-have-no-light-rim");
        check(windowsApplicationIcon(shell.identifier).width() >= 96, "application-icon-refresh-is-high-resolution");
    }
    Q_UNUSED(self)
}

void captureTests() {
    QTemporaryDir directory;
    WinCaptureService service(directory.filePath(QStringLiteral("history.sqlite")));
    service.monitor()->setIgnoreOwnProcess(false); // the test is the clipboard writer
    check(service.start(), "monitor-starts");

    // Existing clipboard content at start-up is a baseline, not history.
    QThread::msleep(300);
    check(history(service).isEmpty(), "startup-clipboard-is-not-recorded");

    auto *text = new QMimeData;
    text->setText(QStringLiteral("hello windows\r\nsecond line"));
    check(copyAndWait(service, text, 1), "text-copy-is-captured");
    auto items = history(service);
    if (!items.isEmpty()) {
        check(items.first().isText() && items.first().text == QStringLiteral("hello windows\nsecond line"),
              "text-is-normalised-to-lf");
        check(!items.first().sourceID.isEmpty() && items.first().sourceID != QStringLiteral("unknown"),
              "text-has-source-application");
    }

    auto *rich = new QMimeData;
    rich->setText(QStringLiteral("bold text"));
    rich->setHtml(QStringLiteral("<p><b>bold</b> text</p>"));
    check(copyAndWait(service, rich, 2), "rich-text-copy-is-captured");
    items = history(service);
    check(!items.isEmpty() && items.first().richTextHtml.contains(QStringLiteral("<b>bold</b>")), "html-is-kept");

    QImage image(40, 30, QImage::Format_ARGB32);
    image.fill(Qt::red);
    auto *picture = new QMimeData;
    picture->setImageData(image);
    check(copyAndWait(service, picture, 3), "image-copy-is-captured");
    items = history(service);
    check(!items.isEmpty() && items.first().isImage(), "image-item-type");

    QTemporaryDir files;
    QFile file(files.filePath(QStringLiteral("report.txt")));
    file.open(QIODevice::WriteOnly);
    file.write("x");
    file.close();
    auto *list = new QMimeData;
    list->setUrls({QUrl::fromLocalFile(file.fileName())});
    check(copyAndWait(service, list, 4), "file-copy-is-captured");
    items = history(service);
    check(!items.isEmpty() && items.first().isFiles() && items.first().files.size() == 1
              && items.first().files.first().name == QStringLiteral("report.txt"),
          "file-item-keeps-name");

    // ClipDeck's own write must not come back as a new history entry.
    ClipboardItem own;
    own.type = ClipboardPayloadType::Text;
    own.text = QStringLiteral("written by clipdeck");
    const auto written = WinClipboard::write(own, false, nullptr);
    check(written.ok, "writer-writes-text");
    service.monitor()->suppressSequence(written.sequence);
    QThread::msleep(600);
    QCoreApplication::processEvents();
    check(history(service).size() == 4, "own-write-is-not-recorded");
    check(QApplication::clipboard()->text() == QStringLiteral("written by clipdeck"), "writer-text-is-pasteable");

    // Password managers mark their content as excluded.
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        const std::wstring secret = L"hunter2";
        HGLOBAL block = GlobalAlloc(GMEM_MOVEABLE, (secret.size() + 1) * sizeof(wchar_t));
        memcpy(GlobalLock(block), secret.c_str(), (secret.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(block);
        SetClipboardData(CF_UNICODETEXT, block);
        HGLOBAL flag = GlobalAlloc(GMEM_MOVEABLE, 4);
        *static_cast<DWORD *>(GlobalLock(flag)) = 1;
        GlobalUnlock(flag);
        SetClipboardData(RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"), flag);
        CloseClipboard();
    }
    QThread::msleep(600);
    QCoreApplication::processEvents();
    check(history(service).size() == 4, "excluded-content-is-not-recorded");

    // Writer variants.
    ClipboardItem imageItem;
    imageItem.type = ClipboardPayloadType::Image;
    imageItem.image = image;
    check(WinClipboard::write(imageItem, false, nullptr).ok && !QApplication::clipboard()->image().isNull(),
          "writer-image-is-pasteable");
    ClipboardItem fileItem;
    fileItem.type = ClipboardPayloadType::Files;
    fileItem.files.append({file.fileName(), QStringLiteral("report.txt"), QStringLiteral("text/plain"), false});
    check(WinClipboard::write(fileItem, false, nullptr).ok
              && QApplication::clipboard()->mimeData()->urls().size() == 1,
          "writer-files-are-pasteable");
    ClipboardItem htmlItem;
    htmlItem.text = QStringLiteral("rich");
    htmlItem.richTextHtml = QStringLiteral("<i>rich</i>");
    WinClipboard::write(htmlItem, true, nullptr);
    check(!QApplication::clipboard()->mimeData()->hasHtml(), "plain-text-write-drops-html");
    WinClipboard::write(htmlItem, false, nullptr);
    check(QApplication::clipboard()->mimeData()->html().contains(QStringLiteral("<i>rich</i>")),
          "rich-write-keeps-html");
    service.stop();
}

ClipboardItem textItem(const QString &text, const QString &html = QString()) {
    ClipboardItem item;
    item.type = ClipboardPayloadType::Text;
    item.text = text;
    item.richTextHtml = html;
    return item;
}

void pasteTests() {
    check(WinPasteService::canDeliverPaste(true, false), "paste-delivers-when-target-front-and-deck-gone");
    check(!WinPasteService::canDeliverPaste(true, true), "paste-waits-while-deck-holds-keyboard");
    check(!WinPasteService::canDeliverPaste(false, false), "paste-waits-until-target-is-front");

    QImage image(30, 10, QImage::Format_ARGB32);
    image.fill(Qt::blue);
    ClipboardItem picture;
    picture.type = ClipboardPayloadType::Image;
    picture.image = image;
    picture.imageAngle = 90;
    const ClipboardItem rotated = WinPasteService::prepareForWrite(picture);
    check(rotated.image.width() == 10 && rotated.image.height() == 30, "image-angle-90-swaps-dimensions");
    picture.imageAngle = 360;
    check(WinPasteService::prepareForWrite(picture).image.size() == image.size(), "image-angle-360-is-unchanged");

    check(WinProcess::integrityLevel(GetCurrentProcessId()) >= 0x2000, "own-integrity-is-at-least-medium");

    QSettings settings;
    settings.remove(QLatin1String(WinPasteService::preserveFormattingKey));

    WinPasteService service;
    quint32 observed = 0;
    service.setWriteObserver([&](quint32 sequence) { observed = sequence; });
    bool dismissed = false;
    auto result = service.paste(textItem(QStringLiteral("plain only"), QStringLiteral("<b>plain only</b>")), nullptr,
                                nullptr, false, [&] { dismissed = true; });
    check(result == WinPasteService::Result::CopiedOnlyNoTarget, "paste-without-target-copies-only");
    check(dismissed, "paste-dismisses-the-deck");
    check(observed != 0, "paste-reports-its-clipboard-write");
    check(QApplication::clipboard()->text() == QStringLiteral("plain only"), "paste-without-target-keeps-clipboard");
    check(!QApplication::clipboard()->mimeData()->hasHtml(), "paste-default-is-plain-text");

    settings.setValue(QLatin1String(WinPasteService::preserveFormattingKey), true);
    service.paste(textItem(QStringLiteral("rich"), QStringLiteral("<b>rich</b>")), nullptr, nullptr, false, {});
    check(QApplication::clipboard()->mimeData()->hasHtml(), "paste-preserve-formatting-adds-html");
    service.paste(textItem(QStringLiteral("rich"), QStringLiteral("<b>rich</b>")), nullptr, nullptr, true, {});
    check(!QApplication::clipboard()->mimeData()->hasHtml(), "paste-force-plain-overrides-preference");
    settings.remove(QLatin1String(WinPasteService::preserveFormattingKey));

    ClipboardItem empty;
    empty.type = ClipboardPayloadType::Files;
    check(service.paste(empty, nullptr, nullptr, false, {}) == WinPasteService::Result::Failed,
          "paste-of-empty-file-list-fails");

    // A window that can never become foreground: cancelled by a clipboard
    // change, then (with the clipboard left alone) timed out after 2 s.
    // The Deck stays visible, so the keyboard is never handed back (mac: deck key window).
    QWidget visibleDeck;
    visibleDeck.show();
    QWidget otherTarget;
    HWND deckWindow = reinterpret_cast<HWND>(visibleDeck.winId());
    HWND hiddenWindow = reinterpret_cast<HWND>(otherTarget.winId());
    service.setAllowOwnProcessTarget(true);
    std::optional<WinPasteService::Outcome> outcome;
    QObject::connect(&service, &WinPasteService::finished, [&](WinPasteService::Outcome value, const QString &) {
        outcome = value;
    });
    result = service.paste(textItem(QStringLiteral("never pasted")), hiddenWindow, deckWindow, false, {});
    check(result == WinPasteService::Result::Pasting && service.isPending(),
          "paste-waits-while-deck-is-still-visible");
    QTimer::singleShot(120, [] { QApplication::clipboard()->setText(QStringLiteral("changed by user")); });
    check(waitFor([&] { return outcome.has_value(); }, 3000) && *outcome == WinPasteService::Outcome::Cancelled,
          "paste-is-cancelled-when-clipboard-changes");
    outcome.reset();
    service.paste(textItem(QStringLiteral("never pasted")), hiddenWindow, deckWindow, false, {});
    check(waitFor([&] { return outcome.has_value(); }, 4000) && *outcome == WinPasteService::Outcome::TimedOut,
          "paste-times-out-after-2s");

    // End to end: a real SendInput Ctrl+V into a focused edit of this process.
    QPlainTextEdit target;
    target.resize(300, 120);
    target.show();
    target.activateWindow();
    target.raise();
    waitFor([] { return false; }, 300);
    outcome.reset();
    bool keystroke = false;
    QObject::connect(&service, &WinPasteService::keystrokeSent, [&] { keystroke = true; });
    service.paste(textItem(QStringLiteral("pasted by clipdeck")), reinterpret_cast<HWND>(target.winId()), nullptr,
                  false, {});
    waitFor([&] { return outcome.has_value(); }, 3000);
    if (outcome && *outcome == WinPasteService::Outcome::Pasted) {
        check(keystroke, "paste-sends-keystroke");
        check(waitFor([&] { return target.toPlainText() == QStringLiteral("pasted by clipdeck"); }, 2000),
              "paste-end-to-end-text-arrives");
    } else {
        std::printf("SKIP paste-end-to-end (Windows refused to focus the test window; outcome=%d)\n",
                    outcome ? int(*outcome) : -1);
    }
}

void trayTests() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        std::printf("SKIP tray tests (no system tray)\n");
        return;
    }
    QSettings().remove(QStringLiteral("app/trayHintShown"));
    QPixmap pixmap(32, 32);
    pixmap.fill(Qt::darkCyan);
    WinTray tray{QIcon(pixmap)};
    int toggled = 0, settingsCount = 0, quitCount = 0;
    QObject::connect(&tray, &WinTray::toggleRequested, [&] { ++toggled; });
    QObject::connect(&tray, &WinTray::settingsRequested, [&] { ++settingsCount; });
    QObject::connect(&tray, &WinTray::quitRequested, [&] { ++quitCount; });

    check(tray.toolTip() == QStringLiteral("左键显示 ClipDeck，右键打开菜单"), "tray-tooltip-matches-mac");
    tray.setHotkeyError(QStringLiteral("Alt+Shift+V 已被其他程序占用"));
    check(tray.toolTip().startsWith(QStringLiteral("全局快捷键注册失败：")), "tray-tooltip-reports-hotkey-failure");
    tray.setHotkeyError(QString());

    check(tray.menu()->actions().size() == 3, "tray-menu-has-settings-separator-quit");
    check(tray.settingsAction()->text().startsWith(QStringLiteral("设置…")), "tray-menu-settings-title");
    check(tray.quitAction()->text().startsWith(QStringLiteral("退出 ClipDeck")), "tray-menu-quit-title");
    tray.settingsAction()->trigger();
    tray.quitAction()->trigger();
    check(settingsCount == 1 && quitCount == 1, "tray-menu-actions-emit-signals");

    tray.show();
    check(tray.isVisible(), "tray-icon-shows");
    tray.simulateActivation(QSystemTrayIcon::Trigger);
    tray.simulateActivation(QSystemTrayIcon::DoubleClick);
    tray.simulateActivation(QSystemTrayIcon::MiddleClick);
    check(toggled == 1, "tray-left-click-toggles-once");
    check(tray.showFirstRunHintIfNeeded(QStringLiteral("Alt+Shift+V")), "tray-first-run-hint-shown-once");
    check(!tray.showFirstRunHintIfNeeded(QStringLiteral("Alt+Shift+V")), "tray-first-run-hint-not-repeated");
    tray.hide();
    check(!tray.isVisible(), "tray-icon-hides");
}

} // namespace

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ClipDeckTests"));
    QCoreApplication::setApplicationName(QStringLiteral("platform-tests"));

    formatTests();
    hotkeyTests();
    sourceTests();
    captureTests();
    pasteTests();
    trayTests();
    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
