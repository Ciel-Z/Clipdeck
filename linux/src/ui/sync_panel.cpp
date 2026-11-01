#include "sync_panel.h"
#include "sync/sync_service.h"

#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QGuiApplication>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QScreen>
#include <QVBoxLayout>

namespace {

QLabel *sectionTitle(const QString &text, QWidget *parent) {
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("sectionTitle"));
    return label;
}

QFrame *group(QWidget *parent) {
    auto *frame = new QFrame(parent);
    frame->setObjectName(QStringLiteral("group"));
    return frame;
}

void clearLayout(QLayout *layout) {
    while (QLayoutItem *item = layout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

} // namespace

SyncPanel::SyncPanel(SyncService *sync, QWidget *parent) : QWidget(parent), sync_(sync) {
    setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setWindowTitle(QStringLiteral("局域网同步"));
    setFixedWidth(380);
    setStyleSheet(QStringLiteral(
        "QWidget{color:#eeeeef;font-size:13px;}"
        "QLabel#title{font-size:16px;font-weight:700;color:white;}"
        "QLabel#secondary,QLabel#sectionTitle{color:#8e8e93;font-size:12px;}"
        "QLabel#sectionTitle{font-weight:600;}"
        "QLabel#hint{color:#ff9f0a;font-size:12px;}"
        "QLabel#error{color:#ff6961;font-size:12px;}"
        "QLabel#code{font-family:monospace;font-size:28px;font-weight:700;color:white;letter-spacing:2px;}"
        "QFrame#group{background:rgba(255,255,255,13);border-radius:10px;}"
        "QFrame#incoming{background:rgba(10,132,255,40);border:1px solid rgba(10,132,255,115);border-radius:10px;}"
        "QLineEdit{background:transparent;border:0;color:white;font-size:14px;font-weight:600;}"
        "QLineEdit#codeInput{background:rgba(0,0,0,77);border:1px solid rgba(255,255,255,38);border-radius:6px;"
        "padding:4px 8px;font-family:monospace;font-size:16px;}"
        "QLineEdit#codeInput:focus{border-color:#0a84ff;}"
        "QPushButton{background:rgba(255,255,255,26);border:0;border-radius:6px;padding:4px 12px;color:#eeeeef;}"
        "QPushButton:hover{background:rgba(255,255,255,40);}"
        "QPushButton#accent{background:#0a84ff;color:white;font-weight:600;}"
        "QPushButton#destructive{color:#ff6961;}"
        "QPushButton#link{background:transparent;color:#bbbbc0;padding:0;}"
        "QCheckBox::indicator{width:40px;height:24px;border-radius:12px;background:rgba(255,255,255,41);}"
        "QCheckBox::indicator:checked{background:#0a84ff;}"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(12);

    auto *header = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("局域网同步"), this);
    title->setObjectName(QStringLiteral("title"));
    status_ = new QLabel(this);
    status_->setObjectName(QStringLiteral("secondary"));
    auto *close = new QPushButton(QStringLiteral("✕"), this);
    close->setObjectName(QStringLiteral("link"));
    connect(close, &QPushButton::clicked, this, &QWidget::hide);
    header->addWidget(title);
    header->addStretch();
    header->addWidget(status_);
    header->addSpacing(8);
    header->addWidget(close);
    layout->addLayout(header);

    hint_ = new QLabel(this);
    hint_->setObjectName(QStringLiteral("hint"));
    hint_->setWordWrap(true);
    layout->addWidget(hint_);

    // Incoming pairing request.
    incoming_ = new QFrame(this);
    incoming_->setObjectName(QStringLiteral("incoming"));
    auto *incomingLayout = new QVBoxLayout(incoming_);
    incomingLayout->setContentsMargins(14, 12, 14, 12);
    incomingTitle_ = new QLabel(incoming_);
    incomingTitle_->setWordWrap(true);
    incomingCode_ = new QLabel(incoming_);
    incomingCode_->setObjectName(QStringLiteral("code"));
    incomingDetail_ = new QLabel(incoming_);
    incomingDetail_->setObjectName(QStringLiteral("secondary"));
    incomingDetail_->setWordWrap(true);
    incomingButton_ = new QPushButton(incoming_);
    connect(incomingButton_, &QPushButton::clicked, this, [this] {
        if (sync_->pairing().value(QStringLiteral("stage")).toString() == QStringLiteral("failed")) sync_->cancelPairing();
        else sync_->rejectPairing();
    });
    incomingLayout->addWidget(incomingTitle_);
    incomingLayout->addWidget(incomingCode_);
    incomingLayout->addWidget(incomingDetail_);
    incomingLayout->addWidget(incomingButton_, 0, Qt::AlignLeft);
    layout->addWidget(incoming_);

    // This device: editable name and visibility.
    layout->addWidget(sectionTitle(QStringLiteral("本机"), this));
    QFrame *device = group(this);
    auto *deviceLayout = new QVBoxLayout(device);
    deviceLayout->setContentsMargins(12, 8, 12, 8);
    name_ = new QLineEdit(device);
    name_->setToolTip(QStringLiteral("点击修改本机名称"));
    connect(name_, &QLineEdit::editingFinished, this, [this] { sync_->setDeviceName(name_->text()); });
    deviceLayout->addWidget(name_);
    auto *visibleRow = new QHBoxLayout;
    auto *visibleText = new QVBoxLayout;
    visibleTitle_ = new QLabel(device);
    visibleDetail_ = new QLabel(device);
    visibleDetail_->setObjectName(QStringLiteral("secondary"));
    visibleText->addWidget(visibleTitle_);
    visibleText->addWidget(visibleDetail_);
    visibleToggle_ = new QCheckBox(device);
    connect(visibleToggle_, &QCheckBox::toggled, sync_, &SyncService::setVisible);
    visibleRow->addLayout(visibleText, 1);
    visibleRow->addWidget(visibleToggle_);
    deviceLayout->addLayout(visibleRow);
    layout->addWidget(device);

    // Paired devices.
    devicesSection_ = new QWidget(this);
    auto *devicesSection = new QVBoxLayout(devicesSection_);
    devicesSection->setContentsMargins(0, 0, 0, 0);
    devicesSection->addWidget(sectionTitle(QStringLiteral("我的设备"), devicesSection_));
    QFrame *devices = group(devicesSection_);
    devicesLayout_ = new QVBoxLayout(devices);
    devicesLayout_->setContentsMargins(0, 4, 0, 4);
    devicesSection->addWidget(devices);
    layout->addWidget(devicesSection_);

    // Nearby devices.
    auto *nearbyHeader = new QHBoxLayout;
    nearbyHeader->addWidget(sectionTitle(QStringLiteral("附近的设备"), this));
    nearbyHeader->addStretch();
    refresh_ = new QPushButton(QStringLiteral("刷新"), this);
    refresh_->setObjectName(QStringLiteral("link"));
    connect(refresh_, &QPushButton::clicked, sync_, [this] { sync_->searchNearby(true); });
    nearbyHeader->addWidget(refresh_);
    layout->addLayout(nearbyHeader);
    QFrame *nearby = group(this);
    nearbyLayout_ = new QVBoxLayout(nearby);
    nearbyLayout_->setContentsMargins(0, 4, 0, 4);
    layout->addWidget(nearby);

    auto *pauseRow = new QHBoxLayout;
    pauseRow->addWidget(new QLabel(QStringLiteral("暂停同步"), this));
    pauseRow->addStretch();
    pausedToggle_ = new QCheckBox(this);
    connect(pausedToggle_, &QCheckBox::toggled, sync_, &SyncService::setPaused);
    pauseRow->addWidget(pausedToggle_);
    layout->addLayout(pauseRow);

    connect(sync_, &SyncService::stateChanged, this, &SyncPanel::updateState);
    connect(sync_, &SyncService::devicesChanged, this, &SyncPanel::rebuildLists);
    connect(sync_, &SyncService::pairingChanged, this, [this] {
        updateState();
        rebuildLists();
    });
    updateState();
    rebuildLists();
}

void SyncPanel::openNear(const QPoint &anchor) {
    openedAtMs_ = QDateTime::currentMSecsSinceEpoch();
    adjustSize();
    // Qt 5.11: QGuiApplication::screenAt can be null on X11; find it by hand.
    QScreen *screen = QGuiApplication::primaryScreen();
    for (QScreen *candidate : QGuiApplication::screens()) {
        if (candidate->geometry().contains(anchor)) screen = candidate;
    }
    const QRect available = screen ? screen->availableGeometry() : QRect(anchor - QPoint(400, 400), QSize(800, 800));
    int x = std::min(anchor.x() - width() / 2, available.right() - width() - 8);
    int y = anchor.y() > available.center().y() ? anchor.y() - height() - 12 : anchor.y() + 12;
    x = std::max(available.left() + 8, x);
    y = std::max(available.top() + 8, std::min(y, available.bottom() - height() - 8));
    move(x, y);
    show();
    raise();
    activateWindow();
    if (sync_->nearby().isEmpty()) sync_->searchNearby();
}

bool SyncPanel::pairingKeepsPanelOpen() const {
    const QVariantMap pairing = sync_->pairing();
    if (!pairing.value(QStringLiteral("active")).toBool()) return false;
    return pairing.value(QStringLiteral("role")).toString() == QStringLiteral("incoming")
        || pairing.value(QStringLiteral("stage")).toString() == QStringLiteral("code");
}

void SyncPanel::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && !isActiveWindow() && isVisible()
        && QDateTime::currentMSecsSinceEpoch() - openedAtMs_ > 600 && !pairingKeepsPanelOpen()) {
        hide();
    }
}

void SyncPanel::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Escape) hide();
    else QWidget::keyPressEvent(event);
}

void SyncPanel::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
    painter.fillPath(path, QColor(37, 37, 40));
    painter.setPen(QPen(QColor(255, 255, 255, 30), 1));
    painter.drawPath(path);
}

void SyncPanel::updateState() {
    status_->setText(sync_->statusText());
    const QString hint = !sync_->isAvailable() ? QStringLiteral("同步服务无法启动（端口被占用或网络不可用）")
                                               : sync_->networkHint();
    hint_->setText(hint);
    hint_->setVisible(!hint.isEmpty());
    if (!name_->hasFocus()) name_->setText(sync_->deviceName());
    const int left = sync_->visibleSecondsLeft();
    visibleTitle_->setText(sync_->isVisible()
                               ? QStringLiteral("局域网可见 · %1:%2").arg(left / 60).arg(left % 60, 2, 10, QLatin1Char('0'))
                               : QStringLiteral("局域网可见"));
    visibleDetail_->setText(sync_->isVisible() ? QStringLiteral("本次标识 %1").arg(sync_->visibilityTag())
                                               : QStringLiteral("开启后 3 分钟内可被附近设备发现"));
    {
        const QSignalBlocker blocker(visibleToggle_);
        visibleToggle_->setChecked(sync_->isVisible());
    }
    {
        const QSignalBlocker blocker(pausedToggle_);
        pausedToggle_->setChecked(sync_->isPaused());
    }
    visibleToggle_->setEnabled(sync_->isAvailable());
    pausedToggle_->setEnabled(sync_->isAvailable());
    refresh_->setText(sync_->isSearching() ? QStringLiteral("正在查找…") : QStringLiteral("刷新"));

    const QVariantMap pairing = sync_->pairing();
    const bool incoming = pairing.value(QStringLiteral("active")).toBool()
        && pairing.value(QStringLiteral("role")).toString() == QStringLiteral("incoming");
    incoming_->setVisible(incoming);
    if (incoming) {
        const QString stage = pairing.value(QStringLiteral("stage")).toString();
        incomingTitle_->setText(QStringLiteral("%1（%2）请求配对")
                                    .arg(pairing.value(QStringLiteral("hostname")).toString(),
                                         pairing.value(QStringLiteral("address")).toString()));
        const QString code = pairing.value(QStringLiteral("code")).toString();
        incomingCode_->setText(code.left(3) + QLatin1Char(' ') + code.mid(3));
        incomingCode_->setVisible(stage == QStringLiteral("code") || stage == QStringLiteral("verifying"));
        incomingDetail_->setObjectName(stage == QStringLiteral("failed") ? QStringLiteral("error") : QStringLiteral("secondary"));
        incomingDetail_->setStyleSheet(QString()); // re-evaluate the object name
        incomingDetail_->setText(stage == QStringLiteral("done")        ? QStringLiteral("配对成功")
                                 : stage == QStringLiteral("failed")    ? pairing.value(QStringLiteral("error")).toString()
                                 : stage == QStringLiteral("finishing") ? QStringLiteral("正在完成配对…")
                                                                        : QStringLiteral("请在对方设备上输入此配对码，2 分钟内有效"));
        incomingButton_->setText(stage == QStringLiteral("failed") ? QStringLiteral("关闭") : QStringLiteral("拒绝"));
        incomingButton_->setVisible(stage == QStringLiteral("code") || stage == QStringLiteral("failed"));
    }
    if (isVisible()) adjustSize();
}

QWidget *SyncPanel::deviceRow(const QVariantMap &device, bool paired) {
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(14, 6, 10, 6);
    auto *text = new QVBoxLayout;
    auto *name = new QLabel(QStringLiteral("%1 · %2").arg(device.value(QStringLiteral("hostname")).toString(),
                                                         device.value(QStringLiteral("address")).toString()),
                            row);
    auto *detail = new QLabel(row);
    detail->setObjectName(QStringLiteral("secondary"));
    text->addWidget(name);
    text->addWidget(detail);
    if (paired) {
        const QString state = device.value(QStringLiteral("state")).toString();
        auto *dot = new QLabel(state == QStringLiteral("offline") ? QStringLiteral("○") : QStringLiteral("●"), row);
        dot->setStyleSheet(state == QStringLiteral("offline") ? QStringLiteral("color:#8e8e93;")
                                                              : QStringLiteral("color:#30d158;"));
        layout->addWidget(dot);
        detail->setText(state == QStringLiteral("syncing") ? QStringLiteral("同步中")
                                                           : device.value(QStringLiteral("lastSeen")).toString());
        layout->addLayout(text, 1);
        const QString deviceID = device.value(QStringLiteral("deviceId")).toString();
        auto *syncNow = new QPushButton(QStringLiteral("立即同步"), row);
        syncNow->setEnabled(!sync_->isPaused());
        connect(syncNow, &QPushButton::clicked, sync_, &SyncService::syncNow);
        auto *unpair = new QPushButton(QStringLiteral("取消配对"), row);
        unpair->setObjectName(QStringLiteral("destructive"));
        connect(unpair, &QPushButton::clicked, this, [this, deviceID] { sync_->unpair(deviceID); });
        layout->addWidget(syncNow);
        layout->addWidget(unpair);
        return row;
    }
    detail->setText(QStringLiteral("标识 %1").arg(device.value(QStringLiteral("tag")).toString()));
    layout->addLayout(text, 1);
    const QString key = device.value(QStringLiteral("key")).toString();
    const QVariantMap pairing = sync_->pairing();
    const bool pairingThis = pairing.value(QStringLiteral("active")).toBool()
        && pairing.value(QStringLiteral("role")).toString() == QStringLiteral("outgoing")
        && pairing.value(QStringLiteral("key")).toString() == key;
    if (!pairingThis) {
        auto *pair = new QPushButton(QStringLiteral("配对"), row);
        pair->setObjectName(QStringLiteral("accent"));
        pair->setEnabled(device.value(QStringLiteral("pairable")).toBool() && !pairing.value(QStringLiteral("active")).toBool());
        connect(pair, &QPushButton::clicked, this, [this, key] { sync_->pair(key); });
        layout->addWidget(pair);
        return row;
    }
    auto *column = new QWidget;
    auto *columnLayout = new QVBoxLayout(column);
    columnLayout->setContentsMargins(0, 0, 0, 0);
    columnLayout->setSpacing(0);
    columnLayout->addWidget(row);
    columnLayout->addWidget(pairingRow());
    return column;
}

QWidget *SyncPanel::pairingRow() {
    const QVariantMap pairing = sync_->pairing();
    const QString stage = pairing.value(QStringLiteral("stage")).toString();
    auto *row = new QWidget;
    auto *layout = new QVBoxLayout(row);
    layout->setContentsMargins(14, 0, 14, 8);
    if (stage == QStringLiteral("code")) {
        auto *entry = new QHBoxLayout;
        // The field survives list rebuilds so typing is not interrupted.
        if (!code_) {
            code_ = new QLineEdit;
            code_->setObjectName(QStringLiteral("codeInput"));
            code_->setPlaceholderText(QStringLiteral("配对码"));
            code_->setMaxLength(6);
            code_->setFixedWidth(120);
            code_->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("\\d{0,6}")), code_));
            connect(code_, &QLineEdit::returnPressed, this, [this] {
                if (code_->text().size() == 6) sync_->submitPairingCode(code_->text());
            });
        }
        code_->setParent(row);
        // Cleared only when a new attempt starts (after a wrong code).
        const int attemptsLeft = pairing.value(QStringLiteral("attemptsLeft")).toInt();
        if (code_->property("attemptsLeft").toInt() != attemptsLeft) {
            code_->clear();
            code_->setProperty("attemptsLeft", attemptsLeft);
        }
        auto *confirm = new QPushButton(QStringLiteral("确定"), row);
        confirm->setObjectName(QStringLiteral("accent"));
        connect(confirm, &QPushButton::clicked, this, [this] {
            if (code_ && code_->text().size() == 6) sync_->submitPairingCode(code_->text());
        });
        auto *cancel = new QPushButton(QStringLiteral("取消"), row);
        connect(cancel, &QPushButton::clicked, sync_, &SyncService::cancelPairing);
        entry->addWidget(code_);
        entry->addWidget(confirm);
        entry->addWidget(cancel);
        entry->addStretch();
        layout->addLayout(entry);
        const QString error = pairing.value(QStringLiteral("error")).toString();
        if (!error.isEmpty()) {
            auto *label = new QLabel(QStringLiteral("%1，还可尝试 %2 次")
                                         .arg(error)
                                         .arg(pairing.value(QStringLiteral("attemptsLeft")).toInt()),
                                     row);
            label->setObjectName(QStringLiteral("error"));
            layout->addWidget(label);
        }
        code_->setFocus();
    } else if (stage == QStringLiteral("failed")) {
        auto *line = new QHBoxLayout;
        auto *label = new QLabel(pairing.value(QStringLiteral("error")).toString(), row);
        label->setObjectName(QStringLiteral("error"));
        auto *close = new QPushButton(QStringLiteral("关闭"), row);
        connect(close, &QPushButton::clicked, sync_, &SyncService::cancelPairing);
        line->addWidget(label);
        line->addStretch();
        line->addWidget(close);
        layout->addLayout(line);
    } else {
        auto *label = new QLabel(stage == QStringLiteral("done")         ? QStringLiteral("配对成功")
                                 : stage == QStringLiteral("connecting") ? QStringLiteral("正在连接…")
                                                                         : QStringLiteral("正在验证…"),
                                 row);
        label->setObjectName(QStringLiteral("secondary"));
        layout->addWidget(label);
    }
    return row;
}

void SyncPanel::rebuildLists() {
    // Keep the code field alive while its row is rebuilt.
    if (code_) code_->setParent(nullptr);
    clearLayout(devicesLayout_);
    clearLayout(nearbyLayout_);
    const QVariantList devices = sync_->devices();
    devicesSection_->setVisible(!devices.isEmpty());
    for (const QVariant &device : devices) devicesLayout_->addWidget(deviceRow(device.toMap(), true));
    const QVariantList nearby = sync_->nearby();
    if (nearby.isEmpty()) {
        auto *empty = new QLabel(sync_->isSearching() ? QStringLiteral("正在查找…")
                                                      : QStringLiteral("没有找到。请在另一台设备上开启「局域网可见」后刷新"));
        empty->setObjectName(QStringLiteral("secondary"));
        empty->setWordWrap(true);
        empty->setAlignment(Qt::AlignCenter);
        empty->setContentsMargins(14, 12, 14, 12);
        nearbyLayout_->addWidget(empty);
    }
    for (const QVariant &device : nearby) nearbyLayout_->addWidget(deviceRow(device.toMap(), false));
    if (code_ && !code_->parent()) code_->deleteLater();
    refresh_->setText(sync_->isSearching() ? QStringLiteral("正在查找…") : QStringLiteral("刷新"));
    if (isVisible()) adjustSize();
}
