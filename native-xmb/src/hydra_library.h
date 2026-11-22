#pragma once
#include "release_index.h"
#include <QHash>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

namespace xmb {
// Why a Hydra request failed, so the screen can say what is true and offer Retry only where retrying
// can help. Refused (HTTP 401/403) and TooLarge (over the body cap) can never succeed on a repeat;
// RateLimited needs time; Timeout and Server (5xx) are transient; Other covers everything else.
enum class HydraFailureKind { None, Refused, RateLimited, TooLarge, Timeout, Server, Other };
struct HydraFailure { HydraFailureKind kind=HydraFailureKind::None; int status=0; };
// One entry in the Hydra Launcher library (api.hydralibrary.com/sources). The url is the release-index
// JSON (e.g. https://hydralinks.cloud/sources/fitgirl.json) that importSource() can read.
struct HydraSource {
    QString title; QUrl url; int gamesCount=0; QString description; QStringList status; QStringList topDownloadOption;
    int installs=0;   // popularity proxy: times the source was installed via Hydra Launcher
};
// One download source a game is available from. A game may appear in several sources.
struct GameSourceOption {
    QString source;         // source display name
    QString sourceUrl;      // the source's release-index JSON, to resolve download links
    QString fileSize;
    QString sourceStatus;   // Trusted / Safe For Use / ...
    QString releaseTitle;   // Exact title in the provider's feed
    QString sourceSlug;      // Local imported source identifier
    QString mirror;          // Display label for a selected single-file link
    QVector<Download> downloads;
};
// One search hit from /games, deduplicated by name. Every source is kept (no silent
// torrent/direct filtering); each game carries one option per source with its name and size.
struct GameResult {
    QString name;           // clean name (the dedup key)
    QString title;
    QString originalTitle;  // the full release line from the first option
    QString coverUrl;       // landscape box art (Steam header), when known
    QVector<GameSourceOption> options;
};
struct PopularTitle {
    QString title;
    QString coverUrl;
};
struct PopularTitleGroup {
    QString name;
    QString coverUrl;
    QVector<PopularTitle> entries;
};
QVector<PopularTitleGroup> groupPopularTitles(const QVector<PopularTitle>& titles);
GameResult mergePopularGameResults(const QString& name,const QString& coverUrl,
                                   const QVector<GameResult>& results);
// One raw record from the Hydra /games API, before merge/dedup. buildGameCatalog turns a batch of
// these into the one-game-per-name catalogue.
struct GameRecord {
    QString name;          // clean name (the dedup key; editions differ here)
    QString title;         // display title (the API's "title")
    QString originalTitle; // full release line (the API's "originalTitle")
    QString source;        // source display name
    QString sourceUrl;     // the source's release-index JSON
    QString fileSize;      // download size label; empty means "unknown"
    QString sourceStatus;  // Trusted / Safe For Use / ...
    QString coverUrl;      // optional provider art URL carried through search results
};

// Deterministic catalogue merge: every source is kept (no silent torrent/direct filtering), each game
// appears once per distinct normalized name, and each source contributes at most one option carrying
// its display name and download size. Editions that differ in `name` stay separate. Games are ordered
// by their most-installed source, descending. `sources` only supplies the install counts.
QVector<GameResult> buildGameCatalog(const QVector<GameRecord>& records,
                                      const QVector<HydraSource>& sources);
// Stable local ranking for a returned search batch: exact normalized title, then prefix,
// contained phrase, then token overlap, then broad/unmatched. It never drops editions or sources.
void rankGameResults(QVector<GameResult>* results,const QString& query);

// One page of api.hydralibrary.com/sources. Exposed (with hasMoreSources) so the pagination that
// guarantees every source is included can be unit-tested without network access.
struct SourcesPage { QVector<HydraSource> sources; int total=0; int limit=0; };
SourcesPage parseSourcesPage(const QJsonObject& root);
// True when another /sources page may hold more entries. `pageNumber` is the 1-based page just
// consumed and `accumulated` the source count gathered so far (this page included). Capped at 100 pages.
bool hasMoreSources(int pageNumber, int accumulated, const SourcesPage& page);
// A small persisted note of how each source feed behaved last time, so Browse Sources can say which
// feeds are known to refuse the app, list working feeds first and avoid re-asking a refusal more than
// once a day. It stores only the last outcome and time per feed and an aggregate per host, keyed by a
// SHA-1 of the URL and of the host (never the URL or host itself), so the file holds no addresses.
// A feed that refused or was too large is re-probed after a day, so a source that recovers comes back;
// nothing here ever retries on its own.
class SourceHealth {
public:
    // How the Browse Sources list should present a feed. The order is the sort rank, best first.
    enum class Standing { Working, Unknown, Failing, LikelyBlocked, TooLarge, Blocked };
    static constexpr qint64 kReprobeSeconds=24*60*60;   // a refusal is re-checked at most once per day
    static constexpr int kLikelyBlockedFeeds=2;         // refusals on one host before its other feeds look blocked
    explicit SourceHealth(const QString& path);
    // The note in the app's data folder (empty when no data folder is available).
    static QString defaultPath();
    bool load();            // false when the file is absent or unreadable; the note is then empty
    bool save() const;      // atomic write; false on failure (the list just loses its memory)
    void record(const QUrl& feed,const HydraFailure& outcome,qint64 nowSeconds);   // kind None = the feed opened
    Standing standing(const QUrl& feed,qint64 nowSeconds) const;
    // True when this feed refused (or was too large) less than a day ago, so opening it again should
    // show the stored reason instead of asking the host once more.
    bool skipProbe(const QUrl& feed,qint64 nowSeconds,HydraFailure* stored=nullptr) const;
    // Stable ordering: working feeds first, then unprobed, then the rest by how sure we are they cannot open.
    QVector<HydraSource> ordered(const QVector<HydraSource>& sources,qint64 nowSeconds) const;
    int feedCount() const { return int(feeds_.size()); }
private:
    struct Feed { HydraFailureKind kind=HydraFailureKind::None; int status=0; qint64 at=0; QString host; };
    struct Host { int refused=0; qint64 refusedAt=0; qint64 okAt=0; };
    static QString feedKey(const QUrl& feed);
    static QString hostKey(const QUrl& feed);
    QString path_;
    QHash<QString,Feed> feeds_;
    QHash<QString,Host> hosts_;
};
// Blocking client, like the other network clients; call from a worker.
class HydraLibrary {
public:
    bool listSources(QVector<HydraSource>* out, QString* error=nullptr, HydraFailure* failure=nullptr) const;
    // The source names that ship extract-and-play (no-setup) games: description/title says
    // "pre-installed"/"portable", or its downloads are direct-host rather than torrent.
    bool preinstalledSources(QStringList* out, QString* error=nullptr) const;
    // Full-text search across /games, deduplicated by name. All sources are included (direct and
    // torrent); each game carries one option per source with its display name and download size.
    bool searchGames(const QString& query, QVector<GameResult>* out, QString* error=nullptr) const;
    // Read one source's release-index JSON without importing or mutating saved feeds. The returned
    // records are intentionally source-scoped; callers may resolve a selected title through /games.
    bool browseSource(const HydraSource& source, QVector<GameResult>* out, QString* error=nullptr, HydraFailure* failure=nullptr) const;
    // browseSource through the health note: a feed that refused (or was too large) less than a day ago is
    // not asked again (the stored reason is returned and *fromNote is set); otherwise the feed is opened
    // and its outcome recorded in `health`. The caller saves the note. `nowSeconds` is the wall clock.
    bool browseSourceNoted(const HydraSource& source, QVector<GameResult>* out, QString* error, HydraFailure* failure,
                           SourceHealth* health, qint64 nowSeconds, bool* fromNote=nullptr) const;
    bool popularTitles(QVector<PopularTitle>* out, QString* error=nullptr) const;
    // Resolve the complete cross-source choices for one selected source-scoped game: a single
    // /games search for its name, then an exact-name union into the source-only game. Returns true
    // on a successful lookup whether or not an exact match was found (a no-match keeps the source's
    // own "Only this source is known" state intact); returns false with *error only on a network or
    // malformed search failure, so a caller never presents an unresolved game as a complete one.
    bool resolveGameChoices(const GameResult& sourceGame, GameResult* resolved, QString* error=nullptr) const;
    bool popularGame(const PopularTitle& title, GameResult* out, QString* error=nullptr) const;
    // Genuinely-popular games, in popularity order: the Hydra Cloud "hot" catalogue
    // (api.hydralauncher.gg/catalogue/hot) matched against the download library, so each entry
    // carries its download sources and Steam box art. Titles with no downloadable
    // source are dropped. The order is the catalogue's own ranking, not newest-first.
    bool popularGames(QVector<GameResult>* out, QString* error=nullptr) const;
};
} // namespace xmb
