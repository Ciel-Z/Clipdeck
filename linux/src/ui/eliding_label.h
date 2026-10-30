#pragma once

#include <QLabel>

// Header text must stay inside its layout allocation, including with UOS
// font substitution and fractional scaling. Keep text() intact for tooltips.
class ElidingLabel final : public QLabel {
public:
    explicit ElidingLabel(const QString &text, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
};
