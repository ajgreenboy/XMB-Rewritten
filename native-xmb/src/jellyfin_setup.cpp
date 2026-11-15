#include "jellyfin_setup.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkInterface>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUdpSocket>
#include <algorithm>

namespace xmb::jellyfinSetup {
namespace {
constexpr int MaxAddressChars = 2048;
constexpr qint64 MaxInfoBytes = 64 * 1024;   // the public-information answer is a few hundred bytes
constexpr int MaxDatagramBytes = 4096;

// Text from the network, shown on the screen: control characters out, whitespace collapsed, bounded.
QString clean(const QString& text, int limit) {
    QString out;
    out.reserve(std::min<int>(text.size(), limit));
    bool space = false;
    for (const QChar ch : text) {
        if (ch.isSpace() || ch.category() == QChar::Other_Control || ch.category() == QChar::Other_Format) { space = !out.isEmpty(); continue; }
        if (space) { out += QLatin1Char(' '); space = false; }
        out += ch;
        if (out.size() >= limit) break;
    }
    return out.trimmed();
}
}

// ---- the address -------------------------------------------------------------------------------------------
ServerAddress parseAddress(const QString& typed) {
    ServerAddress result;
    QString text = typed.trimmed();
    if (text.isEmpty()) { result.error = QObject::tr("Type the address of your Jellyfin server."); return result; }
    if (text.size() > MaxAddressChars) { result.error = QObject::tr("That address is too long."); return result; }
    for (const QChar ch : text)
        if (ch.isSpace() || ch.category() == QChar::Other_Control) { result.error = QObject::tr("An address has no spaces in it."); return result; }
    static const QRegularExpression scheme(QStringLiteral("^([A-Za-z][A-Za-z0-9+.-]*)://"));
    const auto match = scheme.match(text);
    if (match.hasMatch()) {
        const QString name = match.captured(1).toLower();
        if (name != QLatin1String("http") && name != QLatin1String("https")) {
            result.error = QObject::tr("Use an address that starts with http:// or https://.");
            return result;
        }
    } else {
        text = QStringLiteral("http://") + text;   // "192.168.1.20:8096" is what people type
    }
    QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty()) { result.error = QObject::tr("That is not a server address. An example is http://192.168.1.20:8096"); return result; }
    if (!url.userInfo().isEmpty() || url.authority().contains(QLatin1Char('@'))) {
        result.error = QObject::tr("Leave the user name and password out of the address. You sign in after it is added.");
        return result;
    }
    if (url.hasQuery() || url.hasFragment()) { result.error = QObject::tr("Leave off everything after a ? or a #."); return result; }
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    url.setPath(path);
    const QString normalised = url.toString(QUrl::FullyEncoded);
    // The registry's own rule is the final word, so a saved address can never be one the importer refuses.
    if (!ContentRegistry::isValidServerUrl(normalised, true)) {
        result.error = QObject::tr("That is not a server address. An example is http://192.168.1.20:8096");
        return result;
    }
    result.ok = true;
    result.url = url;
    result.text = normalised;
    return result;
}

QString withoutCredentials(const QString& typed) {
    const QString text = typed.trimmed();
    const int schemeEnd = text.indexOf(QLatin1String("://"));
    const int start = schemeEnd >= 0 ? schemeEnd + 3 : 0;
    int slash = text.indexOf(QLatin1Char('/'), start);
    if (slash < 0) slash = int(text.size());
    if (slash <= start) return text;
    const int at = text.lastIndexOf(QLatin1Char('@'), slash - 1);
    return at < start ? text : text.left(start) + text.mid(at + 1);
}

QUrl publicInfoUrl(const QUrl& server) {
    QUrl url = server;
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    url.setPath(path + QStringLiteral("/System/Info/Public"));
    url.setQuery(QString());
    url.setFragment(QString());
    return url;
}

// ---- the server's answer -----------------------------------------------------------------------------------
bool parsePublicInfo(const QByteArray& body, ServerInfo& info, QString& error) {
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(body, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        error = QObject::tr("The address answered, but not like a Jellyfin server.");
        return false;
    }
    const auto object = doc.object();
    const QString product = object.value(QStringLiteral("ProductName")).toString();
    const QString name = clean(object.value(QStringLiteral("ServerName")).toString(), 64);
    const QString version = clean(object.value(QStringLiteral("Version")).toString(), 32);
    const QString id = clean(object.value(QStringLiteral("Id")).toString(), 64);
    if (!product.isEmpty() && !product.contains(QLatin1String("jellyfin"), Qt::CaseInsensitive)) {
        error = QObject::tr("That is a %1, not a Jellyfin server.").arg(clean(product, 40));
        return false;
    }
    // Jellyfin's document always carries a version and an id; anything without them is some other service's JSON.
    if (version.isEmpty() || id.isEmpty()) {
        error = QObject::tr("The address answered, but not like a Jellyfin server.");
        return false;
    }
    info.name = name.isEmpty() ? QObject::tr("Jellyfin") : name;
    info.version = version;
    info.id = id;
    return true;
}

QString describeFailure(QNetworkReply::NetworkError error, int httpStatus, const QString& hostPort, bool* unreachable) {
    if (unreachable) *unreachable = false;
    if (httpStatus >= 300 && httpStatus < 400)
        return QObject::tr("%1 redirects somewhere else. Type the address it redirects to.").arg(hostPort);
    if (httpStatus == 404)
        return QObject::tr("%1 answered, but it is not Jellyfin (nothing at /System/Info/Public). Check the port and any folder in the address.").arg(hostPort);
    if (httpStatus > 0)
        return QObject::tr("%1 answered with an error (%2), so it is not a working Jellyfin server.").arg(hostPort).arg(httpStatus);
    if (unreachable) *unreachable = true;
    switch (error) {
    case QNetworkReply::HostNotFoundError:
        return QObject::tr("The name %1 was not found. Check the spelling, or use the server's number address.").arg(hostPort);
    case QNetworkReply::ConnectionRefusedError:
        return QObject::tr("%1 refused the connection. Check the port, and that Jellyfin is running.").arg(hostPort);
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        return QObject::tr("%1 did not answer in time. Check the address, and that the server is on and on this network.").arg(hostPort);
    case QNetworkReply::SslHandshakeFailedError:
        return QObject::tr("The secure connection to %1 failed. Try http:// instead, or check the server's certificate.").arg(hostPort);
    default:
        return QObject::tr("%1 could not be reached. Check the address and that the server is on.").arg(hostPort);
    }
}

// ---- discovery ---------------------------------------------------------------------------------------------
QByteArray discoveryRequest() { return QByteArrayLiteral("who is JellyfinServer?"); }

bool isLanAddress(const QHostAddress& address) {
    QHostAddress a = address;
    bool ok = false;
    if (a.protocol() == QAbstractSocket::IPv6Protocol) {
        const quint32 v4 = a.toIPv4Address(&ok);   // ::ffff:a.b.c.d
        if (ok) a = QHostAddress(v4);
    }
    if (a.isNull()) return false;
    if (a.isLoopback()) return true;
    if (a.protocol() == QAbstractSocket::IPv4Protocol) {
        static const QVector<QPair<QHostAddress, int>> nets = {
            {QHostAddress(QStringLiteral("10.0.0.0")), 8}, {QHostAddress(QStringLiteral("172.16.0.0")), 12},
            {QHostAddress(QStringLiteral("192.168.0.0")), 16}, {QHostAddress(QStringLiteral("169.254.0.0")), 16}};
        for (const auto& net : nets) if (a.isInSubnet(net.first, net.second)) return true;
        return false;
    }
    return a.isInSubnet(QHostAddress(QStringLiteral("fe80::")), 10) || a.isInSubnet(QHostAddress(QStringLiteral("fc00::")), 7);
}

bool parseDiscoveryReply(const QByteArray& datagram, const QHostAddress& sender, FoundServer& found) {
    if (datagram.isEmpty() || datagram.size() > MaxDatagramBytes) return false;
    if (!isLanAddress(sender)) return false;
    bool ok = false;
    QHostAddress v4 = sender;
    const quint32 ip = sender.toIPv4Address(&ok);
    if (!ok) return false;   // the datagram went out on IPv4 broadcast; an IPv6 answer is not one of ours
    v4 = QHostAddress(ip);
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(datagram, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const auto object = doc.object();
    const QString id = clean(object.value(QStringLiteral("Id")).toString(), 64);
    const QString address = object.value(QStringLiteral("Address")).toString();
    if (id.isEmpty() || address.isEmpty()) return false;
    const auto announced = parseAddress(address);
    if (!announced.ok) return false;
    QUrl url = announced.url;
    url.setHost(v4.toString());
    const auto checked = parseAddress(url.toString(QUrl::FullyEncoded));
    if (!checked.ok) return false;
    const QString name = clean(object.value(QStringLiteral("Name")).toString(), 48);
    found.name = name.isEmpty() ? QObject::tr("Jellyfin server") : name;
    found.id = id;
    found.url = checked.url;
    return true;
}

// ---- apps.json ---------------------------------------------------------------------------------------------
bool hasJellyfinApp(const QVector<ContentApp>& apps) {
    return std::any_of(apps.begin(), apps.end(), [](const ContentApp& app) { return app.kind == QLatin1String("jellyfin"); });
}

QString registrationId(const QVector<ContentApp>& apps) {
    const auto taken = [&](const QString& id) {
        return std::any_of(apps.begin(), apps.end(), [&](const ContentApp& app) { return app.id == id; });
    };
    if (!taken(QStringLiteral("jellyfin"))) return QStringLiteral("jellyfin");
    QString id = QStringLiteral("jellyfin-server");
    for (int n = 2; taken(id); ++n) id = QStringLiteral("jellyfin-server-%1").arg(n);
    return id;
}

bool saveRegistration(const QString& appsJsonPath, const QString& address, const QString& editId, QString& error,
                      QString* savedId) {
    error.clear();
    const auto parsed = parseAddress(address);
    if (!parsed.ok) { error = parsed.error; return false; }
    QVector<ContentApp> apps;
    if (!ContentRegistry::load(appsJsonPath, apps, error)) return false;   // an unreadable apps.json is reported, never replaced
    ContentApp app;
    if (editId.isEmpty()) {
        if (hasJellyfinApp(apps)) { error = QObject::tr("A Jellyfin server is already added. Change its address from its item menu."); return false; }
        app.id = registrationId(apps);
        app.title = QStringLiteral("Jellyfin");
        app.kind = QStringLiteral("jellyfin");
        app.category = QStringLiteral("tv");
    } else {
        const auto it = std::find_if(apps.begin(), apps.end(), [&](const ContentApp& candidate) {
            return candidate.id == editId && candidate.kind == QLatin1String("jellyfin");
        });
        if (it == apps.end()) { error = QObject::tr("That Jellyfin registration is gone."); return false; }
        app = *it;
    }
    app.url = parsed.text;
    if (!ContentRegistry::upsert(appsJsonPath, app, error)) return false;
    if (savedId) *savedId = app.id;
    return true;
}

// ---- ServerProbe -------------------------------------------------------------------------------------------
ServerProbe::ServerProbe(QNetworkAccessManager* network, QObject* parent) : QObject(parent), network_(network) {}

void ServerProbe::cancel() {
    ++request_;
    if (reply_) {
        auto* old = reply_;
        reply_ = nullptr;
        old->disconnect(this);
        old->abort();
        old->deleteLater();
    }
}

quint64 ServerProbe::check(const QUrl& server) {
    cancel();
    const quint64 request = ++request_;
    const QString hostPort = server.port() > 0 ? QStringLiteral("%1:%2").arg(server.host()).arg(server.port()) : server.host();
    if (!network_) {
        QTimer::singleShot(0, this, [this, request, hostPort] {
            if (request != request_) return;
            emit finished(request, false, {}, {}, describeFailure(QNetworkReply::UnknownNetworkError, 0, hostPort), true);
        });
        return request;
    }
    QNetworkRequest get(publicInfoUrl(server));
    get.setTransferTimeout(ProbeTimeoutMs);
    get.setRawHeader("Accept", "application/json");
    // A redirect that stays on the same origin is followed; one to another host is reported, not followed.
    get.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    qInfo().noquote() << "jellyfin setup: asking" << hostPort << "for its public information";
    auto* reply = network_->get(get);
    reply_ = reply;
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
        if (received > MaxInfoBytes) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, request, hostPort] {
        reply->deleteLater();
        if (reply != reply_ || request != request_) return;   // abandoned
        reply_ = nullptr;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool tooBig = reply->bytesAvailable() > MaxInfoBytes;
        if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300 && !tooBig) {
            ServerInfo info;
            QString why;
            if (parsePublicInfo(reply->read(MaxInfoBytes), info, why)) {
                qInfo().noquote() << "jellyfin setup:" << hostPort << "is Jellyfin" << info.version;
                emit finished(request, true, info.name, info.version, {}, false);
            } else {
                qInfo().noquote() << "jellyfin setup:" << hostPort << "answered, not Jellyfin";
                emit finished(request, false, {}, {}, why, false);
            }
            return;
        }
        bool unreachable = false;
        const QString why = tooBig ? QObject::tr("The address answered, but not like a Jellyfin server.")
                                   : describeFailure(reply->error(), status, hostPort, &unreachable);
        qInfo().noquote() << "jellyfin setup:" << hostPort << "failed: error" << int(reply->error()) << "http" << status;
        emit finished(request, false, {}, {}, why, unreachable && !tooBig);
    });
    return request;
}

// ---- Discovery ---------------------------------------------------------------------------------------------
Discovery::Discovery(QObject* parent) : QObject(parent) {}
Discovery::~Discovery() { cancel(); }

void Discovery::cancel() {
    if (timer_) { timer_->stop(); timer_->deleteLater(); timer_ = nullptr; }
    if (socket_) { socket_->disconnect(this); socket_->close(); socket_->deleteLater(); socket_ = nullptr; }
    seen_.clear();
}

void Discovery::start(int timeoutMs) {
    cancel();
    socket_ = new QUdpSocket(this);
    if (!socket_->bind(QHostAddress::AnyIPv4, 0)) {   // an ephemeral port: replies come back to it
        qInfo().noquote() << "jellyfin setup: discovery socket could not be opened:" << socket_->errorString();
        cancel();
        QTimer::singleShot(0, this, [this] { emit finished(0); });
        return;
    }
    connect(socket_, &QUdpSocket::readyRead, this, &Discovery::readReplies);
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &Discovery::finish);
    timer_->start(timeoutMs);
    // One datagram per LAN network this PC is on: the directed broadcast of each private IPv4 subnet, never an
    // address outside the private ranges, so the request cannot leave the local network.
    int sent = 0;
    for (const auto& iface : QNetworkInterface::allInterfaces()) {
        const auto flags = iface.flags();
        if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning) ||
            (flags & QNetworkInterface::IsLoopBack) || !(flags & QNetworkInterface::CanBroadcast)) continue;
        for (const auto& entry : iface.addressEntries()) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol || entry.broadcast().isNull()) continue;
            if (!isLanAddress(entry.ip()) || entry.ip().isLoopback()) continue;
            if (sent >= 8) break;
            if (socket_->writeDatagram(discoveryRequest(), entry.broadcast(), DiscoveryPort) >= 0) ++sent;
        }
    }
    qInfo().noquote() << "jellyfin setup: discovery asked on" << sent << "network(s)";
    if (sent == 0) { cancel(); QTimer::singleShot(0, this, [this] { emit finished(0); }); }
}

void Discovery::readReplies() {
    while (socket_ && socket_->hasPendingDatagrams()) {
        QByteArray data;
        data.resize(int(std::min<qint64>(socket_->pendingDatagramSize(), MaxDatagramBytes + 1)));
        QHostAddress sender;
        quint16 port = 0;
        const qint64 got = socket_->readDatagram(data.data(), data.size(), &sender, &port);
        if (got <= 0) continue;
        data.truncate(int(got));
        FoundServer server;
        if (!parseDiscoveryReply(data, sender, server) || seen_.contains(server.id) || seen_.size() >= MaxFoundServers) continue;
        seen_.push_back(server.id);
        qInfo().noquote() << "jellyfin setup: found a server at" << server.url.host() << ":" << server.url.port();
        emit found(server.name, server.id, server.url);
    }
}

void Discovery::finish() {
    const int count = int(seen_.size());
    cancel();
    emit finished(count);
}

}   // namespace xmb::jellyfinSetup
