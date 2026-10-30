#include "spike_log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QStandardPaths>
#include <QTextStream>

#include <cstdio>

namespace spike {
namespace {

QMutex mutex;
QString path;
int warnings = 0;

void append(const QString &line) {
    QMutexLocker locker(&mutex);
    std::fprintf(stderr, "%s\n", line.toLocal8Bit().constData());
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::Append | QIODevice::Text)) return;
    QTextStream(&file) << line << '\n';
}

void messageHandler(QtMsgType type, const QMessageLogContext &, const QString &message) {
    const char *level = "debug";
    switch (type) {
    case QtDebugMsg: level = "debug"; break;
    case QtInfoMsg: level = "info"; break;
    case QtWarningMsg: level = "warning"; ++warnings; break;
    case QtCriticalMsg: level = "critical"; ++warnings; break;
    case QtFatalMsg: level = "fatal"; ++warnings; break;
    }
    log(QStringLiteral("qt.%1").arg(QLatin1String(level)), message);
}

} // namespace

void initLog() {
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/ClipDeckSpike");
    QDir().mkpath(directory);
    path = directory + QStringLiteral("/spike-log.txt");
    qInstallMessageHandler(messageHandler);
    append(QStringLiteral("\n===== ClipDeck spike started %1 =====")
               .arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
}

void log(const QString &area, const QString &message) {
    append(QStringLiteral("%1 [%2] %3")
               .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz")),
                    area, message));
}

QString logPath() { return path; }

int warningCount() { return warnings; }

} // namespace spike
