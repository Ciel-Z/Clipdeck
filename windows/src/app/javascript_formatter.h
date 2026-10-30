#pragma once

#include <QString>

// Port of JavaScriptFormatter (CategoryManagementView.swift): re-indents a
// classification script with 4 spaces, one statement per line, and leaves
// strings, comments and regular expressions untouched.
namespace JavaScriptFormatter {
QString format(const QString &source);
}
