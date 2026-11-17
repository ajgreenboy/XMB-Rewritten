#include "sabnzbd_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <algorithm>
#include <utility>

namespace xmb {
namespace {

constexpr int requestTimeoutMs = 8000;
constexpr qsizetype maxResponseBytes = 2 * 1024 * 1024;

QByteArray encodeIds(const QStringList& ids) {
    return QUrl::toPercentEncoding(ids.join(u','));
}

} // namespace

SabnzbdClient::SabnzbdClient(QObject* parent)
    : QObject(parent), network_(new QNetworkAccessManager(this)) {
    network_->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
}

QUrl SabnzbdClient::normalizeServer(const QString& server) {
    const QUrl url(server.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()
        || url.hasQuery() || url.hasFragment()
        || (url.scheme() != u"http" && url.scheme() != u"https")) return {};
    return url;
}

bool SabnzbdClient::isJobId(const QString& id) {
    static const QRegularExpression pattern(QStringLiteral("\\ASABnzbd_nzo_[A-Za-z0-9_]{1,128}\\z"));
    return pattern.match(id).hasMatch();
}

void SabnzbdClient::configure(QUrl base, QString apiKey) {
    if (!base.isValid() || base.host().isEmpty() || !base.userInfo().isEmpty()
        || base.hasQuery() || base.hasFragment()
        || (base.scheme() != u"http" && base.scheme() != u"https")) {
        base = QUrl();
    }
    if (base == base_ && apiKey == apiKey_) return;
    ++generation_;
    base_ = std::move(base);
    apiKey_ = std::move(apiKey);
    const auto replies = network_->findChildren<QNetworkReply*>();
    for (QNetworkReply* reply : replies) reply->abort();
}

void SabnzbdClient::status(QStringList ids, StatusResult callback) {
    if (ids.isEmpty() || !std::all_of(ids.cbegin(), ids.cend(), isJobId)) {
        QTimer::singleShot(0, this, [callback = std::move(callback)] {
            callback({}, tr("Download controls require exact SABnzbd job IDs."));
        });
        return;
    }
    ids.removeDuplicates();
    const QSet<QString> requested(ids.cbegin(), ids.cend());
    send("nzo_ids=" + encodeIds(ids), [requested, callback = std::move(callback)](QJsonObject data, QString error) {
        if (!error.isEmpty()) {
            callback({}, std::move(error));
            return;
        }

        const QJsonValue slotsValue = data.value(QStringLiteral("queue")).toObject().value(QStringLiteral("slots"));
        if (!slotsValue.isArray()) {
            callback({}, tr("SABnzbd returned an invalid download queue."));
            return;
        }

        static const QSet<QString> runningStates = {
            QStringLiteral("Downloading"), QStringLiteral("Queued"), QStringLiteral("Propagating"),
            QStringLiteral("Fetching"), QStringLiteral("Grabbing")
        };
        QSet<QString> seen;
        QHash<QString, bool> pausedById;
        for (const QJsonValue& item : slotsValue.toArray()) {
            const QJsonObject slot = item.toObject();
            const QString id = slot.value(QStringLiteral("nzo_id")).toString();
            if (!requested.contains(id)) continue;
            const QString state = slot.value(QStringLiteral("status")).toString();
            if (seen.contains(id) || state.isEmpty()) {
                callback({}, tr("SABnzbd returned duplicate or incomplete download entries."));
                return;
            }
            seen.insert(id);
            if (state == u"Paused") {
                pausedById.insert(id, true);
            } else if (runningStates.contains(state)) {
                pausedById.insert(id, false);
            }
        }
        callback(std::move(pausedById), {});
    });
}

void SabnzbdClient::setPaused(QStringList ids, bool paused, Result callback) {
    change(std::move(ids), paused ? "pause" : "resume", paused, std::move(callback));
}

void SabnzbdClient::cancel(QStringList ids, Result callback) {
    change(std::move(ids), "delete", std::nullopt, std::move(callback));
}

void SabnzbdClient::change(QStringList ids, const QByteArray& action, std::optional<bool> paused, Result callback) {
    ids.removeDuplicates();
    status(ids, [this, ids, action, paused, callback = std::move(callback)]
                (QHash<QString, bool> pausedById, QString error) {
        if (!error.isEmpty()) {
            callback(std::move(error));
            return;
        }
        if (pausedById.size() != ids.size()) {
            callback(tr("At least one matching SABnzbd download is no longer in the active queue."));
            return;
        }

        QStringList targets;
        for (const QString& id : ids) {
            if (!paused.has_value() || pausedById.value(id) != *paused) targets.append(id);
        }
        if (targets.isEmpty()) {
            callback({});
            return;
        }

        QByteArray parameters = "name=" + action + "&value=" + encodeIds(targets);
        if (action == "delete") parameters += "&del_files=0";
        const QSet<QString> expected(targets.cbegin(), targets.cend());
        send(parameters, [expected, callback](QJsonObject data, QString error) {
            if (error.isEmpty()) {
                const QJsonArray changed = data.value(QStringLiteral("nzo_ids")).toArray();
                QSet<QString> confirmed;
                for (const QJsonValue& id : changed) confirmed.insert(id.toString());
                if (!data.value(QStringLiteral("status")).toBool()
                    || confirmed != expected || confirmed.size() != changed.size()) {
                    error = tr("SABnzbd did not confirm the change for every selected download.");
                }
            }
            callback(std::move(error));
        });
    });
}

void SabnzbdClient::send(const QByteArray& parameters, ApiResult callback) {
    if (base_.isEmpty() || apiKey_.isEmpty()) {
        QTimer::singleShot(0, this, [callback = std::move(callback)] {
            callback({}, tr("Set the SABnzbd URL and API key first."));
        });
        return;
    }

    QUrl url = base_;
    QString prefix = url.path(QUrl::FullyEncoded);
    if (!prefix.endsWith(u'/')) prefix.append(u'/');
    url.setPath(prefix + QStringLiteral("api"), QUrl::TolerantMode);

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    const QByteArray form = "mode=queue&output=json&apikey=" + QUrl::toPercentEncoding(apiKey_)
                          + '&' + parameters;
    QNetworkReply* reply = network_->post(request, form);
    reply->setReadBufferSize(maxResponseBytes);
    QTimer::singleShot(requestTimeoutMs, reply, [reply] {
        if (reply->isFinished()) return;
        reply->setProperty("sabTimeout", true);
        reply->abort();
    });
    connect(reply, &QNetworkReply::readyRead, reply, [reply] {
        if (reply->bytesAvailable() < maxResponseBytes) return;
        reply->setProperty("sabOversizedBody", true);
        reply->abort();
    });

    const quint64 generation = generation_;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation, callback = std::move(callback)] {
        reply->deleteLater();
        if (generation != generation_) return;

        QString error;
        QJsonObject payload;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->property("sabTimeout").toBool()) {
            error = tr("The SABnzbd request timed out.");
        } else if (reply->property("sabOversizedBody").toBool()) {
            error = tr("The SABnzbd response was too large.");
        } else if (status == 401 || status == 403) {
            error = tr("SABnzbd denied access. Check the full API key and the server's access settings.");
        } else if (status && status != 200) {
            error = tr("SABnzbd returned HTTP %1.").arg(status);
        } else if (reply->error() != QNetworkReply::NoError || !status) {
            error = tr("Could not reach the SABnzbd server.");
        } else {
            const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
            if (!document.isObject()) {
                error = tr("SABnzbd returned an invalid response.");
            } else {
                payload = document.object();
                if (payload.contains(QStringLiteral("error"))) {
                    error = tr("SABnzbd rejected the request. Check the API key and queue state.");
                }
            }
        }
        callback(std::move(payload), std::move(error));
    });
}

} // namespace xmb
