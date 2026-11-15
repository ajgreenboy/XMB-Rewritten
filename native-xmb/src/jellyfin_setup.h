#pragma once
// Setting Jellyfin up from inside the XMB (audit/JELLYFIN-SETUP.md).
//
// Pure logic first (address rules, the public-info answer, the discovery reply, the apps.json save), so the
// unit runner can exercise all of it without a window or a network; then two small asynchronous helpers:
//   ServerProbe  GET <server>/System/Info/Public and say what answered
//   Discovery    Jellyfin's UDP auto-discovery: the fixed datagram "who is JellyfinServer?" to port 7359
// Neither helper ever blocks the caller, both are cancellable, and neither ever handles a credential: the one
// request is the server's public-information route, the one datagram is that fixed string.
#include "content_registry.h"
#include <QByteArray>
#include <QHostAddress>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVector>

class QNetworkAccessManager;
class QTimer;
class QUdpSocket;

namespace xmb::jellyfinSetup {

constexpr quint16 DiscoveryPort = 7359;
constexpr int ProbeTimeoutMs = 5000;
constexpr int DiscoveryTimeoutMs = 3000;
constexpr int MaxFoundServers = 12;

// ---- the address -------------------------------------------------------------------------------------------
struct ServerAddress {
    bool ok = false;
    QUrl url;
    QString text;    // normalised: lower-case scheme and host, no trailing slash
    QString error;   // one plain sentence when !ok
};
// What the person typed, made into a server address. "192.168.1.20:8096" means http://192.168.1.20:8096. The rules
// are content_registry's (http or https, a host, no credentials, no query, no fragment), with a sentence each.
ServerAddress parseAddress(const QString& typed);
// The typed text with any user name and password before an @ removed, for showing a refused address back without
// echoing a secret.
QString withoutCredentials(const QString& typed);
QUrl publicInfoUrl(const QUrl& server);   // <server>/System/Info/Public, keeping a reverse-proxy subpath

// ---- the server's answer -----------------------------------------------------------------------------------
struct ServerInfo {
    QString name, version, id;
};
// True when the body is Jellyfin's public information. A different product name (Emby) or a body that is not
// that document is refused with `error`.
bool parsePublicInfo(const QByteArray& body, ServerInfo& info, QString& error);
// A sentence for a request that failed: unreachable (no answer), or an answer that is not Jellyfin's.
// `unreachable` is set when the server simply did not answer (so saving the address anyway still makes sense).
QString describeFailure(QNetworkReply::NetworkError error, int httpStatus, const QString& hostPort, bool* unreachable = nullptr);

// ---- discovery ---------------------------------------------------------------------------------------------
struct FoundServer {
    QString name, id;
    QUrl url;
};
QByteArray discoveryRequest();   // "who is JellyfinServer?"
bool isLanAddress(const QHostAddress& address);
// One reply datagram. Only a LAN sender is believed, and the address the person is offered is the one the reply
// actually came from (the sender's IPv4 address with the scheme, port and path the server announced), never a
// host the reply names: a Docker or NAT address in the reply cannot send a request anywhere else.
bool parseDiscoveryReply(const QByteArray& datagram, const QHostAddress& sender, FoundServer& found);

// ---- apps.json ---------------------------------------------------------------------------------------------
bool hasJellyfinApp(const QVector<ContentApp>& apps);
// "jellyfin" unless an app of another kind already owns that id (an old web registration), then "jellyfin-server".
QString registrationId(const QVector<ContentApp>& apps);
// Saves a new registration (editId empty: id from registrationId(), title "Jellyfin", kind "jellyfin", category
// "tv") or changes only the url of the jellyfin app editId. The write is ContentRegistry::upsert, the same
// validation and atomic writer --import-apps uses; a registry that cannot be read is reported, never replaced.
bool saveRegistration(const QString& appsJsonPath, const QString& address, const QString& editId, QString& error,
                      QString* savedId = nullptr);

// ---- the page's state --------------------------------------------------------------------------------------
struct SetupState {
    enum class Check { Idle, Checking, Ok, Failed };
    QString editId;            // empty: adding; else the jellyfin app being changed
    QString address;           // as typed
    Check check = Check::Idle;
    QString checkedAddress;    // the normalised address the result belongs to
    QString serverName, serverVersion, error;
    bool unreachable = false;  // Failed because nothing answered
    bool searching = false, searched = false;
    QVector<FoundServer> found;
    quint64 request = 0;
};

// ---- asynchronous helpers ----------------------------------------------------------------------------------
class ServerProbe final : public QObject {
    Q_OBJECT
public:
    ServerProbe(QNetworkAccessManager* network, QObject* parent = nullptr);
    // Asks `server` for its public information; a request still running is abandoned (its answer never arrives).
    quint64 check(const QUrl& server);
    void cancel();
signals:
    void finished(quint64 request, bool ok, QString name, QString version, QString message, bool unreachable);
private:
    QNetworkAccessManager* network_;
    QNetworkReply* reply_ = nullptr;
    quint64 request_ = 0;
};

class Discovery final : public QObject {
    Q_OBJECT
public:
    explicit Discovery(QObject* parent = nullptr);
    ~Discovery() override;
    void start(int timeoutMs = DiscoveryTimeoutMs);   // returns at once; replies arrive as found()
    void cancel();                                    // silent: neither found() nor finished() follows
    bool running() const { return socket_ != nullptr; }
signals:
    void found(QString name, QString id, QUrl url);
    void finished(int count);
private:
    void readReplies();
    void finish();
    QUdpSocket* socket_ = nullptr;
    QTimer* timer_ = nullptr;
    QVector<QString> seen_;
};

}   // namespace xmb::jellyfinSetup
