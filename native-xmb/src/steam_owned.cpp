#include "steam_owned.h"

#include "steam_library.h"
#include "api_keys.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QDir>
#include <QSet>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QFile>
#include <QTimer>
#include <QUrlQuery>

namespace xmb {
namespace {
constexpr int RequestTimeoutMs = 15000;
constexpr qint64 MaxResponseBytes = 8 * 1024 * 1024;
constexpr int MaxGames = 4096;

QString keyFromEnvironment() { return qEnvironmentVariable("STEAM_WEB_API_KEY").trimmed(); }

// Test seam: XMB_STEAM_API_BASE replaces the https://api.steampowered.com origin, but only with a plain-http
// loopback origin (127.0.0.1, localhost or ::1), so a test can serve canned responses and no real host can be
// substituted (the key header only ever reaches Valve or this machine). Unset or any other value uses the real API.
QString apiOrigin() {
    const QString real = QStringLiteral("https://api.steampowered.com");
    const QUrl url(qEnvironmentVariable("XMB_STEAM_API_BASE").trimmed());
    const QString host = url.host();
    const bool loopback = host == QLatin1String("127.0.0.1") || host == QLatin1String("localhost") || host == QLatin1String("::1");
    if (url.scheme() != QLatin1String("http") || !loopback || url.port(-1) <= 0) return real;
    return QStringLiteral("http://%1:%2").arg(host.contains(QLatin1Char(':')) ? QStringLiteral("[%1]").arg(host) : host).arg(url.port());
}

QString iconUrlFor(const QString& appId, const QString& hash) {
    if (hash.isEmpty()) return {};
    return QStringLiteral("https://media.steampowered.com/steamcommunity/public/images/apps/%1/%2.jpg")
        .arg(appId, hash);
}

QString headerUrlFor(const QString& appId) {
    if (appId.isEmpty()) return {};
    return QStringLiteral("https://cdn.cloudflare.steamstatic.com/steam/apps/%1/header.jpg").arg(appId);
}
// The response's game rows as the column's owned list (title order, installed flag from the local manifests).
OwnedGameList ownedGamesFromResponse(const QJsonObject& response) {
    QSet<QString> installedIds;
    for (const auto& game : scanSteam().games) installedIds.insert(game.appId);

    OwnedGameList games;
    const auto rows = response.value(QLatin1String("games")).toArray();
    games.reserve(std::min<int>(int(rows.size()), MaxGames));
    for (const auto& value : rows) {
        if (!value.isObject()) continue;
        const auto row = value.toObject();
        OwnedGame game;
        game.appId = QString::number(row.value(QLatin1String("appid")).toInt());
        if (game.appId.isEmpty() || game.appId == QLatin1String("0")) continue;
        game.title = row.value(QLatin1String("name")).toString().trimmed();
        if (game.title.isEmpty()) continue;
        game.iconUrl = iconUrlFor(game.appId, row.value(QLatin1String("img_icon_url")).toString());
        game.headerUrl = headerUrlFor(game.appId);
        game.playtimeMinutes = row.value(QLatin1String("playtime_forever")).toInt();
        game.installed = installedIds.contains(game.appId);
        games.push_back(game);
        if (games.size() >= MaxGames) break;
    }
    std::sort(games.begin(), games.end(), [](const OwnedGame& a, const OwnedGame& b) {
        return a.title.localeAwareCompare(b.title) < 0;
    });
    return games;
}

// Test seam (work/ab-layers.ts): XMB_STEAM_OWNED_FILE names a JSON file in the Web API's own response shape
// ({"response":{"games":[{"appid":..,"name":..}]}}). refresh() then answers from it, with no key and no network.
// The file is the caller's private fixture and is never part of the repository.
QString ownedFixturePath() { return qEnvironmentVariable("XMB_STEAM_OWNED_FILE").trimmed(); }
} // namespace

SteamOwned::SteamOwned(QObject* parent) : QObject(parent), network_(new QNetworkAccessManager(this)) {
    qRegisterMetaType<OwnedGameList>("xmb::OwnedGameList");
    qRegisterMetaType<OwnedGame>("xmb::OwnedGame");
}

SteamOwned::~SteamOwned() { cancel(); }

QString SteamOwned::apiKeyFilePath() {
    return apikeys::keyFilePath(apikeys::Id::SteamWeb);  // XMB_API_KEY_DIR (tests only) or the application data folder
}

QString SteamOwned::apiKey() {
    const QString fromEnv = keyFromEnvironment();
    if (!fromEnv.isEmpty()) return fromEnv;
    // A single-line user-only file keeps the key off the command line and out of Git.
    QFile file(apiKeyFilePath());
    if (!file.open(QIODevice::ReadOnly)) return {};
    if (file.size() > 4096) return {};
    const QString text = QString::fromUtf8(file.readAll()).trimmed();
    const int newline = text.indexOf(QRegularExpression(QStringLiteral("[\\r\\n]")));
    return (newline > 0 ? text.left(newline) : text).trimmed();
}

bool SteamOwned::hasApiKey() { return !apiKey().isEmpty() || !ownedFixturePath().isEmpty(); }

void SteamOwned::cancel() {
    ++generation_;
    if (reply_) {
        auto* reply = reply_;
        reply_ = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (busy_) {
        busy_ = false;
        emit busyChanged(false);
    }
}

void SteamOwned::refresh(QString steamId64) {
    cancel();
    steamId64 = steamId64.trimmed();
    if (steamId64.isEmpty()) {
        // Fall back to the account Steam recorded as active, then the sole valid one.
        steamId64 = scanSteam().steamId;
    }
    requestedId_ = steamId64;
    if (steamId64.isEmpty()) {
        emit loaded({}, tr("No Steam account was found. Set XMB_STEAM_ID or sign in to Steam."), false);
        return;
    }
    if (const QString fixture = ownedFixturePath(); !fixture.isEmpty()) {
        QFile file(fixture);
        const auto document = file.size() <= MaxResponseBytes && file.open(QIODevice::ReadOnly)
            ? QJsonDocument::fromJson(file.readAll()) : QJsonDocument();
        const quint64 generation = ++generation_;
        const auto games = ownedGamesFromResponse(document.object().value(QLatin1String("response")).toObject());
        QTimer::singleShot(0, this, [this, generation, games, ok = document.isObject()] {
            if (generation != generation_) return;
            emit loaded(games, ok ? QString() : tr("The owned-games fixture could not be read."), false);
        });
        return;
    }
    const QString key = apiKey();
    if (key.isEmpty()) {
        emit loaded({}, tr("Set STEAM_WEB_API_KEY to list the games this account owns."), true);
        return;
    }

    const quint64 generation = ++generation_;
    busy_ = true;
    emit busyChanged(true);

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("steamid"), steamId64);
    query.addQueryItem(QStringLiteral("include_appinfo"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("include_played_free_games"), QStringLiteral("1"));
    query.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    QNetworkRequest request{
        QUrl(apiOrigin() + QStringLiteral("/IPlayerService/GetOwnedGames/v1/?") +
             query.toString(QUrl::FullyEncoded))};
    // The key travels in a header (never in the URL, so no log or proxy records it); never follow a redirect to another host.
    request.setRawHeader("x-webapi-key", key.toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("xmb-native/1.0"));
    auto* reply = network_->get(request);
    reply_ = reply;

    auto* deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    deadline->start(RequestTimeoutMs);
    connect(reply, &QNetworkReply::downloadProgress, this, [this, reply, generation](qint64 received, qint64) {
        if (generation != generation_ || received <= MaxResponseBytes) return;
        // abort() runs the finished handler synchronously; detach first so the failure is reported exactly once.
        reply_ = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
        fail(generation, tr("The owned-games response was larger than expected."), false);
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        reply_ = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const auto networkError = reply->error();
        reply->deleteLater();
        if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
            // Valve answers 401/403 for a bad key and 400 for a malformed SteamID64.
            if (status == 401 || status == 403)
                fail(generation, tr("Steam rejected the API key or it is not allowed to read this profile."), false);
            else if (status == 400)
                fail(generation, tr("Steam rejected the account id. Set XMB_STEAM_ID to a SteamID64."), false);
            else
                fail(generation, tr("The Steam Web API could not be reached."), false);
            return;
        }
        if (body.size() > MaxResponseBytes) {
            fail(generation, tr("The owned-games response was larger than expected."), false);
            return;
        }
        QJsonParseError parseError{};
        const auto document = QJsonDocument::fromJson(body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            fail(generation, tr("The Steam Web API returned an unexpected response."), false);
            return;
        }
        const auto response = document.object().value(QLatin1String("response")).toObject();
        if (!response.contains(QLatin1String("games"))) {
            // Documented shape for a private or empty profile: response is {} or {game_count:0}.
            if (response.value(QLatin1String("game_count")).toInt(0) == 0) {
                if (generation != generation_) return;
                finish(generation);
                emit loaded({}, tr("This account owns no games, or its game details are private."), false);
                return;
            }
            fail(generation, tr("The Steam Web API returned no game list."), false);
            return;
        }

        const OwnedGameList games = ownedGamesFromResponse(response);
        if (generation != generation_) return;
        finish(generation);
        emit loaded(games, {}, false);
    });
}

void SteamOwned::finish(quint64 generation) {
    if (generation != generation_) return;
    if (busy_) {
        busy_ = false;
        emit busyChanged(false);
    }
}

void SteamOwned::fail(quint64 generation, const QString& message, bool needsKey) {
    if (generation != generation_) return;
    finish(generation);
    emit loaded({}, message, needsKey);
}

QString SteamOwned::installUrl(const QString& appId) {
    const QString trimmed = appId.trimmed();
    if (trimmed.isEmpty() || trimmed.toLongLong() <= 0) return {};
    return QStringLiteral("steam://install/") + trimmed;
}

QString SteamOwned::launchUrl(const QString& appId) {
    const QString trimmed = appId.trimmed();
    if (trimmed.isEmpty() || trimmed.toLongLong() <= 0) return {};
    return QStringLiteral("steam://rungameid/") + trimmed;
}

QString SteamOwned::storeUrl(const QString& appId) {
    const QString trimmed = appId.trimmed();
    if (trimmed.isEmpty() || trimmed.toLongLong() <= 0) return {};
    return QStringLiteral("https://store.steampowered.com/app/") + trimmed;
}

} // namespace xmb
