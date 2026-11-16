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
bool g_fakeOmitsAudioIndex = false;   // the fake's TranscodingUrl leaves AudioStreamIndex out, so the client has to add it
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
QString transcodeUrl(const QString& deviceId, int burn, int audio = 1) {
    QString url = QStringLiteral("/videos/@ITEM@/master.m3u8?DeviceId=@DEVICE@&MediaSourceId=@ITEM@&VideoCodec=h264&AudioCodec=aac,ac3"
        "&AudioStreamIndex=@AUDIO@&VideoBitrate=19808000&AudioBitrate=192000&MaxFramerate=23.976025&PlaySessionId=ps0123456789abcdef"
        "&ApiKey=@KEY@&TranscodingMaxAudioChannels=2&RequireAvc=false&EnableAudioVbrEncoding=true&Tag=a%2Bb%26c%20d"
        "&SegmentContainer=ts&MinSegments=1&BreakOnNonKeyFrames=True&TranscodeReasons=AudioCodecNotSupported,VideoCodecNotSupported");
    url.replace(QStringLiteral("@ITEM@"), QString::fromLatin1(kItem)).replace(QStringLiteral("@DEVICE@"), deviceId)
        .replace(QStringLiteral("@KEY@"), QString::fromLatin1(kToken)).replace(QStringLiteral("@AUDIO@"), QString::number(audio));
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
        const int audio = body.contains("AudioStreamIndex") ? body.value("AudioStreamIndex").toInt() : -1;
        if (audio >= 0 && body.value("EnableDirectPlay").toBool()) return error(400, "a chosen audio stream cannot be a static direct play");
        QJsonObject source{{"Id", QString::fromLatin1(kItem)}, {"SupportsDirectPlay", false}, {"SupportsDirectStream", false},
                           {"SupportsTranscoding", true}, {"TranscodingUrl", g_fakeOmitsAudioIndex ? transcodeUrl(deviceId, burn, audio >= 0 ? audio : 1).remove(QStringLiteral("&AudioStreamIndex=%1").arg(audio >= 0 ? audio : 1)) : transcodeUrl(deviceId, burn, audio >= 0 ? audio : 1)},
                           {"RunTimeTicks", 72000000000LL},
                           {"Chapters", QJsonArray{QJsonObject{{"StartPositionTicks", 0}, {"Name", "Opening"}},
                                                   QJsonObject{{"StartPositionTicks", 6000000000LL}, {"Name", ""}},
                                                   QJsonObject{{"StartPositionTicks", 18000000000LL}, {"Name", "Finale"}}}},
                           {"MediaStreams", QJsonArray{QJsonObject{{"Index", 0}, {"Type", "Video"}, {"Codec", "hevc"}, {"DisplayTitle", "1080p"}},
                               QJsonObject{{"Index", 1}, {"Type", "Audio"}, {"Codec", "dts"}, {"DisplayTitle", "English DTS 5.1"}, {"IsDefault", true}},
                               QJsonObject{{"Index", 2}, {"Type", "Audio"}, {"Codec", "ac3"}, {"DisplayTitle", "Commentary"}}}}};
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

    // ---- the player's server calls (audit/JELLYFIN-PLAYER.md) ---------------------------------------------------------
    // A fake with a mutable state: home lists, played marks, session reports, chapter lookups. Every route is the one a
    // Jellyfin 10.9+ server answers; a flag turns routes off to test the silent fallbacks.
    struct Home { bool resumeNew = true, resumeLegacy = false, nextUp = true; int resumeStatus = 0; } home;
    QList<Req> reportsSeen, playedSeen;
    bool playedNewRoute = true;
    bool chaptersFail = false;
    QList<Req> flagsSeen;
    int flagStatus = 0;
    bool flagLegacyOnly = false;
    struct Latest { int status = 0; bool legacyOnly = false, noFavorites = false; } latest;
    struct NextUp { int status = 0; QStringList items; } nextUp;
    const QString user = QString::fromLatin1(kUser);
    const QJsonObject resumeItem{{"Id", "dddddddddddddddddddddddddddddd01"}, {"Name", "Half Watched"}, {"Type", "Episode"}, {"IsFolder", false},
        {"SeriesName", "Test Show"}, {"ParentIndexNumber", 2}, {"IndexNumber", 5}, {"RunTimeTicks", 36000000000LL},
        {"UserData", QJsonObject{{"Played", false}, {"PlaybackPositionTicks", 6000000000LL}}}};
    const QJsonObject latestItem{{"Id", "dddddddddddddddddddddddddddddd03"}, {"Name", "Fresh Episode"}, {"Type", "Episode"}, {"IsFolder", false}, {"SeriesId", "eeeeeeeeeeeeeeeeeeeeeeeeeeeeee01"},
        {"SeriesName", "Test Show"}, {"ParentIndexNumber", 2}, {"IndexNumber", 6}, {"RunTimeTicks", 36000000000LL}, {"UserData", QJsonObject{{"Played", true}, {"IsFavorite", true}, {"PlaybackPositionTicks", 0}}}};
    fake.handler = [&](const Req& r) -> Resp {
        const QString path = QString::fromLatin1(r.path);
        if (authToken(r) == kToken || r.query().queryItemValue("ApiKey").toLatin1() == kToken) {
            const QUrlQuery q = r.query();
            // Phase 2: Latest, Favourites and the user's flags.
            if (r.method == "GET" && path == QLatin1String("/Items/Latest")) {
                if (latest.status) return error(latest.status, "no");
                return latest.legacyOnly ? error(404, "Not found") : Resp{200, QJsonDocument(QJsonArray{latestItem}).toJson(QJsonDocument::Compact), "application/json", 0};
            }
            if (r.method == "GET" && path == QStringLiteral("/Users/%1/Items/Latest").arg(user))
                return latest.legacyOnly ? Resp{200, QJsonDocument(QJsonArray{latestItem}).toJson(QJsonDocument::Compact), "application/json", 0} : error(404, "Not found");
            if (r.method == "GET" && path == QLatin1String("/Items") && q.queryItemValue("Filters") == QLatin1String("IsFavorite"))
                return latest.noFavorites ? error(404, "Not found") : json({{"Items", QJsonArray{latestItem}}, {"TotalRecordCount", 1}});
            if ((r.method == "POST" || r.method == "DELETE") && (path.startsWith(QLatin1String("/UserFavoriteItems/")) || path.contains(QLatin1String("/FavoriteItems/")))) {
                flagsSeen.push_back(r);
                const bool legacy = path.startsWith(QLatin1String("/Users/"));
                if (flagStatus) return error(flagStatus, "boom");
                return (legacy != flagLegacyOnly) ? error(404, "Not found") : Resp{200, "{}", "application/json", 0};
            }
            if (r.method == "DELETE" && (path.startsWith(QLatin1String("/UserPlayedItems/")) || path.contains(QLatin1String("/PlayedItems/")))) {
                flagsSeen.push_back(r);
                const bool legacy = path.startsWith(QLatin1String("/Users/"));
                if (flagStatus) return error(flagStatus, "boom");
                return (legacy != flagLegacyOnly) ? error(404, "Not found") : Resp{200, "{}", "application/json", 0};
            }
            if (r.method == "POST" && flagStatus && (path.startsWith(QLatin1String("/UserPlayedItems/")) || path.contains(QLatin1String("/PlayedItems/")))) {
                flagsSeen.push_back(r);
                return error(flagStatus, "boom");
            }
            if (r.method == "POST" && flagLegacyOnly && path.startsWith(QLatin1String("/UserPlayedItems/"))) {
                flagsSeen.push_back(r);
                return error(404, "Not found");
            }
            if (r.method == "POST" && flagLegacyOnly && path.contains(QLatin1String("/PlayedItems/"))) {
                flagsSeen.push_back(r);
                return Resp{200, "{}", "application/json", 0};
            }
            if (r.method == "GET" && path == QLatin1String("/Shows/NextUp") && q.hasQueryItem("seriesId")) {
                if (nextUp.status) return error(nextUp.status, "no");
                QJsonArray list;
                for (const QString& itemId : nextUp.items)
                    list.push_back(QJsonObject{{"Id", itemId}, {"Name", "Next One"}, {"Type", "Episode"}, {"IsFolder", false}, {"SeriesId", q.queryItemValue("seriesId")},
                        {"SeriesName", "Test Show"}, {"ParentIndexNumber", 2}, {"IndexNumber", 6}, {"RunTimeTicks", 36000000000LL}, {"UserData", QJsonObject{{"Played", false}, {"PlaybackPositionTicks", 0}}}});
                return json({{"Items", list}, {"TotalRecordCount", int(list.size())}});
            }
            if (home.resumeStatus && (path == QLatin1String("/UserItems/Resume") || path == QStringLiteral("/Users/%1/Items/Resume").arg(user))) return error(home.resumeStatus, "boom");
            if (r.method == "GET" && path == QLatin1String("/UserItems/Resume"))
                return home.resumeNew ? json({{"Items", QJsonArray{resumeItem}}, {"TotalRecordCount", 1}}) : error(404, "Not found");
            if (r.method == "GET" && path == QStringLiteral("/Users/%1/Items/Resume").arg(user))
                return home.resumeLegacy ? json({{"Items", QJsonArray{resumeItem}}, {"TotalRecordCount", 1}}) : error(404, "Not found");
            if (r.method == "GET" && path == QLatin1String("/Shows/NextUp"))
                return home.nextUp ? json({{"Items", QJsonArray{resumeItem}}, {"TotalRecordCount", 1}}) : error(404, "Not found");
            if (r.method == "POST" && path.startsWith(QLatin1String("/Sessions/Playing"))) { reportsSeen.push_back(r); return {204, {}, "application/json", 0}; }
            if (r.method == "POST" && path.startsWith(QLatin1String("/UserPlayedItems/"))) {
                playedSeen.push_back(r);
                return playedNewRoute ? Resp{200, "{}", "application/json", 0} : error(404, "Not found");
            }
            if (r.method == "POST" && path.contains(QLatin1String("/PlayedItems/"))) { playedSeen.push_back(r); return {200, "{}", "application/json", 0}; }
            if (r.method == "GET" && path == QLatin1String("/Items") && q.hasQueryItem("ids")) {
                if (chaptersFail) return error(500, "boom");
                return json({{"Items", QJsonArray{QJsonObject{{"Id", q.queryItemValue("ids")},
                    {"Chapters", QJsonArray{QJsonObject{{"StartPositionTicks", 0}, {"Name", "One"}}, QJsonObject{{"StartPositionTicks", 30000000000LL}, {"Name", "Two"}}}}}}}});
            }
        }
        return serve(r, fake, transcodeRequested);
    };
    {
        // The home routes pin: 10.9+ path, user id and the video media type; NextUp has no older route.
        const QUrl resumeUrl = client.homeUrl(JellyfinClient::HomeList::Resume);
        check(resumeUrl.path() == QLatin1String("/UserItems/Resume"), "home: Continue Watching uses /UserItems/Resume");
        check(QUrlQuery(resumeUrl).queryItemValue("userId") == user && QUrlQuery(resumeUrl).queryItemValue("MediaTypes") == QLatin1String("Video") &&
              QUrlQuery(resumeUrl).queryItemValue("EnableUserData") == QLatin1String("true"), "home: the user, the video media type and the user data are requested");
        check(client.homeUrl(JellyfinClient::HomeList::Resume, true).path() == QStringLiteral("/Users/%1/Items/Resume").arg(user), "home: the older route is user-scoped");
        check(client.homeUrl(JellyfinClient::HomeList::NextUp).path() == QLatin1String("/Shows/NextUp") && QUrlQuery(client.homeUrl(JellyfinClient::HomeList::NextUp)).queryItemValue("userId") == user,
              "home: Next Up is /Shows/NextUp with the user id");
        check(client.homeUrl(JellyfinClient::HomeList::NextUp, true).isEmpty(), "home: Next Up has no older route");

        Collector pages, failures2;
        const auto pageConnection = QObject::connect(&client, &JellyfinClient::pageReady, [&](quint64 request, const JellyfinPage& page) { pages.v.push_back({QVariant::fromValue(request), QVariant::fromValue(page)}); });
        const auto failConnection = QObject::connect(&client, &JellyfinClient::failed, [&](quint64 request, const QString& message) { failures2.v.push_back({QVariant::fromValue(request), message}); });
        const auto firstPage = [&]() { return qvariant_cast<JellyfinPage>(pages.first().at(1)); };

        quint64 id = client.home(JellyfinClient::HomeList::Resume);
        check(waitFor([&] { return pages.count() > 0; }) && pages.first().at(0).toULongLong() == id, "home: Continue Watching answered under its own request id");
        if (pages.count() > 0 && !firstPage().items.isEmpty()) {
            const auto item = firstPage().items.first();
            check(item.resumeMs == 600000 && item.durationMs == 3600000 && !item.played, "home: the server's PlaybackPositionTicks is the resume point (100 ns ticks to ms)");
            check(item.seriesName == QLatin1String("Test Show") && item.seasonNumber == 2 && item.episodeNumber == 5, "home: an episode names its show, season and number");
        } else check(false, "home: the page carries the resume item");

        // 10.9+ route missing, older route present: the page arrives under the original request id.
        pages.clear(); home.resumeNew = false; home.resumeLegacy = true;
        id = client.home(JellyfinClient::HomeList::Resume);
        check(waitFor([&] { return pages.count() > 0; }) && pages.first().at(0).toULongLong() == id && firstPage().items.size() == 1, "home: a 404 on the new route falls back to the older one, same request id");
        // Neither route: an empty page, not an error.
        pages.clear(); failures2.clear(); home.resumeLegacy = false;
        id = client.home(JellyfinClient::HomeList::Resume);
        check(waitFor([&] { return pages.count() > 0 || failures2.count() > 0; }) && pages.count() == 1 && firstPage().items.isEmpty() && failures2.count() == 0,
              "home: a server without the route answers an empty list silently");
        pages.clear(); home.nextUp = false;
        client.home(JellyfinClient::HomeList::NextUp);
        check(waitFor([&] { return pages.count() > 0 || failures2.count() > 0; }) && pages.count() == 1 && firstPage().items.isEmpty() && failures2.count() == 0, "home: Next Up without the route is an empty list too");
        // A real server error is still an error the viewer can read.
        pages.clear(); failures2.clear(); home.resumeNew = true; home.resumeStatus = 500;
        client.home(JellyfinClient::HomeList::Resume);
        check(waitFor([&] { return pages.count() > 0 || failures2.count() > 0; }) && failures2.count() == 1 && pages.count() == 0, "home: an HTTP 500 is reported, not swallowed");
        home.resumeStatus = 0;
        QObject::disconnect(pageConnection); QObject::disconnect(failConnection);
    }
    {
        // Session reports: start, progress, stop; ticks are milliseconds times 10000 and the stream choice rides along.
        JellyfinPlayback p;
        p.itemId = QString::fromLatin1(kItem); p.mediaSourceId = p.itemId; p.playSessionId = QStringLiteral("ps0123456789abcdef"); p.playMethod = QStringLiteral("DirectPlay");
        p.audioStreamIndex = 2;
        reportsSeen.clear();
        client.reportPlayback(p, 61000, false, QStringLiteral("start"));
        client.reportPlayback(p, 71500, true, QStringLiteral("progress"));
        client.reportPlayback(p, 3600000, false, QStringLiteral("stop"));
        client.reportPlayback(p, 5, false, QStringLiteral("bogus"));
        check(waitFor([&] { return reportsSeen.size() >= 3; }), "report: start, progress and stop reach the server");
        if (reportsSeen.size() >= 3) {
            check(reportsSeen[0].path == "/Sessions/Playing" && reportsSeen[1].path == "/Sessions/Playing/Progress" && reportsSeen[2].path == "/Sessions/Playing/Stopped", "report: the three session routes");
            const QJsonObject start = QJsonDocument::fromJson(reportsSeen[0].body).object();
            const QJsonObject progress = QJsonDocument::fromJson(reportsSeen[1].body).object();
            const QJsonObject stopped = QJsonDocument::fromJson(reportsSeen[2].body).object();
            check(start.value("PositionTicks").toDouble() == 610000000.0 && progress.value("PositionTicks").toDouble() == 715000000.0 && stopped.value("PositionTicks").toDouble() == 36000000000.0,
                  "report: PositionTicks is the position in milliseconds times 10000");
            check(start.value("ItemId") == p.itemId && start.value("PlaySessionId") == p.playSessionId && start.value("PlayMethod") == QLatin1String("DirectPlay") && start.value("MediaSourceId") == p.itemId,
                  "report: the item, session, method and media source");
            check(!start.value("IsPaused").toBool() && progress.value("IsPaused").toBool(), "report: the paused flag follows the player");
            check(start.value("AudioStreamIndex").toInt() == 2 && !start.contains("SubtitleStreamIndex"), "report: the chosen audio stream is reported, no subtitle stream unless burned in");
        }
        check(reportsSeen.size() == 3, "report: an unknown event sends nothing");
        QUrlQuery cleanup;
        for (const Req& r : fake.log) if (r.method == "DELETE") cleanup = r.query();
        check(cleanup.queryItemValue("playSessionId") == QLatin1String("ps0123456789abcdef"), "report: the stop also releases the transcode");
    }
    {
        // Marking watched: the 10.9+ route with the user id; a 404 retries the older user-scoped route once.
        playedSeen.clear();
        client.markPlayed(QString::fromLatin1(kItem));
        check(waitFor([&] { return !playedSeen.isEmpty(); }), "played: the mark reaches the server");
        check(playedSeen.size() == 1 && playedSeen[0].method == "POST" && playedSeen[0].path == QByteArray("/UserPlayedItems/") + kItem &&
              playedSeen[0].query().queryItemValue("userId") == user, "played: POST /UserPlayedItems/{id}?userId=");
        playedSeen.clear(); playedNewRoute = false;
        client.markPlayed(QString::fromLatin1(kItem));
        check(waitFor([&] { return playedSeen.size() >= 2; }), "played: a 404 retries the older route");
        check(playedSeen.size() == 2 && playedSeen[1].path == QByteArray("/Users/") + kUser + "/PlayedItems/" + kItem, "played: the older route is /Users/{user}/PlayedItems/{id}");
        playedSeen.clear();
        client.markPlayed(QStringLiteral("../x"));
        waitFor([&] { return false; }, 150);
        check(playedSeen.isEmpty(), "played: an invalid item id is never sent");
        playedNewRoute = true;
    }
    {
        // An audio stream the player cannot switch to is the server's to select; chapters ride on PlaybackInfo.
        ready.clear(); failed.clear();
        client.playback(QString::fromLatin1(kItem), false, 20000, -1, {}, 2);
        check(waitFor([&] { return ready.count() + failed.count() > 0; }) && ready.count() == 1, "audio: PlaybackInfo with AudioStreamIndex accepted (direct play off)");
        if (ready.count() == 1) {
            const auto p = qvariant_cast<JellyfinPlayback>(ready.first().at(1));
            const QJsonObject body = QJsonDocument::fromJson(fake.log.last().body).object();
            check(body.value("AudioStreamIndex").toInt() == 2 && !body.value("EnableDirectPlay").toBool(), "audio: the request names stream 2 and disables direct play");
            check(p.audioStreamIndex == 2 && p.url.query(QUrl::FullyEncoded).count("AudioStreamIndex=2") == 1, "audio: the stream url carries AudioStreamIndex=2 once");
            check(p.durationMs == 7200000, "playback: the source's RunTimeTicks is the duration");
            check(p.chapters.size() == 3 && p.chapters[1].startMs == 600000 && p.chapters[0].name == QLatin1String("Opening") && p.chapters[1].name.isEmpty() && p.chapters[2].startMs == 1800000,
                  "playback: Chapters of the media source are parsed (ticks to ms, an unnamed mark stays unnamed)");
            int audioStreams = 0;
            for (const auto& s : p.streams) if (s.type == QLatin1String("Audio")) ++audioStreams;
            check(audioStreams == 2, "playback: both audio streams are listed");
        }
        // A server whose TranscodingUrl leaves the key out: the client adds it, once.
        g_fakeOmitsAudioIndex = true; ready.clear();
        client.playback(QString::fromLatin1(kItem), false, 20000, -1, {}, 2);
        check(waitFor([&] { return ready.count() > 0; }), "audio: answered when the url has no AudioStreamIndex");
        if (ready.count() > 0) check(qvariant_cast<JellyfinPlayback>(ready.first().at(1)).url.query(QUrl::FullyEncoded).count("AudioStreamIndex=2") == 1, "audio: the client adds the missing AudioStreamIndex, once");
        g_fakeOmitsAudioIndex = false;
        ready.clear();
        client.playback(QString::fromLatin1(kItem), false, 20000, -1);
        check(waitFor([&] { return ready.count() > 0; }), "audio: no stream chosen is still a plain request");
        if (ready.count() > 0) {
            const QJsonObject body = QJsonDocument::fromJson(fake.log.last().body).object();
            check(!body.contains("AudioStreamIndex") && body.value("EnableDirectPlay").toBool(), "audio: without a choice nothing is forced and direct play stays on");
            check(qvariant_cast<JellyfinPlayback>(ready.first().at(1)).audioStreamIndex == -1, "audio: no stream chosen reads -1");
        }
    }
    {
        // Chapters for a server whose PlaybackInfo had none.
        struct Got { quint64 request = 0; QString item; QVector<JellyfinChapter> list; bool done = false; } got;
        const auto chapterConnection = QObject::connect(&client, &JellyfinClient::chaptersReady, [&](quint64 request, const QString& item, const QVector<JellyfinChapter>& list) { got = {request, item, list, true}; });
        const quint64 id = client.chapters(QString::fromLatin1(kItem));
        check(waitFor([&] { return got.done; }) && got.request == id && got.item == QLatin1String(kItem) && got.list.size() == 2 && got.list[1].startMs == 3000000 && got.list[1].name == QLatin1String("Two"),
              "chapters: /Items?ids=&fields=Chapters is parsed");
        const auto& last = fake.log.last();
        check(last.path == "/Items" && last.query().queryItemValue("fields") == QLatin1String("Chapters") && last.query().queryItemValue("userId") == user, "chapters: the request asks for the Chapters field as the signed-in user");
        got = {}; chaptersFail = true;
        client.chapters(QString::fromLatin1(kItem));
        check(waitFor([&] { return got.done; }) && got.list.isEmpty(), "chapters: a failing server answers an empty list, silently");
        got = {}; chaptersFail = false;
        client.chapters(QStringLiteral("a/b"));
        check(waitFor([&] { return got.done; }) && got.list.isEmpty(), "chapters: an invalid id answers an empty list without a request");
        QObject::disconnect(chapterConnection);
    }

    {
        // Latest (per library, a bare JSON array) and Favourites.
        const QString library = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa01");
        const QUrl latestUrl = client.homeUrl(JellyfinClient::HomeList::Latest, false, library);
        check(latestUrl.path() == QLatin1String("/Items/Latest") && QUrlQuery(latestUrl).queryItemValue("ParentId") == library && QUrlQuery(latestUrl).queryItemValue("userId") == user,
              "latest: /Items/Latest for one library as the signed-in user");
        check(client.homeUrl(JellyfinClient::HomeList::Latest, true, library).path() == QStringLiteral("/Users/%1/Items/Latest").arg(user), "latest: the older route is user-scoped");
        check(!QUrlQuery(client.homeUrl(JellyfinClient::HomeList::Latest)).hasQueryItem("ParentId"), "latest: no library means no ParentId");
        check(!QUrlQuery(client.homeUrl(JellyfinClient::HomeList::Latest, false, QStringLiteral("a/b"))).hasQueryItem("ParentId"), "latest: an invalid library id is never sent");
        const QUrl favoriteUrl = client.homeUrl(JellyfinClient::HomeList::Favorites);
        check(favoriteUrl.path() == QLatin1String("/Items") && QUrlQuery(favoriteUrl).queryItemValue("Filters") == QLatin1String("IsFavorite") &&
              QUrlQuery(favoriteUrl).queryItemValue("Recursive") == QLatin1String("true") && QUrlQuery(favoriteUrl).queryItemValue("UserId") == user, "favourites: /Items with Filters=IsFavorite, recursive, as the user");
        check(client.homeUrl(JellyfinClient::HomeList::Favorites, true).isEmpty(), "favourites: no older route");

        Collector pages, failures2;
        const auto pageConnection = QObject::connect(&client, &JellyfinClient::pageReady, [&](quint64 request, const JellyfinPage& page) { pages.v.push_back({QVariant::fromValue(request), QVariant::fromValue(page)}); });
        const auto failConnection = QObject::connect(&client, &JellyfinClient::failed, [&](quint64 request, const QString& message) { failures2.v.push_back({QVariant::fromValue(request), message}); });
        const auto firstPage = [&]() { return qvariant_cast<JellyfinPage>(pages.first().at(1)); };

        quint64 id = client.home(JellyfinClient::HomeList::Latest, library);
        check(waitFor([&] { return pages.count() > 0; }) && pages.first().at(0).toULongLong() == id && firstPage().items.size() == 1, "latest: the array answer becomes a page under the request id");
        if (pages.count() > 0 && !firstPage().items.isEmpty()) {
            const auto item = firstPage().items.first();
            check(item.favorite && item.seriesId == QLatin1String("eeeeeeeeeeeeeeeeeeeeeeeeeeeeee01") && item.played, "latest: UserData.IsFavorite, Played and SeriesId are read");
        }
        check(fake.log.last().query().queryItemValue("ParentId") == library, "latest: the library id reached the server");
        pages.clear(); latest.legacyOnly = true;
        id = client.home(JellyfinClient::HomeList::Latest, library);
        check(waitFor([&] { return pages.count() > 0; }) && pages.first().at(0).toULongLong() == id && firstPage().items.size() == 1, "latest: a 404 falls back to the older route, same request id");
        check(fake.log.last().path == QByteArray("/Users/") + kUser + "/Items/Latest" && fake.log.last().query().queryItemValue("ParentId") == library, "latest: the retry keeps the library");
        latest.legacyOnly = false;
        pages.clear(); failures2.clear(); latest.status = 500;
        client.home(JellyfinClient::HomeList::Latest, library);
        check(waitFor([&] { return pages.count() > 0 || failures2.count() > 0; }) && failures2.count() == 1 && pages.count() == 0, "latest: an HTTP 500 is reported");
        latest.status = 0;
        pages.clear(); failures2.clear();
        client.home(JellyfinClient::HomeList::Favorites);
        check(waitFor([&] { return pages.count() > 0 || failures2.count() > 0; }) && pages.count() == 1 && firstPage().items.size() == 1 && firstPage().items.first().favorite, "favourites: the flagged items arrive with the favourite flag");
        pages.clear(); failures2.clear(); latest.noFavorites = true;
        client.home(JellyfinClient::HomeList::Favorites);
        check(waitFor([&] { return pages.count() > 0 || failures2.count() > 0; }) && pages.count() == 1 && firstPage().items.isEmpty() && failures2.count() == 0, "favourites: a server that cannot filter answers an empty list silently");
        latest.noFavorites = false;
        QObject::disconnect(pageConnection); QObject::disconnect(failConnection);
    }
    {
        // Watched and favourite toggles: POST sets, DELETE clears, the older route is tried once on a 404, the answer is surfaced.
        struct Got { quint64 request = 0; QString item; int flag = -1; bool value = false, ok = false; };
        QList<Got> got;
        const auto flagConnection = QObject::connect(&client, &JellyfinClient::userFlagChanged, [&](quint64 request, const QString& item, int flag, bool value, bool ok) { got.push_back({request, item, flag, value, ok}); });
        const QString item = QString::fromLatin1(kItem);
        flagsSeen.clear();
        quint64 id = client.setUserFlag(item, JellyfinClient::UserFlag::Favorite, true);
        check(waitFor([&] { return !got.isEmpty(); }) && got[0].request == id && got[0].ok && got[0].value && got[0].flag == int(JellyfinClient::UserFlag::Favorite) && got[0].item == item, "flag: a favourite mark is answered ok under its request id");
        check(flagsSeen.size() == 1 && flagsSeen[0].method == "POST" && flagsSeen[0].path == QByteArray("/UserFavoriteItems/") + kItem && flagsSeen[0].query().queryItemValue("userId") == user, "flag: POST /UserFavoriteItems/{id}?userId=");
        got.clear(); flagsSeen.clear();
        id = client.setUserFlag(item, JellyfinClient::UserFlag::Favorite, false);
        check(waitFor([&] { return !got.isEmpty(); }) && got[0].ok && !got[0].value && flagsSeen.size() == 1 && flagsSeen[0].method == "DELETE" && flagsSeen[0].path == QByteArray("/UserFavoriteItems/") + kItem, "flag: clearing a favourite is DELETE on the same route");
        got.clear(); flagsSeen.clear();
        id = client.setUserFlag(item, JellyfinClient::UserFlag::Played, false);
        check(waitFor([&] { return !got.isEmpty(); }) && got[0].ok && !got[0].value && got[0].flag == int(JellyfinClient::UserFlag::Played) && flagsSeen.size() == 1 && flagsSeen[0].method == "DELETE" &&
              flagsSeen[0].path == QByteArray("/UserPlayedItems/") + kItem && flagsSeen[0].query().queryItemValue("userId") == user, "flag: marking unwatched is DELETE /UserPlayedItems/{id}?userId=");
        got.clear(); flagsSeen.clear(); flagLegacyOnly = true;
        id = client.setUserFlag(item, JellyfinClient::UserFlag::Played, false);
        check(waitFor([&] { return !got.isEmpty(); }) && got.size() == 1 && got[0].request == id && got[0].ok && flagsSeen.size() == 2 && flagsSeen[1].method == "DELETE" &&
              flagsSeen[1].path == QByteArray("/Users/") + kUser + "/PlayedItems/" + kItem, "flag: a 404 retries the older user-scoped route once; one answer, the original request id");
        got.clear(); flagsSeen.clear();
        client.setUserFlag(item, JellyfinClient::UserFlag::Favorite, true);
        check(waitFor([&] { return !got.isEmpty(); }) && got[0].ok && flagsSeen.size() == 2 && flagsSeen[1].path == QByteArray("/Users/") + kUser + "/FavoriteItems/" + kItem, "flag: the favourite older route is /Users/{user}/FavoriteItems/{id}");
        flagLegacyOnly = false;
        got.clear(); flagsSeen.clear(); flagStatus = 500;
        client.setUserFlag(item, JellyfinClient::UserFlag::Played, true);
        check(waitFor([&] { return !got.isEmpty(); }) && got.size() == 1 && !got[0].ok && flagsSeen.size() == 1, "flag: an HTTP 500 is a failed answer with no retry (the row must not claim a change)");
        flagStatus = 0;
        got.clear(); flagsSeen.clear();
        client.setUserFlag(QStringLiteral("../x"), JellyfinClient::UserFlag::Played, true);
        check(waitFor([&] { return !got.isEmpty(); }) && !got[0].ok && flagsSeen.isEmpty(), "flag: an invalid id fails without sending anything");
        QObject::disconnect(flagConnection);
    }
    {
        // Play Next Episode: the watched mark first, then /Shows/NextUp?seriesId=, skipping the finished episode itself.
        struct Got { quint64 request = 0; JellyfinItem item; bool found = false, done = false; } got;
        const auto nextConnection = QObject::connect(&client, &JellyfinClient::nextEpisodeReady, [&](quint64 request, const JellyfinItem& item, bool found) { got = {request, item, found, true}; });
        const QString series = QStringLiteral("eeeeeeeeeeeeeeeeeeeeeeeeeeeeee01");
        const QUrl nextUrl = client.nextUpUrl(series);
        check(nextUrl.path() == QLatin1String("/Shows/NextUp") && QUrlQuery(nextUrl).queryItemValue("seriesId") == series && QUrlQuery(nextUrl).queryItemValue("userId") == user, "next: /Shows/NextUp scoped to the series and the user");
        flagsSeen.clear(); playedSeen.clear(); nextUp.items = {QStringLiteral("dddddddddddddddddddddddddddddd02")};
        const quint64 id = client.nextEpisode(series, QString::fromLatin1(kItem));
        check(waitFor([&] { return got.done; }) && got.request == id && got.found && got.item.id == QLatin1String("dddddddddddddddddddddddddddddd02") && got.item.seasonNumber == 2, "next: the server's next-up episode comes back under the request id");
        check(playedSeen.size() == 1 && playedSeen[0].path == QByteArray("/UserPlayedItems/") + kItem && playedSeen[0].method == "POST", "next: the finished episode is marked watched first");
        int markAt = -1, nextAt = -1;
        for (int i = 0; i < fake.log.size(); ++i) {
            if (fake.log[i].path == QByteArray("/UserPlayedItems/") + kItem) markAt = i;
            if (fake.log[i].path == "/Shows/NextUp" && fake.log[i].query().hasQueryItem("seriesId")) nextAt = i;
        }
        check(markAt >= 0 && nextAt > markAt, "next: NextUp is asked only after the watched mark was sent");
        // The server still returns the finished episode (its list not yet updated): not offered as its own successor.
        got = {}; nextUp.items = {QString::fromLatin1(kItem)};
        client.nextEpisode(series, QString::fromLatin1(kItem));
        check(waitFor([&] { return got.done; }) && !got.found, "next: the finished episode is never its own next episode");
        got = {}; nextUp.items = {};
        client.nextEpisode(series, QString::fromLatin1(kItem));
        check(waitFor([&] { return got.done; }) && !got.found, "next: a finished series offers nothing");
        got = {}; nextUp.status = 404; nextUp.items = {QStringLiteral("dddddddddddddddddddddddddddddd02")};
        client.nextEpisode(series, QString::fromLatin1(kItem));
        check(waitFor([&] { return got.done; }) && !got.found, "next: a server without the route offers nothing, silently");
        nextUp.status = 0;
        got = {};
        client.nextEpisode(QStringLiteral("a/b"), QString::fromLatin1(kItem));
        check(waitFor([&] { return got.done; }) && !got.found, "next: an invalid series id offers nothing without a request");
        QObject::disconnect(nextConnection);
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
