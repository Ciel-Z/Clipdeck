#include "deck_item_model.h"

#include "code_highlight.h"
#include "deck_image_provider.h"
#include "deck_presentation.h"
#include "win_source_application.h"

#include <QCryptographicHash>
#include <QFileInfo>
#include <QSet>
#include <QVariantMap>

namespace {

QString sourceIconKey(const ClipboardItem &item) {
    return QStringLiteral("app/")
        + QString::fromLatin1(QCryptographicHash::hash(item.sourceID.toUtf8(), QCryptographicHash::Sha1).toHex());
}

QString imageUrl(const QString &key) { return QStringLiteral("image://deck/") + key; }

} // namespace

DeckItemModel::DeckItemModel(DeckImageProvider *images, QObject *parent)
    : QAbstractListModel(parent), images_(images) {}

int DeckItemModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : int(items_.size()); }

QHash<int, QByteArray> DeckItemModel::roleNames() const {
    return {
        {IdRole, "itemId"},
        {KindRole, "kind"},
        {TitleRole, "title"},
        {CategoryLabelRole, "categoryLabel"},
        {TimeRole, "time"},
        {TextRole, "text"},
        {CharacterCountRole, "characterCount"},
        {MonospacedRole, "monospaced"},
        {ImageUrlRole, "imageUrl"},
        {ImageSizeTextRole, "imageSizeText"},
        {ImageAngleRole, "imageAngle"},
        {FilesRole, "files"},
        {SourceIconUrlRole, "sourceIconUrl"},
        {SourceNameRole, "sourceName"},
        {LinkTitleRole, "linkTitle"},
        {LinkHostRole, "linkHost"},
        {LinkIconUrlRole, "linkIconUrl"},
        {LinkPreviewUrlRole, "linkPreviewUrl"},
        {ColorRole, "colorValue"},
        {ColorTextRole, "colorText"},
        {ImmersiveRole, "immersive"},
        {HighlightedTextRole, "highlightedText"},
        {FromOtherDeviceRole, "fromOtherDevice"},
    };
}

QVariant DeckItemModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= items_.size()) return QVariant();
    const ClipboardItem &item = items_.at(index.row());
    const QString id = item.id.toString(QUuid::WithoutBraces);
    switch (role) {
    case IdRole: return id;
    case FromOtherDeviceRole: return !item.originDevice.isEmpty() && item.originDevice != localDeviceId_;
    case KindRole: return DeckPresentation::cardKind(item);
    case TitleRole: return DeckPresentation::cardHeaderTitle(item);
    case CategoryLabelRole: return DeckPresentation::categoryLabel(item);
    case TimeRole: return DeckPresentation::relativeTime(item.copiedAt, now_);
    case TextRole: return item.text.left(3000);
    case CharacterCountRole: return qlonglong(item.textLength > 0 ? item.textLength : item.text.size());
    case MonospacedRole: return CodeHighlight::isCodeLanguage(item.previewLanguage);
    case HighlightedTextRole: {
        // Only text cards draw the body; colour and link cards have their own.
        if (!item.isText() || !CodeHighlight::isCodeLanguage(item.previewLanguage)) return QString();
        const QString kind = DeckPresentation::cardKind(item);
        if (kind != QLatin1String("text")) return QString();
        const QString body = item.text.left(3000);
        const QString key = id + QLatin1Char('/') + QString::number(qHash(body)) + item.previewLanguage;
        auto found = highlighted_.constFind(key);
        if (found != highlighted_.constEnd()) return found.value();
        if (highlighted_.size() >= 256) highlighted_.clear();
        const QString html = CodeHighlight::highlightedHtml(body, item.previewLanguage, QColor(255, 255, 255, 230));
        highlighted_.insert(key, html);
        return html;
    }
    case ImageUrlRole:
        return item.isImage() && images_->contains(QStringLiteral("thumb/") + id) ? imageUrl(QStringLiteral("thumb/") + id)
                                                                                 : QString();
    case ImageSizeTextRole: {
        // Swapped for quarter turns, like the mac footer.
        const QSize size = item.imagePixelSize;
        const bool quarter = (item.imageAngle / 90) % 2 != 0;
        return QStringLiteral("%1 × %2").arg(quarter ? size.height() : size.width()).arg(quarter ? size.width() : size.height());
    }
    case ImageAngleRole: return item.imageAngle;
    case FilesRole: {
        QVariantList files;
        for (const ClipboardFileReference &file : item.files) {
            files.append(QVariantMap{{QStringLiteral("name"), file.name},
                                     {QStringLiteral("path"), file.path},
                                     {QStringLiteral("isDirectory"), file.isDirectory}});
        }
        return files;
    }
    case SourceIconUrlRole: {
        const QString key = sourceIconKey(item);
        return images_->contains(key) ? imageUrl(key) : QString();
    }
    case SourceNameRole: return item.sourceName;
    case LinkTitleRole: return DeckPresentation::linkDisplayTitle(item);
    case LinkHostRole: return DeckPresentation::linkHost(item);
    case LinkIconUrlRole:
        return images_->contains(QStringLiteral("linkicon/") + id) ? imageUrl(QStringLiteral("linkicon/") + id) : QString();
    case LinkPreviewUrlRole:
        return images_->contains(QStringLiteral("linkpreview/") + id) ? imageUrl(QStringLiteral("linkpreview/") + id)
                                                                     : QString();
    case ColorRole: {
        const auto color = DeckPresentation::parseColor(item.text);
        return color ? QVariant::fromValue(*color) : QVariant();
    }
    case ColorTextRole: return item.text.trimmed();
    case ImmersiveRole: return DeckPresentation::usesImmersiveHeader(item);
    }
    return QVariant();
}

void DeckItemModel::registerImages(const ClipboardItem &item) {
    const QString id = item.id.toString(QUuid::WithoutBraces);
    if (item.isImage()) {
        if (!item.image.isNull()) images_->insert(QStringLiteral("thumb/") + id, item.image);
        else images_->insertEncoded(QStringLiteral("thumb/") + id, item.imageData);
    }
    // Icons depend only on the application; resolve each one once. The
    // executable is asked first because older builds stored 32 px icons.
    const QString iconKey = sourceIconKey(item);
    if (!images_->contains(iconKey)) {
        const QImage fresh = QFileInfo(item.sourceID).isFile() ? windowsApplicationIcon(item.sourceID) : QImage();
        if (!fresh.isNull()) images_->insert(iconKey, fresh);
        else if (!item.sourceIconData.isEmpty()) images_->insertEncoded(iconKey, item.sourceIconData);
        else if (!item.sourceIcon.isNull()) images_->insert(iconKey, item.sourceIcon);
    }
    images_->insertEncoded(QStringLiteral("linkicon/") + id, item.linkIconData);
    images_->insertEncoded(QStringLiteral("linkpreview/") + id, item.linkPreviewData);
}

void DeckItemModel::setItems(const QVector<ClipboardItem> &items) {
    now_ = QDateTime::currentDateTime();
    for (const ClipboardItem &item : items) registerImages(item);

    bool sameRows = items.size() == items_.size();
    for (int row = 0; sameRows && row < items.size(); ++row) sameRows = items.at(row).id == items_.at(row).id;
    if (sameRows) {
        items_ = items;
        if (!items_.isEmpty()) Q_EMIT dataChanged(index(0), index(int(items_.size()) - 1));
        return;
    }
    beginResetModel();
    items_ = items;
    endResetModel();
}

int DeckItemModel::indexOf(const QUuid &id) const {
    for (int row = 0; row < items_.size(); ++row) {
        if (items_.at(row).id == id) return row;
    }
    return -1;
}

void DeckItemModel::refreshTimes() {
    now_ = QDateTime::currentDateTime();
    if (!items_.isEmpty()) Q_EMIT dataChanged(index(0), index(int(items_.size()) - 1), {TimeRole});
}
