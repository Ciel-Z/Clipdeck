#include "link_metadata_fetcher.h"

#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTextDocumentFragment>
#include <QTimer>

#include <memory>

namespace {

constexpr qint64 kMaximumHtmlBytes = 512 * 1024;
constexpr qint64 kMaximumIconBytes = 512 * 1024;
constexpr qint64 kMaximumPreviewBytes = 8 * 1024 * 1024;
constexpr int kMaximumPreviewPixels = 1200;

QString decodeEntities(const QString &text) {
    return QTextDocumentFragment::fromHtml(text).toPlainText().simplified();
}

QString attribute(const QString &tag, const QString &name) {
    const QRegularExpression expression(
        QStringLiteral("\\b%1\\s*=\\s*(['\"])(.*?)\\1").arg(QRegularExpression::escape(name)),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = expression.match(tag);
    return match.hasMatch() ? decodeEntities(match.captured(2)) : QString();
}

QStringList tags(const QString &html, const QString &element) {
    const QRegularExpression expression(QStringLiteral("<%1\\b[^>]*>").arg(element),
                                        QRegularExpression::CaseInsensitiveOption);
    QStringList result;
    auto iterator = expression.globalMatch(html);
    while (iterator.hasNext()) result.append(iterator.next().captured(0));
    return result;
}

QString metaContent(const QString &html, const QStringList &keys) {
    for (const QString &tag : tags(html, QStringLiteral("meta"))) {
        const QString key = attribute(tag, QStringLiteral("property")).isEmpty()
            ? attribute(tag, QStringLiteral("name")) : attribute(tag, QStringLiteral("property"));
        for (const QString &wanted : keys) {
            if (key.compare(wanted, Qt::CaseInsensitive) == 0) {
                const QString content = attribute(tag, QStringLiteral("content"));
                if (!content.isEmpty()) return content;
            }
        }
    }
    return QString();
}

bool isWeb(const QUrl &url) {
    return url.isValid() && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"));
}

} // namespace

LinkMetadataFetcher::LinkMetadataFetcher(QObject *parent) : QObject(parent) {}

QString LinkMetadataFetcher::pageTitle(const QString &html) {
    static const QRegularExpression expression(
        QStringLiteral("<title\\b[^>]*>(.*?)</title\\s*>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    const QRegularExpressionMatch match = expression.match(html);
    QString title = match.hasMatch() ? decodeEntities(match.captured(1)) : QString();
    if (title.isEmpty()) title = decodeEntities(metaContent(html, {QStringLiteral("og:title"), QStringLiteral("twitter:title")}));
    return title.left(300);
}

QList<QUrl> LinkMetadataFetcher::iconUrls(const QString &html, const QUrl &base) {
    // Larger, sharper icons first, as on mac.
    const QStringList relations{QStringLiteral("apple-touch-icon"), QStringLiteral("icon"),
                                QStringLiteral("shortcut icon")};
    QList<QUrl> result;
    const QStringList links = tags(html, QStringLiteral("link"));
    for (const QString &relation : relations) {
        for (const QString &tag : links) {
            const QStringList rel = attribute(tag, QStringLiteral("rel")).toLower().split(QLatin1Char(' '), Qt::SkipEmptyParts);
            const bool matches = relation.contains(QLatin1Char(' ')) ? rel.join(QLatin1Char(' ')) == relation
                                                                     : rel.contains(relation);
            if (!matches) continue;
            const QUrl url = base.resolved(QUrl(attribute(tag, QStringLiteral("href"))));
            if (isWeb(url) && !result.contains(url)) result.append(url);
        }
    }
    for (const QString &path : {QStringLiteral("/apple-touch-icon.png"), QStringLiteral("/favicon.ico")}) {
        const QUrl url = base.resolved(QUrl(path));
        if (isWeb(url) && !result.contains(url)) result.append(url);
    }
    return result;
}

QUrl LinkMetadataFetcher::previewImageUrl(const QString &html, const QUrl &base) {
    const QString value = metaContent(html, {QStringLiteral("og:image"), QStringLiteral("og:image:url"),
                                             QStringLiteral("twitter:image")});
    if (value.isEmpty()) return QUrl();
    const QUrl url = base.resolved(QUrl(value));
    return isWeb(url) ? url : QUrl();
}

void LinkMetadataFetcher::download(const QUrl &url, const QByteArray &accept, qint64 maximumBytes, int timeoutMs,
                                   std::function<void(QByteArray, QUrl)> completion) {
    if (!isWeb(url)) {
        completion(QByteArray(), url);
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) ClipDeck/0.3");
    request.setRawHeader("Accept", accept);
    request.setTransferTimeout(timeoutMs);
    QNetworkReply *reply = network_.get(request);
    // A page keeps its first maximumBytes (the title and meta tags are in the
    // head); an image over the limit is dropped. Either way the rest is not read.
    const bool keepPrefix = accept.startsWith("text/html");
    auto received = std::make_shared<QByteArray>();
    auto truncated = std::make_shared<bool>(false);
    connect(reply, &QNetworkReply::readyRead, reply, [reply, received, truncated, maximumBytes] {
        received->append(reply->readAll());
        if (received->size() > maximumBytes) {
            *truncated = true;
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this,
            [reply, received, truncated, maximumBytes, keepPrefix, url, completion = std::move(completion)] {
        if (reply->error() == QNetworkReply::NoError) received->append(reply->readAll());
        QByteArray data;
        if (*truncated) data = keepPrefix ? received->left(maximumBytes) : QByteArray();
        else if (reply->error() == QNetworkReply::NoError) data = *received;
        const QUrl finalUrl = reply->url().isValid() ? reply->url() : url;
        reply->deleteLater();
        completion(data, finalUrl);
    });
}

void LinkMetadataFetcher::fetchFirstIcon(QList<QUrl> candidates, std::function<void(QByteArray)> completion) {
    if (candidates.isEmpty()) {
        completion(QByteArray());
        return;
    }
    const QUrl next = candidates.takeFirst();
    download(next, "image/*", kMaximumIconBytes, 6000,
             [this, candidates, completion = std::move(completion)](QByteArray data, QUrl) mutable {
        QImage image;
        if (!data.isEmpty() && image.loadFromData(data) && image.width() >= 8) {
            completion(data);
            return;
        }
        fetchFirstIcon(candidates, std::move(completion));
    });
}

void LinkMetadataFetcher::fetchPreview(const QUrl &url, std::function<void(QByteArray)> completion) {
    download(url, "image/*", kMaximumPreviewBytes, 8000, [completion = std::move(completion)](QByteArray data, QUrl) {
        QImage image;
        if (data.isEmpty() || !image.loadFromData(data)) {
            completion(QByteArray());
            return;
        }
        if (image.width() <= kMaximumPreviewPixels && image.height() <= kMaximumPreviewPixels) {
            completion(data);
            return;
        }
        // Re-encode a downscaled copy so the database does not keep a huge image.
        const QImage scaled = image.scaled(kMaximumPreviewPixels, kMaximumPreviewPixels, Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
        QByteArray encoded;
        QBuffer buffer(&encoded);
        buffer.open(QIODevice::WriteOnly);
        scaled.save(&buffer, scaled.hasAlphaChannel() ? "PNG" : "JPG", 85);
        completion(encoded);
    });
}

void LinkMetadataFetcher::fetch(const QUuid &itemId, const QUrl &url, Completion completion) {
    if (!isWeb(url) || requested_.contains(itemId)) return;
    requested_.insert(itemId);
    waiting_.append({url, std::move(completion)});
    startWaiting();
}

void LinkMetadataFetcher::startWaiting() {
    while (running_ < kMaximumConcurrentFetches && !waiting_.isEmpty()) {
        Waiting next = waiting_.takeFirst();
        ++running_;
        start(next.url, [this, completion = std::move(next.completion)](const Metadata &metadata) {
            --running_;
            completion(metadata);
            startWaiting();
        });
    }
}

void LinkMetadataFetcher::start(const QUrl &url, Completion completion) {
    download(url, "text/html,application/xhtml+xml", kMaximumHtmlBytes, 8000,
             [this, url, completion = std::move(completion)](QByteArray page, QUrl finalUrl) {
        struct State {
            Metadata metadata;
            int remaining = 2;
        };
        auto state = std::make_shared<State>();
        auto finish = [state, completion] {
            if (--state->remaining != 0) return;
            completion(state->metadata);
        };
        const QString html = QString::fromUtf8(page);
        state->metadata.title = pageTitle(html);
        fetchFirstIcon(iconUrls(html, finalUrl.isValid() ? finalUrl : url), [state, finish](QByteArray data) {
            state->metadata.iconData = data;
            finish();
        });
        fetchPreview(previewImageUrl(html, finalUrl), [state, finish](QByteArray data) {
            state->metadata.previewData = data;
            finish();
        });
    });
}
