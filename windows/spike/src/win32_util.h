#pragma once

#include <QString>

#include <windows.h>

namespace spike {

// Integrity level RID (SECURITY_MANDATORY_*_RID), or -1 when the process
// cannot be opened (protected or already exited).
long integrityLevel(DWORD processId);
QString integrityName(long rid);
QString processImagePath(DWORD processId);
QString describeWindow(HWND window);
bool belongsToCurrentProcess(HWND window);
HWND rootOwner(HWND window);
QString windowsVersion();
bool transparencyEffectsEnabled();
bool compositionEnabled();

} // namespace spike
