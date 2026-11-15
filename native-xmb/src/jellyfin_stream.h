#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QHash>

class QNetworkAccessManager;
class QTcpServer;
class QTcpSocket;

namespace xmb {

// A diagnostic line fit for a log or a screen: control characters flattened, anything that looks like a
// credential (api_key=, Token=, Authorization:, a 32-hex token or id, a long opaque run) replaced, and cut
// to maxChars. Used for the first bytes of a server reply, so the real server message shows without a secret.
QString scrubDiagnostic(const QByteArray& bytes, int maxChars = 300);

// The largest playlist the stream accepts from the server. A transcode's main.m3u8 lists every segment with
// the whole query string on each line (about 0.8 KB), so a three hour film is a megabyte or two.
constexpr qsizetype kMaxPlaylistBytes = 16 * 1024 * 1024;

class JellyfinStream final : public QObject {
    Q_OBJECT
public:
    explicit JellyfinStream(QObject* parent = nullptr);
    ~JellyfinStream() override;

    QUrl open(const QUrl& upstream, const QByteArray& authorization);
    void close();

signals:
    // The player's request failed in a way the stream can name: one plain sentence.
    void failed(QString message);
    // The server answered a stream request with an error status, or the stream refused to relay it. request is
    // the method and path (no query), status the HTTP status (the one the stream returned to the player when it
    // refused itself), detail the scrubbed first bytes of the body or the stream's own reason.
    void upstreamRejected(int status, QString request, QString detail);

private:
    struct Request;

    void acceptConnections();
    void readRequest(QTcpSocket* socket);
    void beginRequest(QTcpSocket* socket, const QByteArray& bytes);
    void startUpstream(Request* request, const QUrl& url, int redirects = 0);
    void onMetadata(Request* request);
    void pump(Request* request);
    void finish(Request* request);
    void fail(Request* request, int status = 502, const QString& reason = {});
    QString describe(const Request* request) const;
    void removeRequest(QTcpSocket* socket);
    QUrl localUrl(const QUrl& upstream) const;
    bool sameOrigin(const QUrl& url) const;
    bool isPlaylist(const Request* request) const;
    QByteArray responseHead(int status, const QList<QPair<QByteArray, QByteArray>>& headers,
                            bool closeDelimited) const;

    QTcpServer* server_ = nullptr;
    QNetworkAccessManager* network_ = nullptr;
    QHash<QTcpSocket*, Request*> requests_;
    QUrl upstream_;
    QByteArray authorization_;
    QByteArray nonce_;
    bool active_ = false;
};

} // namespace xmb
