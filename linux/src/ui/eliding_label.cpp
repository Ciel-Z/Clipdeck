#include "eliding_label.h"

#include <QPainter>
#include <QStyle>
#include <QStyleOption>

ElidingLabel::ElidingLabel(const QString &text, QWidget *parent)
    : QLabel(text, parent) {
    setTextFormat(Qt::PlainText);
    setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setToolTip(text);
}

QSize ElidingLabel::sizeHint() const {
    ensurePolished();
    return QSize(fontMetrics().horizontalAdvance(text()) + 4 + margin() * 2,
                 fontMetrics().height() + margin() * 2);
}

QSize ElidingLabel::minimumSizeHint() const {
    return QSize(0, sizeHint().height());
}

void ElidingLabel::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    QStyleOption option;
    option.initFrom(this);
    style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);
    const QRect area = contentsRect().adjusted(margin(), margin(), -margin(), -margin());
    if (area.width() <= 0 || area.height() <= 0) return;
    painter.setClipRect(area);
    painter.setFont(font());
    painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled,
                                   QPalette::WindowText));
    painter.drawText(area, alignment() | Qt::TextSingleLine,
                     fontMetrics().elidedText(text(), Qt::ElideRight, area.width()));
}
