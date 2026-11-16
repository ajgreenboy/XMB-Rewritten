#pragma once

// Pure state logic for the Jellyfin player (audit/JELLYFIN-PLAYER.md): position ticks, the played rule, chapter
// navigation, the per-title audio/subtitle choice and which server stream to ask for. No Qt Multimedia, no
// network and no shell: everything here is a function of its arguments, so the unit tests cover it directly.

#include "jellyfin_client.h"

#include <QJsonObject>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace xmb::jfplayer {

// ---- Positions -------------------------------------------------------------------------------------------
constexpr qint64 kTicksPerMs = 10000;
// A title counts as watched from this share of its runtime on; the server's own default (MaxResumePct) is also 90.
constexpr int kPlayedPercent = 90;
// A position nearer the start than this is not worth remembering (the local Continue Watching rule is the same).
constexpr qint64 kMinResumeMs = 10000;

// Milliseconds to the server's 100 ns ticks. Negative input is 0; a value that would overflow saturates.
qint64 ticksFromMs(qint64 ms);
// True once the position is at least kPlayedPercent of a known duration. An unknown duration is never "played".
bool reachedPlayed(qint64 positionMs, qint64 durationMs);

struct StopOutcome {
    qint64 resumeMs = 0;   // what the row should remember (0: nothing to resume)
    bool played = false;   // mark the title watched (and clear its resume point)
};
// What a stop means for the row and the server: reaching the end, or 90 percent, is "played" with no resume
// point; a position nearer the start than kMinResumeMs is no resume point; anything else resumes where it stopped.
StopOutcome stopOutcome(qint64 positionMs, qint64 durationMs, bool completed);

// ---- Chapters --------------------------------------------------------------------------------------------
struct Chapter {
    qint64 startMs = 0;
    QString name;
};
// Chapters sorted by start with duplicates and negative starts dropped, at most 200 (a runaway server list).
QVector<Chapter> normalizedChapters(QVector<Chapter> chapters);
// The chapter the position is in: the last one that starts at or before it, or -1 before the first / with none.
int chapterIndexAt(const QVector<Chapter>& chapters, qint64 positionMs);
// Where a chapter skip goes, or -1 when there is no chapter in that direction (the caller then falls through to
// the next or previous item). Forward: the next chapter's start. Back: the start of the current chapter, unless
// the position is within restartWindowMs of it, then the previous chapter's start.
qint64 chapterSkipTarget(const QVector<Chapter>& chapters, qint64 positionMs, int direction,
                         qint64 restartWindowMs = 3000);
// "1. Opening  00:00", or "3. Chapter 3" for an unnamed one.
QString chapterLabel(const Chapter& chapter, int index);

// ---- Streams and the per-title choice --------------------------------------------------------------------
// The server stream index to ask for: the remembered one when this item still has that audio stream, else the
// stream the server marks default, else the first audio stream, else -1.
int pickAudioStream(const QVector<JellyfinTrack>& streams, int remembered);
// The ordinal (0-based, among the Audio streams only) of a server stream index, or -1.
int audioOrdinal(const QVector<JellyfinTrack>& streams, int streamIndex);

struct Choice {
    int audio = -2;      // server stream index; -2 = nothing remembered
    int subtitle = -2;   // server stream index; -1 = subtitles off; -2 = nothing remembered
    qint64 at = 0;       // unix seconds of the last change
    bool any() const { return audio != -2 || subtitle != -2; }
};
constexpr int kChoiceLimit = 64;
Choice choiceFor(const QJsonObject& map, const QString& key);
// The map with this key's choice replaced (a field left at -2 keeps its stored value) and bounded to kChoiceLimit
// entries, the least recently changed going first.
QJsonObject withChoice(QJsonObject map, const QString& key, int audio, int subtitle, qint64 nowSeconds);
// Shape check for the stored value (used by the preferences validator): an object of at most kChoiceLimit entries
// whose keys are 1-64 letters, digits or underscores and whose values carry only a, s (integers) and at.
bool choiceMapValid(const QJsonObject& map);

// ---- Phase 2: Play Next Episode, home rows, flags, speed -------------------------------------------------------
// The countdown on the Play Next Episode prompt. Circle cancels it, Cross plays at once.
constexpr int kAutoplaySeconds = 10;
// Whole seconds still to go (rounded up) after elapsedMs, 0 when the countdown is over. A negative elapsed time counts as none.
int autoplaySecondsLeft(qint64 elapsedMs, int totalSeconds = kAutoplaySeconds);
// A finished item looks for a next episode only when it is a Jellyfin Episode that knows its series, is not audio, and the
// preference is on.
bool offersNextEpisode(const QString& type, const QString& seriesId, bool audio, bool enabled);
// "S2E5 - Title" for an episode with both numbers, "Title" otherwise. Printable ASCII only is not enforced: the text is the server's.
QString episodeLabel(const JellyfinItem& item);
// The prompt's one line: "Play Next Episode: S1E2 - Title in 7 s. Cross plays now, Circle cancels."
QString autoplayPromptText(const QString& label, int secondsLeft);

// Latest rows are offered for libraries whose items are videos: movies, tv shows, home videos, music videos and mixed
// libraries (no collection type). Music, books, photos, playlists, live TV and boxsets are not.
bool latestEligible(const QString& collectionType);

// The watched and favourite toggles of the item menu: the row's label from the state it is in.
QString watchedToggleLabel(bool played);
QString favoriteToggleLabel(bool favorite);

// Playback speed: the steps the panel offers (0.5 to 2), the step nearest a value, and the label ("1x", "1.25x").
const QVector<double>& speedSteps();
double nearestSpeed(double value);
QString speedLabel(double value);

}   // namespace xmb::jfplayer
