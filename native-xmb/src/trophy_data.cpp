#include "trophy_data.h"
#include "assets.h"
#include "shell.h"
#ifdef XMB_USE_RUST_EDGE
#include "image_decode_rust.h"
#endif
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QXmlStreamReader>
#include <QtEndian>
#include <algorithm>

namespace xmb {
namespace {

constexpr qint64 kMaxTropConfBytes = 4 * 1024 * 1024;
constexpr qint64 kMaxTropUsrBytes = 4 * 1024 * 1024;
constexpr quint64 kCellRtcUnixEpochDeltaSecs = 62135596800ULL;

int trophyGradePoints(QChar grade) {
    switch (grade.toUpper().toLatin1()) {
    case 'B': return 15;
    case 'S': return 30;
    case 'G': return 90;
    case 'P': return 0; // Platinum does not add to base percentage weights on PS3.
    default: return 0;
    }
}

QString trophyGradeLabel(QChar grade) {
    switch (grade.toUpper().toLatin1()) {
    case 'P': return QObject::tr("Platinum");
    case 'G': return QObject::tr("Gold");
    case 'S': return QObject::tr("Silver");
    case 'B': return QObject::tr("Bronze");
    default: return QObject::tr("Unknown");
    }
}

QImage loadBoundedImage(const QString& path, int maxW, int maxH) {
    if (path.isEmpty() || !QFileInfo(path).isFile()) return {};
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > 8192 || size.height() > 8192) return {};
    if (size.width() > maxW || size.height() > maxH) {
        reader.setScaledSize(size.scaled(maxW, maxH, Qt::KeepAspectRatio));
    }
    return reader.read();
}

QString formatBytesHuman(qint64 bytes) {
    if (bytes >= 1024ll * 1024 * 1024)
        return QStringLiteral("%1 GB").arg(double(bytes) / (1024. * 1024. * 1024.), 0, 'f', 2);
    if (bytes >= 1024ll * 1024)
        return QStringLiteral("%1 MB").arg(double(bytes) / (1024. * 1024.), 0, 'f', 1);
    if (bytes >= 1024)
        return QStringLiteral("%1 KB").arg((bytes + 1023) / 1024);
    return QStringLiteral("%1 B").arg(std::max<qint64>(0, bytes));
}

QHash<QString, QString> readParamSfoFile(const QString& path) {
    QFile file(path);
    QHash<QString, QString> values;
    if (!file.open(QIODevice::ReadOnly)) return values;
    const QByteArray d = file.readAll();
#ifdef XMB_USE_RUST_EDGE
    image_rust::parseSfo(d, values);
    return values;
#else
    if (d.size() < 20 || !d.startsWith(QByteArray("\0PSF", 4))) return values;
    const auto u16 = [&](qsizetype at) { return at + 2 <= d.size() ? quint32(quint8(d[at]) | quint8(d[at + 1]) << 8) : 0u; };
    const auto u32 = [&](qsizetype at) { return at + 4 <= d.size() ? quint32(u16(at) | u16(at + 2) << 16) : 0u; };
    const quint32 keys = u32(8), data = u32(12), count = std::min<quint32>(u32(16), 256);
    for (quint32 i = 0; i < count; ++i) {
        const qsizetype e = 20 + qsizetype(i) * 16;
        if (e + 16 > d.size()) break;
        const quint32 keyAt = keys + u16(e), format = u16(e + 2), length = u32(e + 4), dataAt = data + u32(e + 12);
        if (keyAt >= quint32(d.size()) || dataAt + length > quint32(d.size())) continue;
        const QString key = QString::fromLatin1(d.constData() + keyAt);
        if (format == 0x0204 || format == 0x0004)
            values.insert(key, QString::fromUtf8(d.constData() + dataAt, int(length)).remove(QChar(0)));
        else if (format == 0x0404)
            values.insert(key, QString::number(u32(dataAt)));
    }
    return values;
#endif
}

qint64 cellRtcTickToUnixSeconds(quint64 tick) {
    if (tick == 0) return 0;
    const quint64 secs = tick / 1000000ULL;
    if (secs >= kCellRtcUnixEpochDeltaSecs) {
        const quint64 unixSecs = secs - kCellRtcUnixEpochDeltaSecs;
        if (unixSecs <= 4102444800ULL) return qint64(unixSecs);
    }
    if (tick >= 946684800ULL && tick <= 4102444800ULL) return qint64(tick);
    return 0;
}

bool readTrophySetDirectory(const QString& userId, const QString& commId, const QString& dirPath, TrophySet& out) {
    QFile confFile(QDir(dirPath).filePath(QStringLiteral("TROPCONF.SFM")));
    if (!confFile.open(QIODevice::ReadOnly) || confFile.size() <= 0 || confFile.size() > kMaxTropConfBytes)
        return false;
    out = {};
    out.userId = userId;
    out.commId = commId;
    out.dirPath = dirPath;
    if (!parseTropConf(confFile.readAll(), out)) return false;

    const QString icon0 = QDir(dirPath).filePath(QStringLiteral("ICON0.PNG"));
    if (QFileInfo(icon0).isFile()) out.iconPath = icon0;

    QHash<int, QPair<bool, qint64>> unlockMap;
    QFile usrFile(QDir(dirPath).filePath(QStringLiteral("TROPUSR.DAT")));
    if (usrFile.open(QIODevice::ReadOnly) && usrFile.size() > 0 && usrFile.size() <= kMaxTropUsrBytes) {
        parseTropUsr(usrFile.readAll(), unlockMap);
    }

    for (auto& t : out.trophies) {
        const QString iconName = QStringLiteral("TROP%1.PNG").arg(t.id, 3, 10, QLatin1Char('0'));
        const QString p = QDir(dirPath).filePath(iconName);
        if (QFileInfo(p).isFile()) t.iconPath = p;

        if (const auto it = unlockMap.constFind(t.id); it != unlockMap.cend()) {
            t.unlocked = it.value().first;
            t.unlockedUnixSecs = it.value().second;
        }
        const int pts = trophyGradePoints(t.grade);
        out.totalPoints += pts;
        if (t.unlocked) {
            ++out.unlockedCount;
            out.earnedPoints += pts;
            out.latestUnlockUnixSecs = std::max(out.latestUnlockUnixSecs, t.unlockedUnixSecs);
            switch (t.grade.toUpper().toLatin1()) {
            case 'P': ++out.platinum; break;
            case 'G': ++out.gold; break;
            case 'S': ++out.silver; break;
            case 'B': ++out.bronze; break;
            default: break;
            }
        }
    }
    return !out.trophies.isEmpty() || !out.titleName.isEmpty();
}

} // namespace

int TrophySet::completionPercent() const {
    if (totalPoints > 0) return int((qint64(earnedPoints) * 100) / totalPoints);
    if (!trophies.isEmpty()) return int((qint64(unlockedCount) * 100) / trophies.size());
    return 0;
}

QString findPreviewAudio(const QString& dirPath) {
    if (dirPath.isEmpty()) return {};
    const QDir dir(dirPath);
    if (!dir.exists()) return {};
    static const QStringList candidates{
        QStringLiteral("SND0.WAV"),
        QStringLiteral("SND0.OGG"),
        QStringLiteral("SND0.MP3"),
        QStringLiteral("SND0.FLAC"),
        QStringLiteral("SND0.M4A"),
        QStringLiteral("SND0.AT3"),
    };
    for (const QString& name : candidates) {
        const QString full = dir.filePath(name);
        if (QFileInfo(full).isFile()) return full;
    }
    return {};
}

bool parseTropConf(const QByteArray& bytes, TrophySet& out) {
    if (bytes.isEmpty() || bytes.size() > kMaxTropConfBytes) return false;
    const int firstLt = bytes.indexOf('<');
    if (firstLt < 0 || firstLt > 256) return false;

    QXmlStreamReader xml(bytes.mid(firstLt));
    bool sawRoot = false;
    bool inTrophy = false;
    TrophyEntry current;

    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::StartElement) {
            const auto tag = xml.name();
            if (tag == QLatin1String("trophyconf")) {
                sawRoot = true;
            } else if (!inTrophy && tag == QLatin1String("title-name")) {
                out.titleName = xml.readElementText().trimmed();
            } else if (!inTrophy && tag == QLatin1String("title-detail")) {
                out.titleDetail = xml.readElementText().trimmed();
            } else if (tag == QLatin1String("trophy")) {
                inTrophy = true;
                current = {};
                const auto attrs = xml.attributes();
                bool okId = false;
                const int tid = attrs.value(QLatin1String("id")).toInt(&okId);
                current.id = okId ? tid : int(out.trophies.size());
                const auto ttype = attrs.value(QLatin1String("ttype"));
                current.grade = ttype.isEmpty() ? QLatin1Char('B') : ttype.at(0).toUpper();
                const auto hiddenAttr = attrs.value(QLatin1String("hidden"));
                current.hidden = hiddenAttr.compare(QLatin1String("yes"), Qt::CaseInsensitive) == 0 ||
                                 hiddenAttr.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0 ||
                                 hiddenAttr == QLatin1String("1");
            } else if (inTrophy && tag == QLatin1String("name")) {
                current.name = xml.readElementText().trimmed();
            } else if (inTrophy && tag == QLatin1String("detail")) {
                current.detail = xml.readElementText().trimmed();
            }
        } else if (token == QXmlStreamReader::EndElement) {
            if (xml.name() == QLatin1String("trophy") && inTrophy) {
                inTrophy = false;
                if (out.trophies.size() < 512) out.trophies.push_back(std::move(current));
            }
        }
    }
    return sawRoot && !xml.hasError();
}

bool parseTropUsr(const QByteArray& bytes, QHash<int, QPair<bool, qint64>>& unlockMap) {
    unlockMap.clear();
    if (bytes.size() < 0x30 || bytes.size() > kMaxTropUsrBytes) return false;
    const auto be32 = [&](qsizetype at) {
        return qFromBigEndian<quint32>(bytes.constData() + at);
    };
    const auto be64 = [&](qsizetype at) {
        return qFromBigEndian<quint64>(bytes.constData() + at);
    };
    if (be32(0) != 0x818F54ADu) return false;
    const quint32 tablesCount = std::min<quint32>(be32(8), 32u);
    if (0x30ULL + quint64(tablesCount) * 0x20ULL > quint64(bytes.size())) return false;

    bool foundType6 = false;
    for (quint32 ti = 0; ti < tablesCount; ++ti) {
        const qsizetype th = 0x30 + qsizetype(ti) * 0x20;
        const quint32 type = be32(th);
        const quint32 entriesSize = be32(th + 4);
        const quint32 entriesCount = be32(th + 12);
        const quint64 offset = be64(th + 16);
        if (type != 6u) continue;
        if (entriesSize < 0x28u || entriesCount > 1024u) continue;
        const quint64 stride = quint64(entriesSize) + 0x10ULL;
        const quint64 tableBytes = stride * quint64(entriesCount);
        if (offset > quint64(bytes.size()) || tableBytes > quint64(bytes.size()) - offset) continue;

        foundType6 = true;
        for (quint32 ei = 0; ei < entriesCount; ++ei) {
            const qsizetype rec = qsizetype(offset + quint64(ei) * stride);
            const int trophyId = int(be32(rec + 0x10));
            const bool unlocked = be32(rec + 0x14) != 0u;
            const quint64 tick2 = be64(rec + 0x28);
            const quint64 tick1 = be64(rec + 0x20);
            const qint64 unixSecs = unlocked ? cellRtcTickToUnixSeconds(tick2 ? tick2 : tick1) : 0;
            unlockMap.insert(trophyId, qMakePair(unlocked, unixSecs));
        }
    }
    return foundType6;
}

QVector<TrophySet> scanTrophySets(const QString& hddRoot) {
    QVector<TrophySet> sets;
    if (hddRoot.isEmpty()) return sets;
    const QDir homeDir(QDir(hddRoot).filePath(QStringLiteral("home")));
    if (!homeDir.exists()) return sets;

    for (const auto& userInfo : homeDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QDir trophyRoot(QDir(userInfo.absoluteFilePath()).filePath(QStringLiteral("trophy")));
        if (!trophyRoot.exists()) continue;
        for (const auto& setInfo : trophyRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (sets.size() >= 128) break;
            TrophySet set;
            if (readTrophySetDirectory(userInfo.fileName(), setInfo.fileName(), setInfo.absoluteFilePath(), set))
                sets.push_back(std::move(set));
        }
        if (sets.size() >= 128) break;
    }

    std::stable_sort(sets.begin(), sets.end(), [](const TrophySet& a, const TrophySet& b) {
        if (a.latestUnlockUnixSecs != b.latestUnlockUnixSecs)
            return a.latestUnlockUnixSecs > b.latestUnlockUnixSecs;
        return a.titleName.localeAwareCompare(b.titleName) < 0;
    });
    return sets;
}

bool hasTrophySets(const QString& hddRoot) {
    if (hddRoot.isEmpty()) return false;
    const QDir homeDir(QDir(hddRoot).filePath(QStringLiteral("home")));
    if (!homeDir.exists()) return false;
    for (const auto& userInfo : homeDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QDir trophyRoot(QDir(userInfo.absoluteFilePath()).filePath(QStringLiteral("trophy")));
        if (!trophyRoot.exists()) continue;
        for (const auto& setInfo : trophyRoot.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
            if (QFileInfo(QDir(setInfo.absoluteFilePath()).filePath(QStringLiteral("TROPCONF.SFM"))).isFile())
                return true;
        }
    }
    return false;
}

QVector<Entry> trophyCollectionRows(Assets& assets, QString* pageTitle) {
    if (pageTitle) *pageTitle = assets.text("explore_category_psn", "msg_trophies", QObject::tr("Trophy Collection"));
    QVector<Entry> rows;
    const auto sets = scanTrophySets(assets.hddRoot());
    for (const auto& set : sets) {
        Entry row;
        row.id = QStringLiteral("trophy:set:") + set.userId + QLatin1Char(':') + set.commId;
        row.title = set.titleName.isEmpty() ? set.commId : set.titleName;
        row.detail = QStringLiteral("%1%  \u00B7  %2/%3  \u00B7  P:%4 G:%5 S:%6 B:%7")
                         .arg(set.completionPercent())
                         .arg(set.unlockedCount)
                         .arg(int(set.trophies.size()))
                         .arg(set.platinum)
                         .arg(set.gold)
                         .arg(set.silver)
                         .arg(set.bronze);
        row.container = QStringLiteral("explore_plugin_full");
        row.texture = QStringLiteral("item_tex_trophy");
        row.action = Action::TrophySet;
        row.value = set.userId + QLatin1Char('/') + set.commId;
        row.source = QStringLiteral("hdd");
        if (!set.iconPath.isEmpty()) {
            const QImage img = loadBoundedImage(set.iconPath, 512, 288);
            if (!img.isNull()) {
                row.artwork = img;
                row.gameTile = true;
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

QVector<Entry> trophySetRows(Assets& assets, const QString& setKey, QString* pageTitle) {
    QVector<Entry> rows;
    const QString userId = setKey.section(QLatin1Char('/'), 0, 0);
    const QString commId = setKey.section(QLatin1Char('/'), 1, 1);
    if (userId.isEmpty() || commId.isEmpty()) return rows;
    const QString dirPath = QDir(QDir(QDir(assets.hddRoot()).filePath(QStringLiteral("home")))
                                     .filePath(userId + QStringLiteral("/trophy")))
                                .filePath(commId);
    TrophySet set;
    if (!readTrophySetDirectory(userId, commId, dirPath, set)) return rows;
    if (pageTitle) *pageTitle = set.titleName.isEmpty() ? commId : set.titleName;

    const QString hiddenTitle = assets.text("explore_category_psn", "msg_trophy_hidden", QObject::tr("Hidden Trophy"));
    for (const auto& t : set.trophies) {
        Entry row;
        row.id = QStringLiteral("trophy:item:") + commId + QLatin1Char(':') + QString::number(t.id);
        row.container = QStringLiteral("explore_plugin_full");
        row.texture = QStringLiteral("item_tex_trophy");
        row.action = Action::Information;
        row.source = QStringLiteral("hdd");

        const QString gradeStr = trophyGradeLabel(t.grade);
        if (t.hidden && !t.unlocked) {
            row.title = hiddenTitle;
            row.detail = gradeStr + QStringLiteral("  \u00B7  ") + QObject::tr("Locked (Hidden)");
            row.value = row.title + QStringLiteral("\n\n") +
                        QObject::tr("Grade: %1\nStatus: Not Earned (Hidden Trophy)").arg(gradeStr);
        } else {
            row.title = t.name.isEmpty() ? QObject::tr("Trophy %1").arg(t.id) : t.name;
            const QString when = t.unlockedUnixSecs > 0
                ? QDateTime::fromSecsSinceEpoch(t.unlockedUnixSecs).toString(QStringLiteral("yyyy/MM/dd HH:mm"))
                : QString();
            const QString statusLine = t.unlocked
                ? (when.isEmpty() ? QObject::tr("Earned") : QObject::tr("Earned %1").arg(when))
                : QObject::tr("Not Earned");
            row.detail = gradeStr + QStringLiteral("  \u00B7  ") + statusLine;
            if (!t.iconPath.isEmpty()) {
                const QImage img = loadBoundedImage(t.iconPath, 240, 240);
                if (!img.isNull()) {
                    row.artwork = img;
                    row.iconSized = true;
                }
            }
            QStringList info;
            info << QObject::tr("Trophy: %1").arg(row.title);
            info << QObject::tr("Grade: %1").arg(gradeStr);
            info << QObject::tr("Status: %1").arg(statusLine);
            if (!t.detail.isEmpty()) info << QObject::tr("Details: %1").arg(t.detail);
            row.value = row.title + QStringLiteral("\n\n") + info.join('\n');
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

bool hasGameDataEntries(const QString& hddRoot) {
    if (hddRoot.isEmpty()) return false;
    const QDir gameDir(QDir(hddRoot).filePath(QStringLiteral("game")));
    if (!gameDir.exists()) return false;
    for (const auto& info : gameDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QDir dir(info.absoluteFilePath());
        const QString sfoPath = dir.filePath(QStringLiteral("PARAM.SFO"));
        if (!QFileInfo(sfoPath).isFile()) continue;
        const auto sfo = readParamSfoFile(sfoPath);
        if (sfo.isEmpty()) continue;
        const QString cat = sfo.value(QStringLiteral("CATEGORY"));
        if (cat == QLatin1String("GD") || !QFileInfo(dir.filePath(QStringLiteral("USRDIR/EBOOT.BIN"))).isFile())
            return true;
    }
    return false;
}

QVector<Entry> gameDataRows(Assets& assets, QString* pageTitle) {
    if (pageTitle) *pageTitle = assets.text("explore_category_game", "msg_gamedata_utility", QObject::tr("Game Data Utility"));
    QVector<Entry> rows;
    const QDir gameDir(QDir(assets.hddRoot()).filePath(QStringLiteral("game")));
    if (!gameDir.exists()) return rows;

    for (const auto& info : gameDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time)) {
        if (rows.size() >= 128) break;
        const QDir dir(info.absoluteFilePath());
        const QString sfoPath = dir.filePath(QStringLiteral("PARAM.SFO"));
        if (!QFileInfo(sfoPath).isFile()) continue;
        const auto sfo = readParamSfoFile(sfoPath);
        if (sfo.isEmpty()) continue;
        const QString cat = sfo.value(QStringLiteral("CATEGORY"));
        if (cat != QLatin1String("GD") && QFileInfo(dir.filePath(QStringLiteral("USRDIR/EBOOT.BIN"))).isFile())
            continue;

        qint64 totalBytes = 0;
        int countedFiles = 0;
        QDateTime latestModified = info.lastModified();
        QDirIterator it(info.absoluteFilePath(), QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
        while (it.hasNext() && countedFiles < 2000) {
            it.next();
            const QFileInfo f = it.fileInfo();
            totalBytes += f.size();
            ++countedFiles;
            if (f.lastModified().isValid() && (!latestModified.isValid() || f.lastModified() > latestModified))
                latestModified = f.lastModified();
        }

        const QString title = sfo.value(QStringLiteral("TITLE"), info.fileName()).trimmed();
        const QString titleId = sfo.value(QStringLiteral("TITLE_ID"), info.fileName()).trimmed().toUpper();
        const QString version = sfo.value(QStringLiteral("APP_VER"), sfo.value(QStringLiteral("VERSION"))).trimmed();

        Entry row;
        row.id = QStringLiteral("gamedata:dir:") + info.fileName();
        row.title = title.isEmpty() ? info.fileName() : title;
        row.detail = version.isEmpty()
            ? formatBytesHuman(totalBytes)
            : QObject::tr("Version %1  \u00B7  %2").arg(version, formatBytesHuman(totalBytes));
        row.container = QStringLiteral("explore_plugin_full");
        row.texture = QStringLiteral("tex_album_icon");
        row.action = Action::Information;
        row.source = QStringLiteral("hdd");

        const QString icon0 = dir.filePath(QStringLiteral("ICON0.PNG"));
        if (QFileInfo(icon0).isFile()) {
            const QImage img = loadBoundedImage(icon0, 512, 288);
            if (!img.isNull()) {
                row.artwork = img;
                row.gameTile = true;
            }
        }
        const QString pic1 = dir.filePath(QStringLiteral("PIC1.PNG"));
        if (QFileInfo(pic1).isFile()) row.backgroundPath = pic1;
        row.previewAudioPath = findPreviewAudio(info.absoluteFilePath());

        QStringList infoRows;
        infoRows << QObject::tr("Title: %1").arg(row.title);
        if (!titleId.isEmpty()) infoRows << QObject::tr("Title ID: %1").arg(titleId);
        if (!version.isEmpty()) infoRows << QObject::tr("Version: %1").arg(version);
        if (!cat.isEmpty()) infoRows << QObject::tr("Category: %1").arg(cat);
        if (latestModified.isValid())
            infoRows << QObject::tr("Updated: %1").arg(latestModified.toString(QStringLiteral("yyyy/MM/dd HH:mm")));
        infoRows << QObject::tr("Size: %1").arg(formatBytesHuman(totalBytes));
        infoRows << QStringLiteral("Path: ") + QDir::toNativeSeparators(info.absoluteFilePath());
        row.value = row.title + QStringLiteral("\n\n") + infoRows.join('\n');
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace xmb
