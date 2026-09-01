#include "script_editor_widget.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QVBoxLayout>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>

#include <algorithm>
#include <utility>

namespace {

constexpr auto kMessagePrefix = "__CLIPDECK_EDITOR_HOST__";

class ScriptEditorPage final : public QWebEnginePage {
public:
    explicit ScriptEditorPage(QObject *parent, std::function<void(const QString &)> callback)
        : QWebEnginePage(parent), callback_(std::move(callback)) {}

    void setCallback(std::function<void(const QString &)> callback) {
        callback_ = std::move(callback);
    }

protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level,
                                  const QString &message, int lineNumber,
                                  const QString &sourceID) override {
        if (message.startsWith(QLatin1String(kMessagePrefix))) {
            if (callback_) callback_(message.mid(static_cast<int>(sizeof(kMessagePrefix) - 1)));
            return;
        }
        QWebEnginePage::javaScriptConsoleMessage(level, message, lineNumber, sourceID);
    }

private:
    std::function<void(const QString &)> callback_;
};

QString editorIndexPath() {
    const QDir appDirectory(QCoreApplication::applicationDirPath());
    const QStringList candidates = {
        appDirectory.absoluteFilePath(QStringLiteral("../share/script-editor/index.html")),
        appDirectory.absoluteFilePath(QStringLiteral("../../../script-editor/dist/index.html")),
        QDir::current().absoluteFilePath(QStringLiteral("script-editor/dist/index.html")),
        QDir::current().absoluteFilePath(QStringLiteral("../script-editor/dist/index.html")),
    };
    for (const QString &candidate : candidates) {
        const QFileInfo file(candidate);
        if (file.isFile()) return file.canonicalFilePath();
    }
    return QString();
}

QPointer<QWebEngineView> &prewarmedEditorView() {
    static QPointer<QWebEngineView> view;
    return view;
}

QWebEngineView *makeEditorView(QWidget *parent,
                               std::function<void(const QString &)> callback) {
    // Do not reparent the hidden prewarm view. QtWebEngine 5.11 keeps a native
    // child window whose geometry can remain tied to the old top-level, which
    // offsets expanded editors and can race the initial document handshake.
    // The hidden view only keeps the renderer process warm; every visible
    // editor owns a fresh view and page.
    auto *view = new QWebEngineView(parent);
    view->setPage(new ScriptEditorPage(view, std::move(callback)));
    return view;
}

} // namespace

void ScriptEditorWidget::prewarm() {
    if (prewarmedEditorView()) return;
    const QString path = editorIndexPath();
    if (path.isEmpty()) return;

    auto *view = new QWebEngineView;
    auto *page = new ScriptEditorPage(view, {});
    page->setBackgroundColor(QColor(QStringLiteral("#1d1d1f")));
    view->setPage(page);
    view->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    view->setContextMenuPolicy(Qt::NoContextMenu);
    prewarmedEditorView() = view;
    view->load(QUrl::fromLocalFile(path));
}

ScriptEditorWidget::ScriptEditorWidget(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("scriptEditorHost"));
    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    view_ = makeEditorView(this, [this](const QString &message) {
        handleHostMessage(message);
    });
    auto *page = static_cast<ScriptEditorPage *>(view_->page());
    page->setBackgroundColor(QColor(QStringLiteral("#1d1d1f")));
    view_->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    view_->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);
    view_->setContextMenuPolicy(Qt::NoContextMenu);
    layout->addWidget(view_);

    QObject::connect(view_, &QWebEngineView::loadFinished, this, [this](bool ok) {
        if (!ok) return;
        ready_ = true;
        attachAttempts_ = 0;
        attachHost();
    });

    const QString path = editorIndexPath();
    if (path.isEmpty()) {
        view_->setHtml(QStringLiteral(
            "<!doctype html><meta charset='utf-8'><body style='margin:0;padding:12px;"
            "font:13px sans-serif;color:#ff7373;background:#1d1d1f'>"
            "无法加载公共脚本编辑器资源</body>"));
    } else {
        view_->load(QUrl::fromLocalFile(path));
    }
}

QString ScriptEditorWidget::text() const { return text_; }

void ScriptEditorWidget::setText(const QString &text) {
    if (text_ == text) return;
    text_ = text;
    if (!ready_) return;
    scheduleDocumentUpdate();
}

void ScriptEditorWidget::clear() { setText(QString()); }

void ScriptEditorWidget::focusEditor() {
    if (!ready_) {
        focusPending_ = true;
        return;
    }
    send(QJsonObject{{QStringLiteral("type"), QStringLiteral("focus")}});
}

void ScriptEditorWidget::setChangeCallback(std::function<void(const QString &)> callback) {
    changeCallback_ = std::move(callback);
}

void ScriptEditorWidget::setSaveCallback(std::function<void()> callback) {
    saveCallback_ = std::move(callback);
}

void ScriptEditorWidget::setRunCallback(std::function<void()> callback) {
    runCallback_ = std::move(callback);
}

void ScriptEditorWidget::setFormatCallback(std::function<void()> callback) {
    formatCallback_ = std::move(callback);
}

void ScriptEditorWidget::handleHostMessage(const QString &message) {
    const QJsonDocument document = QJsonDocument::fromJson(message.toUtf8());
    if (!document.isObject()) return;
    const QJsonObject payload = document.object();
    const QString type = payload.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("ready")) {
        // loadFinished performs an explicit initialize and verifies the
        // resulting document. The ready notification remains useful when the
        // frontend finishes before the host bridge is attached, but is no
        // longer the only path that can populate the editor.
        if (!ready_) {
            ready_ = true;
            attachAttempts_ = 0;
            attachHost();
        }
        return;
    }
    if (type == QLatin1String("change")) {
        // A late change notification can belong to the category that is being
        // replaced. Do not let it overwrite the newly selected script while a
        // coalesced external document update is pending.
        if (documentUpdateQueued_) return;
        revision_ = std::max(revision_, payload.value(QStringLiteral("revision")).toInt());
        const QString changedText = payload.value(QStringLiteral("text")).toString();
        if (changedText == text_) return;
        text_ = changedText;
        if (changeCallback_) changeCallback_(text_);
        return;
    }
    if (type != QLatin1String("shortcut")) return;
    const QString command = payload.value(QStringLiteral("command")).toString();
    if (command == QLatin1String("save") && saveCallback_) saveCallback_();
    else if (command == QLatin1String("run") && runCallback_) runCallback_();
    else if (command == QLatin1String("format") && formatCallback_) formatCallback_();
}

void ScriptEditorWidget::attachHost() {
    const QJsonObject initialize{
        {QStringLiteral("type"), QStringLiteral("initialize")},
        {QStringLiteral("revision"), revision_},
        {QStringLiteral("text"), text_},
        {QStringLiteral("theme"), QStringLiteral("dark")},
        {QStringLiteral("autofocus"), false},
    };
    const QString json = QString::fromUtf8(
        QJsonDocument(initialize).toJson(QJsonDocument::Compact));
    const QString script = QStringLiteral(
        "(function(){"
        "if(!window.clipDeckAttachHost||!window.clipDeckEditor)return false;"
        "window.clipDeckAttachHost({postMessage:function(message){"
        "console.log('%1'+JSON.stringify(message));}});"
        "var initial=%2;"
        "window.clipDeckEditor.receive(initial);"
        "return window.clipDeckEditor.getDocument()===initial.text;"
        "})()")
        .arg(QLatin1String(kMessagePrefix), json);
    const QPointer<ScriptEditorWidget> guard(this);
    view_->page()->runJavaScript(script, [guard](const QVariant &result) {
        if (!guard) return;
        if (!result.toBool() && guard->attachAttempts_++ < 5) {
            QTimer::singleShot(30, guard, [guard] {
                if (guard) guard->attachHost();
            });
            return;
        }
        if (!result.toBool()) {
            qWarning() << "[ClipDeck][ScriptEditor] failed to initialize document after retries";
            return;
        }
        guard->attachAttempts_ = 0;
        qInfo() << "[ClipDeck][ScriptEditor] initialized document characters="
                << guard->text_.size();
        if (guard->focusPending_) {
            guard->focusPending_ = false;
            guard->send(QJsonObject{{QStringLiteral("type"), QStringLiteral("focus")}});
        }
    });
}

void ScriptEditorWidget::scheduleDocumentUpdate() {
    if (documentUpdateQueued_) return;
    documentUpdateQueued_ = true;
    // Updating CodeMirror crosses into the WebEngine renderer and reparses the
    // JavaScript document. Let the category list commit its selected/icon state
    // first, then coalesce rapid navigation to the final script in one frame.
    QTimer::singleShot(16, this, [this] {
        documentUpdateQueued_ = false;
        if (!ready_) return;
        ++revision_;
        send(QJsonObject{{QStringLiteral("type"), QStringLiteral("setDocument")},
                         {QStringLiteral("revision"), revision_},
                         {QStringLiteral("text"), text_}});
    });
}

void ScriptEditorWidget::send(const QJsonObject &payload) {
    const QString json = QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
    view_->page()->runJavaScript(QStringLiteral("window.clipDeckEditor && window.clipDeckEditor.receive(%1);")
                                    .arg(json));
}
