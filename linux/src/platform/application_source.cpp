#include "application_source.h"
#include "linux_application_icon.h"
#include "../ui/linux_icons.h"

#include <QBuffer>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QVector>
#include <xcb/xcb.h>
#include <cstdlib>
#include <cstring>

namespace {
struct DesktopApplicationEntry {
    QString path;
    QString name;
    QString icon;
    QString startupWMClass;
    QString exec;
    QString execPath;
    QString execBaseName;
};

struct X11ApplicationIdentity {
    xcb_window_t window = XCB_NONE;
    qint64 pid = 0;
    QString wmInstance;
    QString wmClass;
    QString executablePath;
};

QString normalizedApplicationToken(const QString &value) {
    QString result = value.trimmed().toLower();
    if (result.endsWith(QStringLiteral(".desktop"))) result.chop(8);
    result.remove(QRegularExpression(QStringLiteral("[^a-z0-9]+")));
    return result;
}

QString unescapeDesktopValue(QString value) {
    value.replace(QStringLiteral("\\s"), QStringLiteral(" "));
    value.replace(QStringLiteral("\\n"), QStringLiteral("\n"));
    value.replace(QStringLiteral("\\t"), QStringLiteral("\t"));
    value.replace(QStringLiteral("\\r"), QStringLiteral("\r"));
    value.replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
    return value;
}

QString desktopExecCommand(const QString &exec) {
    QString value = exec.trimmed();
    QStringList tokens;
    QString token;
    QChar quote;
    bool escaped = false;
    for (const QChar character : value) {
        if (escaped) { token.append(character); escaped = false; continue; }
        if (character == QLatin1Char('\\')) { escaped = true; continue; }
        if (!quote.isNull()) {
            if (character == quote) quote = QChar();
            else token.append(character);
            continue;
        }
        if (character == QLatin1Char('\'') || character == QLatin1Char('"')) {
            quote = character;
        } else if (character.isSpace()) {
            if (!token.isEmpty()) { tokens.append(token); token.clear(); }
        } else {
            token.append(character);
        }
    }
    if (!token.isEmpty()) tokens.append(token);
    int index = 0;
    if (index < tokens.size() && QFileInfo(tokens[index]).fileName() == QStringLiteral("env")) ++index;
    while (index < tokens.size() && tokens[index].contains(QLatin1Char('='))
           && !tokens[index].startsWith(QLatin1Char('/'))) ++index;
    while (index < tokens.size() && tokens[index].startsWith(QLatin1Char('%'))) ++index;
    return index < tokens.size() ? tokens[index] : QString();
}

QVector<DesktopApplicationEntry> scanDesktopApplications() {
    QVector<DesktopApplicationEntry> entries;
    const QStringList roots = {
        QDir::home().filePath(QStringLiteral(".local/share/applications")),
        QStringLiteral("/usr/share/applications")
    };
    QSet<QString> visited;
    for (const QString &root : roots) {
        if (!QFileInfo(root).isDir()) continue;
        QDirIterator iterator(root, QStringList{QStringLiteral("*.desktop")}, QDir::Files,
                              QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            const QString path = iterator.next();
            const QString desktopID = QFileInfo(path).fileName().toLower();
            if (visited.contains(desktopID)) continue; // User entry overrides system entry.
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
            DesktopApplicationEntry entry;
            entry.path = path;
            QString localizedName;
            bool inDesktopEntry = false;
            while (!file.atEnd()) {
                const QString line = QString::fromUtf8(file.readLine()).trimmed();
                if (line.startsWith(QLatin1Char('['))) {
                    inDesktopEntry = line == QStringLiteral("[Desktop Entry]");
                    continue;
                }
                if (!inDesktopEntry || line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
                const int separator = line.indexOf(QLatin1Char('='));
                if (separator <= 0) continue;
                const QString key = line.left(separator);
                const QString value = unescapeDesktopValue(line.mid(separator + 1));
                if (key == QStringLiteral("Name")) entry.name = value;
                else if ((key == QStringLiteral("Name[zh_CN]") || key == QStringLiteral("Name[zh]"))
                         && localizedName.isEmpty()) localizedName = value;
                else if (key == QStringLiteral("Icon")) entry.icon = value;
                else if (key == QStringLiteral("StartupWMClass")) entry.startupWMClass = value;
                else if (key == QStringLiteral("Exec")) entry.exec = value;
            }
            if (!localizedName.isEmpty()) entry.name = localizedName;
            entry.execPath = desktopExecCommand(entry.exec);
            entry.execBaseName = QFileInfo(entry.execPath).fileName();
            if (!entry.name.isEmpty() || !entry.exec.isEmpty() || !entry.startupWMClass.isEmpty()) {
                entries.append(entry);
                visited.insert(desktopID);
            }
        }
    }
    return entries;
}

const QVector<DesktopApplicationEntry> &desktopApplications() {
    static const QVector<DesktopApplicationEntry> entries = scanDesktopApplications();
    return entries;
}

QString canonicalExecutablePath(const QString &path) {
    if (path.isEmpty()) return {};
    QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

int desktopMatchScore(const DesktopApplicationEntry &entry,
                      const X11ApplicationIdentity &identity) {
    const QString instance = normalizedApplicationToken(identity.wmInstance);
    const QString wmClass = normalizedApplicationToken(identity.wmClass);
    const QString startup = normalizedApplicationToken(entry.startupWMClass);
    if (!startup.isEmpty() && (startup == instance || startup == wmClass)) return 300;

    const QString actualExe = canonicalExecutablePath(identity.executablePath);
    QString desktopExe = entry.execPath;
    if (!desktopExe.isEmpty() && !QFileInfo(desktopExe).isAbsolute()) {
        desktopExe = QStandardPaths::findExecutable(desktopExe);
    }
    if (!actualExe.isEmpty() && !desktopExe.isEmpty()
        && canonicalExecutablePath(desktopExe) == actualExe) return 200;

    const QString actualBase = normalizedApplicationToken(QFileInfo(actualExe).fileName());
    const QString execBase = normalizedApplicationToken(entry.execBaseName);
    const QString desktopBase = normalizedApplicationToken(QFileInfo(entry.path).completeBaseName());
    if (!actualBase.isEmpty() && (actualBase == execBase || actualBase == desktopBase)) return 120;
    if ((!instance.isEmpty() && (instance == execBase || instance == desktopBase))
        || (!wmClass.isEmpty() && (wmClass == execBase || wmClass == desktopBase))) return 100;
    return 0;
}

qint64 windowPID(xcb_connection_t *display, xcb_window_t window) {
    const auto cookie = xcb_intern_atom(display, true, 11, "_NET_WM_PID");
    auto *atom = xcb_intern_atom_reply(display, cookie, nullptr);
    const xcb_atom_t pidAtom = atom ? atom->atom : xcb_atom_t(XCB_NONE);
    std::free(atom);
    if (pidAtom == XCB_NONE) return 0;
    auto *property = xcb_get_property_reply(display, xcb_get_property(display, false, window,
        pidAtom, XCB_ATOM_CARDINAL, 0, 1), nullptr);
    quint32 pid = 0;
    if (property && property->format == 32 && xcb_get_property_value_length(property) >= 4)
        std::memcpy(&pid, xcb_get_property_value(property), 4);
    std::free(property);
    return pid;
}

X11ApplicationIdentity x11ApplicationForWindow(xcb_connection_t *display, xcb_window_t candidate) {
    X11ApplicationIdentity identity;
    identity.window = candidate;
    if (!display || candidate <= 1) return identity;

    // Chrome and some extension editors can own the clipboard through an
    // auxiliary X window without WM_CLASS. Walk towards the WM frame and
    // collect PID/class from the first ancestor that publishes each value.
    xcb_window_t current = candidate;
    for (int depth = 0; depth < 16 && current != XCB_NONE; ++depth) {
        if (identity.pid <= 0) identity.pid = windowPID(display, current);
        if (identity.wmInstance.isEmpty() || identity.wmClass.isEmpty()) {
            auto *property = xcb_get_property_reply(display, xcb_get_property(display, false,
                current, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 1024), nullptr);
            if (property && property->format == 8) {
                const auto classes = QByteArray(static_cast<const char *>(xcb_get_property_value(property)),
                    xcb_get_property_value_length(property)).split('\0');
                if (identity.wmInstance.isEmpty() && !classes.isEmpty())
                    identity.wmInstance = QString::fromLocal8Bit(classes[0]);
                if (identity.wmClass.isEmpty() && classes.size() > 1)
                    identity.wmClass = QString::fromLocal8Bit(classes[1]);
            }
            std::free(property);
        }
        if (identity.pid > 0 && !identity.wmClass.isEmpty()) break;

        auto *tree = xcb_query_tree_reply(display, xcb_query_tree(display, current), nullptr);
        if (!tree) break; // the captured application may already have closed
        const auto parent = tree->parent;
        const auto root = tree->root;
        std::free(tree);
        if (parent == XCB_NONE || parent == root || parent == current) break;
        current = parent;
    }
    if (identity.pid > 0) {
        identity.executablePath = QFileInfo(
            QStringLiteral("/proc/%1/exe").arg(identity.pid)).symLinkTarget();
        identity.executablePath = canonicalExecutablePath(identity.executablePath);
    }
    return identity;
}

int x11IdentityScore(const X11ApplicationIdentity &identity) {
    int score = 0;
    if (identity.pid > 0) score += 50;
    if (!identity.executablePath.isEmpty()) score += 30;
    if (!identity.wmClass.isEmpty()) score += 20;
    if (!identity.wmInstance.isEmpty()) score += 10;
    return score;
}

X11ApplicationIdentity capturedX11Application(xcb_connection_t *display, xcb_window_t sourceWindow, xcb_window_t ownerWindow) {
    X11ApplicationIdentity best;
    int bestScore = -1;
    for (const xcb_window_t window : {sourceWindow, ownerWindow}) {
        if (window <= 1) continue;
        const X11ApplicationIdentity candidate = x11ApplicationForWindow(display, window);
        const int score = x11IdentityScore(candidate);
        if (score > bestScore) { best = candidate; bestScore = score; }
    }
    return best;
}

SourceApplicationInfo capturedApplication(xcb_connection_t *display, xcb_window_t sourceWindow, xcb_window_t ownerWindow) {
    SourceApplicationInfo result;
    if (!display || xcb_connection_has_error(display)) return result;
    const X11ApplicationIdentity identity = capturedX11Application(display, sourceWindow, ownerWindow);
    const DesktopApplicationEntry *matched = nullptr;
    int matchedScore = 0;
    for (const DesktopApplicationEntry &entry : desktopApplications()) {
        const int score = desktopMatchScore(entry, identity);
        if (score > matchedScore) { matched = &entry; matchedScore = score; }
    }

    QString iconValue;
    QString iconPath;
    int pngVariantCount = 0;
    bool hasSvgVariant = false;
    if (matched) {
        result.identifier = QFileInfo(matched->path).fileName();
        result.name = matched->name.isEmpty() ? identity.wmClass : matched->name;
        iconValue = matched->icon;
        const bool isClipDeck = QFileInfo(matched->path).fileName().compare(
            QStringLiteral("clipdeck.desktop"), Qt::CaseInsensitive) == 0;
        if (isClipDeck) {
            // The macOS application artwork contains a platform-style baked
            // shadow. Linux docks and cards add their own treatment, so use a
            // clean, shadow-free source icon here.
            result.icon = shadowFreeClipDeckIcon();
        } else {
            static thread_local QHash<QString, LinuxApplicationIconSet> iconCache;
            if (!iconCache.contains(iconValue)) iconCache.insert(iconValue, resolveLinuxApplicationIconSet(iconValue));
            const LinuxApplicationIconSet iconSet = iconCache.value(iconValue);
            result.iconVariantsData = iconSet.serialize();
            iconPath = iconSet.selectedPath;
            pngVariantCount = iconSet.pngVariants.size();
            hasSvgVariant = !iconSet.svgData.isEmpty();
            result.icon = iconSet.rasterize(QSize(256, 256), 1.0);
        }
    } else {
        const QString executableName = QFileInfo(identity.executablePath).completeBaseName();
        result.identifier = !identity.wmClass.isEmpty() ? identity.wmClass
            : (!identity.wmInstance.isEmpty() ? identity.wmInstance : executableName);
        result.name = !identity.wmClass.isEmpty() ? identity.wmClass
            : (!identity.wmInstance.isEmpty() ? identity.wmInstance
                                              : (!executableName.isEmpty()
                                                     ? executableName
                                                     : QStringLiteral("Unknown")));
    }
    if (!result.icon.isNull()) {
        QBuffer buffer(&result.iconData);
        buffer.open(QIODevice::WriteOnly);
        result.icon.save(&buffer, "PNG");
    }
    qInfo().noquote()
        << QStringLiteral("[ClipDeck][X11Source] window=0x%1 pid=%2 WM_CLASS(instance=\"%3\", class=\"%4\") exe=\"%5\" desktop=\"%6\" score=%7 Icon=\"%8\" iconPath=\"%9\" svg=%10 pngVariants=%11")
               .arg(static_cast<qulonglong>(identity.window), 0, 16)
               .arg(identity.pid)
               .arg(identity.wmInstance, identity.wmClass, identity.executablePath,
                    matched ? matched->path : QString())
               .arg(matchedScore)
               .arg(iconValue, iconPath)
               .arg(hasSvgVariant ? QStringLiteral("yes") : QStringLiteral("no"))
               .arg(pngVariantCount);
    return result;
}


} // namespace

SourceApplicationInfo sourceApplicationForWindows(quint32 sourceWindow, quint32 ownerWindow) {
    struct Connection {
        xcb_connection_t *display = xcb_connect(nullptr, nullptr);
        ~Connection() { if (display) xcb_disconnect(display); }
    };
    thread_local Connection connection;
    return capturedApplication(connection.display, sourceWindow, ownerWindow);
}

QStringList applicationSourceSelfTestFailures() {
    QStringList failures;
    const auto require = [&](bool condition, const QString &name) { if (!condition) failures.append(name); };
    X11ApplicationIdentity identity;
    identity.wmInstance = QStringLiteral("code");
    identity.wmClass = QStringLiteral("Code");
    identity.executablePath = QStringLiteral("/usr/bin/code");
    DesktopApplicationEntry startup;
    startup.startupWMClass = QStringLiteral("Code");
    startup.execPath = QStringLiteral("/unrelated/application");
    startup.execBaseName = QStringLiteral("application");
    DesktopApplicationEntry executable;
    executable.path = QStringLiteral("/usr/share/applications/code.desktop");
    executable.execPath = desktopExecCommand(QStringLiteral("env GTK_THEME=dark /usr/bin/code --unity-launch %F"));
    executable.execBaseName = QFileInfo(executable.execPath).fileName();
    DesktopApplicationEntry fallback = executable;
    fallback.execPath = QStringLiteral("code");
    require(desktopExecCommand(QStringLiteral("env FOO=bar '/opt/My App/bin/example' %U"))
                == QStringLiteral("/opt/My App/bin/example"), QStringLiteral("desktop-exec-parse"));
    require(desktopMatchScore(startup, identity) == 300, QStringLiteral("desktop-match-startup-wm-class"));
    require(desktopMatchScore(executable, identity) == 200, QStringLiteral("desktop-match-executable"));
    identity.executablePath = QStringLiteral("/not/installed/code");
    require(desktopMatchScore(fallback, identity) == 120, QStringLiteral("desktop-match-fallback"));
    return failures;
}
