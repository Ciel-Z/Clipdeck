#pragma once

#include <QPointer>
#include <QWidget>

class QCheckBox;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;
class SyncService;

// 同步面板 (docs/lan-sync-design.md §8), the Linux counterpart of
// windows/qml/SyncPanel.qml: this device, paired devices, nearby devices and
// pairing inline. Hides when it loses focus unless a pairing code is showing.
class SyncPanel final : public QWidget {
public:
    explicit SyncPanel(SyncService *sync, QWidget *parent = nullptr);

    // Opens next to `anchor` (the pointer when the tray menu was used).
    void openNear(const QPoint &anchor);

protected:
    void changeEvent(QEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void updateState();
    void rebuildLists();
    QWidget *deviceRow(const QVariantMap &device, bool paired);
    QWidget *pairingRow();
    bool pairingKeepsPanelOpen() const;

    SyncService *sync_;
    QLabel *status_ = nullptr;
    QLabel *hint_ = nullptr;
    QLineEdit *name_ = nullptr;
    QLabel *visibleTitle_ = nullptr;
    QLabel *visibleDetail_ = nullptr;
    QCheckBox *visibleToggle_ = nullptr;
    QCheckBox *pausedToggle_ = nullptr;
    QFrame *incoming_ = nullptr;
    QLabel *incomingTitle_ = nullptr;
    QLabel *incomingCode_ = nullptr;
    QLabel *incomingDetail_ = nullptr;
    QPushButton *incomingButton_ = nullptr;
    QWidget *devicesSection_ = nullptr;
    QVBoxLayout *devicesLayout_ = nullptr;
    QVBoxLayout *nearbyLayout_ = nullptr;
    QPushButton *refresh_ = nullptr;
    QPointer<QLineEdit> code_;
    qint64 openedAtMs_ = 0;
};
