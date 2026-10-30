#include "win_clipboard_monitor.h"

#include "win_clipboard_reader.h"

#include <QDebug>
#include <QElapsedTimer>

namespace {
constexpr wchar_t kWindowClass[] = L"ClipDeckClipboardMonitor";
}

WinClipboardMonitor::WinClipboardMonitor(Sink sink, QObject *parent) : QThread(parent), sink_(std::move(sink)) {}

WinClipboardMonitor::~WinClipboardMonitor() { end(); }

bool WinClipboardMonitor::begin() {
    if (isRunning()) return true;
    ready_ = false;
    failed_ = false;
    start();
    QElapsedTimer timer;
    timer.start();
    while (!ready_ && !failed_ && timer.elapsed() < 5000) msleep(5);
    return ready_ && !failed_;
}

void WinClipboardMonitor::end() {
    if (!isRunning()) return;
    if (window_) PostMessageW(window_, WM_CLOSE, 0, 0);
    if (!wait(5000)) {
        qWarning() << "[ClipDeck][Clipboard] monitor thread did not stop";
        terminate();
        wait();
    }
}

void WinClipboardMonitor::run() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &WinClipboardMonitor::windowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kWindowClass;
    RegisterClassExW(&windowClass); // already registered on a restart: harmless
    window_ = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                              windowClass.hInstance, this);
    if (!window_ || !AddClipboardFormatListener(window_)) {
        qWarning() << "[ClipDeck][Clipboard] listener failed, error" << GetLastError();
        if (window_) DestroyWindow(window_);
        window_ = nullptr;
        failed_ = true;
        return;
    }
    capture(true);
    ready_ = true;

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    window_ = nullptr;
}

LRESULT CALLBACK WinClipboardMonitor::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto *self = reinterpret_cast<WinClipboardMonitor *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
    case WM_CLIPBOARDUPDATE:
        if (self) self->capture(false);
        return 0;
    case WM_CLOSE:
        RemoveClipboardFormatListener(window);
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void WinClipboardMonitor::capture(bool baseline) {
    const DWORD sequence = GetClipboardSequenceNumber();
    if (!baseline && sequence == suppressedSequence_.load()) return;

    HWND foreground = GetForegroundWindow();
    HWND owner = GetClipboardOwner();
    if (!baseline && ignoreOwnProcess_ && owner) {
        DWORD pid = 0;
        GetWindowThreadProcessId(owner, &pid);
        if (pid == GetCurrentProcessId()) return;
    }

    WinClipboard::ReadResult result = WinClipboard::read(window_);
    if (result.status == WinClipboard::ReadStatus::Busy) {
        qWarning() << "[ClipDeck][Clipboard] clipboard stayed locked; skipped sequence" << sequence;
        return;
    }
    if (result.status != WinClipboard::ReadStatus::Ok) return;

    ClipboardSnapshot snapshot = std::move(result.snapshot);
    snapshot.baseline = baseline;
    snapshot.explicitChange = !baseline;
    snapshot.sourceWindow = quint32(quintptr(foreground));
    snapshot.ownerWindow = quint32(quintptr(owner));
    if (sink_) sink_(std::move(snapshot));
}
