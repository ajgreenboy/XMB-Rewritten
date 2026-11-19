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

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
