#pragma once

// Key-free artwork for imported games: the title is looked up once in Valve's public store search, matched
// conservatively (imported_art_match.h) and the chosen app's public CDN art (header.jpg for the row,
// library_hero.jpg for the backdrop) is cached under <data>/cache/imported-art/. After that it works offline.

#include <QHash>
#include <functional>
#include <QImage>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QVector>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace xmb {

class ImportedArt final : public QObject {
    Q_OBJECT
public:
    // cacheRoot is <data>/cache; files go to <cacheRoot>/imported-art/.
    explicit ImportedArt(QString cacheRoot, QObject* parent = nullptr, QNetworkAccessManager* network = nullptr);
    ~ImportedArt() override;

    // Targets are (row id, title). Supersedes earlier work. Cached matches are delivered from disk; only titles
    // with no cache entry (or an expired no-match entry) use the network, one title at a time.
    void refresh(const QVector<QPair<QString, QString>>& targets);
    // The Fetch Artwork action: forget back-off and expired/unmatched decisions so matching runs again.
    void resetBackoff();
    // False while XMB_STEAM_ARTWORK=0 or XMB_IMPORTED_ART=0 (the gate and tests keep the network out).
    static bool networkEnabled();

signals:
    // preview is the header scaled to at most 512x288; headerPath/heroPath are the cached files (hero may be empty).
    void ready(QString id, QString title, QString headerPath, QString heroPath, QImage preview);
    void heroReady(QString id, QString heroPath);
    void status(QString message);

private:
    using Target = QPair<QString, QString>;
    void pump();
    void startNetwork(const Target& target);
    void search(const Target& target);
    // url empty = the predictable header.jpg; a 404 there falls back to the app's own header_image from appdetails
    // (new releases keep it under a hashed path), once.
    void downloadHeader(const Target& target, const QString& appId, const QUrl& url = {}, bool viaDetails = false);
    void headerFromDetails(const Target& target, const QString& appId);
    void headerUnavailable(const Target& target, const QString& appId);
    void downloadHero(const Target& target, const QString& appId);
    void finishTarget(bool failed);
    void get(const QUrl& url, bool image, std::function<void(QByteArray, int, int)> done, int redirects = 0);
    void deliver(const Target& target, const QString& headerPath, const QString& heroPath);
    void report(const QString& message);

    QString cacheRoot_;
    QNetworkAccessManager* network_ = nullptr;
    QTimer* timer_ = nullptr;
    QVector<Target> pending_;
    QVector<Target> needsNetwork_;
    QSet<Target> delivered_;
    QSet<QString> heroTried_;
    QHash<Target, qint64> retryAfter_;
    Target active_;
    QNetworkReply* reply_ = nullptr;
    quint64 generation_ = 0;
    qint64 networkRetryAfter_ = 0;
    int lookupsThisRun_ = 0;
    bool force_ = false;
    QString lastStatus_;
};

} // namespace xmb
