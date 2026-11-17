#include "seerr_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QUrlQuery>
#include <algorithm>

namespace xmb {
namespace {
constexpr int TimeoutMs = 12000;
constexpr qint64 MaxResponseBytes = 2 * 1024 * 1024;

QString cleanType(const QString& type) {
    const QString value = type.trimmed().toLower();
    return value == QLatin1String("movie") || value == QLatin1String("tv") ? value : QString();
}

class NoCookies final : public QNetworkCookieJar {
public:
    QList<QNetworkCookie> cookiesForUrl(const QUrl&) const override { return {}; }
    bool setCookiesFromUrl(const QList<QNetworkCookie>&, const QUrl&) override { return false; }
};
}

SeerrClient::SeerrClient(QObject* parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)) {
    network_->setCookieJar(new NoCookies);
}

QUrl SeerrClient::normalizeServer(QString server) {
    server = server.trimmed();
    if (server.isEmpty()) return {};
    if (!server.contains(QStringLiteral("://"))) server.prepend(QStringLiteral("http://"));
    QUrl url(server, QUrl::StrictMode);
    if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))
        || url.host().isEmpty() || url.authority().contains(QLatin1Char('@')) || url.hasQuery() || url.hasFragment()) return {};
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    if (!path.endsWith(QLatin1String("/api/v1"))) path += QStringLiteral("/api/v1");
    url.setPath(path);
    return url;
}

void SeerrClient::configure(QUrl base, QString apiKey) {
    ++generation_;
    const auto old = pending_;
    pending_.clear();
    for (QNetworkReply* reply : old) if (reply) reply->abort();
    base_ = normalizeServer(base.toString());
    apiKey_ = apiKey.trimmed();
    if (apiKey_.size() > 512 || std::any_of(apiKey_.cbegin(), apiKey_.cend(),
        [](QChar ch) { return ch.unicode() < 0x21 || ch.unicode() > 0x7e; })) apiKey_.clear();
}

void SeerrClient::search(QString query, int page, Result cb) {
    query = query.trimmed();
    if (query.isEmpty() || query.size() > 300 || page < 1 || page > 10000) {
        if (cb) cb({}, QStringLiteral("Enter a search term and a valid page."));
        return;
    }
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("query"), QString::fromLatin1(QUrl::toPercentEncoding(query)));
    params.addQueryItem(QStringLiteral("page"), QString::number(page));
    get(QStringLiteral("search"), params, std::move(cb));
}

void SeerrClient::details(QString type, int tmdbId, Result cb) {
    type = cleanType(type);
    if (type.isEmpty() || tmdbId <= 0) {
        if (cb) cb({}, QStringLiteral("Choose a movie or TV item with a valid TMDB ID."));
        return;
    }
    get(type + QLatin1Char('/') + QString::number(tmdbId), {}, std::move(cb));
}

void SeerrClient::requests(int skip, Result cb) {
    if (skip < 0 || skip > 1000000) {
        if (cb) cb({}, QStringLiteral("Invalid request list offset."));
        return;
    }
    QUrlQuery params; params.addQueryItem(QStringLiteral("take"), QStringLiteral("20")); params.addQueryItem(QStringLiteral("skip"), QString::number(skip));
    get(QStringLiteral("request"), params, std::move(cb));
}

void SeerrClient::submit(QString type, int tmdbId, QList<int> seasons, Result cb) {
    type = cleanType(type);
    if (type.isEmpty() || tmdbId <= 0 || seasons.size() > 100 ||
        (type == QLatin1String("tv") && seasons.isEmpty())) {
        if (cb) cb({}, QStringLiteral("Choose a valid movie or TV item."));
        return;
    }
    QSet<int> unique;
    QJsonArray seasonArray;
    for (int season : seasons) {
        if (season < 0 || season > 1000 || unique.contains(season)) {
            if (cb) cb({}, QStringLiteral("The selected seasons are invalid."));
            return;
        }
        unique.insert(season); seasonArray.append(season);
    }
    QJsonObject body{{QStringLiteral("mediaType"), type}, {QStringLiteral("mediaId"), tmdbId}};
    if (type == QLatin1String("tv")) body.insert(QStringLiteral("seasons"), seasonArray);
    post(QStringLiteral("request"), body, std::move(cb));
}

void SeerrClient::check(Result cb) { get(QStringLiteral("auth/me"), {}, std::move(cb)); }

void SeerrClient::get(const QString& endpoint, const QUrlQuery& query, Result cb) {
    send(endpoint, query, {}, false, std::move(cb));
}

void SeerrClient::post(const QString& endpoint, const QJsonObject& body, Result cb) {
    send(endpoint, {}, QJsonDocument(body).toJson(QJsonDocument::Compact), true, std::move(cb));
}

void SeerrClient::send(const QString& endpoint, const QUrlQuery& query, const QByteArray& body, bool isPost, Result cb) {
    if (base_.isEmpty() || apiKey_.isEmpty()) {
        if (cb) cb({}, QStringLiteral("Configure the Seerr server and API key first."));
        return;
    }
    QUrl url = base_;
    QString path = url.path();
    if (!path.endsWith(QLatin1Char('/'))) path += QLatin1Char('/');
    path += endpoint;
    url.setPath(path);
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setRawHeader(QByteArrayLiteral("X-Api-Key"), apiKey_.toUtf8());
    request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json"));
    request.setTransferTimeout(TimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    if (isPost) request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const quint64 requestGeneration = generation_;
    QNetworkReply* reply = isPost ? network_->post(request, body) : network_->get(request);
    reply->setReadBufferSize(MaxResponseBytes + 1);
    pending_.append(reply);
    connect(reply, &QNetworkReply::readyRead, this, [reply]() {
        if (reply->bytesAvailable() > MaxResponseBytes) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, requestGeneration, cb = std::move(cb)]() mutable {
        pending_.removeAll(reply);
        const QByteArray bytes = reply->read(MaxResponseBytes + 1);
        const auto status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto networkError = reply->error();
        reply->deleteLater();
        if (requestGeneration != generation_) return;
        if (bytes.size() > MaxResponseBytes) { if (cb) cb({}, QStringLiteral("Seerr response was too large.")); return; }
        if (status != 0 && (status < 200 || status >= 300)) {
            QString error = QStringLiteral("Seerr returned HTTP %1.").arg(status);
            if (status == 401 || status == 403) error = QStringLiteral("Seerr rejected the API key or access.");
            else if (status == 409) error = QStringLiteral("This request already exists or conflicts with another request.");
            else if (status >= 300 && status < 400) error = QStringLiteral("Use your Seerr server’s final address. Redirects are blocked.");
            if (cb) cb({}, error);
            return;
        }
        if (networkError != QNetworkReply::NoError || status == 0) {
            const QString safe = networkError == QNetworkReply::OperationCanceledError && status == 0
                ? QStringLiteral("Seerr request timed out or was interrupted.")
                : QStringLiteral("Could not reach the Seerr server.");
            if (cb) cb({}, safe);
            return;
        }
        QJsonParseError parse;
        const QJsonDocument document = QJsonDocument::fromJson(bytes, &parse);
        if (parse.error != QJsonParseError::NoError || !document.isObject()) {
            if (cb) cb({}, QStringLiteral("Seerr returned an invalid JSON response."));
            return;
        }
        if (cb) cb(document.object(), {});
    });
}

}
