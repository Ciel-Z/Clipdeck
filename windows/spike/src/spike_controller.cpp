#include "spike_controller.h"

#include "spike_log.h"
#include "win32_util.h"

#include <QCoreApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QMetaEnum>
#include <QScreen>
#include <QSettings>
#include <QSGRendererInterface>
#include <QSystemTrayIcon>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace {

const QString savedHeightKey = QStringLiteral("deck.preferredHeight");

struct HeightLimits {
    qreal minimum;
    qreal maximum;
    qreal preferred;
};

// PanelController.updateFrame / DeckResizeHandle in the macOS client.
HeightLimits heightLimits(const QRect &workArea) {
    const qreal maximum = std::max(250.0, std::min(420.0, std::floor(workArea.height() * 0.42)));
    const qreal minimum = std::min(250.0, maximum);
    const qreal preferred = std::min(maximum, std::max(340.0, std::floor(workArea.height() * 0.385)));
    return {minimum, maximum, preferred};
}

QString apiName(QSGRendererInterface::GraphicsApi api) {
    switch (api) {
    case QSGRendererInterface::Direct3D11: return QStringLiteral("d3d11");
    case QSGRendererInterface::OpenGL: return QStringLiteral("opengl");
    case QSGRendererInterface::Vulkan: return QStringLiteral("vulkan");
    case QSGRendererInterface::Software: return QStringLiteral("software");
    default: return QStringLiteral("other(%1)").arg(int(api));
    }
}

} // namespace

SpikeController::SpikeController(bool selfTest, QObject *parent)
    : QObject(parent), selfTest_(selfTest) {
    connect(&paste_, &PasteFlow::finished, this, [this](PasteFlow::Outcome outcome, const QString &detail) {
        setStatus(QStringLiteral("paste: %1")
                      .arg(QLatin1String(QMetaEnum::fromType<PasteFlow::Outcome>().valueToKey(int(outcome)))));
        if (outcome == PasteFlow::Outcome::CopiedOnlyElevatedTarget && tray_) {
            tray_->showMessage(QStringLiteral("内容已复制到剪贴板"),
                               QStringLiteral("目标应用以管理员身份运行，请手动按 Ctrl + V。"));
        }
        Q_UNUSED(detail);
    });
}

void SpikeController::attach(QQuickWindow *window, QSystemTrayIcon *tray) {
    window_ = window;
    tray_ = tray;
    connect(window, &QQuickWindow::sceneGraphInitialized, this, [this] {
        graphicsApi_ = apiName(window_->rendererInterface()->graphicsApi());
        spike::log(QStringLiteral("env"), QStringLiteral("scene graph api=%1 alpha=%2")
                                              .arg(graphicsApi_)
                                              .arg(window_->format().alphaBufferSize()));
        emit graphicsApiChanged();
    }, Qt::QueuedConnection);
    connect(window, &QWindow::activeChanged, this, &SpikeController::onActiveChanged);
    connect(window, &QWindow::widthChanged, this, &SpikeController::applyRegion);
    connect(window, &QWindow::heightChanged, this, &SpikeController::applyRegion);
    connect(window, &QWindow::screenChanged, this, [this](QScreen *screen) {
        spike::log(QStringLiteral("dpi"), QStringLiteral("screen changed to %1 dpr=%2")
                                              .arg(screen ? screen->name() : QStringLiteral("?"))
                                              .arg(screen ? screen->devicePixelRatio() : 0));
        applyRegion();
    });
}

void SpikeController::setHotkeyName(const QString &name) {
    hotkeyName_ = name.isEmpty() ? QStringLiteral("none (registration failed)") : name;
    emit hotkeyNameChanged();
}

void SpikeController::logEnvironment() {
    spike::log(QStringLiteral("env"), spike::windowsVersion());
    spike::log(QStringLiteral("env"), QStringLiteral("Qt %1, accent API=%2, DWM composition=%3, transparency setting=%4, own integrity=%5")
                                          .arg(QLatin1String(qVersion()))
                                          .arg(spike::WindowEffects::isAccentAvailable())
                                          .arg(spike::compositionEnabled())
                                          .arg(spike::transparencyEffectsEnabled())
                                          .arg(spike::integrityName(spike::integrityLevel(GetCurrentProcessId()))));
    for (QScreen *screen : QGuiApplication::screens()) {
        spike::log(QStringLiteral("env"), QStringLiteral("screen %1 geometry=%2,%3 %4x%5 available=%6,%7 %8x%9 dpr=%10 refresh=%11")
                                              .arg(screen->name())
                                              .arg(screen->geometry().x()).arg(screen->geometry().y())
                                              .arg(screen->geometry().width()).arg(screen->geometry().height())
                                              .arg(screen->availableGeometry().x()).arg(screen->availableGeometry().y())
                                              .arg(screen->availableGeometry().width()).arg(screen->availableGeometry().height())
                                              .arg(screen->devicePixelRatio())
                                              .arg(screen->refreshRate()));
    }
}

void SpikeController::setEffectMode(int mode) {
    if (mode < 1 || mode > 4 || mode == int(mode_)) return;
    mode_ = spike::EffectMode(mode);
    spike::log(QStringLiteral("effect"), QStringLiteral("mode -> %1").arg(effectName()));
    emit effectChanged();
    if (window_ && window_->isVisible()) {
        updateGeometry(window_->height() - shadowMarginTop());
        applyEffects();
    }
}

QString SpikeController::effectName() const {
    switch (mode_) {
    case spike::EffectMode::AcrylicRegion: return QStringLiteral("F1 acrylic + region corners");
    case spike::EffectMode::BlurRegion: return QStringLiteral("F2 blur + region corners");
    case spike::EffectMode::AcrylicDwmShadow: return QStringLiteral("F3 acrylic, square, DWM shadow");
    case spike::EffectMode::SolidQmlShadow: return QStringLiteral("F4 solid, QML corners + shadow");
    }
    return QString();
}

void SpikeController::adjustTint(int delta) {
    const int alpha = std::clamp(tintAlpha() + delta, 0, 255);
    tint_ = (quint32(alpha) << 24) | (tint_ & 0x00FFFFFF);
    spike::log(QStringLiteral("effect"), QStringLiteral("tint=0x%1").arg(tint_, 8, 16, QLatin1Char('0')));
    emit effectChanged();
    applyEffects();
}

void SpikeController::toggle(const QString &trigger) {
    if (!window_) return;
    if (window_->isVisible()) {
        hideDeck(trigger);
    } else {
        showDeck(trigger);
    }
}

void SpikeController::showDeck(const QString &trigger) {
    HWND foreground = GetForegroundWindow();
    if (foreground && !spike::belongsToCurrentProcess(foreground)) target_ = foreground;
    spike::log(QStringLiteral("focus"), QStringLiteral("show via %1; target={%2}")
                                            .arg(trigger, spike::describeWindow(target_)));

    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) screen = QGuiApplication::primaryScreen();
    window_->setScreen(screen);
    const HeightLimits limits = heightLimits(screen->availableGeometry());
    const qreal saved = QSettings().value(savedHeightKey, limits.preferred).toReal();
    updateGeometry(saved);

    window_->show();
    applyEffects();
    HWND deck = deckWindow();
    const bool foregroundOk = SetForegroundWindow(deck) != FALSE;
    window_->requestActivate();
    emit deckShown();

    QTimer::singleShot(80, this, [this, deck, foregroundOk] {
        HWND now = GetForegroundWindow();
        spike::log(QStringLiteral("focus"), QStringLiteral("SetForegroundWindow(deck)=%1; after 80 ms foreground is deck=%2; qt active=%3; dpi=%4")
                                                .arg(foregroundOk)
                                                .arg(now == deck)
                                                .arg(window_ && window_->isActive())
                                                .arg(GetDpiForWindow(deck)));
        RECT rect{};
        GetWindowRect(deck, &rect);
        MONITORINFO info{sizeof(MONITORINFO)};
        GetMonitorInfoW(MonitorFromWindow(deck, MONITOR_DEFAULTTONEAREST), &info);
        spike::log(QStringLiteral("geometry"), QStringLiteral("window px=%1,%2-%3,%4 rcWork px=%5,%6-%7,%8")
                                                   .arg(rect.left).arg(rect.top).arg(rect.right).arg(rect.bottom)
                                                   .arg(info.rcWork.left).arg(info.rcWork.top)
                                                   .arg(info.rcWork.right).arg(info.rcWork.bottom));
    });
}

void SpikeController::hideDeck(const QString &reason) {
    if (!window_ || !window_->isVisible()) return;
    spike::log(QStringLiteral("focus"), QStringLiteral("hide: %1").arg(reason));
    window_->hide();
    emit deckHidden();
}

void SpikeController::updateGeometry(qreal height) {
    QScreen *screen = window_->screen();
    const QRect area = screen->availableGeometry();
    const HeightLimits limits = heightLimits(area);
    const qreal deckHeight = std::clamp(height, limits.minimum, limits.maximum);
    const qreal top = shadowMarginTop();
    const qreal side = shadowMarginSide();
    // The Deck sits 8 DIP from both edges and flush with the work area bottom;
    // the QML shadow mode borrows that 8 DIP gutter for its shadow.
    window_->setGeometry(int(area.x() + 8 - side),
                         int(area.y() + area.height() - deckHeight - top),
                         int(area.width() - 16 + side * 2),
                         int(deckHeight + top));
}

void SpikeController::applyEffects() {
    HWND window = deckWindow();
    if (!window) return;
    using spike::Accent;
    using spike::EffectMode;
    bool accentOk = true;
    switch (mode_) {
    case EffectMode::AcrylicRegion:
        accentOk = spike::WindowEffects::setAccent(window, resizing_ && !keepAcrylicDuringResize_
                                                               ? Accent::BlurBehind : Accent::AcrylicBlurBehind, tint_);
        spike::WindowEffects::setDwmShadow(window, false);
        break;
    case EffectMode::BlurRegion:
        accentOk = spike::WindowEffects::setAccent(window, Accent::BlurBehind, tint_);
        spike::WindowEffects::setDwmShadow(window, false);
        break;
    case EffectMode::AcrylicDwmShadow:
        accentOk = spike::WindowEffects::setAccent(window, resizing_ && !keepAcrylicDuringResize_
                                                               ? Accent::BlurBehind : Accent::AcrylicBlurBehind, tint_);
        spike::WindowEffects::setDwmShadow(window, true);
        break;
    case EffectMode::SolidQmlShadow:
        spike::WindowEffects::setAccent(window, Accent::Disabled, 0);
        spike::WindowEffects::setDwmShadow(window, false);
        break;
    }
    if (!accentOk) spike::log(QStringLiteral("effect"), QStringLiteral("SetWindowCompositionAttribute failed"));
    applyRegion();
}

void SpikeController::applyRegion() {
    HWND window = deckWindow();
    if (!window || !window_->isVisible()) return;
    const bool useRegion = mode_ == spike::EffectMode::AcrylicRegion || mode_ == spike::EffectMode::BlurRegion;
    const int radius = useRegion ? int(std::lround(24 * window_->devicePixelRatio())) : 0;
    spike::WindowEffects::setRoundedRegion(window, radius);
}

void SpikeController::beginResize(qreal globalY) {
    resizing_ = true;
    resizeStartY_ = globalY;
    resizeStartHeight_ = window_->height() - shadowMarginTop();
    applyEffects();
}

void SpikeController::updateResize(qreal globalY) {
    if (!resizing_) return;
    updateGeometry(resizeStartHeight_ + (resizeStartY_ - globalY));
}

void SpikeController::endResize() {
    if (!resizing_) return;
    resizing_ = false;
    const qreal height = window_->height() - shadowMarginTop();
    QSettings().setValue(savedHeightKey, height);
    spike::log(QStringLiteral("geometry"), QStringLiteral("resized to %1 DIP").arg(height));
    applyEffects();
}

void SpikeController::paste(const QString &text, bool plainText, const QString &trigger) {
    if (selfTest_) return;
    HWND deck = deckWindow();
    paste_.paste(text, plainText, trigger, target_, deck, [this] { hideDeck(QStringLiteral("paste")); });
}

void SpikeController::onActiveChanged() {
    if (!window_ || window_->isActive() || !window_->isVisible()) return;
    if (pinned_ || resizing_ || paste_.isRunning() || selfTest_) return;
    // Mirrors windowDidResignKey: decide on the next event-loop turn.
    QTimer::singleShot(0, this, [this] {
        if (!window_ || window_->isActive() || !window_->isVisible()) return;
        HWND foreground = GetForegroundWindow();
        if (spike::belongsToCurrentProcess(foreground)) return;
        hideDeck(QStringLiteral("lost activation to {%1}").arg(spike::describeWindow(foreground)));
    });
}

HWND SpikeController::deckWindow() const {
    return window_ ? reinterpret_cast<HWND>(window_->winId()) : nullptr;
}

void SpikeController::log(const QString &area, const QString &message) { spike::log(area, message); }

void SpikeController::setStatus(const QString &status) {
    status_ = status;
    emit statusChanged();
}

void SpikeController::finishSelfTest(const QString &summary) {
    spike::log(QStringLiteral("selftest"), summary);
    const int warnings = spike::warningCount();
    spike::log(QStringLiteral("selftest"), QStringLiteral("warnings=%1 -> %2")
                                               .arg(warnings)
                                               .arg(warnings == 0 ? QStringLiteral("PASS") : QStringLiteral("FAIL")));
    QCoreApplication::exit(warnings == 0 ? 0 : 1);
}

void SpikeController::quit() { QCoreApplication::quit(); }
