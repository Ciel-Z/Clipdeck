#include "file_card_view.h"
#include "deck_symbols.h"
#include "eliding_label.h"
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QLabel>
#include <QToolTip>
#include <QHelpEvent>
#include <QDateTime>
#include <QListView>
#include <QStyledItemDelegate>
#include <QStringListModel>
#include <QHBoxLayout>
#include <QTextLayout>

namespace {
const QColor fileBlue(125, 186, 255);
void text(QPainter &p, QRect area, QString value, int pixels, bool bold = false, int flags = Qt::AlignLeft | Qt::AlignVCenter) {
    QFont font = p.font(); font.setPixelSize(pixels); font.setBold(bold); p.setFont(font);
    p.drawText(area, flags, QFontMetrics(font).elidedText(value, Qt::ElideMiddle, area.width()));
}
QString displayPath(QString path) {
    const QString home = QDir::homePath();
    if (path == home || path.startsWith(home + QLatin1Char('/'))) path.replace(0, home.size(), QStringLiteral("~"));
    return path;
}
QStringList wrappedLines(const QString &value,const QFont &font,int width,int maximumLines) {
    QStringList result;
    QTextLayout layout(value,font);
    QTextOption option;option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);layout.setTextOption(option);
    layout.beginLayout();
    while(result.size()<maximumLines) {
        auto line=layout.createLine();if(!line.isValid()) break;line.setLineWidth(std::max(1,width));
        const int start=line.textStart();
        if(result.size()==maximumLines-1 && start+line.textLength()<value.size())
            result.append(QFontMetrics(font).elidedText(value.mid(start),Qt::ElideMiddle,width));
        else result.append(value.mid(start,line.textLength()).trimmed());
    }
    layout.endLayout();return result;
}
class LocationIcon final : public QWidget {
public:
    explicit LocationIcon(QWidget *parent):QWidget(parent) {setFixedSize(14,14);}
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);drawDeckSymbol(painter,rect(),DeckSymbol::Folder,QColor(155,155,160));
    }
};
class FileRow final : public QWidget {
public:
    FileRow(ClipboardFileReference file, bool common, bool single, QWidget *parent)
        : QWidget(parent), file_(std::move(file)), common_(common), single_(single) {
        setToolTip(file_.path);
        if (!single_) setFixedHeight(common_ ? 36 : 48);
        else setMinimumHeight(100);
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this); p.setRenderHint(QPainter::TextAntialiasing); p.setPen(QColor(238,238,239));
        if (single_) {
            paintFileCard(p, rect(), {file_}, false);
            return;
        }
        drawDeckSymbol(p, QRect(10, (height()-22)/2, 22, 22), fileSymbol(file_), fileBlue);
        const QString type = file_.isDirectory ? QStringLiteral("文件夹") : QFileInfo(file_.name).suffix().toUpper();
        const int badgeWidth = std::min(80, QFontMetrics(p.font()).horizontalAdvance(type) + 12);
        QRect name(42, common_ ? 0 : 3, std::max(0, width()-badgeWidth-60), common_ ? height() : 22);
        text(p, name, file_.name, 13, true);
        if (!common_) {
            p.setPen(QColor(155,155,160));
            text(p, QRect(42, 25, std::max(0,width()-52), 18), displayPath(QFileInfo(file_.path).path()), 11);
        }
        const QRect badge(width()-badgeWidth-8, (height()-22)/2, badgeWidth, 22);
        p.setPen(Qt::NoPen); p.setBrush(QColor(255,255,255,12)); p.drawRoundedRect(badge,4,4);
        p.setPen(QColor(155,155,160)); text(p,badge.adjusted(5,0,-5,0),type,10,false,Qt::AlignCenter);
    }
private:
    ClipboardFileReference file_; bool common_, single_;
};

class FileListDelegate final : public QStyledItemDelegate {
public:
    FileListDelegate(QVector<ClipboardFileReference> files, bool common, QObject *parent)
        : QStyledItemDelegate(parent), files_(std::move(files)), common_(common) {}
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override {
        return QSize(0,common_?40:52);
    }
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const auto &file=files_[index.row()];
        QPainter &p=*painter; p.save();p.setClipRect(option.rect);
        const QRect area=option.rect.adjusted(0,0,0,-4);
        if(index.row()%2==0) {p.setPen(Qt::NoPen);p.setBrush(QColor(255,255,255,9));p.drawRoundedRect(area,7,7);}
        drawDeckSymbol(p,QRect(area.x()+10,area.center().y()-11,22,22),fileSymbol(file),fileBlue);
        const QString type=file.isDirectory?QStringLiteral("文件夹"):QFileInfo(file.name).suffix().toUpper();
        QFont badgeFont=p.font();badgeFont.setPixelSize(10);
        const int badgeWidth=std::min(80,QFontMetrics(badgeFont).horizontalAdvance(type)+12);
        p.setPen(QColor(238,238,239));
        text(p,QRect(area.x()+42,area.y()+(common_?0:3),std::max(0,area.width()-badgeWidth-60),common_?area.height():22),file.name,13,true);
        if(!common_) {
            p.setPen(QColor(155,155,160));
            text(p,QRect(area.x()+42,area.y()+25,std::max(0,area.width()-52),18),displayPath(QFileInfo(file.path).path()),11);
        }
        const QRect badge(area.right()-badgeWidth-8,area.center().y()-11,badgeWidth,22);
        p.setPen(Qt::NoPen);p.setBrush(QColor(255,255,255,12));p.drawRoundedRect(badge,4,4);
        p.setPen(QColor(155,155,160));text(p,badge.adjusted(5,0,-5,0),type,10,false,Qt::AlignCenter);
        p.restore();
    }
private:
    QVector<ClipboardFileReference> files_;bool common_;
};
}

QRect fileFeedbackRect(const QRect &content, const FilePasteFeedback &feedback) {
    if (feedback.phase == FilePasteFeedback::Idle) return {};
    const int h = feedback.phase == FilePasteFeedback::Partial ? 126 : 74;
    return QRect(content.x()+4, std::max(content.y(),content.bottom()-h+1), content.width()-8, std::min(h,content.height()));
}

void paintFileCard(QPainter &p, const QRect &content, const QVector<ClipboardFileReference> &files,
                   bool compact, const FilePasteFeedback &feedback) {
    if (files.isEmpty()) return;
    p.save(); p.setClipRect(content, Qt::IntersectClip); p.setRenderHint(QPainter::TextAntialiasing);
    const QRect feedbackArea = fileFeedbackRect(content,feedback);
    QRect body = content;
    if (!feedbackArea.isEmpty()) body.setBottom(feedbackArea.top()-4);
    p.setPen(QColor(238,238,239));
    if (files.size() == 1) {
        const bool shortBody = body.height()<170;
        const bool tiny = body.height()<120;
        const int icon = tiny ? 24 : shortBody ? 34 : 56;
        const int gap = tiny ? 6 : shortBody ? 10 : 20;
        QFont nameFont=p.font();nameFont.setPixelSize(shortBody?14:18);nameFont.setWeight(QFont::DemiBold);
        QFont pathFont=p.font();pathFont.setPixelSize(compact?12:13);pathFont.setWeight(QFont::Normal);
        const auto names=wrappedLines(files.first().name,nameFont,std::max(1,body.width()-24),shortBody?1:2);
        const auto paths=wrappedLines(displayPath(files.first().path),pathFont,std::max(1,body.width()-24),shortBody?1:compact?2:4);
        const int nameHeight=names.size()*QFontMetrics(nameFont).height();
        const int pathHeight=paths.size()*QFontMetrics(pathFont).height();
        const int total = icon+gap+nameHeight+(body.height()>=100 ? gap+pathHeight : 0);
        const int y = body.y()+std::max(4,(body.height()-total)/2);
        drawDeckSymbol(p,QRect(body.center().x()-icon/2,y,icon,icon),fileSymbol(files.first()),fileBlue);
        p.setFont(nameFont);
        for(int index=0;index<names.size();++index)
            p.drawText(QRect(body.x()+12,y+icon+gap+index*QFontMetrics(nameFont).height(),body.width()-24,QFontMetrics(nameFont).height()),Qt::AlignCenter,names[index]);
        if (body.height()>=100) {
            p.setPen(QColor(155,155,160));
            p.setFont(pathFont);
            for(int index=0;index<paths.size();++index)
                p.drawText(QRect(body.x()+12,y+icon+gap+nameHeight+gap+index*QFontMetrics(pathFont).height(),body.width()-24,QFontMetrics(pathFont).height()),Qt::AlignCenter,paths[index]);
        }
    } else {
        const int available = std::max(0,body.height()-24);
        const int footer = files.size()*30-4<=available ? 0 : 22;
        const int count = std::min(files.size(),std::max(0,(available-footer+4)/30));
        for (int i=0;i<count;++i) {
            const auto &file=files[i]; const int y=body.y()+12+i*30;
            drawDeckSymbol(p,QRect(body.x()+12,y+3,20,20),fileSymbol(file),fileBlue);
            p.setPen(QColor(238,238,239));
            text(p,QRect(body.x()+40,y,std::max(0,body.width()-58-(feedback.phase==FilePasteFeedback::Partial?18:0)),26),file.name,13,true);
            if (feedback.phase==FilePasteFeedback::Partial) {
                bool ok=false;
                for (const auto &f:feedback.available) if (f.path==file.path) ok=true;
                drawDeckSymbol(p,QRect(body.right()-24,y+4,18,18),ok?DeckSymbol::Checkmark:DeckSymbol::Warning,
                    ok?QColor(52,199,89):QColor(255,176,138));
            }
        }
        if (count<files.size()) {
            p.setPen(QColor(155,155,160));
            text(p,QRect(body.x()+12,body.bottom()-30,body.width()/2-12,22),QStringLiteral("+%1 项").arg(files.size()-count),11);
            text(p,QRect(body.center().x(),body.bottom()-30,body.width()/2-12,22),QStringLiteral("空格预览全部"),11,false,Qt::AlignRight|Qt::AlignVCenter);
        }
    }
    if (!feedbackArea.isEmpty()) {
        p.setPen(Qt::NoPen); p.setBrush(files.size()==1 ? QColor(79,46,36) : QColor(23,26,31,250));
        p.drawRoundedRect(feedbackArea, files.size()==1?0:10,files.size()==1?0:10);
        p.setPen(QColor(255,176,138));
        const QString title = feedback.phase==FilePasteFeedback::Checking ? QStringLiteral("正在检查源文件…")
            : feedback.phase==FilePasteFeedback::Partial ? QStringLiteral("%1 个文件不可访问").arg(feedback.unavailable.size()) : feedback.message;
        if(feedback.phase==FilePasteFeedback::Checking) {
            p.setBrush(Qt::NoBrush);p.setPen(QPen(QColor(190,190,195),1.7,Qt::SolidLine,Qt::RoundCap));
            p.drawArc(QRect(feedbackArea.x()+12,feedbackArea.y()+14,14,14),int(QDateTime::currentMSecsSinceEpoch()%1000)*360/1000*16,250*16);
        } else drawDeckSymbol(p,QRect(feedbackArea.x()+12,feedbackArea.y()+13,16,16),DeckSymbol::WarningCircle,QColor(255,176,138));
        p.setPen(QColor(255,176,138));
        text(p,feedbackArea.adjusted(34,8,-12,-feedbackArea.height()+34),title,12,true);
        p.setPen(QColor(155,155,160));
        text(p,QRect(feedbackArea.x()+12,feedbackArea.y()+34,feedbackArea.width()-24,22),feedback.phase==FilePasteFeedback::Partial
            ? QStringLiteral("是否仅粘贴其余 %1 个文件？").arg(feedback.available.size()) : QStringLiteral("文件可能已移动或挂载已断开"),11);
        if (feedback.phase==FilePasteFeedback::Partial) {
            const QRect cancel(feedbackArea.x()+12,feedbackArea.bottom()-42,52,30);
            const QRect confirm(cancel.right()+8,cancel.y(),std::min(110,feedbackArea.width()-84),30);
            p.setPen(Qt::NoPen); p.setBrush(QColor(255,255,255,26)); p.drawRoundedRect(cancel,6,6);
            p.setBrush(QColor(10,122,255)); p.drawRoundedRect(confirm,6,6);
            p.setPen(Qt::white); text(p,cancel,QStringLiteral("取消"),11,true,Qt::AlignCenter);
            text(p,confirm,QStringLiteral("粘贴可用文件"),11,true,Qt::AlignCenter);
        }
    }
    p.restore();
}

FilePreviewWidget::FilePreviewWidget(QVector<ClipboardFileReference> files, QWidget *parent)
    : QScrollArea(parent) {
    setWidgetResizable(true); setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setStyleSheet(QStringLiteral(
        "QScrollArea,QWidget#filePreviewBody,QListView{background:#131315;border:0;color:#eee;outline:0;}"
        "QScrollBar:vertical{background:transparent;width:8px;margin:1px;}"
        "QScrollBar::handle:vertical{background:#646468;min-height:28px;border-radius:3px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}"));
    auto *body = new QWidget; body->setObjectName(QStringLiteral("filePreviewBody"));
    auto *layout = new QVBoxLayout(body); layout->setContentsMargins(10,8,10,8); layout->setSpacing(4);
    if (files.size()==1) layout->addWidget(new FileRow(files.first(),true,true,body),1);
    else if (!files.isEmpty()) {
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        QString common = QFileInfo(files.first().path).path();
        for (const auto &file:files) if (QFileInfo(file.path).path()!=common) common.clear();
        auto *directory = new ElidingLabel(common.isEmpty()?QStringLiteral("多个文件位置"):displayPath(common),body);
        directory->setToolTip(common); directory->setFixedHeight(28);
        directory->setStyleSheet(QStringLiteral("color:#9b9ba0;font-size:11px;"));
        auto *pathBar=new QWidget(body);auto *pathLayout=new QHBoxLayout(pathBar);
        pathLayout->setContentsMargins(10,0,10,0);pathLayout->setSpacing(7);
        pathLayout->addWidget(new LocationIcon(pathBar));pathLayout->addWidget(directory,1);
        layout->addWidget(pathBar);
        list_=new QListView(body);
        list_->setObjectName(QStringLiteral("filePreviewList"));
        list_->setUniformItemSizes(true);
        list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list_->setSelectionMode(QAbstractItemView::NoSelection);
        list_->setFocusPolicy(Qt::NoFocus);
        auto *model=new QStringListModel(list_);QStringList names;
        for(const auto &file:files) names.append(file.name);
        model->setStringList(names); list_->setModel(model);
        list_->setItemDelegate(new FileListDelegate(files,!common.isEmpty(),list_));
        list_->setMouseTracking(true);
        QObject::connect(list_,&QListView::entered,list_,[list=list_,files](const QModelIndex &index) {
            list->setToolTip(files[index.row()].path);
        });
        QObject::connect(list_,&QListView::viewportEntered,list_,[list=list_] { list->setToolTip(QString()); });
        layout->addWidget(list_,1);
    }
    setWidget(body);
}

void FilePreviewWidget::scrollBy(int direction) {
    auto *bar=list_?list_->verticalScrollBar():verticalScrollBar();
    bar->setValue(bar->value()+direction*40);
}
