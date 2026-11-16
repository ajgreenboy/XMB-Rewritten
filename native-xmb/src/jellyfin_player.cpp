#include "jellyfin_player.h"

#include <QJsonValue>
#include <algorithm>
#include <limits>

namespace xmb::jfplayer {

qint64 ticksFromMs(qint64 ms) {
    if (ms <= 0) return 0;
    if (ms > std::numeric_limits<qint64>::max() / kTicksPerMs) return std::numeric_limits<qint64>::max();
    return ms * kTicksPerMs;
}

bool reachedPlayed(qint64 positionMs, qint64 durationMs) {
    if (durationMs <= 0 || positionMs < 0) return false;
    constexpr qint64 limit = std::numeric_limits<qint64>::max() / 100;
    if (durationMs > limit) return false;
    if (positionMs > limit) return true;
    return positionMs * 100 >= durationMs * kPlayedPercent;
}

StopOutcome stopOutcome(qint64 positionMs, qint64 durationMs, bool completed) {
    StopOutcome out;
    if (completed || reachedPlayed(positionMs, durationMs)) { out.played = true; return out; }
    if (positionMs >= kMinResumeMs) out.resumeMs = positionMs;
    return out;
}

QVector<Chapter> normalizedChapters(QVector<Chapter> chapters) {
    chapters.erase(std::remove_if(chapters.begin(), chapters.end(), [](const Chapter& c) { return c.startMs < 0; }), chapters.end());
    std::stable_sort(chapters.begin(), chapters.end(), [](const Chapter& a, const Chapter& b) { return a.startMs < b.startMs; });
    QVector<Chapter> out;
    for (const auto& c : chapters) {
        if (!out.isEmpty() && out.last().startMs == c.startMs) continue;
        out.push_back(c);
        if (out.size() >= 200) break;
    }
    return out;
}

int chapterIndexAt(const QVector<Chapter>& chapters, qint64 positionMs) {
    int found = -1;
    for (int i = 0; i < chapters.size(); ++i) {
        if (chapters[i].startMs <= positionMs) found = i;
        else break;
    }
    return found;
}

qint64 chapterSkipTarget(const QVector<Chapter>& chapters, qint64 positionMs, int direction, qint64 restartWindowMs) {
    if (chapters.size() < 2 || direction == 0) return -1;
    const int at = chapterIndexAt(chapters, positionMs);
    if (direction > 0) {
        const int next = at + 1;
        return next < chapters.size() ? chapters[next].startMs : -1;
    }
    if (at < 0) return -1;
    if (positionMs - chapters[at].startMs > restartWindowMs) return chapters[at].startMs;
    return at > 0 ? chapters[at - 1].startMs : -1;
}

QString chapterLabel(const Chapter& chapter, int index) {
    const QString name = chapter.name.trimmed().isEmpty() ? QStringLiteral("Chapter %1").arg(index + 1) : chapter.name.trimmed().left(60);
    return QStringLiteral("%1. %2").arg(index + 1).arg(name);
}

int pickAudioStream(const QVector<JellyfinTrack>& streams, int remembered) {
    int first = -1, preferred = -1;
    for (const auto& s : streams) {
        if (s.type != QLatin1String("Audio") || s.index < 0) continue;
        if (remembered >= 0 && s.index == remembered) return s.index;
        if (first < 0) first = s.index;
        if (s.isDefault && preferred < 0) preferred = s.index;
    }
    return preferred >= 0 ? preferred : first;
}

int audioOrdinal(const QVector<JellyfinTrack>& streams, int streamIndex) {
    int ordinal = 0;
    for (const auto& s : streams) {
        if (s.type != QLatin1String("Audio")) continue;
        if (s.index == streamIndex) return ordinal;
        ++ordinal;
    }
    return -1;
}

Choice choiceFor(const QJsonObject& map, const QString& key) {
    Choice c;
    const QJsonObject record = map.value(key).toObject();
    if (record.isEmpty()) return c;
    if (record.value(QStringLiteral("a")).isDouble()) c.audio = std::clamp(record.value(QStringLiteral("a")).toInt(-2), -2, 4096);
    if (record.value(QStringLiteral("s")).isDouble()) c.subtitle = std::clamp(record.value(QStringLiteral("s")).toInt(-2), -2, 4096);
    c.at = static_cast<qint64>(record.value(QStringLiteral("at")).toDouble(0));
    return c;
}

QJsonObject withChoice(QJsonObject map, const QString& key, int audio, int subtitle, qint64 nowSeconds) {
    if (key.isEmpty()) return map;
    Choice c = choiceFor(map, key);
    if (audio != -2) c.audio = audio;
    if (subtitle != -2) c.subtitle = subtitle;
    QJsonObject record;
    if (c.audio != -2) record.insert(QStringLiteral("a"), c.audio);
    if (c.subtitle != -2) record.insert(QStringLiteral("s"), c.subtitle);
    record.insert(QStringLiteral("at"), static_cast<double>(nowSeconds));
    map.insert(key, record);
    while (map.size() > kChoiceLimit) {
        QString oldest;
        double oldestAt = std::numeric_limits<double>::max();
        for (auto it = map.begin(); it != map.end(); ++it) {
            if (it.key() == key) continue;
            const double at = it.value().toObject().value(QStringLiteral("at")).toDouble(0);
            if (at < oldestAt) { oldestAt = at; oldest = it.key(); }
        }
        if (oldest.isEmpty()) break;
        map.remove(oldest);
    }
    return map;
}

bool choiceMapValid(const QJsonObject& map) {
    if (map.size() > kChoiceLimit) return false;
    for (auto it = map.begin(); it != map.end(); ++it) {
        const QString& key = it.key();
        if (key.isEmpty() || key.size() > 64) return false;
        for (const QChar ch : key)
            if (!(ch.isLetterOrNumber() && ch.unicode() < 128) && ch != QLatin1Char('_')) return false;
        if (!it.value().isObject()) return false;
        const QJsonObject record = it.value().toObject();
        for (auto f = record.begin(); f != record.end(); ++f) {
            const QString name = f.key();
            if (name != QLatin1String("a") && name != QLatin1String("s") && name != QLatin1String("at")) return false;
            if (!f.value().isDouble()) return false;
            const double v = f.value().toDouble();
            if (v != static_cast<double>(static_cast<qint64>(v))) return false;
            if (name == QLatin1String("at") ? (v < 0 || v > 4102444800.0) : (v < -1 || v > 4096)) return false;
        }
    }
    return true;
}

}   // namespace xmb::jfplayer
