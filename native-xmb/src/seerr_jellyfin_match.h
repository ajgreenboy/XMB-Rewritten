#pragma once
// Seerr -> Jellyfin Watch resolution: the safety policy that decides whether a Seerr request may turn into a
// Jellyfin Watch action (audit/PHASES-SEERR-JELLYFIN-2026-11-17.md). This is a pure function with no network and no
// session state, so the policy is testable without a server or a live sign-in. `seerr_shell.cpp` calls it from the
// search result handler; nothing here touches the UI or the client.
//
// The Stage 0 rules:
//   * A movie request matches only a Jellyfin Type "Movie" item.
//   * With a Seerr TMDB id, a candidate matches only on exact TMDB equality. A same-title candidate with a
//     different id is never accepted, and a candidate with no id cannot be confirmed either — title fallback
//     is allowed only when the Seerr id itself is missing.
//   * A 4K request watches only a candidate whose MediaSources width confirms >= 3840; an unknown width is
//     unverified, never presumed 4K.
//   * A regular request prefers a known < 3840 copy; otherwise it watches its sole matching copy (a regular
//     request needs no quality proof), and refuses only when several copies leave no safe pick.
//   * A TV request never matches Movie/Video/Series as directly playable: no Watch until Stage 1 season/episode
//     matching exists.
#include "jellyfin_client.h"

#include <QString>
#include <QVector>

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
    WrongType,       // Pending: TV (or an unknown type) — no direct Watch in Stage 0
    MismatchedTmdb,  // Pending: a same-title candidate carried a different TMDB id
    MissingTmdb,     // Pending: the Seerr id was supplied but a candidate carried no TMDB id to confirm
    Not4k,           // Pending: a 4K request's candidates are known < 3840
    WidthUnknown,    // Pending: a 4K request's width was not confirmed (never presume 4K)
    Ambiguous,       // Pending: several candidates, none a safe regular pick
    NoCandidate,     // Pending: no matching Movie candidate
};

// The Seerr request's side of the resolution.
struct SeerrWatchRequest {
    QString mediaType;   // "movie" or "tv"
    QString tmdbId;      // the Seerr TMDB id (numeric); empty when Seerr supplied none
    QString title;       // the Seerr title, for the title fallback
    bool is4k = false;   // the request's is4k flag
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

} // namespace xmb
