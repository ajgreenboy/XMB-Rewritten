#pragma once
// Seerr -> Jellyfin Watch resolution: the safety policy that decides whether a Seerr request may turn into a
// Jellyfin Watch action (audit/PHASES-SEERR-JELLYFIN-2026-11-17.md). This is a pure function with no network and no
// session state, so the policy is testable without a server or a live sign-in. `seerr_shell.cpp` calls it from the
// search result handler; nothing here touches the UI or the client.
//
// The Stage 0 (movie) rules:
//   * A movie request matches only a Jellyfin Type "Movie" item.
//   * With a Seerr TMDB id, a candidate matches only on exact TMDB equality. A same-title candidate with a
//     different id is never accepted, and a candidate with no id cannot be confirmed either — title fallback
//     is allowed only when the Seerr id itself is missing.
//   * A 4K request watches only a candidate whose MediaSources width confirms >= 3840; an unknown width is
//     unverified, never presumed 4K.
//   * A regular request prefers a known < 3840 copy; otherwise it watches its sole matching copy (a regular
//     request needs no quality proof), and refuses only when several copies leave no safe pick.
//
// The Stage 1 (TV) rules, added on top (resolveSeerrSeries + selectSeerrTvEpisode):
//   * A TV request first recognises its Series by the same identity rules (exact TMDB equality, title fallback
//     only when the Seerr id is missing); only a Type "Series" item counts.
//   * It then chooses an Episode, never a Series/Season/Movie. The episode must carry the matched series' id and
//     its season (ParentIndexNumber) must be in the requested seasons.
//   * The lowest requested season that has a playable episode is chosen; within it, the first unwatched episode
//     by IndexNumber, or the first episode when every one is watched.
//   * A 4K TV request watches only an episode whose width confirms >= 3840; without one it stays Pending (never a
//     silent SD fallback), and an unknown width is unverified, never presumed 4K.
#include "jellyfin_client.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>
#include <algorithm>

namespace xmb {

// The width that marks a source 4K (3840, the horizontal resolution of UHD).
constexpr int Seerr4kMinWidth = 3840;

// Whether the request may become a Watch action.
enum class SeerrWatchState {
    Watch,    // a candidate is accepted and may be watched
    Pending,  // nothing is directly playable yet (still importing, or no confirmed match)
};

// Why the decision ended the way it did, for the audit and the tests.
enum class SeerrWatchReason {
    Matched4k,       // Watch: a 4K request found a confirmed >= 3840 source
    PreferRegular,   // Watch: a regular request chose a known < 3840 copy
    SoleCopy,        // Watch: a regular request accepted its sole matching copy (quality unproven)
    MatchedEpisode,  // Watch: a TV request chose a playable episode
    MatchedSeries,   // Watch (Step 1 only): a TV request recognised its Series
    WrongType,       // Pending: an unknown media type, or a step applied to the wrong type
    MismatchedTmdb,  // Pending: a same-title candidate carried a different TMDB id
    MissingTmdb,     // Pending: the Seerr id was supplied but a candidate carried no TMDB id to confirm
    Not4k,           // Pending: a 4K request's candidates are known < 3840
    WidthUnknown,    // Pending: a 4K request's width was not confirmed (never presume 4K)
    Ambiguous,       // Pending: several candidates, none a safe regular pick
    NoCandidate,     // Pending: no matching Movie candidate
    NoSeries,        // Pending: no matching Series candidate (TV)
    NoEpisode,       // Pending: the Series matched but no playable episode in the requested seasons
};

// The Seerr request's side of the resolution.
struct SeerrWatchRequest {
    QString mediaType;         // "movie" or "tv"
    QString tmdbId;            // the Seerr TMDB id (numeric); empty when Seerr supplied none
    QString title;             // the Seerr title, for the title fallback
    bool is4k = false;         // the request's is4k flag
    QVector<int> seasons;      // a TV request's requested season numbers (ascending); empty for a movie
};

struct SeerrWatchDecision {
    SeerrWatchState state = SeerrWatchState::Pending;
    SeerrWatchReason reason = SeerrWatchReason::NoCandidate;
    JellyfinItem item;   // the accepted candidate when state == Watch
};

// The width a Jellyfin item reports; 0 means the server reported none (unknown). Item.width is filled by
// parseItem() from MediaSources[].Width / a top-level Width.
inline int jellyfinItemWidth(const JellyfinItem& item) { return item.width; }

// Decide which of `candidates` (one Jellyfin search page) a Seerr request may watch. Pure.
inline SeerrWatchDecision resolveSeerrWatch(const SeerrWatchRequest& request, const QVector<JellyfinItem>& candidates) {
    SeerrWatchDecision decision;

    // TV: never match Movie/Video/Series as directly playable. Season/episode matching is Stage 1.
    if (request.mediaType != QLatin1String("movie")) {
        decision.reason = SeerrWatchReason::WrongType;
        return decision;
    }

    const QString seerrTmdb = request.tmdbId.trimmed();
    const QString title = request.title.trimmed();

    // 1. Identity: the Movie candidates that match by exact TMDB equality, or by the title fallback.
    QVector<const JellyfinItem*> matching;
    bool sawMismatchedTmdb = false;
    bool sawMissingTmdb = false;
    for (const auto& item : candidates) {
        if (item.type != QLatin1String("Movie")) continue;
        const QString candTmdb = item.tmdbId.trimmed();
        if (!seerrTmdb.isEmpty()) {
            // The Seerr request names its TMDB id: a candidate must carry the exact same id. A missing id
            // cannot be confirmed against a supplied id, and a different id is a conflict — neither resolves
            // by title.
            if (candTmdb.isEmpty()) { sawMissingTmdb = true; continue; }
            if (seerrTmdb == candTmdb) matching.push_back(&item);
            else sawMismatchedTmdb = true;
        } else {
            // No Seerr id: title fallback is the only way to recognise the item.
            const bool titleMatch = !title.isEmpty() && !item.title.isEmpty() &&
                                    item.title.trimmed().compare(title, Qt::CaseInsensitive) == 0;
            if (titleMatch) matching.push_back(&item);
        }
    }
    if (matching.isEmpty()) {
        decision.reason = sawMismatchedTmdb ? SeerrWatchReason::MismatchedTmdb
                        : sawMissingTmdb ? SeerrWatchReason::MissingTmdb
                        : SeerrWatchReason::NoCandidate;
        return decision;
    }

    // 2. Quality. A 4K request needs a confirmed >= 3840 source; unknown width is unverified, never 4K.
    if (request.is4k) {
        bool anyKnownWidth = false;
        for (const auto* item : matching) {
            if (jellyfinItemWidth(*item) >= Seerr4kMinWidth) {
                decision.state = SeerrWatchState::Watch;
                decision.reason = SeerrWatchReason::Matched4k;
                decision.item = *item;
                return decision;
            }
            if (jellyfinItemWidth(*item) > 0) anyKnownWidth = true;
        }
        decision.reason = anyKnownWidth ? SeerrWatchReason::Not4k : SeerrWatchReason::WidthUnknown;
        return decision;
    }

    // Regular request: prefer a known < 3840 copy.
    for (const auto* item : matching) {
        const int w = jellyfinItemWidth(*item);
        if (w > 0 && w < Seerr4kMinWidth) {
            decision.state = SeerrWatchState::Watch;
            decision.reason = SeerrWatchReason::PreferRegular;
            decision.item = *item;
            return decision;
        }
    }
    // No known < 3840 copy: a regular request still watches its sole matching copy — it needs no quality proof,
    // so an unknown width is fine and a confirmed 4K copy is permitted too.
    if (matching.size() == 1) {
        decision.state = SeerrWatchState::Watch;
        decision.reason = SeerrWatchReason::SoleCopy;
        decision.item = *matching.first();
        return decision;
    }
    // Several candidates and no known regular copy: ambiguous, never invent a variant.
    decision.reason = SeerrWatchReason::Ambiguous;
    return decision;
}

// The result of the TV Step 1: the matching Series (or why there is none). `state == Watch` here means only that
// a Series was recognised — it is not yet the final Watch action, which still needs a playable episode (Step 2).
struct SeerrSeriesDecision {
    SeerrWatchState state = SeerrWatchState::Pending;
    SeerrWatchReason reason = SeerrWatchReason::NoSeries;
    JellyfinItem series;   // the recognised Series when state == Watch
};

// The result of the TV Step 2: the chosen episode (or why there is none). This is the final TV Watch decision.
struct SeerrEpisodeDecision {
    SeerrWatchState state = SeerrWatchState::Pending;
    SeerrWatchReason reason = SeerrWatchReason::NoEpisode;
    JellyfinItem episode;   // the chosen episode when state == Watch
    int season = 0;         // the episode's season
};

// TV Step 1: recognise the matching Series for a TV request. Only a Type "Series" candidate counts; an
// Episode/Season/Movie is never a series match. The identity rules mirror the movie path: with a Seerr TMDB id,
// only a Series with the exact same id matches (a missing id cannot be confirmed, a different id is a conflict);
// without a Seerr id, the title fallback (case-insensitive, trimmed) is the only way to recognise it.
inline SeerrSeriesDecision resolveSeerrSeries(const SeerrWatchRequest& request, const QVector<JellyfinItem>& candidates) {
    SeerrSeriesDecision decision;
    if (request.mediaType != QLatin1String("tv")) {
        decision.reason = SeerrWatchReason::WrongType;
        return decision;
    }
    const QString seerrTmdb = request.tmdbId.trimmed();
    const QString title = request.title.trimmed();
    QVector<const JellyfinItem*> matching;
    bool sawMismatchedTmdb = false;
    bool sawMissingTmdb = false;
    for (const auto& item : candidates) {
        if (item.type != QLatin1String("Series")) continue;
        const QString candTmdb = item.tmdbId.trimmed();
        if (!seerrTmdb.isEmpty()) {
            if (candTmdb.isEmpty()) { sawMissingTmdb = true; continue; }
            if (seerrTmdb == candTmdb) matching.push_back(&item);
            else sawMismatchedTmdb = true;
        } else {
            const bool titleMatch = !title.isEmpty() && !item.title.isEmpty() &&
                                    item.title.trimmed().compare(title, Qt::CaseInsensitive) == 0;
            if (titleMatch) matching.push_back(&item);
        }
    }
    if (matching.isEmpty()) {
        decision.reason = sawMismatchedTmdb ? SeerrWatchReason::MismatchedTmdb
                        : sawMissingTmdb ? SeerrWatchReason::MissingTmdb
                        : SeerrWatchReason::NoSeries;
        return decision;
    }
    decision.state = SeerrWatchState::Watch;
    decision.reason = SeerrWatchReason::MatchedSeries;
    decision.series = *matching.first();
    return decision;
}

// TV Step 2: choose the playable episode. `series` is the Series matched in Step 1; `episodes` is the flat list
// of episodes the server returned for the requested seasons. The rules:
//   * Only a Type "Episode" item is watched — never a Series/Season/Movie.
//   * An episode must carry the matched series' id (SeriesId) and its season (ParentIndexNumber) must be one of
//     request.seasons; an episode of another series or a wrong season is not playable for this request.
//   * The lowest requested season with a qualifying episode is chosen.
//   * Within that season episodes are ordered by IndexNumber: the first unwatched one is chosen, and if every one
//     is watched the first (lowest) is chosen.
//   * A 4K request chooses only an episode whose width confirms >= 3840; without one it stays Pending (never a
//     silent SD fallback), and an unknown width is unverified, never presumed 4K.
inline SeerrEpisodeDecision selectSeerrTvEpisode(const SeerrWatchRequest& request, const JellyfinItem& series,
                                                 const QVector<JellyfinItem>& episodes) {
    SeerrEpisodeDecision decision;
    if (request.mediaType != QLatin1String("tv") || series.id.isEmpty()) {
        decision.reason = SeerrWatchReason::WrongType;
        return decision;
    }
    QSet<int> wanted;
    for (int s : request.seasons) if (s > 0) wanted.insert(s);
    QHash<int, QVector<const JellyfinItem*>> bySeason;
    bool sawKnownWidth = false;
    for (const auto& ep : episodes) {
        if (ep.type != QLatin1String("Episode")) continue;             // never a Series/Season/Movie
        if (ep.id.isEmpty()) continue;
        if (ep.seriesId.isEmpty() || ep.seriesId != series.id) continue;   // the episode must belong to the series
        if (!wanted.contains(ep.seasonNumber)) continue;               // a wrong-season episode is not playable
        if (ep.width > 0) sawKnownWidth = true;
        if (request.is4k && jellyfinItemWidth(ep) < Seerr4kMinWidth) continue;   // a 4K request never takes an SD copy
        bySeason[ep.seasonNumber].push_back(&ep);
    }
    QVector<int> ordered = wanted.values();
    std::sort(ordered.begin(), ordered.end());
    for (int season : ordered) {
        const auto it = bySeason.constFind(season);
        if (it == bySeason.cend() || it->isEmpty()) continue;
        const JellyfinItem* first = nullptr;
        const JellyfinItem* firstUnwatched = nullptr;
        for (const JellyfinItem* ep : *it) {
            if (!first || ep->episodeNumber < first->episodeNumber) first = ep;
            if (!ep->played && (!firstUnwatched || ep->episodeNumber < firstUnwatched->episodeNumber)) firstUnwatched = ep;
        }
        decision.state = SeerrWatchState::Watch;
        decision.reason = SeerrWatchReason::MatchedEpisode;
        decision.episode = *(firstUnwatched ? firstUnwatched : first);
        decision.season = season;
        return decision;
    }
    decision.reason = request.is4k ? (sawKnownWidth ? SeerrWatchReason::Not4k : SeerrWatchReason::WidthUnknown)
                                   : SeerrWatchReason::NoEpisode;
    return decision;
}

} // namespace xmb
