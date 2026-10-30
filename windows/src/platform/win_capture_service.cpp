#include "win_capture_service.h"

#include "win_source_application.h"

#include <QMetaObject>

WinCaptureService::WinCaptureService(const QString &databasePath, QObject *parent)
    : QObject(parent),
      pipeline_(databasePath, false, windowsSourceApplication),
      monitor_([this](ClipboardSnapshot snapshot) {
          // The monitor runs on its own thread; the pipeline lives on the GUI thread.
          QMetaObject::invokeMethod(&pipeline_, [this, snapshot = std::move(snapshot)]() mutable {
              pipeline_.submit(std::move(snapshot));
          }, Qt::QueuedConnection);
      }) {}

WinCaptureService::~WinCaptureService() { stop(); }

bool WinCaptureService::start() {
    if (started_) return true;
    pipeline_.start();
    started_ = monitor_.begin();
    return started_;
}

void WinCaptureService::stop() {
    if (!started_) return;
    monitor_.end();
    started_ = false;
}
