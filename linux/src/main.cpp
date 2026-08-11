#include "history_repository.h"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QCursor>
#include <QDateTime>
#include <QDesktopWidget>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMenu>
#include <QMap>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegion>
#include <QScreen>
#include <QSlider>
#include <QShowEvent>
#include <QSqlQuery>
#include <QStackedWidget>
#include <QSettings>
#include <QSpinBox>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QSyntaxHighlighter>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QUrl>
#include <QTimer>
#include <QTextStream>
#include <QTransform>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWheelEvent>
#include <QX11Info>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <xcb/xcb.h>

#ifdef KeyPress
#undef KeyPress
#endif

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace {

constexpr int kHistoryCapacity = 100;
constexpr int kHorizontalInset = 8;
constexpr int kCardSpacing = 24;
constexpr int kContentInset = 22;

QKeySequence configuredShortcut(const QString &key, const QKeySequence &fallback) {
    const QString value = QSettings().value(key, fallback.toString(QKeySequence::PortableText)).toString();
    const QKeySequence sequence(value, QKeySequence::PortableText);
    return sequence.isEmpty() ? fallback : sequence;
}

Qt::KeyboardModifiers configuredModifier(const QString &key, Qt::KeyboardModifier fallback) {
    return static_cast<Qt::KeyboardModifiers>(
        QSettings().value(key, static_cast<int>(fallback)).toInt());
}

bool matchesShortcut(const QKeyEvent *event, const QString &key, const QKeySequence &fallback) {
    const QKeySequence configured = configuredShortcut(key, fallback);
    return !configured.isEmpty()
        && configured[0] == (event->key() | static_cast<int>(event->modifiers()));
}

Qt::KeyboardModifier modifierForKey(int key) {
    if (key == Qt::Key_Control) return Qt::ControlModifier;
    if (key == Qt::Key_Shift) return Qt::ShiftModifier;
    if (key == Qt::Key_Alt) return Qt::AltModifier;
    if (key == Qt::Key_Meta) return Qt::MetaModifier;
    return Qt::NoModifier;
}

QString relativeTime(const QDateTime &dateTime) {
    const qint64 seconds = std::max<qint64>(0, dateTime.secsTo(QDateTime::currentDateTime()));
    if (seconds < 60) {
        return QStringLiteral("现在");
    }
    if (seconds < 3600) {
        return QStringLiteral("%1 分钟前").arg(seconds / 60);
    }
    if (seconds < 86400) {
        return QStringLiteral("%1 小时前").arg(seconds / 3600);
    }
    return QStringLiteral("%1 天前").arg(seconds / 86400);
}

QString displayCodeLanguage(const QString &language) {
    static const QMap<QString, QString> names = {
        {QStringLiteral("bash"), QStringLiteral("Shell")},
        {QStringLiteral("cpp"), QStringLiteral("C++")},
        {QStringLiteral("csharp"), QStringLiteral("C#")},
        {QStringLiteral("javascript"), QStringLiteral("JavaScript")},
        {QStringLiteral("objectivec"), QStringLiteral("Objective-C")},
        {QStringLiteral("typescript"), QStringLiteral("TypeScript")},
        {QStringLiteral("xml"), QStringLiteral("HTML/XML")}
    };
    return names.value(language, language.isEmpty()
        ? QString() : language.left(1).toUpper() + language.mid(1));
}

bool containsLocalFileURL(const QMimeData *mimeData) {
    if (!mimeData || !mimeData->hasUrls()) return false;
    for (const QUrl &url : mimeData->urls()) {
        if (url.isLocalFile() || url.scheme().compare(QStringLiteral("file"), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

QByteArray clipboardFingerprint(const QMimeData *mimeData) {
    if (!mimeData) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QStringList formats = mimeData->formats();
    for (const QString &format : formats) hash.addData(format.toUtf8());
    if (mimeData->hasImage()) {
        const QImage image = qvariant_cast<QImage>(mimeData->imageData());
        hash.addData(QByteArray::number(image.width()));
        hash.addData(QByteArray::number(image.height()));
        if (!image.isNull()) {
            hash.addData(QByteArray::number(image.pixel(0, 0)));
            hash.addData(QByteArray::number(image.pixel(image.width() / 2, image.height() / 2)));
            hash.addData(QByteArray::number(image.pixel(image.width() - 1, image.height() - 1)));
        }
        const QByteArray png = mimeData->data(QStringLiteral("image/png"));
        if (!png.isEmpty()) hash.addData(png);
    } else {
        hash.addData(mimeData->text().toUtf8());
        if (mimeData->hasHtml()) hash.addData(mimeData->html().toUtf8());
    }
    return hash.result();
}

Window topLevelWindow(Display *display, Window window) {
    if (!display || window == None || window == PointerRoot) {
        return None;
    }

    Window current = window;
    while (true) {
        Window root = None;
        Window parent = None;
        Window *children = nullptr;
        unsigned int childCount = 0;
        if (!XQueryTree(display, current, &root, &parent, &children, &childCount)) {
            return window;
        }
        if (children) {
            XFree(children);
        }
        if (parent == None || parent == root) {
            return current;
        }
        current = parent;
    }
}

Window activeTopLevelWindow(Display *display) {
    if (!display) {
        return None;
    }
    const Atom property = XInternAtom(display, "_NET_ACTIVE_WINDOW", True);
    if (property == None) {
        return None;
    }
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(display, DefaultRootWindow(display), property, 0, 1, False,
                           XA_WINDOW, &actualType, &actualFormat, &itemCount, &bytesAfter,
                           &data) != Success || !data || itemCount == 0) {
        if (data) XFree(data);
        return None;
    }
    const Window window = *reinterpret_cast<Window *>(data);
    XFree(data);
    return window;
}

struct SourceApplicationInfo {
    QString identifier = QStringLiteral("unknown");
    QString name = QStringLiteral("Unknown");
    QByteArray iconData;
    QImage icon;
};

class CardSyntaxHighlighter final : public QSyntaxHighlighter {
public:
    CardSyntaxHighlighter(QTextDocument *document, QString language)
        : QSyntaxHighlighter(document), language_(std::move(language)) {}

protected:
    void highlightBlock(const QString &text) override {
        auto apply = [this, &text](const QString &pattern, const QColor &color,
                                  QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption) {
            QRegularExpression expression(pattern, options);
            auto matches = expression.globalMatch(text);
            QTextCharFormat format;
            format.setForeground(color);
            while (matches.hasNext()) {
                const auto match = matches.next();
                setFormat(match.capturedStart(), match.capturedLength(), format);
            }
        };

        QStringList keywords;
        if (language_ == QStringLiteral("sql")) {
            keywords = QStringList{QStringLiteral("select"), QStringLiteral("from"), QStringLiteral("where"),
                        QStringLiteral("join"), QStringLiteral("insert"), QStringLiteral("update"),
                        QStringLiteral("delete"), QStringLiteral("create"), QStringLiteral("alter"),
                        QStringLiteral("drop"), QStringLiteral("group"), QStringLiteral("order"),
                        QStringLiteral("by"), QStringLiteral("limit"), QStringLiteral("as"),
                        QStringLiteral("and"), QStringLiteral("or"), QStringLiteral("not"),
                        QStringLiteral("into"), QStringLiteral("values"), QStringLiteral("set")};
        } else if (language_ == QStringLiteral("shell")) {
            keywords = QStringList{QStringLiteral("if"), QStringLiteral("then"), QStringLiteral("else"),
                        QStringLiteral("fi"), QStringLiteral("for"), QStringLiteral("while"),
                        QStringLiteral("do"), QStringLiteral("done"), QStringLiteral("case"),
                        QStringLiteral("esac"), QStringLiteral("function"), QStringLiteral("export"),
                        QStringLiteral("local"), QStringLiteral("return")};
        } else if (language_ != QStringLiteral("text") && !language_.isEmpty()) {
            keywords = QStringList{QStringLiteral("function"), QStringLiteral("return"), QStringLiteral("const"),
                        QStringLiteral("let"), QStringLiteral("var"), QStringLiteral("if"),
                        QStringLiteral("else"), QStringLiteral("for"), QStringLiteral("while"),
                        QStringLiteral("class"), QStringLiteral("new"), QStringLiteral("try"),
                        QStringLiteral("catch"), QStringLiteral("throw"), QStringLiteral("import"),
                        QStringLiteral("export"), QStringLiteral("async"), QStringLiteral("await"),
                        QStringLiteral("public"), QStringLiteral("private"), QStringLiteral("static")};
        }
        if (!keywords.isEmpty()) {
            apply(QStringLiteral("\\b(?:%1)\\b").arg(keywords.join(QLatin1Char('|'))),
                  QColor(191, 132, 255), QRegularExpression::CaseInsensitiveOption);
        }
        apply(QStringLiteral("\\b(?:true|false|null|undefined|nil|NULL)\\b"), QColor(191, 132, 255));
        apply(QStringLiteral("\\b\\d+(?:\\.\\d+)?\\b"), QColor(75, 208, 190));
        apply(QStringLiteral("(['\"])(?:\\\\.|(?!\\1).)*\\1"), QColor(255, 159, 67));
        apply(QStringLiteral("//.*|--.*|#(?![# ]).*"), QColor(135, 135, 140));
        if (language_ == QStringLiteral("json")) {
            apply(QStringLiteral("\"(?:\\\\.|[^\"])*\"(?=\\s*:)") , QColor(82, 168, 255));
        } else if (language_ == QStringLiteral("markdown")) {
            apply(QStringLiteral("^#{1,6}\\s+.*$"), QColor(82, 168, 255));
            apply(QStringLiteral("`[^`]+`|\\*\\*[^*]+\\*\\*"), QColor(255, 159, 67));
        }
    }

private:
    QString language_;
};

void paintHighlightedText(QPainter &painter, const QRect &rect, const QString &text,
                          const QString &language, const QFont &baseFont) {
    QTextDocument document;
    document.setDocumentMargin(0);
    QFont contentFont = baseFont;
    contentFont.setPointSizeF(13.5);
    if (!language.isEmpty() && language != QStringLiteral("text")) {
        contentFont.setFamily(QStringLiteral("monospace"));
        contentFont.setStyleHint(QFont::Monospace);
    }
    document.setDefaultFont(contentFont);
    document.setDefaultStyleSheet(QStringLiteral("body{color:rgba(255,255,255,0.90);}"));
    document.setPlainText(text.left(3000));
    QTextCursor cursor(&document);
    cursor.select(QTextCursor::Document);
    QTextCharFormat foreground;
    foreground.setForeground(QColor(255, 255, 255, 230));
    cursor.mergeCharFormat(foreground);
    CardSyntaxHighlighter highlighter(&document, language);
    highlighter.rehighlight();
    document.setTextWidth(rect.width());
    painter.save();
    painter.setClipRect(rect);
    painter.translate(rect.topLeft());
    document.drawContents(&painter, QRectF(0, 0, rect.width(), rect.height()));
    painter.restore();
}

QImage windowIcon(Display *display, Window window) {
    if (!display || window == None) return {};
    const Atom iconAtom = XInternAtom(display, "_NET_WM_ICON", True);
    if (iconAtom == None) return {};
    Atom actualType = None;
    int actualFormat = 0;
    unsigned long itemCount = 0;
    unsigned long bytesAfter = 0;
    unsigned char *raw = nullptr;
    if (XGetWindowProperty(display, window, iconAtom, 0, 1024 * 1024, False, XA_CARDINAL,
                           &actualType, &actualFormat, &itemCount, &bytesAfter, &raw) != Success
        || !raw || actualFormat != 32) {
        if (raw) XFree(raw);
        return {};
    }

    const auto *values = reinterpret_cast<unsigned long *>(raw);
    unsigned long offset = 0;
    QImage best;
    int bestDistance = std::numeric_limits<int>::max();
    while (offset + 2 <= itemCount) {
        const int width = static_cast<int>(values[offset]);
        const int height = static_cast<int>(values[offset + 1]);
        const unsigned long pixelCount = width > 0 && height > 0
            ? static_cast<unsigned long>(width) * static_cast<unsigned long>(height) : 0;
        if (pixelCount == 0 || offset + 2 + pixelCount > itemCount) break;
        const int distance = std::abs(std::max(width, height) - 64);
        if (width <= 512 && height <= 512 && distance < bestDistance) {
            QImage candidate(width, height, QImage::Format_ARGB32);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    candidate.setPixel(x, y, static_cast<QRgb>(values[offset + 2 + y * width + x]));
                }
            }
            best = candidate;
            bestDistance = distance;
        }
        offset += 2 + pixelCount;
    }
    XFree(raw);
    return best;
}

SourceApplicationInfo activeApplication(Display *display) {
    SourceApplicationInfo result;
    if (!display) {
        return result;
    }
    Window focused = None;
    int revertTo = RevertToNone;
    XGetInputFocus(display, &focused, &revertTo);
    const Window topLevel = topLevelWindow(display, focused);
    if (topLevel == None) {
        return result;
    }

    XClassHint classHint{};
    if (XGetClassHint(display, topLevel, &classHint)) {
        if (classHint.res_class) {
            result.identifier = QString::fromLocal8Bit(classHint.res_class);
            result.name = result.identifier;
            XFree(classHint.res_class);
        }
        if (classHint.res_name) {
            if (result.name == QStringLiteral("Unknown")) {
                result.name = QString::fromLocal8Bit(classHint.res_name);
            }
            XFree(classHint.res_name);
        }
    }
    char *windowName = nullptr;
    if (XFetchName(display, topLevel, &windowName) && windowName) {
        const QString title = QString::fromLocal8Bit(windowName).trimmed();
        if (!title.isEmpty() && result.name == QStringLiteral("Unknown")) {
            result.name = title;
        }
        XFree(windowName);
    }
    result.icon = windowIcon(display, topLevel);
    if (!result.icon.isNull()) {
        QBuffer buffer(&result.iconData);
        buffer.open(QIODevice::WriteOnly);
        result.icon.save(&buffer, "PNG");
    }
    return result;
}

QIcon transparentTrayIcon() {
    // UOS displays application icons literally in the tray. Drawing a small
    // monochrome glyph keeps the panel transparent instead of showing the
    // coloured square background from the regular application icon.
    QPixmap pixmap(44, 44);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor(235, 235, 235), 4.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    painter.setPen(pen);
    painter.drawPolyline(QPolygonF({QPointF(17, 12), QPointF(9, 22), QPointF(17, 32)}));
    painter.drawPolyline(QPolygonF({QPointF(27, 12), QPointF(35, 22), QPointF(27, 32)}));
    painter.drawLine(QPointF(25, 9), QPointF(19, 35));
    painter.end();
    pixmap.setDevicePixelRatio(2.0);
    return QIcon(pixmap);
}

bool setLaunchAtLogin(bool enabled) {
    const QString directory = QDir::home().filePath(QStringLiteral(".config/autostart"));
    const QString path = QDir(directory).filePath(QStringLiteral("clipdeck.desktop"));
    if (!enabled) {
        return !QFile::exists(path) || QFile::remove(path);
    }
    if (!QDir().mkpath(directory)) return false;
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;
    file.write("[Desktop Entry]\nType=Application\nName=ClipDeck\n"
               "Exec=/usr/bin/clipdeck\nIcon=clipdeck\nTerminal=false\n"
               "X-GNOME-Autostart-enabled=true\n");
    return true;
}

class DeckWindow final : public QWidget {
public:
    explicit DeckWindow(Display *display, HistoryRepository *repository, QWidget *parent = nullptr)
        : QWidget(parent), display_(display), repository_(repository) {
        setWindowTitle(QStringLiteral("ClipDeck"));
        setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAutoFillBackground(false);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        searchEdit_ = new QLineEdit(this);
        searchEdit_->setPlaceholderText(QStringLiteral("搜索剪贴板历史"));
        searchEdit_->setClearButtonEnabled(false);
        searchEdit_->setVisible(false);
        searchEdit_->setStyleSheet(QStringLiteral(
            "QLineEdit{color:white;background:rgba(255,255,255,34);border:1px solid rgba(255,255,255,38);"
            "border-radius:17px;padding:5px 13px;font-size:13px;}"
            "QLineEdit:focus{color:white;background:rgba(255,255,255,44);"
            "border:1px solid rgba(255,255,255,66);}"));
        QPalette searchPalette = searchEdit_->palette();
        searchPalette.setColor(QPalette::Text, QColor(255, 255, 255, 235));
        searchEdit_->setPalette(searchPalette);
        QAction *closeSearch = searchEdit_->addAction(
            style()->standardIcon(QStyle::SP_DialogCloseButton), QLineEdit::TrailingPosition);
        QObject::connect(closeSearch, &QAction::triggered, this, [this] { endSearch(); });
        searchEdit_->installEventFilter(this);
        QObject::connect(searchEdit_, &QLineEdit::textChanged, this, [this](const QString &value) {
            searchQuery_ = value.trimmed();
            reload();
        });
        QObject::connect(searchEdit_, &QLineEdit::returnPressed, this, [this] {
            setFocus(Qt::ShortcutFocusReason);
        });
        reload();
        QTimer::singleShot(0, this, [this] {
            const int count = std::min(20, static_cast<int>(items_.size()));
            for (int index = 0; index < count; ++index) fetchLinkMetadataIfNeeded(items_[index]);
        });
    }

    void insertText(const QString &text, const QString &richTextHtml,
                    const SourceApplicationInfo &source) {
        if (text.isEmpty()) {
            return;
        }
        ClipboardItem item;
        item.title = QStringLiteral("文本");
        item.text = text;
        item.richTextHtml = richTextHtml;
        item.sourceID = source.identifier;
        item.sourceName = source.name;
        item.sourceIcon = source.icon;
        item.sourceIconData = source.iconData;
        item.category = repository_->classifyText(text, source.identifier, source.name,
                                                   QDateTime::currentDateTime());
        item.copiedAt = QDateTime::currentDateTime();
        item = repository_->save(item);
        fetchLinkMetadataIfNeeded(item);
        selectedCategory_ = QStringLiteral("History");
        reload();
    }

    void insertImage(const QImage &image, const SourceApplicationInfo &source) {
        if (image.isNull()) {
            return;
        }
        QByteArray pngData;
        QBuffer buffer(&pngData);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG")) {
            return;
        }
        ClipboardItem item;
        item.title = QStringLiteral("图片");
        item.type = ClipboardPayloadType::Image;
        item.image = image;
        item.imageData = pngData;
        item.sourceID = source.identifier;
        item.sourceName = source.name;
        item.sourceIcon = source.icon;
        item.sourceIconData = source.iconData;
        item.copiedAt = QDateTime::currentDateTime();
        repository_->save(item);
        selectedCategory_ = QStringLiteral("History");
        reload();
    }

    void toggleNearPointer() {
        if (isVisible()) {
            hide();
            return;
        }
        showNearPointer();
    }

    void showNearPointer() {
        rememberPasteTarget();

        QDesktopWidget *desktop = QApplication::desktop();
        const int screenNumber = desktop->screenNumber(QCursor::pos());
        const QRect available = desktop->availableGeometry(screenNumber);
        const int maximumHeight = std::max(250, std::min(420, static_cast<int>(available.height() * 0.42)));
        QSettings settings;
        const int preferred = settings.value(QStringLiteral("deck/preferredHeight"),
            std::max(340, static_cast<int>(available.height() * 0.385))).toInt();
        const int height = std::min(maximumHeight, std::max(250, preferred));

        setGeometry(
            available.x() + kHorizontalInset,
            available.y() + available.height() - height,
            std::max(320, available.width() - kHorizontalInset * 2),
            height);
        clampScrollOffset();
        show();
        enableKWinBlur();
        raise();
        activateWindow();
        setFocus(Qt::ShortcutFocusReason);
        requestDeckFocus();
    }

    void pasteSelected(bool forcePlainText = false) {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) {
            return;
        }

        suppressClipboardCaptureUntil_ = QDateTime::currentMSecsSinceEpoch() + 800;
        if (items_[selectedIndex_].isImage()) {
            QApplication::clipboard()->setImage(items_[selectedIndex_].image, QClipboard::Clipboard);
        } else {
            QSettings settings;
            const bool preserveFormatting = !forcePlainText && settings.value(
                QStringLiteral("paste/preserveFormatting"), false).toBool();
            if (preserveFormatting && !items_[selectedIndex_].richTextHtml.isEmpty()) {
                auto *mimeData = new QMimeData;
                mimeData->setText(items_[selectedIndex_].text);
                mimeData->setHtml(items_[selectedIndex_].richTextHtml);
                QApplication::clipboard()->setMimeData(mimeData, QClipboard::Clipboard);
            } else {
                QApplication::clipboard()->setText(items_[selectedIndex_].text, QClipboard::Clipboard);
            }
        }
        hide();

        if (!display_ || pasteTarget_ == None) {
            return;
        }

        const Window target = pasteTarget_;
        QTimer::singleShot(30, this, [this, target] {
            activateTarget(target);
            waitForTargetAndPaste(target, 0);
        });
    }

    bool takeClipboardCaptureSuppression() {
        return QDateTime::currentMSecsSinceEpoch() <= suppressClipboardCaptureUntil_;
    }

    void refresh() { reload(); }
    void beginSearchForRenderCheck() { search(); }
    void setSettingsCallback(std::function<void(int)> callback) {
        settingsCallback_ = std::move(callback);
    }

protected:
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        enableKWinBlur();
    }

    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        QPainterPath deckPath;
        deckPath.addRoundedRect(rect().adjusted(1, 1, -1, -1), 24, 24);
        painter.fillPath(deckPath, QColor(15, 16, 17, 58));
        painter.setPen(QPen(QColor(255, 255, 255, 55), 1));
        painter.drawPath(deckPath);

        QLinearGradient sheen(0, 0, 0, height());
        sheen.setColorAt(0, QColor(255, 255, 255, 10));
        sheen.setColorAt(0.45, QColor(255, 255, 255, 0));
        sheen.setColorAt(1, QColor(0, 0, 0, 10));
        painter.fillPath(deckPath, sheen);

        if (resizing_) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(QColor(255, 255, 255, 82));
            painter.drawRoundedRect(QRect(width() / 2 - 22, 7, 44, 3), 2, 2);
        }

        paintHeader(painter);
        if (items_.empty()) {
            paintEmptyState(painter);
        } else {
            paintCards(painter);
        }
    }

    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        updateSearchGeometry();
        if (isVisible()) enableKWinBlur();
    }

    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == searchEdit_ && event->type() == QEvent::KeyPress) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (keyEvent->key() == Qt::Key_Escape) {
                endSearch();
                return true;
            }
            if (keyEvent->key() == Qt::Key_Backspace && searchEdit_->text().isEmpty()) {
                endSearch();
                return true;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

    void closeEvent(QCloseEvent *event) override {
        event->ignore();
        hide();
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) {
            if (searchActive_) endSearch();
            else hide();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            pasteSelected(event->modifiers() == configuredModifier(
                QStringLiteral("shortcuts/plainTextModifier"), Qt::ShiftModifier));
            return;
        }
        if (matchesShortcut(event, QStringLiteral("shortcuts/previousCategory"),
                            QKeySequence(Qt::CTRL | Qt::Key_Left))) {
            changeCategory(-1);
            return;
        }
        if (matchesShortcut(event, QStringLiteral("shortcuts/nextCategory"),
                            QKeySequence(Qt::CTRL | Qt::Key_Right))) {
            changeCategory(1);
            return;
        }
        if (event->key() == Qt::Key_Left) {
            moveSelection(-1);
            return;
        }
        if (event->key() == Qt::Key_Right) {
            moveSelection(1);
            return;
        }
        if (event->modifiers() == Qt::ControlModifier
            && (event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete)) {
            deleteSelected();
            return;
        }
        if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_Q) {
            QApplication::quit();
            return;
        }
        if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_F) {
            search();
            return;
        }
        if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_E) {
            editSelected();
            return;
        }
        if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_R) {
            renameSelected();
            return;
        }
        if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_O) {
            openSelected();
            return;
        }
        if (event->key() == Qt::Key_Space) {
            previewSelected();
            return;
        }
        const Qt::KeyboardModifiers quickPasteModifier = configuredModifier(
            QStringLiteral("shortcuts/quickPasteModifier"), Qt::ControlModifier);
        if (event->modifiers() == quickPasteModifier
            && event->key() >= Qt::Key_1 && event->key() <= Qt::Key_9) {
            const int index = event->key() - Qt::Key_1;
            if (index < static_cast<int>(items_.size())) {
                selectedIndex_ = index;
                quickPasteOverlay_ = false;
                pasteSelected();
            }
            return;
        }
        if (modifierForKey(event->key()) != Qt::NoModifier
            && modifierForKey(event->key()) == quickPasteModifier) {
            quickPasteOverlay_ = true;
            update();
            return;
        }
        if (event->modifiers().testFlag(Qt::ControlModifier) == false
            && event->modifiers().testFlag(Qt::AltModifier) == false
            && event->modifiers().testFlag(Qt::MetaModifier) == false
            && event->key() != Qt::Key_Space && !event->text().isEmpty()
            && event->text().at(0).isPrint()) {
            search();
            searchEdit_->insert(event->text());
            return;
        }
        QWidget::keyPressEvent(event);
    }

    void keyReleaseEvent(QKeyEvent *event) override {
        if (modifierForKey(event->key()) != Qt::NoModifier
            && modifierForKey(event->key()) == configuredModifier(
                QStringLiteral("shortcuts/quickPasteModifier"), Qt::ControlModifier)) {
            quickPasteOverlay_ = false;
            update();
            return;
        }
        QWidget::keyReleaseEvent(event);
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() != Qt::LeftButton) {
            return QWidget::mousePressEvent(event);
        }
        if (event->pos().y() <= 8) {
            resizing_ = true;
            resizeStartGlobalY_ = event->globalY();
            resizeStartGeometry_ = geometry();
            return;
        }
        if (event->pos().y() < 60) {
            const QStringList categories = categoryValues();
            const int first = categoryPageStart_;
            const int last = std::min(categories.size(), first + kCategoriesPerPage);
            for (int index = first; index < last; ++index) {
                if (categoryRect(index - first).contains(event->pos())) {
                    selectedCategory_ = categories[index];
                    reload();
                    return;
                }
            }
            if (searchRect().contains(event->pos())) {
                search();
                return;
            }
            if (previousCategoryRect().contains(event->pos())) { changeCategoryPage(-1); return; }
            if (nextCategoryRect().contains(event->pos())) { changeCategoryPage(1); return; }
            if ((addRect().contains(event->pos()) || moreRect().contains(event->pos())) && settingsCallback_) {
                settingsCallback_(3);
                return;
            }
        }
        const int index = cardIndexAt(event->pos());
        if (index >= 0) {
            selectedIndex_ = index;
            if (searchActive_) {
                searchEdit_->clearFocus();
                setFocus(Qt::MouseFocusReason);
            }
            centerSelection();
            update();
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (resizing_) {
            QDesktopWidget *desktop = QApplication::desktop();
            const QRect available = desktop->availableGeometry(desktop->screenNumber(frameGeometry().center()));
            const int bottom = resizeStartGeometry_.bottom() + 1;
            const int maximumHeight = std::max(250, std::min(420, static_cast<int>(available.height() * 0.42)));
            const int newHeight = std::max(250, std::min(maximumHeight,
                resizeStartGeometry_.height() + resizeStartGlobalY_ - event->globalY()));
            setGeometry(resizeStartGeometry_.x(), bottom - newHeight,
                        resizeStartGeometry_.width(), newHeight);
            clampScrollOffset();
            update();
            return;
        }
        setCursor(event->pos().y() <= 8 ? Qt::SizeVerCursor : Qt::ArrowCursor);
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (resizing_ && event->button() == Qt::LeftButton) {
            resizing_ = false;
            QSettings().setValue(QStringLiteral("deck/preferredHeight"), height());
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        const int index = cardIndexAt(event->pos());
        if (event->button() == Qt::LeftButton && index >= 0) {
            selectedIndex_ = index;
            pasteSelected(event->modifiers() == configuredModifier(
                QStringLiteral("shortcuts/plainTextModifier"), Qt::ShiftModifier));
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override {
        const int delta = event->angleDelta().x() != 0
            ? event->angleDelta().x()
            : event->angleDelta().y();
        scrollOffset_ -= delta / 2;
        clampScrollOffset();
        update();
        event->accept();
    }

    void contextMenuEvent(QContextMenuEvent *event) override {
        const int index = cardIndexAt(event->pos());
        if (index < 0) {
            return;
        }
        selectedIndex_ = index;
        update();
        QMenu menu(this);
        QAction *pasteAction = menu.addAction(QStringLiteral("粘贴\tEnter"));
        QAction *previewAction = menu.addAction(QStringLiteral("预览\tSpace"));
        QAction *openAction = nullptr;
        if (items_[index].isText()) {
            const QUrl url(items_[index].text.trimmed());
            if (url.isValid() && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"))) {
                openAction = menu.addAction(QStringLiteral("访问\tCtrl+O"));
            }
        }
        menu.addSeparator();
        QAction *editAction = menu.addAction(QStringLiteral("编辑\tCtrl+E"));
        QAction *renameAction = menu.addAction(QStringLiteral("重命名\tCtrl+R"));
        QAction *rotateLeftAction = nullptr;
        QAction *rotateRightAction = nullptr;
        if (items_[index].isImage()) {
            rotateLeftAction = menu.addAction(QStringLiteral("向左旋转\tCtrl+["));
            rotateRightAction = menu.addAction(QStringLiteral("向右旋转\tCtrl+]"));
        }
        menu.addSeparator();
        QAction *deleteAction = menu.addAction(QStringLiteral("删除\tCtrl+Delete"));
        const QAction *chosen = menu.exec(event->globalPos());
        if (chosen == pasteAction) pasteSelected();
        else if (chosen == previewAction) previewSelected();
        else if (chosen == openAction) QDesktopServices::openUrl(QUrl(items_[index].text.trimmed()));
        else if (chosen == editAction) editSelected();
        else if (chosen == renameAction) renameSelected();
        else if (chosen == rotateLeftAction) rotateSelected(-90);
        else if (chosen == rotateRightAction) rotateSelected(90);
        else if (chosen == deleteAction) deleteSelected();
    }

    void changeEvent(QEvent *event) override {
        QWidget::changeEvent(event);
        if (event->type() != QEvent::ActivationChange || isActiveWindow() || !isVisible()) {
            return;
        }
        QTimer::singleShot(0, this, [this] {
            if (isVisible() && !isActiveWindow()
                && QApplication::activeModalWidget() == nullptr
                && QApplication::activePopupWidget() == nullptr) {
                hide();
            }
        });
    }

private:
    QRect cardsViewport() const {
        return QRect(0, 66, width(), height() - 80);
    }

    int cardSide() const {
        return std::max(166, cardsViewport().height() - 24);
    }

    QRect cardRect(int index) const {
        const QRect viewport = cardsViewport();
        const int side = cardSide();
        return QRect(
            kContentInset - scrollOffset_ + index * (side + kCardSpacing),
            viewport.y() + (viewport.height() - side) / 2,
            side,
            side);
    }

    int cardIndexAt(const QPoint &point) const {
        for (int index = 0; index < static_cast<int>(items_.size()); ++index) {
            if (cardRect(index).contains(point)) {
                return index;
            }
        }
        return -1;
    }

    void paintHeader(QPainter &painter) {
        QFont font = this->font();
        font.setPointSize(11);
        font.setBold(true);
        painter.setFont(font);
        const QStringList categories = categoryValues();
        const QColor dots[] = {QColor(160, 170, 180), QColor(255, 56, 71), QColor(255, 145, 48),
                               QColor(175, 82, 222), QColor(50, 173, 230), QColor(52, 199, 89),
                               QColor(255, 55, 95), QColor(88, 86, 214), QColor(100, 210, 190)};
        const int first = categoryPageStart_;
        const int last = std::min(categories.size(), first + kCategoriesPerPage);
        for (int index = first; index < last; ++index) {
            const QRect itemRect = categoryRect(index - first);
            if (categories[index] == selectedCategory_) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(255, 255, 255, 38));
                painter.drawRoundedRect(itemRect, 16, 16);
            }
            if (index == 0) {
                const QPoint center(itemRect.x() + 14, itemRect.center().y());
                painter.setBrush(Qt::NoBrush);
                painter.setPen(QPen(QColor(220, 225, 230), 1.5, Qt::SolidLine,
                                    Qt::RoundCap, Qt::RoundJoin));
                painter.drawEllipse(center, 5, 5);
                painter.drawLine(center, center + QPoint(0, -3));
                painter.drawLine(center, center + QPoint(3, 1));
                painter.drawArc(QRect(center.x() - 8, center.y() - 8, 16, 16), 110 * 16, 75 * 16);
            } else {
                painter.setPen(Qt::NoPen);
                painter.setBrush(dots[index % 9]);
                painter.drawEllipse(QPoint(itemRect.x() + 14, itemRect.center().y()), 5, 5);
            }
            painter.setPen(QColor(255, 255, 255, 225));
            painter.drawText(itemRect.adjusted(25, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft,
                             categories[index]);
        }

        if (!searchActive_) {
            const QPoint center = searchRect().center() + QPoint(-1, -1);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(255, 255, 255, 210), 2, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            painter.drawEllipse(center, 6, 6);
            painter.drawLine(center + QPoint(4, 4), center + QPoint(9, 9));
        }
        painter.setFont(font);
        auto drawChevron = [&painter](const QRect &rect, bool pointsRight, int alpha) {
            const QPoint center = rect.center();
            QPainterPath path;
            path.moveTo(center + QPoint(pointsRight ? -2 : 2, -4));
            path.lineTo(center + QPoint(pointsRight ? 2 : -2, 0));
            path.lineTo(center + QPoint(pointsRight ? -2 : 2, 4));
            painter.setPen(QPen(QColor(255, 255, 255, alpha), 1.8, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            painter.drawPath(path);
        };
        drawChevron(previousCategoryRect(), false, categoryPageStart_ > 0 ? 210 : 55);
        drawChevron(nextCategoryRect(), true,
                    categoryPageStart_ + kCategoriesPerPage < categories.size() ? 210 : 55);
        painter.setPen(QPen(QColor(255, 255, 255, 220), 2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(addRect().center() + QPoint(-7, 0), addRect().center() + QPoint(7, 0));
        painter.drawLine(addRect().center() + QPoint(0, -7), addRect().center() + QPoint(0, 7));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 215));
        for (int offset : {-7, 0, 7}) painter.drawEllipse(moreRect().center() + QPoint(offset, 0), 2, 2);
    }

    void paintEmptyState(QPainter &painter) {
        QFont headline = font();
        headline.setPointSize(18);
        headline.setBold(true);
        painter.setFont(headline);
        painter.setPen(QColor(255, 255, 255, 220));
        painter.drawText(cardsViewport().adjusted(0, -18, 0, 0), Qt::AlignCenter,
                         QStringLiteral("复制一些内容开始使用"));

        QFont detail = font();
        detail.setPointSize(11);
        painter.setFont(detail);
        painter.setPen(QColor(255, 255, 255, 135));
        painter.drawText(cardsViewport().adjusted(0, 34, 0, 0), Qt::AlignCenter,
                         QStringLiteral("复制或剪切的文本和图片会出现在这里"));
    }

    void paintCards(QPainter &painter) {
        const QRect viewport = cardsViewport();
        painter.save();
        painter.setClipRect(viewport);
        QPainterPath deckClip;
        deckClip.addRoundedRect(rect().adjusted(1, 1, -1, -1), 24, 24);
        painter.setClipPath(deckClip, Qt::IntersectClip);

        for (int index = 0; index < static_cast<int>(items_.size()); ++index) {
            const QRect card = cardRect(index);
            if (!card.intersects(viewport)) continue;

            const ClipboardItem &item = items_[index];
            const bool selected = index == selectedIndex_;
            const bool expanded = card.height() >= 180;
            const int cornerRadius = expanded ? 18 : 15;
            const int headerHeight = std::min(58, std::max(48, qRound(card.height() * 0.20)));
            const QUrl webURL(item.isText() ? item.text.trimmed() : QString());
            const bool isLink = item.isText() && webURL.isValid()
                && (webURL.scheme() == QStringLiteral("http") || webURL.scheme() == QStringLiteral("https"))
                && !webURL.host().isEmpty() && !item.text.contains(QRegularExpression(QStringLiteral("\\s")));
            QColor parsedColor;
            if (item.isText() && item.previewLanguage == QStringLiteral("color")) {
                QString value = item.text.trimmed();
                if (!value.startsWith(QLatin1Char('#')) && (value.size() == 6 || value.size() == 8)) {
                    value.prepend(QLatin1Char('#'));
                }
                parsedColor = QColor(value);
            }
            const bool immersive = item.isImage() || parsedColor.isValid();

            QPainterPath path;
            path.addRoundedRect(card, cornerRadius, cornerRadius);
            painter.save();

            if (!resizing_) {
                for (int spread = 8; spread >= 2; spread -= 2) {
                    QPainterPath shadow;
                    shadow.addRoundedRect(card.adjusted(-spread / 3, 2, spread / 3, 6),
                                          cornerRadius + 2, cornerRadius + 2);
                    painter.fillPath(shadow, selected
                        ? QColor(10, 95, 180, 6 + spread * 2)
                        : QColor(0, 0, 0, 6 + spread * 2));
                }
            }
            painter.fillPath(path, QColor(19, 19, 21, 255));

            QRect contentArea = (expanded && !immersive)
                ? QRect(card.x(), card.y() + headerHeight, card.width(), card.height() - headerHeight)
                : card;
            painter.save();
            painter.setClipPath(path);
            if (item.isImage()) {
                const int checkerSize = 12;
                painter.fillRect(contentArea, QColor(26, 26, 28));
                for (int y = contentArea.y(); y < contentArea.bottom(); y += checkerSize) {
                    for (int x = contentArea.x(); x < contentArea.right(); x += checkerSize) {
                        painter.fillRect(QRect(x, y, checkerSize, checkerSize),
                            ((x / checkerSize + y / checkerSize) % 2 == 0)
                                ? QColor(255, 255, 255, 14) : QColor(0, 0, 0, 30));
                    }
                }
                const QSize fitted = item.image.size().scaled(
                    contentArea.adjusted(8, 8, -8, -8).size(), Qt::KeepAspectRatio);
                const QRect imageRect(contentArea.center().x() - fitted.width() / 2,
                                      contentArea.center().y() - fitted.height() / 2,
                                      fitted.width(), fitted.height());
                painter.drawImage(imageRect, item.image);
            } else if (parsedColor.isValid()) {
                painter.fillRect(contentArea, parsedColor);
            } else if (isLink) {
                const int footerHeight = expanded ? std::min(88, std::max(72, qRound(card.height() * 0.23))) : 0;
                const QRect previewArea = expanded
                    ? QRect(contentArea.x(), contentArea.y(), contentArea.width(), contentArea.height() - footerHeight)
                    : contentArea;
                QImage linkPreview;
                if (!item.linkPreviewData.isEmpty()) linkPreview.loadFromData(item.linkPreviewData);
                if (expanded && !linkPreview.isNull()) {
                    const QImage scaled = linkPreview.scaled(
                        previewArea.size(), Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
                    const QRect source((scaled.width() - previewArea.width()) / 2,
                                       (scaled.height() - previewArea.height()) / 2,
                                       previewArea.width(), previewArea.height());
                    painter.drawImage(previewArea, scaled, source);
                    painter.fillRect(previewArea, QColor(0, 0, 0, 18));
                }
                const int iconSize = expanded ? std::min(58, std::max(42, qRound(card.height() * 0.25))) : 36;
                const QRect iconRect(previewArea.center().x() - iconSize / 2,
                                     previewArea.center().y() - iconSize / 2 + (expanded ? 0 : 10),
                                     iconSize, iconSize);
                if (!expanded || linkPreview.isNull()) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(QColor(245, 245, 247, 245));
                    painter.drawRoundedRect(iconRect, qRound(iconSize * 0.23), qRound(iconSize * 0.23));
                    QImage linkIcon;
                    if (!item.linkIconData.isEmpty()) linkIcon.loadFromData(item.linkIconData);
                    if (!linkIcon.isNull()) {
                        painter.drawImage(iconRect.adjusted(5, 5, -5, -5), linkIcon);
                    } else {
                        QFont linkIconFont = font();
                        linkIconFont.setPointSizeF(iconSize * 0.46);
                        linkIconFont.setBold(true);
                        painter.setFont(linkIconFont);
                        painter.setPen(QColor(90, 95, 105, 180));
                        painter.drawText(iconRect, Qt::AlignCenter, QStringLiteral("↗"));
                    }
                }
                if (expanded) {
                    const QRect footer(contentArea.x(), contentArea.bottom() - footerHeight + 1,
                                       contentArea.width(), footerHeight);
                    painter.fillRect(footer, QColor(19, 19, 21, 255));
                    const QRect smallIcon(footer.x() + 12, footer.center().y() - 16, 32, 32);
                    painter.setBrush(QColor(245, 245, 247, 245));
                    painter.setPen(Qt::NoPen);
                    painter.drawRoundedRect(smallIcon, 7, 7);
                    QImage linkIcon;
                    if (!item.linkIconData.isEmpty()) linkIcon.loadFromData(item.linkIconData);
                    if (!linkIcon.isNull()) {
                        painter.drawImage(smallIcon.adjusted(4, 4, -4, -4), linkIcon);
                    } else {
                        QFont footerIconFont = font();
                        footerIconFont.setPointSize(13);
                        footerIconFont.setBold(true);
                        painter.setFont(footerIconFont);
                        painter.setPen(QColor(90, 95, 105, 180));
                        painter.drawText(smallIcon, Qt::AlignCenter, QStringLiteral("↗"));
                    }
                    QFont titleFont = font();
                    titleFont.setPointSizeF(11.5);
                    titleFont.setBold(true);
                    painter.setFont(titleFont);
                    painter.setPen(QColor(255, 255, 255, 232));
                    painter.drawText(QRect(smallIcon.right() + 10, footer.y() + 13,
                                           footer.width() - 66, 22),
                                     Qt::AlignLeft | Qt::AlignVCenter,
                                     item.title == QStringLiteral("文本") ? webURL.host() : item.title);
                    QFont hostFont = font();
                    hostFont.setPointSizeF(9.5);
                    painter.setFont(hostFont);
                    painter.setPen(QColor(255, 255, 255, 128));
                    QString host = webURL.host();
                    if (host.startsWith(QStringLiteral("www."))) host.remove(0, 4);
                    painter.drawText(QRect(smallIcon.right() + 10, footer.y() + 36,
                                           footer.width() - 66, 18),
                                     Qt::AlignLeft | Qt::AlignVCenter, host);
                }
            } else {
                const QRect textRect = expanded
                    ? contentArea.adjusted(14, 13, -14, -34)
                    : contentArea.adjusted(14, 37, -14, -12);
                paintHighlightedText(painter, textRect, item.text, item.previewLanguage, font());
            }
            painter.restore();

            const QRect headerRect(card.x(), card.y(), card.width(), headerHeight);
            if (expanded && !immersive) {
                QLinearGradient headerGradient(headerRect.topLeft(), headerRect.bottomRight());
                if (selected) {
                    headerGradient.setColorAt(0, QColor(3, 36, 89));
                    headerGradient.setColorAt(1, QColor(5, 66, 153));
                } else {
                    headerGradient.setColorAt(0, QColor(69, 158, 237));
                    headerGradient.setColorAt(1, QColor(33, 120, 224));
                }
                painter.save();
                painter.setClipPath(path);
                painter.fillRect(headerRect, headerGradient);
                painter.restore();
            }

            QFont metaFont = font();
            metaFont.setPointSizeF(expanded ? 11.5 : 9.5);
            metaFont.setBold(true);
            painter.setFont(metaFont);
            painter.setPen(Qt::white);
            const QString headerTitle = isLink ? QStringLiteral("链接") : item.title;
            QString categoryLabel = item.category;
            if (item.previewLanguage == QStringLiteral("code")) {
                const QString detected = HistoryRepository::detectedCodeLanguage(item.text);
                if (!detected.isEmpty()) categoryLabel = displayCodeLanguage(detected);
            }
            if (expanded) {
                painter.drawText(QRect(card.x() + 14, card.y() + 7, card.width() - 70, 23),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 categoryLabel.isEmpty() ? headerTitle
                                     : headerTitle + QStringLiteral("  ") + categoryLabel);
            } else {
                painter.drawText(QRect(card.x() + 11, card.y() + 7, card.width() - 48, 24),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 categoryLabel.isEmpty() ? headerTitle
                                     : headerTitle + QStringLiteral("  ") + categoryLabel
                                         + QStringLiteral("  ") + relativeTime(item.copiedAt));
            }

            QFont smallFont = font();
            smallFont.setPointSizeF(9.5);
            if (expanded) {
                painter.setFont(smallFont);
                painter.setPen(QColor(255, 255, 255, 198));
                painter.drawText(QRect(card.x() + 14, card.y() + 28, card.width() - 70, 18),
                                 Qt::AlignLeft | Qt::AlignVCenter, relativeTime(item.copiedAt));
            }
            const int sourceSize = expanded ? headerHeight - 8 : 18;
            const QRect sourceRect(card.right() - sourceSize - (expanded ? 8 : 11),
                                   card.y() + (expanded ? 4 : 9),
                                   sourceSize, sourceSize);
            if (!item.sourceIcon.isNull()) {
                painter.drawImage(sourceRect, item.sourceIcon);
            } else {
                painter.setPen(QColor(255, 255, 255, 225));
                painter.setBrush(QColor(255, 255, 255, 38));
                painter.drawRoundedRect(sourceRect, std::max(4, sourceSize / 5),
                                        std::max(4, sourceSize / 5));
                painter.drawText(sourceRect, Qt::AlignCenter,
                                 item.sourceName.left(1).toUpper());
            }

            painter.setFont(smallFont);
            painter.setPen(QColor(255, 255, 255, 115));
            if (expanded && !isLink) {
                const QString footer = item.isImage()
                    ? QStringLiteral("%1 × %2").arg(item.image.width()).arg(item.image.height())
                    : parsedColor.isValid() ? item.text.trimmed()
                    : QStringLiteral("%1 个字符").arg(item.text.size());
                if (item.isImage() || parsedColor.isValid()) {
                    QFont badgeFont = font();
                    badgeFont.setPointSizeF(item.isImage() ? 9.5 : 11);
                    badgeFont.setBold(true);
                    painter.setFont(badgeFont);
                    const int badgeWidth = std::min(card.width() - 24,
                        QFontMetrics(badgeFont).horizontalAdvance(footer) + 20);
                    const QRect badge(card.center().x() - badgeWidth / 2, card.bottom() - 40,
                                      badgeWidth, 28);
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(QColor(0, 0, 0, item.isImage() ? 148 : 122));
                    painter.drawRoundedRect(badge, 14, 14);
                    painter.setPen(QColor(255, 255, 255, 225));
                    painter.drawText(badge, Qt::AlignCenter, footer);
                } else {
                    painter.drawText(QRect(card.x() + 14, card.bottom() - 29,
                                           card.width() - 28, 20), Qt::AlignCenter, footer);
                }
            }
            if (quickPasteOverlay_ && index < 9) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0, 0, 0, selected ? 168 : 122));
                painter.drawRoundedRect(QRect(card.right() - 37, card.bottom() - 37, 27, 27), 7, 7);
                painter.setPen(QColor(255, 255, 255, 238));
                painter.drawText(QRect(card.right() - 37, card.bottom() - 37, 27, 27), Qt::AlignCenter,
                                 QString::number(index + 1));
            }

            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(selected ? QColor(10, 143, 255, 245) : QColor(255, 255, 255, 23),
                                selected ? 4.0 : 1.0));
            painter.drawPath(path);
            painter.restore();
        }
        painter.restore();
    }

    void moveSelection(int direction) {
        if (items_.empty()) {
            return;
        }
        selectedIndex_ = std::max(0, std::min(
            static_cast<int>(items_.size()) - 1,
            selectedIndex_ + direction));
        centerSelection();
        update();
    }

    void changeCategory(int direction) {
        const QStringList categories = categoryValues();
        const int current = std::max(0, categories.indexOf(selectedCategory_));
        const int next = (current + direction + categories.size()) % categories.size();
        selectedCategory_ = categories[next];
        if (next < categoryPageStart_) categoryPageStart_ = next;
        else if (next >= categoryPageStart_ + kCategoriesPerPage) {
            categoryPageStart_ = std::max(0, next - (kCategoriesPerPage - 1));
        }
        reload();
    }

    void changeCategoryPage(int direction) {
        const int maximum = std::max(0, categoryValues().size() - kCategoriesPerPage);
        categoryPageStart_ = direction < 0
            ? std::max(0, categoryPageStart_ - 5)
            : std::min(maximum, categoryPageStart_ + 5);
        update();
    }

    void deleteSelected() {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) {
            return;
        }
        repository_->remove(items_[selectedIndex_].databaseID);
        items_.erase(items_.begin() + selectedIndex_);
        if (items_.empty()) {
            selectedIndex_ = -1;
        } else {
            selectedIndex_ = std::min(selectedIndex_, static_cast<int>(items_.size()) - 1);
        }
        clampScrollOffset();
        update();
    }

    QStringList categoryValues() const {
        QStringList values{QStringLiteral("History"), QStringLiteral("Text"), QStringLiteral("Images")};
        for (const ClipboardCategory &category : repository_->categories()) values.append(category.name);
        return values;
    }

    QRect categoryRect(int index) const {
        return QRect(categoryStripX() + index * 104, 12, 96, 35);
    }

    QRect searchRect() const {
        return QRect(toolbarStartX(), 12, searchActive_ ? 230 : 36, 35);
    }

    int toolbarStartX() const {
        const int searchWidth = searchActive_ ? 230 : 36;
        const int toolbarWidth = searchWidth + 20 + kCategoriesPerPage * 104 + 3 * 38;
        return std::max(62, (width() - toolbarWidth) / 2);
    }
    int categoryStripX() const { return toolbarStartX() + (searchActive_ ? 250 : 56); }
    QRect previousCategoryRect() const { return QRect(categoryStripX() + 624, 12, 34, 35); }
    QRect nextCategoryRect() const { return QRect(categoryStripX() + 662, 12, 34, 35); }
    QRect addRect() const { return QRect(categoryStripX() + 700, 12, 34, 35); }
    QRect moreRect() const { return QRect(width() - 68, 12, 42, 35); }

    void search() {
        const bool starting = !searchActive_;
        if (starting) {
            searchActive_ = true;
            selectedCategory_ = QStringLiteral("History");
            reload();
        }
        updateSearchGeometry();
        searchEdit_->setText(searchQuery_);
        searchEdit_->show();
        searchEdit_->raise();
        searchEdit_->setFocus(Qt::ShortcutFocusReason);
        searchEdit_->setCursorPosition(searchEdit_->text().size());
    }

    void endSearch() {
        searchActive_ = false;
        searchQuery_.clear();
        searchEdit_->clear();
        searchEdit_->hide();
        selectedCategory_ = QStringLiteral("History");
        setFocus(Qt::ShortcutFocusReason);
        reload();
    }

    void updateSearchGeometry() {
        if (searchActive_) searchEdit_->setGeometry(searchRect().adjusted(0, -2, 0, 1));
    }

    void preparePopover(QDialog &dialog, const QSize &size) const {
        dialog.setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
        dialog.setModal(true);
        dialog.resize(size);
        const QRect card = selectedIndex_ >= 0 ? cardRect(selectedIndex_) : rect();
        const QPoint cardTopLeft = mapToGlobal(card.topLeft());
        const QRect globalCard(cardTopLeft, card.size());
        const QRect available = QApplication::desktop()->availableGeometry(globalCard.center());
        int x = globalCard.center().x() - size.width() / 2;
        int y = globalCard.top() - size.height() - 12;
        if (y < available.top() + 8) y = globalCard.bottom() + 12;
        x = std::max(available.left() + 8, std::min(x, available.right() - size.width() - 7));
        y = std::max(available.top() + 8, std::min(y, available.bottom() - size.height() - 7));
        dialog.move(x, y);
        dialog.setStyleSheet(QStringLiteral(
            "QDialog{background:#242427;color:#eee;border:1px solid #49494d;border-radius:14px;}"
            "QPlainTextEdit{background:#111113;color:#eee;border:1px solid #38383c;border-radius:10px;padding:10px;}"
            "QLineEdit{background:#333337;color:#eee;border:0;border-radius:9px;padding:8px 12px;}"
            "QPushButton{background:#3a3a3e;color:#eee;border:0;border-radius:17px;padding:8px 18px;}"
            "QPushButton#primary{background:#1683ff;color:white;font-weight:600;}"));
    }

    void previewSelected() {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) return;
        const ClipboardItem item = items_[selectedIndex_];
        QDialog dialog(this);
        preparePopover(dialog, QSize(560, 360));
        QVBoxLayout *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        QWidget *header = new QWidget(&dialog);
        QHBoxLayout *headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(22, 10, 22, 10);
        QLabel *category = new QLabel(item.category.isEmpty()
            ? (item.isImage() ? QStringLiteral("图片") : QStringLiteral("文本")) : item.category, header);
        QFont categoryFont = category->font(); categoryFont.setPointSize(14); categoryFont.setBold(true);
        category->setFont(categoryFont);
        QLabel *source = new QLabel(item.sourceName, header);
        source->setStyleSheet(QStringLiteral("color:#9b9ba0;"));
        QLabel *time = new QLabel(item.copiedAt.toString(QStringLiteral("yyyy-MM-dd  HH:mm")), header);
        time->setStyleSheet(QStringLiteral("color:#8d8d92;"));
        headerLayout->addWidget(category);
        headerLayout->addWidget(source);
        headerLayout->addStretch();
        headerLayout->addWidget(time);
        layout->addWidget(header);
        if (item.isImage()) {
            QLabel *label = new QLabel(&dialog);
            label->setAlignment(Qt::AlignCenter);
            label->setStyleSheet(QStringLiteral("background:#151517;border-top:1px solid #3a3a3d;"));
            label->setPixmap(QPixmap::fromImage(item.image)
                                 .scaled(510, 260, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            layout->addWidget(label, 1);
        } else {
            QPlainTextEdit *text = new QPlainTextEdit(item.text.left(200000), &dialog);
            text->setReadOnly(true);
            text->setLineWrapMode(QPlainTextEdit::NoWrap);
            QFont textFont = text->font();
            if (!item.previewLanguage.isEmpty() && item.previewLanguage != QStringLiteral("text")) {
                textFont.setFamily(QStringLiteral("monospace"));
                textFont.setStyleHint(QFont::Monospace);
            }
            textFont.setPointSize(12); text->setFont(textFont);
            new CardSyntaxHighlighter(text->document(), item.previewLanguage);
            layout->addWidget(text, 1);
        }
        dialog.exec();
    }

    void editSelected() {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) return;
        const ClipboardItem item = items_[selectedIndex_];
        QDialog dialog(this);
        preparePopover(dialog, QSize(560, 430));
        QVBoxLayout *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(12, 10, 12, 10);
        layout->setSpacing(10);
        QHBoxLayout *toolbar = new QHBoxLayout;
        QPushButton *cancel = new QPushButton(QStringLiteral("取消"), &dialog);
        QPushButton *save = new QPushButton(QStringLiteral("保存"), &dialog);
        save->setObjectName(QStringLiteral("primary"));
        toolbar->addWidget(cancel);
        toolbar->addStretch();
        QLabel *heading = new QLabel(item.isImage() ? QStringLiteral("编辑图片") : QStringLiteral("编辑文本"), &dialog);
        QFont headingFont = heading->font(); headingFont.setBold(true); heading->setFont(headingFont);
        toolbar->insertWidget(1, heading, 0, Qt::AlignCenter);
        toolbar->addWidget(save);
        layout->addLayout(toolbar);

        QImage editedImage = item.image;
        QPlainTextEdit *editor = nullptr;
        QLabel *imageLabel = nullptr;
        QLabel *statistics = new QLabel(&dialog);
        statistics->setStyleSheet(QStringLiteral("color:#8f8f94;font-size:12px;"));
        if (item.isImage()) {
            QHBoxLayout *imageTools = new QHBoxLayout;
            QPushButton *rotateLeft = new QPushButton(QStringLiteral("↶  向左旋转"), &dialog);
            QPushButton *rotateRight = new QPushButton(QStringLiteral("↷  向右旋转"), &dialog);
            imageTools->addStretch(); imageTools->addWidget(rotateLeft); imageTools->addWidget(rotateRight); imageTools->addStretch();
            layout->addLayout(imageTools);
            imageLabel = new QLabel(&dialog);
            imageLabel->setAlignment(Qt::AlignCenter);
            imageLabel->setStyleSheet(QStringLiteral("background:#151517;border:1px solid #38383c;border-radius:10px;"));
            auto updateImagePreview = [&] {
                imageLabel->setPixmap(QPixmap::fromImage(editedImage)
                    .scaled(500, 280, Qt::KeepAspectRatio, Qt::SmoothTransformation));
                statistics->setText(QStringLiteral("%1 × %2").arg(editedImage.width()).arg(editedImage.height()));
            };
            QObject::connect(rotateLeft, &QPushButton::clicked, [&] {
                QTransform transform; transform.rotate(-90);
                editedImage = editedImage.transformed(transform, Qt::SmoothTransformation);
                updateImagePreview();
            });
            QObject::connect(rotateRight, &QPushButton::clicked, [&] {
                QTransform transform; transform.rotate(90);
                editedImage = editedImage.transformed(transform, Qt::SmoothTransformation);
                updateImagePreview();
            });
            updateImagePreview();
            layout->addWidget(imageLabel, 1);
        } else {
            editor = new QPlainTextEdit(item.text, &dialog);
            QFont editorFont = editor->font();
            if (!item.previewLanguage.isEmpty() && item.previewLanguage != QStringLiteral("text")) {
                editorFont.setFamily(QStringLiteral("monospace"));
                editorFont.setStyleHint(QFont::Monospace);
            }
            editorFont.setPointSize(12); editor->setFont(editorFont);
            new CardSyntaxHighlighter(editor->document(), item.previewLanguage);
            layout->addWidget(editor, 1);
            auto updateStatistics = [editor, statistics] {
                const QString text = editor->toPlainText();
                statistics->setText(QStringLiteral("%1 个字符  ·  %2 个单词  ·  %3 行")
                    .arg(text.size())
                    .arg(text.split(QRegularExpression(QStringLiteral("\\s+")), QString::SkipEmptyParts).size())
                    .arg(std::max(1, text.count(QLatin1Char('\n')) + 1)));
            };
            QObject::connect(editor, &QPlainTextEdit::textChanged, updateStatistics);
            updateStatistics();
            editor->setFocus();
        }
        layout->addWidget(statistics);
        QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
        QObject::connect(save, &QPushButton::clicked, &dialog, &QDialog::accept);
        if (dialog.exec() != QDialog::Accepted) return;
        if (item.isImage()) {
            QByteArray pngData;
            QBuffer buffer(&pngData); buffer.open(QIODevice::WriteOnly); editedImage.save(&buffer, "PNG");
            repository_->updateImage(item.databaseID, pngData, editedImage);
        } else if (editor) {
            const QString value = editor->toPlainText();
            repository_->updateText(item.databaseID, value, repository_->classifyText(value));
        }
        reload();
    }

    void renameSelected() {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) return;
        const ClipboardItem item = items_[selectedIndex_];
        QDialog dialog(this);
        preparePopover(dialog, QSize(380, 160));
        QVBoxLayout *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(18, 16, 18, 16);
        QLabel *heading = new QLabel(QStringLiteral("重命名"), &dialog);
        QFont headingFont = heading->font(); headingFont.setPointSize(13); headingFont.setBold(true); heading->setFont(headingFont);
        QLineEdit *title = new QLineEdit(item.title, &dialog);
        QHBoxLayout *actions = new QHBoxLayout;
        QLabel *hint = new QLabel(QStringLiteral("空标题将恢复为默认名称"), &dialog);
        hint->setStyleSheet(QStringLiteral("color:#8f8f94;font-size:11px;"));
        QPushButton *cancel = new QPushButton(QStringLiteral("取消"), &dialog);
        QPushButton *save = new QPushButton(QStringLiteral("保存"), &dialog);
        save->setObjectName(QStringLiteral("primary"));
        actions->addWidget(hint); actions->addStretch(); actions->addWidget(cancel); actions->addWidget(save);
        layout->addWidget(heading); layout->addWidget(title); layout->addLayout(actions);
        QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
        QObject::connect(save, &QPushButton::clicked, &dialog, &QDialog::accept);
        QObject::connect(title, &QLineEdit::returnPressed, &dialog, &QDialog::accept);
        title->setFocus(); title->selectAll();
        if (dialog.exec() == QDialog::Accepted) {
            repository_->updateTitle(item.databaseID, title->text());
            reload();
        }
    }

    void rotateSelected(int degrees) {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())
            || !items_[selectedIndex_].isImage()) return;
        QTransform transform;
        transform.rotate(degrees);
        const QImage image = items_[selectedIndex_].image.transformed(transform, Qt::SmoothTransformation);
        QByteArray pngData;
        QBuffer buffer(&pngData);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        repository_->updateImage(items_[selectedIndex_].databaseID, pngData, image);
        reload();
    }

    static QString htmlAttribute(const QString &tag, const QString &name) {
        const QRegularExpression expression(
            QStringLiteral("\\b%1\\s*=\\s*(['\"])(.*?)\\1")
                .arg(QRegularExpression::escape(name)),
            QRegularExpression::CaseInsensitiveOption
                | QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch match = expression.match(tag);
        return match.hasMatch()
            ? QTextDocumentFragment::fromHtml(match.captured(2)).toPlainText().trimmed()
            : QString();
    }

    static QUrl metadataURL(const QString &html, const QUrl &baseURL,
                            const QStringList &metadataNames) {
        static const QRegularExpression tagExpression(
            QStringLiteral("<(meta|link)\\b[^>]*>"),
            QRegularExpression::CaseInsensitiveOption
                | QRegularExpression::DotMatchesEverythingOption);
        QRegularExpressionMatchIterator iterator = tagExpression.globalMatch(html);
        while (iterator.hasNext()) {
            const QString tag = iterator.next().captured(0);
            const QString key = htmlAttribute(tag, QStringLiteral("property")).isEmpty()
                ? htmlAttribute(tag, QStringLiteral("name"))
                : htmlAttribute(tag, QStringLiteral("property"));
            const QString relation = htmlAttribute(tag, QStringLiteral("rel"));
            bool matches = false;
            for (const QString &metadataName : metadataNames) {
                if (key.compare(metadataName, Qt::CaseInsensitive) == 0
                    || relation.contains(metadataName, Qt::CaseInsensitive)) {
                    matches = true;
                    break;
                }
            }
            if (!matches) continue;
            QString value = htmlAttribute(tag, QStringLiteral("content"));
            if (value.isEmpty()) value = htmlAttribute(tag, QStringLiteral("href"));
            const QUrl resolved = baseURL.resolved(QUrl(value));
            if (resolved.isValid() && !resolved.isEmpty()) return resolved;
        }
        return QUrl();
    }

    static QString pageTitle(const QString &html, const QUrl &url) {
        static const QRegularExpression expression(
            QStringLiteral("<title\\b[^>]*>(.*?)</title>"),
            QRegularExpression::CaseInsensitiveOption
                | QRegularExpression::DotMatchesEverythingOption);
        const QRegularExpressionMatch match = expression.match(html);
        const QString title = match.hasMatch()
            ? QTextDocumentFragment::fromHtml(match.captured(1)).toPlainText().simplified()
            : QString();
        return title.isEmpty() ? url.host() : title.left(300);
    }

    void downloadLinkImage(const QUrl &url, int maximumBytes,
                           std::function<void(QByteArray)> completion) {
        if (!url.isValid() || (url.scheme() != QStringLiteral("http")
                              && url.scheme() != QStringLiteral("https"))) {
            completion(QByteArray());
            return;
        }
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
        request.setRawHeader("User-Agent", "ClipDeck/0.3 (UOS; Linux)");
        QNetworkReply *reply = network_.get(request);
        QObject::connect(reply, &QNetworkReply::finished, this,
                         [reply, maximumBytes, completion = std::move(completion)]() mutable {
            QByteArray data;
            if (reply->error() == QNetworkReply::NoError) data = reply->readAll();
            reply->deleteLater();
            QImage image;
            if (data.size() > maximumBytes || !image.loadFromData(data)) data.clear();
            completion(std::move(data));
        });
    }

    void fetchLinkMetadataIfNeeded(const ClipboardItem &item) {
        if (!item.isText() || item.databaseID < 0
            || (item.hasFetchedLinkPreview && !item.linkIconData.isEmpty()
                && item.title != QStringLiteral("文本"))) return;
        const QUrl url(item.text.trimmed());
        if (!url.isValid() || (url.scheme() != QStringLiteral("http")
                              && url.scheme() != QStringLiteral("https"))
            || url.host().isEmpty()
            || item.text.contains(QRegularExpression(QStringLiteral("\\s")))) return;

        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
        request.setRawHeader("User-Agent", "ClipDeck/0.3 (UOS; Linux)");
        QNetworkReply *reply = network_.get(request);
        const qint64 databaseID = item.databaseID;
        QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, databaseID, url] {
            QByteArray pageData;
            if (reply->error() == QNetworkReply::NoError) pageData = reply->readAll();
            const QUrl finalURL = reply->url().isValid() ? reply->url() : url;
            reply->deleteLater();
            if (pageData.size() > 2 * 1024 * 1024) pageData.clear();
            if (pageData.isEmpty()) {
                repository_->updateLinkMetadata(databaseID, finalURL.host(), QByteArray(), QByteArray());
                reload();
                return;
            }

            const QString html = QString::fromUtf8(pageData);
            struct MetadataState {
                qint64 databaseID = -1;
                QString title;
                QByteArray icon;
                QByteArray preview;
                int pending = 2;
            };
            auto state = std::make_shared<MetadataState>();
            state->databaseID = databaseID;
            state->title = pageTitle(html, finalURL);
            const QUrl iconURL = [&] {
                const QUrl declared = metadataURL(html, finalURL,
                    {QStringLiteral("icon"), QStringLiteral("shortcut icon"), QStringLiteral("apple-touch-icon")});
                return declared.isValid() ? declared : finalURL.resolved(QUrl(QStringLiteral("/favicon.ico")));
            }();
            const QUrl previewURL = metadataURL(html, finalURL,
                {QStringLiteral("og:image"), QStringLiteral("twitter:image")});
            auto finish = [this, state] {
                if (--state->pending != 0) return;
                repository_->updateLinkMetadata(state->databaseID, state->title,
                                                state->icon, state->preview);
                reload();
            };
            downloadLinkImage(iconURL, 2 * 1024 * 1024,
                              [state, finish](QByteArray data) mutable {
                                  state->icon = std::move(data);
                                  finish();
                              });
            downloadLinkImage(previewURL, 8 * 1024 * 1024,
                              [state, finish](QByteArray data) mutable {
                                  state->preview = std::move(data);
                                  finish();
                              });
        });
    }

    void ensureSelectionVisible() {
        if (selectedIndex_ < 0) {
            return;
        }
        const QRect viewport = cardsViewport();
        const QRect card = cardRect(selectedIndex_);
        if (card.left() < viewport.left()) {
            scrollOffset_ -= viewport.left() - card.left();
        } else if (card.right() > viewport.right()) {
            scrollOffset_ += card.right() - viewport.right();
        }
        clampScrollOffset();
    }

    void centerSelection() {
        if (selectedIndex_ < 0) return;
        const int desired = kContentInset + selectedIndex_ * (cardSide() + kCardSpacing)
            + cardSide() / 2 - cardsViewport().width() / 2;
        scrollOffset_ = desired;
        clampScrollOffset();
    }

    void openSelected() {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) return;
        const QUrl url(items_[selectedIndex_].text.trimmed());
        if (url.isValid() && (url.scheme() == QStringLiteral("http")
                             || url.scheme() == QStringLiteral("https"))) {
            QDesktopServices::openUrl(url);
        }
    }

    void clampScrollOffset() {
        const int contentWidth = kContentInset * 2
            + static_cast<int>(items_.size()) * (cardSide() + kCardSpacing) - kCardSpacing;
        const int maximum = std::max(0, contentWidth - cardsViewport().width());
        scrollOffset_ = std::max(0, std::min(scrollOffset_, maximum));
    }

    void rememberPasteTarget() {
        if (!display_) {
            pasteTarget_ = None;
            return;
        }
        Window focused = None;
        int revertTo = RevertToNone;
        XGetInputFocus(display_, &focused, &revertTo);
        const Window target = topLevelWindow(display_, focused);
        if (target != None && target != static_cast<Window>(winId())) {
            pasteTarget_ = target;
        }
    }

    void activateTarget(Window target) {
        if (!display_ || target == None) {
            return;
        }
        const Atom activeWindow = XInternAtom(display_, "_NET_ACTIVE_WINDOW", False);
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.window = target;
        event.xclient.message_type = activeWindow;
        event.xclient.format = 32;
        event.xclient.data.l[0] = 2; // EWMH: request originated from a pager-like utility.
        event.xclient.data.l[1] = CurrentTime;
        XSendEvent(display_, DefaultRootWindow(display_), False,
                   SubstructureRedirectMask | SubstructureNotifyMask, &event);
        XFlush(display_);
    }

    void enableKWinBlur() {
        QPainterPath roundedPath;
        roundedPath.addRoundedRect(QRectF(rect().adjusted(1, 1, -1, -1)), 24, 24);
        const QRegion roundedRegion(roundedPath.toFillPolygon().toPolygon());
        setMask(roundedRegion);
        if (!display_) return;
        const Atom blur = XInternAtom(display_, "_KDE_NET_WM_BLUR_BEHIND_REGION", False);
        QVector<unsigned long> values;
        for (const QRect &rectangle : roundedRegion.rects()) {
            values.append(static_cast<unsigned long>(rectangle.x()));
            values.append(static_cast<unsigned long>(rectangle.y()));
            values.append(static_cast<unsigned long>(rectangle.width()));
            values.append(static_cast<unsigned long>(rectangle.height()));
        }
        XChangeProperty(display_, static_cast<Window>(winId()), blur, XA_CARDINAL, 32,
                        PropModeReplace,
                        reinterpret_cast<const unsigned char *>(values.constData()), values.size());
        XFlush(display_);
    }

    void requestDeckFocus() {
        if (!display_) return;
        const Window deckWindow = static_cast<Window>(winId());
        activateTarget(deckWindow);
        QTimer::singleShot(45, this, [this, deckWindow] {
            if (!isVisible()) return;
            XRaiseWindow(display_, deckWindow);
            XSetInputFocus(display_, deckWindow, RevertToPointerRoot, CurrentTime);
            XFlush(display_);
            activateWindow();
            setFocus(Qt::ActiveWindowFocusReason);
        });
    }

    void sendControlV() {
        if (!display_) {
            return;
        }
        const KeyCode control = XKeysymToKeycode(display_, XK_Control_L);
        const KeyCode v = XKeysymToKeycode(display_, XK_v);
        if (control == 0 || v == 0) {
            return;
        }
        XTestFakeKeyEvent(display_, control, True, CurrentTime);
        XTestFakeKeyEvent(display_, v, True, CurrentTime);
        XTestFakeKeyEvent(display_, v, False, CurrentTime);
        XTestFakeKeyEvent(display_, control, False, CurrentTime);
        XFlush(display_);
    }

    void waitForTargetAndPaste(Window target, int attempt) {
        if (!display_ || target == None) return;
        if (activeTopLevelWindow(display_) == target) {
            sendControlV();
            return;
        }
        if (attempt >= 30) {
            return; // Clipboard is already populated; the user can paste manually.
        }
        QTimer::singleShot(25, this, [this, target, attempt] {
            waitForTargetAndPaste(target, attempt + 1);
        });
    }

    void reload() {
        const qint64 selectedDatabaseID = selectedIndex_ >= 0
            && selectedIndex_ < static_cast<int>(items_.size())
            ? items_[selectedIndex_].databaseID : -1;
        QString repositoryCategory;
        if (selectedCategory_ == QStringLiteral("Text")) repositoryCategory = QStringLiteral("文本");
        else if (selectedCategory_ == QStringLiteral("Images")) repositoryCategory = QStringLiteral("图片");
        else if (selectedCategory_ != QStringLiteral("History")) repositoryCategory = selectedCategory_;
        items_ = repository_->fetch(repositoryCategory, searchQuery_, kHistoryCapacity).toStdVector();
        selectedIndex_ = -1;
        if (selectedDatabaseID >= 0) {
            for (int index = 0; index < static_cast<int>(items_.size()); ++index) {
                if (items_[index].databaseID == selectedDatabaseID) {
                    selectedIndex_ = index;
                    break;
                }
            }
        }
        if (selectedIndex_ < 0 && !items_.empty()) selectedIndex_ = 0;
        centerSelection();
        update();
    }

    Display *display_ = nullptr;
    HistoryRepository *repository_ = nullptr;
    QNetworkAccessManager network_;
    Window pasteTarget_ = None;
    std::vector<ClipboardItem> items_;
    int selectedIndex_ = -1;
    int scrollOffset_ = 0;
    qint64 suppressClipboardCaptureUntil_ = 0;
    bool resizing_ = false;
    int resizeStartGlobalY_ = 0;
    QRect resizeStartGeometry_;
    int categoryPageStart_ = 0;
    static constexpr int kCategoriesPerPage = 6;
    QLineEdit *searchEdit_ = nullptr;
    std::function<void(int)> settingsCallback_;
    QString selectedCategory_ = QStringLiteral("History");
    QString searchQuery_;
    bool searchActive_ = false;
    bool quickPasteOverlay_ = false;
};

void showSettings(HistoryRepository *repository, DeckWindow *deck, int initialPage = 0,
                  std::function<void()> shortcutChanged = {},
                  const QString &renderPath = QString()) {
    QSettings settings;
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("ClipDeck 设置"));
    dialog.resize(980, 680);
    dialog.setMinimumSize(920, 640);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog{background:#1d1d1f;color:#eeeeef;}"
        "QListWidget#sidebar{background:#28282a;border:0;padding:18px 10px;color:#aaa;font-size:17px;font-weight:600;}"
        "QListWidget#sidebar::item{height:56px;padding-left:18px;border-radius:12px;}"
        "QListWidget#sidebar::item:selected{background:#4a4a4d;color:white;}"
        "QListWidget#categoryList,QPlainTextEdit,QLineEdit{background:#151517;color:#eee;border:1px solid #343438;border-radius:9px;padding:8px;}"
        "QListWidget#categoryList::item{height:34px;padding-left:8px;}"
        "QListWidget#categoryList::item:selected{background:#176fd1;color:white;}"
        "QMenu{background:#29292c;color:#eee;border:1px solid #444;padding:5px;}"
        "QMenu::item:selected{background:#176fd1;color:white;}"
        "QWidget#page{background:#1d1d1f;}"
        "QLabel{color:#dedee1;font-size:14px;} QLabel#title{font-size:30px;font-weight:700;}"
        "QLabel#subtitle{color:#8f8f94;font-size:16px;}"
        "QGroupBox{background:#202022;border:1px solid #2b2b2e;border-radius:14px;margin-top:14px;padding:22px 18px 16px;}"
        "QGroupBox::title{color:#e5e5e7;font-size:18px;font-weight:700;subcontrol-origin:margin;left:4px;}"
        "QComboBox,QSpinBox{background:#303033;color:#eee;border:0;border-radius:9px;padding:8px 12px;min-width:130px;}"
        "QPushButton{background:#343437;color:#eee;border:0;border-radius:9px;padding:9px 16px;}"
        "QPushButton:hover{background:#444448;} QCheckBox{spacing:12px;font-size:15px;}"
        "QSlider::groove:horizontal{height:5px;background:#363639;border-radius:2px;}"
        "QSlider::sub-page:horizontal{background:#1683ff;border-radius:2px;}"
        "QSlider::handle:horizontal{background:#eee;width:18px;margin:-7px 0;border-radius:9px;}"));

    QHBoxLayout *root = new QHBoxLayout(&dialog);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    QListWidget *sidebar = new QListWidget(&dialog);
    sidebar->setObjectName(QStringLiteral("sidebar"));
    sidebar->setFixedWidth(190);
    sidebar->addItems({QStringLiteral("⚙  通用"), QStringLiteral("▣  图片"),
                       QStringLiteral("⌨  键盘快捷键"), QStringLiteral("▦  分类"),
                       QStringLiteral("▱  数据")});
    QStackedWidget *pages = new QStackedWidget(&dialog);
    root->addWidget(sidebar);
    root->addWidget(pages, 1);

    auto makePage = [&](const QString &title, const QString &subtitle) {
        QWidget *page = new QWidget(pages);
        page->setObjectName(QStringLiteral("page"));
        QVBoxLayout *layout = new QVBoxLayout(page);
        layout->setContentsMargins(38, 26, 38, 26);
        layout->setSpacing(20);
        QLabel *heading = new QLabel(title, page);
        heading->setObjectName(QStringLiteral("title"));
        QLabel *detail = new QLabel(subtitle, page);
        detail->setObjectName(QStringLiteral("subtitle"));
        layout->addWidget(heading);
        layout->addWidget(detail);
        return qMakePair(page, layout);
    };

    auto generalPage = makePage(QStringLiteral("通用"), QStringLiteral("设置 ClipDeck 的默认行为"));
    QGroupBox *pasteBox = new QGroupBox(QStringLiteral("粘贴"), generalPage.first);
    QFormLayout *pasteForm = new QFormLayout(pasteBox);
    QComboBox *pasteFormat = new QComboBox(pasteBox);
    pasteFormat->addItems({QStringLiteral("纯文本"), QStringLiteral("保留格式")});
    pasteFormat->setCurrentIndex(settings.value(QStringLiteral("paste/preserveFormatting"), false).toBool() ? 1 : 0);
    pasteForm->addRow(QStringLiteral("默认粘贴格式"), pasteFormat);
    pasteForm->addRow(new QLabel(QStringLiteral("纯文本会移除字体、颜色和段落样式。"), pasteBox));
    generalPage.second->addWidget(pasteBox);
    QGroupBox *startupBox = new QGroupBox(QStringLiteral("启动"), generalPage.first);
    QVBoxLayout *startupLayout = new QVBoxLayout(startupBox);
    QCheckBox *launchAtLogin = new QCheckBox(QStringLiteral("登录 UOS 时自动启动"), startupBox);
    launchAtLogin->setChecked(QFile::exists(QDir::home().filePath(QStringLiteral(".config/autostart/clipdeck.desktop"))));
    startupLayout->addWidget(launchAtLogin);
    generalPage.second->addWidget(startupBox);
    QGroupBox *historyBox = new QGroupBox(QStringLiteral("保留历史"), generalPage.first);
    QVBoxLayout *historyLayout = new QVBoxLayout(historyBox);
    QLabel *retentionValue = new QLabel(historyBox);
    retentionValue->setAlignment(Qt::AlignCenter);
    QSlider *retention = new QSlider(Qt::Horizontal, historyBox);
    const QVector<int> retentionOptions = {
        1, 2, 3, 4, 5, 6, 7, 14, 21, 30, 60, 90, 120, 150, 180,
        210, 240, 270, 300, 330, 365, 0
    };
    retention->setRange(0, retentionOptions.size() - 1);
    const int savedRetention = settings.value(QStringLiteral("history/retentionDays"), 30).toInt();
    int retentionIndex = retentionOptions.indexOf(savedRetention);
    if (retentionIndex < 0) {
        retentionIndex = 0;
        int closest = std::numeric_limits<int>::max();
        for (int index = 0; index < retentionOptions.size() - 1; ++index) {
            const int distance = std::abs(retentionOptions[index] - savedRetention);
            if (distance < closest) { closest = distance; retentionIndex = index; }
        }
    }
    retention->setValue(retentionIndex);
    auto selectedRetentionDays = [retention, &retentionOptions] {
        return retentionOptions[std::max(0, std::min(retention->value(), retentionOptions.size() - 1))];
    };
    auto updateRetention = [retentionValue, &retentionOptions](int index) {
        const int days = retentionOptions[index];
        QString label;
        if (days == 0) label = QStringLiteral("永久");
        else if (days == 7) label = QStringLiteral("1 周");
        else if (days == 30) label = QStringLiteral("1 个月");
        else if (days == 365) label = QStringLiteral("1 年");
        else label = QStringLiteral("%1 天").arg(days);
        retentionValue->setText(label);
    };
    updateRetention(retention->value());
    QObject::connect(retention, &QSlider::valueChanged, updateRetention);
    historyLayout->addWidget(retentionValue);
    historyLayout->addWidget(retention);
    generalPage.second->addWidget(historyBox);
    generalPage.second->addStretch();
    pages->addWidget(generalPage.first);

    auto imagePage = makePage(QStringLiteral("图片"), QStringLiteral("设置图片剪贴板历史"));
    QGroupBox *imageBox = new QGroupBox(QStringLiteral("图片记录"), imagePage.first);
    QFormLayout *imageForm = new QFormLayout(imageBox);
    QCheckBox *recordImages = new QCheckBox(QStringLiteral("记录图片剪贴板内容"), imageBox);
    recordImages->setChecked(settings.value(QStringLiteral("clipboard/recordsImages"), true).toBool());
    QSpinBox *maximumImage = new QSpinBox(imageBox);
    maximumImage->setRange(1, 100);
    maximumImage->setSuffix(QStringLiteral(" MB"));
    maximumImage->setValue(settings.value(QStringLiteral("clipboard/maximumImageMegabytes"), 10).toInt());
    imageForm->addRow(recordImages);
    imageForm->addRow(QStringLiteral("最大图片大小"), maximumImage);
    imagePage.second->addWidget(imageBox);
    imagePage.second->addStretch();
    pages->addWidget(imagePage.first);

    auto keyboardPage = makePage(QStringLiteral("键盘快捷键"), QStringLiteral("使用键盘快速操作 ClipDeck"));
    QGroupBox *keyBox = new QGroupBox(QStringLiteral("全局与 Deck"), keyboardPage.first);
    QFormLayout *keyForm = new QFormLayout(keyBox);
    QKeySequenceEdit *showDeckShortcut = new QKeySequenceEdit(
        configuredShortcut(QStringLiteral("shortcuts/showDeck"), QKeySequence(Qt::Key_F12)), keyBox);
    QKeySequenceEdit *previousCategoryShortcut = new QKeySequenceEdit(
        configuredShortcut(QStringLiteral("shortcuts/previousCategory"),
                           QKeySequence(Qt::CTRL | Qt::Key_Left)), keyBox);
    QKeySequenceEdit *nextCategoryShortcut = new QKeySequenceEdit(
        configuredShortcut(QStringLiteral("shortcuts/nextCategory"),
                           QKeySequence(Qt::CTRL | Qt::Key_Right)), keyBox);
    auto modifierPicker = [keyBox](Qt::KeyboardModifiers current) {
        QComboBox *picker = new QComboBox(keyBox);
        picker->addItem(QStringLiteral("Ctrl"), static_cast<int>(Qt::ControlModifier));
        picker->addItem(QStringLiteral("Shift"), static_cast<int>(Qt::ShiftModifier));
        picker->addItem(QStringLiteral("Alt"), static_cast<int>(Qt::AltModifier));
        picker->addItem(QStringLiteral("Meta"), static_cast<int>(Qt::MetaModifier));
        picker->setCurrentIndex(std::max(0, picker->findData(static_cast<int>(current))));
        return picker;
    };
    QComboBox *quickPasteModifier = modifierPicker(
        configuredModifier(QStringLiteral("shortcuts/quickPasteModifier"), Qt::ControlModifier));
    QComboBox *plainTextModifier = modifierPicker(
        configuredModifier(QStringLiteral("shortcuts/plainTextModifier"), Qt::ShiftModifier));
    keyForm->addRow(QStringLiteral("显示 / 隐藏 Deck"), showDeckShortcut);
    keyForm->addRow(QStringLiteral("显示上一个类型"), previousCategoryShortcut);
    keyForm->addRow(QStringLiteral("显示下一个类型"), nextCategoryShortcut);
    keyForm->addRow(QStringLiteral("快速粘贴"), quickPasteModifier);
    keyForm->addRow(QStringLiteral("纯文本模式"), plainTextModifier);
    keyForm->addRow(QStringLiteral("选择内容"), new QLabel(QStringLiteral("← / →"), keyBox));
    keyForm->addRow(QStringLiteral("粘贴"), new QLabel(QStringLiteral("Enter / 快速粘贴修饰键 + 1…9"), keyBox));
    keyForm->addRow(QStringLiteral("搜索"), new QLabel(QStringLiteral("Ctrl + F"), keyBox));
    QPushButton *resetShortcuts = new QPushButton(QStringLiteral("恢复默认快捷键"), keyBox);
    keyForm->addRow(QString(), resetShortcuts);
    QObject::connect(resetShortcuts, &QPushButton::clicked, [&] {
        showDeckShortcut->setKeySequence(QKeySequence(Qt::Key_F12));
        previousCategoryShortcut->setKeySequence(QKeySequence(Qt::CTRL | Qt::Key_Left));
        nextCategoryShortcut->setKeySequence(QKeySequence(Qt::CTRL | Qt::Key_Right));
        quickPasteModifier->setCurrentIndex(quickPasteModifier->findData(
            static_cast<int>(Qt::ControlModifier)));
        plainTextModifier->setCurrentIndex(plainTextModifier->findData(
            static_cast<int>(Qt::ShiftModifier)));
    });
    keyboardPage.second->addWidget(keyBox);
    keyboardPage.second->addStretch();
    pages->addWidget(keyboardPage.first);

    auto categoryPage = makePage(QStringLiteral("分类"), QStringLiteral("使用 JavaScript 自动整理剪贴板内容"));
    QGroupBox *categoryBox = new QGroupBox(QStringLiteral("自动分类"), categoryPage.first);
    QHBoxLayout *categoryLayout = new QHBoxLayout(categoryBox);
    QListWidget *categoryList = new QListWidget(categoryBox);
    categoryList->setObjectName(QStringLiteral("categoryList"));
    categoryList->setMinimumWidth(190);
    QVBoxLayout *categoryEditor = new QVBoxLayout;
    QLineEdit *categoryName = new QLineEdit(categoryBox);
    categoryName->setPlaceholderText(QStringLiteral("分类名称"));
    QHBoxLayout *categoryMetadata = new QHBoxLayout;
    QLineEdit *categoryIcon = new QLineEdit(categoryBox);
    categoryIcon->setPlaceholderText(QStringLiteral("图标，例如 </>"));
    QComboBox *previewLanguage = new QComboBox(categoryBox);
    previewLanguage->addItems({QStringLiteral("text"), QStringLiteral("code"),
        QStringLiteral("json"), QStringLiteral("sql"), QStringLiteral("shell"),
        QStringLiteral("markdown"), QStringLiteral("url"), QStringLiteral("color"),
        QStringLiteral("javascript"), QStringLiteral("java"), QStringLiteral("xml"),
        QStringLiteral("yaml")});
    categoryMetadata->addWidget(categoryIcon);
    categoryMetadata->addWidget(previewLanguage);
    QPlainTextEdit *categoryScript = new QPlainTextEdit(categoryBox);
    categoryScript->setPlaceholderText(QStringLiteral("function classify(item) {\n    return false;\n}"));
    categoryScript->setTabStopDistance(28);
    new CardSyntaxHighlighter(categoryScript->document(), QStringLiteral("javascript"));
    QHBoxLayout *categoryTestLayout = new QHBoxLayout;
    QLineEdit *categoryTestText = new QLineEdit(categoryBox);
    categoryTestText->setPlaceholderText(QStringLiteral("输入示例剪贴板内容测试规则"));
    QPushButton *testCategory = new QPushButton(QStringLiteral("测试规则"), categoryBox);
    QLabel *categoryTestResult = new QLabel(categoryBox);
    categoryTestResult->setMinimumWidth(88);
    categoryTestLayout->addWidget(categoryTestText, 1);
    categoryTestLayout->addWidget(testCategory);
    categoryTestLayout->addWidget(categoryTestResult);
    QHBoxLayout *categoryButtons = new QHBoxLayout;
    QPushButton *newCategory = new QPushButton(QStringLiteral("新建"), categoryBox);
    QPushButton *restoreCategories = new QPushButton(QStringLiteral("还原预制分类"), categoryBox);
    QPushButton *moveCategoryUp = new QPushButton(QStringLiteral("↑"), categoryBox);
    QPushButton *moveCategoryDown = new QPushButton(QStringLiteral("↓"), categoryBox);
    QPushButton *saveCategory = new QPushButton(QStringLiteral("保存"), categoryBox);
    QPushButton *deleteCategory = new QPushButton(QStringLiteral("删除"), categoryBox);
    categoryButtons->addWidget(newCategory);
    categoryButtons->addWidget(restoreCategories);
    categoryButtons->addWidget(moveCategoryUp);
    categoryButtons->addWidget(moveCategoryDown);
    categoryButtons->addStretch();
    categoryButtons->addWidget(deleteCategory);
    categoryButtons->addWidget(saveCategory);
    categoryEditor->addWidget(categoryName);
    categoryEditor->addLayout(categoryMetadata);
    categoryEditor->addWidget(categoryScript, 1);
    categoryEditor->addLayout(categoryTestLayout);
    categoryEditor->addLayout(categoryButtons);
    categoryLayout->addWidget(categoryList);
    categoryLayout->addLayout(categoryEditor, 1);
    QVector<ClipboardCategory> editableCategories;
    auto reloadCategories = [&] {
        editableCategories = repository->categories();
        categoryList->clear();
        for (const ClipboardCategory &category : editableCategories) categoryList->addItem(category.name);
        deck->refresh();
    };
    reloadCategories();
    QObject::connect(categoryList, &QListWidget::currentRowChanged, [&](int row) {
        if (row < 0 || row >= editableCategories.size()) return;
        categoryName->setText(editableCategories[row].name);
        categoryIcon->setText(editableCategories[row].icon);
        const int languageIndex = previewLanguage->findText(editableCategories[row].previewLanguage);
        previewLanguage->setCurrentIndex(std::max(0, languageIndex));
        categoryScript->setPlainText(editableCategories[row].script);
    });
    QObject::connect(newCategory, &QPushButton::clicked, [&] {
        categoryList->clearSelection();
        categoryList->setCurrentRow(-1);
        categoryName->setText(QStringLiteral("新分类"));
        categoryIcon->setText(QStringLiteral("●"));
        previewLanguage->setCurrentText(QStringLiteral("text"));
        categoryScript->setPlainText(QStringLiteral("function classify(item) {\n    return false;\n}"));
        categoryName->setFocus();
        categoryName->selectAll();
    });
    QObject::connect(testCategory, &QPushButton::clicked, [&] {
        QString error;
        const bool matches = repository->categoryScriptMatches(
            categoryScript->toPlainText(), categoryTestText->text(),
            previewLanguage->currentText(), &error);
        categoryTestResult->setText(error.isEmpty()
            ? (matches ? QStringLiteral("匹配：true") : QStringLiteral("匹配：false"))
            : QStringLiteral("错误：%1").arg(error));
        categoryTestResult->setStyleSheet(error.isEmpty()
            ? QStringLiteral("color:#a8d8ad;") : QStringLiteral("color:#ff7373;"));
    });
    QObject::connect(restoreCategories, &QPushButton::clicked, [&] {
        if (QMessageBox::question(&dialog, QStringLiteral("还原预制分类"),
            QStringLiteral("同名预制分类会恢复默认规则；自建分类会保留。"),
            QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Cancel) != QMessageBox::Ok) return;
        if (!repository->restoreDefaultCategories()) {
            QMessageBox::warning(&dialog, QStringLiteral("还原失败"), repository->lastError());
            return;
        }
        reloadCategories();
    });
    auto moveCategoryRow = [&](int direction) {
        const int row = categoryList->currentRow();
        const int target = row + direction;
        if (row < 0 || target < 0 || target >= editableCategories.size()) return;
        std::swap(editableCategories[row], editableCategories[target]);
        QVector<qint64> ids;
        ids.reserve(editableCategories.size());
        for (const ClipboardCategory &category : editableCategories) ids.append(category.databaseID);
        if (!repository->reorderCategories(ids)) return;
        reloadCategories();
        categoryList->setCurrentRow(target);
    };
    QObject::connect(moveCategoryUp, &QPushButton::clicked, [&] { moveCategoryRow(-1); });
    QObject::connect(moveCategoryDown, &QPushButton::clicked, [&] { moveCategoryRow(1); });
    QObject::connect(saveCategory, &QPushButton::clicked, [&] {
        const QString name = categoryName->text().trimmed();
        const QString script = categoryScript->toPlainText();
        if (name.isEmpty() || !script.contains(QStringLiteral("classify"))) {
            QMessageBox::warning(&dialog, QStringLiteral("无法保存"),
                                 QStringLiteral("请输入分类名称，并定义 classify(item) 函数。"));
            return;
        }
        const QString scriptError = repository->categoryScriptError(script);
        if (!scriptError.isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("脚本错误"), scriptError);
            return;
        }
        const int row = categoryList->currentRow();
        const bool ok = row >= 0 && row < editableCategories.size()
            ? repository->updateCategory(editableCategories[row].databaseID, name,
                                         categoryIcon->text(), script, previewLanguage->currentText())
            : repository->createCategory(name, categoryIcon->text(), script,
                                         previewLanguage->currentText());
        if (!ok) {
            QMessageBox::warning(&dialog, QStringLiteral("无法保存"), repository->lastError());
            return;
        }
        reloadCategories();
        const auto matches = categoryList->findItems(name, Qt::MatchExactly);
        if (!matches.isEmpty()) categoryList->setCurrentItem(matches.first());
    });
    QObject::connect(deleteCategory, &QPushButton::clicked, [&] {
        const int row = categoryList->currentRow();
        if (row < 0 || row >= editableCategories.size()) return;
        if (QMessageBox::warning(&dialog, QStringLiteral("删除分类"),
            QStringLiteral("确定删除“%1”？").arg(editableCategories[row].name),
            QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Cancel) != QMessageBox::Ok) return;
        repository->removeCategory(editableCategories[row].databaseID);
        reloadCategories();
        categoryName->clear();
        categoryIcon->clear();
        categoryScript->clear();
    });
    if (!editableCategories.isEmpty()) categoryList->setCurrentRow(0);
    categoryPage.second->addWidget(categoryBox);
    categoryPage.second->addStretch();
    pages->addWidget(categoryPage.first);

    auto dataPage = makePage(QStringLiteral("数据"), QStringLiteral("管理 ClipDeck 历史数据"));
    QGroupBox *dataBox = new QGroupBox(QStringLiteral("存储"), dataPage.first);
    QFormLayout *dataForm = new QFormLayout(dataBox);
    QLabel *size = new QLabel(QStringLiteral("%1 MB").arg(repository->databaseByteCount() / 1048576.0, 0, 'f', 2), dataBox);
    QPushButton *clearExpired = new QPushButton(QStringLiteral("清理过期内容"), dataBox);
    QPushButton *clearAll = new QPushButton(QStringLiteral("删除历史…"), dataBox);
    dataForm->addRow(QStringLiteral("当前占用"), size);
    dataForm->addRow(clearExpired, clearAll);
    dataPage.second->addWidget(dataBox);
    dataPage.second->addStretch();
    pages->addWidget(dataPage.first);

    QObject::connect(sidebar, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    sidebar->setCurrentRow(std::max(0, std::min(4, initialPage)));
    QObject::connect(clearExpired, &QPushButton::clicked, [&] {
        const int removed = repository->clearExpiredAndReclaim(selectedRetentionDays());
        deck->refresh();
        size->setText(QStringLiteral("%1 MB").arg(repository->databaseByteCount() / 1048576.0, 0, 'f', 2));
        QMessageBox::information(&dialog, QStringLiteral("清理完成"),
                                 QStringLiteral("已删除 %1 条过期历史并回收数据库空间。").arg(removed));
    });
    QObject::connect(clearAll, &QPushButton::clicked, [&] {
        if (QMessageBox::warning(&dialog, QStringLiteral("删除历史"),
            QStringLiteral("所有剪贴板历史将被永久删除。"),
            QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Cancel) == QMessageBox::Ok) {
            repository->clearAll();
            deck->refresh();
            size->setText(QStringLiteral("0.00 MB"));
        }
    });
    QObject::connect(&dialog, &QDialog::finished, [&] {
        settings.setValue(QStringLiteral("paste/preserveFormatting"), pasteFormat->currentIndex() == 1);
        settings.setValue(QStringLiteral("history/retentionDays"), selectedRetentionDays());
        settings.setValue(QStringLiteral("clipboard/recordsImages"), recordImages->isChecked());
        settings.setValue(QStringLiteral("clipboard/maximumImageMegabytes"), maximumImage->value());
        settings.setValue(QStringLiteral("shortcuts/showDeck"),
                          showDeckShortcut->keySequence().toString(QKeySequence::PortableText));
        settings.setValue(QStringLiteral("shortcuts/previousCategory"),
                          previousCategoryShortcut->keySequence().toString(QKeySequence::PortableText));
        settings.setValue(QStringLiteral("shortcuts/nextCategory"),
                          nextCategoryShortcut->keySequence().toString(QKeySequence::PortableText));
        settings.setValue(QStringLiteral("shortcuts/quickPasteModifier"),
                          quickPasteModifier->currentData().toInt());
        settings.setValue(QStringLiteral("shortcuts/plainTextModifier"),
                          plainTextModifier->currentData().toInt());
        setLaunchAtLogin(launchAtLogin->isChecked());
        repository->clearExpired(selectedRetentionDays());
        deck->refresh();
        if (shortcutChanged) shortcutChanged();
    });
    if (!renderPath.isEmpty()) {
        QDir().mkpath(QFileInfo(renderPath).absolutePath());
        QTimer::singleShot(100, &dialog, [&dialog, renderPath] {
            dialog.grab().save(renderPath, "PNG");
            dialog.reject();
        });
    }
    dialog.exec();
}

class GlobalShortcutFilter final : public QAbstractNativeEventFilter {
public:
    explicit GlobalShortcutFilter(std::function<void()> callback)
        : callback_(std::move(callback)), display_(QX11Info::display()) {
        if (QGuiApplication::platformName() != QStringLiteral("xcb") || !display_) {
            display_ = nullptr;
            return;
        }
        for (int index = 0; index < ScreenCount(display_); ++index) {
            roots_.push_back(RootWindow(display_, index));
        }
        configure();
    }

    ~GlobalShortcutFilter() override {
        unregisterShortcut();
    }

    void configure() {
        if (!display_) return;
        unregisterShortcut();
        const QKeySequence sequence = configuredShortcut(
            QStringLiteral("shortcuts/showDeck"), QKeySequence(Qt::Key_F12));
        if (sequence.isEmpty()) return;
        const int value = sequence[0];
        modifiers_ = 0;
        if (value & Qt::SHIFT) modifiers_ |= ShiftMask;
        if (value & Qt::CTRL) modifiers_ |= ControlMask;
        if (value & Qt::ALT) modifiers_ |= Mod1Mask;
        if (value & Qt::META) modifiers_ |= Mod4Mask;
        const int key = value & ~Qt::KeyboardModifierMask;
        KeySym symbol = NoSymbol;
        if (key >= Qt::Key_F1 && key <= Qt::Key_F35) symbol = XK_F1 + (key - Qt::Key_F1);
        else if (key >= Qt::Key_A && key <= Qt::Key_Z) symbol = XK_A + (key - Qt::Key_A);
        else if (key >= Qt::Key_0 && key <= Qt::Key_9) symbol = XK_0 + (key - Qt::Key_0);
        else if (key == Qt::Key_Left) symbol = XK_Left;
        else if (key == Qt::Key_Right) symbol = XK_Right;
        else if (key == Qt::Key_Up) symbol = XK_Up;
        else if (key == Qt::Key_Down) symbol = XK_Down;
        keycode_ = symbol == NoSymbol ? 0 : XKeysymToKeycode(display_, symbol);
        if (keycode_ == 0) return;
        const unsigned int lockVariants[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
        for (const Window root : roots_) {
            for (const unsigned int locks : lockVariants) {
                XGrabKey(display_, keycode_, modifiers_ | locks, root, True,
                         GrabModeAsync, GrabModeAsync);
            }
        }
        XSync(display_, False);
    }

    bool nativeEventFilter(const QByteArray &eventType, void *message, long *) override {
        if (eventType != QByteArrayLiteral("xcb_generic_event_t")) {
            return false;
        }
        const auto *event = static_cast<xcb_generic_event_t *>(message);
        if ((event->response_type & 0x7f) != XCB_KEY_PRESS) {
            return false;
        }
        const auto *keyEvent = reinterpret_cast<const xcb_key_press_event_t *>(event);
        const unsigned int relevantState = keyEvent->state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
        if (keyEvent->detail != keycode_ || relevantState != modifiers_) {
            return false;
        }
        callback_();
        return true;
    }

private:
    void unregisterShortcut() {
        if (!display_ || keycode_ == 0) return;
        const unsigned int lockVariants[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
        for (const Window root : roots_) {
            for (const unsigned int locks : lockVariants) {
                XUngrabKey(display_, keycode_, modifiers_ | locks, root);
            }
        }
        XSync(display_, False);
        keycode_ = 0;
    }

    std::function<void()> callback_;
    Display *display_ = nullptr;
    int keycode_ = 0;
    unsigned int modifiers_ = 0;
    std::vector<Window> roots_;
};

} // namespace

int main(int argc, char *argv[]) {
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("ClipDeck"));
    QApplication::setOrganizationName(QStringLiteral("ClipDeck"));
    QApplication::setQuitOnLastWindowClosed(false);
    app.setStyleSheet(QStringLiteral(
        "QMenu{background:#29292c;color:#eee;border:1px solid #444;padding:5px;}"
        "QMenu::item{padding:7px 24px 7px 12px;border-radius:6px;}"
        "QMenu::item:selected{background:#176fd1;color:white;}"
        "QMenu::separator{height:1px;background:#444;margin:5px 8px;}"
        "QDialog{background:#1d1d1f;color:#eee;}"
        "QDialog QLabel{color:#ddd;}"
        "QDialog QPlainTextEdit,QDialog QLineEdit{background:#151517;color:#eee;"
        "border:1px solid #3b3b3e;border-radius:8px;padding:7px;selection-background-color:#176fd1;}"
        "QDialog QPushButton{background:#363639;color:#eee;border:0;border-radius:8px;padding:8px 14px;}"
        "QDialog QPushButton:hover{background:#48484c;}"));

    Display *display = QGuiApplication::platformName() == QStringLiteral("xcb")
        ? QX11Info::display()
        : nullptr;
    const bool selfTestRequested = app.arguments().contains(QStringLiteral("--self-test"));
    QString renderCheckPath;
    QString settingsRenderPath;
    for (const QString &argument : app.arguments()) {
        if (argument.startsWith(QStringLiteral("--render-check="))) {
            renderCheckPath = argument.mid(QStringLiteral("--render-check=").size());
        }
        if (argument.startsWith(QStringLiteral("--settings-render="))) {
            settingsRenderPath = argument.mid(QStringLiteral("--settings-render=").size());
        }
    }
    const bool usesTemporaryRepository = selfTestRequested || !renderCheckPath.isEmpty()
        || !settingsRenderPath.isEmpty();
    QTemporaryDir selfTestDirectory;
    HistoryRepository repository(usesTemporaryRepository
        ? selfTestDirectory.filePath(QStringLiteral("history.sqlite")) : QString());
    QSettings settings;
    if (selfTestRequested) {
        const QDateTime now = QDateTime::currentDateTime();
        ClipboardItem first;
        first.title = QStringLiteral("文本");
        first.text = QStringLiteral("duplicate-window-test");
        first.sourceName = QStringLiteral("Self Test");
        first.copiedAt = now;
        const ClipboardItem savedFirst = repository.save(first);
        first.copiedAt = now.addSecs(5);
        const ClipboardItem savedDuplicate = repository.save(first);
        first.copiedAt = now.addSecs(16);
        const ClipboardItem savedLater = repository.save(first);

        ClipboardItem urlItem;
        urlItem.title = QStringLiteral("文本");
        urlItem.text = QStringLiteral("https://example.com/self-test");
        urlItem.sourceName = QStringLiteral("Self Test");
        const QDateTime urlDay(QDate(2026, 8, 11), QTime(10, 0));
        urlItem.copiedAt = urlDay;
        const ClipboardItem savedURL = repository.save(urlItem);
        urlItem.copiedAt = urlDay.addSecs(3600);
        const ClipboardItem savedURLAgain = repository.save(urlItem);
        repository.updateLinkMetadata(savedURL.databaseID, QStringLiteral("Example title"),
                                      QByteArrayLiteral("icon"), QByteArrayLiteral("preview"));
        const QVector<ClipboardItem> stored = repository.fetch(QString(), QString(), 100);
        const auto linked = std::find_if(stored.cbegin(), stored.cend(), [savedURL](const ClipboardItem &item) {
            return item.databaseID == savedURL.databaseID;
        });
        QString categoryTestError;
        const bool categoryInputMatches = repository.categoryScriptMatches(
            QStringLiteral("function classify(item){return item.textLength===4&&item.sourceApplication.name==='脚本测试'&&item.copiedAt>0;}"),
            QStringLiteral("test"), QStringLiteral("text"), &categoryTestError);

        const QString legacyPath = selfTestDirectory.filePath(QStringLiteral("preview6.sqlite"));
        const QString legacyConnection = QStringLiteral("clipdeck-preview6-self-test");
        bool legacySetupOK = false;
        {
            QSqlDatabase legacyDatabase = QSqlDatabase::addDatabase(
                QStringLiteral("QSQLITE"), legacyConnection);
            legacyDatabase.setDatabaseName(legacyPath);
            if (legacyDatabase.open()) {
                QSqlQuery query(legacyDatabase);
                legacySetupOK = query.exec(QStringLiteral(
                    "CREATE TABLE content (id INTEGER PRIMARY KEY AUTOINCREMENT,title TEXT NOT NULL,"
                    "content_type TEXT NOT NULL,plain_text TEXT,rich_text_html TEXT,image_data BLOB,"
                    "image_width INTEGER,image_height INTEGER,content_hash BLOB NOT NULL UNIQUE,"
                    "source_id TEXT,source_name TEXT NOT NULL,source_icon BLOB,category TEXT,"
                    "copied_at INTEGER NOT NULL,last_copied_at INTEGER NOT NULL,"
                    "copy_count INTEGER NOT NULL DEFAULT 1,updated_at INTEGER NOT NULL)"))
                    && query.exec(QStringLiteral(
                        "CREATE TABLE category (id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL UNIQUE,"
                        "script TEXT NOT NULL,sort_order INTEGER NOT NULL DEFAULT 0)"));
                const QStringList legacyCategories = {
                    QStringLiteral("JSON"), QStringLiteral("URL"), QStringLiteral("SQL"),
                    QStringLiteral("Markdown"), QStringLiteral("代码"), QStringLiteral("色彩")
                };
                for (int index = 0; legacySetupOK && index < legacyCategories.size(); ++index) {
                    query.prepare(QStringLiteral(
                        "INSERT INTO category(name,script,sort_order) VALUES(?,?,?)"));
                    query.addBindValue(legacyCategories[index]);
                    query.addBindValue(QStringLiteral("function classify(item){return false;}"));
                    query.addBindValue(index);
                    legacySetupOK = query.exec();
                }
            }
            legacyDatabase.close();
        }
        QSqlDatabase::removeDatabase(legacyConnection);
        HistoryRepository migratedLegacyRepository(legacyPath);
        ClipboardItem migratedFirst;
        migratedFirst.title = QStringLiteral("文本");
        migratedFirst.text = QStringLiteral("preview6-migration-test");
        migratedFirst.sourceName = QStringLiteral("Self Test");
        migratedFirst.copiedAt = now;
        const ClipboardItem migratedSavedFirst = migratedLegacyRepository.save(migratedFirst);
        migratedFirst.copiedAt = now.addSecs(20);
        const ClipboardItem migratedSavedLater = migratedLegacyRepository.save(migratedFirst);

        QMimeData httpURLMime;
        httpURLMime.setUrls({QUrl(QStringLiteral("https://example.com"))});
        httpURLMime.setText(QStringLiteral("https://example.com"));
        QMimeData localFileMime;
        localFileMime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/example.txt"))});
        QStringList failures;
        auto require = [&failures](bool condition, const QString &name) {
            if (!condition) failures.append(name);
        };
        require(repository.isOpen(), QStringLiteral("database-open"));
        require(repository.categories().size() == 6, QStringLiteral("default-categories"));
        require(legacySetupOK && migratedLegacyRepository.isOpen(),
                QStringLiteral("preview6-migration-open"));
        require(migratedLegacyRepository.categories().size() == 6,
                QStringLiteral("preview6-migration-categories"));
        require(migratedSavedFirst.databaseID >= 0
                    && migratedSavedLater.databaseID != migratedSavedFirst.databaseID,
                QStringLiteral("preview6-migration-removes-global-unique"));
        require(!containsLocalFileURL(&httpURLMime) && containsLocalFileURL(&localFileMime),
                QStringLiteral("clipboard-url-filter"));
        require(repository.classifyText(QStringLiteral("https://example.com")) == QStringLiteral("URL"),
                QStringLiteral("classify-url"));
        require(repository.classifyText(QStringLiteral("{\"ok\":true}")) == QStringLiteral("JSON"),
                QStringLiteral("classify-json"));
        require(repository.classifyText(QStringLiteral("select * from sample")) == QStringLiteral("SQL"),
                QStringLiteral("classify-sql"));
        require(repository.classifyText(QStringLiteral("#12abef")) == QStringLiteral("色彩"),
                QStringLiteral("classify-color"));
        require(repository.classifyText(QStringLiteral("def main():\n    return 1")) == QStringLiteral("代码"),
                QStringLiteral("classify-code"));
        require(HistoryRepository::detectedCodeLanguage(
                    QStringLiteral("def main():\n    return 1")) == QStringLiteral("python"),
                QStringLiteral("detect-code-language"));
        require(categoryInputMatches && categoryTestError.isEmpty(),
                QStringLiteral("category-script-input"));
        require(savedFirst.databaseID >= 0, QStringLiteral("save-first"));
        require(savedDuplicate.databaseID == savedFirst.databaseID, QStringLiteral("dedupe-10s"));
        require(savedDuplicate.copyCount == 2, QStringLiteral("dedupe-count"));
        require(savedLater.databaseID != savedFirst.databaseID, QStringLiteral("allow-after-10s"));
        require(savedURLAgain.databaseID == savedURL.databaseID, QStringLiteral("dedupe-url-day"));
        require(linked != stored.cend(), QStringLiteral("fetch-link"));
        require(linked != stored.cend() && linked->title == QStringLiteral("Example title"),
                QStringLiteral("link-title"));
        require(linked != stored.cend() && linked->hasFetchedLinkPreview,
                QStringLiteral("link-preview-state"));
        const bool passed = failures.isEmpty();
        QTextStream(stdout) << (passed ? QStringLiteral("self-test: ok\n")
                                      : QStringLiteral("self-test: failed: ") + failures.join(QStringLiteral(", "))
                                          + (repository.lastError().isEmpty() ? QString()
                                              : QStringLiteral("; ") + repository.lastError())
                                          + QLatin1Char('\n'));
        return passed ? 0 : 1;
    }
    repository.clearExpired(settings.value(QStringLiteral("history/retentionDays"), 30).toInt());
    DeckWindow deck(display, &repository);

    if (!settingsRenderPath.isEmpty()) {
        const QFileInfo output(settingsRenderPath);
        const QString categoryPath = output.absolutePath() + QLatin1Char('/')
            + output.completeBaseName() + QStringLiteral("-categories.png");
        showSettings(&repository, &deck, 0, {}, settingsRenderPath);
        showSettings(&repository, &deck, 3, {}, categoryPath);
        QTextStream(stdout) << QStringLiteral("settings-render: ok: %1, %2\n")
            .arg(settingsRenderPath, categoryPath);
        return 0;
    }

    if (!renderCheckPath.isEmpty()) {
        const QDateTime copiedAt = QDateTime::currentDateTime();
        auto saveText = [&](const QString &title, const QString &text, const QString &category,
                            int secondsAgo = 0) {
            ClipboardItem item;
            item.title = title;
            item.text = text;
            item.category = category;
            item.sourceName = QStringLiteral("Terminal");
            item.copiedAt = copiedAt.addSecs(-secondsAgo);
            repository.save(item);
        };
        saveText(QStringLiteral("文本"),
                 QStringLiteral("user@uos-PC:~$ dpkg-query -W clipdeck\nclipdeck  0.3.0~preview7"),
                 QStringLiteral("代码"), 5);
        saveText(QStringLiteral("文本"),
                 QStringLiteral("select title, category from content where copy_count > 1 order by last_copied_at desc;"),
                 QStringLiteral("SQL"), 75);
        saveText(QStringLiteral("文本"),
                 QStringLiteral("{\n  \"platform\": \"UOS\",\n  \"parity\": true,\n  \"shortcut\": \"F12\"\n}"),
                 QStringLiteral("JSON"), 160);
        saveText(QStringLiteral("色彩"), QStringLiteral("#21C46B"), QStringLiteral("色彩"), 240);

        QImage preview(720, 360, QImage::Format_ARGB32_Premultiplied);
        preview.fill(QColor(23, 93, 171));
        {
            QPainter painter(&preview);
            QLinearGradient gradient(preview.rect().topLeft(), preview.rect().bottomRight());
            gradient.setColorAt(0, QColor(29, 155, 240));
            gradient.setColorAt(1, QColor(93, 49, 180));
            painter.fillRect(preview.rect(), gradient);
            painter.setPen(Qt::white);
            QFont font = app.font(); font.setPixelSize(58); font.setBold(true); painter.setFont(font);
            painter.drawText(preview.rect(), Qt::AlignCenter, QStringLiteral("ClipDeck · UOS"));
        }
        QByteArray previewBytes;
        QBuffer previewBuffer(&previewBytes); previewBuffer.open(QIODevice::WriteOnly); preview.save(&previewBuffer, "PNG");
        ClipboardItem link;
        link.title = QStringLiteral("ClipDeck 跨平台设计");
        link.text = QStringLiteral("https://example.com/clipdeck");
        link.category = QStringLiteral("URL");
        link.sourceName = QStringLiteral("Browser");
        link.linkPreviewData = previewBytes;
        link.hasFetchedLinkPreview = true;
        link.copiedAt = copiedAt.addSecs(-320);
        repository.save(link);

        QImage sampleImage(640, 420, QImage::Format_ARGB32_Premultiplied);
        sampleImage.fill(QColor(248, 248, 250));
        {
            QPainter painter(&sampleImage);
            painter.setPen(QPen(QColor(30, 128, 245), 8));
            painter.drawRoundedRect(sampleImage.rect().adjusted(35, 35, -35, -35), 32, 32);
            painter.setPen(QColor(31, 31, 34));
            QFont font = app.font(); font.setPixelSize(44); font.setBold(true); painter.setFont(font);
            painter.drawText(sampleImage.rect(), Qt::AlignCenter, QStringLiteral("图片预览"));
        }
        QByteArray imageBytes;
        QBuffer imageBuffer(&imageBytes); imageBuffer.open(QIODevice::WriteOnly); sampleImage.save(&imageBuffer, "PNG");
        ClipboardItem imageItem;
        imageItem.title = QStringLiteral("图片");
        imageItem.type = ClipboardPayloadType::Image;
        imageItem.image = sampleImage;
        imageItem.imageData = imageBytes;
        imageItem.sourceName = QStringLiteral("Screenshot");
        imageItem.copiedAt = copiedAt.addSecs(-390);
        repository.save(imageItem);

        deck.refresh();
        QDir().mkpath(QFileInfo(renderCheckPath).absolutePath());
        auto render = [&](const QString &path, int height) {
            deck.setGeometry(8, 8, 2032, height);
            deck.show();
            app.processEvents();
            const bool saved = deck.grab().save(path, "PNG");
            deck.hide();
            return saved;
        };
        const QFileInfo output(renderCheckPath);
        const QString compactPath = output.absolutePath() + QLatin1Char('/')
            + output.completeBaseName() + QStringLiteral("-compact.png");
        const QString searchPath = output.absolutePath() + QLatin1Char('/')
            + output.completeBaseName() + QStringLiteral("-search.png");
        const bool expandedSaved = render(renderCheckPath, 292);
        const bool compactSaved = render(compactPath, 250);
        deck.setGeometry(8, 8, 2032, 292);
        deck.show();
        deck.beginSearchForRenderCheck();
        app.processEvents();
        const bool searchSaved = deck.grab().save(searchPath, "PNG");
        deck.hide();
        QTextStream(stdout) << (expandedSaved && compactSaved && searchSaved
            ? QStringLiteral("render-check: ok: %1, %2, %3\n").arg(renderCheckPath, compactPath, searchPath)
            : QStringLiteral("render-check: failed\n"));
        return expandedSaved && compactSaved && searchSaved ? 0 : 1;
    }

    QClipboard *clipboard = QApplication::clipboard();
    QByteArray lastClipboardFingerprint = clipboardFingerprint(
        clipboard->mimeData(QClipboard::Clipboard));
    auto captureClipboard = [&](bool captureEvenIfUnchanged) {
        const QMimeData *mimeData = clipboard->mimeData(QClipboard::Clipboard);
        const QByteArray fingerprint = clipboardFingerprint(mimeData);
        if (!captureEvenIfUnchanged && fingerprint == lastClipboardFingerprint) return;
        lastClipboardFingerprint = fingerprint;
        if (deck.takeClipboardCaptureSuppression()) return;
        if (containsLocalFileURL(mimeData)) return;

        const auto source = activeApplication(QX11Info::display());
        if (mimeData && mimeData->hasImage()) {
            if (!settings.value(QStringLiteral("clipboard/recordsImages"), true).toBool()) return;
            const QImage image = qvariant_cast<QImage>(mimeData->imageData());
            if (image.isNull()
                || static_cast<qint64>(image.width()) * image.height() > 60'000'000) return;
            const int maximumBytes = settings.value(QStringLiteral("clipboard/maximumImageMegabytes"), 10).toInt()
                * 1024 * 1024;
            QByteArray encoded;
            QBuffer buffer(&encoded);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            if (encoded.size() <= maximumBytes) deck.insertImage(image, source);
            return;
        }
        const QString text = clipboard->text(QClipboard::Clipboard);
        if (!text.isEmpty()) {
            deck.insertText(text, mimeData && mimeData->hasHtml() ? mimeData->html() : QString(), source);
        }
    };
    QObject::connect(clipboard, &QClipboard::dataChanged, [&] { captureClipboard(true); });
    QTimer clipboardPollTimer;
    clipboardPollTimer.setInterval(400);
    QObject::connect(&clipboardPollTimer, &QTimer::timeout, [&] { captureClipboard(false); });
    clipboardPollTimer.start();

    // The shipped UOS target uses X11/xcb. Keeping the non-X11 path inert lets
    // the packaged ELF and bundled Qt plugins be smoke-tested in a headless
    // environment where no system-tray host exists.
    if (!display) {
        return app.exec();
    }

    QSystemTrayIcon tray(transparentTrayIcon());
    QMenu trayMenu;
    QAction *showAction = trayMenu.addAction(QStringLiteral("显示 Deck"));
    QAction *settingsAction = trayMenu.addAction(QStringLiteral("设置…\tCtrl+,"));
    trayMenu.addSeparator();
    QAction *quitAction = trayMenu.addAction(QStringLiteral("退出 ClipDeck\tCtrl+Q"));
    tray.setContextMenu(&trayMenu);
    GlobalShortcutFilter shortcut([&deck] { deck.toggleNearPointer(); });
    app.installNativeEventFilter(&shortcut);
    auto updateShortcutPresentation = [&] {
        const QString label = configuredShortcut(
            QStringLiteral("shortcuts/showDeck"), QKeySequence(Qt::Key_F12)).toString(QKeySequence::NativeText);
        showAction->setText(QStringLiteral("显示 Deck\t%1").arg(label));
        tray.setToolTip(QStringLiteral("ClipDeck · %1 呼出").arg(label));
    };
    updateShortcutPresentation();

    QObject::connect(showAction, &QAction::triggered, [&deck] { deck.toggleNearPointer(); });
    QObject::connect(settingsAction, &QAction::triggered,
                     [&repository, &deck, &shortcut, &updateShortcutPresentation] {
        deck.hide();
        showSettings(&repository, &deck, 0, [&] {
            shortcut.configure();
            updateShortcutPresentation();
        });
    });
    deck.setSettingsCallback([&repository, &deck, &shortcut, &updateShortcutPresentation](int page) {
        deck.hide();
        showSettings(&repository, &deck, page, [&] {
            shortcut.configure();
            updateShortcutPresentation();
        });
    });
    QObject::connect(quitAction, &QAction::triggered, &app, &QApplication::quit);
    QObject::connect(&tray, &QSystemTrayIcon::activated,
                     [&deck](QSystemTrayIcon::ActivationReason reason) {
                         if (reason == QSystemTrayIcon::Trigger) {
                             deck.toggleNearPointer();
                         }
                     });
    tray.show();

    return app.exec();
}
