#include "history_repository.h"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDateTime>
#include <QDesktopWidget>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QSlider>
#include <QStackedWidget>
#include <QSettings>
#include <QSpinBox>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QUrl>
#include <QTimer>
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

#include <algorithm>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace {

constexpr int kHistoryCapacity = 100;
constexpr int kHorizontalInset = 8;
constexpr int kCardSpacing = 24;
constexpr int kContentInset = 22;

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
        searchEdit_->setClearButtonEnabled(true);
        searchEdit_->setVisible(false);
        searchEdit_->setStyleSheet(QStringLiteral(
            "QLineEdit{color:white;background:rgba(255,255,255,38);border:1px solid rgba(255,255,255,42);"
            "border-radius:17px;padding:5px 13px;font-size:13px;}"
            "QLineEdit:focus{border:1px solid rgba(90,170,255,210);background:rgba(25,25,28,225);}"));
        QObject::connect(searchEdit_, &QLineEdit::textChanged, this, [this](const QString &value) {
            searchQuery_ = value.trimmed();
            reload();
        });
        QObject::connect(searchEdit_, &QLineEdit::returnPressed, this, [this] {
            searchEdit_->hide();
            setFocus(Qt::ShortcutFocusReason);
        });
        reload();
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
        item.category = HistoryRepository::classifyText(text);
        item.copiedAt = QDateTime::currentDateTime();
        repository_->save(item);
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

    void pasteSelected() {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) {
            return;
        }

        suppressClipboardCapture_ = true;
        if (items_[selectedIndex_].isImage()) {
            QApplication::clipboard()->setImage(items_[selectedIndex_].image, QClipboard::Clipboard);
        } else {
            QSettings settings;
            const bool preserveFormatting = settings.value(
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
        if (!suppressClipboardCapture_) {
            return false;
        }
        suppressClipboardCapture_ = false;
        return true;
    }

    void refresh() { reload(); }
    void setSettingsCallback(std::function<void()> callback) {
        settingsCallback_ = std::move(callback);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        QPainterPath deckPath;
        deckPath.addRoundedRect(rect().adjusted(1, 1, -1, -1), 24, 24);
        painter.fillPath(deckPath, QColor(15, 16, 17, 92));
        painter.setPen(QPen(QColor(255, 255, 255, 55), 1));
        painter.drawPath(deckPath);

        QLinearGradient sheen(0, 0, 0, height());
        sheen.setColorAt(0, QColor(255, 255, 255, 14));
        sheen.setColorAt(0.45, QColor(255, 255, 255, 0));
        sheen.setColorAt(1, QColor(0, 0, 0, 18));
        painter.fillPath(deckPath, sheen);

        paintHeader(painter);
        if (items_.empty()) {
            paintEmptyState(painter);
        } else {
            paintCards(painter);
        }
    }

    void closeEvent(QCloseEvent *event) override {
        event->ignore();
        hide();
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Escape) {
            hide();
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            pasteSelected();
            return;
        }
        if (event->key() == Qt::Key_Left) {
            if (event->modifiers() == Qt::ControlModifier) {
                changeCategory(-1);
                return;
            }
            moveSelection(-1);
            return;
        }
        if (event->key() == Qt::Key_Right) {
            if (event->modifiers() == Qt::ControlModifier) {
                changeCategory(1);
                return;
            }
            moveSelection(1);
            return;
        }
        if (event->key() == Qt::Key_Delete) {
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
        if (event->key() == Qt::Key_Space) {
            previewSelected();
            return;
        }
        if (event->modifiers() == Qt::ControlModifier
            && event->key() >= Qt::Key_1 && event->key() <= Qt::Key_9) {
            const int index = event->key() - Qt::Key_1;
            if (index < static_cast<int>(items_.size())) {
                selectedIndex_ = index;
                pasteSelected();
            }
            return;
        }
        QWidget::keyPressEvent(event);
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
            const int first = categoryPage_ * kCategoriesPerPage;
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
                settingsCallback_();
                return;
            }
        }
        const int index = cardIndexAt(event->pos());
        if (index >= 0) {
            selectedIndex_ = index;
            ensureSelectionVisible();
            update();
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override {
        if (resizing_) {
            QDesktopWidget *desktop = QApplication::desktop();
            const QRect available = desktop->availableGeometry(desktop->screenNumber(frameGeometry().center()));
            const int bottom = resizeStartGeometry_.bottom() + 1;
            const int maximumHeight = std::max(250, std::min(520, static_cast<int>(available.height() * 0.55)));
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
            pasteSelected();
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
        QAction *deleteAction = menu.addAction(QStringLiteral("删除\tDelete"));
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
        return QRect(kContentInset, 66, width() - kContentInset * 2, height() - 80);
    }

    int cardSide() const {
        return std::max(166, std::min(270, cardsViewport().height() - 24));
    }

    QRect cardRect(int index) const {
        const QRect viewport = cardsViewport();
        const int side = cardSide();
        return QRect(
            viewport.x() - scrollOffset_ + index * (side + kCardSpacing),
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
        const int first = categoryPage_ * kCategoriesPerPage;
        const int last = std::min(categories.size(), first + kCategoriesPerPage);
        for (int index = first; index < last; ++index) {
            const QRect itemRect = categoryRect(index - first);
            if (categories[index] == selectedCategory_) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(255, 255, 255, 38));
                painter.drawRoundedRect(itemRect, 16, 16);
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(dots[index % 9]);
            painter.drawEllipse(QPoint(itemRect.x() + 14, itemRect.center().y()), 5, 5);
            painter.setPen(QColor(255, 255, 255, 225));
            painter.drawText(itemRect.adjusted(25, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft,
                             categories[index]);
        }

        painter.setPen(QColor(255, 255, 255, 210));
        QFont searchFont = font;
        searchFont.setPointSize(16);
        painter.setFont(searchFont);
        painter.drawText(searchRect(), Qt::AlignCenter, QStringLiteral("⌕"));
        painter.setFont(font);
        painter.setPen(QColor(255, 255, 255, categoryPage_ > 0 ? 210 : 55));
        painter.drawText(previousCategoryRect(), Qt::AlignCenter, QStringLiteral("‹"));
        painter.setPen(QColor(255, 255, 255,
            (categoryPage_ + 1) * kCategoriesPerPage < categories.size() ? 210 : 55));
        painter.drawText(nextCategoryRect(), Qt::AlignCenter, QStringLiteral("›"));
        QFont actionFont = font;
        actionFont.setPointSize(22);
        actionFont.setBold(false);
        painter.setFont(actionFont);
        painter.setPen(QColor(255, 255, 255, 220));
        painter.drawText(addRect(), Qt::AlignCenter, QStringLiteral("+"));
        actionFont.setPointSize(16);
        painter.setFont(actionFont);
        painter.drawText(moreRect(), Qt::AlignCenter, QStringLiteral("•••"));
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

        for (int index = 0; index < static_cast<int>(items_.size()); ++index) {
            const QRect card = cardRect(index);
            if (!card.intersects(viewport)) {
                continue;
            }

            QPainterPath path;
            path.addRoundedRect(card, 18, 18);
            const bool selected = index == selectedIndex_;
            painter.fillPath(path, QColor(19, 19, 21, 242));
            painter.setPen(QPen(selected ? QColor(117, 190, 255, 230) : QColor(255, 255, 255, 35),
                                selected ? 4.0 : 1.0));
            painter.drawPath(path);

            const QRect headerRect(card.x(), card.y(), card.width(), 52);
            QLinearGradient headerGradient(headerRect.topLeft(), headerRect.bottomRight());
            if (items_[index].isImage()) {
                headerGradient.setColorAt(0, QColor(92, 107, 128));
                headerGradient.setColorAt(1, QColor(56, 69, 87));
            } else if (selected) {
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

            QFont metaFont = font();
            metaFont.setPointSize(10);
            metaFont.setBold(true);
            painter.setFont(metaFont);
            painter.setPen(Qt::white);
            painter.drawText(card.adjusted(14, 8, -52, -card.height() + 44),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             items_[index].title);

            QFont smallFont = font();
            smallFont.setPointSize(9);
            painter.setFont(smallFont);
            painter.setPen(QColor(255, 255, 255, 185));
            painter.drawText(card.adjusted(14, 28, -52, -card.height() + 50),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             (items_[index].category.isEmpty() ? QString() : items_[index].category + QStringLiteral(" · "))
                                 + relativeTime(items_[index].copiedAt));
            const QRect sourceRect(card.right() - 42, card.y() + 8, 34, 34);
            if (!items_[index].sourceIcon.isNull()) {
                painter.drawImage(sourceRect, items_[index].sourceIcon);
            } else {
                painter.setPen(QColor(255, 255, 255, 225));
                painter.setBrush(QColor(255, 255, 255, 38));
                painter.drawRoundedRect(sourceRect, 8, 8);
                painter.drawText(sourceRect, Qt::AlignCenter,
                                 items_[index].sourceName.left(1).toUpper());
            }

            QFont contentFont = font();
            contentFont.setPointSize(13);
            painter.setFont(contentFont);
            painter.setPen(QColor(255, 255, 255, 225));
            const QRect textRect = card.adjusted(14, 65, -14, -32);
            painter.save();
            painter.setClipRect(textRect);
            const bool isColorCard = items_[index].category == QStringLiteral("色彩")
                && items_[index].isText();
            QColor parsedColor;
            if (isColorCard) {
                QString value = items_[index].text.trimmed();
                if (!value.startsWith(QLatin1Char('#')) && (value.size() == 6 || value.size() == 8)) {
                    value.prepend(QLatin1Char('#'));
                }
                parsedColor = QColor(value);
            }
            if (items_[index].isImage()) {
                const int checkerSize = 12;
                for (int y = textRect.y(); y < textRect.bottom(); y += checkerSize) {
                    for (int x = textRect.x(); x < textRect.right(); x += checkerSize) {
                        painter.fillRect(QRect(x, y, checkerSize, checkerSize),
                                         ((x / checkerSize + y / checkerSize) % 2 == 0)
                                             ? QColor(55, 55, 58) : QColor(34, 34, 37));
                    }
                }
                const QImage image = items_[index].image;
                const QSize fitted = image.size().scaled(textRect.size(), Qt::KeepAspectRatio);
                const QRect imageRect(
                    textRect.x() + (textRect.width() - fitted.width()) / 2,
                    textRect.y() + (textRect.height() - fitted.height()) / 2,
                    fitted.width(), fitted.height());
                painter.drawImage(imageRect, image);
            } else if (parsedColor.isValid()) {
                painter.fillRect(textRect, parsedColor);
                painter.setPen(Qt::white);
                painter.setBrush(QColor(0, 0, 0, 120));
                const QRect badge(textRect.center().x() - 44, textRect.bottom() - 38, 88, 28);
                painter.drawRoundedRect(badge, 14, 14);
                painter.drawText(badge, Qt::AlignCenter, items_[index].text.trimmed());
            } else {
                painter.drawText(textRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                                 items_[index].text);
            }
            painter.restore();

            painter.setFont(smallFont);
            painter.setPen(QColor(255, 255, 255, 115));
            const QString footer = items_[index].isImage()
                ? QStringLiteral("%1 × %2").arg(items_[index].image.width()).arg(items_[index].image.height())
                : QStringLiteral("%1 个字符").arg(items_[index].text.size());
            painter.drawText(QRect(card.x() + 14, card.bottom() - 29, card.width() - 28, 20),
                             Qt::AlignCenter, footer);
            if (index < 9) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(0, 0, 0, selected ? 168 : 122));
                painter.drawRoundedRect(QRect(card.right() - 37, card.bottom() - 37, 27, 27), 7, 7);
                painter.setPen(QColor(255, 255, 255, 238));
                painter.drawText(QRect(card.right() - 37, card.bottom() - 37, 27, 27), Qt::AlignCenter,
                                 QString::number(index + 1));
            }
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
        ensureSelectionVisible();
        update();
    }

    void changeCategory(int direction) {
        const QStringList categories = categoryValues();
        const int current = std::max(0, categories.indexOf(selectedCategory_));
        const int next = (current + direction + categories.size()) % categories.size();
        selectedCategory_ = categories[next];
        categoryPage_ = next / kCategoriesPerPage;
        reload();
    }

    void changeCategoryPage(int direction) {
        const int pages = (categoryValues().size() + kCategoriesPerPage - 1) / kCategoriesPerPage;
        categoryPage_ = std::max(0, std::min(pages - 1, categoryPage_ + direction));
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
        return {QStringLiteral("History"), QStringLiteral("Text"), QStringLiteral("Images"),
                QStringLiteral("URL"), QStringLiteral("JSON"), QStringLiteral("SQL"),
                QStringLiteral("代码"), QStringLiteral("Markdown"), QStringLiteral("色彩")};
    }

    QRect categoryRect(int index) const {
        return QRect(toolbarStartX() + index * 104, 12, 96, 35);
    }

    QRect searchRect() const {
        return QRect(toolbarStartX() - 48, 12, 36, 35);
    }

    int toolbarStartX() const {
        const int toolbarWidth = kCategoriesPerPage * 104 + 4 * 38;
        return std::max(62, (width() - toolbarWidth) / 2);
    }
    QRect previousCategoryRect() const { return QRect(toolbarStartX() + 624, 12, 34, 35); }
    QRect nextCategoryRect() const { return QRect(toolbarStartX() + 662, 12, 34, 35); }
    QRect addRect() const { return QRect(toolbarStartX() + 700, 12, 34, 35); }
    QRect moreRect() const { return QRect(width() - 68, 12, 42, 35); }

    void search() {
        searchEdit_->setGeometry(searchRect().x(), 10, 250, 38);
        searchEdit_->setText(searchQuery_);
        searchEdit_->show();
        searchEdit_->raise();
        searchEdit_->setFocus(Qt::ShortcutFocusReason);
        searchEdit_->selectAll();
    }

    void previewSelected() {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) return;
        QDialog dialog(this);
        dialog.setWindowTitle(items_[selectedIndex_].title);
        dialog.resize(680, 480);
        QVBoxLayout *layout = new QVBoxLayout(&dialog);
        if (items_[selectedIndex_].isImage()) {
            QLabel *label = new QLabel(&dialog);
            label->setAlignment(Qt::AlignCenter);
            label->setPixmap(QPixmap::fromImage(items_[selectedIndex_].image)
                                 .scaled(640, 420, Qt::KeepAspectRatio, Qt::SmoothTransformation));
            layout->addWidget(label);
        } else {
            QPlainTextEdit *text = new QPlainTextEdit(items_[selectedIndex_].text, &dialog);
            text->setReadOnly(true);
            layout->addWidget(text);
        }
        QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        layout->addWidget(buttons);
        dialog.exec();
    }

    void editSelected() {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) return;
        if (items_[selectedIndex_].isImage()) {
            previewSelected();
            return;
        }
        bool accepted = false;
        const QString value = QInputDialog::getMultiLineText(
            this, QStringLiteral("编辑文本"), QStringLiteral("内容"),
            items_[selectedIndex_].text, &accepted);
        if (accepted && !value.isEmpty()) {
            repository_->updateText(items_[selectedIndex_].databaseID, value,
                                    HistoryRepository::classifyText(value));
            reload();
        }
    }

    void renameSelected() {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) return;
        bool accepted = false;
        const QString title = QInputDialog::getText(
            this, QStringLiteral("重命名"), QStringLiteral("标题"), QLineEdit::Normal,
            items_[selectedIndex_].title, &accepted);
        if (accepted) {
            repository_->updateTitle(items_[selectedIndex_].databaseID, title);
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

    void clampScrollOffset() {
        const int contentWidth = static_cast<int>(items_.size()) * (cardSide() + kCardSpacing) - kCardSpacing;
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
        if (!display_) return;
        const Atom blur = XInternAtom(display_, "_KDE_NET_WM_BLUR_BEHIND_REGION", False);
        XChangeProperty(display_, static_cast<Window>(winId()), blur, XA_CARDINAL, 32,
                        PropModeReplace, nullptr, 0);
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
        QString repositoryCategory;
        if (selectedCategory_ == QStringLiteral("Text")) repositoryCategory = QStringLiteral("文本");
        else if (selectedCategory_ == QStringLiteral("Images")) repositoryCategory = QStringLiteral("图片");
        else if (selectedCategory_ != QStringLiteral("History")) repositoryCategory = selectedCategory_;
        items_ = repository_->fetch(repositoryCategory, searchQuery_, 300).toStdVector();
        selectedIndex_ = items_.empty() ? -1 : 0;
        scrollOffset_ = 0;
        update();
    }

    Display *display_ = nullptr;
    HistoryRepository *repository_ = nullptr;
    Window pasteTarget_ = None;
    std::vector<ClipboardItem> items_;
    int selectedIndex_ = -1;
    int scrollOffset_ = 0;
    bool suppressClipboardCapture_ = false;
    bool resizing_ = false;
    int resizeStartGlobalY_ = 0;
    QRect resizeStartGeometry_;
    int categoryPage_ = 0;
    static constexpr int kCategoriesPerPage = 6;
    QLineEdit *searchEdit_ = nullptr;
    std::function<void()> settingsCallback_;
    QString selectedCategory_ = QStringLiteral("History");
    QString searchQuery_;
};

void showLegacySettings(HistoryRepository *repository, DeckWindow *deck) {
    QSettings settings;
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("ClipDeck 设置"));
    dialog.resize(580, 420);
    QVBoxLayout *root = new QVBoxLayout(&dialog);
    QTabWidget *tabs = new QTabWidget(&dialog);

    QWidget *general = new QWidget(tabs);
    QFormLayout *generalForm = new QFormLayout(general);
    QCheckBox *launchAtLogin = new QCheckBox(QStringLiteral("登录 UOS 时自动启动 ClipDeck"), general);
    launchAtLogin->setChecked(QFile::exists(QDir::home().filePath(QStringLiteral(".config/autostart/clipdeck.desktop"))));
    QSpinBox *retention = new QSpinBox(general);
    retention->setRange(0, 3650);
    retention->setSpecialValueText(QStringLiteral("永久"));
    retention->setSuffix(QStringLiteral(" 天"));
    retention->setValue(settings.value(QStringLiteral("history/retentionDays"), 30).toInt());
    generalForm->addRow(QString(), launchAtLogin);
    generalForm->addRow(QStringLiteral("保留历史"), retention);
    tabs->addTab(general, QStringLiteral("通用"));

    QWidget *images = new QWidget(tabs);
    QFormLayout *imageForm = new QFormLayout(images);
    QCheckBox *recordImages = new QCheckBox(QStringLiteral("记录图片剪贴板内容"), images);
    recordImages->setChecked(settings.value(QStringLiteral("clipboard/recordsImages"), true).toBool());
    QSpinBox *maximumImage = new QSpinBox(images);
    maximumImage->setRange(1, 100);
    maximumImage->setSuffix(QStringLiteral(" MB"));
    maximumImage->setValue(settings.value(QStringLiteral("clipboard/maximumImageMegabytes"), 10).toInt());
    imageForm->addRow(QString(), recordImages);
    imageForm->addRow(QStringLiteral("最大图片大小"), maximumImage);
    tabs->addTab(images, QStringLiteral("图片"));

    QWidget *data = new QWidget(tabs);
    QFormLayout *dataForm = new QFormLayout(data);
    QLabel *size = new QLabel(QStringLiteral("%1 MB").arg(repository->databaseByteCount() / 1048576.0, 0, 'f', 2), data);
    QPushButton *clearExpired = new QPushButton(QStringLiteral("清理过期内容"), data);
    QPushButton *clearAll = new QPushButton(QStringLiteral("清空全部历史…"), data);
    dataForm->addRow(QStringLiteral("当前占用"), size);
    dataForm->addRow(QString(), clearExpired);
    dataForm->addRow(QString(), clearAll);
    QObject::connect(clearExpired, &QPushButton::clicked, [&] {
        const int removed = repository->clearExpired(retention->value());
        deck->refresh();
        QMessageBox::information(&dialog, QStringLiteral("清理完成"),
                                 QStringLiteral("已删除 %1 条过期历史").arg(removed));
    });
    QObject::connect(clearAll, &QPushButton::clicked, [&] {
        if (QMessageBox::warning(&dialog, QStringLiteral("清空全部历史"),
                                 QStringLiteral("所有文本和图片历史都将永久删除。"),
                                 QMessageBox::Cancel | QMessageBox::Ok,
                                 QMessageBox::Cancel) == QMessageBox::Ok) {
            repository->clearAll();
            deck->refresh();
        }
    });
    tabs->addTab(data, QStringLiteral("数据"));

    QWidget *keyboard = new QWidget(tabs);
    QFormLayout *keyboardForm = new QFormLayout(keyboard);
    keyboardForm->addRow(QStringLiteral("启动 ClipDeck"), new QLabel(QStringLiteral("Ctrl + Shift + V"), keyboard));
    keyboardForm->addRow(QStringLiteral("快速粘贴"), new QLabel(QStringLiteral("Ctrl + 1…9"), keyboard));
    keyboardForm->addRow(QStringLiteral("搜索"), new QLabel(QStringLiteral("Ctrl + F"), keyboard));
    keyboardForm->addRow(QStringLiteral("编辑 / 重命名"), new QLabel(QStringLiteral("Ctrl + E / Ctrl + R"), keyboard));
    tabs->addTab(keyboard, QStringLiteral("键盘快捷键"));

    root->addWidget(tabs);
    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Save, &dialog);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    root->addWidget(buttons);
    if (dialog.exec() == QDialog::Accepted) {
        settings.setValue(QStringLiteral("history/retentionDays"), retention->value());
        settings.setValue(QStringLiteral("clipboard/recordsImages"), recordImages->isChecked());
        settings.setValue(QStringLiteral("clipboard/maximumImageMegabytes"), maximumImage->value());
        setLaunchAtLogin(launchAtLogin->isChecked());
        repository->clearExpired(retention->value());
        deck->refresh();
    }
}

void showSettings(HistoryRepository *repository, DeckWindow *deck) {
    QSettings settings;
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("ClipDeck 设置"));
    dialog.resize(1120, 760);
    dialog.setMinimumSize(860, 600);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog{background:#1d1d1f;color:#eeeeef;}"
        "QListWidget{background:#28282a;border:0;padding:18px 10px;color:#aaa;font-size:17px;font-weight:600;}"
        "QListWidget::item{height:56px;padding-left:18px;border-radius:12px;}"
        "QListWidget::item:selected{background:#4a4a4d;color:white;}"
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
    sidebar->setFixedWidth(245);
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
        layout->setContentsMargins(48, 42, 48, 34);
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
    retention->setRange(0, 365);
    retention->setValue(std::min(365, settings.value(QStringLiteral("history/retentionDays"), 30).toInt()));
    auto updateRetention = [retentionValue](int days) {
        retentionValue->setText(days == 0 ? QStringLiteral("永久") : QStringLiteral("%1 天").arg(days));
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
    keyForm->addRow(QStringLiteral("显示 / 隐藏 Deck"), new QLabel(QStringLiteral("F12"), keyBox));
    keyForm->addRow(QStringLiteral("切换分类"), new QLabel(QStringLiteral("Ctrl + ← / →"), keyBox));
    keyForm->addRow(QStringLiteral("选择内容"), new QLabel(QStringLiteral("← / →"), keyBox));
    keyForm->addRow(QStringLiteral("粘贴"), new QLabel(QStringLiteral("Enter / Ctrl + 1…9"), keyBox));
    keyForm->addRow(QStringLiteral("搜索"), new QLabel(QStringLiteral("Ctrl + F"), keyBox));
    keyboardPage.second->addWidget(keyBox);
    keyboardPage.second->addStretch();
    pages->addWidget(keyboardPage.first);

    auto categoryPage = makePage(QStringLiteral("分类"), QStringLiteral("当前版本的内置分类"));
    QGroupBox *categoryBox = new QGroupBox(QStringLiteral("可用分类"), categoryPage.first);
    QVBoxLayout *categoryLayout = new QVBoxLayout(categoryBox);
    categoryLayout->addWidget(new QLabel(QStringLiteral("History   Text   Images   URL   JSON   SQL   代码   Markdown   色彩"), categoryBox));
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
    sidebar->setCurrentRow(0);
    QObject::connect(clearExpired, &QPushButton::clicked, [&] {
        repository->clearExpired(retention->value());
        deck->refresh();
        size->setText(QStringLiteral("%1 MB").arg(repository->databaseByteCount() / 1048576.0, 0, 'f', 2));
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
        settings.setValue(QStringLiteral("history/retentionDays"), retention->value());
        settings.setValue(QStringLiteral("clipboard/recordsImages"), recordImages->isChecked());
        settings.setValue(QStringLiteral("clipboard/maximumImageMegabytes"), maximumImage->value());
        setLaunchAtLogin(launchAtLogin->isChecked());
        repository->clearExpired(retention->value());
        deck->refresh();
    });
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

        keycode_ = XKeysymToKeycode(display_, XK_F12);
        modifiers_ = 0;
        const unsigned int lockVariants[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
        for (int index = 0; index < ScreenCount(display_); ++index) {
            const Window root = RootWindow(display_, index);
            roots_.push_back(root);
            for (const unsigned int locks : lockVariants) {
                XGrabKey(display_, keycode_, modifiers_ | locks, root, True,
                         GrabModeAsync, GrabModeAsync);
            }
        }
        XSync(display_, False);
    }

    ~GlobalShortcutFilter() override {
        if (!display_) {
            return;
        }
        const unsigned int lockVariants[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};
        for (const Window root : roots_) {
            for (const unsigned int locks : lockVariants) {
                XUngrabKey(display_, keycode_, modifiers_ | locks, root);
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

    Display *display = QGuiApplication::platformName() == QStringLiteral("xcb")
        ? QX11Info::display()
        : nullptr;
    HistoryRepository repository;
    QSettings settings;
    repository.clearExpired(settings.value(QStringLiteral("history/retentionDays"), 30).toInt());
    DeckWindow deck(display, &repository);

    QClipboard *clipboard = QApplication::clipboard();
    QObject::connect(clipboard, &QClipboard::dataChanged, [&deck, clipboard, &settings] {
        if (deck.takeClipboardCaptureSuppression()) {
            return;
        }
        const auto source = activeApplication(QX11Info::display());
        const QMimeData *mimeData = clipboard->mimeData(QClipboard::Clipboard);
        if (mimeData && mimeData->hasUrls()) {
            return;
        }
        if (mimeData && mimeData->hasImage()) {
            if (!settings.value(QStringLiteral("clipboard/recordsImages"), true).toBool()) return;
            const QImage image = qvariant_cast<QImage>(mimeData->imageData());
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
    });

    // The shipped UOS target uses X11/xcb. Keeping the non-X11 path inert lets
    // the packaged ELF and bundled Qt plugins be smoke-tested in a headless
    // environment where no system-tray host exists.
    if (!display) {
        return app.exec();
    }

    QSystemTrayIcon tray(transparentTrayIcon());
    tray.setToolTip(QStringLiteral("ClipDeck · F12 呼出"));
    QMenu trayMenu;
    QAction *showAction = trayMenu.addAction(QStringLiteral("显示 Deck\tF12"));
    QAction *settingsAction = trayMenu.addAction(QStringLiteral("设置…\tCtrl+,"));
    trayMenu.addSeparator();
    QAction *quitAction = trayMenu.addAction(QStringLiteral("退出 ClipDeck\tCtrl+Q"));
    tray.setContextMenu(&trayMenu);

    QObject::connect(showAction, &QAction::triggered, [&deck] { deck.toggleNearPointer(); });
    QObject::connect(settingsAction, &QAction::triggered, [&repository, &deck] {
        deck.hide();
        showSettings(&repository, &deck);
    });
    deck.setSettingsCallback([&repository, &deck] {
        deck.hide();
        showSettings(&repository, &deck);
    });
    QObject::connect(quitAction, &QAction::triggered, &app, &QApplication::quit);
    QObject::connect(&tray, &QSystemTrayIcon::activated,
                     [&deck](QSystemTrayIcon::ActivationReason reason) {
                         if (reason == QSystemTrayIcon::Trigger) {
                             deck.toggleNearPointer();
                         }
                     });
    tray.show();

    GlobalShortcutFilter shortcut([&deck] { deck.toggleNearPointer(); });
    app.installNativeEventFilter(&shortcut);

    return app.exec();
}
