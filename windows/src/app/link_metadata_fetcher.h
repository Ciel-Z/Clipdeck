#pragma once

#include <QByteArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QUuid>

#include <functional>

// LinkMetadataService (mac ClipboardMonitor.swift): fetches a copied web
// link's page title, site icon and og:image preview so the link card can show
// them. Icon candidates are tried in order (declared icons, then the site's
// /favicon.ico and /apple-touch-icon.png); the preview is scaled to at most
// 1200 px. Any part that cannot be fetched comes back empty.
class LinkMetadataFetcher final : public QObject {
    Q_OBJECT
public:
    struct Metadata {
        QString title;
        QByteArray iconData;
        QByteArray previewData;
    };
    using Completion = std::function<void(const Metadata &)>;

    explicit LinkMetadataFetcher(QObject *parent = nullptr);

    // The link queue (LinkMetadataQueue in mac ClipboardMonitor.swift): fresh
    // copies, history loaded at start and cards synced from another device
    // are all just enqueued. Each item is fetched at most once per run, a few
    // at a time, in the order they were enqueued.
    static constexpr int kMaximumConcurrentFetches = 3;
    void fetch(const QUuid &itemId, const QUrl &url, Completion completion);

    // HTML parsing, exposed for tests.
    static QString pageTitle(const QString &html);
    static QList<QUrl> iconUrls(const QString &html, const QUrl &base);
    static QUrl previewImageUrl(const QString &html, const QUrl &base);

private:
    void download(const QUrl &url, const QByteArray &accept, qint64 maximumBytes, int timeoutMs,
                  std::function<void(QByteArray, QUrl)> completion);
    void fetchFirstIcon(QList<QUrl> candidates, std::function<void(QByteArray)> completion);
    void fetchPreview(const QUrl &url, std::function<void(QByteArray)> completion);
    void startWaiting();
    void start(const QUrl &url, Completion completion);

    struct Waiting {
        QUrl url;
        Completion completion;
    };

    QNetworkAccessManager network_;
    QSet<QUuid> requested_;
    QList<Waiting> waiting_;
    int running_ = 0;
};
