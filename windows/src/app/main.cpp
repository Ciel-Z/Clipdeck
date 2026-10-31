#include "code_highlight.h"
#include "deck_controller.h"
#include "deck_image_provider.h"

#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QLockFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStandardPaths>
#include <QTimer>

int main(int argc, char *argv[]) {
    bool showOnLaunch = false;
    QString showSettings; // null: do not open
    QString databaseOverride;
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QLatin1String("--show-on-launch")) showOnLaunch = true;
        if (argument.startsWith(QLatin1String("--settings"))) {
            showSettings = argument.section(QLatin1Char('='), 1); // --settings[=pane]
            if (showSettings.isNull()) showSettings = QLatin1String("");
        }
        if (argument == QLatin1String("--database") && index + 1 < argc) databaseOverride = QString::fromLocal8Bit(argv[++index]);
    }

    QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    QQuickWindow::setDefaultAlphaBuffer(true);
    // Native glyph rasterisation (as on mac) instead of distance fields,
    // which look soft at 11–14 px.
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    // Qt 6.12 + D3D11 + an alpha-buffer window: the threaded render loop
    // stalls after the first frames (Phase 0 finding); the basic loop does not.
    if (!qEnvironmentVariableIsSet("QSG_RENDER_LOOP")) qputenv("QSG_RENDER_LOOP", "basic");

    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("ClipDeck"));
    QApplication::setApplicationName(QStringLiteral("ClipDeck"));
    QApplication::setQuitOnLastWindowClosed(false);

    const QString dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    QDir().mkpath(dataDirectory);
    // One capture process per user: a second instance would record every copy twice.
    QLockFile instanceLock(QDir(dataDirectory).filePath(QStringLiteral("clipdeck.lock")));
    instanceLock.setStaleLockTime(0);
    if (!instanceLock.tryLock(100)) {
        qWarning("ClipDeck is already running");
        return 0;
    }
    const QString databasePath = databaseOverride.isEmpty()
        ? QDir(dataDirectory).filePath(QStringLiteral("history.sqlite"))
        : databaseOverride;

    auto *images = new DeckImageProvider; // owned by the engine
    DeckController controller(databasePath, images);

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("deck"), images);
    qmlRegisterType<SyntaxHighlightAttachment>("ClipDeck.Native", 1, 0, "SyntaxHighlight");
    engine.rootContext()->setContextProperty(QStringLiteral("deck"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("settings"), controller.settings());
    engine.loadFromModule(QStringLiteral("ClipDeck"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) return 2;
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    engine.loadFromModule(QStringLiteral("ClipDeck"), QStringLiteral("SettingsWindow"));
    if (engine.rootObjects().size() < 2) return 3;
    controller.settings()->attachWindow(qobject_cast<QWindow *>(engine.rootObjects().at(1)));

    // Every size the tray and title bar use at 100–250 % scaling, so Windows
    // never scales a neighbouring size and blurs it.
    QIcon trayIcon;
    for (const int size : {16, 20, 24, 32, 40, 48, 64, 256}) {
        trayIcon.addFile(QStringLiteral(":/icons/icon-%1.png").arg(size));
    }
    QApplication::setWindowIcon(trayIcon);
    controller.start(window, trayIcon);

    if (showOnLaunch) QTimer::singleShot(300, &controller, [&controller] { controller.showDeck(QStringLiteral("launch")); });
    if (!showSettings.isNull()) QTimer::singleShot(300, &controller, [&controller, showSettings] { controller.openSettings(showSettings); });
    return application.exec();
}
