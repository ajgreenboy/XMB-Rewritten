#pragma once
#include "release_index.h"
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

namespace xmb {
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
// Blocking client, like the other network clients; call from a worker.
class HydraLibrary {
public:
    // Default construction keeps every existing call site working (no FlareSolverr fallback).
    HydraLibrary() = default;
    // An optional FlareSolverr base URL (e.g. "http://localhost:8191"). For browseSource() it is
    // PRIMARY: a configured endpoint fetches the selected source feed through the solver first, and
    // falls back to the direct fetch only when the solver fails. The catalogue API paths (listSources,
    // popularTitles, popularGame, searchGames) keep the solver as a last-resort fallback after a failed
    // or HTML-challenged direct fetch. Empty (the default) leaves every fetch path byte-identical.
    explicit HydraLibrary(QString flareSolverrEndpoint);
    void setFlareSolverrEndpoint(const QString& endpoint);

    bool listSources(QVector<HydraSource>* out, QString* error=nullptr) const;
    // The source names that ship extract-and-play (no-setup) games: description/title says
    // "pre-installed"/"portable", or its downloads are direct-host rather than torrent.
    bool preinstalledSources(QStringList* out, QString* error=nullptr) const;
    // Full-text search across /games, deduplicated by name. All sources are included (direct and
    // torrent); each game carries one option per source with its display name and download size.
    bool searchGames(const QString& query, QVector<GameResult>* out, QString* error=nullptr) const;
    // Read one source's release-index JSON without importing or mutating saved feeds. The returned
    // records are intentionally source-scoped; callers may resolve a selected title through /games.
    bool browseSource(const HydraSource& source, QVector<GameResult>* out, QString* error=nullptr) const;
    bool popularTitles(QVector<PopularTitle>* out, QString* error=nullptr) const;
    bool popularGame(const PopularTitle& title, GameResult* out, QString* error=nullptr) const;
    // Genuinely-popular games, in popularity order: the Hydra Cloud "hot" catalogue
    // (api.hydralauncher.gg/catalogue/hot) matched against the download library, so each entry
    // carries its download sources and Steam box art. Titles with no downloadable
    // source are dropped. The order is the catalogue's own ranking, not newest-first.
    bool popularGames(QVector<GameResult>* out, QString* error=nullptr) const;
private:
    QString flareSolverrEndpoint_;
};
} // namespace xmb
