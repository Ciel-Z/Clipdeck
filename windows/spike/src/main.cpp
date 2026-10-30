#include "global_hotkey.h"
#include "spike_controller.h"
#include "spike_images.h"
#include "spike_log.h"

#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QIcon>
#include <QMenu>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSystemTrayIcon>
#include <QUrl>

int main(int argc, char *argv[]) {
    bool selfTest = false;
    QString rhi;
    for (int index = 1; index < argc; ++index) {
        const QString argument = QString::fromLocal8Bit(argv[index]);
        if (argument == QLatin1String("--self-test")) selfTest = true;
        if (argument.startsWith(QLatin1String("--rhi="))) rhi = argument.mid(6);
    }
    if (rhi == QLatin1String("d3d11")) QQuickWindow::setGraphicsApi(QSGRendererInterface::Direct3D11);
    if (rhi == QLatin1String("opengl")) QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    if (rhi == QLatin1String("software")) QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QQuickWindow::setDefaultAlphaBuffer(true);
    // Qt 6.12 + D3D11 + an alpha-buffer window: the threaded render loop stalls after the first
    // frames (observed on Win 11 25H2, RTX 5080 + AMD iGPU). The basic loop renders normally.
    if ((rhi.isEmpty() || rhi == QLatin1String("d3d11")) && !qEnvironmentVariableIsSet("QSG_RENDER_LOOP"))
        qputenv("QSG_RENDER_LOOP", "basic");

    QApplication application(argc, argv);
    QApplication::setOrganizationName(QStringLiteral("ClipDeck"));
    QApplication::setApplicationName(QStringLiteral("ClipDeckSpike"));
    QApplication::setQuitOnLastWindowClosed(false);

    spike::initLog();
    spike::log(QStringLiteral("env"), QStringLiteral("args rhi=%1 selfTest=%2 log=%3")
                                          .arg(rhi.isEmpty() ? QStringLiteral("default") : rhi)
                                          .arg(selfTest)
                                          .arg(spike::logPath()));

    SpikeController controller(selfTest);
    controller.logEnvironment();

    QQmlApplicationEngine engine;
    engine.addImageProvider(QStringLiteral("spike"), new SpikeImageProvider);
    engine.rootContext()->setContextProperty(QStringLiteral("spike"), &controller);
    engine.loadFromModule(QStringLiteral("ClipDeckSpike"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        spike::log(QStringLiteral("qml"), QStringLiteral("Main.qml failed to load"));
        return 2;
    }
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());

    QSystemTrayIcon tray(QIcon(QStringLiteral(":/icon-32.png")));
    QMenu menu;
    menu.addAction(QStringLiteral("打开日志位置"), [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(spike::logPath()).absolutePath()));
    });
    menu.addSeparator();
    menu.addAction(QStringLiteral("退出 ClipDeck Spike"), &application, &QCoreApplication::quit);
    tray.setContextMenu(&menu);
    tray.setToolTip(QStringLiteral("左键显示 ClipDeck，右键打开菜单"));
    QObject::connect(&tray, &QSystemTrayIcon::activated, &controller,
                     [&controller](QSystemTrayIcon::ActivationReason reason) {
                         if (reason == QSystemTrayIcon::Trigger) controller.toggle(QStringLiteral("tray"));
                     });
    if (!selfTest) tray.show();

    controller.attach(window, &tray);

    GlobalHotkey hotkey;
    if (!selfTest) {
        application.installNativeEventFilter(&hotkey);
        controller.setHotkeyName(hotkey.registerDefault());
        QObject::connect(&hotkey, &GlobalHotkey::triggered, &controller,
                         [&controller] { controller.toggle(QStringLiteral("hotkey")); });
    }

    if (selfTest) controller.toggle(QStringLiteral("self-test"));
    return application.exec();
}
