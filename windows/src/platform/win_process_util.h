#pragma once

#include <QString>

#include <windows.h>

namespace WinProcess {

// Integrity level RID (SECURITY_MANDATORY_*_RID), or -1 when the process
// cannot be opened (protected or already exited).
long integrityLevel(DWORD processId);
QString integrityName(long rid);
QString imagePath(DWORD processId);
QString describeWindow(HWND window);
bool belongsToCurrentProcess(HWND window);
HWND rootOwner(HWND window);

} // namespace WinProcess
