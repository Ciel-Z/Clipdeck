#pragma once

#include <QString>

// Phase 0 results are collected from real Windows 10 machines, so every
// observation goes to one plain-text file the tester can send back.
namespace spike {

void initLog();
void log(const QString &area, const QString &message);
QString logPath();
int warningCount();

} // namespace spike
