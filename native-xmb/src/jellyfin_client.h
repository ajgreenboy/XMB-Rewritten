#pragma once

#include <QObject>
#include <QUrl>
#include <QString>
#include <QVector>
#include <QByteArray>
#include <QImage>
#include <QSet>
#include <QHash>
#include <QtGlobal>

class QNetworkAccessManager;
class QNetworkReply;

namespace xmb {
struct JellyfinItem {
    QString id, title, type, overview;
    QString collectionType;   // a library's kind ("movies", "tvshows", "music", ...); empty on items
    bool folder = false;
    qint64 durationMs = 0, resumeMs = 0;
    bool played = false;
    // The server's image tags, for the artwork cache key: a changed poster gets a new file
    // instead of serving the previous one forever.
    QString primaryTag, backdropTag;
    // Music (Audio, MusicAlbum, MusicArtist, Playlist items): the performer line, the album an Audio item
    // belongs to and that album's picture tag (a track has no cover of its own), its place on the disc.
    QString artist, album, albumId, albumTag;
    int trackNumber = 0, discNumber = 0, year = 0;
    // Episodes: the show, and the season and episode numbers (0 when the server sent none), for the Continue
    // Watching and Next Up rows, which mix shows.
    QString seriesName;
    int seasonNumber = 0, episodeNumber = 0;
    // The episode's series (SeriesId), for the Play Next Episode prompt, and the user's favourite flag (UserData.IsFavorite).
    QString seriesId;
    bool favorite = false;
    int playCount = 0;   // UserData.PlayCount (the Most Played list)
};
struct JellyfinPage { QVector<JellyfinItem> items; int startIndex = 0, total = 0; };
// One MediaStream from PlaybackInfo. The player cannot necessarily switch to it; the shell asks
// QMediaPlayer what it reports and disables the rest with the reason, rather than pretending.
struct JellyfinTrack {
    int index = -1;
    QString type;      // "Video", "Audio", "Subtitle", "EmbeddedImage"
    QString title;     // DisplayTitle, else Language, else Codec
    QString language, codec;
    int channels = 0;
    bool external = false;
    bool text = false;      // IsTextSubtitleStream: the server can deliver it as SRT (subtitle())
    bool isDefault = false;
};
// One chapter mark of an item: where it starts and its name (empty when the server has none).
struct JellyfinChapter {
    qint64 startMs = 0;
    QString name;
};
// One line of a song's lyrics: when it starts (ms from the beginning of the track) and its text (empty for an instrumental gap).
struct JellyfinLyricLine {
    qint64 startMs = 0;
    QString text;
};
struct JellyfinPlayback {
    QUrl url;
    QString itemId, mediaSourceId, playSessionId, playMethod;
    bool audio = false;
    int burnSubtitleIndex = -1;   // the subtitle stream the server burns into this transcode, -1 = none
    int audioStreamIndex = -1;    // the audio stream asked for (AudioStreamIndex), -1 = the server's default
    qint64 durationMs = 0;        // the source's RunTimeTicks, 0 when the server did not say
    QVector<JellyfinChapter> chapters;   // MediaSources[].Chapters when PlaybackInfo carries them (else chapters())
    QVector<JellyfinTrack> streams;
};
// Whether a subtitle stream is English. The language field is Jellyfin's ISO 639-2 code (usually
// "eng"); the title is DisplayTitle (e.g. "English (Forced)"), so both are consulted. Covers text
// and image (PGS) subtitle streams alike.
inline bool isEnglishSubtitle(const JellyfinTrack& track) {
    const QString lang=track.language.trimmed().toLower();
    const QString title=track.title.trimmed().toLower();
    if(lang==QLatin1String("eng") || lang==QLatin1String("en"))return true;
    if(lang.startsWith(QLatin1String("en-")))return true;
    return title.contains(QLatin1String("english"));
}

class JellyfinClient final : public QObject {
    Q_OBJECT
public:
    explicit JellyfinClient(QObject* parent = nullptr);
    ~JellyfinClient() override;
    void signIn(QUrl server, QString user, QString password);
    void signOut();
    bool signedIn() const;
    QUrl serverUrl() const;
    QByteArray authorizationHeader() const;
    QString userId() const;
    quint64 views();
    quint64 items(QString parentId, int startIndex = 0);
    // A free-text library search: the same user-scoped /Items route with SearchTerm and
    // Recursive=true, so results carry the same UserData (resume/played) as a folder listing.
    quint64 search(QString term, int startIndex = 0);
    // The lists of the Music column (audit/JELLYFIN-MUSIC.md). Every one is a paged, user-scoped request answered
    // through pageReady like items(): Artists = the album artists, Albums, Songs, Playlists (audio playlists),
    // Recent = albums by date added, Random = a random page of albums, ArtistAlbums (id = the artist),
    // AlbumTracks (id = the album, disc and track order), PlaylistTracks (id = the playlist, its own order), Search
    // (id = the term), ArtistTracks (id = the artist: every song they perform, album by album, for "Play All"). libraryId, when
    // given, limits a list to that music library. Favorites = songs and albums flagged favourite (Filters=IsFavorite), RecentlyPlayed =
    // songs by DatePlayed (Filters=IsPlayed), MostPlayed = songs by PlayCount (Filters=IsPlayed).
    enum class MusicList { Artists, Albums, Songs, Playlists, Recent, Random, ArtistAlbums, AlbumTracks, PlaylistTracks, Search, ArtistTracks,
                          Favorites, RecentlyPlayed, MostPlayed };
    quint64 music(MusicList kind, QString libraryId = {}, QString id = {}, int startIndex = 0);
    // maxBitrateKbps caps what the server may send: above the file's bitrate it direct-plays, below it the
    // server transcodes down (HLS) to fit.
    // burnSubtitleIndex >= 0 asks for that subtitle stream burned into a server transcode
    // (SubtitleStreamIndex + a SubtitleProfile whose method is Encode; direct play is switched off).
    quint64 playback(QString itemId, bool audio = false, int maxBitrateKbps = 20000,
                     int burnSubtitleIndex = -1, QString burnCodec = {}, int audioStreamIndex = -1);
    // The server-side lists behind the Jellyfin home rows (audit/JELLYFIN-PLAYER.md). Resume = Continue Watching
    // (items with a saved position, newest first), NextUp = the next unwatched episode of every show being watched.
    // Answered through pageReady like items(); a server without the route answers an empty page, not an error.
    // Latest = /Items/Latest (the newest additions; libraryId limits it to one library), Favorites = the items flagged
    // favourite (Filters=IsFavorite). The numbers are the request's context codes, so the order is fixed.
    enum class HomeList { Resume, NextUp, Latest, Favorites };
    quint64 home(HomeList kind, QString libraryId = {});
    // Mark an item watched (clears its resume point server-side). Best effort, no answer is surfaced.
    void markPlayed(QString itemId);
    // The user's own flags on an item: watched (POST/DELETE /UserPlayedItems/{id}) and favourite
    // (POST/DELETE /UserFavoriteItems/{id}), each with the pre-10.9 /Users/{user}/... route retried once on a 404. Unlike
    // markPlayed the answer is surfaced (userFlagChanged), so the row only changes when the server agreed.
    enum class UserFlag { Played, Favorite };
    quint64 setUserFlag(QString itemId, UserFlag flag, bool value);
    QUrl userFlagUrl(UserFlag flag, const QString& itemId, bool legacyRoute = false) const;
    // The episode that follows `currentId` in its series: the watched mark for currentId is sent first and, once the server
    // has answered it, /Shows/NextUp?seriesId= is asked, so the server's own "next unwatched" rule decides. Answers through
    // nextEpisodeReady; found=false (nothing next, no route, any failure) is silent and means "do not prompt".
    quint64 nextEpisode(QString seriesId, QString currentId);
    QUrl nextUpUrl(const QString& seriesId) const;
    // The item's chapter marks, for a server whose PlaybackInfo did not carry them. Answers through chaptersReady with
    // an empty list when there are none or the request fails.
    quint64 chapters(QString itemId);
    // A song's lyrics: GET /Audio/{itemId}/Lyrics (Jellyfin 10.9+). Answers through lyricsReady; a server without the route, a
    // track without lyrics or any failure answers an empty list (nothing is shown, no error). `synced` is true only when every
    // line carries a start time: unsynced text is returned as read but the Now Playing page shows synced lyrics only.
    quint64 lyrics(QString itemId);
    // The URL a home list is asked from; exposed so the tests can pin the route and its query.
    QUrl homeUrl(HomeList kind, bool legacyRoute = false, const QString& libraryId = {}) const;
    // Primary (poster) or Backdrop art for one item, authenticated, bounded, and cached under
    // the directory set by setImageCache(). A failure emits artworkReady with a null image:
    // missing art is not an error the user needs to see.
    quint64 artwork(QString itemId, QString tag, bool backdrop = false);
    void setImageCache(QString directory);
    // A text subtitle stream of the playing item, converted by the server to SRT:
    // GET /Videos/{itemId}/{mediaSourceId}/Subtitles/{streamIndex}/Stream.srt. Bounded like every
    // response; subtitleReady carries empty bytes when it fails.
    quint64 subtitle(QString itemId, QString mediaSourceId, int streamIndex);
    // An image-based (PGS) stream in its own format: GET .../Subtitles/{streamIndex}/Stream.sup.
    // Bounded at 64 MB with a 120 s deadline; answers through subtitleReady like subtitle().
    quint64 subtitleImage(QString itemId, QString mediaSourceId, int streamIndex);
    void reportPlayback(const JellyfinPlayback& playback, qint64 positionMs, bool paused, QString event);
    // Diagnostics for a stream the player could not open: the same URL, asked for with the same headers (the
    // Authorization header, the XMB user agent), one GET capped at a few KB with a short deadline. The answer
    // comes through probeFinished with the HTTP status (0: no answer) and a scrubbed digest of the first bytes
    // of the body or of the network error, so the real server message shows. Never carries the query string.
    quint64 probeStream(const QUrl& url);
    // ---- Remembered sign-in (DPAPI, opt-in) ----------------------------------------------
    // The store keeps one record: server URL, user id and the access token, protected with
    // CryptProtectData for the current Windows user. The password is never stored. An empty
    // path disables the store entirely.
    void setSessionStore(QString path);
    bool saveSession();
    bool restoreSession();
    void forgetSession();
    // True when a record is on disk, without unprotecting it: this is the sign-in page's opt-in
    // state and the condition for trying a restore at all.
    bool hasStoredSession() const;
signals:
    void authenticated();
    void failed(quint64 request, QString message);
    void pageReady(quint64 request, xmb::JellyfinPage page);
    void playbackReady(quint64 request, xmb::JellyfinPlayback playback);
    void artworkReady(quint64 request, QString itemId, bool backdrop, QImage image);
    void subtitleReady(quint64 request, int streamIndex, QByteArray bytes);
    void probeFinished(quint64 request, int status, QString detail);
    void chaptersReady(quint64 request, QString itemId, QVector<xmb::JellyfinChapter> chapters);
    void lyricsReady(quint64 request, QString itemId, QVector<xmb::JellyfinLyricLine> lines, bool synced);
    void userFlagChanged(quint64 request, QString itemId, int flag, bool value, bool ok);
    void nextEpisodeReady(quint64 request, xmb::JellyfinItem item, bool found);
private:
    enum class Operation { SignIn, Views, Items, Search, Playback, Artwork, Report, Subtitle, SubtitleImage, Probe, Home, Chapters, UserFlag, NextEpisode, Lyrics };
    quint64 send(Operation operation, QNetworkReply* reply, QString context = {}, bool audio = false);
    void finish(QNetworkReply* reply, Operation operation, quint64 request,
                quint64 generation, QString context = {}, bool audio = false);
    QNetworkReply* get(const QUrl& url, int transferTimeoutMs = 15000);
    QNetworkReply* post(const QUrl& url, const QByteArray& body);
    QNetworkReply* deleteResource(const QUrl& url);
    QUrl apiUrl(const QString& route) const;
    bool isCurrent(quint64 generation) const;
    void fail(quint64 request, const QString& message);
    void cancelRequests(bool includeReports = true);
    bool trustedServerUrl(const QUrl& url) const;
    void flushLogouts();
    QString cachedImagePath(const QString& itemId, const QString& tag, bool backdrop) const;
    void writeImageCache(const QString& path, const QByteArray& bytes) const;
    void trimImageCache() const;
    struct PendingLogout { QUrl url; QByteArray authorization; };
    QVector<PendingLogout> logouts_;
    bool destroying_=false;
    QNetworkAccessManager* network_ = nullptr;
    QVector<QNetworkReply*> replies_;
    QSet<QNetworkReply*> reportReplies_;
    QUrl server_;
    QByteArray accessToken_;
    QString userId_;
    QString deviceId_;
    QString imageCache_;
    QString sessionPath_;
    QHash<quint64, quint64> homeAlias_;   // a retried home list request -> the id the shell is waiting on
    QHash<quint64, quint64> flagAlias_;   // a retried user flag request -> the id the caller is waiting on
    struct NextWait { quint64 request = 0; QString seriesId, currentId; };
    QHash<quint64, NextWait> nextWait_;   // the watched mark in flight -> the next-episode request waiting behind it
    QHash<quint64, quint64> nextAlias_;   // the NextUp request -> the id the caller is waiting on
    quint64 generation_ = 0;
    quint64 nextRequest_ = 0;
};
} // namespace xmb
Q_DECLARE_METATYPE(xmb::JellyfinChapter)
Q_DECLARE_METATYPE(xmb::JellyfinLyricLine)
Q_DECLARE_METATYPE(xmb::JellyfinItem)
Q_DECLARE_METATYPE(xmb::JellyfinPage)
Q_DECLARE_METATYPE(xmb::JellyfinPlayback)
