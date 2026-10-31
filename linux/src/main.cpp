#include "history_repository.h"
#include "history/clipboard_pipeline.h"
#include "history/clipboard_pipeline_tests.h"
#include "history/file_reference_tests.h"
#include "platform/linux_application_icon.h"
#include "platform/x11_global_shortcut.h"
#include "ui/display_scale.h"
#include "ui/eliding_label.h"
#include "ui/deck_symbols.h"
#include "ui/file_card_view.h"
#include "ui/linux_icons.h"
#include "ui/script_editor_widget.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QButtonGroup>
#include <QColorDialog>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QCryptographicHash>
#include <QCursor>
#include <QDateTime>
#include <QDesktopWidget>
#include <QDesktopServices>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDirIterator>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHash>
#include <QHideEvent>
#include <QHelpEvent>
#include <QIcon>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QKeySequenceEdit>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QListWidget>
#include <QListView>
#include <QLineEdit>
#include <QMenu>
#include <QMap>
#include <QMimeData>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProcess>
#include <QProxyStyle>
#include <QPointer>
#include <QPushButton>
#include <QRegion>
#include <QRegularExpression>
#include <QScreen>
#include <QScopedValueRollback>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QSlider>
#include <QShowEvent>
#include <QSocketNotifier>
#include <QSqlQuery>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QSystemTrayIcon>
#include <QSyntaxHighlighter>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextOption>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QUrl>
#include <QTimer>
#include <QVariantAnimation>
#include <QTextStream>
#include <QTransform>
#include <QToolTip>
#include <QSet>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWheelEvent>
#include <QX11Info>
#include <QtWebEngine/QtWebEngine>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/XKBlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <xcb/xcb.h>

#ifdef KeyPress
#undef KeyPress
#endif
#ifdef KeyRelease
#undef KeyRelease
#endif

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

namespace {

constexpr int kHistoryCapacity = 100;
constexpr int kInitialImageCapacity = 20;
constexpr int kImageBatchCapacity = 20;
constexpr int kHorizontalInset = 8;
constexpr int kCardSpacing = 24;
constexpr int kContentInset = 22;
constexpr auto kShortcutRecordingStarted = "__CLIPDECK_SHORTCUT_RECORDING_STARTED__";
constexpr auto kShortcutRecordingStopped = "__CLIPDECK_SHORTCUT_RECORDING_STOPPED__";
constexpr auto kSettingsActivatePrefix = "__CLIPDECK_SETTINGS_ACTIVATE__:";

QKeySequence defaultShowDeckShortcut() {
    return QKeySequence(Qt::ALT | Qt::SHIFT | Qt::Key_V);
}

class ClipDeckStyle final : public QProxyStyle {
public:
    using QProxyStyle::QProxyStyle;

    void drawPrimitive(PrimitiveElement element, const QStyleOption *option,
                       QPainter *painter, const QWidget *widget = nullptr) const override {
        // UOS themes draw an additional native focus rectangle on top of
        // styled inputs. ClipDeck supplies its own focus border instead.
        if (element == PE_FrameFocusRect) return;
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
};

QKeySequence configuredShortcut(const QString &key, const QKeySequence &fallback) {
    const QString value = QSettings().value(key, fallback.toString(QKeySequence::PortableText)).toString();
    const QKeySequence sequence(value, QKeySequence::PortableText);
    return sequence.isEmpty() ? fallback : sequence;
}

Qt::KeyboardModifiers configuredModifier(const QString &key, Qt::KeyboardModifier fallback) {
    return static_cast<Qt::KeyboardModifiers>(
        QSettings().value(key, static_cast<int>(fallback)).toInt());
}

Qt::KeyboardModifiers standardKeyboardModifiers(Qt::KeyboardModifiers modifiers) {
    return modifiers & (Qt::ControlModifier | Qt::ShiftModifier
                        | Qt::AltModifier | Qt::MetaModifier);
}

unsigned int x11MaskForQtModifiers(Qt::KeyboardModifiers modifiers) {
    unsigned int mask = 0;
    if (modifiers.testFlag(Qt::ShiftModifier)) mask |= ShiftMask;
    if (modifiers.testFlag(Qt::ControlModifier)) mask |= ControlMask;
    if (modifiers.testFlag(Qt::AltModifier)) mask |= Mod1Mask;
    if (modifiers.testFlag(Qt::MetaModifier)) mask |= Mod4Mask;
    return mask;
}

bool matchesShortcut(const QKeyEvent *event, const QString &key, const QKeySequence &fallback) {
    const QKeySequence configured = configuredShortcut(key, fallback);
    const Qt::KeyboardModifiers modifiers = standardKeyboardModifiers(event->modifiers());
    return !configured.isEmpty()
        && configured[0] == (event->key() | static_cast<int>(modifiers));
}

bool isSettingsShortcut(const QKeyEvent *event) {
    return event->key() == Qt::Key_Comma
        && standardKeyboardModifiers(event->modifiers()) == Qt::ControlModifier;
}

Qt::KeyboardModifier modifierForKey(int key) {
    if (key == Qt::Key_Control) return Qt::ControlModifier;
    if (key == Qt::Key_Shift) return Qt::ShiftModifier;
    if (key == Qt::Key_Alt) return Qt::AltModifier;
    if (key == Qt::Key_Meta) return Qt::MetaModifier;
    return Qt::NoModifier;
}

QString shortcutLabel(const QKeySequence &sequence) {
    if (sequence.isEmpty()) return QStringLiteral("未设置");
    return sequence.toString(QKeySequence::NativeText).replace(QStringLiteral("+"), QStringLiteral(" + "));
}

class ShortcutCaptureButton final : public QPushButton {
public:
    explicit ShortcutCaptureButton(const QKeySequence &sequence, QWidget *parent = nullptr)
        : QPushButton(parent), sequence_(sequence) {
        setFocusPolicy(Qt::StrongFocus);
        setMinimumHeight(36);
        refreshLabel();
        connect(this, &QPushButton::clicked, this, [this] {
            if (recording_) return;
            recording_ = true;
            if (recordingCallback_) recordingCallback_(true);
            setText(QStringLiteral("请按新的快捷键…"));
            setProperty("recording", true);
            style()->unpolish(this);
            style()->polish(this);
            setFocus(Qt::MouseFocusReason);
        });
    }

    QKeySequence keySequence() const { return sequence_; }
    void setChangeCallback(std::function<void(const QKeySequence &)> callback) {
        changeCallback_ = std::move(callback);
    }
    void setRecordingCallback(std::function<void(bool)> callback) {
        recordingCallback_ = std::move(callback);
    }
    void setKeySequence(const QKeySequence &sequence) {
        const bool changed = sequence_ != sequence;
        const bool wasRecording = recording_;
        sequence_ = sequence;
        recording_ = false;
        setProperty("recording", false);
        refreshLabel();
        style()->unpolish(this);
        style()->polish(this);
        if (changed && changeCallback_) changeCallback_(sequence_);
        if (wasRecording && recordingCallback_) recordingCallback_(false);
    }

protected:
    void keyPressEvent(QKeyEvent *event) override {
        if (!recording_) {
            QPushButton::keyPressEvent(event);
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            cancelRecording(true);
            event->accept();
            return;
        }
        if (modifierForKey(event->key()) != Qt::NoModifier) return;
        const bool functionKey = event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F35;
        const Qt::KeyboardModifiers modifiers = event->modifiers() &
            (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
        if (modifiers == Qt::NoModifier && !functionKey) {
            QApplication::beep();
            return;
        }
        if (keySymForQtKey(event->key()) == NoSymbol) {
            QApplication::beep();
            return;
        }
        setKeySequence(QKeySequence(event->key() | static_cast<int>(modifiers)));
    }

    void focusOutEvent(QFocusEvent *event) override {
        if (recording_) cancelRecording(false);
        QPushButton::focusOutEvent(event);
    }

private:
    void cancelRecording(bool removeFocus) {
        const bool wasRecording = recording_;
        recording_ = false;
        setProperty("recording", false);
        refreshLabel();
        style()->unpolish(this);
        style()->polish(this);
        update();
        if (removeFocus && hasFocus()) clearFocus();
        if (wasRecording && recordingCallback_) recordingCallback_(false);
    }

    void refreshLabel() { setText(shortcutLabel(sequence_)); }
    QKeySequence sequence_;
    bool recording_ = false;
    std::function<void(const QKeySequence &)> changeCallback_;
    std::function<void(bool)> recordingCallback_;
};

class StyledComboBox final : public QComboBox {
public:
    explicit StyledComboBox(QWidget *parent = nullptr) : QComboBox(parent) {
        auto *list = new QListView(this);
        list->setFrameShape(QFrame::NoFrame);
        list->setContentsMargins(0, 0, 0, 0);
        list->setStyleSheet(QStringLiteral(
            "QListView{background:#29292c;color:#eeeeef;border:0;padding:0;margin:0;outline:0;}"
            "QListView::item{color:#eeeeef;background:#29292c;padding:7px 10px;min-height:22px;}"
            "QListView::item:selected,QListView::item:hover{background:#176fd1;color:white;}"));
        QPalette popupPalette = list->palette();
        popupPalette.setColor(QPalette::Text, QColor(238, 238, 239));
        popupPalette.setColor(QPalette::WindowText, QColor(238, 238, 239));
        popupPalette.setColor(QPalette::HighlightedText, Qt::white);
        popupPalette.setColor(QPalette::Base, QColor(41, 41, 44));
        popupPalette.setColor(QPalette::Highlight, QColor(23, 111, 209));
        list->setPalette(popupPalette);
        list->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list->setUniformItemSizes(true);
        setView(list);
        setMaxVisibleItems(20);
        preparePopup();
    }

protected:
    void showPopup() override {
        preparePopup();
        QComboBox::showPopup();
        preparePopup();
    }

    void paintEvent(QPaintEvent *event) override {
        QComboBox::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPen pen(QColor(224, 224, 226), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        painter.setPen(pen);
        const int x = width() - 18;
        const int y = height() / 2 - 1;
        QPainterPath chevron;
        chevron.moveTo(x - 4, y - 2);
        chevron.lineTo(x, y + 2);
        chevron.lineTo(x + 4, y - 2);
        painter.drawPath(chevron);
    }

private:
    void preparePopup() {
        QWidget *popup = view() ? view()->window() : nullptr;
        if (!popup || !(popup->windowFlags() & Qt::Popup)) return;
        popup->setAttribute(Qt::WA_StyledBackground, true);
        popup->setWindowFlag(Qt::FramelessWindowHint, true);
        popup->setWindowFlag(Qt::NoDropShadowWindowHint, true);
        popup->setContentsMargins(0, 0, 0, 0);
        QPalette palette = popup->palette();
        palette.setColor(QPalette::Window, QColor(41, 41, 44));
        palette.setColor(QPalette::Base, QColor(41, 41, 44));
        popup->setPalette(palette);
        popup->setStyleSheet(QStringLiteral(
            "QFrame{background:#29292c;border:1px solid #49494e;padding:0;margin:0;}"
            "QScrollBar{width:0px;height:0px;background:transparent;}"));
    }
};

class CategoryListDelegate final : public QStyledItemDelegate {
public:
    explicit CategoryListDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override {
        return QSize(210, 42);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect row = option.rect.adjusted(4, 2, -4, -2);
        if (option.state & QStyle::State_Selected) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(255, 255, 255, 31));
            painter->drawRoundedRect(row, 9, 9);
        }
        QFont textFont = option.font;
        textFont.setPointSizeF(10.5);
        textFont.setWeight(QFont::DemiBold);
        painter->setFont(textFont);
        painter->setPen((option.state & QStyle::State_Selected)
            ? QColor(255, 255, 255, 240) : QColor(215, 215, 220));
        // The same dot the category's capsule shows in the Deck.
        painter->save();
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(HistoryRepository::categoryColor(
            index.data(Qt::UserRole + 1).toString(), index.row())));
        painter->drawEllipse(QPointF(row.x() + 24, row.center().y()), 5.5, 5.5);
        painter->restore();
        const QRect nameRect(row.x() + 43, row.y(), std::max(20, row.width() - 112), row.height());
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(textFont).elidedText(index.data(Qt::DisplayRole).toString(),
                                                           Qt::ElideRight, nameRect.width()));
        painter->setPen(QPen(QColor(255, 255, 255, 100), 1.4, Qt::SolidLine,
                             Qt::RoundCap, Qt::RoundJoin));
        const int handleX = row.right() - 43;
        for (int offset : {-4, 0, 4}) {
            painter->drawLine(handleX - 5, row.center().y() + offset,
                              handleX + 5, row.center().y() + offset);
        }
        painter->setBrush(QColor(235, 235, 238, 190));
        painter->setPen(Qt::NoPen);
        for (int offset : {-6, 0, 6}) {
            painter->drawEllipse(QPointF(row.right() - 13 + offset, row.center().y()), 1.5, 1.5);
        }
        painter->restore();
    }
};

class CategoryListWidget final : public QListWidget {
public:
    explicit CategoryListWidget(QWidget *parent = nullptr) : QListWidget(parent) {}

    void setDeleteCallback(std::function<void(int)> callback) {
        deleteCallback_ = std::move(callback);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        QListWidgetItem *hit = itemAt(event->pos());
        if (hit && event->pos().x() >= viewport()->width() - 38) {
            const int hitRow = row(hit);
            setCurrentRow(hitRow);
            if (deleteCallback_) deleteCallback_(hitRow);
            event->accept();
            return;
        }
        QListWidget::mousePressEvent(event);
    }

private:
    std::function<void(int)> deleteCallback_;
};

class CategoryHeaderButton final : public QPushButton {
public:
    enum class Symbol { Restore, Add };
    CategoryHeaderButton(Symbol symbol, QWidget *parent = nullptr)
        : QPushButton(parent), symbol_(symbol) {
        setFixedSize(34, 34);
        setObjectName(QStringLiteral("categoryHeaderButton"));
    }

protected:
    void paintEvent(QPaintEvent *event) override {
        QPushButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(235, 235, 238), 1.8, Qt::SolidLine,
                            Qt::RoundCap, Qt::RoundJoin));
        const QPoint center = rect().center();
        if (symbol_ == Symbol::Add) {
            painter.drawLine(center + QPoint(-6, 0), center + QPoint(6, 0));
            painter.drawLine(center + QPoint(0, -6), center + QPoint(0, 6));
        } else {
            painter.drawArc(QRect(center.x() - 7, center.y() - 7, 14, 14), 35 * 16, 285 * 16);
            QPainterPath arrow;
            arrow.moveTo(center + QPoint(-7, -5));
            arrow.lineTo(center + QPoint(-8, 1));
            arrow.lineTo(center + QPoint(-2, -1));
            painter.drawPath(arrow);
        }
    }

private:
    Symbol symbol_;
};

struct RetentionOption {
    int days;
    QString title;
    double position;
    bool major;
};

const QVector<RetentionOption> &retentionOptions() {
    static const QVector<RetentionOption> options = [] {
        QVector<RetentionOption> values;
        for (int day = 1; day <= 6; ++day) {
            values.append({day, QStringLiteral("%1天").arg(day),
                           static_cast<double>(day - 1) / 6.0 * 0.25, day == 1});
        }
        values.append({7, QStringLiteral("1周"), 0.25, true});
        values.append({14, QStringLiteral("2周"), 0.25 + 1.0 / 3.0 * 0.25, false});
        values.append({21, QStringLiteral("3周"), 0.25 + 2.0 / 3.0 * 0.25, false});
        values.append({30, QStringLiteral("1个月"), 0.5, true});
        for (int month = 2; month <= 11; ++month) {
            values.append({month * 30, QStringLiteral("%1个月").arg(month),
                           0.5 + static_cast<double>(month - 1) / 11.0 * 0.25, false});
        }
        values.append({365, QStringLiteral("1年"), 0.75, true});
        values.append({0, QStringLiteral("永久"), 1.0, true});
        return values;
    }();
    return options;
}

bool isShorterRetention(int currentDays, int proposedDays) {
    return currentDays == 0 ? proposedDays != 0
        : proposedDays != 0 && proposedDays < currentDays;
}

class RetentionControl final : public QWidget {
public:
    explicit RetentionControl(int days, QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumHeight(94);
        setValue(days);
    }

    int value() const { return retentionOptions()[index_].days; }
    void setChangeCallback(std::function<void(int)> callback) {
        changeCallback_ = std::move(callback);
    }
    void setValue(int days) {
        int best = 0;
        int distance = std::numeric_limits<int>::max();
        for (int index = 0; index < retentionOptions().size(); ++index) {
            if (retentionOptions()[index].days == days) { best = index; break; }
            if (retentionOptions()[index].days != 0 && std::abs(retentionOptions()[index].days - days) < distance) {
                distance = std::abs(retentionOptions()[index].days - days);
                best = index;
            }
        }
        index_ = best;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const int left = 14;
        const int right = width() - 14;
        const int usable = std::max(1, right - left);
        const int trackY = 40;
        const double selected = retentionOptions()[index_].position;
        const int selectedX = left + qRound(usable * selected);

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(58, 58, 62));
        painter.drawRoundedRect(QRect(left, trackY - 2, usable, 5), 2, 2);
        painter.setBrush(QColor(22, 131, 255));
        painter.drawRoundedRect(QRect(left, trackY - 2, std::max(3, selectedX - left), 5), 2, 2);
        painter.setBrush(QColor(238, 238, 240));
        painter.drawEllipse(QPoint(selectedX, trackY), 9, 9);

        painter.setPen(QColor(238, 238, 240));
        QFont valueFont = font();
        valueFont.setBold(true);
        painter.setFont(valueFont);
        const QString valueTitle = retentionOptions()[index_].title;
        painter.drawText(QRect(std::max(0, selectedX - 45), 3, 90, 20), Qt::AlignCenter, valueTitle);

        painter.setFont(font());
        painter.setPen(QColor(155, 155, 160));
        const QVector<QPair<QString, double>> labels = {
            {QStringLiteral("天"), 0}, {QStringLiteral("周"), 0.25},
            {QStringLiteral("月"), 0.5}, {QStringLiteral("年"), 0.75},
            {QStringLiteral("永久"), 1.0}};
        for (const auto &label : labels) {
            const int x = left + qRound(usable * label.second);
            painter.drawText(QRect(x - 30, 68, 60, 20), Qt::AlignCenter, label.first);
        }
        for (const RetentionOption &option : retentionOptions()) {
            const int x = left + qRound(usable * option.position);
            painter.setBrush(QColor(155, 155, 160, option.major ? 170 : 85));
            const int radius = option.major ? 2 : 1;
            painter.drawEllipse(QPoint(x, 61), radius, radius);
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        dragging_ = true;
        dragStartDays_ = value();
        selectAt(event->pos().x());
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (event->buttons() & Qt::LeftButton) selectAt(event->pos().x());
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        if (dragging_ && event->button() == Qt::LeftButton) {
            dragging_ = false;
            if (value() != dragStartDays_ && changeCallback_) changeCallback_(value());
        }
    }

private:
    void selectAt(int x) {
        const double position = std::max(0.0, std::min(1.0,
            static_cast<double>(x - 14) / std::max(1, width() - 28)));
        int nearest = 0;
        for (int index = 1; index < retentionOptions().size(); ++index) {
            if (std::abs(retentionOptions()[index].position - position) <
                std::abs(retentionOptions()[nearest].position - position)) nearest = index;
        }
        if (nearest != index_) {
            index_ = nearest;
            update();
        }
    }
    int index_ = 0;
    int dragStartDays_ = 0;
    bool dragging_ = false;
    std::function<void(int)> changeCallback_;
};

QString formatJavaScript(const QString &source) {
    enum class Mode { Code, SingleQuote, DoubleQuote, Template, LineComment, BlockComment, Regex };
    QString output;
    Mode mode = Mode::Code;
    int indentation = 0;
    int parenthesisDepth = 0;
    bool escaped = false;
    bool regexCharacterClass = false;
    auto trimRight = [&] {
        output.remove(QRegularExpression(QStringLiteral("[ \\t]+$")));
    };
    auto appendIndent = [&] {
        if (output.isEmpty() || output.endsWith(QLatin1Char('\n'))) {
            output += QString(indentation * 4, QLatin1Char(' '));
        }
    };
    auto appendNewline = [&] {
        trimRight();
        if (!output.isEmpty() && !output.endsWith(QLatin1Char('\n'))) output += QLatin1Char('\n');
    };
    auto appendSpace = [&] {
        if (!output.isEmpty() && !output.endsWith(QLatin1Char(' '))
            && !output.endsWith(QLatin1Char('\n'))) output += QLatin1Char(' ');
    };
    auto startsRegex = [&] {
        const QString trimmed = output.trimmed();
        if (trimmed.isEmpty()) return true;
        if (QStringLiteral("=([{!?:;,").contains(trimmed.back())) return true;
        for (const QString &keyword : {QStringLiteral("return"), QStringLiteral("case"),
                                      QStringLiteral("throw"), QStringLiteral("typeof"),
                                      QStringLiteral("instanceof")}) {
            if (trimmed.endsWith(keyword)) return true;
        }
        return false;
    };

    for (int index = 0; index < source.size(); ++index) {
        const QChar character = source[index];
        const QChar next = index + 1 < source.size() ? source[index + 1] : QChar();
        if (mode == Mode::SingleQuote || mode == Mode::DoubleQuote || mode == Mode::Template) {
            output += character;
            if (escaped) escaped = false;
            else if (character == QLatin1Char('\\')) escaped = true;
            else if ((mode == Mode::SingleQuote && character == QLatin1Char('\''))
                     || (mode == Mode::DoubleQuote && character == QLatin1Char('"'))
                     || (mode == Mode::Template && character == QLatin1Char('`'))) mode = Mode::Code;
            continue;
        }
        if (mode == Mode::LineComment) {
            if (character == QLatin1Char('\n')) { appendNewline(); mode = Mode::Code; }
            else output += character;
            continue;
        }
        if (mode == Mode::BlockComment) {
            output += character;
            if (character == QLatin1Char('*') && next == QLatin1Char('/')) {
                output += next; ++index; appendNewline(); mode = Mode::Code;
            }
            continue;
        }
        if (mode == Mode::Regex) {
            output += character;
            if (escaped) escaped = false;
            else if (character == QLatin1Char('\\')) escaped = true;
            else if (character == QLatin1Char('[')) regexCharacterClass = true;
            else if (character == QLatin1Char(']')) regexCharacterClass = false;
            else if (character == QLatin1Char('/') && !regexCharacterClass) mode = Mode::Code;
            continue;
        }

        if (character.isSpace()) {
            appendSpace();
        } else if (character == QLatin1Char('\'') || character == QLatin1Char('"')
                   || character == QLatin1Char('`')) {
            appendIndent(); output += character;
            mode = character == QLatin1Char('\'') ? Mode::SingleQuote
                : (character == QLatin1Char('"') ? Mode::DoubleQuote : Mode::Template);
        } else if (character == QLatin1Char('/') && next == QLatin1Char('/')) {
            appendIndent(); output += QStringLiteral("//"); ++index; mode = Mode::LineComment;
        } else if (character == QLatin1Char('/') && next == QLatin1Char('*')) {
            appendIndent(); output += QStringLiteral("/*"); ++index; mode = Mode::BlockComment;
        } else if (character == QLatin1Char('/') && startsRegex()) {
            appendIndent(); output += character; regexCharacterClass = false; mode = Mode::Regex;
        } else if (character == QLatin1Char('{')) {
            trimRight();
            if (!output.isEmpty() && !output.endsWith(QLatin1Char(' '))
                && !output.endsWith(QLatin1Char('\n')) && !output.endsWith(QLatin1Char('('))
                && !output.endsWith(QLatin1Char('['))) output += QLatin1Char(' ');
            appendIndent(); output += character; ++indentation; appendNewline();
        } else if (character == QLatin1Char('}')) {
            trimRight(); if (!output.isEmpty() && !output.endsWith(QLatin1Char('\n'))) appendNewline();
            indentation = std::max(0, indentation - 1); appendIndent(); output += character; appendNewline();
        } else if (character == QLatin1Char(';')) {
            trimRight(); appendIndent(); output += character;
            if (parenthesisDepth == 0) appendNewline();
        } else if (character == QLatin1Char('(')) {
            appendIndent(); output += character; ++parenthesisDepth;
        } else if (character == QLatin1Char(')')) {
            trimRight(); output += character; parenthesisDepth = std::max(0, parenthesisDepth - 1);
        } else if (character == QLatin1Char(',')) {
            trimRight(); output += QStringLiteral(", ");
        } else {
            appendIndent(); output += character;
        }
    }
    return output.trimmed();
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

// Calls back with a widget's new size whenever it is resized.
class ResizeWatcher final : public QObject {
public:
    ResizeWatcher(QWidget *target, std::function<void(const QSize &)> callback)
        : QObject(target), callback_(std::move(callback)) {
        target->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() == QEvent::Resize)
            callback_(static_cast<QResizeEvent *>(event)->size());
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void(const QSize &)> callback_;
};

class CardSyntaxHighlighter final : public QSyntaxHighlighter {
public:
    CardSyntaxHighlighter(QTextDocument *document, QString language)
        : QSyntaxHighlighter(document), language_(std::move(language)), rules_(rulesFor(language_)) {}

protected:
    void highlightBlock(const QString &text) override {
        // Never inherit the platform/editor theme's default foreground. UOS
        // can otherwise leave identifiers and punctuation black on our dark
        // JavaScript editor even while highlighted tokens remain coloured.
        QTextCharFormat baseFormat;
        baseFormat.setForeground(QColor(238, 238, 239));
        setFormat(0, text.size(), baseFormat);
        // Very long lines (minified JSON, logs) would stall typing and
        // scrolling; they keep the base colour only.
        if (text.size() > kMaximumHighlightedLine) return;
        for (const Rule &rule : *rules_) {
            auto matches = rule.expression.globalMatch(text);
            while (matches.hasNext()) {
                const auto match = matches.next();
                setFormat(match.capturedStart(), match.capturedLength(), rule.format);
            }
        }
    }

private:
    struct Rule {
        QRegularExpression expression;
        QTextCharFormat format;
    };
    using Rules = QVector<Rule>;
    static constexpr int kMaximumHighlightedLine = 4000;

    // Compiled once per language and shared: highlightBlock runs for every
    // block of every preview and editor, and compiling the patterns there made
    // opening a long text noticeably slow.
    static std::shared_ptr<const Rules> rulesFor(const QString &language) {
        static QHash<QString, std::shared_ptr<const Rules>> cache;
        const auto cached = cache.constFind(language);
        if (cached != cache.constEnd()) return cached.value();
        auto rules = std::make_shared<Rules>();
        auto add = [&rules](const QString &pattern, const QColor &color,
                            QRegularExpression::PatternOptions options = QRegularExpression::NoPatternOption) {
            QRegularExpression expression(pattern, options);
            expression.optimize();
            QTextCharFormat format;
            format.setForeground(color);
            rules->append({expression, format});
        };
        // Plain text is not coloured (as on macOS); only languages are.
        if (!language.isEmpty() && language != QStringLiteral("text")) {
            QStringList keywords;
            if (language == QStringLiteral("sql")) {
                keywords = QStringList{QStringLiteral("select"), QStringLiteral("from"), QStringLiteral("where"),
                            QStringLiteral("join"), QStringLiteral("insert"), QStringLiteral("update"),
                            QStringLiteral("delete"), QStringLiteral("create"), QStringLiteral("alter"),
                            QStringLiteral("drop"), QStringLiteral("group"), QStringLiteral("order"),
                            QStringLiteral("by"), QStringLiteral("limit"), QStringLiteral("as"),
                            QStringLiteral("and"), QStringLiteral("or"), QStringLiteral("not"),
                            QStringLiteral("into"), QStringLiteral("values"), QStringLiteral("set")};
            } else if (language == QStringLiteral("shell")) {
                keywords = QStringList{QStringLiteral("if"), QStringLiteral("then"), QStringLiteral("else"),
                            QStringLiteral("fi"), QStringLiteral("for"), QStringLiteral("while"),
                            QStringLiteral("do"), QStringLiteral("done"), QStringLiteral("case"),
                            QStringLiteral("esac"), QStringLiteral("function"), QStringLiteral("export"),
                            QStringLiteral("local"), QStringLiteral("return")};
            } else {
                keywords = QStringList{QStringLiteral("function"), QStringLiteral("return"), QStringLiteral("const"),
                            QStringLiteral("let"), QStringLiteral("var"), QStringLiteral("if"),
                            QStringLiteral("else"), QStringLiteral("for"), QStringLiteral("while"),
                            QStringLiteral("class"), QStringLiteral("new"), QStringLiteral("try"),
                            QStringLiteral("catch"), QStringLiteral("throw"), QStringLiteral("import"),
                            QStringLiteral("export"), QStringLiteral("async"), QStringLiteral("await"),
                            QStringLiteral("public"), QStringLiteral("private"), QStringLiteral("static")};
            }
            add(QStringLiteral("\b(?:%1)\b").arg(keywords.join(QLatin1Char('|'))),
                QColor(191, 132, 255), QRegularExpression::CaseInsensitiveOption);
            add(QStringLiteral("\b(?:true|false|null|undefined|nil|NULL)\b"), QColor(191, 132, 255));
            add(QStringLiteral("\b\d+(?:\.\d+)?\b"), QColor(75, 208, 190));
            add(QStringLiteral("(['\"])(?:\\.|(?!\1).)*\1"), QColor(255, 159, 67));
            add(QStringLiteral("//.*|--.*|#(?![# ]).*"), QColor(135, 135, 140));
            if (language == QStringLiteral("json")) {
                add(QStringLiteral("\"(?:\\.|[^\"])*\"(?=\s*:)"), QColor(82, 168, 255));
            } else if (language == QStringLiteral("markdown")) {
                add(QStringLiteral("^#{1,6}\s+.*$"), QColor(82, 168, 255));
                add(QStringLiteral("`[^`]+`|\*\*[^*]+\*\*"), QColor(255, 159, 67));
            }
        }
        std::shared_ptr<const Rules> shared = std::move(rules);
        cache.insert(language, shared);
        return shared;
    }

    QString language_;
    std::shared_ptr<const Rules> rules_;
};

void paintHighlightedText(QPainter &painter, const QRect &rect, const QString &text,
                          const QString &language, const QFont &baseFont) {
    QTextDocument document;
    document.setDocumentMargin(0);
    QFont contentFont = baseFont;
    // Body copy should sit one visual step below the card header.
    contentFont.setPointSizeF(11.0);
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

class ImagePreviewWidget final : public QWidget {
public:
    explicit ImagePreviewWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setMinimumSize(240, 160);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setImage(const QImage &image) {
        image_ = image;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath background;
        background.addRoundedRect(rect().adjusted(0, 0, -1, -1), 10, 10);
        painter.fillPath(background, QColor(21, 21, 23));
        painter.setPen(QPen(QColor(56, 56, 60), 1));
        painter.drawPath(background);
        if (image_.isNull()) return;

        const QRect targetBounds = rect().adjusted(10, 10, -10, -10);
        const QSize fitted = image_.size().scaled(targetBounds.size(), Qt::KeepAspectRatio);
        const QRect target(targetBounds.center().x() - fitted.width() / 2,
                           targetBounds.center().y() - fitted.height() / 2,
                           fitted.width(), fitted.height());
        painter.setClipPath(background);
        painter.drawImage(target, image_, image_.rect());
    }

private:
    QImage image_;
};

enum class AlertTone { Information, Warning, Destructive };

bool showClipDeckAlert(QWidget *parent, const QString &title, const QString &message,
                       const QString &acceptText = QStringLiteral("确定"),
                       const QString &cancelText = QString(),
                       AlertTone tone = AlertTone::Information) {
    QDialog dialog(parent);
    dialog.setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    dialog.setAttribute(Qt::WA_TranslucentBackground, true);
    dialog.setModal(true);
    dialog.setMinimumWidth(430);
    dialog.setMaximumWidth(520);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog{background:transparent;color:#eeeeef;border:0;}"
        "QWidget#alertContent{background:#242427;border:1px solid #4a4a4e;"
        "border-top-left-radius:16px;border-top-right-radius:16px;}"
        "QWidget#alertFooter{background:#101012;border:1px solid #343438;border-top:0;"
        "border-bottom-left-radius:16px;border-bottom-right-radius:16px;}"
        "QLabel#alertTitle{color:#f3f3f4;font-size:18px;font-weight:700;}"
        "QLabel#alertMessage{color:#b8b8bd;font-size:14px;}"
        "QPushButton{background:#3a3a3e;color:#eeeeef;border:0;border-radius:9px;"
        "padding:9px 18px;min-width:72px;}"
        "QPushButton:hover{background:#4a4a4f;}"
        "QPushButton#accept{background:#1683ff;color:white;font-weight:600;}"
        "QPushButton#destructive{background:#c83b45;color:white;font-weight:600;}"));
    QVBoxLayout *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    QWidget *content = new QWidget(&dialog);
    content->setObjectName(QStringLiteral("alertContent"));
    QVBoxLayout *contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(22, 20, 22, 18);
    contentLayout->setSpacing(14);
    QHBoxLayout *heading = new QHBoxLayout;
    QLabel *indicator = new QLabel(QStringLiteral("●"), content);
    indicator->setStyleSheet(tone == AlertTone::Destructive
        ? QStringLiteral("color:#ff5f68;font-size:16px;")
        : tone == AlertTone::Warning
            ? QStringLiteral("color:#ffb44a;font-size:16px;")
            : QStringLiteral("color:#4da3ff;font-size:16px;"));
    QLabel *titleLabel = new QLabel(title, content);
    titleLabel->setObjectName(QStringLiteral("alertTitle"));
    heading->addWidget(indicator);
    heading->addWidget(titleLabel, 1);
    QLabel *messageLabel = new QLabel(message, content);
    messageLabel->setObjectName(QStringLiteral("alertMessage"));
    messageLabel->setWordWrap(true);
    messageLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    contentLayout->addLayout(heading);
    contentLayout->addWidget(messageLabel);
    QWidget *footer = new QWidget(&dialog);
    footer->setObjectName(QStringLiteral("alertFooter"));
    QHBoxLayout *buttons = new QHBoxLayout(footer);
    buttons->setContentsMargins(18, 12, 18, 12);
    buttons->setSpacing(10);
    buttons->addStretch();
    if (!cancelText.isEmpty()) {
        QPushButton *cancel = new QPushButton(cancelText, footer);
        QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
        buttons->addWidget(cancel);
    }
    QPushButton *accept = new QPushButton(acceptText, footer);
    accept->setObjectName(tone == AlertTone::Destructive
        ? QStringLiteral("destructive") : QStringLiteral("accept"));
    accept->setDefault(true);
    QObject::connect(accept, &QPushButton::clicked, &dialog, &QDialog::accept);
    buttons->addWidget(accept);
    layout->addWidget(content);
    layout->addWidget(footer);
    dialog.adjustSize();
    QPainterPath rounded;
    rounded.addRoundedRect(QRectF(dialog.rect().adjusted(0, 0, -1, -1)), 16, 16);
    dialog.setMask(QRegion(rounded.toFillPolygon().toPolygon()));
    return dialog.exec() == QDialog::Accepted;
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

class SearchFieldGlyph final : public QWidget {
public:
    SearchFieldGlyph(bool closeGlyph, QWidget *parent)
        : QWidget(parent), closeGlyph_(closeGlyph) {
        setFocusPolicy(Qt::NoFocus);
        setCursor(Qt::PointingHandCursor);
        setMouseTracking(true);
    }

    void setTriggered(std::function<void()> callback) { triggered_ = std::move(callback); }

protected:
    void paintEvent(QPaintEvent *) override {
        // Paint directly into the Deck backing store. Unlike QLineEdit's
        // QAction/QIcon path this uses the same logical coordinate system as
        // the field, so Qt 5.11 cannot reinterpret a high-DPI pixmap as a
        // larger logical icon or blur it with a second resize.
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const int alpha = hovered_ ? (closeGlyph_ ? 205 : 245)
                                   : (closeGlyph_ ? 155 : 220);
        painter.setPen(QPen(QColor(255, 255, 255, alpha), 1.4,
                            Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (closeGlyph_) {
            painter.setBrush(QColor(255, 255, 255, hovered_ ? 38 : 24));
            painter.drawEllipse(QRectF(6.5, 10.5, 13.0, 13.0));
            painter.drawLine(QPointF(10.5, 14.5), QPointF(15.5, 19.5));
            painter.drawLine(QPointF(15.5, 14.5), QPointF(10.5, 19.5));
        } else {
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(QRectF(6.0, 9.5, 9.5, 9.5));
            painter.drawLine(QPointF(14.2, 17.8), QPointF(18.2, 21.8));
        }
    }

    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) event->accept();
        else QWidget::mousePressEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
            event->accept();
            if (triggered_) triggered_();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void enterEvent(QEvent *event) override {
        hovered_ = true;
        update();
        QWidget::enterEvent(event);
    }

    void leaveEvent(QEvent *event) override {
        hovered_ = false;
        update();
        QWidget::leaveEvent(event);
    }

private:
    bool closeGlyph_ = false;
    bool hovered_ = false;
    std::function<void()> triggered_;
};

// Approximates the macOS behind-window material without the compositor:
// downsample, box-blur three times (close to a gaussian), then scale back up
// so bilinear filtering removes any remaining blockiness.
// Tunable without rebuilding: edit the [material] group in
// ~/.config/ClipDeck/ClipDeck.conf and reopen the Deck.
struct DeckMaterial {
    // Close to the UOS terminal's look: translucent black over the live desktop.
    int opacity = 37;     // percent of black laid over the backdrop
    int blur = 0;         // approximate blur radius in physical pixels, 0 = off

    static DeckMaterial load() {
        QSettings settings;
        settings.beginGroup(QStringLiteral("material"));
        DeckMaterial material;
        auto value = [&settings](const char *key, int fallback, int low, int high) {
            const QString name = QString::fromLatin1(key);
            if (!settings.contains(name)) settings.setValue(name, fallback);
            return qBound(low, settings.value(name, fallback).toInt(), high);
        };
        material.opacity = value("opacity", material.opacity, 0, 100);
        material.blur = value("blur", material.blur, 0, 80);
        settings.endGroup();
        settings.sync();
        return material;
    }

    void save() const {
        QSettings settings;
        settings.beginGroup(QStringLiteral("material"));
        settings.setValue(QStringLiteral("opacity"), opacity);
        settings.setValue(QStringLiteral("blur"), blur);
        settings.endGroup();
        settings.sync();
    }

    QColor tint() const { return QColor(0, 0, 0, qRound(opacity * 2.55)); }
};

QPixmap frostedBackdrop(const QImage &source, const QSize &logicalSize, qreal dpr, int blur) {
    if (source.isNull() || logicalSize.isEmpty()) return {};
    // Three box passes of radius 2 give sigma ~2.45 samples, so the
    // downsample factor sets the blur radius in source pixels.
    const int downsample = std::max(1, qRound(blur / 2.45));
    const int radius = std::min(blur, 2);
    QImage small = source.convertToFormat(QImage::Format_RGB32).scaled(
        std::max(1, source.width() / downsample), std::max(1, source.height() / downsample),
        Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const int w = small.width();
    const int h = small.height();
    std::vector<QRgb> buffer(static_cast<size_t>(w) * h);
    std::vector<QRgb> scratch(buffer.size());
    for (int y = 0; y < h; ++y) {
        const auto *line = reinterpret_cast<const QRgb *>(small.constScanLine(y));
        std::copy(line, line + w, buffer.begin() + static_cast<ptrdiff_t>(y) * w);
    }
    auto boxPass = [&](const std::vector<QRgb> &in, std::vector<QRgb> &out, bool horizontal) {
        const int outer = horizontal ? h : w;
        const int inner = horizontal ? w : h;
        for (int o = 0; o < outer; ++o) {
            auto at = [&](int i) {
                i = qBound(0, i, inner - 1);
                return in[horizontal ? static_cast<size_t>(o) * w + i : static_cast<size_t>(i) * w + o];
            };
            int r = 0, g = 0, b = 0;
            for (int i = -radius; i <= radius; ++i) {
                const QRgb c = at(i);
                r += qRed(c); g += qGreen(c); b += qBlue(c);
            }
            const int span = radius * 2 + 1;
            for (int i = 0; i < inner; ++i) {
                out[horizontal ? static_cast<size_t>(o) * w + i : static_cast<size_t>(i) * w + o]
                    = qRgb(r / span, g / span, b / span);
                const QRgb add = at(i + radius + 1);
                const QRgb drop = at(i - radius);
                r += qRed(add) - qRed(drop);
                g += qGreen(add) - qGreen(drop);
                b += qBlue(add) - qBlue(drop);
            }
        }
    };
    for (int pass = 0; pass < 3; ++pass) {
        boxPass(buffer, scratch, true);
        boxPass(scratch, buffer, false);
    }
    for (int y = 0; y < h; ++y)
        std::copy(buffer.begin() + static_cast<ptrdiff_t>(y) * w,
                  buffer.begin() + static_cast<ptrdiff_t>(y + 1) * w,
                  reinterpret_cast<QRgb *>(small.scanLine(y)));
    QPixmap result = QPixmap::fromImage(small.scaled(
        DisplayScale::rasterSize(logicalSize, dpr), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    result.setDevicePixelRatio(dpr);
    return result;
}

int normalizedImageAngle(int angle) {
    return ((angle % 360) + 360) % 360;
}

QImage imageAtAngle(const QImage &image, int angle) {
    angle = normalizedImageAngle(angle);
    if (image.isNull() || angle == 0) return image;
    QTransform transform;
    transform.rotate(angle);
    return image.transformed(transform, Qt::SmoothTransformation);
}

QSize imageSizeAtAngle(const QSize &size, int angle) {
    angle = normalizedImageAngle(angle);
    return angle == 90 || angle == 270 ? QSize(size.height(), size.width()) : size;
}

QSize imagePreviewWindowSize(const QSize &imageSize, const QSize &availableSize) {
    const int maximumWidth = std::max(1, std::min(680, availableSize.width() - 16));
    const int maximumHeight = std::max(1, std::min(500, availableSize.height() - 16));
    if (imageSize.isEmpty()) return QSize(std::min(560, maximumWidth),
                                         std::min(360, maximumHeight));

    // Match the original preview's image area, then fit its aspect ratio into
    // bounded dimensions. Pixel count does not affect the window's size.
    // ImagePreviewWidget has 10px padding; the header consumes another 58px.
    const double aspect = imageSize.width() / static_cast<double>(imageSize.height());
    const double area = 540.0 * 282.0;
    double imageWidth = std::sqrt(area * aspect);
    double imageHeight = imageWidth / aspect;
    const double scale = std::min(1.0, std::min(
        std::max(1, maximumWidth - 20) / imageWidth,
        std::max(1, maximumHeight - 78) / imageHeight));
    imageWidth *= scale;
    imageHeight *= scale;
    // Extreme panoramas still need a readable title and a useful body area.
    // The image remains aspect-fitted inside these minimum dimensions.
    const int width = std::max(std::min(260, maximumWidth), qRound(imageWidth) + 20);
    const int height = std::max(std::min(180, maximumHeight), qRound(imageHeight) + 78);
    return QSize(std::min(width, maximumWidth), std::min(height, maximumHeight));
}

QIcon imageRotationIcon(bool clockwise) {
    const qreal dpr = DisplayScale::factor();
    QPixmap pixmap(DisplayScale::rasterSize(QSize(28, 28), dpr));
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(dpr);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(QColor(242, 242, 244, 225), 1.8,
                        Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawArc(QRectF(6.5, 6.5, 15, 15), clockwise ? 35 * 16 : 145 * 16, 275 * 16);
    QPainterPath arrow;
    if (clockwise) {
        arrow.moveTo(21.2, 5.8);
        arrow.lineTo(21.7, 11.0);
        arrow.lineTo(16.7, 9.2);
    } else {
        arrow.moveTo(6.8, 5.8);
        arrow.lineTo(6.3, 11.0);
        arrow.lineTo(11.3, 9.2);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(242, 242, 244, 225));
    painter.drawPath(arrow);
    return QIcon(pixmap);
}

class DeckWindow final : public QWidget {
    struct PasteRequest {
        quint64 generation = 0;
        quint64 clipboardRevision = 0;
        bool isImage = false;
        QString text;
        QImage image;
        QVector<ClipboardFileReference> files;
    };
    using PasteRequestPtr = std::shared_ptr<PasteRequest>;

public:
    explicit DeckWindow(Display *display, HistoryRepository *repository, QWidget *parent = nullptr,
                        ClipboardPipeline *pipeline = nullptr)
        : QWidget(parent), display_(display), repository_(repository), pipeline_(pipeline) {
        setWindowTitle(QStringLiteral("ClipDeck"));
        setWindowFlags(Qt::FramelessWindowHint | Qt::Tool | Qt::WindowStaysOnTopHint
                       | Qt::NoDropShadowWindowHint | Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_X11DoNotAcceptFocus, true);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAutoFillBackground(false);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        filePaste_ = new FilePasteController(this);
        filePaste_->changed = [this] { update(); };
        QObject::connect(QApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
            ++clipboardRevision_;
            if (!writingHistoryClipboard_ && filePaste_->feedback().phase != FilePasteFeedback::Idle)
                filePaste_->cancel();
        });
        searchEdit_ = new QLineEdit(this);
        searchEdit_->setPlaceholderText(QStringLiteral("搜索文本"));
        searchEdit_->setClearButtonEnabled(false);
        searchEdit_->setVisible(false);
        searchEdit_->setStyleSheet(QStringLiteral(
            "QLineEdit{color:white;background:rgba(62,62,66,220);border:0;"
            "border-radius:17px;padding:5px 8px;font-size:14px;font-weight:500;outline:0;"
            "selection-background-color:rgba(100,100,106,150);selection-color:white;}"
            "QLineEdit:!focus{background:rgba(62,62,66,220);border:0;}"
            "QLineEdit:focus{color:white;background:rgba(68,68,72,232);border:0;"
            "selection-background-color:#1683ff;}"));
        QPalette searchPalette = searchEdit_->palette();
        searchPalette.setColor(QPalette::Text, QColor(255, 255, 255, 235));
        searchEdit_->setPalette(searchPalette);
        searchEdit_->setTextMargins(27, 0, 27, 0);
        searchDebounce_ = new QTimer(this);
        searchDebounce_->setSingleShot(true);
        searchDebounce_->setInterval(90);
        QObject::connect(searchDebounce_, &QTimer::timeout, this, [this] { reload(); });
        blurUpdateTimer_ = new QTimer(this);
        blurUpdateTimer_->setSingleShot(true);
        blurUpdateTimer_->setInterval(24);
        QObject::connect(blurUpdateTimer_, &QTimer::timeout, this, [this] {
            if (isVisible() && !resizing_) enableKWinBlur();
        });
        resizeReleaseTimer_ = new QTimer(this);
        resizeReleaseTimer_->setInterval(16);
        QObject::connect(resizeReleaseTimer_, &QTimer::timeout, this, [this] {
            if (resizing_ && !(QApplication::mouseButtons() & Qt::LeftButton)) {
                finishLiveResize();
            }
        });
        resizeFrameTimer_ = new QTimer(this);
        resizeFrameTimer_->setSingleShot(true);
        resizeFrameTimer_->setInterval(8);
        QObject::connect(resizeFrameTimer_, &QTimer::timeout, this, [this] {
            applyPendingResizeGeometry();
        });
        outsideClickTimer_ = new QTimer(this);
        outsideClickTimer_->setInterval(20);
        QObject::connect(outsideClickTimer_, &QTimer::timeout, this, [this] {
            if (!display_ || !isVisible()) return;
            Window root = None;
            Window child = None;
            int rootX = 0;
            int rootY = 0;
            int windowX = 0;
            int windowY = 0;
            unsigned int mask = 0;
            if (!XQueryPointer(display_, DefaultRootWindow(display_), &root, &child,
                               &rootX, &rootY, &windowX, &windowY, &mask)) return;
            const unsigned int buttons = mask & (Button1Mask | Button2Mask | Button3Mask);
            const bool newlyPressed = (buttons & ~lastPointerButtonMask_) != 0;
            lastPointerButtonMask_ = buttons;
            // XQueryPointer returns native X pixels while QWindow geometry is
            // expressed in Qt logical coordinates. Comparing them directly
            // misclassifies clicks near the search action at 125% scaling.
            // QCursor performs the platform scale conversion for us.
            const QPoint localPointer = mapFromGlobal(QCursor::pos());
            if (!newlyPressed || rect().contains(localPointer)) return;
            // Clicks in the Deck's own popovers (preview, editor, rename,
            // alerts, menus) are not outside clicks; hiding here would also
            // close those child windows before their buttons fire.
            if (QApplication::activeModalWidget() || QApplication::activePopupWidget()) return;
            for (QWidget *window : QApplication::topLevelWidgets()) {
                if (window != this && window->isVisible()
                        && window->frameGeometry().contains(QCursor::pos())) return;
            }
            hide();
        });
        searchLeadingGlyph_ = new SearchFieldGlyph(false, searchEdit_);
        searchLeadingGlyph_->setTriggered([this] {
            searchEdit_->setFocus(Qt::MouseFocusReason);
        });
        searchCloseGlyph_ = new SearchFieldGlyph(true, searchEdit_);
        searchCloseGlyph_->setTriggered([this] { endSearch(); });
        searchEdit_->installEventFilter(this);
        QObject::connect(searchEdit_, &QLineEdit::textChanged, this, [this](const QString &value) {
            searchQuery_ = value.trimmed();
            if (searchActive_) searchDebounce_->start();
        });
        QObject::connect(searchEdit_, &QLineEdit::returnPressed, this, [this] {
            setFocus(Qt::ShortcutFocusReason);
            pasteSelected(false);
        });
        searchAnimation_ = new QVariantAnimation(this);
        searchAnimation_->setDuration(140);
        searchAnimation_->setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(searchAnimation_, &QVariantAnimation::valueChanged, this,
                         [this](const QVariant &value) {
            searchExpansion_ = value.toReal();
            updateSearchGeometry();
            update();
        });
        QObject::connect(searchAnimation_, &QVariantAnimation::finished, this, [this] {
            if (!searchActive_ && searchExpansion_ <= 0.001) searchEdit_->hide();
        });
        categoryAnimation_ = new QVariantAnimation(this);
        categoryAnimation_->setDuration(180);
        categoryAnimation_->setEasingCurve(QEasingCurve::OutCubic);
        QObject::connect(categoryAnimation_, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) { categoryScrollOffset_ = value.toInt(); update(); });
        if (pipeline_) {
            QObject::connect(pipeline_, &ClipboardPipeline::recentChanged, this,
                             [this](quint64 sequence) { recentHistoryChanged(sequence); });
        }
        refreshCategoryValues();
        reload();
        QTimer::singleShot(0, this, [this] {
            const int count = std::min(20, static_cast<int>(items_.size()));
            for (int index = 0; index < count; ++index) fetchLinkMetadataIfNeeded(items_[index]);
        });
    }

    void toggleNearPointer() {
        if (isVisible()) {
            hide();
            return;
        }
        showNearPointer();
    }

    void showNearPointer() {
        ++pasteRequestGeneration_;
        if (pipeline_) {
            pipeline_->setDeckVisible(true);
            if (selectedCategory_ == QStringLiteral("History") && searchQuery_.isEmpty())
                applyHistoryItems(pipeline_->recentItems(), selectedDatabaseID(), 10);
        }
        rememberPasteTarget();
        lastCardPressID_ = -1;
        lastCardPressAt_ = 0;
        pendingMousePasteID_ = -1;
        quickPasteOverlay_ = false;
        nonActivatingPresentation_ = true;

        QDesktopWidget *desktop = QApplication::desktop();
        const int screenNumber = desktop->screenNumber(QCursor::pos());
        const QRect available = desktop->availableGeometry(screenNumber);
        const int maximumHeight = std::max(250, std::min(420, static_cast<int>(available.height() * 0.42)));
        QSettings settings;
        const int preferred = settings.value(QStringLiteral("deck/preferredHeight"),
            std::max(340, static_cast<int>(available.height() * 0.385))).toInt();
        const int height = std::min(maximumHeight, std::max(250, preferred));

        const QRect deckRect(
            available.x() + kHorizontalInset,
            available.y() + available.height() - height,
            std::max(320, available.width() - kHorizontalInset * 2),
            height);
        // Capture what sits behind the Deck before it appears.  Cover the
        // tallest resize height so dragging the top edge never runs past it.
        if (!isVisible()) {
            const QRect band(deckRect.x(), deckRect.bottom() + 1 - maximumHeight,
                             deckRect.width(), maximumHeight);
            material_ = DeckMaterial::load();
            captureBackdrop(band);
        }
        setGeometry(deckRect);
        clampScrollOffset();
        show();
        raise();
        if (pipeline_) reload();
        // Keep the source application's X input focus untouched. The keyboard
        // grab routes Deck navigation keys here without causing browser DOM
        // fields (for example online Excel search) to blur.
        setFocus(Qt::ShortcutFocusReason);
        setDeckInputHint(false);
        grabDeckKeyboard(0);
        lastPointerButtonMask_ = 0;
        if (outsideClickTimer_) outsideClickTimer_->start();
    }

    void pasteSelected(bool forcePlainText = false, bool quickPaste = false) {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) {
            return;
        }

        if (ensureSelectedPayload([this, forcePlainText, quickPaste] {
                pasteSelected(forcePlainText, quickPaste);
            }, true)) return;
        const ClipboardItem selectedItem = items_[selectedIndex_];
        if (selectedItem.isFiles()) {
            const auto &feedback = filePaste_->feedback();
            if (feedback.itemID == selectedItem.databaseID && feedback.phase == FilePasteFeedback::Checking) return;
            const quint64 revision = clipboardRevision_;
            const quint64 generation = ++pasteRequestGeneration_;
            const qint64 id = selectedItem.databaseID;
            filePaste_->prepare(selectedItem,
                [this, id, revision, generation] {
                    return isVisible() && selectedDatabaseID() == id && clipboardRevision_ == revision
                        && generation == pasteRequestGeneration_;
                }, [this, forcePlainText, quickPaste](ClipboardItem item) { pasteItem(item, forcePlainText, quickPaste); });
            return;
        }
        pasteItem(selectedItem, forcePlainText, quickPaste);
    }

    void pasteItem(const ClipboardItem &selectedItem, bool forcePlainText, bool quickPaste) {
        const auto request = std::make_shared<PasteRequest>();
        request->generation = ++pasteRequestGeneration_;
        request->isImage = selectedItem.isImage();
        request->text = selectedItem.text;
        request->files = selectedItem.files;
        {
            // Only this explicit history action may replace CLIPBOARD. Do not
            // suppress external copies/cuts for an arbitrary time afterwards.
            QScopedValueRollback<bool> writing(writingHistoryClipboard_, true);
            if (selectedItem.isFiles()) {
                QApplication::clipboard()->setMimeData(FileReferences::mimeData(selectedItem.files));
            } else if (selectedItem.isImage()) {
                request->image = imageAtAngle(selectedItem.image, selectedItem.imageAngle);
                QApplication::clipboard()->setImage(request->image, QClipboard::Clipboard);
            } else {
                QSettings settings;
                const bool preserveFormatting = !forcePlainText && settings.value(
                    QStringLiteral("paste/preserveFormatting"), false).toBool();
                if (preserveFormatting && !selectedItem.richTextHtml.isEmpty()) {
                    auto *mimeData = new QMimeData;
                    mimeData->setText(selectedItem.text);
                    mimeData->setHtml(selectedItem.richTextHtml);
                    QApplication::clipboard()->setMimeData(mimeData, QClipboard::Clipboard);
                } else {
                    QApplication::clipboard()->setText(selectedItem.text, QClipboard::Clipboard);
                }
            }
        }
        request->clipboardRevision = clipboardRevision_;
        // In non-activating presentation the source application's input
        // focus is intentionally left untouched. Remember that state before
        // hideEvent clears it, and synchronously release the X keyboard grab
        // before injecting Ctrl+V. Otherwise the synthetic paste keystroke can
        // still be delivered back to Deck itself.
        // The focus-preservation contract starts as soon as Deck is shown;
        // mouse activation may select a card before the asynchronous keyboard
        // grab succeeds, so it must not depend on keyboardGrabbed_.
        const bool preservedSourceFocus = nonActivatingPresentation_;
        releaseDeckKeyboard();
        hide();

        if (!display_ || pasteTarget_ == None) {
            return;
        }

        const Window target = pasteTarget_;
        QTimer::singleShot(20, this,
                           [this, target, request, quickPaste, forcePlainText, preservedSourceFocus] {
            if (!pasteRequestIsCurrent(request)) return;
            const bool heldControlPaste = quickPaste && configuredModifier(
                QStringLiteral("shortcuts/quickPasteModifier"), Qt::ControlModifier)
                    == Qt::ControlModifier;
            if (heldControlPaste) {
                if (preservedSourceFocus) {
                    pasteUsingPreservedFocus(target, request, true);
                } else {
                    activateTarget(target);
                    restorePasteWidgetFocus(target);
                    waitForTargetAndPaste(target, request, 0, true);
                }
            } else if (forcePlainText && configuredModifier(
                           QStringLiteral("shortcuts/plainTextModifier"), Qt::ShiftModifier)
                           == Qt::ShiftModifier) {
                // Shift+Enter used to wait until Shift was physically released,
                // making plain-text paste noticeably slower. Restore focus at
                // once and temporarily suppress Shift only around synthetic
                // Ctrl+V; the clipboard payload is already plain text.
                if (preservedSourceFocus) {
                    pasteUsingPreservedFocus(target, request, false, true);
                } else {
                    activateTarget(target);
                    restorePasteWidgetFocus(target);
                    waitForTargetAndPaste(target, request, 0, false, true);
                }
            } else {
                waitForPasteModifiersReleased(target, request, 0, preservedSourceFocus);
            }
        });
    }

    bool isWritingHistoryClipboard() const { return writingHistoryClipboard_; }

    const DeckMaterial &material() const { return material_; }
    void setMaterial(const DeckMaterial &material) {
        material_ = material;
        material_.save();
        rebuildBackdrop();
        update();
    }

    void refresh() {
        if (pipeline_) pipeline_->refreshRecent();
        refreshCategoryValues();
        reload();
    }
    void beginSearchForRenderCheck() {
        search();
        if (searchAnimation_) searchAnimation_->stop();
        searchExpansion_ = 1.0;
        updateSearchGeometry();
        update();
    }
    void showNextCategoryPageForRenderCheck() { changeCategoryPage(1); }
    void setSettingsCallback(std::function<void(int)> callback) {
        settingsCallback_ = std::move(callback);
    }

    bool handleNativeQuickPaste(unsigned int keycode, unsigned int state) {
        if (!isVisible() || !display_) return false;
        const KeySym base = XkbKeycodeToKeysym(
            display_, static_cast<KeyCode>(keycode), 0, 0);
        if (base < XK_1 || base > XK_9) return false;
        const unsigned int relevant = state & (ShiftMask | ControlMask | Mod1Mask | Mod4Mask);
        const unsigned int configured = x11MaskForQtModifiers(configuredModifier(
            QStringLiteral("shortcuts/quickPasteModifier"), Qt::ControlModifier));
        if (relevant != configured) return false;
        const int index = static_cast<int>(base - XK_1);
        if (index < static_cast<int>(items_.size())) {
            selectedIndex_ = index;
            quickPasteOverlay_ = false;
            const qint64 itemID = items_[index].databaseID;
            if (filePaste_->feedback().itemID == itemID
                && filePaste_->feedback().phase == FilePasteFeedback::Checking) return true;
            const quint64 revision = clipboardRevision_;
            const quint64 generation = ++pasteRequestGeneration_;
            QTimer::singleShot(0, this, [this, itemID, revision, generation] {
                if (!isVisible() || revision != clipboardRevision_
                        || generation != pasteRequestGeneration_) return;
                for (int selected = 0; selected < static_cast<int>(items_.size()); ++selected) {
                    if (items_[selected].databaseID != itemID) continue;
                    selectedIndex_ = selected;
                    pasteSelected(false, true);
                    break;
                }
            });
        }
        return true;
    }

protected:
    void enterEvent(QEvent *event) override {
        setCursor(mapFromGlobal(QCursor::pos()).y() < 18 ? Qt::SizeVerCursor : Qt::ArrowCursor);
        QWidget::enterEvent(event);
    }
    void leaveEvent(QEvent *event) override {
        if (!resizing_) unsetCursor();
        QWidget::leaveEvent(event);
    }
    bool event(QEvent *event) override {
        if(event->type()==QEvent::ToolTip) {
            auto *help=static_cast<QHelpEvent *>(event);
            QString tooltip;
            const QString labels[]={QStringLiteral("History"),QStringLiteral("文本"),QStringLiteral("图片"),QStringLiteral("文件")};
            for(int index=0;index<4;++index) if(fixedTypeRect(index).contains(help->pos())) tooltip=labels[index];
            if(customStripRect().contains(help->pos()))
                for(int index=4;index<categoryValues().size();++index)
                    if(categoryRect(index-4).contains(help->pos())) tooltip=categoryValues()[index];
            const int index=cardIndexAt(help->pos());
            if(index>=0 && items_[index].isFiles()) {
                const auto &files=items_[index].files;
                const QRect content=fileContentRect(index);
                if(files.size()==1) tooltip=files.first().path;
                else if(content.contains(help->pos())) {
                    const int row=(help->pos().y()-content.y()-12)/30;
                    if(row>=0 && row<files.size()) tooltip=files[row].path;
                }
            }
            if(!tooltip.isEmpty()) QToolTip::showText(help->globalPos(),tooltip,this);
            else QToolTip::hideText();
            event->accept();return true;
        }
        return QWidget::event(event);
    }
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        enableKWinBlur();
    }

    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        // WA_NoSystemBackground leaves the translucent backing store intact.
        // Clear it explicitly so live top-edge resizing cannot retain pixels
        // from older window geometries as horizontal ghost lines.
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setRenderHint(QPainter::TextAntialiasing);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, !resizing_);

        QPainterPath deckPath;
        deckPath.addRoundedRect(QRectF(0, 0, width(), height()), 24, 24);
        // Material: an optional frosted snapshot of what is behind the Deck,
        // then black at the configured opacity (the UOS terminal uses 60%).
        // Without the snapshot the compositor shows the live desktop through
        // the same tint.  Cards stay fully opaque either way.
        if (!backdrop_.isNull()) {
            painter.save();
            painter.setClipPath(deckPath);
            painter.drawPixmap(backdropOrigin_ - mapToGlobal(QPoint(0, 0)), backdrop_);
            painter.restore();
        }
        painter.fillPath(deckPath, material_.tint());

        QLinearGradient materialSheen(0, 0, 0, height());
        materialSheen.setColorAt(0, QColor(255, 255, 255, 15));
        materialSheen.setColorAt(0.50, QColor(255, 255, 255, 0));
        materialSheen.setColorAt(1, QColor(0, 0, 0, 15));
        painter.fillPath(deckPath, materialSheen);
        // The faint light rim macOS draws around dark translucent windows.
        painter.setPen(QPen(QColor(255, 255, 255, 22), 1));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 23.5, 23.5);
        painter.setPen(Qt::NoPen);

        paintHeader(painter);
        if (items_.empty()) {
            paintEmptyState(painter);
        } else {
            paintCards(painter);
        }
    }

    void resizeEvent(QResizeEvent *event) override {
        QWidget::resizeEvent(event);
        if(categoryAnimation_) categoryAnimation_->stop();
        categoryScrollOffset_=qBound(0,categoryScrollOffset_,maximumCategoryScroll());
        updateSearchGeometry();
        // XShape + KWin blur property updates are synchronous X11 operations.
        // Coalesce ordinary resizes and never perform them for every live-drag
        // frame; doing so produced the stacked lines above the Deck on UOS.
        if (isVisible() && !resizing_ && blurUpdateTimer_) blurUpdateTimer_->start();
    }

    int quickPasteIndexForEvent(const QKeyEvent *event) const {
        if (event->key() >= Qt::Key_1 && event->key() <= Qt::Key_9) {
            return event->key() - Qt::Key_1;
        }
        if (display_ && event->nativeScanCode() != 0) {
            const KeySym base = XkbKeycodeToKeysym(
                display_, static_cast<KeyCode>(event->nativeScanCode()), 0, 0);
            if (base >= XK_1 && base <= XK_9) return static_cast<int>(base - XK_1);
        }
        // Fallback for platforms that do not expose the X11 scan code.
        const int shiftedDigits[] = {Qt::Key_Exclam, Qt::Key_At, Qt::Key_NumberSign,
            Qt::Key_Dollar, Qt::Key_Percent, Qt::Key_AsciiCircum, Qt::Key_Ampersand,
            Qt::Key_Asterisk, Qt::Key_ParenLeft};
        for (int index = 0; index < 9; ++index) {
            if (event->key() == shiftedDigits[index]) return index;
        }
        return -1;
    }

    bool eventFilter(QObject *watched, QEvent *event) override {
        if (previewDialog_ && previewKeyHandler_
                && (watched == previewDialog_ || previewDialog_->isAncestorOf(
                        qobject_cast<QWidget *>(watched)))) {
            if (event->type() == QEvent::ShortcutOverride) {
                event->accept();
                return true;
            }
            if (event->type() == QEvent::KeyPress) {
                previewKeyHandler_(static_cast<QKeyEvent *>(event));
                return true;
            }
        }
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
            const Qt::KeyboardModifiers modifiers = keyEvent->modifiers()
                & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier | Qt::MetaModifier);
            const Qt::KeyboardModifiers quickModifier = configuredModifier(
                QStringLiteral("shortcuts/quickPasteModifier"), Qt::ControlModifier);
            if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
                setFocus(Qt::ShortcutFocusReason);
                pasteSelected(modifiers == configuredModifier(
                    QStringLiteral("shortcuts/plainTextModifier"), Qt::ShiftModifier));
                return true;
            }
            const bool quickNumber = modifiers == quickModifier
                && quickPasteIndexForEvent(keyEvent) >= 0;
            // Once a query exists, bare arrows browse the filtered cards while
            // the editor keeps focus. Modified arrows remain text navigation.
            if (!searchEdit_->text().isEmpty() && modifiers == Qt::NoModifier
                && (keyEvent->key() == Qt::Key_Left
                    || keyEvent->key() == Qt::Key_Right)) {
                moveSelection(keyEvent->key() == Qt::Key_Left ? -1 : 1);
                return true;
            }
            const bool deckShortcut = isSettingsShortcut(keyEvent);
            if (quickNumber || deckShortcut || modifierForKey(keyEvent->key()) == quickModifier) {
                keyPressEvent(keyEvent);
                return true;
            }
        }
        if (watched == searchEdit_ && event->type() == QEvent::KeyRelease) {
            auto *keyEvent = static_cast<QKeyEvent *>(event);
            if (modifierForKey(keyEvent->key()) != Qt::NoModifier) {
                keyReleaseEvent(keyEvent);
                return true;
            }
        }
        return QWidget::eventFilter(watched, event);
    }

    void closeEvent(QCloseEvent *event) override {
        event->ignore();
        hide();
    }

    void hideEvent(QHideEvent *event) override {
        filePaste_->cancel();
        pendingFileConfirmID_=-1;
        if (pipeline_) {
            pipeline_->setDeckVisible(false);
            ++historyQueryGeneration_;
            ++payloadRequestGeneration_;
            const qint64 selectedID = selectedDatabaseID();
            applyHistoryItems(selectedCategory_ == QStringLiteral("History") && searchQuery_.isEmpty()
                ? pipeline_->recentItems() : QVector<ClipboardItem>{}, selectedID, 10);
        }
        lastCardPressID_ = -1;
        lastCardPressAt_ = 0;
        pendingMousePasteID_ = -1;
        if (previewDialog_) previewDialog_->reject();
        releaseDeckKeyboard();
        if (outsideClickTimer_) outsideClickTimer_->stop();
        nonActivatingPresentation_ = false;
        quickPasteOverlay_ = false;
        QWidget::hideEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override {
        if (previewKeyHandler_) {
            previewKeyHandler_(event);
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            if (filePaste_->feedback().phase != FilePasteFeedback::Idle) { filePaste_->cancel(); return; }
            if (searchActive_) endSearch();
            else hide();
            return;
        }
        if (isSettingsShortcut(event)) {
            if (settingsCallback_) settingsCallback_(0);
            return;
        }
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            pasteSelected(standardKeyboardModifiers(event->modifiers()) == configuredModifier(
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
        if (event->modifiers() == Qt::ControlModifier
            && event->key() == Qt::Key_BracketLeft) {
            rotateSelected(-90);
            return;
        }
        if (event->modifiers() == Qt::ControlModifier
            && event->key() == Qt::Key_BracketRight) {
            rotateSelected(90);
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
        const int quickPasteIndex = quickPasteIndexForEvent(event);
        if (standardKeyboardModifiers(event->modifiers()) == quickPasteModifier
            && quickPasteIndex >= 0) {
            const int index = quickPasteIndex;
            if (index < static_cast<int>(items_.size())) {
                selectedIndex_ = index;
                quickPasteOverlay_ = false;
                pasteSelected(false, true);
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
        pendingFileConfirmID_=-1;
        pendingMousePasteID_ = -1;
        if (previewDialog_) previewDialog_->reject();
        if (event->button() != Qt::LeftButton) {
            lastCardPressID_ = -1;
            return QWidget::mousePressEvent(event);
        }
        if (cardIndexAt(event->pos()) < 0) lastCardPressID_ = -1;
        if (event->pos().y() < 18) {
            resizing_ = true;
            if (blurUpdateTimer_) blurUpdateTimer_->stop();
            suspendKWinBlur();
            resizeStartGlobalY_ = event->globalY();
            resizeStartGeometry_ = geometry();
            pendingResizeHeight_ = height();
            // The pointer immediately leaves the frameless window when its top
            // edge is dragged upward. Keep receiving moves until release.
            grabMouse(Qt::SizeVerCursor);
            if (resizeReleaseTimer_) resizeReleaseTimer_->start();
            return;
        }
        if (event->pos().y() < 60) {
            const QStringList &categories = categoryValues();
            for (int index = 0; index < 4; ++index)
                if (fixedTypeRect(index).contains(event->pos())) { selectCategory(categories[index]); return; }
            if (customStripRect().contains(event->pos()))
                for (int index=4;index<categories.size();++index)
                    if (categoryRect(index-4).contains(event->pos())) { selectCategory(categories[index]); return; }
            if (searchRect().contains(event->pos())) {
                search();
                return;
            }
            if (previousCategoryRect().contains(event->pos())) { changeCategoryPage(-1); return; }
            if (nextCategoryRect().contains(event->pos())) { changeCategoryPage(1); return; }
            if (addRect().contains(event->pos()) && settingsCallback_) {
                settingsCallback_(5); // Category page in new-category mode.
                return;
            }
            if (moreRect().contains(event->pos()) && settingsCallback_) {
                settingsCallback_(3);
                return;
            }
        }
        const int index = cardIndexAt(event->pos());
        if (index >= 0) {
            if (selectedIndex_ != index) filePaste_->cancel();
            const auto &feedback = filePaste_->feedback();
            if (feedback.itemID == items_[index].databaseID && feedback.phase == FilePasteFeedback::Partial) {
                const QRect area = fileFeedbackRect(fileContentRect(index), feedback);
                const QRect cancel(area.x()+12, area.bottom()-42,52,30);
                const QRect confirm(cancel.right()+8,cancel.y(),std::min(110,area.width()-84),30);
                if (cancel.contains(event->pos())) { filePaste_->cancel(); lastCardPressID_=-1; return; }
                if (confirm.contains(event->pos())) {
                    pendingFileConfirmID_=items_[index].databaseID;
                    pendingFileConfirmRevision_=clipboardRevision_;
                    lastCardPressID_=-1; return;
                }
            }
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            const qint64 cardID = items_[index].databaseID >= 0
                ? items_[index].databaseID : index;
            const bool fallbackDoubleClick = lastCardPressID_ == cardID
                && now - lastCardPressAt_ <= QApplication::doubleClickInterval()
                && (event->globalPos() - lastCardPressGlobalPos_).manhattanLength()
                    <= QApplication::startDragDistance();
            selectedIndex_ = index;
            if (searchActive_) {
                searchEdit_->clearFocus();
                setFocus(Qt::MouseFocusReason);
            }
            if (fallbackDoubleClick) {
                armMousePaste(index, event);
                return;
            }
            lastCardPressID_ = cardID;
            lastCardPressAt_ = now;
            lastCardPressGlobalPos_ = event->globalPos();
            // A mouse-selected card is already under the pointer. Moving the
            // row here (even just enough to reveal a clipped edge) changes the
            // hit target between the two presses of a double click.
            update();
            maybeLoadMoreImages();
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
            pendingResizeHeight_ = newHeight;
            if (resizeFrameTimer_ && !resizeFrameTimer_->isActive()) resizeFrameTimer_->start();
            return;
        }
        setCursor(event->pos().y() < 18 ? Qt::SizeVerCursor : Qt::ArrowCursor);
        QWidget::mouseMoveEvent(event);
    }

    void mouseReleaseEvent(QMouseEvent *event) override {
        if(event->button()==Qt::LeftButton && pendingFileConfirmID_>=0) {
            const qint64 id=pendingFileConfirmID_;
            const quint64 revision=pendingFileConfirmRevision_;
            pendingFileConfirmID_=-1;
            const auto &feedback=filePaste_->feedback();
            if(selectedDatabaseID()==id && feedback.phase==FilePasteFeedback::Partial) {
                const QRect area=fileFeedbackRect(fileContentRect(selectedIndex_),feedback);
                const QRect confirm(area.x()+71,area.bottom()-42,std::min(110,area.width()-84),30);
                if(confirm.contains(event->pos())) QTimer::singleShot(0,this,[this,id,revision] {
                    if(isVisible() && selectedDatabaseID()==id && clipboardRevision_==revision
                        && filePaste_->feedback().phase==FilePasteFeedback::Partial) pasteSelected();
                });
            }
            event->accept();return;
        }
        if (resizing_ && event->button() == Qt::LeftButton) {
            finishLiveResize();
            return;
        }
        if (event->button() == Qt::LeftButton && pendingMousePasteID_ >= 0) {
            const qint64 itemID = pendingMousePasteID_;
            const bool plainText = pendingMousePasteModifiers_ == configuredModifier(
                QStringLiteral("shortcuts/plainTextModifier"), Qt::ShiftModifier);
            pendingMousePasteID_ = -1;
            const int index = cardIndexAt(event->pos());
            if (index >= 0 && items_[index].databaseID == itemID
                    && (event->globalPos() - pendingMousePastePosition_).manhattanLength()
                        <= QApplication::startDragDistance()) {
                // Qt releases its implicit pointer grab after this handler.
                // Keep Deck mapped through button-up, then paste on the next
                // event-loop turn so neither the click nor Ctrl+V is rerouted
                // while button 1 is still held.
                if (filePaste_->feedback().itemID == itemID
                    && filePaste_->feedback().phase == FilePasteFeedback::Checking) {
                    event->accept(); return;
                }
                const quint64 revision = clipboardRevision_;
                const quint64 generation = ++pasteRequestGeneration_;
                QTimer::singleShot(0, this, [this, itemID, plainText, revision, generation] {
                    if (!isVisible() || revision != clipboardRevision_
                            || generation != pasteRequestGeneration_) return;
                    for (int selected = 0; selected < static_cast<int>(items_.size()); ++selected) {
                        if (items_[selected].databaseID != itemID) continue;
                        selectedIndex_ = selected;
                        qInfo() << "[ClipDeck][Paste] card double-click released id=" << itemID;
                        pasteSelected(plainText);
                        break;
                    }
                });
            }
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void armMousePaste(int index, QMouseEvent *event) {
        selectedIndex_ = index;
        pendingMousePasteID_ = items_[index].databaseID;
        pendingMousePasteModifiers_ = standardKeyboardModifiers(event->modifiers());
        pendingMousePastePosition_ = event->globalPos();
        lastCardPressID_ = -1;
        lastCardPressAt_ = 0;
        update();
        event->accept();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override {
        const int index = cardIndexAt(event->pos());
        if (event->button() == Qt::LeftButton && index >= 0) {
            armMousePaste(index, event);
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

    void wheelEvent(QWheelEvent *event) override {
        lastCardPressID_ = -1;
        pendingMousePasteID_ = -1;
        const int delta = event->angleDelta().x() != 0
            ? event->angleDelta().x()
            : event->angleDelta().y();
        if (customStripRect().contains(event->pos())) {
            scrollCategoriesTo(categoryScrollOffset_-delta/2,false);
            event->accept();
            return;
        }
        scrollOffset_ -= delta / 2;
        clampScrollOffset();
        update();
        maybeLoadMoreImages();
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
        auto addShortcutAction = [&menu](const QString &title, const QKeySequence &shortcut) {
            QAction *action = menu.addAction(title);
            action->setShortcut(shortcut);
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            action->setShortcutVisibleInContextMenu(true);
            return action;
        };
        QAction *pasteAction = addShortcutAction(QStringLiteral("粘贴"), QKeySequence(Qt::Key_Return));
        QAction *previewAction = addShortcutAction(QStringLiteral("预览"), QKeySequence(Qt::Key_Space));
        QAction *openAction = nullptr;
        if (items_[index].isText()) {
            const QUrl url(items_[index].text.trimmed());
            if (url.isValid() && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"))) {
                openAction = addShortcutAction(QStringLiteral("访问"),
                                               QKeySequence(Qt::CTRL | Qt::Key_O));
            }
        }
        menu.addSeparator();
        QAction *editAction = addShortcutAction(QStringLiteral("编辑"),
                                               QKeySequence(Qt::CTRL | Qt::Key_E));
        editAction->setEnabled(!items_[index].isFiles());
        QAction *renameAction = addShortcutAction(QStringLiteral("重命名"),
                                                 QKeySequence(Qt::CTRL | Qt::Key_R));
        QAction *rotateLeftAction = nullptr;
        QAction *rotateRightAction = nullptr;
        if (items_[index].isImage()) {
            rotateLeftAction = addShortcutAction(QStringLiteral("向左旋转"),
                                                 QKeySequence(Qt::CTRL | Qt::Key_BracketLeft));
            rotateRightAction = addShortcutAction(QStringLiteral("向右旋转"),
                                                  QKeySequence(Qt::CTRL | Qt::Key_BracketRight));
        }
        menu.addSeparator();
        QAction *deleteAction = addShortcutAction(QStringLiteral("删除"),
                                                 QKeySequence(Qt::CTRL | Qt::Key_Delete));
        popoverOpen_ = true;
        const QAction *chosen = menu.exec(event->globalPos());
        popoverOpen_ = false;
        restoreDeckKeyboard();
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
        if (nonActivatingPresentation_) return;
        if (event->type() != QEvent::ActivationChange || isActiveWindow() || !isVisible()) {
            return;
        }
        const int delay = static_cast<int>(std::max<qint64>(0,
            focusAcquisitionDeadline_ - QDateTime::currentMSecsSinceEpoch()));
        QTimer::singleShot(delay, this, [this] {
            if (isVisible() && !isActiveWindow()
                && QApplication::activeModalWidget() == nullptr
                && QApplication::activePopupWidget() == nullptr) {
                hide();
            }
        });
    }

private:
    void applyPendingResizeGeometry() {
        if (!resizing_ || pendingResizeHeight_ <= 0) return;
        const int bottom = resizeStartGeometry_.bottom() + 1;
        setGeometry(resizeStartGeometry_.x(), bottom - pendingResizeHeight_,
                    resizeStartGeometry_.width(), pendingResizeHeight_);
        clampScrollOffset();
        update();
    }

    void finishLiveResize() {
        if (!resizing_) return;
        if (resizeFrameTimer_) resizeFrameTimer_->stop();
        applyPendingResizeGeometry();
        resizing_ = false;
        if (QWidget::mouseGrabber() == this) releaseMouse();
        if (resizeReleaseTimer_) resizeReleaseTimer_->stop();
        QSettings().setValue(QStringLiteral("deck/preferredHeight"), height());
        textCardCache_.clear();
        if (blurUpdateTimer_) blurUpdateTimer_->start(0);
        update();
    }

    QRect cardsViewport() const {
        // Keep the three vertical breathing spaces visually equal: window top
        // to category strip, category strip to cards, and cards to bottom.
        // At normal Deck heights these are all 18 px.
        return QRect(0, 59, width(), height() - 65);
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

    QRect fileContentRect(int index) const {
        const QRect card = cardRect(index);
        const int header = card.height() >= 180 ? std::min(58,std::max(48,qRound(card.height()*0.20))) : 36;
        return card.adjusted(0, header, 0, 0);
    }

    // macOS marks selection with a faint white fill and a soft 4px shadow
    // (no visible edge); stacked low-alpha rings approximate that blur.
    static void paintSelectionPill(QPainter &painter, const QRect &area, int radius, int fillAlpha) {
        painter.save();
        painter.setPen(Qt::NoPen);
        for (int ring = 4; ring >= 1; --ring) {
            painter.setBrush(QColor(0, 0, 0, 7));
            painter.drawRoundedRect(QRectF(area).translated(0, 1).adjusted(-ring, -ring, ring, ring),
                                    radius + ring, radius + ring);
        }
        painter.setBrush(QColor(255, 255, 255, fillAlpha));
        painter.drawRoundedRect(area, radius, radius);
        painter.restore();
    }

    void paintHeader(QPainter &painter) {
        QFont font = categoryFont();
        painter.setFont(font);
        const QStringList &categories = categoryValues();
        const DeckSymbol symbols[] = {DeckSymbol::History, DeckSymbol::Text, DeckSymbol::Image, DeckSymbol::Folder};
        const QString labels[] = {QStringLiteral("History"),QStringLiteral("文本"),QStringLiteral("图片"),QStringLiteral("文件")};
        for (int index=0; index<4; ++index) {
            const QRect area=fixedTypeRect(index);
            const bool selected=categories.value(index)==selectedCategory_;
            if (selected) paintSelectionPill(painter,area,9,36);
            drawDeckSymbol(painter,QRect(area.x()+10,area.y()+8,19,19),symbols[index],QColor(255,255,255,selected?255:166));
            if (index==0 || selected) {
                painter.setPen(QColor(255,255,255,selected?255:166));
                painter.drawText(area.adjusted(35,0,-10,0),Qt::AlignLeft|Qt::AlignVCenter,labels[index]);
            }
        }
        painter.setPen(QPen(QColor(255,255,255,46),1));
        painter.drawLine(categoryStripX()-15,24,categoryStripX()-15,46);
        painter.save(); painter.setClipRect(customStripRect());
        for (int index=4;index<categories.size();++index) {
            const QRect area=categoryRect(index-4);
            if (!area.intersects(customStripRect())) continue;
            const bool selected=categories[index]==selectedCategory_;
            if (selected) paintSelectionPill(painter,area,16,31);
            QFont labelFont=font;
            const int natural=QFontMetrics(labelFont).horizontalAdvance(categories[index]);
            const int available=area.width()-22;
            if(natural>available && natural<=area.width())
                labelFont.setPixelSize(std::max(11,13*available/std::max(1,natural)));
            painter.setFont(labelFont);
            const QString label=QFontMetrics(labelFont).elidedText(categories[index],Qt::ElideRight,available);
            const int textWidth=QFontMetrics(labelFont).horizontalAdvance(label);
            const int left=area.x()+(area.width()-18-textWidth)/2;
            painter.setPen(Qt::NoPen);
            painter.setBrush(categoryColorsCache_.value(index-4,
                QColor(HistoryRepository::categoryColor(QString(), index-4))));
            painter.drawEllipse(QPointF(left+5.5,area.center().y()),5.5,5.5);
            painter.setPen(QColor(255,255,255,235));
            painter.drawText(QRect(left+18,area.y(),textWidth+1,area.height()),Qt::AlignLeft|Qt::AlignVCenter,label);
        }
        painter.restore();

        if (searchExpansion_ <= 0.001) {
            const QPoint center = searchRect().center() + QPoint(-1, -1);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(255, 255, 255, 210), 2, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            painter.drawEllipse(center, 6, 6);
            painter.drawLine(center + QPoint(4, 4), center + QPoint(9, 9));
        }
        painter.setFont(font);
        auto drawChevron = [&painter](const QRect &rect, bool pointsRight, int alpha) {
            if (rect.isEmpty()) return;
            const QPoint center = rect.center();
            QPainterPath path;
            path.moveTo(center + QPoint(pointsRight ? -2 : 2, -4));
            path.lineTo(center + QPoint(pointsRight ? 2 : -2, 0));
            path.lineTo(center + QPoint(pointsRight ? -2 : 2, 4));
            painter.setPen(QPen(QColor(255, 255, 255, alpha), 1.8, Qt::SolidLine,
                                Qt::RoundCap, Qt::RoundJoin));
            painter.drawPath(path);
        };
        drawChevron(previousCategoryRect(), false, categoryScrollOffset_ > 0 ? 210 : 55);
        drawChevron(nextCategoryRect(), true,
                    categoryScrollOffset_ < maximumCategoryScroll() ? 210 : 55);
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
                         QStringLiteral("复制的文本、图片和文件会显示在这里"));
    }

    QImage cardThumbnail(const ClipboardItem &item) {
        const auto found = imageThumbnailCache_.constFind(item.databaseID);
        if (found != imageThumbnailCache_.constEnd()) return found.value();
        QImage image = imageAtAngle(item.image, item.imageAngle);
        if (image.width() > 768 || image.height() > 768) {
            image = image.scaled(768, 768, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        imageThumbnailCache_.insert(item.databaseID, image);
        return image;
    }

    QImage decodedLinkAsset(const ClipboardItem &item, bool icon) {
        QHash<qint64, QImage> &cache = icon ? linkIconCache_ : linkPreviewCache_;
        const auto found = cache.constFind(item.databaseID);
        if (found != cache.constEnd()) return found.value();
        QImage image;
        image.loadFromData(icon ? item.linkIconData : item.linkPreviewData);
        if (!image.isNull()) cache.insert(item.databaseID, image);
        return image;
    }

    QImage renderedSourceIcon(const ClipboardItem &item, int logicalSide) {
        const qreal dpr = DisplayScale::factor();
        const QSize physicalSize = DisplayScale::rasterSize(QSize(logicalSide, logicalSide), dpr);
        const QString key = QStringLiteral("%1:%2:%3:%4")
            .arg(item.sourceID)
            .arg(physicalSize.width())
            .arg(qRound(dpr * 100.0))
            .arg(qHash(item.sourceIconVariantsData.isEmpty()
                           ? item.sourceIconData : item.sourceIconVariantsData));
        const auto found = sourceIconRenderCache_.constFind(key);
        if (found != sourceIconRenderCache_.constEnd()) return found.value();
        if (sourceIconRenderCache_.size() > 128) sourceIconRenderCache_.clear();

        QImage rendered;
        if (!item.sourceIconVariantsData.isEmpty()) {
            const LinuxApplicationIconSet iconSet =
                LinuxApplicationIconSet::deserialize(item.sourceIconVariantsData);
            rendered = iconSet.rasterize(physicalSize, dpr);
        }
        if (rendered.isNull() && !item.sourceIcon.isNull()) {
            const QImage scaled = item.sourceIcon.scaled(
                physicalSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            rendered = QImage(physicalSize, QImage::Format_ARGB32_Premultiplied);
            rendered.fill(Qt::transparent);
            QPainter iconPainter(&rendered);
            iconPainter.setRenderHint(QPainter::SmoothPixmapTransform);
            iconPainter.drawImage(QPoint((physicalSize.width() - scaled.width()) / 2,
                                         (physicalSize.height() - scaled.height()) / 2), scaled);
            iconPainter.end();
            rendered.setDevicePixelRatio(dpr);
        }
        if (!rendered.isNull()) sourceIconRenderCache_.insert(key, rendered);
        return rendered;
    }

    QImage renderedCardText(const ClipboardItem &item, const QSize &size) {
        const qreal dpr = DisplayScale::factor();
        const int dprKey = qRound(dpr * 100.0);
        const QString key = QStringLiteral("%1:%2x%3:%4:%5:%6")
            .arg(item.databaseID).arg(size.width()).arg(size.height())
            .arg(qHash(item.text)).arg(item.previewLanguage).arg(dprKey);
        const auto found = textCardCache_.constFind(key);
        if (found != textCardCache_.constEnd()) return found.value();
        if (textCardCache_.size() > 160) textCardCache_.clear();
        const QSize pixelSize(static_cast<int>(std::ceil(size.width() * dpr)),
                              static_cast<int>(std::ceil(size.height() * dpr)));
        QImage image(pixelSize, QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(dpr);
        image.fill(Qt::transparent);
        QPainter cachePainter(&image);
        cachePainter.setRenderHint(QPainter::TextAntialiasing);
        paintHighlightedText(cachePainter, QRect(QPoint(0, 0), size),
                             item.text, item.previewLanguage, font());
        textCardCache_.insert(key, image);
        return image;
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
            if (item.isFiles()) {
                FilePasteFeedback feedback;
                if (filePaste_->feedback().itemID == item.databaseID) feedback = filePaste_->feedback();
                paintFileCard(painter, fileContentRect(index), item.files, true, feedback);
            } else if (item.isImage()) {
                const int checkerSize = 12;
                painter.fillRect(contentArea, QColor(26, 26, 28));
                for (int y = contentArea.y(); y < contentArea.bottom(); y += checkerSize) {
                    for (int x = contentArea.x(); x < contentArea.right(); x += checkerSize) {
                        painter.fillRect(QRect(x, y, checkerSize, checkerSize),
                            ((x / checkerSize + y / checkerSize) % 2 == 0)
                                ? QColor(255, 255, 255, 14) : QColor(0, 0, 0, 30));
                    }
                }
                const QImage thumbnail = cardThumbnail(item);
                const QSize fitted = thumbnail.size().scaled(
                    contentArea.adjusted(8, 8, -8, -8).size(), Qt::KeepAspectRatio);
                const QRect imageRect(contentArea.center().x() - fitted.width() / 2,
                                      contentArea.center().y() - fitted.height() / 2,
                                      fitted.width(), fitted.height());
                painter.drawImage(imageRect, thumbnail, thumbnail.rect());
            } else if (parsedColor.isValid()) {
                painter.fillRect(contentArea, parsedColor);
            } else if (isLink) {
                const int footerHeight = expanded ? std::min(88, std::max(72, qRound(card.height() * 0.23))) : 0;
                const QRect previewArea = expanded
                    ? QRect(contentArea.x(), contentArea.y(), contentArea.width(), contentArea.height() - footerHeight)
                    : contentArea;
                const QImage linkPreview = decodedLinkAsset(item, false);
                if (expanded && !linkPreview.isNull()) {
                    const double targetAspect = previewArea.width() / double(previewArea.height());
                    const double sourceAspect = linkPreview.width() / double(linkPreview.height());
                    QRect source = linkPreview.rect();
                    if (sourceAspect > targetAspect) {
                        const int width = qRound(linkPreview.height() * targetAspect);
                        source.setLeft((linkPreview.width() - width) / 2);
                        source.setWidth(width);
                    } else {
                        const int height = qRound(linkPreview.width() / targetAspect);
                        source.setTop((linkPreview.height() - height) / 2);
                        source.setHeight(height);
                    }
                    painter.drawImage(previewArea, linkPreview, source);
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
                    const QImage linkIcon = decodedLinkAsset(item, true);
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
                    const QImage linkIcon = decodedLinkAsset(item, true);
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
                if (resizing_) {
                    QFont quickFont = font();
                    quickFont.setPointSizeF(11.0);
                    painter.setFont(quickFont);
                    painter.setPen(QColor(255, 255, 255, 230));
                    painter.drawText(textRect, Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                                     item.text.left(1200));
                } else {
                    painter.drawImage(textRect.topLeft(), renderedCardText(item, textRect.size()));
                }
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
            metaFont.setPointSizeF(expanded ? 12.0 : 10.0);
            metaFont.setBold(true);
            painter.setFont(metaFont);
            const QString headerTitle = item.isFiles() ? QStringLiteral("文件 · %1 项").arg(item.files.size())
                : isLink ? QStringLiteral("链接") : item.title;
            QString categoryLabel = item.category;
            if (item.previewLanguage == QStringLiteral("code")) {
                const QString detected = HistoryRepository::detectedCodeLanguage(item.text);
                if (!detected.isEmpty()) categoryLabel = displayCodeLanguage(detected);
            }
            auto drawHeaderText = [&](const QRect &target, int flags, const QString &text,
                                      const QColor &foreground) {
                if (parsedColor.isValid()) {
                    // Match macOS colour swatches: a compact dark halo keeps
                    // white metadata readable over yellow/white/pastel fills.
                    painter.setPen(QColor(0, 0, 0, 118));
                    for (const QPoint &offset : {QPoint(-1, 0), QPoint(1, 0),
                                                  QPoint(0, -1), QPoint(0, 2)}) {
                        painter.drawText(target.translated(offset), flags, text);
                    }
                }
                painter.setPen(foreground);
                painter.drawText(target, flags, text);
            };
            const QString titleText = categoryLabel.isEmpty() ? headerTitle
                : expanded ? headerTitle + QStringLiteral("  ") + categoryLabel
                : headerTitle + QStringLiteral("  ") + categoryLabel
                    + QStringLiteral("  ") + relativeTime(item.copiedAt);
            const QRect titleRect = expanded
                ? QRect(card.x() + 14, card.y() + 7, card.width() - 70, 23)
                : QRect(card.x() + 11, card.y() + 7, card.width() - 48, 24);
            drawHeaderText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                           titleText, Qt::white);

            QFont smallFont = font();
            smallFont.setPointSizeF(9.5);
            if (expanded) {
                painter.setFont(smallFont);
                drawHeaderText(QRect(card.x() + 14, card.y() + 28,
                                     card.width() - 70, 18),
                               Qt::AlignLeft | Qt::AlignVCenter,
                               relativeTime(item.copiedAt),
                               QColor(255, 255, 255, 198));
            }
            const int sourceSize = expanded ? headerHeight - 8 : 18;
            const QRect sourceRect(card.right() - sourceSize - (expanded ? 8 : 11),
                                   card.y() + (expanded ? 4 : 9),
                                   sourceSize, sourceSize);
            const bool clipDeckSource = item.sourceID.contains(QStringLiteral("clipdeck"), Qt::CaseInsensitive)
                || item.sourceName.compare(QStringLiteral("ClipDeck"), Qt::CaseInsensitive) == 0;
            const QImage sourceIcon = clipDeckSource
                ? shadowFreeClipDeckIcon()
                : renderedSourceIcon(item, sourceSize);
            if (item.isFiles()) {
                const int side = expanded ? 30 : 18;
                const QRect fileBadge=expanded?QRect(card.right()-44,card.y()+(headerHeight-side)/2,side,side):sourceRect;
                drawDeckSymbol(painter,fileBadge,DeckSymbol::Folder,Qt::white);
            } else if (!sourceIcon.isNull()) {
                painter.drawImage(sourceRect, sourceIcon);
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
            if (expanded && !isLink && !item.isFiles()) {
                const QSize displayedImageSize = imageSizeAtAngle(
                    item.imagePixelSize.isValid() ? item.imagePixelSize : item.image.size(), item.imageAngle);
                const QString footer = item.isImage()
                    ? QStringLiteral("%1 × %2")
                        .arg(displayedImageSize.width()).arg(displayedImageSize.height())
                    : parsedColor.isValid() ? item.text.trimmed()
                    : QStringLiteral("%1 个字符").arg(item.textLength > 0 ? item.textLength : item.text.size());
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
        filePaste_->cancel();
        if (items_.empty()) {
            return;
        }
        selectedIndex_ = std::max(0, std::min(
            static_cast<int>(items_.size()) - 1,
            selectedIndex_ + direction));
        centerSelection();
        update();
        maybeLoadMoreImages();
    }

    void maybeLoadMoreImages() {
        if (selectedCategory_ != QStringLiteral("Images") || !imageHasMore_
            || imageBatchLoading_ || items_.empty()) return;
        const int loaded = static_cast<int>(items_.size());
        const int maximumScroll = std::max(0,
            kContentInset * 2 + loaded * (cardSide() + kCardSpacing) - kCardSpacing
            - cardsViewport().width());
        const bool selectionNearEnd = selectedIndex_ >= loaded - 5;
        const bool viewportNearEnd = scrollOffset_ >= std::max(0,
            maximumScroll - 2 * (cardSide() + kCardSpacing));
        if (!selectionNearEnd && !viewportNearEnd) return;

        imageBatchLoading_ = true;
        const int offset = loaded;
        const QString expectedSearch = searchQuery_;
        const int generation = imageLoadGeneration_;
        QTimer::singleShot(0, this, [this, offset, expectedSearch, generation] {
            if (selectedCategory_ != QStringLiteral("Images")
                || imageLoadGeneration_ != generation
                || searchQuery_ != expectedSearch
                || static_cast<int>(items_.size()) != offset) {
                imageBatchLoading_ = false;
                return;
            }
            const int remaining = std::max(0, kHistoryCapacity - offset);
            const int requested = std::min(kImageBatchCapacity, remaining);
            if (requested <= 0) {
                imageHasMore_ = false;
                imageBatchLoading_ = false;
                return;
            }
            const auto append = [this, offset, expectedSearch, generation, requested](QVector<ClipboardItem> next) {
                if (selectedCategory_ != QStringLiteral("Images")
                    || imageLoadGeneration_ != generation || searchQuery_ != expectedSearch
                    || static_cast<int>(items_.size()) != offset) return;
                for (const ClipboardItem &item : next) items_.push_back(item);
                imageLoadLimit_ = static_cast<int>(items_.size());
                imageHasMore_ = next.size() == requested
                    && imageLoadLimit_ < kHistoryCapacity;
                imageBatchLoading_ = false;
                clampScrollOffset();
                update();
            };
            if (pipeline_) {
                const QPointer<DeckWindow> guard(this);
                pipeline_->requestHistory(QStringLiteral("图片"), expectedSearch, requested, offset,
                    [guard, append](QVector<ClipboardItem> next) {
                        if (guard) append(std::move(next));
                    });
            } else append(repository_->fetch(QStringLiteral("图片"), expectedSearch, requested, offset));
        });
    }

    void changeCategory(int direction) {
        const QStringList &categories = categoryValues();
        const int current = std::max(0, categories.indexOf(selectedCategory_));
        const int next = (current + direction + categories.size()) % categories.size();
        selectCategory(categories[next]);
    }

    void changeCategoryPage(int direction) {
        scrollCategoriesTo(categoryScrollOffset_+direction*5*108,true);
    }

    void scrollCategoriesTo(int offset, bool animated) {
        offset=qBound(0,offset,maximumCategoryScroll());
        categoryAnimation_->stop();
        if (animated) {
            categoryAnimation_->setStartValue(categoryScrollOffset_);
            categoryAnimation_->setEndValue(offset); categoryAnimation_->start();
        } else { categoryScrollOffset_=offset; update(); }
    }

    void selectCategory(const QString &category) {
        filePaste_->cancel();
        selectedCategory_=category;
        selectedIndex_=-1;
        scrollOffset_=0;
        const int index=categoryValues().indexOf(category)-4;
        if (index>=0) {
            const int left=index*108;
            if (left<categoryScrollOffset_) scrollCategoriesTo(left,true);
            else if (left+96>categoryScrollOffset_+visibleCategoryStripWidth())
                scrollCategoriesTo(left+96-visibleCategoryStripWidth(),true);
        }
        updateSearchGeometry(); update(); reload();
    }

    void deleteSelected() {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) {
            return;
        }
        repository_->remove(items_[selectedIndex_].databaseID);
        if (pipeline_) pipeline_->refreshRecent();
        items_.erase(items_.begin() + selectedIndex_);
        if (items_.empty()) {
            selectedIndex_ = -1;
        } else {
            selectedIndex_ = std::min(selectedIndex_, static_cast<int>(items_.size()) - 1);
        }
        clampScrollOffset();
        update();
    }

    const QStringList &categoryValues() const {
        return categoryValuesCache_;
    }

    void refreshCategoryValues() {
        QStringList values{QStringLiteral("History"), QStringLiteral("Text"),
                           QStringLiteral("Images"), QStringLiteral("Files")};
        QVector<QColor> colors;
        const QVector<ClipboardCategory> categories = repository_->categories();
        for (int position = 0; position < categories.size(); ++position) {
            values.append(categories[position].name);
            colors.append(QColor(HistoryRepository::categoryColor(categories[position].icon, position)));
        }
        categoryValuesCache_ = std::move(values);
        categoryColorsCache_ = std::move(colors);
        if (!categoryValuesCache_.contains(selectedCategory_)) {
            selectedCategory_ = QStringLiteral("History");
        }
    }

    QFont categoryFont() const {
        QFont value = font();
        value.setPixelSize(13);
        value.setWeight(QFont::DemiBold);
        return value;
    }

    int visibleCategoryStripWidth() const {
        const int count=std::max(0,categoryValues().size()-4);
        const int preferred=std::min(760,std::max(240,count*108-12));
        return std::min(preferred,std::max(0,width()-searchControlWidth()-fixedTypesWidth()-20-29-42-54-(count>6?46:0)-48));
    }

    int fixedTypeWidth(int index) const {
        const QString labels[]={QStringLiteral("History"),QStringLiteral("文本"),QStringLiteral("图片"),QStringLiteral("文件")};
        return 39+((index==0 || categoryValues().value(index)==selectedCategory_)
            ? 6+QFontMetrics(categoryFont()).horizontalAdvance(labels[index]) : 0);
    }
    int fixedTypesWidth() const {
        int total=12;
        for(int index=0;index<4;++index) total+=fixedTypeWidth(index);
        return total;
    }
    QRect fixedTypeRect(int index) const {
        int x=toolbarStartX()+searchControlWidth()+20;
        for(int i=0;i<index;++i) x+=fixedTypeWidth(i)+4;
        return QRect(x,18,fixedTypeWidth(index),35);
    }
    QRect customStripRect() const { return QRect(categoryStripX(),20,visibleCategoryStripWidth(),31); }
    int maximumCategoryScroll() const {
        return std::max(0,(categoryValues().size()-4)*108-12-visibleCategoryStripWidth());
    }

    QRect categoryRect(int index) const {
        return QRect(categoryStripX()+index*108-qBound(0,categoryScrollOffset_,maximumCategoryScroll()),20,96,31);
    }

    QRect searchRect() const {
        return QRect(toolbarStartX(), 18, searchControlWidth(), 34);
    }

    int searchControlWidth() const {
        return qRound(28.0 + (230.0 - 28.0) * searchExpansion_);
    }

    int toolbarStartX() const {
        const int searchWidth = searchControlWidth();
        const int toolbarWidth=searchWidth+20+fixedTypesWidth()+29+visibleCategoryStripWidth()
            +(categoryValues().size()>10?46:0)+42+54;
        return std::max(24,(width()-toolbarWidth)/2);
    }
    int categoryStripX() const { return toolbarStartX()+searchControlWidth()+20+fixedTypesWidth()+29; }
    QRect previousCategoryRect() const {
        return categoryValues().size()>10 ? QRect(categoryStripX()+visibleCategoryStripWidth()+2,20,23,31) : QRect();
    }
    QRect nextCategoryRect() const { return previousCategoryRect().isEmpty()?QRect():previousCategoryRect().translated(23,0); }
    QRect addRect() const { return QRect(categoryStripX()+visibleCategoryStripWidth()+(categoryValues().size()>10?48:2)+5,18,34,34); }
    QRect moreRect() const { return QRect(width() - 68, 18, 42, 35); }

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
        if (searchAnimation_) {
            searchAnimation_->stop();
            searchAnimation_->setStartValue(searchExpansion_);
            searchAnimation_->setEndValue(1.0);
            searchAnimation_->start();
        } else {
            searchExpansion_ = 1.0;
        }
        searchEdit_->setFocus(Qt::ShortcutFocusReason);
        searchEdit_->setCursorPosition(searchEdit_->text().size());
    }

    void endSearch() {
        searchActive_ = false;
        searchQuery_.clear();
        searchEdit_->clear();
        if (searchDebounce_) searchDebounce_->stop();
        selectedCategory_ = QStringLiteral("History");
        setFocus(Qt::ShortcutFocusReason);
        reload();
        if (searchAnimation_) {
            searchAnimation_->stop();
            searchAnimation_->setStartValue(searchExpansion_);
            searchAnimation_->setEndValue(0.0);
            searchAnimation_->start();
        } else {
            searchExpansion_ = 0.0;
            searchEdit_->hide();
        }
    }

    void updateSearchGeometry() {
        if (!searchEdit_) return;
        searchEdit_->setGeometry(searchRect());
        const int glyphWidth = 26;
        if (searchLeadingGlyph_) {
            searchLeadingGlyph_->setGeometry(0, 0, glyphWidth, searchEdit_->height());
            searchLeadingGlyph_->raise();
        }
        if (searchCloseGlyph_) {
            searchCloseGlyph_->setGeometry(
                std::max(0, searchEdit_->width() - glyphWidth), 0,
                glyphWidth, searchEdit_->height());
            searchCloseGlyph_->setVisible(searchActive_ && searchEdit_->width() >= 72);
            searchCloseGlyph_->raise();
        }
    }

    QRect selectedCardScreenGeometry() const {
        const QRect card = selectedIndex_ >= 0 ? cardRect(selectedIndex_) : rect();
        return QApplication::desktop()->availableGeometry(mapToGlobal(card.center()));
    }

    void positionPopover(QDialog &dialog, const QSize &size) const {
        dialog.resize(size);
        const QRect card = selectedIndex_ >= 0 ? cardRect(selectedIndex_) : rect();
        const QRect globalCard(mapToGlobal(card.topLeft()), card.size());
        const QRect available = selectedCardScreenGeometry();
        int x = globalCard.center().x() - size.width() / 2;
        int y = globalCard.top() - size.height() - 12;
        if (y < available.top() + 8) y = globalCard.bottom() + 12;
        x = std::max(available.left() + 8, std::min(x, available.right() - size.width() - 7));
        y = std::max(available.top() + 8, std::min(y, available.bottom() - size.height() - 7));
        dialog.move(x, y);
    }

    void preparePopover(QDialog &dialog, const QSize &size, bool passivePreview = false) const {
        if (passivePreview) {
            dialog.setWindowFlags(Qt::Tool | Qt::FramelessWindowHint
                | Qt::WindowStaysOnTopHint | Qt::WindowDoesNotAcceptFocus
                | Qt::NoDropShadowWindowHint);
            dialog.setAttribute(Qt::WA_X11DoNotAcceptFocus, true);
            dialog.setAttribute(Qt::WA_ShowWithoutActivating, true);
        } else {
            dialog.setWindowFlags(Qt::Popup | Qt::FramelessWindowHint);
        }
        dialog.setAttribute(Qt::WA_TranslucentBackground, true);
        dialog.setAttribute(Qt::WA_NoSystemBackground, true);
        dialog.setModal(!passivePreview);
        // Do not use an XShape window mask here.  UOS/DDE rasterizes the
        // rounded mask into visible horizontal scan bands at its top and
        // bottom edges.  WA_TranslucentBackground plus the stylesheet's
        // antialiased border radius supplies the rounded corners per pixel.
        dialog.clearMask();
        positionPopover(dialog, size);
        dialog.setStyleSheet(QStringLiteral(
            "QDialog{background:#242427;color:#eee;border:1px solid #49494d;border-radius:14px;}"
            "QPlainTextEdit{background:#111113;color:#eee;border:1px solid #38383c;border-radius:10px;"
            "padding:10px;outline:0;selection-background-color:#48484d;}"
            "QPlainTextEdit:!focus{background:#111113;border-color:#38383c;}"
            "QPlainTextEdit:focus{background:#151518;border-color:#1683ff;selection-background-color:#1683ff;}"
            "QLineEdit{background:#333337;color:#eee;border:1px solid transparent;border-radius:9px;"
            "padding:8px 12px;outline:0;selection-background-color:#48484d;}"
            "QLineEdit:!focus{background:#333337;border-color:transparent;}"
            "QLineEdit:focus{background:#39393d;border-color:#1683ff;selection-background-color:#1683ff;}"
            "QWidget#previewHeader{background:#2b2b2e;border:0;border-top-left-radius:14px;"
            "border-top-right-radius:14px;}"
            "QLabel#previewCategory{color:#f4f4f5;font-size:18px;font-weight:700;}"
            "QLabel#previewSource{color:#b0b0b5;font-size:13px;font-weight:500;}"
            "QLabel#previewTime{color:#9a9aa0;font-size:11px;}"
            "QPlainTextEdit#contentPreview{background:#151517;color:#eeeeef;border:0;"
            "border-top:1px solid #3a3a3d;border-bottom-left-radius:14px;"
            "border-bottom-right-radius:14px;padding:22px;font-size:14px;}"
            "QDialog#contentEditorDialog{background:qlineargradient(x1:0,y1:0,x2:0,y2:1,"
            "stop:0 #2e2e35,stop:1 #212126);border:1px solid #49494d;border-radius:14px;}"
            "QWidget#editorToolbar{background:rgba(255,255,255,9);border-bottom:1px solid #414146;"
            "border-top-left-radius:14px;border-top-right-radius:14px;}"
            "QLabel#editorHeading{color:rgba(255,255,255,194);font-size:15px;font-weight:600;}"
            "QWidget#editorBody{background:transparent;}"
            "QPlainTextEdit#contentEditor{background:rgba(0,0,0,143);color:#eeeeef;"
            "border:1px solid rgba(255,255,255,23);border-radius:12px;padding:16px;outline:0;}"
            "QPlainTextEdit#contentEditor:focus{background:rgba(0,0,0,143);"
            "border:1px solid rgba(255,255,255,23);selection-background-color:#1683ff;}"
            "QLabel#editorStatistics{color:rgba(255,255,255,128);font-size:12px;font-weight:500;}"
            "QPushButton#editorCancel,QPushButton#editorSave{border:0;border-radius:17px;"
            "min-width:68px;max-width:68px;min-height:34px;max-height:34px;padding:0;}"
            "QPushButton#editorCancel{background:rgba(255,255,255,18);color:#eeeeef;}"
            "QPushButton#editorSave{background:#1683ff;color:white;font-weight:600;}"
            "QPushButton#editorTool{background:transparent;border:0;border-radius:17px;"
            "min-width:34px;max-width:34px;min-height:34px;max-height:34px;padding:0;font-size:18px;}"
            "QPushButton#editorTool:hover{background:rgba(255,255,255,16);}"
            "QPushButton{background:#3a3a3e;color:#eee;border:0;border-radius:17px;padding:8px 18px;}"
            "QPushButton#primary{background:#1683ff;color:white;font-weight:600;}"));
    }

    void previewSelected() {
        if (previewDialog_ || popoverOpen_ || selectedIndex_ < 0
                || selectedIndex_ >= static_cast<int>(items_.size())) return;
        if (ensureSelectedPayload([this] { previewSelected(); })) return;
        ClipboardItem item = items_[selectedIndex_];
        QDialog dialog(this);
        const QSize previewSize = item.isImage()
            ? imagePreviewWindowSize(imageSizeAtAngle(item.image.size(), item.imageAngle),
                                     selectedCardScreenGeometry().size())
            : QSize(560, 360);
        preparePopover(dialog, previewSize, true);
        bool editRequested = false;
        bool imageAngleChanged = false;
        int moveDirection = 0;
        std::function<void(int)> rotatePreview;
        QVBoxLayout *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);
        QWidget *header = new QWidget(&dialog);
        header->setObjectName(QStringLiteral("previewHeader"));
        header->setFixedHeight(item.isFiles()?52:58);
        QHBoxLayout *headerLayout = new QHBoxLayout(header);
        headerLayout->setContentsMargins(22, 0, 22, 0);
        headerLayout->setSpacing(12);
        QLabel *category = new ElidingLabel(item.isFiles() ? QStringLiteral("文件 · %1 项").arg(item.files.size()) : item.category.isEmpty()
            ? (item.isImage() ? QStringLiteral("图片") : QStringLiteral("文本")) : item.category, header);
        category->setObjectName(QStringLiteral("previewCategory"));
        QLabel *source = new ElidingLabel(item.sourceName, header);
        source->setObjectName(QStringLiteral("previewSource"));
        // Narrow portrait previews must not be expanded by a long app name.
        source->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        source->setToolTip(item.sourceName);
        category->setToolTip(category->text());
        category->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        category->setMaximumWidth(item.isFiles()?160:100);
        if (item.isFiles()) category->setStyleSheet(QStringLiteral("font-size:15px;"));
        source->setMinimumWidth(24);
        QLabel *time = new ElidingLabel(item.copiedAt.toString(QStringLiteral("yyyy-MM-dd  HH:mm")), header);
        time->setObjectName(QStringLiteral("previewTime"));
        headerLayout->addWidget(category);
        headerLayout->addWidget(source, 1);
        headerLayout->addWidget(time);
        layout->addWidget(header);
        QPlainTextEdit *previewText = nullptr;
        FilePreviewWidget *filePreview = nullptr;
        ImagePreviewWidget *imagePreview = nullptr;
        if (item.isFiles()) {
            filePreview = new FilePreviewWidget(item.files, &dialog);
            layout->addWidget(filePreview, 1);
        } else if (item.isImage()) {
            imagePreview = new ImagePreviewWidget(&dialog);
            imagePreview->setMinimumSize(0, 0);
            imagePreview->setImage(imageAtAngle(item.image, item.imageAngle));
            layout->addWidget(imagePreview, 1);

            rotatePreview = [this, &dialog, &item, imagePreview, &imageAngleChanged](int degrees) {
                const int angle = normalizedImageAngle(item.imageAngle + degrees);
                if (!repository_->updateImageAngle(item.databaseID, angle)) return;
                if (pipeline_) pipeline_->refreshRecent();
                item.imageAngle = angle;
                imageThumbnailCache_.remove(item.databaseID);
                imagePreview->setImage(imageAtAngle(item.image, item.imageAngle));
                positionPopover(dialog, imagePreviewWindowSize(
                    imageSizeAtAngle(item.image.size(), item.imageAngle),
                    selectedCardScreenGeometry().size()));
                imageAngleChanged = true;
            };
        } else {
            QPlainTextEdit *text = new QPlainTextEdit(item.text.left(200000), &dialog);
            previewText = text;
            text->setObjectName(QStringLiteral("contentPreview"));
            text->setReadOnly(true);
            QFont textFont = text->font();
            if (!item.previewLanguage.isEmpty() && item.previewLanguage != QStringLiteral("text")) {
                textFont.setFamily(QStringLiteral("monospace"));
                textFont.setStyleHint(QFont::Monospace);
                text->setLineWrapMode(QPlainTextEdit::NoWrap);
            } else {
                text->setLineWrapMode(QPlainTextEdit::WidgetWidth);
                text->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
                text->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            }
            textFont.setPointSize(14);
            text->setFont(textFont);
            new CardSyntaxHighlighter(text->document(), item.previewLanguage);
            layout->addWidget(text, 1);
        }
        // A Qt::Popup/exec() takes native focus and may replace Deck's X11
        // keyboard grab. Keep this read-only window nonmodal and nonactivating;
        // the existing Deck grab dispatches preview controls here instead.
        previewDialog_ = &dialog;
        previewKeyHandler_ = [&](QKeyEvent *event) {
            const auto modifiers = standardKeyboardModifiers(event->modifiers());
            if (modifiers == Qt::NoModifier
                    && (event->key() == Qt::Key_Left || event->key() == Qt::Key_Right)) {
                moveDirection = event->key() == Qt::Key_Left ? -1 : 1;
                dialog.reject();
            } else if (modifiers == Qt::NoModifier
                    && (event->key() == Qt::Key_Escape || event->key() == Qt::Key_Space)) {
                dialog.reject();
            } else if (!item.isFiles() && modifiers == Qt::ControlModifier && event->key() == Qt::Key_E) {
                editRequested = true;
                dialog.accept();
            } else if (rotatePreview && modifiers == Qt::ControlModifier
                    && (event->key() == Qt::Key_BracketLeft
                        || event->key() == Qt::Key_BracketRight)) {
                rotatePreview(event->key() == Qt::Key_BracketLeft ? -90 : 90);
            } else if (previewText && modifiers == Qt::NoModifier
                    && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
                QScrollBar *bar = previewText->verticalScrollBar();
                bar->setValue(bar->value() + (event->key() == Qt::Key_Up ? -1 : 1)
                    * std::max(1, bar->singleStep()));
            } else if (filePreview && modifiers == Qt::NoModifier
                    && (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
                filePreview->scrollBy(event->key() == Qt::Key_Up ? -1 : 1);
            }
            event->accept();
        };
        dialog.installEventFilter(this);
        if (previewText) previewText->installEventFilter(this);
        QEventLoop previewLoop;
        QObject::connect(&dialog, &QDialog::finished, &previewLoop, &QEventLoop::quit);
        dialog.show();
        dialog.raise();
        previewLoop.exec();
        previewKeyHandler_ = {};
        previewDialog_.clear();
        dialog.hide();
        if (imageAngleChanged) reload();
        if (moveDirection != 0) moveSelection(moveDirection);
        if (isVisible()) {
            restoreDeckKeyboard();
            if (editRequested) editSelected();
        }
    }

    void editSelected() {
        if (popoverOpen_ || selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) return;
        if (items_[selectedIndex_].isFiles()) return;
        if (ensureSelectedPayload([this] { editSelected(); })) return;
        const ClipboardItem item = items_[selectedIndex_];
        QDialog dialog(this);
        preparePopover(dialog, QSize(560, 360));
        QVBoxLayout *layout = new QVBoxLayout(&dialog);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        QWidget *toolbarWidget = new QWidget(&dialog);
        toolbarWidget->setObjectName(QStringLiteral("previewHeader"));
        toolbarWidget->setFixedHeight(58);
        QHBoxLayout *toolbar = new QHBoxLayout(toolbarWidget);
        toolbar->setContentsMargins(14, 0, 14, 0);
        toolbar->setSpacing(10);
        QPushButton *cancel = new QPushButton(QStringLiteral("取消"), toolbarWidget);
        cancel->setObjectName(QStringLiteral("editorCancel"));
        QPushButton *save = new QPushButton(QStringLiteral("保存"), toolbarWidget);
        save->setObjectName(QStringLiteral("editorSave"));
        QLabel *statistics = new QLabel(toolbarWidget);
        statistics->setObjectName(QStringLiteral("editorStatistics"));
        QPushButton *formatJSON = nullptr;
        const bool isJSON = item.previewLanguage == QStringLiteral("json")
            || item.category.compare(QStringLiteral("JSON"), Qt::CaseInsensitive) == 0;
        toolbar->addWidget(cancel);
        toolbar->addStretch();
        QPushButton *rotateLeft = nullptr;
        QPushButton *rotateRight = nullptr;
        if (item.isImage()) {
            rotateLeft = new QPushButton(toolbarWidget);
            rotateRight = new QPushButton(toolbarWidget);
            rotateLeft->setObjectName(QStringLiteral("editorTool"));
            rotateRight->setObjectName(QStringLiteral("editorTool"));
            rotateLeft->setIcon(imageRotationIcon(false));
            rotateRight->setIcon(imageRotationIcon(true));
            rotateLeft->setIconSize(QSize(24, 24));
            rotateRight->setIconSize(QSize(24, 24));
            rotateLeft->setToolTip(QStringLiteral("向左旋转"));
            rotateRight->setToolTip(QStringLiteral("向右旋转"));
            toolbar->addWidget(rotateLeft);
            toolbar->addWidget(rotateRight);
        } else {
            QLabel *heading = new QLabel(QStringLiteral("编辑文本"), toolbarWidget);
            heading->setObjectName(QStringLiteral("previewCategory"));
            toolbar->addWidget(heading);
        }
        toolbar->addStretch();
        toolbar->addWidget(statistics);
        if (!item.isImage() && isJSON) {
            formatJSON = new QPushButton(QStringLiteral("格式化"), toolbarWidget);
            formatJSON->setToolTip(QStringLiteral("格式化 JSON"));
            toolbar->addWidget(formatJSON);
        }
        toolbar->addWidget(save);
        layout->addWidget(toolbarWidget);

        QWidget *body = new QWidget(&dialog);
        body->setObjectName(QStringLiteral("editorBody"));
        QVBoxLayout *bodyLayout = new QVBoxLayout(body);
        bodyLayout->setContentsMargins(0, 0, 0, 0);
        bodyLayout->setSpacing(0);

        int editedImageAngle = item.imageAngle;
        QPlainTextEdit *editor = nullptr;
        ImagePreviewWidget *imagePreview = nullptr;
        if (item.isImage()) {
            imagePreview = new ImagePreviewWidget(body);
            auto updateImagePreview = [imagePreview, statistics, &item, &editedImageAngle] {
                const QImage displayedImage = imageAtAngle(item.image, editedImageAngle);
                imagePreview->setImage(displayedImage);
                statistics->setText(QStringLiteral("%1 × %2  ·  %3°")
                    .arg(displayedImage.width()).arg(displayedImage.height())
                    .arg(normalizedImageAngle(editedImageAngle)));
            };
            QObject::connect(rotateLeft, &QPushButton::clicked, &dialog,
                             [&editedImageAngle, updateImagePreview] {
                editedImageAngle = normalizedImageAngle(editedImageAngle - 90);
                updateImagePreview();
            });
            QObject::connect(rotateRight, &QPushButton::clicked, &dialog,
                             [&editedImageAngle, updateImagePreview] {
                editedImageAngle = normalizedImageAngle(editedImageAngle + 90);
                updateImagePreview();
            });
            QShortcut *rotateLeftShortcut = new QShortcut(
                QKeySequence(Qt::CTRL | Qt::Key_BracketLeft), &dialog);
            QShortcut *rotateRightShortcut = new QShortcut(
                QKeySequence(Qt::CTRL | Qt::Key_BracketRight), &dialog);
            QObject::connect(rotateLeftShortcut, &QShortcut::activated,
                             rotateLeft, &QPushButton::click);
            QObject::connect(rotateRightShortcut, &QShortcut::activated,
                             rotateRight, &QPushButton::click);
            updateImagePreview();
            bodyLayout->addWidget(imagePreview, 1);
        } else {
            editor = new QPlainTextEdit(item.text, body);
            editor->setObjectName(QStringLiteral("contentPreview"));
            QFont editorFont = editor->font();
            if (!item.previewLanguage.isEmpty() && item.previewLanguage != QStringLiteral("text")) {
                editorFont.setFamily(QStringLiteral("monospace"));
                editorFont.setStyleHint(QFont::Monospace);
                editor->setLineWrapMode(QPlainTextEdit::NoWrap);
            } else {
                editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
                editor->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
                editor->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
            }
            editorFont.setPointSize(14);
            editor->setFont(editorFont);
            new CardSyntaxHighlighter(editor->document(), item.previewLanguage);
            bodyLayout->addWidget(editor, 1);
            auto updateStatistics = [editor, statistics] {
                // One pass over the text; splitting it with a regex on every
                // keystroke made long texts lag while typing.
                const QString text = editor->toPlainText();
                int words = 0;
                int lines = 1;
                bool inWord = false;
                for (const QChar character : text) {
                    if (character == QLatin1Char('\n')) ++lines;
                    const bool space = character.isSpace();
                    if (!space && !inWord) ++words;
                    inWord = !space;
                }
                statistics->setStyleSheet(QString());
                statistics->setText(QStringLiteral("%1 个字符  ·  %2 个单词  ·  %3 行")
                    .arg(text.size()).arg(words).arg(lines));
            };
            QTimer *statisticsTimer = new QTimer(&dialog);
            statisticsTimer->setSingleShot(true);
            statisticsTimer->setInterval(150);
            QObject::connect(statisticsTimer, &QTimer::timeout, &dialog, updateStatistics);
            QObject::connect(editor, &QPlainTextEdit::textChanged, statisticsTimer,
                             qOverload<>(&QTimer::start));
            if (formatJSON) {
                QObject::connect(formatJSON, &QPushButton::clicked, &dialog,
                                 [editor, statistics] {
                    QJsonParseError error;
                    const QJsonDocument document = QJsonDocument::fromJson(
                        editor->toPlainText().toUtf8(), &error);
                    if (error.error != QJsonParseError::NoError || document.isNull()) {
                        // Shown in the toolbar, not in an alert window: opening
                        // another window closes this Qt::Popup and loses the edit.
                        statistics->setStyleSheet(QStringLiteral("color:#ff7373;"));
                        statistics->setText(QStringLiteral("JSON 第 %1 字节附近有语法错误")
                            .arg(error.offset));
                        return;
                    }
                    editor->setPlainText(QString::fromUtf8(document.toJson(QJsonDocument::Indented)));
                });
            }
            updateStatistics();
            editor->setFocus();
        }
        layout->addWidget(body, 1);
        QObject::connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
        QObject::connect(save, &QPushButton::clicked, &dialog, &QDialog::accept);
        if (!execPopover(dialog)) return;
        if (item.isImage()) {
            repository_->updateImageAngle(item.databaseID, editedImageAngle);
            imageThumbnailCache_.remove(item.databaseID);
        } else if (editor) {
            const QString value = editor->toPlainText();
            repository_->updateText(item.databaseID, value, repository_->classifyText(value));
        }
        if (pipeline_) pipeline_->refreshRecent();
        reload();
    }

    void renameSelected() {
        if (popoverOpen_ || selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) return;
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
        if (execPopover(dialog)) {
            repository_->updateTitle(item.databaseID, title->text());
            if (pipeline_) pipeline_->refreshRecent();
            reload();
        }
    }

    void rotateSelected(int degrees) {
        if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())
            || !items_[selectedIndex_].isImage()) return;
        const ClipboardItem &item = items_[selectedIndex_];
        repository_->updateImageAngle(
            item.databaseID, normalizedImageAngle(item.imageAngle + degrees));
        imageThumbnailCache_.remove(items_[selectedIndex_].databaseID);
        if (pipeline_) pipeline_->refreshRecent();
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
                if (pipeline_) pipeline_->refreshRecent();
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
                if (pipeline_) pipeline_->refreshRecent();
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
        if (ensureSelectedPayload([this] { openSelected(); })) return;
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
        pasteTarget_ = None;
        pasteTargetInputWindow_ = None;
        if (!display_) {
            return;
        }
        // _NET_ACTIVE_WINDOW is the EWMH client window.  Walking X input
        // focus up to the root returns the window-manager frame instead; that
        // frame never equals _NET_ACTIVE_WINDOW and prevented automatic paste.
        QWidget *focusedWidget = QApplication::focusWidget();
        if (focusedWidget && focusedWidget != this
                && focusedWidget->window() != this) {
            pasteTargetWidget_ = focusedWidget;
        } else {
            pasteTargetWidget_.clear();
        }
        Window focused = None;
        int revertTo = RevertToNone;
        XGetInputFocus(display_, &focused, &revertTo);
        pasteTargetInputWindow_ = focused;
        Window target = activeTopLevelWindow(display_);
        if (target == None) {
            target = focused;
        }
        if (target != None && target != static_cast<Window>(winId())) {
            pasteTarget_ = target;
        }
        qInfo().noquote() << QStringLiteral(
            "[ClipDeck][Paste] remembered target=0x%1 input=0x%2")
            .arg(static_cast<qulonglong>(pasteTarget_), 0, 16)
            .arg(static_cast<qulonglong>(pasteTargetInputWindow_), 0, 16);
    }

    bool pasteModifierPressed() const {
        if (!display_) return false;
        char keys[32]{};
        XQueryKeymap(display_, keys);
        const KeySym modifiers[] = {XK_Control_L, XK_Control_R, XK_Shift_L, XK_Shift_R,
                                    XK_Alt_L, XK_Alt_R, XK_Meta_L, XK_Meta_R,
                                    XK_Super_L, XK_Super_R};
        for (const KeySym symbol : modifiers) {
            const KeyCode code = XKeysymToKeycode(display_, symbol);
            if (code != 0 && (keys[code / 8] & (1 << (code % 8)))) return true;
        }
        return false;
    }

    bool pasteRequestIsCurrent(const PasteRequestPtr &request) {
        if (!request || request->generation != pasteRequestGeneration_) return false;
        if (request->clipboardRevision == clipboardRevision_) return true;

        // UOS's clipboard manager may take ownership while preserving the
        // same payload. Permit that handoff, but never emit a stale Ctrl+V
        // once a user's newer cut/copy has replaced the selected content.
        QClipboard *clipboard = QApplication::clipboard();
        const quint64 revisionBeforeRead = clipboardRevision_;
        bool sameContent = false;
        if (!request->files.isEmpty()) {
            const QMimeData *mime = clipboard->mimeData(QClipboard::Clipboard);
            if (mime) {
                QVector<ClipboardFileReference> files;
                auto urls=mime->urls().toVector();
                if(urls.isEmpty()) urls=FileReferences::urls(mime->data(QStringLiteral("x-special/gnome-copied-files")));
                for (const auto &url : urls) if (url.isLocalFile()) files.append({url.toLocalFile(),{},{},false});
                sameContent = FileReferences::identity(files) == FileReferences::identity(request->files);
            }
        } else if (request->isImage) {
            const QMimeData *mime = clipboard->mimeData(QClipboard::Clipboard);
            if (mime && mime->hasImage()) {
                const QImage current = qvariant_cast<QImage>(mime->imageData());
                sameContent = current.size() == request->image.size()
                    && current.convertToFormat(QImage::Format_ARGB32)
                        == request->image.convertToFormat(QImage::Format_ARGB32);
            }
        } else {
            sameContent = clipboard->text(QClipboard::Clipboard) == request->text;
        }
        // Clipboard reads can dispatch X11 events and enter another paste
        // operation. Recheck the generation before updating the request.
        if (request->generation != pasteRequestGeneration_
                || revisionBeforeRead != clipboardRevision_) return false;
        if (sameContent) {
            request->clipboardRevision = clipboardRevision_;
            return true;
        }
        ++pasteRequestGeneration_;
        qInfo() << "[ClipDeck][Paste] cancelled obsolete paste after new clipboard content";
        return false;
    }

    void waitForPasteModifiersReleased(Window target, PasteRequestPtr request, int attempt,
                                       bool preservedSourceFocus = false) {
        if (!pasteRequestIsCurrent(request)) return;
        // Ctrl+number is handled while Ctrl is still physically down. Sending
        // synthetic Ctrl+V at that point can be swallowed, or release the
        // user's Ctrl before the target receives V. Wait for the gesture to
        // finish before restoring focus and emitting paste.
        if (pasteModifierPressed()) {
            if (attempt >= 150) {
                qWarning() << "[ClipDeck][Paste] modifier release timed out";
                return;
            }
            QTimer::singleShot(20, this, [this, target, request, attempt, preservedSourceFocus] {
                waitForPasteModifiersReleased(target, request, attempt + 1, preservedSourceFocus);
            });
            return;
        }
        if (preservedSourceFocus) {
            pasteUsingPreservedFocus(target, request);
            return;
        }
        activateTarget(target);
        restorePasteWidgetFocus(target);
        waitForTargetAndPaste(target, request, 0);
    }

    void pasteUsingPreservedFocus(Window target, PasteRequestPtr request, bool useHeldControl = false,
                                  bool temporarilyReleaseShift = false) {
        if (!display_ || target == None || !pasteRequestIsCurrent(request)) return;
        Window focused = None;
        int revertTo = RevertToNone;
        XGetInputFocus(display_, &focused, &revertTo);
        const bool focusPreserved = pasteTargetInputWindow_ != None
            && focused == pasteTargetInputWindow_;
        qInfo().noquote() << QStringLiteral(
            "[ClipDeck][Paste] preserved target=0x%1 expectedInput=0x%2 currentInput=0x%3 ok=%4")
            .arg(static_cast<qulonglong>(target), 0, 16)
            .arg(static_cast<qulonglong>(pasteTargetInputWindow_), 0, 16)
            .arg(static_cast<qulonglong>(focused), 0, 16)
            .arg(focusPreserved);
        if (focusPreserved) {
            if (useHeldControl && controlKeyPressed()) sendVWhileControlHeld();
            else sendControlV(temporarilyReleaseShift);
            return;
        }

        // The compositor really moved focus despite the non-activating hint.
        // Fall back to the established activation path only in that case.
        activateTarget(target);
        restorePasteWidgetFocus(target);
        waitForTargetAndPaste(target, request, 0, useHeldControl, temporarilyReleaseShift);
    }

    void restorePasteWidgetFocus(Window target) {
        QWidget *widget = pasteTargetWidget_.data();
        if (!widget || !widget->window()
                || static_cast<Window>(widget->window()->winId()) != target) return;
        widget->window()->raise();
        widget->window()->activateWindow();
        widget->setFocus(Qt::OtherFocusReason);
    }

    void restoreExternalInputFocus(Window target) {
        if (!display_ || pasteTargetWidget_ || pasteTargetInputWindow_ == None
                || pasteTargetInputWindow_ == target
                || pasteTargetInputWindow_ == static_cast<Window>(winId())) return;
        // Some X11 applications keep keyboard input in a child surface. Restore
        // that surface only after the WM has reactivated its top-level window.
        // Browser DOM inputs normally share the top-level X window; in that
        // case deliberately do nothing and let the browser restore activeElement.
        XSetInputFocus(display_, pasteTargetInputWindow_, RevertToPointerRoot, CurrentTime);
        XFlush(display_);
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
        XRaiseWindow(display_, target);
        // Never force an external application's top-level X window to become
        // the input window. Browsers use the normal WM activation sequence to
        // restore the focused DOM element; forcing the top-level here makes
        // online Excel fall back from its search field to the selected cell.
        if (target == static_cast<Window>(winId())) {
            XSetInputFocus(display_, target, RevertToPointerRoot, CurrentTime);
        }
        XFlush(display_);
    }

    void captureBackdrop(const QRect &band) {
        backdropShot_ = QImage();
        backdrop_ = QPixmap();
        if (band.isEmpty()) return;
        // QGuiApplication::screenAt() can return null on Qt 5.11/X11.
        QScreen *screen = QGuiApplication::primaryScreen();
        for (QScreen *candidate : QGuiApplication::screens()) {
            if (candidate->geometry().contains(band.center())) screen = candidate;
        }
        if (!screen) return;
        const QRect local = band.translated(-screen->geometry().topLeft());
        const QImage shot = screen->grabWindow(0, local.x(), local.y(),
                                               local.width(), local.height()).toImage();
        if (shot.isNull()) qWarning("ClipDeck: could not capture the Deck backdrop");
        backdropShot_ = shot;
        backdropOrigin_ = band.topLeft();
        backdropSize_ = band.size();
        rebuildBackdrop();
    }

    void rebuildBackdrop() {
        backdrop_ = material_.blur > 0
            ? frostedBackdrop(backdropShot_, backdropSize_, devicePixelRatioF(), material_.blur)
            : QPixmap();
    }

    void suspendKWinBlur() {
        clearMask();
        if (!display_) return;
        const Atom blur = XInternAtom(display_, "_KDE_NET_WM_BLUR_BEHIND_REGION", False);
        XDeleteProperty(display_, static_cast<Window>(winId()), blur);
        XFlush(display_);
    }

    void enableKWinBlur() {
        // Do not apply an XShape polygon.  UOS discretizes its rounded edge
        // into horizontal scan bands which appear as repeated top/bottom
        // lines. The translucent backing store already supplies per-pixel
        // rounded corners, including antialiasing.
        clearMask();
        if (!display_) return;
        // Never use whole-window opacity: that would also fade the cards.
        const Atom windowOpacity = XInternAtom(display_, "_NET_WM_WINDOW_OPACITY", False);
        XDeleteProperty(display_, static_cast<Window>(winId()), windowOpacity);
        // Explicitly keep compositing enabled for this top-level utility window.
        const Atom bypassCompositor = XInternAtom(display_, "_NET_WM_BYPASS_COMPOSITOR", False);
        const unsigned long compositorValue = 0;
        XChangeProperty(display_, static_cast<Window>(winId()), bypassCompositor,
                        XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(&compositorValue), 1);
        // On UOS the KWin blur-behind property makes this translucent surface
        // noticeably greyer and can leave repeated horizontal compositor
        // artifacts.  The unblurred state seen during live resize is the
        // intended material, so keep only per-pixel transparency.
        const Atom blur = XInternAtom(display_, "_KDE_NET_WM_BLUR_BEHIND_REGION", False);
        XDeleteProperty(display_, static_cast<Window>(winId()), blur);
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

    void setDeckInputHint(bool acceptsFocus) {
        if (!display_ || winId() == 0) return;
        XWMHints *hints = XGetWMHints(display_, static_cast<Window>(winId()));
        if (!hints) hints = XAllocWMHints();
        if (!hints) return;
        hints->flags |= InputHint;
        hints->input = acceptsFocus ? True : False;
        XSetWMHints(display_, static_cast<Window>(winId()), hints);
        XFree(hints);
        XFlush(display_);
    }

    void grabDeckKeyboard(int attempt) {
        if (!display_ || !isVisible()) return;
        const int result = XGrabKeyboard(display_, static_cast<Window>(winId()), False,
                                         GrabModeAsync, GrabModeAsync, CurrentTime);
        if (result == GrabSuccess) {
            keyboardGrabbed_ = true;
            qInfo() << "[ClipDeck][Focus] non-activating keyboard grab active";
            return;
        }
        if (attempt < 8) {
            QTimer::singleShot(20, this, [this, attempt] { grabDeckKeyboard(attempt + 1); });
            return;
        }
        // Some desktop components may already own an active keyboard grab.
        // Preserve Deck usability in that exceptional case, accepting that
        // the source application will temporarily lose focus.
        qWarning() << "[ClipDeck][Focus] keyboard grab failed, using activating fallback"
                   << result;
        nonActivatingPresentation_ = false;
        setDeckInputHint(true);
        requestDeckFocus();
    }

    // The editor, rename box and context menu are Qt::Popup windows: while
    // open they own the X keyboard grab, and closing them ungrabs it, which
    // also dropped the Deck's own grab. Keys then went to the application
    // underneath and the Deck stopped answering arrows, Space and Esc.
    bool execPopover(QDialog &dialog) {
        popoverOpen_ = true;
        const bool accepted = dialog.exec() == QDialog::Accepted;
        popoverOpen_ = false;
        restoreDeckKeyboard();
        return accepted;
    }

    void restoreDeckKeyboard() {
        if (!isVisible()) return;
        setFocus(Qt::OtherFocusReason);
        grabDeckKeyboard(0);
    }

    void releaseDeckKeyboard() {
        if (!display_ || !keyboardGrabbed_) return;
        XUngrabKeyboard(display_, CurrentTime);
        // XSync is intentional: Ctrl+V must not be synthesized until the X
        // server has actually processed the ungrab request.
        XSync(display_, False);
        keyboardGrabbed_ = false;
        qInfo() << "[ClipDeck][Focus] keyboard grab released";
    }

    void sendControlV(bool temporarilyReleaseShift = false) {
        if (!display_) {
            return;
        }
        const KeyCode control = XKeysymToKeycode(display_, XK_Control_L);
        const KeyCode v = XKeysymToKeycode(display_, XK_v);
        if (control == 0 || v == 0) {
            return;
        }
        std::vector<KeyCode> heldShiftKeys;
        if (temporarilyReleaseShift) {
            char keys[32]{};
            XQueryKeymap(display_, keys);
            for (const KeySym symbol : {XK_Shift_L, XK_Shift_R}) {
                const KeyCode code = XKeysymToKeycode(display_, symbol);
                if (code != 0 && (keys[code / 8] & (1 << (code % 8)))) {
                    heldShiftKeys.push_back(code);
                    XTestFakeKeyEvent(display_, code, False, CurrentTime);
                }
            }
        }
        XTestFakeKeyEvent(display_, control, True, CurrentTime);
        XTestFakeKeyEvent(display_, v, True, CurrentTime);
        XTestFakeKeyEvent(display_, v, False, CurrentTime);
        XTestFakeKeyEvent(display_, control, False, CurrentTime);
        for (const KeyCode code : heldShiftKeys) {
            XTestFakeKeyEvent(display_, code, True, CurrentTime);
        }
        XFlush(display_);
    }

    void sendVWhileControlHeld() {
        if (!display_) return;
        const KeyCode v = XKeysymToKeycode(display_, XK_v);
        if (v == 0) return;
        XTestFakeKeyEvent(display_, v, True, CurrentTime);
        XTestFakeKeyEvent(display_, v, False, CurrentTime);
        XFlush(display_);
    }

    bool controlKeyPressed() const {
        if (!display_) return false;
        char keys[32]{};
        XQueryKeymap(display_, keys);
        for (const KeySym symbol : {XK_Control_L, XK_Control_R}) {
            const KeyCode code = XKeysymToKeycode(display_, symbol);
            if (code != 0 && (keys[code / 8] & (1 << (code % 8)))) return true;
        }
        return false;
    }

    void waitForTargetAndPaste(Window target, PasteRequestPtr request, int attempt, bool useHeldControl = false,
                               bool temporarilyReleaseShift = false) {
        if (!display_ || target == None || !pasteRequestIsCurrent(request)) return;
        if (activeTopLevelWindow(display_) == target) {
            // The WM may acknowledge _NET_ACTIVE_WINDOW asynchronously. Set
            // the remembered Qt child focus again immediately before paste so
            // a QWebEngine editor receives Ctrl+V rather than the dialog shell.
            restorePasteWidgetFocus(target);
            restoreExternalInputFocus(target);
            if (!pasteRequestIsCurrent(request)) return;
            if (useHeldControl && controlKeyPressed()) sendVWhileControlHeld();
            else sendControlV(temporarilyReleaseShift);
            return;
        }
        if (attempt >= 40) {
            // Some UOS window managers update _NET_ACTIVE_WINDOW late or not
            // at all for utility windows. The target is the window captured
            // before Deck appeared, so make one final focused paste attempt
            // instead of silently dropping the operation.
            qWarning() << "[ClipDeck][Paste] active-window wait timed out; forcing target paste";
            activateTarget(target);
            restorePasteWidgetFocus(target);
            restoreExternalInputFocus(target);
            if (!pasteRequestIsCurrent(request)) return;
            if (useHeldControl && controlKeyPressed()) sendVWhileControlHeld();
            else sendControlV(temporarilyReleaseShift);
            return;
        }
        QTimer::singleShot(10, this,
                          [this, target, request, attempt, useHeldControl, temporarilyReleaseShift] {
            waitForTargetAndPaste(
                target, request, attempt + 1, useHeldControl, temporarilyReleaseShift);
        });
    }

    qint64 selectedDatabaseID() const {
        return selectedIndex_ >= 0 && selectedIndex_ < int(items_.size())
            ? items_[selectedIndex_].databaseID : -1;
    }

    // Cards use bounded summaries; only an explicit action needs the original
    // payload. Bind that load to identity and clipboard revision, never index.
    bool ensureSelectedPayload(std::function<void()> action, bool protectClipboard = false) {
        const quint64 generation = ++payloadRequestGeneration_;
        if (!pipeline_ || selectedIndex_ < 0 || selectedIndex_ >= int(items_.size())
            || items_[selectedIndex_].payloadComplete) return false;
        const qint64 id = selectedDatabaseID();
        const quint64 revision = clipboardRevision_;
        const QPointer<DeckWindow> guard(this);
        pipeline_->requestItem(id, [guard, id, generation, revision, protectClipboard,
                                    action = std::move(action)](QVector<ClipboardItem> items) {
            if (!guard || generation != guard->payloadRequestGeneration_
                || guard->selectedDatabaseID() != id || items.isEmpty()
                || (protectClipboard && revision != guard->clipboardRevision_)) return;
            guard->items_[guard->selectedIndex_] = std::move(items.first());
            action();
        });
        return true;
    }

    void recentHistoryChanged(quint64 sequence) {
        if (!pipeline_) return;
        const bool newCapture = sequence > latestCaptureSequence_;
        if (newCapture) {
            latestCaptureSequence_ = sequence;
            ++payloadRequestGeneration_;
            searchDebounce_->stop();
            searchAnimation_->stop();
            searchActive_ = false;
            searchExpansion_ = 0;
            searchQuery_.clear();
            const QSignalBlocker blocker(searchEdit_);
            searchEdit_->clear();
            searchEdit_->hide();
            selectedCategory_ = QStringLiteral("History");
            selectedIndex_ = -1;
            scrollOffset_ = 0;
            updateSearchGeometry();
        }
        if (!isVisible() || newCapture || items_.empty())
            applyHistoryItems(pipeline_->recentItems(), selectedDatabaseID(), 10);
        if (isVisible()) reload();
        // Metadata requests remain asynchronous network operations; the
        // capture callback itself never performs them or creates card widgets.
        for (const auto &item : pipeline_->recentItems()) {
            if (!metadataRequested_.contains(item.databaseID)) {
                metadataRequested_.insert(item.databaseID);
                fetchLinkMetadataIfNeeded(item);
            }
        }
    }

    void reload() {
        imageBatchLoading_ = false;
        ++imageLoadGeneration_;
        const bool categoryChanged = loadedCategory_ != selectedCategory_;
        if (categoryChanged) {
            loadedCategory_ = selectedCategory_;
            imageLoadLimit_ = kInitialImageCapacity;
        }
        const qint64 selectedID = selectedDatabaseID();
        QString repositoryCategory;
        if (selectedCategory_ == QStringLiteral("Text")) repositoryCategory = QStringLiteral("文本");
        else if (selectedCategory_ == QStringLiteral("Images")) repositoryCategory = QStringLiteral("图片");
        else if (selectedCategory_ == QStringLiteral("Files")) repositoryCategory = QStringLiteral("文件");
        else if (selectedCategory_ != QStringLiteral("History")) repositoryCategory = selectedCategory_;
        const int fetchLimit = selectedCategory_ == QStringLiteral("Images")
            ? std::min(kHistoryCapacity, std::max(kInitialImageCapacity, imageLoadLimit_))
            : kHistoryCapacity;
        const quint64 generation = ++historyQueryGeneration_;
        if (pipeline_) {
            if (!isVisible()) return;
            const QPointer<DeckWindow> guard(this);
            pipeline_->requestHistory(repositoryCategory, searchQuery_, fetchLimit, 0,
                [guard, generation, selectedID, fetchLimit](QVector<ClipboardItem> items) {
                    if (guard && generation == guard->historyQueryGeneration_) {
                        // Preserve navigation performed while the query was in flight.
                        const qint64 currentID = guard->selectedDatabaseID();
                        guard->applyHistoryItems(std::move(items), currentID >= 0 ? currentID : selectedID, fetchLimit);
                    }
                });
        } else applyHistoryItems(repository_->fetch(repositoryCategory, searchQuery_, fetchLimit), selectedID, fetchLimit);
    }

    void applyHistoryItems(QVector<ClipboardItem> items, qint64 selectedID, int fetchLimit) {
        items_ = items.toStdVector();
        QSet<qint64> retained;
        for (const auto &item : items_) retained.insert(item.databaseID);
        const auto prune = [&retained](QHash<qint64, QImage> &cache) {
            for (auto it = cache.begin(); it != cache.end();) {
                if (!retained.contains(it.key())) it = cache.erase(it);
                else ++it;
            }
        };
        prune(imageThumbnailCache_);
        prune(linkPreviewCache_);
        prune(linkIconCache_);
        imageLoadLimit_ = selectedCategory_ == QStringLiteral("Images")
            ? static_cast<int>(items_.size()) : kInitialImageCapacity;
        imageHasMore_ = selectedCategory_ == QStringLiteral("Images")
            && static_cast<int>(items_.size()) == fetchLimit
            && fetchLimit < kHistoryCapacity;
        selectedIndex_ = -1;
        if (selectedID >= 0) {
            for (int index = 0; index < static_cast<int>(items_.size()); ++index) {
                if (items_[index].databaseID == selectedID) {
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
    FilePasteController *filePaste_ = nullptr;
    ClipboardPipeline *pipeline_ = nullptr;
    quint64 historyQueryGeneration_ = 0;
    quint64 payloadRequestGeneration_ = 0;
    quint64 latestCaptureSequence_ = 0;
    QSet<qint64> metadataRequested_;
    QNetworkAccessManager network_;
    Window pasteTarget_ = None;
    Window pasteTargetInputWindow_ = None;
    QPointer<QWidget> pasteTargetWidget_;
    QPointer<QDialog> previewDialog_;
    // An editor, rename box or context menu is open (each runs its own loop).
    bool popoverOpen_ = false;
    std::function<void(QKeyEvent *)> previewKeyHandler_;
    std::vector<ClipboardItem> items_;
    QStringList categoryValuesCache_;
    QVector<QColor> categoryColorsCache_; // Automatic categories only, in Deck order.
    QString loadedCategory_;
    int imageLoadLimit_ = kInitialImageCapacity;
    int imageLoadGeneration_ = 0;
    bool imageHasMore_ = false;
    bool imageBatchLoading_ = false;
    int selectedIndex_ = -1;
    int scrollOffset_ = 0;
    bool writingHistoryClipboard_ = false;
    quint64 clipboardRevision_ = 0;
    quint64 pasteRequestGeneration_ = 0;
    qint64 focusAcquisitionDeadline_ = 0;
    bool nonActivatingPresentation_ = false;
    bool keyboardGrabbed_ = false;
    qint64 lastCardPressID_ = -1;
    qint64 lastCardPressAt_ = 0;
    QPoint lastCardPressGlobalPos_;
    qint64 pendingMousePasteID_ = -1;
    qint64 pendingFileConfirmID_ = -1;
    quint64 pendingFileConfirmRevision_ = 0;
    Qt::KeyboardModifiers pendingMousePasteModifiers_ = Qt::NoModifier;
    QPoint pendingMousePastePosition_;
    unsigned int lastPointerButtonMask_ = 0;
    bool resizing_ = false;
    int resizeStartGlobalY_ = 0;
    QRect resizeStartGeometry_;
    int categoryScrollOffset_ = 0;
    QVariantAnimation *categoryAnimation_ = nullptr;
    QLineEdit *searchEdit_ = nullptr;
    SearchFieldGlyph *searchLeadingGlyph_ = nullptr;
    SearchFieldGlyph *searchCloseGlyph_ = nullptr;
    QVariantAnimation *searchAnimation_ = nullptr;
    qreal searchExpansion_ = 0.0;
    QTimer *searchDebounce_ = nullptr;
    QTimer *blurUpdateTimer_ = nullptr;
    QImage backdropShot_;
    QSize backdropSize_;
    QPixmap backdrop_;
    DeckMaterial material_;
    QPoint backdropOrigin_;
    QTimer *resizeReleaseTimer_ = nullptr;
    QTimer *resizeFrameTimer_ = nullptr;
    QTimer *outsideClickTimer_ = nullptr;
    QHash<qint64, QImage> imageThumbnailCache_;
    QHash<qint64, QImage> linkPreviewCache_;
    QHash<qint64, QImage> linkIconCache_;
    QHash<QString, QImage> sourceIconRenderCache_;
    QHash<QString, QImage> textCardCache_;
    std::function<void(int)> settingsCallback_;
    QString selectedCategory_ = QStringLiteral("History");
    QString searchQuery_;
    bool searchActive_ = false;
    bool quickPasteOverlay_ = false;
    int pendingResizeHeight_ = 0;
};

void showSettings(HistoryRepository *repository, DeckWindow *deck, int initialPage = 0,
                  std::function<void()> shortcutChanged = {},
                  const QString &renderPath = QString(),
                  bool beginNewCategory = false,
                  std::function<void(bool)> shortcutRecordingChanged = {},
                  bool enableCommandInput = false) {
    QSettings settings;
    const auto refreshDeck = [deck] { if (deck) deck->refresh(); };
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("ClipDeck 设置"));
    // The category editor uses the same logical three-column geometry as the
    // macOS settings window: navigation, category list, and a full editor.
    dialog.resize(1220, 880);
    dialog.setMinimumSize(1080, 720);
    dialog.setStyleSheet(QStringLiteral(
        "QDialog{background:#1d1d1f;color:#eeeeef;} QDialog QWidget{color:#eeeeef;}"
        "QListWidget#sidebar{background:#28282a;border:0;padding:18px 10px;color:#aaa;font-size:17px;font-weight:600;}"
        "QListWidget#sidebar::item{height:56px;padding-left:18px;border-radius:12px;color:#cfcfd3;}"
        "QListWidget#sidebar::item:selected{background:#4a4a4d;color:white;}"
        "QListWidget#categoryList,QPlainTextEdit,QLineEdit{background:#151517;color:#eee;border:1px solid #343438;"
        "border-radius:9px;padding:8px;outline:0;selection-background-color:#48484d;selection-color:white;}"
        "QPlainTextEdit:!focus,QLineEdit:!focus{background:#151517;border-color:#343438;}"
        "QPlainTextEdit:focus,QLineEdit:focus{background:#19191c;border-color:#1683ff;selection-background-color:#1683ff;}"
        "QListWidget#categoryList::item{height:34px;padding-left:8px;}"
        "QListWidget#categoryList::item:selected{background:#176fd1;color:white;}"
        "QMenu{background:#29292c;color:#eee;border:1px solid #444;padding:5px;}"
        "QMenu::item:selected{background:#176fd1;color:white;}"
        "QWidget#page{background:#1d1d1f;}"
        "QWidget#categorySidebar{background:#1b1b1d;}"
        "QWidget#categorySidebar,QWidget#categorySidebar QLabel,QWidget#categorySidebar QListWidget{color:#dedee1;}"
        "QListWidget#categoryList{background:transparent;border:0;border-radius:0;padding:0;outline:0;}"
        "QListWidget#categoryList::item{background:transparent;padding:0;}"
        "QListWidget#categoryList::item:selected{background:transparent;}"
        "QFrame#categoryDivider{color:#343437;background:#343437;max-width:1px;border:0;}"
        "QScrollArea#categoryEditorScroll,QWidget#categoryEditor,QScrollArea#settingsScroll{background:#1d1d1f;border:0;}"
        "QWidget#categoryEditor,QWidget#categoryEditor QLabel,QWidget#categoryEditor QPushButton{color:#dedee1;}"
        "QLabel#categoryEditorTitle{font-size:25px;font-weight:700;color:#eeeeef;}"
        "QLabel#categorySectionTitle{font-size:18px;font-weight:700;color:#e8e8ea;}"
        "QLabel#categoryRowLabel{font-size:15px;font-weight:600;color:#e2e2e5;}"
        "QFrame#categorySection{background:#202022;border:1px solid #2b2b2e;border-radius:14px;}"
        "QFrame#categorySection QFrame{color:#38383c;}"
        "QFrame#categorySection QPlainTextEdit{background:transparent;border:0;border-radius:0;padding:12px 16px;}"
        "QFrame#categorySection QPlainTextEdit:focus{background:#232326;border:0;}"
        "QFrame#categorySection QLineEdit,QFrame#categorySection QComboBox{color:#eeeeef;}"
        "QPlainTextEdit#categoryScript,QPlainTextEdit#categoryTestText{color:#eeeeef;}"
        "QPushButton#categoryHeaderButton{background:transparent;border:0;padding:0;}"
        "QPushButton#categoryHeaderButton:hover{background:#343437;}"
        "QPushButton#categorySave{background:#087ff5;color:white;font-weight:600;padding:9px 18px;}"
        "QPushButton#categorySave:hover{background:#168cff;}"
        "QLabel{color:#dedee1;font-size:14px;} QLabel#title{font-size:30px;font-weight:700;}"
        "QLabel#subtitle{color:#8f8f94;font-size:16px;}"
        "QGroupBox{background:#202022;border:1px solid #2b2b2e;border-radius:14px;margin-top:14px;padding:22px 18px 16px;}"
        "QGroupBox::title{color:#e5e5e7;font-size:18px;font-weight:700;subcontrol-origin:margin;left:4px;}"
        "QComboBox,QSpinBox{background:#303033;color:#eee;border:1px solid #3b3b3f;border-radius:9px;"
        "padding:8px 34px 8px 12px;min-width:130px;outline:0;selection-background-color:#1683ff;}"
        "QComboBox:hover,QSpinBox:hover{background:#39393d;border-color:#55555a;}"
        "QComboBox:!focus,QSpinBox:!focus{background:#303033;border-color:#3b3b3f;}"
        "QComboBox:focus,QSpinBox:focus{background:#39393d;border-color:#1683ff;}"
        "QComboBox::drop-down{subcontrol-origin:padding;subcontrol-position:top right;width:30px;border:0;}"
        "QComboBox::down-arrow{image:none;width:0;height:0;}"
        "QComboBox QAbstractItemView{background:#29292c;color:#eee;border:1px solid #49494e;border-radius:9px;padding:5px;selection-background-color:#176fd1;selection-color:white;outline:0;}"
        "QPushButton{background:#343437;color:#eee;border:1px solid transparent;border-radius:9px;padding:9px 16px;outline:0;}"
        "QPushButton:hover{background:#444448;} QPushButton:focus{border-color:#1683ff;}"
        "QPushButton[recording=\"true\"]{background:#176fd1;border:1px solid #62aaff;}"
        "QCheckBox{spacing:12px;font-size:15px;color:#eee;} QCheckBox:disabled{color:#8f8f94;}"));
    dialog.setStyleSheet(dialog.styleSheet() + QStringLiteral(
        "QScrollBar:vertical{background:#1b1b1d;width:9px;margin:2px;border:0;}"
        "QScrollBar::handle:vertical{background:#55555a;min-height:28px;border-radius:4px;}"
        "QScrollBar:horizontal{background:#1b1b1d;height:9px;margin:2px;border:0;}"
        "QScrollBar::handle:horizontal{background:#55555a;min-width:28px;border-radius:4px;}"
        "QScrollBar::add-line,QScrollBar::sub-line{width:0;height:0;background:transparent;}"
        "QScrollBar::add-page,QScrollBar::sub-page{background:transparent;}"));

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

    // A settings pane as on macOS and Windows: title and subtitle, then
    // sections (bold title above a rounded group of 52 px rows, control on the
    // right). At most 760 wide, centred once the window is wider, scrolls.
    struct SettingsPage {
        QWidget *body;
        QVBoxLayout *layout;
    };
    auto makePage = [&](const QString &title, const QString &subtitle) {
        QScrollArea *scroll = new QScrollArea(pages);
        scroll->setObjectName(QStringLiteral("settingsScroll"));
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QWidget *canvas = new QWidget(scroll);
        canvas->setObjectName(QStringLiteral("page"));
        QHBoxLayout *centring = new QHBoxLayout(canvas);
        centring->setContentsMargins(38, 26, 38, 26);
        QWidget *body = new QWidget(canvas);
        body->setObjectName(QStringLiteral("page"));
        body->setMaximumWidth(760 - 76);
        centring->addStretch(0);
        centring->addWidget(body, 1);
        centring->addStretch(0);
        QVBoxLayout *layout = new QVBoxLayout(body);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(10);
        QLabel *heading = new QLabel(title, body);
        heading->setObjectName(QStringLiteral("categoryEditorTitle"));
        QLabel *detail = new QLabel(subtitle, body);
        detail->setObjectName(QStringLiteral("subtitle"));
        detail->setStyleSheet(QStringLiteral("font-size:13px;"));
        layout->addWidget(heading);
        layout->addWidget(detail);
        layout->addSpacing(14);
        scroll->setWidget(canvas);
        pages->addWidget(scroll);
        return SettingsPage{body, layout};
    };
    // A section title and its rounded group; rows go into the returned layout.
    auto addSettingsSection = [](const SettingsPage &page, const QString &title) {
        QLabel *label = new QLabel(title, page.body);
        label->setObjectName(QStringLiteral("categorySectionTitle"));
        page.layout->addWidget(label);
        QFrame *section = new QFrame(page.body);
        section->setObjectName(QStringLiteral("categorySection"));
        QVBoxLayout *rows = new QVBoxLayout(section);
        rows->setContentsMargins(16, 0, 16, 0);
        rows->setSpacing(0);
        page.layout->addWidget(section);
        page.layout->addSpacing(14);
        return rows;
    };
    // A row: title (and optional secondary line) on the left, control on the
    // right, and an inset divider above every row but the first.
    auto addSettingsRow = [](QVBoxLayout *rows, const QString &title, QWidget *control,
                             const QString &detail = QString()) {
        QWidget *section = rows->parentWidget();
        if (rows->count() > 0) {
            QFrame *divider = new QFrame(section);
            divider->setFrameShape(QFrame::HLine);
            rows->addWidget(divider);
        }
        QHBoxLayout *row = new QHBoxLayout;
        row->setContentsMargins(0, 8, 0, 8);
        QVBoxLayout *labels = new QVBoxLayout;
        labels->setSpacing(2);
        QLabel *label = new QLabel(title, section);
        label->setObjectName(QStringLiteral("categoryRowLabel"));
        labels->addWidget(label);
        if (!detail.isEmpty()) {
            QLabel *secondary = new QLabel(detail, section);
            secondary->setObjectName(QStringLiteral("subtitle"));
            secondary->setStyleSheet(QStringLiteral("color:#9a9aa0;font-size:12px;"));
            secondary->setWordWrap(true);
            labels->addWidget(secondary);
        }
        row->addLayout(labels, 1);
        if (control) {
            control->setParent(section);
            row->addWidget(control, 0, Qt::AlignRight | Qt::AlignVCenter);
        }
        rows->addLayout(row);
        return row;
    };
    // A secondary note under a section, outside its rounded group.
    auto addSettingsNote = [](const SettingsPage &page, const QString &text) {
        QLabel *note = new QLabel(text, page.body);
        note->setObjectName(QStringLiteral("subtitle"));
        note->setStyleSheet(QStringLiteral("color:#8f8f94;font-size:12px;"));
        note->setWordWrap(true);
        page.layout->insertWidget(page.layout->count() - 1, note);
    };

    SettingsPage generalPage = makePage(QStringLiteral("通用"), QStringLiteral("设置 ClipDeck 的默认行为"));
    QVBoxLayout *pasteRows = addSettingsSection(generalPage, QStringLiteral("粘贴"));
    QComboBox *pasteFormat = new StyledComboBox(generalPage.body);
    pasteFormat->addItems({QStringLiteral("纯文本"), QStringLiteral("保留原格式")});
    pasteFormat->setCurrentIndex(settings.value(QStringLiteral("paste/preserveFormatting"), false).toBool() ? 1 : 0);
    addSettingsRow(pasteRows, QStringLiteral("默认粘贴格式"), pasteFormat,
                   QStringLiteral("纯文本会移除字体、颜色和段落样式"));
    QVBoxLayout *startupRows = addSettingsSection(generalPage, QStringLiteral("启动"));
    QCheckBox *launchAtLogin = new QCheckBox(generalPage.body);
    launchAtLogin->setChecked(QFile::exists(QDir::home().filePath(QStringLiteral(".config/autostart/clipdeck.desktop"))));
    QObject::connect(launchAtLogin, &QCheckBox::toggled, &dialog,
                     [launchAtLogin, &dialog](bool enabled) {
        if (setLaunchAtLogin(enabled)) return;
        const QSignalBlocker blocker(launchAtLogin);
        launchAtLogin->setChecked(!enabled);
        showClipDeckAlert(&dialog, QStringLiteral("无法更改开机启动"),
                          enabled ? QStringLiteral("无法创建 UOS 开机启动项。")
                                  : QStringLiteral("无法移除 UOS 开机启动项。"),
                          QStringLiteral("知道了"), QString(), AlertTone::Warning);
    });
    addSettingsRow(startupRows, QStringLiteral("登录时自动启动"), launchAtLogin);
    QVBoxLayout *historyRows = addSettingsSection(generalPage, QStringLiteral("保留历史"));
    const int savedRetention = settings.value(QStringLiteral("history/retentionDays"), 30).toInt();
    RetentionControl *retention = new RetentionControl(savedRetention, generalPage.body);
    historyRows->setContentsMargins(16, 12, 16, 12);
    historyRows->addWidget(retention);
    addSettingsNote(generalPage, QStringLiteral("期限以项目最近一次复制时间为起点"));
    auto selectedRetentionDays = [retention] { return retention->value(); };
    int currentRetentionDays = savedRetention;
    retention->setChangeCallback([&, retention](int days) {
        if (days == currentRetentionDays) return;
        const bool shorter = isShorterRetention(currentRetentionDays, days);
        if (shorter) {
            QString title = QStringLiteral("%1天").arg(days);
            for (const RetentionOption &option : retentionOptions()) {
                if (option.days == days) { title = option.title; break; }
            }
            if (!showClipDeckAlert(&dialog, QStringLiteral("缩短历史保留期限？"),
                    QStringLiteral("将永久删除最近复制时间早于%1的历史项目，然后应用新期限。此操作无法撤销。")
                        .arg(title),
                    QStringLiteral("删除并应用"), QStringLiteral("取消"),
                    AlertTone::Destructive)) {
                retention->setValue(currentRetentionDays);
                return;
            }
            repository->clearExpired(days);
            refreshDeck();
        }
        currentRetentionDays = days;
        settings.setValue(QStringLiteral("history/retentionDays"), days);
        settings.sync();
    });
    generalPage.layout->addStretch();

    SettingsPage imagePage = makePage(QStringLiteral("图片"), QStringLiteral("控制图片剪贴板内容的记录方式"));
    QVBoxLayout *imageRows = addSettingsSection(imagePage, QStringLiteral("图片历史"));
    QCheckBox *recordImages = new QCheckBox(imagePage.body);
    recordImages->setChecked(settings.value(QStringLiteral("clipboard/recordsImages"), true).toBool());
    QComboBox *maximumImage = new StyledComboBox(imagePage.body);
    for (const int size : {1, 2, 5, 10, 20}) maximumImage->addItem(QStringLiteral("%1 MB").arg(size), size);
    maximumImage->setCurrentIndex(std::max(0, maximumImage->findData(
        settings.value(QStringLiteral("clipboard/maximumImageMegabytes"), 10).toInt())));
    addSettingsRow(imageRows, QStringLiteral("记录图片"), recordImages);
    addSettingsRow(imageRows, QStringLiteral("最大图片大小"), maximumImage);
    addSettingsNote(imagePage, QStringLiteral("超过大小限制的图片不会被记录。"));
    imagePage.layout->addStretch();

    SettingsPage keyboardPage = makePage(QStringLiteral("键盘快捷键"), QStringLiteral("点击快捷键区域，然后按下新的组合键 · Esc 取消录制"));
    QWidget *keyBox = keyboardPage.body;
    ShortcutCaptureButton *showDeckShortcut = new ShortcutCaptureButton(
        configuredShortcut(QStringLiteral("shortcuts/showDeck"), defaultShowDeckShortcut()), keyBox);
    ShortcutCaptureButton *previousCategoryShortcut = new ShortcutCaptureButton(
        configuredShortcut(QStringLiteral("shortcuts/previousCategory"),
                           QKeySequence(Qt::CTRL | Qt::Key_Left)), keyBox);
    ShortcutCaptureButton *nextCategoryShortcut = new ShortcutCaptureButton(
        configuredShortcut(QStringLiteral("shortcuts/nextCategory"),
                           QKeySequence(Qt::CTRL | Qt::Key_Right)), keyBox);
    auto applyShortcut = [&settings, shortcutChanged](const QString &key,
                                                       const QKeySequence &sequence) {
        settings.setValue(key, sequence.toString(QKeySequence::PortableText));
        settings.sync();
        if (shortcutChanged) shortcutChanged();
    };
    showDeckShortcut->setChangeCallback([&applyShortcut](const QKeySequence &sequence) {
        applyShortcut(QStringLiteral("shortcuts/showDeck"), sequence);
    });
    previousCategoryShortcut->setChangeCallback([&applyShortcut](const QKeySequence &sequence) {
        applyShortcut(QStringLiteral("shortcuts/previousCategory"), sequence);
    });
    nextCategoryShortcut->setChangeCallback([&applyShortcut](const QKeySequence &sequence) {
        applyShortcut(QStringLiteral("shortcuts/nextCategory"), sequence);
    });
    for (ShortcutCaptureButton *capture : {showDeckShortcut, previousCategoryShortcut,
                                            nextCategoryShortcut}) {
        capture->setRecordingCallback(shortcutRecordingChanged);
    }
    auto modifierPicker = [keyBox](Qt::KeyboardModifiers current) {
        QComboBox *picker = new StyledComboBox(keyBox);
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
    QObject::connect(quickPasteModifier, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     [&settings, quickPasteModifier](int) {
        settings.setValue(QStringLiteral("shortcuts/quickPasteModifier"),
                          quickPasteModifier->currentData().toInt());
        settings.sync();
    });
    QObject::connect(plainTextModifier, QOverload<int>::of(&QComboBox::currentIndexChanged),
                     [&settings, plainTextModifier](int) {
        settings.setValue(QStringLiteral("shortcuts/plainTextModifier"),
                          plainTextModifier->currentData().toInt());
        settings.sync();
    });
    QVBoxLayout *globalRows = addSettingsSection(keyboardPage, QStringLiteral("全局"));
    addSettingsRow(globalRows, QStringLiteral("启动 ClipDeck"), showDeckShortcut);
    QVBoxLayout *deckRows = addSettingsSection(keyboardPage, QStringLiteral("Deck"));
    addSettingsRow(deckRows, QStringLiteral("显示下一个类型"), nextCategoryShortcut);
    addSettingsRow(deckRows, QStringLiteral("显示上一个类型"), previousCategoryShortcut);
    addSettingsRow(deckRows, QStringLiteral("快速粘贴"), quickPasteModifier,
                   QStringLiteral("按住修饰键再按 1…9"));
    addSettingsRow(deckRows, QStringLiteral("纯文本模式"), plainTextModifier,
                   QStringLiteral("按住修饰键再粘贴"));
    QVBoxLayout *navigationRows = addSettingsSection(keyboardPage, QStringLiteral("固定按键"));
    addSettingsRow(navigationRows, QStringLiteral("选择内容"), new QLabel(QStringLiteral("← / →"), keyBox));
    addSettingsRow(navigationRows, QStringLiteral("粘贴"), new QLabel(QStringLiteral("Enter"), keyBox));
    addSettingsRow(navigationRows, QStringLiteral("搜索"), new QLabel(QStringLiteral("Ctrl + F"), keyBox));
    QPushButton *resetShortcuts = new QPushButton(QStringLiteral("将快捷方式重置为默认"), keyBox);
    QHBoxLayout *resetRow = new QHBoxLayout;
    resetRow->addStretch();
    resetRow->addWidget(resetShortcuts);
    keyboardPage.layout->addLayout(resetRow);
    QObject::connect(resetShortcuts, &QPushButton::clicked, [&] {
        showDeckShortcut->setKeySequence(defaultShowDeckShortcut());
        previousCategoryShortcut->setKeySequence(QKeySequence(Qt::CTRL | Qt::Key_Left));
        nextCategoryShortcut->setKeySequence(QKeySequence(Qt::CTRL | Qt::Key_Right));
        quickPasteModifier->setCurrentIndex(quickPasteModifier->findData(
            static_cast<int>(Qt::ControlModifier)));
        plainTextModifier->setCurrentIndex(plainTextModifier->findData(
            static_cast<int>(Qt::ShiftModifier)));
    });
    keyboardPage.layout->addStretch();

    QWidget *categoryPageWidget = new QWidget(pages);
    categoryPageWidget->setObjectName(QStringLiteral("page"));
    QHBoxLayout *categoryLayout = new QHBoxLayout(categoryPageWidget);
    categoryLayout->setContentsMargins(0, 0, 0, 0);
    categoryLayout->setSpacing(0);
    QWidget *categorySidebar = new QWidget(categoryPageWidget);
    categorySidebar->setObjectName(QStringLiteral("categorySidebar"));
    categorySidebar->setFixedWidth(220);
    QVBoxLayout *categorySidebarLayout = new QVBoxLayout(categorySidebar);
    categorySidebarLayout->setContentsMargins(10, 0, 10, 12);
    categorySidebarLayout->setSpacing(0);
    QHBoxLayout *categoryHeader = new QHBoxLayout;
    QLabel *categoryHeaderTitle = new QLabel(QStringLiteral("分类"), categorySidebar);
    QFont categoryHeaderFont = categoryHeaderTitle->font();
    categoryHeaderFont.setPointSize(14);
    categoryHeaderFont.setBold(true);
    categoryHeaderTitle->setFont(categoryHeaderFont);
    categoryHeaderTitle->setMinimumHeight(62);
    QPushButton *restoreCategories = new CategoryHeaderButton(
        CategoryHeaderButton::Symbol::Restore, categorySidebar);
    restoreCategories->setToolTip(QStringLiteral("还原预制分类"));
    QPushButton *newCategory = new CategoryHeaderButton(
        CategoryHeaderButton::Symbol::Add, categorySidebar);
    newCategory->setToolTip(QStringLiteral("新增分类"));
    categoryHeader->addWidget(categoryHeaderTitle);
    categoryHeader->addStretch();
    categoryHeader->addWidget(restoreCategories);
    categoryHeader->addWidget(newCategory);
    categoryHeader->setContentsMargins(6, 0, 0, 0);
    categoryHeader->setSpacing(4);
    categoryHeader->setAlignment(Qt::AlignVCenter);
    categorySidebarLayout->addLayout(categoryHeader);
    categorySidebarLayout->setStretch(0, 0);
    CategoryListWidget *categoryList = new CategoryListWidget(categorySidebar);
    categoryList->setObjectName(QStringLiteral("categoryList"));
    categoryList->setItemDelegate(new CategoryListDelegate(categoryList));
    categoryList->setSpacing(3);
    categoryList->setUniformItemSizes(true);
    categoryList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    categoryList->setDragDropMode(QAbstractItemView::InternalMove);
    categoryList->setDefaultDropAction(Qt::MoveAction);
    categoryList->setDragEnabled(true);
    categoryList->setAcceptDrops(true);
    categoryList->setDropIndicatorShown(true);
    categorySidebarLayout->addWidget(categoryList, 1);

    QFrame *categoryDivider = new QFrame(categoryPageWidget);
    categoryDivider->setFrameShape(QFrame::VLine);
    categoryDivider->setObjectName(QStringLiteral("categoryDivider"));

    QScrollArea *categoryScroll = new QScrollArea(categoryPageWidget);
    categoryScroll->setObjectName(QStringLiteral("categoryEditorScroll"));
    categoryScroll->setWidgetResizable(true);
    categoryScroll->setFrameShape(QFrame::NoFrame);
    categoryScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QWidget *categoryEditorWidget = new QWidget(categoryScroll);
    categoryEditorWidget->setObjectName(QStringLiteral("categoryEditor"));
    QVBoxLayout *categoryEditor = new QVBoxLayout(categoryEditorWidget);
    categoryEditor->setContentsMargins(38, 26, 38, 26);
    categoryEditor->setSpacing(10);

    QLabel *editorTitle = new QLabel(QStringLiteral("编辑分类"), categoryEditorWidget);
    editorTitle->setObjectName(QStringLiteral("categoryEditorTitle"));
    QLabel *editorSubtitle = new QLabel(
        QStringLiteral("调整名称、颜色、卡片显示方式和自动分类规则"), categoryEditorWidget);
    editorSubtitle->setObjectName(QStringLiteral("subtitle"));
    categoryEditor->addWidget(editorTitle);
    categoryEditor->addWidget(editorSubtitle);
    categoryEditor->addSpacing(14);

    auto addSectionTitle = [&](const QString &text) {
        QLabel *label = new QLabel(text, categoryEditorWidget);
        label->setObjectName(QStringLiteral("categorySectionTitle"));
        categoryEditor->addWidget(label);
    };
    auto makeSection = [&]() {
        QFrame *section = new QFrame(categoryEditorWidget);
        section->setObjectName(QStringLiteral("categorySection"));
        return section;
    };

    addSectionTitle(QStringLiteral("基本信息"));
    QFrame *basicSection = makeSection();
    QVBoxLayout *basicLayout = new QVBoxLayout(basicSection);
    basicLayout->setContentsMargins(16, 0, 16, 0);
    basicLayout->setSpacing(0);
    QHBoxLayout *nameRow = new QHBoxLayout;
    QLabel *nameLabel = new QLabel(QStringLiteral("分类名称"), basicSection);
    nameLabel->setObjectName(QStringLiteral("categoryRowLabel"));
    nameLabel->setStyleSheet(QStringLiteral("color:#e6e6e8;"));
    QLineEdit *categoryName = new QLineEdit(basicSection);
    categoryName->setPlaceholderText(QStringLiteral("分类名称"));
    categoryName->setFixedWidth(270);
    nameRow->addWidget(nameLabel);
    nameRow->addStretch();
    nameRow->addWidget(categoryName);
    nameRow->setContentsMargins(0, 8, 0, 8);
    basicLayout->addLayout(nameRow);
    QFrame *basicDivider = new QFrame(basicSection);
    basicDivider->setFrameShape(QFrame::HLine);
    basicLayout->addWidget(basicDivider);
    // A row title with a secondary line under it.
    auto makeRowTitle = [](const QString &title, QLabel *detail, QWidget *parent) {
        QVBoxLayout *column = new QVBoxLayout;
        column->setSpacing(2);
        QLabel *label = new QLabel(title, parent);
        label->setObjectName(QStringLiteral("categoryRowLabel"));
        label->setStyleSheet(QStringLiteral("color:#e6e6e8;"));
        detail->setObjectName(QStringLiteral("subtitle"));
        detail->setStyleSheet(QStringLiteral("color:#9a9aa0;"));
        column->addWidget(label);
        column->addWidget(detail);
        return column;
    };

    // The category's colour is kept in its icon field as "#RRGGBB".
    QLineEdit *categoryIcon = new QLineEdit(categoryEditorWidget);
    categoryIcon->hide();
    QHBoxLayout *colorRow = new QHBoxLayout;
    colorRow->addLayout(makeRowTitle(QStringLiteral("颜色"),
        new QLabel(QStringLiteral("列表和 Deck 分类栏中显示的圆点颜色"), basicSection), basicSection));
    colorRow->addStretch();
    QButtonGroup *colorButtons = new QButtonGroup(basicSection);
    colorButtons->setExclusive(true);
    for (const QString &color : HistoryRepository::categoryColorPalette()) {
        QPushButton *swatch = new QPushButton(basicSection);
        swatch->setCheckable(true);
        swatch->setFixedSize(24, 24);
        swatch->setCursor(Qt::PointingHandCursor);
        swatch->setProperty("categoryColor", color);
        swatch->setStyleSheet(QStringLiteral(
            "QPushButton{background:%1;border:4px solid #2b2b2e;border-radius:12px;padding:0;min-width:0;}"
            "QPushButton:checked{border:2px solid white;}").arg(color));
        colorButtons->addButton(swatch);
        colorRow->addWidget(swatch);
        colorRow->addSpacing(4);
    }
    // Custom colour: a hue wheel until a colour outside the presets is chosen,
    // then that colour.
    QPushButton *customColor = new QPushButton(basicSection);
    customColor->setCheckable(true);
    customColor->setFixedSize(24, 24);
    customColor->setCursor(Qt::PointingHandCursor);
    customColor->setToolTip(QStringLiteral("自定义颜色"));
    colorRow->addWidget(customColor);
    auto showCategoryColor = [colorButtons, categoryIcon, customColor](const QString &color) {
        categoryIcon->setText(color);
        bool preset = false;
        // An exclusive group refuses to uncheck its checked button.
        colorButtons->setExclusive(false);
        for (QAbstractButton *button : colorButtons->buttons()) {
            const bool chosen = button->property("categoryColor").toString().compare(color, Qt::CaseInsensitive) == 0;
            button->setChecked(chosen);
            preset = preset || chosen;
        }
        colorButtons->setExclusive(true);
        const bool custom = !preset && !color.isEmpty();
        customColor->setChecked(custom);
        customColor->setStyleSheet(QStringLiteral(
            "QPushButton{background:%1;border:4px solid #2b2b2e;border-radius:12px;padding:0;min-width:0;}"
            "QPushButton:checked{border:2px solid white;}")
            .arg(custom ? color : QStringLiteral(
                "qconicalgradient(cx:0.5,cy:0.5,angle:0,stop:0 #ff0000,stop:0.17 #ff00ff,stop:0.33 #0000ff,"
                "stop:0.5 #00ffff,stop:0.67 #00ff00,stop:0.83 #ffff00,stop:1 #ff0000)")));
    };
    QObject::connect(colorButtons, qOverload<QAbstractButton *>(&QButtonGroup::buttonClicked),
                     [showCategoryColor](QAbstractButton *button) {
        showCategoryColor(button->property("categoryColor").toString());
    });
    // Qt's colour panel (saturation/brightness field, hue strip, hex field)
    // embedded in a popover under the swatch, not a separate dialog window.
    QObject::connect(customColor, &QPushButton::clicked, [customColor, categoryIcon, showCategoryColor] {
        showCategoryColor(categoryIcon->text());
        QFrame *popover = new QFrame(customColor, Qt::Popup | Qt::FramelessWindowHint);
        popover->setAttribute(Qt::WA_DeleteOnClose);
        popover->setObjectName(QStringLiteral("colorPopover"));
        popover->setStyleSheet(QStringLiteral(
            "QFrame#colorPopover{background:#2e2e32;border:1px solid rgba(255,255,255,30);border-radius:10px;}"
            "QWidget{color:#eeeeef;}"
            "QLineEdit,QSpinBox{background:rgba(0,0,0,64);border:1px solid rgba(255,255,255,30);"
            "border-radius:5px;padding:2px 4px;}"));
        QVBoxLayout *layout = new QVBoxLayout(popover);
        layout->setContentsMargins(10, 10, 10, 10);
        QColorDialog *panel = new QColorDialog(QColor(categoryIcon->text()), popover);
        panel->setWindowFlags(Qt::Widget);
        panel->setOptions(QColorDialog::NoButtons | QColorDialog::DontUseNativeDialog);
        layout->addWidget(panel);
        QObject::connect(panel, &QColorDialog::currentColorChanged, [showCategoryColor](const QColor &color) {
            showCategoryColor(color.name(QColor::HexRgb).toUpper());
        });
        popover->adjustSize();
        QPoint origin = customColor->mapToGlobal(QPoint(customColor->width(), customColor->height() + 8))
            - QPoint(popover->width(), 0);
        // QWidget::screen() needs Qt 5.14 and screenAt() can be null on 5.11/X11.
        QScreen *target = QGuiApplication::primaryScreen();
        for (QScreen *candidate : QGuiApplication::screens()) {
            if (candidate->geometry().contains(customColor->mapToGlobal(QPoint()))) target = candidate;
        }
        const QRect screen = target ? target->availableGeometry() : QRect(origin, popover->size());
        origin.setX(std::clamp(origin.x(), screen.left() + 8, screen.right() - popover->width() - 8));
        if (origin.y() + popover->height() > screen.bottom())
            origin.setY(customColor->mapToGlobal(QPoint(0, 0)).y() - popover->height() - 8);
        popover->move(origin);
        popover->show();
    });
    colorRow->setContentsMargins(0, 8, 0, 8);
    basicLayout->addLayout(colorRow);
    categoryEditor->addWidget(basicSection);
    categoryEditor->addSpacing(14);

    addSectionTitle(QStringLiteral("卡片显示"));
    QFrame *displaySection = makeSection();
    QVBoxLayout *displayLayout = new QVBoxLayout(displaySection);
    displayLayout->setContentsMargins(16, 0, 16, 0);
    // Text cards come in four types, stored as the category's preview_language;
    // code always detects its language.
    auto previewLanguageState = std::make_shared<QString>(QStringLiteral("text"));
    QHBoxLayout *cardTypeRow = new QHBoxLayout;
    QLabel *cardTypeDetail = new QLabel(displaySection);
    cardTypeRow->addLayout(makeRowTitle(QStringLiteral("卡片类型"), cardTypeDetail, displaySection));
    cardTypeRow->addStretch();
    QFrame *cardTypeTrack = new QFrame(displaySection);
    cardTypeTrack->setObjectName(QStringLiteral("cardTypeTrack"));
    cardTypeTrack->setStyleSheet(QStringLiteral(
        "QFrame#cardTypeTrack{background:rgba(255,255,255,20);border-radius:7px;}"
        "QFrame#cardTypeTrack QPushButton{background:transparent;color:rgba(255,255,255,180);border:0;"
        "border-radius:5px;min-width:72px;min-height:24px;padding:0;}"
        "QFrame#cardTypeTrack QPushButton:checked{background:rgba(255,255,255,51);color:white;font-weight:600;}"));
    QHBoxLayout *cardTypeLayout = new QHBoxLayout(cardTypeTrack);
    cardTypeLayout->setContentsMargins(2, 2, 2, 2);
    cardTypeLayout->setSpacing(0);
    QButtonGroup *cardTypeButtons = new QButtonGroup(cardTypeTrack);
    cardTypeButtons->setExclusive(true);
    for (const auto &choice : HistoryRepository::cardTypeChoices()) {
        QPushButton *segment = new QPushButton(choice.label, cardTypeTrack);
        segment->setCheckable(true);
        segment->setCursor(Qt::PointingHandCursor);
        segment->setProperty("cardType", choice.value);
        segment->setProperty("detail", choice.detail);
        cardTypeButtons->addButton(segment);
        cardTypeLayout->addWidget(segment);
    }
    cardTypeRow->addWidget(cardTypeTrack);
    cardTypeRow->setContentsMargins(0, 8, 0, 8);
    displayLayout->addLayout(cardTypeRow);

    auto showPreviewLanguage = [=](const QString &language) {
        *previewLanguageState = language.isEmpty() ? QStringLiteral("text") : language;
        const QString cardType = HistoryRepository::cardTypeOf(*previewLanguageState);
        for (QAbstractButton *button : cardTypeButtons->buttons()) {
            if (button->property("cardType").toString() != cardType) continue;
            button->setChecked(true);
            cardTypeDetail->setText(button->property("detail").toString());
        }
    };
    QObject::connect(cardTypeButtons, qOverload<QAbstractButton *>(&QButtonGroup::buttonClicked),
                     [=](QAbstractButton *button) {
        showPreviewLanguage(HistoryRepository::previewLanguageForCardType(
            button->property("cardType").toString(), *previewLanguageState));
    });
    auto previewLanguageValue = [previewLanguageState] { return *previewLanguageState; };
    showPreviewLanguage(QStringLiteral("text"));
    categoryEditor->addWidget(displaySection);
    categoryEditor->addSpacing(14);

    addSectionTitle(QStringLiteral("JavaScript 分类规则"));
    QFrame *scriptSection = makeSection();
    QVBoxLayout *scriptLayout = new QVBoxLayout(scriptSection);
    scriptLayout->setContentsMargins(0, 0, 0, 0);
    scriptLayout->setSpacing(0);
    ScriptEditorWidget *categoryScript = new ScriptEditorWidget(scriptSection);
    categoryScript->setObjectName(QStringLiteral("categoryScript"));
    categoryScript->setMinimumHeight(280);
    QHBoxLayout *categoryScriptHeader = new QHBoxLayout;
    QLabel *categoryScriptHint = new QLabel(
        QStringLiteral("函数需返回 true 或 false，首个匹配的分类生效"), scriptSection);
    categoryScriptHint->setObjectName(QStringLiteral("subtitle"));
    categoryScriptHint->setStyleSheet(QStringLiteral("color:#b8b8bd;"));
    QPushButton *formatCategory = new QPushButton(QStringLiteral("格式化"), scriptSection);
    QPushButton *expandCategoryEditor = new QPushButton(QStringLiteral("展开编辑"), scriptSection);
    categoryScriptHeader->addWidget(categoryScriptHint);
    categoryScriptHeader->addStretch();
    categoryScriptHeader->addWidget(formatCategory);
    categoryScriptHeader->addWidget(expandCategoryEditor);
    categoryScriptHeader->setContentsMargins(16, 7, 16, 7);
    scriptLayout->addLayout(categoryScriptHeader);
    QFrame *scriptDivider = new QFrame(scriptSection);
    scriptDivider->setFrameShape(QFrame::HLine);
    scriptLayout->addWidget(scriptDivider);
    scriptLayout->addWidget(categoryScript, 1);
    QHBoxLayout *categoryScriptStatus = new QHBoxLayout;
    QLabel *categoryScriptShortcuts = new QLabel(
        QStringLiteral("Ctrl+/ 注释 · Tab / Shift+Tab 缩进"), scriptSection);
    categoryScriptShortcuts->setObjectName(QStringLiteral("subtitle"));
    QLabel *categoryScriptMetrics = new QLabel(scriptSection);
    categoryScriptMetrics->setObjectName(QStringLiteral("subtitle"));
    categoryScriptStatus->addWidget(categoryScriptShortcuts);
    categoryScriptStatus->addStretch();
    categoryScriptStatus->addWidget(categoryScriptMetrics);
    categoryScriptStatus->setContentsMargins(16, 7, 16, 7);
    scriptLayout->addLayout(categoryScriptStatus);
    categoryEditor->addWidget(scriptSection);
    categoryEditor->addSpacing(14);

    addSectionTitle(QStringLiteral("规则测试"));
    QFrame *testSection = makeSection();
    QVBoxLayout *testLayout = new QVBoxLayout(testSection);
    testLayout->setContentsMargins(0, 0, 0, 0);
    testLayout->setSpacing(0);
    QPlainTextEdit *categoryTestText = new QPlainTextEdit(testSection);
    categoryTestText->setObjectName(QStringLiteral("categoryTestText"));
    categoryTestText->setStyleSheet(QStringLiteral("color:#eeeeef;"));
    categoryTestText->setPlaceholderText(QStringLiteral("输入示例剪贴板内容测试规则"));
    categoryTestText->setFixedHeight(78);
    testLayout->addWidget(categoryTestText);
    QFrame *testDivider = new QFrame(testSection);
    testDivider->setFrameShape(QFrame::HLine);
    testLayout->addWidget(testDivider);
    QHBoxLayout *categoryTestLayout = new QHBoxLayout;
    QPushButton *testCategory = new QPushButton(QStringLiteral("测试脚本"), testSection);
    QLabel *categoryTestResult = new QLabel(testSection);
    categoryTestLayout->addWidget(testCategory);
    categoryTestLayout->addWidget(categoryTestResult);
    categoryTestLayout->addStretch();
    categoryTestLayout->setContentsMargins(16, 7, 16, 7);
    testLayout->addLayout(categoryTestLayout);
    categoryEditor->addWidget(testSection);
    categoryEditor->addSpacing(14);

    QHBoxLayout *categoryButtons = new QHBoxLayout;
    QPushButton *saveCategory = new QPushButton(QStringLiteral("保存更改"), categoryEditorWidget);
    saveCategory->setObjectName(QStringLiteral("categorySave"));
    categoryButtons->addStretch();
    categoryButtons->addWidget(saveCategory);
    categoryEditor->addLayout(categoryButtons);
    categoryEditor->addStretch();
    categoryEditorWidget->setMinimumWidth(620);
    // Centred and at most 1120 wide in a wide (maximised) window; the script
    // editor grows with the window height, 280 at the default size.
    categoryEditorWidget->setMaximumWidth(1120);
    QWidget *categoryEditorCanvas = new QWidget(categoryScroll);
    categoryEditorCanvas->setObjectName(QStringLiteral("categoryEditor"));
    QHBoxLayout *categoryEditorCentring = new QHBoxLayout(categoryEditorCanvas);
    categoryEditorCentring->setContentsMargins(0, 0, 0, 0);
    categoryEditorCentring->addStretch(0);
    categoryEditorCentring->addWidget(categoryEditorWidget, 1);
    categoryEditorCentring->addStretch(0);
    categoryScroll->setWidget(categoryEditorCanvas);
    auto *scriptHeightWatcher = new ResizeWatcher(categoryScroll->viewport(), [categoryScript](const QSize &size) {
        categoryScript->setMinimumHeight(std::max(280, std::min(760, size.height() - 420)));
    });
    Q_UNUSED(scriptHeightWatcher);
    categoryLayout->addWidget(categorySidebar);
    categoryLayout->addWidget(categoryDivider);
    categoryLayout->addWidget(categoryScroll, 1);
    auto updateCategoryScriptMetrics = [categoryScript, categoryScriptMetrics](const QString &text) {
        categoryScriptMetrics->setText(QStringLiteral("%1 行 · %2 字符")
            .arg(text.isEmpty() ? 1 : text.count(QLatin1Char('\n')) + 1)
            .arg(text.size()));
    };
    categoryScript->setChangeCallback(updateCategoryScriptMetrics);
    updateCategoryScriptMetrics(categoryScript->text());
    QVector<ClipboardCategory> editableCategories;
    auto reloadCategories = [&] {
        editableCategories = repository->categories();
        categoryList->clear();
        for (const ClipboardCategory &category : editableCategories) {
            QListWidgetItem *item = new QListWidgetItem(category.name, categoryList);
            item->setData(Qt::UserRole, category.databaseID);
            item->setData(Qt::UserRole + 1, category.icon);
            item->setToolTip(QStringLiteral("拖动调整顺序"));
        }
        refreshDeck();
    };
    reloadCategories();
    QObject::connect(categoryList, &QListWidget::currentRowChanged, [&](int row) {
        if (row < 0 || row >= editableCategories.size()) return;
        categoryName->setText(editableCategories[row].name);
        showCategoryColor(HistoryRepository::categoryColor(editableCategories[row].icon, row));
        showPreviewLanguage(editableCategories[row].previewLanguage);
        categoryScript->setText(editableCategories[row].script);
        updateCategoryScriptMetrics(categoryScript->text());
    });
    QObject::connect(newCategory, &QPushButton::clicked, [&] {
        categoryList->clearSelection();
        categoryList->setCurrentRow(-1);
        categoryName->setText(QStringLiteral("新分类"));
        const QStringList palette = HistoryRepository::categoryColorPalette();
        showCategoryColor(palette.at(editableCategories.size() % palette.size()));
        showPreviewLanguage(QStringLiteral("text"));
        categoryScript->setText(QStringLiteral("function classify(item) {\n\n}"));
        updateCategoryScriptMetrics(categoryScript->text());
        categoryName->setFocus();
        categoryName->selectAll();
    });
    QObject::connect(testCategory, &QPushButton::clicked, [&] {
        QString error;
        const bool matches = repository->categoryScriptMatches(
            categoryScript->text(), categoryTestText->toPlainText(),
            previewLanguageValue(), &error);
        categoryTestResult->setText(error.isEmpty()
            ? (matches ? QStringLiteral("匹配：true") : QStringLiteral("匹配：false"))
            : QStringLiteral("错误：%1").arg(error));
        categoryTestResult->setStyleSheet(error.isEmpty()
            ? QStringLiteral("color:#a8d8ad;") : QStringLiteral("color:#ff7373;"));
    });
    QObject::connect(formatCategory, &QPushButton::clicked, [&] {
        categoryScript->setText(formatJavaScript(categoryScript->text()));
        updateCategoryScriptMetrics(categoryScript->text());
    });
    categoryScript->setFormatCallback([formatCategory] { formatCategory->click(); });
    categoryScript->setRunCallback([testCategory] { testCategory->click(); });
    categoryScript->setSaveCallback([saveCategory] { saveCategory->click(); });
    QObject::connect(expandCategoryEditor, &QPushButton::clicked, [&] {
        QDialog expanded(&dialog);
        expanded.setWindowTitle(QStringLiteral("编辑 JavaScript 分类规则"));
        expanded.setModal(true);
        expanded.resize(1040, 680);
        expanded.setStyleSheet(dialog.styleSheet());

        QVBoxLayout *expandedRoot = new QVBoxLayout(&expanded);
        expandedRoot->setContentsMargins(0, 0, 0, 0);
        expandedRoot->setSpacing(0);
        QWidget *expandedHeaderBar = new QWidget(&expanded);
        expandedHeaderBar->setObjectName(QStringLiteral("expandedEditorHeader"));
        expandedHeaderBar->setFixedHeight(68);
        expandedHeaderBar->setStyleSheet(QStringLiteral(
            "QWidget#expandedEditorHeader{background:#252527;border-bottom:1px solid #38383c;}"));
        QHBoxLayout *expandedHeader = new QHBoxLayout(expandedHeaderBar);
        expandedHeader->setContentsMargins(18, 8, 18, 8);
        QVBoxLayout *expandedHeading = new QVBoxLayout;
        expandedHeading->setSpacing(2);
        QLabel *expandedTitle = new QLabel(
            QStringLiteral("%1 · JavaScript 分类规则").arg(categoryName->text()), expandedHeaderBar);
        expandedTitle->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;color:#eeeeef;"));
        QLabel *expandedSubtitle = new QLabel(
            QStringLiteral("编辑分类规则并使用右侧样本验证结果"), expandedHeaderBar);
        expandedSubtitle->setObjectName(QStringLiteral("subtitle"));
        expandedHeading->addWidget(expandedTitle);
        expandedHeading->addWidget(expandedSubtitle);
        expandedHeader->addLayout(expandedHeading);
        expandedHeader->addStretch();
        QPushButton *cancelExpanded = new QPushButton(QStringLiteral("取消"), expandedHeaderBar);
        QPushButton *saveExpanded = new QPushButton(QStringLiteral("保存并完成"), expandedHeaderBar);
        saveExpanded->setObjectName(QStringLiteral("categorySave"));
        expandedHeader->addWidget(cancelExpanded);
        expandedHeader->addWidget(saveExpanded);
        expandedRoot->addWidget(expandedHeaderBar);

        QSplitter *expandedContent = new QSplitter(Qt::Horizontal, &expanded);
        expandedContent->setChildrenCollapsible(false);
        QWidget *editorPane = new QWidget(expandedContent);
        editorPane->setObjectName(QStringLiteral("expandedEditorPane"));
        QVBoxLayout *editorPaneLayout = new QVBoxLayout(editorPane);
        editorPaneLayout->setContentsMargins(12, 8, 12, 8);
        editorPaneLayout->setSpacing(0);
        QHBoxLayout *editorToolbar = new QHBoxLayout;
        QLabel *editorToolbarTitle = new QLabel(QStringLiteral("{ }  分类脚本"), editorPane);
        editorToolbarTitle->setObjectName(QStringLiteral("categorySectionTitle"));
        QPushButton *expandedFormat = new QPushButton(QStringLiteral("格式化"), editorPane);
        editorToolbar->addWidget(editorToolbarTitle);
        editorToolbar->addStretch();
        editorToolbar->addWidget(expandedFormat);
        editorToolbar->setContentsMargins(0, 0, 0, 6);
        editorPaneLayout->addLayout(editorToolbar);
        ScriptEditorWidget *expandedScript = new ScriptEditorWidget(editorPane);
        expandedScript->setText(categoryScript->text());
        QLabel *expandedMetrics = new QLabel(editorPane);
        expandedMetrics->setObjectName(QStringLiteral("subtitle"));
        auto updateExpandedMetrics = [expandedMetrics](const QString &text) {
            expandedMetrics->setText(QStringLiteral("Ctrl+/ 注释 · Tab / Shift+Tab 缩进    %1 行 · %2 字符")
                .arg(text.isEmpty() ? 1 : text.count(QLatin1Char('\n')) + 1)
                .arg(text.size()));
        };
        expandedScript->setChangeCallback(updateExpandedMetrics);
        updateExpandedMetrics(expandedScript->text());
        editorPaneLayout->addWidget(expandedScript, 1);
        expandedMetrics->setFixedHeight(32);
        expandedMetrics->setContentsMargins(6, 5, 6, 0);
        editorPaneLayout->addWidget(expandedMetrics);

        QFrame *testPane = new QFrame(expandedContent);
        testPane->setObjectName(QStringLiteral("expandedTestPane"));
        testPane->setStyleSheet(QStringLiteral(
            "QFrame#expandedTestPane{background:#202022;border-left:1px solid #38383c;}"));
        QVBoxLayout *testPaneLayout = new QVBoxLayout(testPane);
        testPaneLayout->setContentsMargins(14, 14, 14, 14);
        QHBoxLayout *testToolbar = new QHBoxLayout;
        QLabel *testTitle = new QLabel(QStringLiteral("规则测试"), testPane);
        testTitle->setObjectName(QStringLiteral("categorySectionTitle"));
        QPushButton *expandedTestButton = new QPushButton(QStringLiteral("运行测试"), testPane);
        testToolbar->addWidget(testTitle);
        testToolbar->addStretch();
        testToolbar->addWidget(expandedTestButton);
        QPlainTextEdit *expandedTestText = new QPlainTextEdit(testPane);
        expandedTestText->setPlaceholderText(QStringLiteral("输入示例剪贴板内容"));
        expandedTestText->setPlainText(categoryTestText->toPlainText());
        expandedTestText->setFixedHeight(132);
        QLabel *expandedTestResult = new QLabel(testPane);
        expandedTestResult->setWordWrap(true);
        QLabel *consoleTitle = new QLabel(QStringLiteral("控制台输出"), testPane);
        consoleTitle->setObjectName(QStringLiteral("categorySectionTitle"));
        QPlainTextEdit *expandedConsole = new QPlainTextEdit(testPane);
        expandedConsole->setReadOnly(true);
        expandedConsole->setPlaceholderText(QStringLiteral("console.log / info / warn / error / debug 输出会显示在这里"));
        expandedConsole->setFixedHeight(104);
        QLabel *fieldsTitle = new QLabel(QStringLiteral("item 可用字段"), testPane);
        fieldsTitle->setObjectName(QStringLiteral("categorySectionTitle"));
        QPlainTextEdit *itemFields = new QPlainTextEdit(testPane);
        itemFields->setReadOnly(true);
        itemFields->setPlainText(QStringLiteral(
            "text\n  复制的文本内容\n\n"
            "textLength\n  文本字符数\n\n"
            "sourceApplication.bundleId\n  来源应用标识\n\n"
            "sourceApplication.name\n  来源应用名称\n\n"
            "copiedAt\n  毫秒时间戳\n\n"
            "detectedLanguage / codeScore / codeEngineMatched\n  代码分类时提供的识别结果"));
        testPaneLayout->addLayout(testToolbar);
        testPaneLayout->addWidget(expandedTestText);
        testPaneLayout->addWidget(expandedTestResult);
        testPaneLayout->addWidget(consoleTitle);
        testPaneLayout->addWidget(expandedConsole);
        testPaneLayout->addWidget(fieldsTitle);
        testPaneLayout->addWidget(itemFields, 1);
        expandedContent->addWidget(editorPane);
        expandedContent->addWidget(testPane);
        expandedContent->setStretchFactor(0, 3);
        expandedContent->setStretchFactor(1, 1);
        expandedContent->setSizes({720, 320});
        expandedRoot->addWidget(expandedContent, 1);

        auto runExpandedTest = [&] {
            QString error;
            QStringList consoleOutput;
            const bool matches = repository->categoryScriptMatches(
                expandedScript->text(), expandedTestText->toPlainText(),
                previewLanguageValue(), &error, &consoleOutput);
            expandedConsole->setPlainText(consoleOutput.join(QLatin1Char('\n')));
            expandedTestResult->setText(error.isEmpty()
                ? (matches ? QStringLiteral("匹配：true") : QStringLiteral("匹配：false"))
                : QStringLiteral("错误：%1").arg(error));
            expandedTestResult->setStyleSheet(error.isEmpty()
                ? QStringLiteral("color:#a8d8ad;") : QStringLiteral("color:#ff7373;"));
        };
        QObject::connect(expandedTestButton, &QPushButton::clicked, &expanded, runExpandedTest);
        QObject::connect(cancelExpanded, &QPushButton::clicked, &expanded, &QDialog::reject);
        QObject::connect(saveExpanded, &QPushButton::clicked, &expanded, &QDialog::accept);
        expandedScript->setRunCallback(runExpandedTest);
        expandedScript->setFormatCallback([&] {
            expandedScript->setText(formatJavaScript(expandedScript->text()));
            updateExpandedMetrics(expandedScript->text());
        });
        QObject::connect(expandedFormat, &QPushButton::clicked, &expanded, [&] {
            expandedScript->setText(formatJavaScript(expandedScript->text()));
            updateExpandedMetrics(expandedScript->text());
        });
        expandedScript->setSaveCallback([saveExpanded] { saveExpanded->click(); });
        QTimer::singleShot(0, expandedScript, [expandedScript] { expandedScript->focusEditor(); });
        if (expanded.exec() != QDialog::Accepted) return;
        categoryScript->setText(expandedScript->text());
        updateCategoryScriptMetrics(categoryScript->text());
        categoryTestText->setPlainText(expandedTestText->toPlainText());
        saveCategory->click();
    });
    QObject::connect(restoreCategories, &QPushButton::clicked, [&] {
        if (!showClipDeckAlert(&dialog, QStringLiteral("还原预制分类"),
                QStringLiteral("同名预制分类会恢复默认规则；自建分类会保留。"),
                QStringLiteral("还原"), QStringLiteral("取消"), AlertTone::Warning)) return;
        if (!repository->restoreDefaultCategories()) {
            showClipDeckAlert(&dialog, QStringLiteral("还原失败"), repository->lastError(),
                              QStringLiteral("知道了"), QString(), AlertTone::Warning);
            return;
        }
        reloadCategories();
    });
    QObject::connect(categoryList->model(), &QAbstractItemModel::rowsMoved,
                     categoryList, [&, categoryList] {
        QVector<qint64> ids;
        ids.reserve(categoryList->count());
        for (int row = 0; row < categoryList->count(); ++row) {
            ids.append(categoryList->item(row)->data(Qt::UserRole).toLongLong());
        }
        if (repository->reorderCategories(ids)) {
            editableCategories = repository->categories();
            refreshDeck();
        }
    });
    QObject::connect(saveCategory, &QPushButton::clicked, [&] {
        const QString name = categoryName->text().trimmed();
        const QString script = categoryScript->text();
        if (name.isEmpty() || !script.contains(QStringLiteral("classify"))) {
            showClipDeckAlert(&dialog, QStringLiteral("无法保存"),
                              QStringLiteral("请输入分类名称，并定义 classify(item) 函数。"),
                              QStringLiteral("知道了"), QString(), AlertTone::Warning);
            return;
        }
        const QString scriptError = repository->categoryScriptError(script);
        if (!scriptError.isEmpty()) {
            showClipDeckAlert(&dialog, QStringLiteral("脚本错误"), scriptError,
                              QStringLiteral("知道了"), QString(), AlertTone::Warning);
            return;
        }
        const int row = categoryList->currentRow();
        const bool ok = row >= 0 && row < editableCategories.size()
            ? repository->updateCategory(editableCategories[row].databaseID, name,
                                         categoryIcon->text(), script, previewLanguageValue())
            : repository->createCategory(name, categoryIcon->text(), script,
                                         previewLanguageValue());
        if (!ok) {
            showClipDeckAlert(&dialog, QStringLiteral("无法保存"), repository->lastError(),
                              QStringLiteral("知道了"), QString(), AlertTone::Warning);
            return;
        }
        reloadCategories();
        for (int index = 0; index < editableCategories.size(); ++index) {
            if (editableCategories[index].name == name) {
                categoryList->setCurrentRow(index);
                break;
            }
        }
    });
    categoryList->setDeleteCallback([&](int row) {
        if (row < 0 || row >= editableCategories.size()) return;
        if (!showClipDeckAlert(&dialog, QStringLiteral("删除分类"),
                QStringLiteral("确定删除“%1”？").arg(editableCategories[row].name),
                QStringLiteral("删除"), QStringLiteral("取消"), AlertTone::Destructive)) return;
        repository->removeCategory(editableCategories[row].databaseID);
        reloadCategories();
        categoryName->clear();
        showCategoryColor(QString());
        categoryScript->clear();
    });
    if (!editableCategories.isEmpty()) categoryList->setCurrentRow(0);
    if (beginNewCategory) {
        QTimer::singleShot(0, newCategory, &QPushButton::click);
    }
    pages->addWidget(categoryPageWidget);

    SettingsPage dataPage = makePage(QStringLiteral("数据"), QStringLiteral("查看数据库占用并管理本地历史"));
    QVBoxLayout *dataRows = addSettingsSection(dataPage, QStringLiteral("数据库"));
    QLabel *size = new QLabel(QStringLiteral("%1 MB").arg(repository->databaseByteCount() / 1048576.0, 0, 'f', 2), dataPage.body);
    size->setStyleSheet(QStringLiteral("color:#9a9aa0;"));
    QPushButton *clearExpired = new QPushButton(QStringLiteral("清理过期内容…"), dataPage.body);
    QPushButton *clearAll = new QPushButton(QStringLiteral("清空全部历史…"), dataPage.body);
    clearAll->setStyleSheet(QStringLiteral("QPushButton{color:#ff6961;}"));
    addSettingsRow(dataRows, QStringLiteral("当前占用"), size);
    addSettingsRow(dataRows, QStringLiteral("过期缓存"), clearExpired,
                   QStringLiteral("删除超过保留期限的历史并回收空间"));
    addSettingsRow(dataRows, QStringLiteral("全部历史"), clearAll);
    dataPage.layout->addStretch();

    QObject::connect(sidebar, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);
    sidebar->setCurrentRow(std::max(0, std::min(4, initialPage)));
    QObject::connect(clearExpired, &QPushButton::clicked, [&] {
        const int removed = repository->clearExpiredAndReclaim(selectedRetentionDays());
        refreshDeck();
        size->setText(QStringLiteral("%1 MB").arg(repository->databaseByteCount() / 1048576.0, 0, 'f', 2));
        showClipDeckAlert(&dialog, QStringLiteral("清理完成"),
                          QStringLiteral("已删除 %1 条过期历史并回收数据库空间。").arg(removed));
    });
    QObject::connect(clearAll, &QPushButton::clicked, [&] {
        if (showClipDeckAlert(&dialog, QStringLiteral("删除历史"),
                QStringLiteral("所有剪贴板历史将被永久删除。"),
                QStringLiteral("全部删除"), QStringLiteral("取消"), AlertTone::Destructive)) {
            repository->clearAll();
            refreshDeck();
            size->setText(QStringLiteral("0.00 MB"));
        }
    });
    QObject::connect(&dialog, &QDialog::finished, [&] {
        settings.setValue(QStringLiteral("paste/preserveFormatting"), pasteFormat->currentIndex() == 1);
        settings.setValue(QStringLiteral("history/retentionDays"), selectedRetentionDays());
        settings.setValue(QStringLiteral("clipboard/recordsImages"), recordImages->isChecked());
        settings.setValue(QStringLiteral("clipboard/maximumImageMegabytes"), maximumImage->currentData().toInt());
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
        refreshDeck();
        if (shortcutChanged) shortcutChanged();
    });
    if (!renderPath.isEmpty()) {
        QDir().mkpath(QFileInfo(renderPath).absolutePath());
        QTimer::singleShot(100, &dialog, [&dialog, renderPath] {
            dialog.grab().save(renderPath, "PNG");
            dialog.reject();
        });
    }
    std::unique_ptr<QFile> settingsCommandInput;
    std::unique_ptr<QSocketNotifier> settingsCommandNotifier;
    if (enableCommandInput) {
        settingsCommandInput = std::make_unique<QFile>();
        if (settingsCommandInput->open(stdin, QIODevice::ReadOnly)) {
            settingsCommandNotifier = std::make_unique<QSocketNotifier>(
                settingsCommandInput->handle(), QSocketNotifier::Read, &dialog);
            QObject::connect(settingsCommandNotifier.get(), &QSocketNotifier::activated,
                             &dialog, [&, input = settingsCommandInput.get()](int) {
                const QByteArray command = input->readLine().trimmed();
                if (!command.startsWith(QByteArray(kSettingsActivatePrefix))) return;
                const QList<QByteArray> parts = command.mid(
                    static_cast<int>(sizeof(kSettingsActivatePrefix) - 1)).split(':');
                bool pageOK = false;
                const int requestedPage = parts.value(0).toInt(&pageOK);
                const bool createCategory = parts.value(1) == QByteArrayLiteral("1");
                if (pageOK && requestedPage >= 0) {
                    sidebar->setCurrentRow(std::max(0, std::min(4, requestedPage)));
                }
                if (createCategory) newCategory->click();
                if (dialog.isMinimized()) dialog.showNormal();
                else dialog.show();
                dialog.raise();
                dialog.activateWindow();
                if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
                    Display *settingsDisplay = QX11Info::display();
                    const Window settingsWindow = static_cast<Window>(dialog.winId());
                    const Atom activeWindow = XInternAtom(
                        settingsDisplay, "_NET_ACTIVE_WINDOW", False);
                    XEvent event{};
                    event.xclient.type = ClientMessage;
                    event.xclient.window = settingsWindow;
                    event.xclient.message_type = activeWindow;
                    event.xclient.format = 32;
                    event.xclient.data.l[0] = 2;
                    event.xclient.data.l[1] = CurrentTime;
                    XSendEvent(settingsDisplay, DefaultRootWindow(settingsDisplay), False,
                               SubstructureRedirectMask | SubstructureNotifyMask, &event);
                    XRaiseWindow(settingsDisplay, settingsWindow);
                    XSetInputFocus(settingsDisplay, settingsWindow,
                                   RevertToPointerRoot, CurrentTime);
                    XFlush(settingsDisplay);
                }
            });
        }
    }
    dialog.exec();
}

} // namespace

int main(int argc, char *argv[]) {
    XInitThreads();
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    QApplication app(argc, argv);
    QtWebEngine::initialize();
    DisplayScale::initialize(QGuiApplication::primaryScreen());
    app.setStyle(new ClipDeckStyle);
    QApplication::setApplicationName(QStringLiteral("ClipDeck"));
    QApplication::setOrganizationName(QStringLiteral("ClipDeck"));
    QApplication::setQuitOnLastWindowClosed(false);
    if (!app.arguments().contains(QStringLiteral("--self-test"))
        && app.arguments().filter(QStringLiteral("--render-check=")).isEmpty()) {
        ScriptEditorWidget::prewarm();
    }
    app.setStyleSheet(QStringLiteral(
        "QMenu{background:#29292c;color:#eee;border:1px solid #444;padding:5px;}"
        "QMenu::item{padding:7px 24px 7px 12px;border-radius:6px;}"
        "QMenu::item:selected{background:#176fd1;color:white;}"
        "QMenu::separator{height:1px;background:#444;margin:5px 8px;}"
        "QDialog{background:#1d1d1f;color:#eee;}"
        "QDialog QLabel{color:#ddd;}"
        "QDialog QPlainTextEdit,QDialog QLineEdit{background:#151517;color:#eee;outline:0;"
        "border:1px solid #3b3b3e;border-radius:8px;padding:7px;selection-background-color:#48484d;}"
        "QDialog QPlainTextEdit:!focus,QDialog QLineEdit:!focus{background:#151517;border-color:#3b3b3e;}"
        "QDialog QPlainTextEdit:focus,QDialog QLineEdit:focus{background:#19191c;border-color:#1683ff;"
        "selection-background-color:#1683ff;}"
        "QDialog QPushButton{background:#363639;color:#eee;border:1px solid transparent;"
        "border-radius:8px;padding:8px 14px;outline:0;}"
        "QDialog QPushButton:hover{background:#48484c;}"
        "QDialog QPushButton:focus{border-color:#1683ff;}"));

    Display *display = QGuiApplication::platformName() == QStringLiteral("xcb")
        ? QX11Info::display()
        : nullptr;
    const bool selfTestRequested = app.arguments().contains(QStringLiteral("--self-test"));
    QString renderCheckPath;
    QString settingsRenderPath;
    int settingsPage = -1;
    bool beginNewCategory = false;
    bool settingsIPCEnabled = false;
    for (const QString &argument : app.arguments()) {
        if (argument.startsWith(QStringLiteral("--render-check="))) {
            renderCheckPath = argument.mid(QStringLiteral("--render-check=").size());
        }
        if (argument.startsWith(QStringLiteral("--settings-render="))) {
            settingsRenderPath = argument.mid(QStringLiteral("--settings-render=").size());
        }
        if (argument.startsWith(QStringLiteral("--settings-page="))) {
            bool ok = false;
            const int requested = argument.mid(QStringLiteral("--settings-page=").size()).toInt(&ok);
            if (ok) settingsPage = std::max(0, std::min(4, requested));
        }
        if (argument == QStringLiteral("--new-category")) beginNewCategory = true;
        if (argument == QStringLiteral("--settings-ipc")) settingsIPCEnabled = true;
    }
    const bool usesTemporaryRepository = selfTestRequested || !renderCheckPath.isEmpty()
        || !settingsRenderPath.isEmpty();
    QTemporaryDir selfTestDirectory;
    HistoryRepository repository(usesTemporaryRepository
        ? selfTestDirectory.filePath(QStringLiteral("history.sqlite")) : QString());
    QSettings settings;
    if (selfTestRequested) {
        const QDateTime now = QDateTime::currentDateTime();
        auto solidPng = [](int side, const QColor &color) {
            QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
            image.fill(color);
            QByteArray data;
            QBuffer buffer(&data);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            return data;
        };
        LinuxApplicationIconSet rasterIconSet;
        rasterIconSet.pngVariants = {
            {32, 32, solidPng(32, QColor(220, 40, 40))},
            {64, 64, solidPng(64, QColor(40, 200, 80))},
            {128, 128, solidPng(128, QColor(40, 90, 220))}
        };
        const QByteArray serializedRasterIcons = rasterIconSet.serialize();
        const LinuxApplicationIconSet restoredRasterIcons =
            LinuxApplicationIconSet::deserialize(serializedRasterIcons);
        const QImage raster50 = restoredRasterIcons.rasterize(QSize(50, 50), 1.0);
        const QImage raster100 = restoredRasterIcons.rasterize(QSize(100, 100), 1.0);
        const QImage raster180 = restoredRasterIcons.rasterize(QSize(180, 180), 1.0);
        LinuxApplicationIconSet svgIconSet = rasterIconSet;
        svgIconSet.svgData = QByteArrayLiteral(
            "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'>"
            "<rect width='32' height='32' fill='#d02020'/></svg>");
        const QImage svg63 = svgIconSet.rasterize(QSize(63, 63), 1.25);
        ClipboardItem first;
        first.title = QStringLiteral("文本");
        first.text = QStringLiteral("duplicate-window-test");
        first.sourceID = QStringLiteral("self.test.application");
        first.sourceName = QStringLiteral("Self Test");
        first.sourceIconVariantsData = serializedRasterIcons;
        first.copiedAt = now;
        const ClipboardItem savedFirst = repository.save(first);
        first.copiedAt = now.addSecs(5);
        const ClipboardItem savedDuplicate = repository.save(first);
        ClipboardItem nonDuplicateBetweenCopies;
        nonDuplicateBetweenCopies.title = QStringLiteral("文本");
        nonDuplicateBetweenCopies.text = QStringLiteral("content-between-duplicate-copies");
        nonDuplicateBetweenCopies.sourceName = QStringLiteral("Self Test");
        nonDuplicateBetweenCopies.copiedAt = now.addSecs(10);
        repository.save(nonDuplicateBetweenCopies);
        first.copiedAt = now.addSecs(16);
        first.sourceID = QStringLiteral("dde-grand-search");
        first.sourceName = QStringLiteral("dde-grand-search");
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

        QImage angleSource(3, 2, QImage::Format_ARGB32_Premultiplied);
        angleSource.fill(QColor(12, 34, 56));
        QByteArray angleSourceData;
        QBuffer angleBuffer(&angleSourceData);
        angleBuffer.open(QIODevice::WriteOnly);
        angleSource.save(&angleBuffer, "PNG");
        ClipboardItem angleItem;
        angleItem.title = QStringLiteral("旋转测试");
        angleItem.type = ClipboardPayloadType::Image;
        angleItem.image = angleSource;
        angleItem.imageData = angleSourceData;
        angleItem.sourceName = QStringLiteral("Self Test");
        angleItem.copiedAt = now.addSecs(30);
        const ClipboardItem savedAngleItem = repository.save(angleItem);
        const bool angleUpdated = repository.updateImageAngle(savedAngleItem.databaseID, -90);
        const QVector<ClipboardItem> stored = repository.fetch(QString(), QString(), 100);
        const QVector<ClipboardItem> paged = repository.fetch(QString(), QString(), 2, 1);
        const auto linked = std::find_if(stored.cbegin(), stored.cend(), [savedURL](const ClipboardItem &item) {
            return item.databaseID == savedURL.databaseID;
        });
        const auto storedIconSet = std::find_if(stored.cbegin(), stored.cend(), [savedFirst](const ClipboardItem &item) {
            return item.databaseID == savedFirst.databaseID;
        });
        const auto storedAngleItem = std::find_if(stored.cbegin(), stored.cend(),
            [savedAngleItem](const ClipboardItem &item) {
                return item.databaseID == savedAngleItem.databaseID;
            });
        QString categoryTestError;
        QStringList categoryTestConsole;
        const bool categoryInputMatches = repository.categoryScriptMatches(
            QStringLiteral("function classify(item){console.log('length',item.textLength);return item.textLength===4&&item.sourceApplication.name==='脚本测试'&&item.copiedAt>0;}"),
            QStringLiteral("test"), QStringLiteral("text"), &categoryTestError,
            &categoryTestConsole);

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
                    query.addBindValue(index == 0
                        ? QStringLiteral("function classify(item){const t=item.text.trim();if(!((t.startsWith('{')&&t.endsWith('}'))||(t.startsWith('[')&&t.endsWith(']'))))return false;try{const v=JSON.parse(t);return v!==null&&typeof v==='object';}catch(e){return false;}}")
                        : QStringLiteral("function classify(item){return false;}"));
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
        ClipboardItem migratedIntervening;
        migratedIntervening.title = QStringLiteral("文本");
        migratedIntervening.text = QStringLiteral("intervening-content");
        migratedIntervening.sourceName = QStringLiteral("Self Test");
        migratedIntervening.copiedAt = now.addSecs(10);
        migratedLegacyRepository.save(migratedIntervening);
        migratedFirst.copiedAt = now.addSecs(25 * 60 * 60);
        const ClipboardItem migratedSavedLater = migratedLegacyRepository.save(migratedFirst);

        const QString duplicatePath = selfTestDirectory.filePath(
            QStringLiteral("consecutive-duplicates.sqlite"));
        const QString duplicateConnection = QStringLiteral("clipdeck-duplicate-cleanup-self-test");
        bool duplicateSetupOK = false;
        {
            QSqlDatabase duplicateDatabase = QSqlDatabase::addDatabase(
                QStringLiteral("QSQLITE"), duplicateConnection);
            duplicateDatabase.setDatabaseName(duplicatePath);
            if (duplicateDatabase.open()) {
                QSqlQuery query(duplicateDatabase);
                duplicateSetupOK = query.exec(QStringLiteral(
                    "CREATE TABLE content (id INTEGER PRIMARY KEY AUTOINCREMENT,title TEXT NOT NULL,"
                    "content_type TEXT NOT NULL,plain_text TEXT,image_data BLOB,image_width INTEGER,"
                    "image_height INTEGER,content_hash BLOB NOT NULL,source_id TEXT,source_name TEXT NOT NULL,"
                    "category TEXT,copied_at INTEGER NOT NULL,last_copied_at INTEGER NOT NULL,"
                    "copy_count INTEGER NOT NULL DEFAULT 1,updated_at INTEGER NOT NULL)"))
                    && query.exec(QStringLiteral(
                        "CREATE TABLE category (id INTEGER PRIMARY KEY AUTOINCREMENT,name TEXT NOT NULL UNIQUE,"
                        "script TEXT NOT NULL,sort_order INTEGER NOT NULL DEFAULT 0)"));
                auto insertDuplicate = [&query](const QString &sourceID, const QString &sourceName,
                                                qint64 copiedAt) {
                    query.prepare(QStringLiteral(
                        "INSERT INTO content(title,content_type,plain_text,content_hash,source_id,source_name,"
                        "copied_at,last_copied_at,copy_count,updated_at) VALUES(?,?,?,?,?,?,?,?,?,?)"));
                    query.addBindValue(QStringLiteral("文本"));
                    query.addBindValue(QStringLiteral("text"));
                    query.addBindValue(QStringLiteral("same-content"));
                    query.addBindValue(QByteArrayLiteral("same-hash"));
                    query.addBindValue(sourceID);
                    query.addBindValue(sourceName);
                    query.addBindValue(copiedAt);
                    query.addBindValue(copiedAt);
                    query.addBindValue(1);
                    query.addBindValue(copiedAt);
                    return query.exec();
                };
                duplicateSetupOK = duplicateSetupOK
                    && insertDuplicate(QStringLiteral("com.intellij.idea"),
                                       QStringLiteral("IntelliJ IDEA"), 1000)
                    && insertDuplicate(QStringLiteral("dde-grand-search"),
                                       QStringLiteral("dde-grand-search"), 2000);
            }
            duplicateDatabase.close();
        }
        QSqlDatabase::removeDatabase(duplicateConnection);
        HistoryRepository duplicateCleanupRepository(duplicatePath);
        const QVector<ClipboardItem> cleanedDuplicates =
            duplicateCleanupRepository.fetch(QString(), QString(), 10);
        bool migratedDefaultScriptFormatted = false;
        bool migratedCustomScriptPreserved = false;
        for (const ClipboardCategory &category : migratedLegacyRepository.categories()) {
            if (category.name == QStringLiteral("JSON")) {
                migratedDefaultScriptFormatted = category.script.contains(QLatin1Char('\n'));
            }
            if (category.name == QStringLiteral("SQL")) {
                migratedCustomScriptPreserved =
                    category.script == QStringLiteral("function classify(item){return false;}");
            }
        }

        QMimeData httpURLMime;
        httpURLMime.setUrls({QUrl(QStringLiteral("https://example.com"))});
        httpURLMime.setText(QStringLiteral("https://example.com"));
        QMimeData localFileMime;
        localFileMime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/example.txt"))});

        QString defaultColorError;
        bool defaultColorScriptMatches = false;
        bool defaultScriptsFormatted = true;
        for (const ClipboardCategory &category : repository.categories()) {
            defaultScriptsFormatted = defaultScriptsFormatted
                && category.script.contains(QLatin1Char('\n'));
            if (category.name == QStringLiteral("色彩")) {
                defaultColorScriptMatches = repository.categoryScriptMatches(
                    category.script, QStringLiteral("#12abef"), category.previewLanguage,
                    &defaultColorError);
            }
        }
        if (!defaultColorError.isEmpty()) {
            qWarning().noquote() << "default color script:" << defaultColorError;
        }
        const QString formattedJavaScript = formatJavaScript(QStringLiteral(
            "function classify(item){const value=1;if(value){return /a+/.test(item.text);}return false;}"));
        const QKeyEvent settingsShortcutEvent(
            QEvent::KeyPress, Qt::Key_Comma, Qt::ControlModifier, QStringLiteral(","));
        QStringList failures;
        auto require = [&failures](bool condition, const QString &name) {
            if (!condition) failures.append(name);
        };
        require(repository.isOpen(), QStringLiteral("database-open"));
        require(stored.size() >= 3 && paged.size() == 2
                    && paged[0].databaseID == stored[1].databaseID
                    && paged[1].databaseID == stored[2].databaseID,
                QStringLiteral("history-fetch-offset-pagination"));
        require(!serializedRasterIcons.isEmpty()
                    && restoredRasterIcons.pngVariants.size() == 3,
                QStringLiteral("application-icon-set-serialization"));
        require(raster50.size() == QSize(50, 50)
                    && raster50.pixelColor(25, 25) == QColor(40, 200, 80),
                QStringLiteral("application-icon-smallest-png-above-target"));
        require(raster100.size() == QSize(100, 100)
                    && raster100.pixelColor(50, 50) == QColor(40, 90, 220)
                    && raster180.pixelColor(90, 90) == QColor(40, 90, 220),
                QStringLiteral("application-icon-largest-png-fallback"));
        require(svg63.size() == QSize(63, 63)
                    && svg63.pixelColor(31, 31).red() > 180,
                QStringLiteral("application-icon-svg-priority"));
        require(storedIconSet != stored.cend()
                    && storedIconSet->sourceIconVariantsData == serializedRasterIcons,
                QStringLiteral("application-icon-set-persistence"));
        require(angleUpdated && storedAngleItem != stored.cend()
                    && storedAngleItem->imageAngle == 270
                    && storedAngleItem->imageData == angleSourceData
                    && imageAtAngle(storedAngleItem->image, storedAngleItem->imageAngle).size()
                        == QSize(2, 3),
                QStringLiteral("image-angle-metadata-persistence"));
        require(repository.categories().size() == 6, QStringLiteral("default-categories"));
        require(defaultScriptsFormatted, QStringLiteral("default-categories-formatted"));
        require(defaultColorScriptMatches && defaultColorError.isEmpty(),
                QStringLiteral("default-color-script"));
        require(formattedJavaScript.contains(QLatin1Char('\n'))
                    && repository.categoryScriptError(formattedJavaScript).isEmpty(),
                QStringLiteral("javascript-formatter"));
        require(isShorterRetention(30, 7) && isShorterRetention(0, 30)
                    && !isShorterRetention(30, 365) && !isShorterRetention(30, 0),
                QStringLiteral("retention-shortening-detection"));
        require(keySymForQtKey(Qt::Key_Space) == XK_space
                    && keySymForQtKey(Qt::Key_Delete) == XK_Delete
                    && keySymForQtKey(Qt::Key_F12) == XK_F12,
                QStringLiteral("global-shortcut-key-mapping"));
        require(defaultShowDeckShortcut().toString(QKeySequence::PortableText)
                    == QStringLiteral("Alt+Shift+V"),
                QStringLiteral("default-deck-shortcut"));
        require(isSettingsShortcut(&settingsShortcutEvent),
                QStringLiteral("deck-settings-shortcut"));
        require(legacySetupOK && migratedLegacyRepository.isOpen(),
                QStringLiteral("preview6-migration-open"));
        require(migratedLegacyRepository.categories().size() == 6,
                QStringLiteral("preview6-migration-categories"));
        require(migratedDefaultScriptFormatted && migratedCustomScriptPreserved,
                QStringLiteral("default-script-format-migration"));
        require(migratedSavedFirst.databaseID >= 0
                    && migratedSavedLater.databaseID != migratedSavedFirst.databaseID,
                QStringLiteral("preview6-migration-removes-global-unique"));
        require(duplicateSetupOK && duplicateCleanupRepository.isOpen()
                    && cleanedDuplicates.size() == 1
                    && cleanedDuplicates.first().sourceID == QStringLiteral("com.intellij.idea")
                    && cleanedDuplicates.first().copyCount == 2,
                QStringLiteral("collapse-existing-dde-grand-search-duplicate"));
        require(!containsLocalFileURL(&httpURLMime) && containsLocalFileURL(&localFileMime),
                QStringLiteral("clipboard-url-filter"));
        failures.append(applicationSourceSelfTestFailures());
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
        require(categoryTestConsole == QStringList{QStringLiteral("LOG  length 4")},
                QStringLiteral("category-script-console"));
        require(savedFirst.databaseID >= 0, QStringLiteral("save-first"));
        require(savedDuplicate.databaseID == savedFirst.databaseID,
                QStringLiteral("dedupe-within-24-hours"));
        require(savedDuplicate.copyCount == 2, QStringLiteral("dedupe-count"));
        require(savedLater.databaseID == savedFirst.databaseID,
                QStringLiteral("dedupe-nonconsecutive-within-24-hours"));
        require(storedIconSet != stored.cend()
                    && storedIconSet->sourceID == QStringLiteral("self.test.application")
                    && storedIconSet->sourceName == QStringLiteral("Self Test"),
                QStringLiteral("preserve-source-over-dde-grand-search"));
        require(savedURLAgain.databaseID == savedURL.databaseID,
                QStringLiteral("dedupe-url-within-24-hours"));
        require(linked != stored.cend(), QStringLiteral("fetch-link"));
        require(linked != stored.cend() && linked->title == QStringLiteral("Example title"),
                QStringLiteral("link-title"));
        require(linked != stored.cend() && linked->hasFetchedLinkPreview,
                QStringLiteral("link-preview-state"));
        for (int headerWidth : {260, 320, 560}) {
            QWidget header;
            header.setObjectName(QStringLiteral("headerLayoutTest"));
            header.setStyleSheet(QStringLiteral(
                "QWidget#headerLayoutTest{background:#2b2b2e;}"
                "QLabel{color:#ffffff;font-size:13px;}"
                "QLabel#previewCategory{font-size:18px;font-weight:700;}"));
            QHBoxLayout *row = new QHBoxLayout(&header);
            row->setContentsMargins(22, 0, 22, 0);
            row->setSpacing(12);
            const QString type = QStringLiteral("超长类型名称 Markdown / JavaScript");
            const QString appName = QStringLiteral("Very long application source name with 中文文字");
            auto *typeLabel = new ElidingLabel(type, &header);
            typeLabel->setObjectName(QStringLiteral("previewCategory"));
            typeLabel->setMaximumWidth(100);
            auto *appLabel = new ElidingLabel(appName, &header);
            appLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            appLabel->setMinimumWidth(24);
            auto *timeLabel = new ElidingLabel(QStringLiteral("2026-10-29  14:35"), &header);
            row->addWidget(typeLabel);
            row->addWidget(appLabel, 1);
            row->addWidget(timeLabel);
            header.resize(headerWidth, 58);
            header.show();
            app.processEvents();
            require(header.width() == headerWidth
                && typeLabel->geometry().right() + 12 <= appLabel->geometry().left()
                && appLabel->geometry().right() + 12 <= timeLabel->geometry().left()
                && timeLabel->geometry().right() < headerWidth - 21,
                QStringLiteral("preview-header-fields-keep-separate-layout-allocations"));
            require(typeLabel->text() == type && appLabel->text() == appName
                && appLabel->toolTip() == appName,
                QStringLiteral("preview-header-retains-full-text-for-tooltip"));
            // Text may be elided but must never paint into the inter-field gap.
            const QImage pixels = header.grab().toImage();
            const qreal ratio = pixels.devicePixelRatio();
            bool gapClear = true;
            for (int x = typeLabel->geometry().right() + 2; x < appLabel->x() - 1; ++x) {
                for (int y = 8; y < 50; ++y) {
                    if (QColor::fromRgba(pixels.pixel(qRound(x * ratio), qRound(y * ratio))).red() > 100)
                        gapClear = false;
                }
            }
            require(gapClear, QStringLiteral("preview-header-text-does-not-bleed-into-neighbor"));
            header.hide();
        }
        const QSize previewScreen(1920, 1080);
        for (const QSize imageSize : {QSize(3000, 2000), QSize(2000, 3000), QSize(2000, 2000)}) {
            const QSize window = imagePreviewWindowSize(imageSize, previewScreen);
            const double bodyAspect = (window.width() - 20) / double(window.height() - 78);
            require(std::abs(bodyAspect - imageSize.width() / double(imageSize.height())) < 0.01,
                    QStringLiteral("image-preview-preserves-aspect"));
            require(window.width() <= 680 && window.height() <= 500,
                    QStringLiteral("image-preview-bounded-size"));
        }
        require(imagePreviewWindowSize(QSize(30, 20), previewScreen)
                    == imagePreviewWindowSize(QSize(30000, 20000), previewScreen),
                QStringLiteral("image-preview-independent-of-pixel-count"));
        for (const QSize imageSize : {QSize(100000, 1), QSize(1, 100000)}) {
            const QSize window = imagePreviewWindowSize(imageSize, QSize(600, 400));
            require(window.width() <= 584 && window.height() <= 384
                        && window.width() >= 260 && window.height() >= 180,
                    QStringLiteral("image-preview-extreme-aspect-screen-bounds"));
        }
        {
            // Exercise the actual nested preview/navigation path. With xcb,
            // also verify that the source's native input focus never moves.
            HistoryRepository previewRepository(selfTestDirectory.filePath(
                QStringLiteral("preview-focus.sqlite")));
            ClipboardItem textItem;
            textItem.text = QStringLiteral("preview focus regression");
            textItem.title = QStringLiteral("文本");
            textItem.copiedAt = now;
            previewRepository.save(textItem);
            ClipboardItem imageItem;
            imageItem.type = ClipboardPayloadType::Image;
            imageItem.title = QStringLiteral("图片");
            imageItem.imageData = solidPng(64, QColor(40, 90, 220));
            imageItem.image.loadFromData(imageItem.imageData);
            imageItem.image = imageItem.image.scaled(120, 80);
            imageItem.imageData.clear();
            QBuffer imageBuffer(&imageItem.imageData);
            imageBuffer.open(QIODevice::WriteOnly);
            imageItem.image.save(&imageBuffer, "PNG");
            imageItem.copiedAt = now.addSecs(1);
            previewRepository.save(imageItem);

            QLineEdit sourceInput;
            sourceInput.show();
            app.processEvents();
            Window sourceFocus = None;
            int revertTo = RevertToNone;
            if (display) {
                XSetInputFocus(display, static_cast<Window>(sourceInput.winId()),
                               RevertToParent, CurrentTime);
                XSync(display, False);
                XGetInputFocus(display, &sourceFocus, &revertTo);
            }
            DeckWindow previewDeck(display, &previewRepository);
            previewDeck.showNearPointer();
            auto sendKey = [](QWidget *receiver, int key, const QString &text = QString()) {
                QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, text);
                QApplication::sendEvent(receiver, &press);
            };
            auto checkPreview = [&](int direction, const QString &expectedTitle) {
                bool navigated = false;
                QTimer watchdog;
                watchdog.setSingleShot(true);
                QObject::connect(&watchdog, &QTimer::timeout, [&] {
                    for (QDialog *dialog : previewDeck.findChildren<QDialog *>()) {
                        if (dialog->isVisible()) dialog->reject();
                    }
                });
                watchdog.start(2000);
                QTimer::singleShot(0, &previewDeck, [&] {
                    for (QDialog *dialog : previewDeck.findChildren<QDialog *>()) {
                        if (!dialog->isVisible()) continue;
                        require(dialog->windowType() == Qt::Tool
                                    && dialog->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus)
                                    && !dialog->isModal(),
                                QStringLiteral("preview-nonactivating-nonmodal"));
                        QLabel *label = dialog->findChild<QLabel *>(QStringLiteral("previewCategory"));
                        require(label && label->text() == expectedTitle,
                                QStringLiteral("preview-navigation-content"));
                        if (expectedTitle == QStringLiteral("图片")) {
                            const QSize screen = QApplication::desktop()->availableGeometry(
                                dialog->frameGeometry().center()).size();
                            require(dialog->size() == imagePreviewWindowSize(imageItem.image.size(), screen),
                                    QStringLiteral("image-preview-dynamic-window-size"));
                            QKeyEvent rotate(QEvent::KeyPress, Qt::Key_BracketRight, Qt::ControlModifier);
                            QApplication::sendEvent(&previewDeck, &rotate);
                            require(dialog->size() == imagePreviewWindowSize(
                                        imageSizeAtAngle(imageItem.image.size(), 90), screen),
                                    QStringLiteral("image-preview-rotation-resizes-window"));
                        }
                        if (display) {
                            Window actual = None;
                            XGetInputFocus(display, &actual, &revertTo);
                            require(actual == sourceFocus, QStringLiteral("preview-preserves-native-focus"));
                        }
                        navigated = true;
                        if (display) {
                            const KeyCode code = XKeysymToKeycode(display,
                                direction == Qt::Key_Left ? XK_Left : XK_Right);
                            XTestFakeKeyEvent(display, code, True, CurrentTime);
                            XTestFakeKeyEvent(display, code, False, CurrentTime);
                            XFlush(display);
                        } else {
                            sendKey(&previewDeck, direction);
                        }
                        break;
                    }
                });
                sendKey(&previewDeck, Qt::Key_Space);
                require(navigated && previewDeck.isVisible(),
                        QStringLiteral("preview-navigation-keeps-deck"));
                if (display) {
                    Window actual = None;
                    XGetInputFocus(display, &actual, &revertTo);
                    require(actual == sourceFocus, QStringLiteral("preview-navigation-preserves-native-focus"));
                }
            };
            checkPreview(Qt::Key_Right, QStringLiteral("图片"));
            checkPreview(Qt::Key_Left, QStringLiteral("文本"));
            if (display) {
                const KeyCode code = XKeysymToKeycode(display, XK_a);
                XTestFakeKeyEvent(display, code, True, CurrentTime);
                XTestFakeKeyEvent(display, code, False, CurrentTime);
                XFlush(display);
                QEventLoop inputLoop;
                QTimer::singleShot(100, &inputLoop, &QEventLoop::quit);
                inputLoop.exec();
            } else {
                sendKey(&previewDeck, Qt::Key_A, QStringLiteral("a"));
            }
            QLineEdit *search = previewDeck.findChild<QLineEdit *>();
            require(search && search->isVisible() && search->text() == QStringLiteral("a"),
                    QStringLiteral("preview-navigation-resumes-deck-search"));
            previewDeck.hide();
        }
        {
            HistoryRepository mouseRepository(selfTestDirectory.filePath(
                QStringLiteral("mouse-paste.sqlite")));
            ClipboardItem mouseItem;
            mouseItem.text = QStringLiteral("mouse-paste-target");
            mouseItem.copiedAt = now;
            mouseRepository.save(mouseItem);
            DeckWindow mouseDeck(display, &mouseRepository);
            auto waitForEvents = [](int milliseconds) {
                QEventLoop loop;
                QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
                loop.exec();
            };
            auto sendMouse = [&mouseDeck](QEvent::Type type, Qt::MouseButtons buttons) {
                const QPoint local(kContentInset + 40, mouseDeck.height() - 100);
                QMouseEvent event(type, local, mouseDeck.mapToGlobal(local),
                                  Qt::LeftButton, buttons, Qt::NoModifier);
                QApplication::sendEvent(&mouseDeck, &event);
            };
            // Cover both Qt's native DblClick and UOS's two-press fallback.
            for (bool fallback : {false, true}) {
                QApplication::clipboard()->setText(QStringLiteral("before-mouse-paste"));
                mouseDeck.showNearPointer();
                sendMouse(QEvent::MouseButtonPress, Qt::LeftButton);
                sendMouse(QEvent::MouseButtonRelease, Qt::NoButton);
                sendMouse(fallback ? QEvent::MouseButtonPress : QEvent::MouseButtonDblClick,
                          Qt::LeftButton);
                waitForEvents(60);
                require(mouseDeck.isVisible()
                            && QApplication::clipboard()->text() == QStringLiteral("before-mouse-paste"),
                        QStringLiteral("double-click-defers-paste-until-release"));
                sendMouse(QEvent::MouseButtonRelease, Qt::NoButton);
                waitForEvents(60);
                require(!mouseDeck.isVisible()
                            && QApplication::clipboard()->text() == mouseItem.text,
                        QStringLiteral("double-click-pastes-on-release"));
            }
            // A new cut/copy must supersede an older mouse-paste gesture
            // which is still queued for the next event-loop turn.
            mouseDeck.showNearPointer();
            sendMouse(QEvent::MouseButtonPress, Qt::LeftButton);
            sendMouse(QEvent::MouseButtonRelease, Qt::NoButton);
            sendMouse(QEvent::MouseButtonDblClick, Qt::LeftButton);
            sendMouse(QEvent::MouseButtonRelease, Qt::NoButton);
            QApplication::clipboard()->setText(QStringLiteral("new-cut-before-queued-paste"));
            waitForEvents(60);
            require(QApplication::clipboard()->text() == QStringLiteral("new-cut-before-queued-paste"),
                    QStringLiteral("new-cut-cancels-queued-mouse-paste"));
            mouseDeck.hide();
            if (display) {
                // Separate X client: detect Ctrl+V delivered to the original
                // focus, and whether it arrives while button 1 is still held.
                Display *receiver = XOpenDisplay(nullptr);
                require(receiver != nullptr, QStringLiteral("mouse-paste-x11-receiver"));
                if (receiver) {
                    const Window target = XCreateSimpleWindow(receiver,
                        DefaultRootWindow(receiver), 20, 20, 500, 180, 0, 0, 0);
                    XSelectInput(receiver, target, KeyPressMask | KeyReleaseMask);
                    XMapWindow(receiver, target);
                    XSetInputFocus(receiver, target, RevertToParent, CurrentTime);
                    XSync(receiver, False);
                    mouseDeck.showNearPointer();
                    QCursor::setPos(mouseDeck.mapToGlobal(
                        QPoint(kContentInset + 40, mouseDeck.height() - 100)));
                    XSync(display, False);
                    XTestFakeButtonEvent(display, 1, True, CurrentTime);
                    XTestFakeButtonEvent(display, 1, False, CurrentTime);
                    XFlush(display);
                    waitForEvents(40);
                    XTestFakeButtonEvent(display, 1, True, CurrentTime);
                    XFlush(display);
                    waitForEvents(100);
                    require(mouseDeck.isVisible(), QStringLiteral("native-double-click-keeps-deck-until-release"));
                    XTestFakeButtonEvent(display, 1, False, CurrentTime);
                    XFlush(display);
                    waitForEvents(150);
                    int pasteCount = 0;
                    bool pastedWhileMouseHeld = false;
                    while (XPending(receiver)) {
                        XEvent event{};
                        XNextEvent(receiver, &event);
                        if (event.type == 2 /* X11 KeyPress */
                                && XLookupKeysym(&event.xkey, 0) == XK_v
                                && (event.xkey.state & ControlMask)) {
                            ++pasteCount;
                            pastedWhileMouseHeld |= (event.xkey.state & Button1Mask) != 0;
                        }
                    }
                    require(pasteCount == 1 && !pastedWhileMouseHeld,
                            QStringLiteral("native-double-click-delivers-one-paste-after-release"));
                    mouseDeck.showNearPointer();
                    mouseDeck.pasteSelected(false, true);
                    QApplication::clipboard()->setText(QStringLiteral("new-cut-before-delayed-control-v"));
                    waitForEvents(150);
                    int obsoletePasteCount = 0;
                    while (XPending(receiver)) {
                        XEvent event{};
                        XNextEvent(receiver, &event);
                        if (event.type == 2 && XLookupKeysym(&event.xkey, 0) == XK_v
                                && (event.xkey.state & ControlMask)) ++obsoletePasteCount;
                    }
                    require(obsoletePasteCount == 0,
                            QStringLiteral("new-cut-cancels-obsolete-control-v"));
                    mouseDeck.showNearPointer();
                    mouseDeck.pasteSelected(false, true);
                    QApplication::clipboard()->setText(mouseItem.text);
                    waitForEvents(150);
                    int sameContentPasteCount = 0;
                    while (XPending(receiver)) {
                        XEvent event{};
                        XNextEvent(receiver, &event);
                        if (event.type == 2 && XLookupKeysym(&event.xkey, 0) == XK_v
                                && (event.xkey.state & ControlMask)) ++sameContentPasteCount;
                    }
                    require(sameContentPasteCount == 1,
                            QStringLiteral("same-content-clipboard-handoff-keeps-paste"));
                    mouseDeck.showNearPointer();
                    mouseDeck.handleNativeQuickPaste(XKeysymToKeycode(display, XK_1), ControlMask);
                    QApplication::clipboard()->setText(QStringLiteral("new-cut-before-queued-quick-paste"));
                    waitForEvents(100);
                    require(QApplication::clipboard()->text()
                                == QStringLiteral("new-cut-before-queued-quick-paste"),
                            QStringLiteral("new-cut-cancels-queued-quick-paste"));
                    mouseDeck.hide();
                    XDestroyWindow(receiver, target);
                    XCloseDisplay(receiver);
                }
            }
        }
        {
            QTemporaryDir asyncDirectory;
            HistoryRepository asyncRepository(asyncDirectory.filePath(QStringLiteral("async-deck.sqlite")));
            const qint64 captureTime = QDateTime::currentMSecsSinceEpoch();
            const auto content = [](int index) {
                return QStringLiteral("async-card-%1\n").arg(index) + QString(6000, QLatin1Char('b'));
            };
            for (int index = 0; index < 30; ++index) {
                ClipboardItem item;
                item.text = content(index);
                item.copiedAt = QDateTime::fromMSecsSinceEpoch(captureTime + index);
                asyncRepository.save(item);
            }
            ClipboardPipeline asyncPipeline(asyncRepository.databasePath(), false, {});
            DeckWindow asyncDeck(nullptr, &asyncRepository, nullptr, &asyncPipeline);
            asyncPipeline.start();
            const auto waitUntil = [&](const std::function<bool()> &done) {
                QElapsedTimer elapsed;
                elapsed.start();
                while (!done() && elapsed.elapsed() < 5000) {
                    app.processEvents(QEventLoop::AllEvents, 5);
                    QThread::msleep(1);
                }
                return done();
            };
            const auto pump = [&](int duration) {
                QEventLoop loop;
                QTimer::singleShot(duration, &loop, &QEventLoop::quit);
                loop.exec();
            };
            require(waitUntil([&] { return asyncPipeline.recentItems().size() == 10; }),
                    QStringLiteral("async-deck-startup-ten-item-cache"));
            asyncDeck.showNearPointer();
            pump(150);
            for (int index = 0; index < 10; ++index) {
                QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
                QApplication::sendEvent(&asyncDeck, &right);
            }
            asyncDeck.pasteSelected();
            require(waitUntil([&] { return QApplication::clipboard()->text() == content(19); }),
                    QStringLiteral("async-deck-loads-beyond-cache-and-pastes-full-selected-id"));
            asyncDeck.showNearPointer();
            asyncDeck.pasteSelected();
            QApplication::clipboard()->setText(QStringLiteral("new-cut-during-payload-load"));
            pump(150);
            require(QApplication::clipboard()->text() == QStringLiteral("new-cut-during-payload-load"),
                    QStringLiteral("async-payload-load-cannot-overwrite-new-native-cut"));
            ClipboardSnapshot incoming;
            incoming.text = QByteArrayLiteral("new-capture-goes-to-history-first");
            incoming.copiedAt = QDateTime::fromMSecsSinceEpoch(captureTime + 100);
            asyncPipeline.submit(incoming);
            require(waitUntil([&] {
                return !asyncPipeline.recentItems().isEmpty()
                    && asyncPipeline.recentItems().first().text == QString::fromUtf8(incoming.text);
            }), QStringLiteral("async-deck-receives-new-history"));
            asyncDeck.pasteSelected();
            require(QApplication::clipboard()->text() == QString::fromUtf8(incoming.text),
                    QStringLiteral("async-deck-new-capture-selects-history-first"));
            asyncDeck.hide();
        }
        {
            QTemporaryDir filesDirectory;
            const QString livePath=filesDirectory.filePath(QStringLiteral("live.pdf"));
            const QString missingPath=filesDirectory.filePath(QStringLiteral("missing.epub"));
            QFile live(livePath);live.open(QIODevice::WriteOnly);live.write("%PDF-1.4");live.close();
            HistoryRepository fileRepository(filesDirectory.filePath(QStringLiteral("deck-files.sqlite")));
            ClipboardItem item;item.type=ClipboardPayloadType::Files;item.title=QStringLiteral("文件 · 2 项");item.copiedAt=now;
            item.files={{livePath,QStringLiteral("live.pdf"),QStringLiteral("application/pdf"),false},
                        {missingPath,QStringLiteral("missing.epub"),QStringLiteral("application/epub+zip"),false}};
            fileRepository.save(item);
            DeckWindow fileDeck(nullptr,&fileRepository);
            FilePasteController *controller=nullptr;
            for(QObject *child:fileDeck.children()) if(auto *found=dynamic_cast<FilePasteController *>(child)) controller=found;
            const auto waitUntil=[&](const std::function<bool()> &done) {
                QElapsedTimer elapsed;elapsed.start();
                while(!done() && elapsed.elapsed()<3000) {app.processEvents(QEventLoop::AllEvents,5);QThread::msleep(1);}
                return done();
            };
            const auto key=[&](int value,Qt::KeyboardModifiers modifiers=Qt::NoModifier) {
                QKeyEvent press(QEvent::KeyPress,value,modifiers);QApplication::sendEvent(&fileDeck,&press);
            };
            QApplication::clipboard()->setText(QStringLiteral("before-file-paste"));
            fileDeck.showNearPointer();
            bool sawFilePreview=false;
            QTimer::singleShot(0,&fileDeck,[&] {
                for(QDialog *dialog:fileDeck.findChildren<QDialog *>()) {
                    if(!dialog->isVisible()) continue;
                    sawFilePreview=true;
                    const auto *label=dialog->findChild<QLabel *>(QStringLiteral("previewCategory"));
                    require(label && label->text()==QStringLiteral("文件 · 2 项"),QStringLiteral("file-preview-type-count-heading"));
                    const QString directory=qEnvironmentVariable("CLIPDECK_RENDER_DIRECTORY");
                    if(!directory.isEmpty()) {QDir().mkpath(directory);dialog->grab().save(QDir(directory).filePath(QStringLiteral("file-popover.png")));}
                    key(Qt::Key_Down);key(Qt::Key_Up);dialog->reject();break;
                }
            });
            key(Qt::Key_Space);
            require(sawFilePreview && fileDeck.isVisible(),QStringLiteral("file-preview-navigation-keeps-deck-open"));
            key(Qt::Key_Return);
            require(controller && waitUntil([&]{return controller->feedback().phase==FilePasteFeedback::Partial;}),
                    QStringLiteral("deck-file-enter-shows-partial-confirmation"));
            require(fileDeck.isVisible() && QApplication::clipboard()->text()==QStringLiteral("before-file-paste"),
                    QStringLiteral("deck-file-feedback-preserves-clipboard-and-visibility"));
            key(Qt::Key_Escape);
            require(fileDeck.isVisible() && controller && controller->feedback().phase==FilePasteFeedback::Idle,
                    QStringLiteral("deck-file-escape-dismisses-feedback-not-deck"));
            key(Qt::Key_Return);
            require(controller && waitUntil([&]{return controller->feedback().phase==FilePasteFeedback::Partial;}),
                    QStringLiteral("deck-file-retry-rechecks-original-set"));
            key(Qt::Key_Return);
            require(waitUntil([&]{return !fileDeck.isVisible();})
                && QApplication::clipboard()->mimeData()->urls()==QList<QUrl>{QUrl::fromLocalFile(livePath)},
                QStringLiteral("deck-file-confirmation-pastes-only-available-native-references"));
            QFile restored(missingPath);restored.open(QIODevice::WriteOnly);restored.write("PK\003\004document");restored.close();
            fileDeck.showNearPointer();
            QMouseEvent doubleClick(QEvent::MouseButtonDblClick,QPointF(60,90),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
            QMouseEvent release(QEvent::MouseButtonRelease,QPointF(60,90),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
            QApplication::sendEvent(&fileDeck,&doubleClick);QApplication::sendEvent(&fileDeck,&release);
            require(waitUntil([&]{return !fileDeck.isVisible();}) && QApplication::clipboard()->mimeData()->urls().size()==2,
                    QStringLiteral("deck-file-double-click-pastes-original-file-set"));
            fileDeck.showNearPointer();key(Qt::Key_1,Qt::ControlModifier);
            require(waitUntil([&]{return !fileDeck.isVisible();}) && QApplication::clipboard()->mimeData()->urls().size()==2,
                    QStringLiteral("deck-file-quick-paste-shares-file-preflight"));
            QFile::remove(missingPath);
            fileDeck.showNearPointer();key(Qt::Key_Return);
            require(controller && waitUntil([&]{return controller->feedback().phase==FilePasteFeedback::Partial;}),
                    QStringLiteral("deck-file-feedback-before-confirm-button"));
            if(controller && controller->feedback().phase==FilePasteFeedback::Partial) {
                const int side=std::max(166,fileDeck.height()-89);
                const int top=59+(fileDeck.height()-65-side)/2;
                const int heading=side>=180?std::min(58,std::max(48,qRound(side*0.20))):36;
                const QRect content(22,top+heading,side,side-heading);
                const QRect area=fileFeedbackRect(content,controller->feedback());
                const QPoint point=QRect(area.x()+71,area.bottom()-42,std::min(110,area.width()-84),30).center();
                const QString directory=qEnvironmentVariable("CLIPDECK_RENDER_DIRECTORY");
                if(!directory.isEmpty()) fileDeck.grab().save(QDir(directory).filePath(QStringLiteral("deck-files-partial.png")));
                QMouseEvent press(QEvent::MouseButtonPress,QPointF(point),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
                QApplication::sendEvent(&fileDeck,&press);
                QElapsedTimer hold;hold.start();waitUntil([&]{return hold.elapsed()>60;});
                require(fileDeck.isVisible() && QApplication::clipboard()->mimeData()->urls().size()==2,
                        QStringLiteral("file-confirm-button-never-pastes-before-mouse-release"));
                QMouseEvent releaseConfirm(QEvent::MouseButtonRelease,QPointF(point),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
                QApplication::sendEvent(&fileDeck,&releaseConfirm);
                require(waitUntil([&]{return !fileDeck.isVisible();}) && QApplication::clipboard()->mimeData()->urls().size()==1,
                        QStringLiteral("file-confirm-button-pastes-after-release"));
            }
            fileDeck.showNearPointer();
            for(int x:{10,fileDeck.width()/2,fileDeck.width()-10}) {
                QMouseEvent move(QEvent::MouseMove,QPointF(x,9),Qt::NoButton,Qt::NoButton,Qt::NoModifier);
                QApplication::sendEvent(&fileDeck,&move);
                require(fileDeck.cursor().shape()==Qt::SizeVerCursor,QStringLiteral("deck-top-resize-cursor-covers-left-middle-right"));
            }
            fileDeck.hide();
        }
        failures.append(clipboardPipelineSelfTestFailures());
        failures.append(fileReferenceSelfTestFailures());
        const bool passed = failures.isEmpty();
        QTextStream(stdout) << (passed ? QStringLiteral("self-test: ok\n")
                                      : QStringLiteral("self-test: failed: ") + failures.join(QStringLiteral(", "))
                                          + (repository.lastError().isEmpty() ? QString()
                                              : QStringLiteral("; ") + repository.lastError())
                                          + QLatin1Char('\n'));
        return passed ? 0 : 1;
    }
    if (settingsPage >= 0) {
        QApplication::setQuitOnLastWindowClosed(true);
        const auto notifyShortcutRecording = [](bool recording) {
            QTextStream stream(stdout);
            stream << (recording ? QLatin1String(kShortcutRecordingStarted)
                                 : QLatin1String(kShortcutRecordingStopped))
                   << QLatin1Char('\n');
            stream.flush();
        };
        showSettings(&repository, nullptr, settingsPage, {}, QString(), beginNewCategory,
                     notifyShortcutRecording, settingsIPCEnabled);
        return 0;
    }
    repository.clearExpired(settings.value(QStringLiteral("history/retentionDays"), 30).toInt());
    ClipboardPipeline pipeline(repository.databasePath(), display != nullptr);
    const bool rendering = !renderCheckPath.isEmpty() || !settingsRenderPath.isEmpty();
    DeckWindow deck(display, &repository, nullptr, rendering ? nullptr : &pipeline);

    if (!settingsRenderPath.isEmpty()) {
        const QFileInfo output(settingsRenderPath);
        const QString imagePath = output.absolutePath() + QLatin1Char('/')
            + output.completeBaseName() + QStringLiteral("-images.png");
        const QString keyboardPath = output.absolutePath() + QLatin1Char('/')
            + output.completeBaseName() + QStringLiteral("-keyboard.png");
        const QString categoryPath = output.absolutePath() + QLatin1Char('/')
            + output.completeBaseName() + QStringLiteral("-categories.png");
        showSettings(&repository, &deck, 0, {}, settingsRenderPath);
        showSettings(&repository, &deck, 1, {}, imagePath);
        showSettings(&repository, &deck, 2, {}, keyboardPath);
        showSettings(&repository, &deck, 3, {}, categoryPath);
        QTextStream(stdout) << QStringLiteral("settings-render: ok: %1, %2, %3, %4\n")
            .arg(settingsRenderPath, imagePath, keyboardPath, categoryPath);
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
                 QStringLiteral("{\n  \"platform\": \"UOS\",\n  \"parity\": true,\n  \"shortcut\": \"Alt+Shift+V\"\n}"),
                 QStringLiteral("JSON"), 160);
        saveText(QStringLiteral("色彩"), QStringLiteral("#21C46B"), QStringLiteral("色彩"), 240);
        ClipboardItem fileSample;
        fileSample.type=ClipboardPayloadType::Files;
        fileSample.title=QStringLiteral("文件 · 12 项");
        fileSample.sourceName=QStringLiteral("文件管理器");
        fileSample.copiedAt=copiedAt;
        for(int index=0;index<12;++index) fileSample.files.append({
            QStringLiteral("/example/季度报告-%1.pdf").arg(index+1),QStringLiteral("季度报告-%1.pdf").arg(index+1),
            QStringLiteral("application/pdf"),false});
        repository.save(fileSample);

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
        const QString categoryPath = output.absolutePath() + QLatin1Char('/')
            + output.completeBaseName() + QStringLiteral("-categories.png");
        const bool expandedSaved = render(renderCheckPath, 292);
        const bool compactSaved = render(compactPath, 250);
        deck.showNextCategoryPageForRenderCheck();
        const bool categorySaved = render(categoryPath, 292);
        deck.setGeometry(8, 8, 2032, 292);
        deck.show();
        deck.beginSearchForRenderCheck();
        app.processEvents();
        const bool searchSaved = deck.grab().save(searchPath, "PNG");
        deck.hide();
        QTextStream(stdout) << (expandedSaved && compactSaved && categorySaved && searchSaved
            ? QStringLiteral("render-check: ok: %1, %2, %3, %4\n")
                .arg(renderCheckPath, compactPath, categoryPath, searchPath)
            : QStringLiteral("render-check: failed\n"));
        return expandedSaved && compactSaved && categorySaved && searchSaved ? 0 : 1;
    }

    QClipboard *clipboard = QApplication::clipboard();
    QObject::connect(clipboard, &QClipboard::dataChanged, [&] {
        // No MIME reads, image encoding, source lookup or database work here.
        pipeline.captureChanged(deck.isWritingHistoryClipboard() || clipboard->ownsClipboard());
    });
    QObject::connect(&pipeline, &ClipboardPipeline::processingError, [](const QString &message) {
        qWarning() << "[ClipDeck][Capture]" << message;
    });
    pipeline.start();
    QTimer clipboardPollTimer;
    clipboardPollTimer.setInterval(400);
    QObject::connect(&clipboardPollTimer, &QTimer::timeout, [&] {
        if (!clipboard->ownsClipboard()) pipeline.pollClipboard();
    });
    clipboardPollTimer.start();

    // The shipped UOS target uses X11/xcb. Keeping the non-X11 path inert lets
    // the packaged ELF and bundled Qt plugins be smoke-tested in a headless
    // environment where no system-tray host exists.
    if (!display) {
        return app.exec();
    }

    const QString packagedTrayIconPath = QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("../share/tray/clipdeck.svg"));
    const QIcon packagedTrayIcon(packagedTrayIconPath);
    QSystemTrayIcon tray(packagedTrayIcon.isNull()
        ? transparentTrayIcon(DisplayScale::factor()) : packagedTrayIcon);
    QMenu trayMenu;
    QAction *showAction = trayMenu.addAction(QStringLiteral("显示 Deck"));
    QAction *settingsAction = trayMenu.addAction(QStringLiteral("设置…"));
    QAction *blurAction = trayMenu.addAction(QStringLiteral("背景模糊"));
    blurAction->setCheckable(true);
    blurAction->setChecked(DeckMaterial::load().blur > 0);
    settingsAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Comma));
    settingsAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    settingsAction->setShortcutVisibleInContextMenu(true);
    trayMenu.addSeparator();
    QAction *quitAction = trayMenu.addAction(QStringLiteral("退出 ClipDeck"));
    quitAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
    quitAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
    quitAction->setShortcutVisibleInContextMenu(true);
    tray.setContextMenu(&trayMenu);
    GlobalShortcutFilter shortcut(
        [&deck] { deck.toggleNearPointer(); },
        [&deck](const xcb_key_press_event_t *event) {
            return deck.handleNativeQuickPaste(event->detail, event->state);
        },
        [] {
            return configuredShortcut(QStringLiteral("shortcuts/showDeck"),
                                      defaultShowDeckShortcut());
        });
    app.installNativeEventFilter(&shortcut);
    auto updateShortcutPresentation = [&] {
        const QKeySequence sequence = configuredShortcut(
            QStringLiteral("shortcuts/showDeck"), defaultShowDeckShortcut());
        const QString label = sequence.toString(QKeySequence::NativeText);
        showAction->setText(QStringLiteral("显示 Deck"));
        showAction->setShortcut(sequence);
        showAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        showAction->setShortcutVisibleInContextMenu(true);
        tray.setToolTip(QStringLiteral("ClipDeck · %1 呼出").arg(label));
    };
    updateShortcutPresentation();

    QProcess *settingsProcess = nullptr;
    QByteArray settingsProcessOutput;
    auto openSettingsProcess = [&](int page) {
        if (settingsProcess && settingsProcess->state() != QProcess::NotRunning) {
            deck.hide();
            const bool categoryEntry = page == 3 || page == 5;
            const int requestedPage = categoryEntry ? 3 : -1;
            const QByteArray command = QByteArray(kSettingsActivatePrefix)
                + QByteArray::number(requestedPage) + ':' + (page == 5 ? "1\n" : "0\n");
            settingsProcess->write(command);
            qInfo() << "[ClipDeck][Settings] activating existing window page="
                    << requestedPage << "createCategory=" << (page == 5);
            return;
        }
        deck.hide();
        QProcess *process = new QProcess(&app);
        settingsProcess = process;
        settingsProcessOutput.clear();
        process->setProgram(QCoreApplication::applicationFilePath());
        const bool createCategory = page == 5;
        QStringList arguments{QStringLiteral("--settings-page=%1").arg(createCategory ? 3 : page)};
        arguments.append(QStringLiteral("--settings-ipc"));
        if (createCategory) arguments.append(QStringLiteral("--new-category"));
        process->setArguments(arguments);
        qInfo() << "[ClipDeck][Settings] starting" << arguments;
        QObject::connect(process, &QProcess::readyReadStandardOutput,
                         [&, process] {
            settingsProcessOutput.append(process->readAllStandardOutput());
            int newline = -1;
            while ((newline = settingsProcessOutput.indexOf('\n')) >= 0) {
                const QByteArray message = settingsProcessOutput.left(newline).trimmed();
                settingsProcessOutput.remove(0, newline + 1);
                if (message == QByteArray(kShortcutRecordingStarted)) {
                    shortcut.suspend();
                    qInfo() << "[ClipDeck][Shortcut] suspended for settings capture";
                } else if (message == QByteArray(kShortcutRecordingStopped)) {
                    settings.sync();
                    shortcut.configure();
                    updateShortcutPresentation();
                    qInfo() << "[ClipDeck][Shortcut] restored after settings capture";
                }
            }
        });
        QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                         [&, process](int, QProcess::ExitStatus) {
            settings.sync();
            shortcut.configure();
            updateShortcutPresentation();
            deck.refresh();
            settingsProcess = nullptr;
            process->deleteLater();
        });
        QObject::connect(process, &QProcess::errorOccurred, [&, process](QProcess::ProcessError error) {
            qWarning() << "[ClipDeck][Settings] process error" << error << process->errorString();
            if (error == QProcess::FailedToStart) {
                shortcut.configure();
                settingsProcess = nullptr;
                process->deleteLater();
            }
        });
        process->start();
    };

    QObject::connect(showAction, &QAction::triggered, [&deck] { deck.toggleNearPointer(); });
    QObject::connect(settingsAction, &QAction::triggered, [&] { openSettingsProcess(0); });
    QObject::connect(blurAction, &QAction::toggled, [&deck](bool enabled) {
        DeckMaterial material = deck.material();
        material.blur = enabled ? 1 : 0;
        deck.setMaterial(material);
    });
    deck.setSettingsCallback([&](int page) { openSettingsProcess(page); });
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
