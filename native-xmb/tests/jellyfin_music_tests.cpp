// Jellyfin music (audit/JELLYFIN-MUSIC.md): the requests behind the Music column's lists (artists, albums, playlists,
// songs, recently added, random, an artist's albums, an album's and a playlist's tracks, search), paging, what the
// parser keeps of an artist / album / track, album covers and their cache, the audio PlaybackInfo and its direct play
// through the loopback relay, the reason an error carries, and that the TV page's search no longer returns music.
// Everything runs against an in-process stand-in server on 127.0.0.1; no real server or account is involved.
//   xmb-jellyfin-music-tests
#include "jellyfin_client.h"
#include "jellyfin_stream.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>
#include <cstdio>
#include <functional>

namespace {
int checks = 0, failures = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}

const QByteArray kToken = "standin-token-for-tests-only";
const QString kUser = QStringLiteral("0123456789abcdef0123456789abcdef");
const QString kLib = QStringLiteral("cccccccccccccccccccccccccccccc01");
const QString kArtist = QStringLiteral("dddddddddddddddddddddddddddddd01");
const QString kAlbum = QStringLiteral("eeeeeeeeeeeeeeeeeeeeeeeeeeeeee01");
const QString kTrack = QStringLiteral("ffffffffffffffffffffffffffffff01");
const QString kList = QStringLiteral("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa09");

struct Req {
    QByteArray method, path, rawQuery, body;
    QHash<QByteArray, QByteArray> headers;
    QUrlQuery query() const { return QUrlQuery(QString::fromLatin1(rawQuery)); }
};
struct Resp { int status = 200; QByteArray body, type = "application/json"; };

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
    Req last(const QByteArray& path) const {
        for (int i = int(log.size()) - 1; i >= 0; --i) if (log[i].path == path) return log[i];
        return {};
    }
    int count(const QByteArray& startsWith) const {
        int n = 0;
        for (const Req& r : log) if (r.path.startsWith(startsWith)) ++n;
        return n;
    }
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
        req.path = QUrl::fromPercentEncoding(q < 0 ? target : target.left(q)).toUtf8();
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
        QByteArray out = "HTTP/1.1 " + QByteArray::number(resp.status) + " X\r\nContent-Type: " + resp.type +
                         "\r\nContent-Length: " + QByteArray::number(resp.body.size()) + "\r\nConnection: close\r\n\r\n" + resp.body;
        socket->write(out);
        socket->disconnectFromHost();
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

QByteArray tokenOf(const Req& r) {
    const QByteArray header = r.headers.value("authorization");
    const int at = header.indexOf("Token=\"");
    if (at < 0) return {};
    const int from = at + 7;
    return header.mid(from, header.indexOf('"', from) - from);
}
Resp json(const QJsonValue& v, int status = 200) {
    return {status, v.isArray() ? QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact) : QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact), "application/json"};
}
Resp page(const QJsonArray& items, int total, int start = 0) {
    return json(QJsonObject{{"Items", items}, {"TotalRecordCount", total}, {"StartIndex", start}});
}
QByteArray pngBytes() {
    QImage image(64, 48, QImage::Format_RGB32);
    image.fill(QColor(30, 120, 200));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}
QJsonObject trackJson(int n, const QString& id = {}) {
    return QJsonObject{{"Id", id.isEmpty() ? QStringLiteral("f%1").arg(n, 31, 16, QLatin1Char('0')) : id}, {"Name", QStringLiteral("Track %1").arg(n)},
        {"Type", "Audio"}, {"IsFolder", false}, {"RunTimeTicks", 2400000000LL}, {"IndexNumber", n}, {"ParentIndexNumber", 1},
        {"Artists", QJsonArray{"Alpha Artist", "Guest Singer"}}, {"AlbumArtist", "Alpha Artist"}, {"Album", "First Album"},
        {"AlbumId", kAlbum}, {"AlbumPrimaryImageTag", "albumtag1"}, {"ProductionYear", 2019},
        {"UserData", QJsonObject{{"Played", false}, {"PlaybackPositionTicks", 0}}}};
}

int audioFetches = 0;
Resp serve(const Req& r) {
    const QString path = QString::fromLatin1(r.path);
    if (r.method == "POST" && path == QLatin1String("/Users/AuthenticateByName"))
        return json(QJsonObject{{"AccessToken", QString::fromLatin1(kToken)}, {"User", QJsonObject{{"Id", kUser}}}});
    if (tokenOf(r) != kToken) return json(QJsonObject{{"Message", "Unauthorized"}}, 401);
    const QUrlQuery q = r.query();
    if (path == QLatin1String("/UserViews"))
        return page(QJsonArray{QJsonObject{{"Id", kLib}, {"Name", "Music"}, {"Type", "CollectionFolder"}, {"IsFolder", true}, {"CollectionType", "music"}},
                               QJsonObject{{"Id", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa01"}, {"Name", "Movies"}, {"Type", "CollectionFolder"}, {"IsFolder", true}, {"CollectionType", "movies"}}}, 2);
    if (path == QLatin1String("/Artists/AlbumArtists"))
        return page(QJsonArray{QJsonObject{{"Id", kArtist}, {"Name", "Alpha Artist"}, {"Type", "MusicArtist"}, {"IsFolder", true},
                                           {"ImageTags", QJsonObject{{"Primary", "arttag"}}}}}, 1);
    if (path == QLatin1String("/Items") && q.queryItemValue("IncludeItemTypes") == QLatin1String("MusicAlbum"))
        return page(QJsonArray{QJsonObject{{"Id", kAlbum}, {"Name", "First Album"}, {"Type", "MusicAlbum"}, {"IsFolder", true}, {"AlbumArtist", "Alpha Artist"},
                                           {"ProductionYear", 2019}, {"ImageTags", QJsonObject{{"Primary", "albumtag1"}}}}}, 1);
    if (path == QLatin1String("/Items") && q.queryItemValue("IncludeItemTypes") == QLatin1String("Audio")) {
        const int start = q.queryItemValue("StartIndex").toInt();
        const int limit = q.queryItemValue("Limit").toInt();
        QJsonArray items;
        for (int i = 0; i < limit && start + i < 25000; ++i) items.push_back(trackJson(start + i + 1));
        return page(items, 25000, start);
    }
    if (path == QLatin1String("/Items") && q.queryItemValue("IncludeItemTypes") == QLatin1String("Playlist"))
        return page(QJsonArray{QJsonObject{{"Id", kList}, {"Name", "Road Trip"}, {"Type", "Playlist"}, {"IsFolder", true}, {"MediaType", "Audio"}}}, 1);
    if (path == QLatin1String("/Items") && q.queryItemValue("IncludeItemTypes").contains("MusicArtist"))
        return page(QJsonArray{trackJson(1)}, 1);
    if (path == QStringLiteral("/Playlists/%1/Items").arg(kList))
        return page(QJsonArray{trackJson(2), trackJson(1)}, 2);
    if (path == QLatin1String("/Items") && q.hasQueryItem("SearchTerm"))
        return page(QJsonArray{QJsonObject{{"Id", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbb01"}, {"Name", "Film"}, {"Type", "Movie"}}}, 1);
    if (path.startsWith(QLatin1String("/Items/")) && path.endsWith(QLatin1String("/Images/Primary")))
        return {200, pngBytes(), "image/png"};
    if (path == QStringLiteral("/Items/%1/PlaybackInfo").arg(kTrack)) {
        const QJsonObject body = QJsonDocument::fromJson(r.body).object();
        const QJsonObject profile = body.value("DeviceProfile").toObject();
        const QJsonArray direct = profile.value("DirectPlayProfiles").toArray();
        if (direct.isEmpty() || direct.first().toObject().value("Type") != "Audio") return json(QJsonObject{{"Message", "an audio direct play profile is required"}}, 400);
        const QString containers = direct.first().toObject().value("Container").toString();
        if (!containers.contains("wav") || !containers.contains("flac") || !containers.contains("mp3")) return json(QJsonObject{{"Message", "containers"}}, 400);
        const QJsonObject source{{"Id", kTrack}, {"SupportsDirectPlay", true}, {"SupportsDirectStream", true}, {"SupportsTranscoding", true}, {"Container", "wav"},
            {"MediaStreams", QJsonArray{QJsonObject{{"Index", 0}, {"Type", "Audio"}, {"Codec", "pcm_s16le"}, {"DisplayTitle", "WAV"}}}}};
        return json(QJsonObject{{"MediaSources", QJsonArray{source}}, {"PlaySessionId", "ps0123456789abcdef"}});
    }
    if (path == QStringLiteral("/Audio/%1/stream").arg(kTrack)) {
        ++audioFetches;
        if (r.query().queryItemValue("static") != QLatin1String("true")) return json(QJsonObject{{"Message", "static required"}}, 400);
        return {200, QByteArray(4096, 'W'), "audio/wav"};
    }
    return json(QJsonObject{{"Message", "Not found"}}, 404);
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace xmb;
    using ML = JellyfinClient::MusicList;

    FakeServer fake;
    check(fake.listening, "stand-in server listens");
    fake.handler = [&](const Req& r) { return serve(r); };

    JellyfinClient client;
    QTemporaryDir cache;
    client.setImageCache(cache.path());
    struct Got { quint64 request; JellyfinPage page; };
    QList<Got> pages;
    QStringList errors;
    int authenticated = 0;
    QList<QImage> covers;
    QObject::connect(&client, &JellyfinClient::authenticated, [&] { ++authenticated; });
    QObject::connect(&client, &JellyfinClient::pageReady, [&](quint64 request, const JellyfinPage& p) { pages.push_back({request, p}); });
    QObject::connect(&client, &JellyfinClient::failed, [&](quint64, const QString& m) { errors.push_back(m); });
    QObject::connect(&client, &JellyfinClient::artworkReady, [&](quint64, const QString&, bool, const QImage& image) { covers.push_back(image); });
    client.signIn(fake.url(), QStringLiteral("tester"), QStringLiteral("not-a-real-password"));
    check(waitFor([&] { return authenticated > 0; }), "sign-in against the stand-in");

    const auto ask = [&](ML kind, const QString& library, const QString& id, int start, const char* label) -> JellyfinPage {
        pages.clear();
        client.music(kind, library, id, start);
        check(waitFor([&] { return !pages.isEmpty() || !errors.isEmpty(); }), label);
        return pages.isEmpty() ? JellyfinPage{} : pages.first().page;
    };

    // ---- the libraries the root page filters: music only --------------------------------------------------------------
    {
        pages.clear();
        client.views();
        check(waitFor([&] { return !pages.isEmpty(); }), "views answered");
        int music = 0;
        for (const auto& item : pages.first().page.items) if (item.collectionType == QLatin1String("music")) ++music;
        check(pages.first().page.items.size() == 2 && music == 1, "views: the server lists both libraries; the Music one is identifiable by CollectionType");
    }

    // ---- artists --------------------------------------------------------------------------------------------------------
    {
        const auto p = ask(ML::Artists, kLib, {}, 0, "artists answered");
        const Req r = fake.last("/Artists/AlbumArtists");
        check(r.query().queryItemValue("UserId") == kUser && r.query().queryItemValue("ParentId") == kLib, "artists: user scoped and limited to the library");
        check(r.query().queryItemValue("SortBy") == QLatin1String("SortName") && r.query().queryItemValue("SortOrder") == QLatin1String("Ascending"), "artists: sorted by SortName");
        check(r.query().queryItemValue("Limit") == QLatin1String("200") && r.query().queryItemValue("StartIndex") == QLatin1String("0"), "artists: a 200-row window");
        check(!r.rawQuery.contains("Token") && !r.rawQuery.contains(kToken), "artists: no credential in the url");
        check(r.headers.value("user-agent") == "XMB/1.0", "artists: the XMB user agent");
        check(p.items.size() == 1 && p.items[0].type == QLatin1String("MusicArtist") && p.items[0].title == QLatin1String("Alpha Artist") && p.items[0].primaryTag == QLatin1String("arttag"),
              "artists: parsed with the picture tag");
    }
    // ---- albums, recent, random ---------------------------------------------------------------------------------------
    {
        const auto p = ask(ML::Albums, kLib, {}, 0, "albums answered");
        const Req r = fake.last("/Items");
        check(r.query().queryItemValue("IncludeItemTypes") == QLatin1String("MusicAlbum") && r.query().queryItemValue("Recursive") == QLatin1String("true") &&
              r.query().queryItemValue("ParentId") == kLib && r.query().queryItemValue("SortBy") == QLatin1String("SortName"), "albums: request");
        check(p.items.size() == 1 && p.items[0].artist == QLatin1String("Alpha Artist") && p.items[0].year == 2019, "albums: artist and year parsed");
        ask(ML::Recent, kLib, {}, 0, "recent answered");
        check(fake.last("/Items").query().queryItemValue("SortBy").startsWith("DateCreated") && fake.last("/Items").query().queryItemValue("SortOrder") == QLatin1String("Descending"), "recent: newest first");
        ask(ML::Random, kLib, {}, 0, "random answered");
        check(fake.last("/Items").query().queryItemValue("SortBy") == QLatin1String("Random") && fake.last("/Items").query().queryItemValue("Limit") == QLatin1String("50"), "random: one page of fifty");
        ask(ML::ArtistAlbums, kLib, kArtist, 0, "artist albums answered");
        check(fake.last("/Items").query().queryItemValue("ArtistIds") == kArtist && fake.last("/Items").query().queryItemValue("IncludeItemTypes") == QLatin1String("MusicAlbum"), "artist: albums by ArtistIds");
    }
    // ---- playlists and their tracks -----------------------------------------------------------------------------------
    {
        const auto p = ask(ML::Playlists, kLib, {}, 0, "playlists answered");
        const Req r = fake.last("/Items");
        check(r.query().queryItemValue("MediaTypes") == QLatin1String("Audio") && !r.query().hasQueryItem("ParentId"), "playlists: audio playlists, server wide (they are not inside a music library)");
        check(p.items.size() == 1 && p.items[0].type == QLatin1String("Playlist"), "playlists: parsed");
        const auto t = ask(ML::PlaylistTracks, {}, kList, 0, "playlist tracks answered");
        check(fake.last(QStringLiteral("/Playlists/%1/Items").arg(kList).toLatin1()).query().queryItemValue("UserId") == kUser &&
              !fake.last(QStringLiteral("/Playlists/%1/Items").arg(kList).toLatin1()).query().hasQueryItem("SortBy"), "playlist: its own order, user scoped");
        check(t.items.size() == 2 && t.items[0].trackNumber == 2 && t.items[1].trackNumber == 1, "playlist: the server's order is kept, not re-sorted");
    }
    // ---- an album's tracks and what a track carries ----------------------------------------------------------------------
    {
        const auto p = ask(ML::AlbumTracks, {}, kAlbum, 0, "album tracks answered");
        const Req r = fake.last("/Items");
        check(r.query().queryItemValue("ParentId") == kAlbum && r.query().queryItemValue("IncludeItemTypes") == QLatin1String("Audio") &&
              r.query().queryItemValue("SortBy") == QLatin1String("ParentIndexNumber,IndexNumber,SortName"), "album: disc and track order");
        check(!p.items.isEmpty(), "album: tracks returned");
        if (!p.items.isEmpty()) {
            const auto& t = p.items[0];
            check(t.type == QLatin1String("Audio") && t.artist == QLatin1String("Alpha Artist, Guest Singer"), "track: every performer listed");
            check(t.album == QLatin1String("First Album") && t.albumId == kAlbum && t.albumTag == QLatin1String("albumtag1"), "track: album, album id and cover tag");
            check(t.trackNumber == 1 && t.discNumber == 1 && t.year == 2019, "track: track, disc, year");
            check(t.durationMs == 240000 && t.resumeMs == 0, "track: 4:00 and no resume point");
        }
    }
    // ---- Play All: an artist's songs, album by album; an album's and a playlist's tracks are the lists above --------------------------
    {
        const auto p = ask(ML::ArtistTracks, kLib, kArtist, 0, "artist tracks answered");
        const Req r = fake.last("/Items");
        check(r.query().queryItemValue("ArtistIds") == kArtist && r.query().queryItemValue("IncludeItemTypes") == QLatin1String("Audio") && r.query().queryItemValue("Recursive") == QLatin1String("true"), "artist tracks: songs by ArtistIds, recursive");
        check(r.query().queryItemValue("SortBy") == QLatin1String("Album,ParentIndexNumber,IndexNumber,SortName") && r.query().queryItemValue("SortOrder") == QLatin1String("Ascending"), "artist tracks: album by album, disc and track order");
        check(r.query().queryItemValue("UserId") == kUser && r.query().queryItemValue("Limit") == QLatin1String("200") && !r.rawQuery.contains(kToken), "artist tracks: user scoped, a 200-row window, no credential in the url");
        check(!p.items.isEmpty() && p.items[0].type == QLatin1String("Audio"), "artist tracks: tracks parsed");
        ask(ML::ArtistTracks, kLib, kArtist, 200, "artist tracks window 2 answered");
        check(fake.last("/Items").query().queryItemValue("StartIndex") == QLatin1String("200"), "artist tracks: the next window asks StartIndex=200");
        errors.clear();
        const int before = int(fake.log.size());
        client.music(ML::ArtistTracks, kLib, {});
        client.music(ML::ArtistTracks, kLib, QStringLiteral("../x"));
        check(waitFor([&] { return errors.size() >= 2; }) && int(fake.log.size()) == before, "artist tracks: a missing or invalid artist id is refused before anything is sent");
        errors.clear();
    }
    // ---- a very large library is paged, never read whole -----------------------------------------------------------------
    {
        QElapsedTimer timer; timer.start();
        const auto p = ask(ML::Songs, kLib, {}, 0, "songs page 1 answered");
        check(p.items.size() == 200 && p.total == 25000, "songs: 200 rows of 25000");
        const auto p2 = ask(ML::Songs, kLib, {}, 200, "songs page 2 answered");
        check(fake.last("/Items").query().queryItemValue("StartIndex") == QLatin1String("200"), "songs: the next window asks StartIndex=200");
        check(p2.items.size() == 200 && p2.items.first().trackNumber == 201 && p2.startIndex == 200, "songs: the second window starts where the first ended");
        check(timer.elapsed() < 5000, "songs: two windows of a 25000-song library are fast");
    }
    // ---- search ---------------------------------------------------------------------------------------------------------------
    {
        ask(ML::Search, kLib, QStringLiteral("alpha"), 0, "music search answered");
        const Req r = fake.last("/Items");
        check(r.query().queryItemValue("SearchTerm") == QLatin1String("alpha") && r.query().queryItemValue("IncludeItemTypes") == QLatin1String("MusicArtist,MusicAlbum,Audio"), "music search: artists, albums and songs");
        pages.clear();
        client.search(QStringLiteral("alpha"));
        check(waitFor([&] { return !pages.isEmpty(); }), "tv search answered");
        const QString types = fake.last("/Items").query().queryItemValue("IncludeItemTypes");
        check(!types.contains("Audio") && types.contains("Movie") && types.contains("Episode"), "tv search: video only, music is searched from the Music column");
    }
    // ---- invalid input never reaches the server ------------------------------------------------------------------------------
    {
        errors.clear();
        const int before = int(fake.log.size());
        client.music(ML::AlbumTracks, {}, QStringLiteral("../x"));
        client.music(ML::Artists, QStringLiteral("a/b"));
        client.music(ML::Search, {}, QStringLiteral("   "));
        client.music(ML::Songs, {}, {}, -1);
        check(waitFor([&] { return errors.size() >= 4; }), "invalid ids, library, term and offset are refused");
        check(int(fake.log.size()) == before, "invalid requests are not sent");
    }
    // ---- covers: the Primary picture, bounded, cached -----------------------------------------------------------------------------
    {
        covers.clear();
        client.artwork(kAlbum, QStringLiteral("albumtag1"), false);
        check(waitFor([&] { return !covers.isEmpty(); }), "cover fetched");
        check(!covers.isEmpty() && !covers.first().isNull() && covers.first().width() == 64, "cover decoded");
        const Req r = fake.last(QStringLiteral("/Items/%1/Images/Primary").arg(kAlbum).toLatin1());
        check(r.query().queryItemValue("tag") == QLatin1String("albumtag1") && r.query().queryItemValue("maxHeight") == QLatin1String("480"), "cover: the tag and a bounded size");
        const int fetched = fake.count("/Items/" + kAlbum.toLatin1() + "/Images/Primary");
        covers.clear();
        client.artwork(kAlbum, QStringLiteral("albumtag1"), false);
        check(waitFor([&] { return !covers.isEmpty(); }) && !covers.first().isNull(), "cover: second ask answered");
        check(fake.count("/Items/" + kAlbum.toLatin1() + "/Images/Primary") == fetched, "cover: the second ask is served from the disk cache, not the server");
        covers.clear();
        client.artwork(kAlbum, QStringLiteral("albumtag2"), false);
        check(waitFor([&] { return !covers.isEmpty(); }), "cover: a changed tag is answered");
        check(fake.count("/Items/" + kAlbum.toLatin1() + "/Images/Primary") == fetched + 1, "cover: a changed tag is fetched again");
    }
    // ---- audio PlaybackInfo, direct play, the loopback relay -------------------------------------------------------------------------
    {
        QList<JellyfinPlayback> ready;
        QObject::connect(&client, &JellyfinClient::playbackReady, [&](quint64, const JellyfinPlayback& p) { ready.push_back(p); });
        errors.clear();
        client.playback(kTrack, true, 20000, -1);
        check(waitFor([&] { return !ready.isEmpty() || !errors.isEmpty(); }), "audio: PlaybackInfo answered");
        check(ready.size() == 1 && errors.isEmpty(), "audio: PlaybackInfo accepted (an Audio direct play profile with wav, flac and mp3)");
        if (ready.size() == 1) {
            const auto& p = ready.first();
            check(p.audio && p.playMethod == QLatin1String("DirectPlay"), "audio: the original file is direct played");
            check(p.url.path() == QStringLiteral("/Audio/%1/stream").arg(kTrack) && p.url.query().contains("static=true"), "audio: the static stream route");
            check(!p.url.toString().contains(QString::fromLatin1(kToken)), "audio: no token in the url handed to the player");
            JellyfinStream stream;
            const QUrl local = stream.open(p.url, client.authorizationHeader());
            QNetworkAccessManager network;
            QNetworkReply* reply = network.get(QNetworkRequest(local));
            bool done = false;
            QObject::connect(reply, &QNetworkReply::finished, [&] { done = true; });
            waitFor([&] { return done; }, 15000);
            check(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200 && reply->readAll().size() == 4096, "audio: the relay delivers the file with the Authorization header added upstream");
            check(audioFetches == 1 && tokenOf(fake.last(QStringLiteral("/Audio/%1/stream").arg(kTrack).toLatin1())) == kToken, "audio: the upstream request was authorised by header");
            reply->deleteLater();
            stream.close();
        }
    }
    // ---- audio reports: start, progress, stopped (what keeps "recently played" and play counts right) ---------------------------------
    {
        JellyfinPlayback audio;
        audio.audio = true; audio.itemId = kTrack; audio.mediaSourceId = kTrack; audio.playSessionId = QStringLiteral("ps0123456789abcdef");
        audio.playMethod = QStringLiteral("DirectPlay");
        const int before = int(fake.log.size());
        client.reportPlayback(audio, 0, false, QStringLiteral("start"));
        client.reportPlayback(audio, 10000, false, QStringLiteral("progress"));
        client.reportPlayback(audio, 10000, true, QStringLiteral("progress"));
        client.reportPlayback(audio, 240000, false, QStringLiteral("stop"));   // a finished 4:00 song is reported at its end
        check(waitFor([&] { return fake.count("/Sessions/Playing") >= 4; }), "audio reports: four requests reach the server");
        const auto body = [&](const char* path, int nth) {
            int seen = 0;
            for (int i = before; i < fake.log.size(); ++i) if (fake.log[i].path == path && seen++ == nth) return QJsonDocument::fromJson(fake.log[i].body).object();
            return QJsonObject{};
        };
        const auto start = body("/Sessions/Playing", 0), progress = body("/Sessions/Playing/Progress", 0), paused = body("/Sessions/Playing/Progress", 1), stop = body("/Sessions/Playing/Stopped", 0);
        check(start.value("ItemId").toString() == kTrack && start.value("PositionTicks").toInteger() == 0 && start.value("PlayMethod").toString() == QLatin1String("DirectPlay"), "audio reports: start names the item, position 0 and the play method");
        check(progress.value("PositionTicks").toInteger() == 100000000 && !progress.value("IsPaused").toBool(), "audio reports: progress carries the position in ticks");
        check(paused.value("IsPaused").toBool(), "audio reports: a pause is a progress report with IsPaused");
        check(stop.value("PositionTicks").toInteger() == 2400000000LL && !stop.value("IsPaused").toBool(), "audio reports: a finished song stops at its end (so the server counts it played)");
        check(start.value("PlaySessionId").toString() == QLatin1String("ps0123456789abcdef") && stop.value("MediaSourceId").toString() == kTrack, "audio reports: the play session and media source ride along");
        bool leaked = false;
        for (int i = before; i < fake.log.size(); ++i) if (fake.log[i].rawQuery.contains(kToken) || fake.log[i].body.contains(kToken)) leaked = true;
        check(!leaked, "audio reports: no credential in a url or a body");
        check(tokenOf(fake.last("/Sessions/Playing/Stopped")) == kToken, "audio reports: authorised by header");
    }
    // ---- the reason an error carries --------------------------------------------------------------------------------------------------
    {
        const auto failWith = [&](int status, const QByteArray& body) {
            fake.handler = [&, status, body](const Req& r) {
                if (tokenOf(r) != kToken) return Resp{401, "{}"};
                return Resp{status, body, "application/json"};
            };
            errors.clear();
            client.music(ML::Albums, kLib);
            waitFor([&] { return !errors.isEmpty(); }, 8000);
            return errors.isEmpty() ? QString{} : errors.first();
        };
        const QString e500 = failWith(500, "{\"Message\":\"Error processing request.\"}");
        check(e500.contains("HTTP 500") && e500.contains("Error processing request."), "error: a server fault names the status and the server's own words");
        check(!e500.contains("{") && !e500.contains("Message"), "error: a JSON error body is read for its text, not shown raw");
        const QString e401 = failWith(403, "{}");
        check(e401.contains("HTTP 403") && e401.contains("sign in again", Qt::CaseInsensitive), "error: a refusal says to sign in again");
        const QString e404 = failWith(404, "{\"Message\":\"Item not found\"}");
        check(e404.contains("HTTP 404") && e404.contains("moved or removed"), "error: a missing item says so");
        check(!e500.contains(QString::fromLatin1(kToken)) && !e500.contains(kUser), "error: no credential, no id");
        fake.handler = [&](const Req& r) { return serve(r); };
    }
    {
        // Nothing listening: the network's reason, not a generic sentence.
        JellyfinClient lonely;
        int signed_ = 0;
        QStringList lonelyErrors;
        QObject::connect(&lonely, &JellyfinClient::authenticated, [&] { ++signed_; });
        QObject::connect(&lonely, &JellyfinClient::failed, [&](quint64, const QString& m) { lonelyErrors.push_back(m); });
        lonely.signIn(fake.url(), QStringLiteral("tester"), QStringLiteral("not-a-real-password"));
        waitFor([&] { return signed_ > 0; });
        fake.handler = nullptr;
        // The server is still up but answers 404 to everything; closing it is not needed to see a non-JSON reply path.
        lonely.music(ML::Albums);
        waitFor([&] { return !lonelyErrors.isEmpty(); }, 8000);
        check(!lonelyErrors.isEmpty() && lonelyErrors.first().contains("HTTP"), "error: a reply without the expected body still names its status");
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
