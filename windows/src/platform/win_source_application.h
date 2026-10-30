#pragma once

#include "platform/application_source.h"

// Resolves the application that wrote the clipboard from the windows recorded
// at capture time (never the current focus). The clipboard owner wins because
// UWP/Store apps and browsers report the real process there; the foreground
// window is the fallback. identifier is the executable path (mac: bundle id).
SourceApplicationInfo windowsSourceApplication(quint32 sourceWindow, quint32 ownerWindow);

// High resolution icon of an executable; used to refresh icons stored by
// older builds at 32 px.
QImage windowsApplicationIcon(const QString &path);

// Exposed for tests.
QString windowsExecutableDescription(const QString &path);
