// Pure policy tests for the Seerr -> Jellyfin Watch resolution (seerr_jellyfin_match.h). No server, no network,
// no session: each case builds a Jellyfin search page in memory and asserts resolveSeerrWatch()'s decision. The
// policy itself is the production code path (seerr_shell.cpp calls the same function), so these pin the safety
// rules directly: movies only, exact TMDB equality, the 4K width rule, and no TV Watch in Stage 0.
//   xmb-seerr-watch-tests
#include "seerr_jellyfin_match.h"

#include <QCoreApplication>
#include <QVector>
#include <cstdio>

namespace {
int checks = 0, failures = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", label); }
}

using xmb::SeerrWatchDecision;
using xmb::SeerrWatchReason;
using xmb::SeerrWatchRequest;
using xmb::SeerrWatchState;
using xmb::SeerrEpisodeDecision;
using xmb::SeerrSeriesDecision;

xmb::JellyfinItem movie(QString title, QString tmdb = {}, int width = 0) {
    xmb::JellyfinItem item;
    item.type = QStringLiteral("Movie");
    item.title = std::move(title);
    item.tmdbId = std::move(tmdb);
    item.width = width;
    item.id = QStringLiteral("movie-item");
    return item;
}
xmb::JellyfinItem typed(QString type, QString title, QString tmdb = {}) {
    xmb::JellyfinItem item;
    item.type = std::move(type);
    item.title = std::move(title);
    item.tmdbId = std::move(tmdb);
    item.id = QStringLiteral("other-item");
    return item;
}
SeerrWatchRequest request(QString mediaType, QString tmdb, QString title, bool is4k = false) {
    SeerrWatchRequest r;
    r.mediaType = std::move(mediaType);
    r.tmdbId = std::move(tmdb);
    r.title = std::move(title);
    r.is4k = is4k;
    return r;
}
bool isWatch(const SeerrWatchDecision& d) { return d.state == SeerrWatchState::Watch; }
xmb::JellyfinItem seriesItem(QString id, QString tmdb, QString title) {
    xmb::JellyfinItem item;
    item.type = QStringLiteral("Series");
    item.id = std::move(id);
    item.tmdbId = std::move(tmdb);
    item.title = std::move(title);
    return item;
}
xmb::JellyfinItem episode(QString seriesId, int season, int number, bool played = false, int width = 1920) {
    xmb::JellyfinItem item;
    item.type = QStringLiteral("Episode");
    item.id = QStringLiteral("episode-item");
    item.seriesId = std::move(seriesId);
    item.seasonNumber = season;
    item.episodeNumber = number;
    item.played = played;
    item.width = width;
    return item;
}
SeerrWatchRequest tvRequest(QString tmdb, QString title, bool is4k, QVector<int> seasons) {
    SeerrWatchRequest r;
    r.mediaType = QStringLiteral("tv");
    r.tmdbId = std::move(tmdb);
    r.title = std::move(title);
    r.is4k = is4k;
    r.seasons = std::move(seasons);
    return r;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // ---- 4K request: only a confirmed >= 3840 source watches; unknown is never presumed 4K -------------------
    {
        const auto r = request(QStringLiteral("movie"), QStringLiteral("100"), QStringLiteral("Dune"), true);
        auto d = resolveSeerrWatch(r, {movie(QStringLiteral("Dune"), QStringLiteral("100"), 1920)});
        check(!isWatch(d) && d.reason == SeerrWatchReason::Not4k, "4K: a 1920 (SD) copy is refused");

        d = resolveSeerrWatch(r, {movie(QStringLiteral("Dune"), QStringLiteral("100"), 3840)});
        check(isWatch(d) && d.reason == SeerrWatchReason::Matched4k && d.item.width == 3840, "4K: a confirmed 3840 source watches");

        d = resolveSeerrWatch(r, {movie(QStringLiteral("Dune"), QStringLiteral("100"), 2160)});
        check(!isWatch(d) && d.reason == SeerrWatchReason::Not4k, "4K: a 2160p (non-UHD) source is still not 4K");

        d = resolveSeerrWatch(r, {movie(QStringLiteral("Dune"), QStringLiteral("100"), 0)});
        check(!isWatch(d) && d.reason == SeerrWatchReason::WidthUnknown, "4K: an unknown width is unverified, not presumed 4K");
    }
    {
        // A matching 4K copy wins even when the page also lists the same title's SD copy.
        const auto r = request(QStringLiteral("movie"), QStringLiteral("100"), QStringLiteral("Dune"), true);
        const auto d = resolveSeerrWatch(r, {movie(QStringLiteral("Dune"), QStringLiteral("100"), 1920),
                                             movie(QStringLiteral("Dune"), QStringLiteral("100"), 3840)});
        check(isWatch(d) && d.reason == SeerrWatchReason::Matched4k && d.item.width == 3840, "4K: the 4K source is chosen among several");
    }

    // ---- identity: movies only, exact TMDB equality, title fallback only when the Seerr id is missing ---------
    {
        // Same title, different TMDB id: never a fallback.
        const auto r = request(QStringLiteral("movie"), QStringLiteral("100"), QStringLiteral("Dune"));
        auto d = resolveSeerrWatch(r, {movie(QStringLiteral("Dune"), QStringLiteral("999"), 1920)});
        check(!isWatch(d) && d.reason == SeerrWatchReason::MismatchedTmdb, "identity: a same-title candidate with a different id is refused");
    }
    {
        // Seerr TMDB missing: title fallback is allowed.
        const auto r = request(QStringLiteral("movie"), {}, QStringLiteral("Dune"));
        auto d = resolveSeerrWatch(r, {movie(QStringLiteral("Dune"), QStringLiteral("999"), 1920)});
        check(isWatch(d) && d.reason == SeerrWatchReason::PreferRegular, "identity: title fallback when the Seerr id is missing");
    }
    {
        // Candidate TMDB missing with a Seerr id supplied: cannot be confirmed, never a title fallback.
        const auto r = request(QStringLiteral("movie"), QStringLiteral("100"), QStringLiteral("Dune"));
        auto d = resolveSeerrWatch(r, {movie(QStringLiteral("Dune"), {}, 1920)});
        check(!isWatch(d) && d.reason == SeerrWatchReason::MissingTmdb, "identity: a candidate with no id is refused when the Seerr id is supplied");
    }
    {
        // Exact TMDB equality matches even when the title differs (an alternate title).
        const auto r = request(QStringLiteral("movie"), QStringLiteral("100"), QStringLiteral("Dune"));
        auto d = resolveSeerrWatch(r, {movie(QStringLiteral("Dune: Part Two (Alt)"), QStringLiteral("100"), 1920)});
        check(isWatch(d) && d.reason == SeerrWatchReason::PreferRegular, "identity: an exact id match ignores a title difference");
    }
    {
        // Title fallback is case-insensitive and trims.
        const auto r = request(QStringLiteral("movie"), {}, QStringLiteral("  The Movie "));
        auto d = resolveSeerrWatch(r, {movie(QStringLiteral("the movie"), {}, 1920)});
        check(isWatch(d) && d.reason == SeerrWatchReason::PreferRegular, "identity: title fallback is case-insensitive and trimmed");
    }
    {
        // Episode/Video/Series are never a movie match, even with the same title and id.
        const auto r = request(QStringLiteral("movie"), QStringLiteral("100"), QStringLiteral("Dune"));
        const QVector<xmb::JellyfinItem> page{
            typed(QStringLiteral("Episode"), QStringLiteral("Dune"), QStringLiteral("100")),
            typed(QStringLiteral("Video"), QStringLiteral("Dune"), QStringLiteral("100")),
            typed(QStringLiteral("Series"), QStringLiteral("Dune"), QStringLiteral("100"))};
        const auto d = resolveSeerrWatch(r, page);
        check(!isWatch(d) && d.reason == SeerrWatchReason::NoCandidate, "identity: a movie request never matches Episode/Video/Series");
    }

    // ---- TV: no directly playable Watch in Stage 0 ---------------------------------------------------------------
    {
        const auto r = request(QStringLiteral("tv"), QStringLiteral("100"), QStringLiteral("Dune"));
        const QVector<xmb::JellyfinItem> page{
            movie(QStringLiteral("Dune"), QStringLiteral("100"), 1920),
            typed(QStringLiteral("Series"), QStringLiteral("Dune"), QStringLiteral("100")),
            typed(QStringLiteral("Episode"), QStringLiteral("Dune"), QStringLiteral("100"))};
        const auto d = resolveSeerrWatch(r, page);
        check(!isWatch(d) && d.reason == SeerrWatchReason::WrongType, "TV: never matches Movie/Video/Series as directly playable");
    }
    {
        // An unknown media type is treated the same way: nothing to watch yet.
        const auto r = request(QStringLiteral(""), {}, QStringLiteral("Dune"));
        const auto d = resolveSeerrWatch(r, {movie(QStringLiteral("Dune"), {}, 1920)});
        check(!isWatch(d) && d.reason == SeerrWatchReason::WrongType, "TV/unknown type: an empty type watches nothing");
    }

    // ---- regular request: prefer < 3840, watch a sole copy (4K or unknown), no false variants ---------------
    {
        // A single SD copy: the ordinary case.
        const auto r = request(QStringLiteral("movie"), QStringLiteral("100"), QStringLiteral("Dune"));
        auto d = resolveSeerrWatch(r, {movie(QStringLiteral("Dune"), QStringLiteral("100"), 1920)});
        check(isWatch(d) && d.reason == SeerrWatchReason::PreferRegular && d.item.width == 1920, "regular: a single SD copy watches");
    }
    {
        // Both versions exist: prefer the known < 3840 copy, whatever the page order.
        const auto r = request(QStringLiteral("movie"), QStringLiteral("100"), QStringLiteral("Dune"));
        auto d = resolveSeerrWatch(r, {movie(QStringLiteral("Dune"), QStringLiteral("100"), 3840),
                                       movie(QStringLiteral("Dune"), QStringLiteral("100"), 1920)});
        check(isWatch(d) && d.reason == SeerrWatchReason::PreferRegular && d.item.width == 1920, "regular: prefers a known < 3840 copy");
    }
    {
        // Only a 4K copy exists: a regular request watches its sole matching copy.
        const auto r = request(QStringLiteral("movie"), QStringLiteral("100"), QStringLiteral("Dune"));
        auto d = resolveSeerrWatch(r, {movie(QStringLiteral("Dune"), QStringLiteral("100"), 3840)});
        check(isWatch(d) && d.reason == SeerrWatchReason::SoleCopy && d.item.width == 3840, "regular: a sole confirmed 4K copy is permitted");
    }
    {
        // A sole exact-id copy of unknown width: a regular request watches it (no quality proof required).
        const auto r = request(QStringLiteral("movie"), QStringLiteral("100"), QStringLiteral("Dune"));
        auto d = resolveSeerrWatch(r, {movie(QStringLiteral("Dune"), QStringLiteral("100"), 0)});
        check(isWatch(d) && d.reason == SeerrWatchReason::SoleCopy && d.item.width == 0, "regular: a sole exact-id copy of unknown width watches");
    }
    {
        // Two 4K copies (or several with no regular pick): ambiguous, no false variant.
        const auto r = request(QStringLiteral("movie"), QStringLiteral("100"), QStringLiteral("Dune"));
        auto d = resolveSeerrWatch(r, {movie(QStringLiteral("Dune"), QStringLiteral("100"), 3840),
                                       movie(QStringLiteral("Dune"), QStringLiteral("100"), 3840)});
        check(!isWatch(d) && d.reason == SeerrWatchReason::Ambiguous, "regular: several 4K copies with no regular pick are ambiguous");
    }

    // ---- TV Stage 1: series identity ---------------------------------------------------------------------------
    {
        // Exact TMDB match on a Series.
        const auto r = request(QStringLiteral("tv"), QStringLiteral("100"), QStringLiteral("The Show"));
        const auto d = resolveSeerrSeries(r, {typed(QStringLiteral("Series"), QStringLiteral("The Show"), QStringLiteral("100"))});
        check(d.state == SeerrWatchState::Watch && d.reason == SeerrWatchReason::MatchedSeries && d.series.tmdbId == QLatin1String("100"), "series: an exact-TMDB Series matches");
    }
    {
        // Same title, different TMDB: refused (never a title fallback against a known id).
        const auto r = request(QStringLiteral("tv"), QStringLiteral("100"), QStringLiteral("The Show"));
        const auto d = resolveSeerrSeries(r, {typed(QStringLiteral("Series"), QStringLiteral("The Show"), QStringLiteral("999"))});
        check(d.state == SeerrWatchState::Pending && d.reason == SeerrWatchReason::MismatchedTmdb, "series: a same-title Series with a different id is refused");
    }
    {
        // A Series with no id cannot be confirmed against a supplied Seerr id.
        const auto r = request(QStringLiteral("tv"), QStringLiteral("100"), QStringLiteral("The Show"));
        const auto d = resolveSeerrSeries(r, {typed(QStringLiteral("Series"), QStringLiteral("The Show"), {})});
        check(d.state == SeerrWatchState::Pending && d.reason == SeerrWatchReason::MissingTmdb, "series: a Series with no id is refused when the Seerr id is supplied");
    }
    {
        // Title fallback applies only when the Seerr id is missing.
        const auto r = request(QStringLiteral("tv"), {}, QStringLiteral("The Show"));
        const auto d = resolveSeerrSeries(r, {typed(QStringLiteral("Series"), QStringLiteral("The Show"), QStringLiteral("999"))});
        check(d.state == SeerrWatchState::Watch && d.reason == SeerrWatchReason::MatchedSeries, "series: title fallback when the Seerr id is missing");
    }
    {
        // Cross-type: a TV request's series step ignores Episode/Movie/Season candidates.
        const auto r = request(QStringLiteral("tv"), QStringLiteral("100"), QStringLiteral("The Show"));
        const QVector<xmb::JellyfinItem> page{
            typed(QStringLiteral("Episode"), QStringLiteral("The Show"), QStringLiteral("100")),
            typed(QStringLiteral("Movie"), QStringLiteral("The Show"), QStringLiteral("100")),
            typed(QStringLiteral("Season"), QStringLiteral("The Show"), QStringLiteral("100"))};
        const auto d = resolveSeerrSeries(r, page);
        check(d.state == SeerrWatchState::Pending && d.reason == SeerrWatchReason::NoSeries, "series: Episode/Movie/Season are never a Series match");
    }

    // ---- TV Stage 2: episode selection --------------------------------------------------------------------------
    const QString seriesId = QStringLiteral("eeeeeeeeeeeeeeeeeeeeeeeeeeeeee01");
    {
        // The lowest requested season with episodes wins, even when the request lists the seasons out of order.
        const auto r = tvRequest(QStringLiteral("100"), QStringLiteral("The Show"), false, {3, 2});
        const auto s = seriesItem(seriesId, QStringLiteral("100"), QStringLiteral("The Show"));
        const auto d = selectSeerrTvEpisode(r, s, {episode(seriesId, 2, 5), episode(seriesId, 2, 6)});
        check(d.state == SeerrWatchState::Watch && d.reason == SeerrWatchReason::MatchedEpisode && d.season == 2 && d.episode.episodeNumber == 5,
              "tv: the lowest requested season with episodes is chosen (request order ignored)");
    }
    {
        // A lower all-watched season still wins over a higher season with unwatched episodes.
        const auto r = tvRequest(QStringLiteral("100"), QStringLiteral("The Show"), false, {1, 2});
        const auto s = seriesItem(seriesId, QStringLiteral("100"), QStringLiteral("The Show"));
        const auto d = selectSeerrTvEpisode(r, s, {episode(seriesId, 1, 1, true), episode(seriesId, 1, 2, true), episode(seriesId, 2, 1, false)});
        check(d.state == SeerrWatchState::Watch && d.season == 1 && d.episode.episodeNumber == 1, "tv: the lowest season with episodes wins even when its episodes are all watched");
    }
    {
        // Within a season the first unwatched episode by IndexNumber is chosen.
        const auto r = tvRequest(QStringLiteral("100"), QStringLiteral("The Show"), false, {2});
        const auto s = seriesItem(seriesId, QStringLiteral("100"), QStringLiteral("The Show"));
        const auto d = selectSeerrTvEpisode(r, s, {episode(seriesId, 2, 5, true), episode(seriesId, 2, 6, false)});
        check(d.state == SeerrWatchState::Watch && d.episode.episodeNumber == 6, "tv: the first unwatched episode by IndexNumber is chosen");
    }
    {
        // When every episode of the season is watched, the first (lowest IndexNumber) is chosen.
        const auto r = tvRequest(QStringLiteral("100"), QStringLiteral("The Show"), false, {2});
        const auto s = seriesItem(seriesId, QStringLiteral("100"), QStringLiteral("The Show"));
        const auto d = selectSeerrTvEpisode(r, s, {episode(seriesId, 2, 7, true), episode(seriesId, 2, 6, true)});
        check(d.state == SeerrWatchState::Watch && d.episode.episodeNumber == 6, "tv: when every episode is watched the first is chosen");
    }
    {
        // Wrong season: episodes outside the requested seasons are never watched.
        const auto r = tvRequest(QStringLiteral("100"), QStringLiteral("The Show"), false, {2});
        const auto s = seriesItem(seriesId, QStringLiteral("100"), QStringLiteral("The Show"));
        const auto d = selectSeerrTvEpisode(r, s, {episode(seriesId, 1, 1), episode(seriesId, 3, 1)});
        check(d.state == SeerrWatchState::Pending && d.reason == SeerrWatchReason::NoEpisode, "tv: a wrong-season episode is never watched");
    }
    {
        // Cross-type: Movie/Season/Series in the episode list are ignored, never watched.
        const auto r = tvRequest(QStringLiteral("100"), QStringLiteral("The Show"), false, {1});
        const auto s = seriesItem(seriesId, QStringLiteral("100"), QStringLiteral("The Show"));
        const QVector<xmb::JellyfinItem> list{
            typed(QStringLiteral("Movie"), QStringLiteral("The Show"), QStringLiteral("100")),
            typed(QStringLiteral("Season"), QStringLiteral("The Show"), QStringLiteral("100")),
            typed(QStringLiteral("Series"), QStringLiteral("The Show"), QStringLiteral("100"))};
        const auto d = selectSeerrTvEpisode(r, s, list);
        check(d.state == SeerrWatchState::Pending && d.reason == SeerrWatchReason::NoEpisode, "tv: a Series/Season/Movie is never watched as an episode");
    }
    {
        // Wrong series: an episode of another series is never watched.
        const auto r = tvRequest(QStringLiteral("100"), QStringLiteral("The Show"), false, {1});
        const auto s = seriesItem(seriesId, QStringLiteral("100"), QStringLiteral("The Show"));
        const auto d = selectSeerrTvEpisode(r, s, {episode(QStringLiteral("ffffffffffffffffffffffffffffffff"), 1, 1)});
        check(d.state == SeerrWatchState::Pending && d.reason == SeerrWatchReason::NoEpisode, "tv: an episode of another series is never watched");
    }
    {
        // An episode without a SeriesId cannot be confirmed to belong to the series.
        const auto r = tvRequest(QStringLiteral("100"), QStringLiteral("The Show"), false, {1});
        const auto s = seriesItem(seriesId, QStringLiteral("100"), QStringLiteral("The Show"));
        const auto d = selectSeerrTvEpisode(r, s, {episode({}, 1, 1)});
        check(d.state == SeerrWatchState::Pending && d.reason == SeerrWatchReason::NoEpisode, "tv: an episode without a SeriesId is not confirmed to belong to the series");
    }
    {
        // Missing season / no episodes: Pending.
        const auto r = tvRequest(QStringLiteral("100"), QStringLiteral("The Show"), false, {1, 2});
        const auto s = seriesItem(seriesId, QStringLiteral("100"), QStringLiteral("The Show"));
        const auto d = selectSeerrTvEpisode(r, s, {});
        check(d.state == SeerrWatchState::Pending && d.reason == SeerrWatchReason::NoEpisode, "tv: no episodes in the requested seasons stays Pending");
    }
    {
        // TV 4K: a known SD episode is refused (never a silent fallback), a confirmed 3840 episode watches,
        // and an unknown width is unverified.
        const auto s = seriesItem(seriesId, QStringLiteral("100"), QStringLiteral("The Show"));
        const auto r4k = tvRequest(QStringLiteral("100"), QStringLiteral("The Show"), true, {1});
        auto d = selectSeerrTvEpisode(r4k, s, {episode(seriesId, 1, 1, false, 1920)});
        check(d.state == SeerrWatchState::Pending && d.reason == SeerrWatchReason::Not4k, "tv 4K: a known SD episode is refused");
        d = selectSeerrTvEpisode(r4k, s, {episode(seriesId, 1, 1, false, 3840)});
        check(d.state == SeerrWatchState::Watch && d.episode.width == 3840, "tv 4K: a confirmed 3840 episode watches");
        d = selectSeerrTvEpisode(r4k, s, {episode(seriesId, 1, 1, false, 0)});
        check(d.state == SeerrWatchState::Pending && d.reason == SeerrWatchReason::WidthUnknown, "tv 4K: an unknown width is unverified, never presumed 4K");
    }

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
