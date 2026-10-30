#pragma once

#include "paste_flow.h"
#include "window_effects.h"

#include <QObject>
#include <QPointer>
#include <QQuickWindow>
#include <QString>

class QSystemTrayIcon;

// Glue between the QML Deck mock and the Win32 experiments. Everything the
// tester can toggle is a property so the HUD always shows the active setup.
class SpikeController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int effectMode READ effectMode WRITE setEffectMode NOTIFY effectChanged)
    Q_PROPERTY(QString effectName READ effectName NOTIFY effectChanged)
    Q_PROPERTY(int tintAlpha READ tintAlpha NOTIFY effectChanged)
    Q_PROPERTY(bool keepAcrylicDuringResize MEMBER keepAcrylicDuringResize_ NOTIFY effectChanged)
    Q_PROPERTY(bool pinned MEMBER pinned_ NOTIFY pinnedChanged)
    Q_PROPERTY(qreal shadowMarginTop READ shadowMarginTop NOTIFY effectChanged)
    Q_PROPERTY(qreal shadowMarginSide READ shadowMarginSide NOTIFY effectChanged)
    Q_PROPERTY(QString graphicsApi READ graphicsApi NOTIFY graphicsApiChanged)
    Q_PROPERTY(QString hotkeyName MEMBER hotkeyName_ NOTIFY hotkeyNameChanged)
    Q_PROPERTY(QString status MEMBER status_ NOTIFY statusChanged)
    Q_PROPERTY(bool selfTest READ selfTest CONSTANT)

public:
    explicit SpikeController(bool selfTest, QObject *parent = nullptr);

    void attach(QQuickWindow *window, QSystemTrayIcon *tray);
    void setHotkeyName(const QString &name);
    void logEnvironment();

    int effectMode() const { return int(mode_); }
    void setEffectMode(int mode);
    QString effectName() const;
    int tintAlpha() const { return int(tint_ >> 24); }
    qreal shadowMarginTop() const { return mode_ == spike::EffectMode::SolidQmlShadow ? 16 : 0; }
    qreal shadowMarginSide() const { return mode_ == spike::EffectMode::SolidQmlShadow ? 8 : 0; }
    QString graphicsApi() const { return graphicsApi_; }
    bool selfTest() const { return selfTest_; }

    Q_INVOKABLE void toggle(const QString &trigger);
    Q_INVOKABLE void hideDeck(const QString &reason);
    Q_INVOKABLE void adjustTint(int delta);
    Q_INVOKABLE void beginResize(qreal globalY);
    Q_INVOKABLE void updateResize(qreal globalY);
    Q_INVOKABLE void endResize();
    Q_INVOKABLE void paste(const QString &text, bool plainText, const QString &trigger);
    Q_INVOKABLE void log(const QString &area, const QString &message);
    Q_INVOKABLE void finishSelfTest(const QString &summary);
    Q_INVOKABLE void quit();

signals:
    void effectChanged();
    void pinnedChanged();
    void graphicsApiChanged();
    void hotkeyNameChanged();
    void statusChanged();
    void deckShown();
    void deckHidden();

private:
    void showDeck(const QString &trigger);
    void updateGeometry(qreal height);
    void applyEffects();
    void applyRegion();
    void onActiveChanged();
    HWND deckWindow() const;
    void setStatus(const QString &status);

    QPointer<QQuickWindow> window_;
    QSystemTrayIcon *tray_ = nullptr;
    PasteFlow paste_;
    HWND target_ = nullptr;
    spike::EffectMode mode_ = spike::EffectMode::AcrylicRegion;
    // Close to the macOS stack: material at 62% plus black 20%.
    quint32 tint_ = 0x99161618;
    bool keepAcrylicDuringResize_ = false;
    bool pinned_ = false;
    bool resizing_ = false;
    bool selfTest_ = false;
    qreal resizeStartY_ = 0;
    qreal resizeStartHeight_ = 0;
    QString graphicsApi_ = QStringLiteral("pending");
    QString hotkeyName_;
    QString status_;
};
