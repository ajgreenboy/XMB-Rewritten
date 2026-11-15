// Jellyfin playback: the PlaybackInfo request and DeviceProfile, the transcode URL the client hands the player, the
// loopback stream that relays it (HLS playlists, big playlists, error replies), the diagnostic probe and the
// subtitle deadline (audit/JELLYFIN-PLAYBACK.md). Everything runs against a strict in-process stand-in for a
// Jellyfin 10.10+/12 server on 127.0.0.1; no real server, account or credential is involved.
//   xmb-jellyfin-playback-tests
#include "jellyfin_client.h"
#include "jellyfin_stream.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrlQuery>
#include <cstdio>
#include <functional>

namespace {
int checks = 0, failures = 0;
struct Collector {
    QList<QVariantList> v;
    int count() const { return int(v.size()); }
    QVariantList first() const { return v.first(); }
    void clear() { v.clear(); }
};
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}

constexpr const char* kItem = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb02";
constexpr const char* kUser = "0123456789abcdef0123456789abcdef";
const QByteArray kToken = "standin-token-for-tests-only";

struct Req {
    QByteArray method, path, rawQuery, body;
    QHash<QByteArray, QByteArray> headers;   // lower-case names
    QUrlQuery query() const { return QUrlQuery(QString::fromLatin1(rawQuery)); }
};
struct Resp {
    int status = 200;
    QByteArray body, type = "application/json";
    int delayMs = 0;
};

// A deliberately strict stand-in: like the real server it needs the Authorization header (or ApiKey), and the
// transcode routes need MediaSourceId, DeviceId and PlaySessionId.
class FakeServer : public QObject {
public:
    explicit FakeServer(QObject* parent = nullptr) : QObject(parent) {
        connect(&server_, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = server_.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { read(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        listening = server_.listen(QHostAddress::LocalHost, 0);
    }
    quint16 port() const { return server_.serverPort(); }
    QUrl url() const { return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(port())); }
    bool listening = false;
    std::function<Resp(const Req&)> handler;
    QList<Req> log;
    int bigPlaylistSegments = 0;   // main.m3u8 length when non-zero

private:
    void read(QTcpSocket* socket) {
        auto& buffer = buffers_[socket];
        buffer += socket->readAll();
        const int end = buffer.indexOf("\r\n\r\n");
        if (end < 0) return;
        const QList<QByteArray> lines = buffer.left(end).split('\n');
        Req req;
        const QList<QByteArray> first = lines.first().trimmed().split(' ');
        if (first.size() < 2) return;
        req.method = first[0];
        const QByteArray target = first[1];
        const int q = target.indexOf('?');
        req.path = q < 0 ? target : target.left(q);
        req.rawQuery = q < 0 ? QByteArray() : target.mid(q + 1);
        for (int i = 1; i < lines.size(); ++i) {
            const QByteArray line = lines[i].trimmed();
            const int colon = line.indexOf(':');
            if (colon > 0) req.headers.insert(line.left(colon).toLower(), line.mid(colon + 1).trimmed());
        }
        const int length = req.headers.value("content-length", "0").toInt();
        if (buffer.size() < end + 4 + length) return;
        req.body = buffer.mid(end + 4, length);
        buffers_.remove(socket);
        log.push_back(req);
        const Resp resp = handler ? handler(req) : Resp{404, "{}"};
        QPointer<QTcpSocket> guard(socket);
        auto write = [guard, resp] {
            if (!guard) return;
            QByteArray out = "HTTP/1.1 " + QByteArray::number(resp.status) + " X\r\nContent-Type: " + resp.type +
                             "\r\nContent-Length: " + QByteArray::number(resp.body.size()) + "\r\nConnection: close\r\n\r\n" + resp.body;
            guard->write(out);
            guard->disconnectFromHost();
        };
        if (resp.delayMs > 0) QTimer::singleShot(resp.delayMs, this, write); else write();
    }
    QTcpServer server_;
    QHash<QTcpSocket*, QByteArray> buffers_;
};

template<class Fn>
bool waitFor(Fn done, int timeoutMs = 20000) {
    QElapsedTimer timer; timer.start();
    while (!done() && timer.elapsed() < timeoutMs) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

struct Fetched { int status = 0; QByteArray body; bool finished = false; QNetworkReply::NetworkError error = QNetworkReply::NoError; };
Fetched fetch(QNetworkAccessManager& network, const QUrl& url, const QByteArray& range = {}) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(30000);
    if (!range.isEmpty()) request.setRawHeader("Range", range);
    QNetworkReply* reply = network.get(request);
    Fetched result;
    QObject::connect(reply, &QNetworkReply::finished, [&] { result.finished = true; });
    waitFor([&] { return result.finished; }, 30000);
    result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = reply->readAll();
    result.error = reply->error();
    reply->deleteLater();
    return result;
}

QByteArray authToken(const Req& r) {
    const QByteArray header = r.headers.value("authorization");
    const int at = header.indexOf("Token=\"");
    if (at < 0) return {};
    const int from = at + 7;
    return header.mid(from, header.indexOf('"', from) - from);
}
QByteArray authDevice(const Req& r) {
    const QByteArray header = r.headers.value("authorization");
    const int at = header.indexOf("DeviceId=\"");
    if (at < 0) return {};
    const int from = at + 10;
    return header.mid(from, header.indexOf('"', from) - from);
}

Resp json(const QJsonObject& o, int status = 200) { return {status, QJsonDocument(o).toJson(QJsonDocument::Compact), "application/json", 0}; }
Resp error(int status, const QString& message) { return json({{"Message", message}}, status); }

// What 10.9+ builds in StreamInfo.ToUrl: a relative path, the query beginning with DeviceId, ApiKey appended,
// and values that need their percent-encoding kept (a comma list, a plus, an ampersand inside a value).
QString transcodeUrl(const QString& deviceId, int burn) {
    QString url = QStringLiteral("/videos/@ITEM@/master.m3u8?DeviceId=@DEVICE@&MediaSourceId=@ITEM@&VideoCodec=h264&AudioCodec=aac,ac3"
        "&AudioStreamIndex=1&VideoBitrate=19808000&AudioBitrate=192000&MaxFramerate=23.976025&PlaySessionId=ps0123456789abcdef"
        "&ApiKey=@KEY@&TranscodingMaxAudioChannels=2&RequireAvc=false&EnableAudioVbrEncoding=true&Tag=a%2Bb%26c%20d"
        "&SegmentContainer=ts&MinSegments=1&BreakOnNonKeyFrames=True&TranscodeReasons=AudioCodecNotSupported,VideoCodecNotSupported");
    url.replace(QStringLiteral("@ITEM@"), QString::fromLatin1(kItem)).replace(QStringLiteral("@DEVICE@"), deviceId)
        .replace(QStringLiteral("@KEY@"), QString::fromLatin1(kToken));
    if (burn >= 0) url += QStringLiteral("&SubtitleStreamIndex=") + QString::number(burn) + QStringLiteral("&SubtitleMethod=Encode");
    return url;
}

Resp serve(const Req& r, FakeServer& fake, bool& transcodeRequested) {
    const QString path = QString::fromLatin1(r.path);
    if (r.method == "POST" && path == QLatin1String("/Users/AuthenticateByName"))
        return json({{"AccessToken", QString::fromLatin1(kToken)}, {"User", QJsonObject{{"Id", QString::fromLatin1(kUser)}}}});
    if (authToken(r) != kToken && r.query().queryItemValue("ApiKey").toLatin1() != kToken) return error(401, "Unauthorized");
    if (r.method == "POST" && path == QStringLiteral("/Items/%1/PlaybackInfo").arg(QString::fromLatin1(kItem))) {
        const QJsonObject body = QJsonDocument::fromJson(r.body).object();
        const QJsonObject profile = body.value("DeviceProfile").toObject();
        const QString deviceId = body.value("DeviceId").toString();
        // The checks a real server's model binding and a careful client would care about.
        if (deviceId.isEmpty() || deviceId.toLatin1() != authDevice(r) || profile.value("DeviceId").toString() != deviceId)
            return error(400, "DeviceId must be the same in the header, the body and the profile");
        if (r.headers.value("user-agent") != "XMB/1.0") return error(400, "user agent");
        const QJsonArray transcoding = profile.value("TranscodingProfiles").toArray();
        if (transcoding.isEmpty()) return error(400, "no TranscodingProfiles");
        const QJsonObject t = transcoding.first().toObject();
        if (t.value("Container") != "ts" || t.value("Protocol") != "hls" || t.value("Type") != "Video" ||
            t.value("VideoCodec") != "h264" || t.value("AudioCodec") != "aac" || t.value("Context") != "Streaming")
            return error(400, "transcoding profile");
        if (body.value("UserId").toString() != QLatin1String(kUser)) return error(400, "UserId");
        const int burn = body.contains("SubtitleStreamIndex") ? body.value("SubtitleStreamIndex").toInt() : -1;
        if (burn >= 0) {
            const QJsonArray subs = profile.value("SubtitleProfiles").toArray();
            if (subs.isEmpty() || subs.first().toObject().value("Method") != "Encode" || body.value("EnableDirectPlay").toBool())
                return error(400, "burn-in needs an Encode subtitle profile and no direct play");
        }
        QJsonObject source{{"Id", QString::fromLatin1(kItem)}, {"SupportsDirectPlay", false}, {"SupportsDirectStream", false},
                           {"SupportsTranscoding", true}, {"TranscodingUrl", transcodeUrl(deviceId, burn)},
                           {"MediaStreams", QJsonArray{QJsonObject{{"Index", 0}, {"Type", "Video"}, {"Codec", "hevc"}, {"DisplayTitle", "1080p"}}}}};
        return json({{"MediaSources", QJsonArray{source}}, {"PlaySessionId", "ps0123456789abcdef"}});
    }
    if (path.startsWith(QLatin1String("/videos/"))) {
        transcodeRequested = true;
        const QUrlQuery q = r.query();
        if (q.queryItemValue("MediaSourceId").isEmpty()) return error(400, "The mediaSourceId field is required.");
        if (q.queryItemValue("DeviceId").isEmpty() || q.queryItemValue("PlaySessionId").isEmpty()) return error(500, "DeviceId/PlaySessionId missing");
        // The encoded values must reach the server exactly as the server wrote them (a decoded plus is a space).
        if (path.endsWith(QLatin1String("/master.m3u8")) || path.endsWith(QLatin1String("/main.m3u8"))) {
            if (!r.rawQuery.contains("Tag=a%2Bb%26c%20d")) return error(500, "Tag was re-encoded: " + QString::fromLatin1(r.rawQuery));
            if (r.rawQuery.contains("ApiKey")) return error(500, "ApiKey leaked into the stream URL");
        }
        if (path.endsWith(QLatin1String("/master.m3u8"))) {
            QByteArray list = "#EXTM3U\n#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH=8000000,CODECS=\"avc1.640028,mp4a.40.2\",RESOLUTION=1920x1080\nmain.m3u8?" + r.rawQuery + "\n";
            return {200, list, "application/x-mpegURL", 0};
        }
        if (path.endsWith(QLatin1String("/main.m3u8"))) {
            QByteArray list = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:6\n#EXT-X-MEDIA-SEQUENCE:0\n#EXT-X-PLAYLIST-TYPE:VOD\n";
            const int count = fake.bigPlaylistSegments > 0 ? fake.bigPlaylistSegments : 2;
            for (int i = 0; i < count; ++i)
                list += "#EXTINF:6.000000, nodesc\nhls1/main/" + QByteArray::number(i) + ".ts?" + r.rawQuery + "&runtimeTicks=" +
                        QByteArray::number(i * 60000000LL) + "&actualSegmentLengthTicks=60000000\n";
            list += "#EXT-X-ENDLIST\n";
            return {200, list, "application/x-mpegURL", 0};
        }
        if (path.contains(QLatin1String("/hls1/main/"))) {
            if (!q.hasQueryItem("runtimeTicks")) return error(400, "runtimeTicks");
            return {200, QByteArray(188, 'G'), "video/mp2t", 0};
        }
    }
    if (path.contains(QLatin1String("/Subtitles/")) && path.endsWith(QLatin1String("/Stream.sup")))
        return {200, QByteArray(32, 'S'), "application/octet-stream", 16500};   // extraction first: slower than the 15 s API deadline
    return error(404, "Not found");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace xmb;

    // ---- scrubDiagnostic: the server's own words, never a credential ------------------------------------------
    {
        const QString s = scrubDiagnostic("{\"Message\":\"Error processing request.\",\"ApiKey\":\"abcdef0123456789abcdef0123456789\"}");
        check(s.contains("Error processing request."), "scrub: the server's message survives");
        check(!s.contains("abcdef0123456789"), "scrub: an api key is removed");
        check(!scrubDiagnostic("Token=\"0123456789abcdef0123456789abcdef\" ok").contains("0123456789abcdef"), "scrub: Token= value removed");
        check(!scrubDiagnostic("api_key=zzzzzzzzzzzz&x=1").contains("zzzzzzzz"), "scrub: api_key= removed");
        check(!scrubDiagnostic("item 0123456789abcdef0123456789abcdef failed").contains("0123456789abcdef"), "scrub: a 32-hex id/token removed");
        check(!scrubDiagnostic("Authorization: MediaBrowser Token=abc").contains("MediaBrowser Token=abc"), "scrub: Authorization value removed");
        check(scrubDiagnostic(QByteArray(1000, 'x') + " tail").size() <= 303, "scrub: cut to 300 characters");
        check(!scrubDiagnostic(QByteArray("a\r\nb\x01 c")).contains(QChar(1)) && !scrubDiagnostic("a\r\nb").contains('\n'), "scrub: control characters flattened");
        check(scrubDiagnostic(QByteArray(100, 'k') + QByteArray(40, 'Q')).contains("<hidden>"), "scrub: a long opaque run is hidden");
    }

    FakeServer fake;
    check(fake.listening, "stand-in server listens");
    bool transcodeRequested = false;
    fake.handler = [&](const Req& r) { return serve(r, fake, transcodeRequested); };

    JellyfinClient client;
    Collector authenticated, ready, failed, probed, sub;
    QObject::connect(&client, &JellyfinClient::authenticated, [&] { authenticated.v.push_back({}); });
    QObject::connect(&client, &JellyfinClient::playbackReady, [&](quint64, const JellyfinPlayback& p) { ready.v.push_back({QVariant::fromValue(quint64(0)), QVariant::fromValue(p)}); });
    QObject::connect(&client, &JellyfinClient::failed, [&](quint64, const QString&) { failed.v.push_back({}); });
    QObject::connect(&client, &JellyfinClient::probeFinished, [&](quint64, int status, const QString& detail) { probed.v.push_back({QVariant::fromValue(quint64(0)), status, detail}); });
    QObject::connect(&client, &JellyfinClient::subtitleReady, [&](quint64, int, const QByteArray& bytes) { sub.v.push_back({QVariant::fromValue(quint64(0)), 0, bytes}); });
    client.signIn(fake.url(), QStringLiteral("tester"), QStringLiteral("not-a-real-password"));
    check(waitFor([&] { return authenticated.count() > 0; }), "sign-in against the stand-in");
    check(fake.log.first().headers.value("user-agent") == "XMB/1.0", "sign-in sends the XMB user agent");

    // ---- PlaybackInfo and the transcode URL ----------------------------------------------------------------------
    JellyfinPlayback transcode;
    {
        ready.clear(); failed.clear();
        client.playback(QString::fromLatin1(kItem), false, 20000, -1);
        check(waitFor([&] { return ready.count() + failed.count() > 0; }), "transcode: PlaybackInfo answered");
        check(ready.count() == 1, "transcode: PlaybackInfo accepted by the strict server (profile, DeviceId, user agent)");
        if (ready.count() == 1) {
            transcode = qvariant_cast<JellyfinPlayback>(ready.first().at(1));
            check(transcode.playMethod == QLatin1String("Transcode"), "transcode: method");
            check(transcode.url.path() == QStringLiteral("/videos/%1/master.m3u8").arg(QString::fromLatin1(kItem)), "transcode: the server's own path is used");
            check(transcode.url.host() == QLatin1String("127.0.0.1") && transcode.url.port() == fake.port(), "transcode: the relative url resolves on the signed-in server");
            const QString query = transcode.url.query(QUrl::FullyEncoded);
            check(!query.contains("ApiKey", Qt::CaseInsensitive), "transcode: the api key is not in the url handed to the player");
            check(query.contains("Tag=a%2Bb%26c%20d"), "transcode: encoded values keep their encoding (plus, ampersand, space)");
            check(query.contains("AudioCodec=aac,ac3") || query.contains("AudioCodec=aac%2Cac3"), "transcode: a comma list survives");
            check(query.contains("PlaySessionId=ps0123456789abcdef") && query.contains("MediaSourceId=") && query.contains("DeviceId="), "transcode: required keys kept");
            check(query.contains("TranscodeReasons=") && query.contains("SegmentContainer=ts"), "transcode: the server's other keys kept");
        }
    }
    JellyfinPlayback burn;
    {
        ready.clear(); failed.clear();
        client.playback(QString::fromLatin1(kItem), false, 20000, 3, QStringLiteral("pgssub"));
        check(waitFor([&] { return ready.count() + failed.count() > 0; }), "burn-in: PlaybackInfo answered");
        check(ready.count() == 1, "burn-in: PlaybackInfo accepted (SubtitleStreamIndex, Encode profile, no direct play)");
        if (ready.count() == 1) {
            burn = qvariant_cast<JellyfinPlayback>(ready.first().at(1));
            const QString query = burn.url.query(QUrl::FullyEncoded);
            check(query.contains("SubtitleStreamIndex=3") && query.contains("SubtitleMethod=Encode"), "burn-in: the url carries the burn parameters exactly once");
            check(query.count("SubtitleStreamIndex") == 1, "burn-in: no duplicate burn parameter");
            check(burn.burnSubtitleIndex == 3, "burn-in: index remembered");
        }
    }

    // ---- the loopback stream: playlists, segments, relayed errors ---------------------------------------------
    JellyfinStream stream;
    QNetworkAccessManager network;
    QList<QVariantList> rejections;
    QObject::connect(&stream, &JellyfinStream::upstreamRejected, [&](int status, const QString& request, const QString& detail) {
        rejections.push_back({status, request, detail});
    });
    if (transcode.url.isValid()) {
        const QUrl local = stream.open(transcode.url, client.authorizationHeader());
        check(local.isValid() && local.host() == QLatin1String("127.0.0.1"), "stream: a local url");
        check(!local.toString().contains("ApiKey") && !local.toString().contains(QString::fromLatin1(kToken)), "stream: the local url carries no token");
        const auto master = fetch(network, local);
        check(master.status == 200, "stream: master.m3u8 relayed (status 200 from the strict server)");
        check(master.body.contains("#EXT-X-STREAM-INF"), "stream: master body");
        QString variant;
        for (const QByteArray& line : master.body.split('\n'))
            if (line.startsWith("http://127.0.0.1:")) variant = QString::fromLatin1(line.trimmed());
        check(!variant.isEmpty() && !master.body.contains(QByteArray::number(fake.port()).prepend("127.0.0.1:") + "/videos/"),
              "stream: the variant url is rewritten to the loopback relay");
        const auto main = fetch(network, QUrl(variant));
        check(main.status == 200 && main.body.contains("hls1/main/") && main.body.count("#EXTINF") == 2, "stream: main.m3u8 relayed with its segments");
        QString segment;
        for (const QByteArray& line : main.body.split('\n'))
            if (line.contains("/hls1/main/0.ts")) segment = QString::fromLatin1(line.trimmed());
        const auto ts = fetch(network, QUrl(segment), "bytes=0-");
        check(ts.status == 200 && ts.body.size() == 188, "stream: a segment relayed (with the player's Range header)");
        for (const Req& r : fake.log)
            if (r.path.startsWith("/videos/")) {
                check(authToken(r) == kToken, "stream: every upstream request carries the Authorization header");
                check(r.headers.value("user-agent") == "XMB/1.0", "stream: upstream user agent is the XMB one");
                check(!r.rawQuery.contains("ApiKey"), "stream: no api key in any upstream url");
            }
        check(rejections.isEmpty(), "stream: nothing rejected on the good path");
    }

    // A transcode of a long film: every segment line repeats the whole query string, so the playlist passes the old
    // 1 MiB limit. (2200 segments of about 0.9 KB.)
    if (transcode.url.isValid()) {
        fake.bigPlaylistSegments = 2200;
        const QUrl local = stream.open(transcode.url, client.authorizationHeader());
        const auto master = fetch(network, local);
        QString variant;
        for (const QByteArray& line : master.body.split('\n'))
            if (line.startsWith("http://127.0.0.1:")) variant = QString::fromLatin1(line.trimmed());
        const auto main = fetch(network, QUrl(variant));
        check(main.status == 200 && main.body.count("#EXTINF") == 2200, "stream: a 2200-segment (3 hour) playlist larger than 1 MiB is relayed whole");
        check(main.body.size() > 1024 * 1024, "stream: the test playlist really exceeds 1 MiB");
        // A playlist beyond the limit is refused with a reason, not silently as a bare 502.
        rejections.clear();
        fake.bigPlaylistSegments = 40000;
        const auto huge = fetch(network, QUrl(variant));
        check(huge.status == 502, "stream: a playlist beyond the limit answers 502");
        check(waitFor([&] { return !rejections.isEmpty(); }, 5000) && rejections.first().at(2).toString().contains("larger than"),
              "stream: the refusal names its reason");
        fake.bigPlaylistSegments = 0;
    }

    // The server's own error reply reaches the log and the screen: status, request without a query, first bytes.
    {
        fake.handler = [&](const Req& r) -> Resp {
            if (r.path.startsWith("/videos/")) return {500, "{\"Message\":\"Error processing request.\",\"Token\":\"0123456789abcdef0123456789abcdef\"}", "application/json", 0};
            return serve(r, fake, transcodeRequested);
        };
        rejections.clear();
        const QUrl local = stream.open(transcode.url, client.authorizationHeader());
        const auto master = fetch(network, local);
        check(master.status == 500, "stream: an upstream 500 is relayed as 500");
        check(waitFor([&] { return !rejections.isEmpty(); }, 5000), "stream: an upstream 500 is reported");
        if (!rejections.isEmpty()) {
            check(rejections.first().at(0).toInt() == 500, "stream: the report carries the status");
            check(rejections.first().at(1).toString().startsWith("GET /videos/") && !rejections.first().at(1).toString().contains('?'), "stream: the report names the request without its query");
            check(rejections.first().at(2).toString().contains("Error processing request."), "stream: the report carries the server's message");
            check(!rejections.first().at(2).toString().contains("0123456789abcdef"), "stream: the report is scrubbed");
        }

        // The XMB's own probe of the same URL says the same thing.
        probed.clear();
        client.probeStream(transcode.url);
        check(waitFor([&] { return probed.count() > 0; }, 10000), "probe: answered");
        if (probed.count() > 0) {
            check(probed.first().at(1).toInt() == 500, "probe: the HTTP status");
            check(probed.first().at(2).toString().contains("Error processing request.") && !probed.first().at(2).toString().contains("0123456789abcdef"),
                  "probe: the scrubbed server message");
        }
        const Req& last = fake.log.last();
        check(authToken(last) == kToken && last.headers.value("user-agent") == "XMB/1.0", "probe: same headers as the real request");
        check(!last.rawQuery.contains("ApiKey"), "probe: no api key in the url");
        probed.clear();
        client.probeStream(QUrl(QStringLiteral("http://192.0.2.1/videos/x/master.m3u8")));
        check(waitFor([&] { return probed.count() > 0; }, 5000) && probed.first().at(1).toInt() == 0, "probe: another host is refused, never contacted");
        fake.handler = [&](const Req& r) { return serve(r, fake, transcodeRequested); };
    }
    stream.close();

    // ---- the PGS stream's deadline ---------------------------------------------------------------------------------
    // The server extracts every subtitle stream of the file before it answers (64 s measured on a 1080p remux). The
    // API requests give up after 15 s of silence; a subtitle must wait much longer.
    {
        sub.clear();
        client.subtitleImage(QString::fromLatin1(kItem), QString::fromLatin1(kItem), 3);
        check(waitFor([&] { return sub.count() > 0; }, 40000), "subtitle: answered");
        check(sub.count() > 0 && sub.first().at(2).toByteArray().size() == 32, "subtitle: a stream the server takes 16.5 s to extract is delivered, not abandoned at 15 s");
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
