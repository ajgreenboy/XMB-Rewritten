#pragma once
#include <QChar>
#include <QHash>
#include <QPair>
#include <QString>
#include <QVector>

namespace xmb {
class Assets;
struct Entry;

struct TrophyEntry {
    int id = 0;
    QChar grade = QLatin1Char('B'); // 'P', 'G', 'S', 'B'
    bool hidden = false;
    bool unlocked = false;
    qint64 unlockedUnixSecs = 0;
    QString name;
    QString detail;
    QString iconPath;
};

struct TrophySet {
    QString commId;
    QString userId;
    QString dirPath;
    QString titleName;
    QString titleDetail;
    QString iconPath;
    QVector<TrophyEntry> trophies;
    int unlockedCount = 0;
    int platinum = 0, gold = 0, silver = 0, bronze = 0;
    int totalPoints = 0, earnedPoints = 0;
    qint64 latestUnlockUnixSecs = 0;
    int completionPercent() const;
};

// Case-insensitive lookup for SND0.WAV/.OGG/.MP3/.FLAC/.M4A/.AT3 inside dirPath (or empty).
QString findPreviewAudio(const QString& dirPath);

// Parses Sony TROPCONF.SFM XML (with optional binary header before '<').
bool parseTropConf(const QByteArray& bytes, TrophySet& out);

// Parses RPCS3/PS3 TROPUSR.DAT binary unlock state table (magic 0x818F54AD, table type 6).
bool parseTropUsr(const QByteArray& bytes, QHash<int, QPair<bool, qint64>>& unlockMap);

// Scans <hddRoot>/home/*/trophy/* for installed RPCS3 trophy sets (bounded to 128).
QVector<TrophySet> scanTrophySets(const QString& hddRoot);

// True if at least one valid trophy set exists under <hddRoot>/home/*/trophy/*.
bool hasTrophySets(const QString& hddRoot);

// Top-level rows for PSN > Trophy Collection.
QVector<Entry> trophyCollectionRows(Assets& assets, QString* pageTitle = nullptr);

// Per-game rows for one trophy set ("<userId>/<commId>").
QVector<Entry> trophySetRows(Assets& assets, const QString& setKey, QString* pageTitle = nullptr);

// True if at least one Game Data entry (CATEGORY == "GD" or non-bootable game data) exists under <hddRoot>/game/*.
bool hasGameDataEntries(const QString& hddRoot);

// Rows for Game > Game Data Utility (<hddRoot>/game/*).
QVector<Entry> gameDataRows(Assets& assets, QString* pageTitle = nullptr);

// Thread-safe forms: they touch only the filesystem under hddRoot (no Assets, whose RCO text cache is
// GUI-thread state), so the Shell can build a page on a worker and keep the GUI thread free.
QVector<Entry> trophyCollectionRowsAt(const QString& hddRoot);
QVector<Entry> trophySetRowsAt(const QString& hddRoot, const QString& setKey, const QString& hiddenTitle,
                               QString* pageTitle = nullptr);
QVector<Entry> gameDataRowsAt(const QString& hddRoot);

} // namespace xmb
