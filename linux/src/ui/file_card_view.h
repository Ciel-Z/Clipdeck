#pragma once
#include "../history/file_paste_controller.h"
#include <QScrollArea>

class QPainter;
class QListView;
QRect fileFeedbackRect(const QRect &content, const FilePasteFeedback &feedback);
void paintFileCard(QPainter &painter, const QRect &content, const QVector<ClipboardFileReference> &files,
                   bool compact, const FilePasteFeedback &feedback = {});

class FilePreviewWidget final : public QScrollArea {
public:
    explicit FilePreviewWidget(QVector<ClipboardFileReference> files, QWidget *parent = nullptr);
    void scrollBy(int direction);
private:
    QListView *list_ = nullptr;
};
