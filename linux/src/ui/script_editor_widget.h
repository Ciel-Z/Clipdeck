#pragma once

#include <QWidget>

#include <functional>

class QWebEngineView;
class QJsonObject;

class ScriptEditorWidget final : public QWidget {
public:
    explicit ScriptEditorWidget(QWidget *parent = nullptr);

    // Warm the shared QtWebEngine renderer before the settings window first
    // constructs an editor. Visible editors keep independent native views.
    static void prewarm();

    QString text() const;
    void setText(const QString &text);
    void clear();
    void focusEditor();

    void setChangeCallback(std::function<void(const QString &)> callback);
    void setSaveCallback(std::function<void()> callback);
    void setRunCallback(std::function<void()> callback);
    void setFormatCallback(std::function<void()> callback);

private:
    void handleHostMessage(const QString &message);
    void attachHost();
    void scheduleDocumentUpdate();
    void send(const QJsonObject &payload);

    QWebEngineView *view_ = nullptr;
    QString text_;
    int revision_ = 0;
    bool ready_ = false;
    bool focusPending_ = false;
    bool documentUpdateQueued_ = false;
    int attachAttempts_ = 0;
    std::function<void(const QString &)> changeCallback_;
    std::function<void()> saveCallback_;
    std::function<void()> runCallback_;
    std::function<void()> formatCallback_;
};
