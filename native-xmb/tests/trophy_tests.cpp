#include "trophy_data.h"
#include "assets.h"
#include "shell.h"
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdio>

namespace {

int gFailures = 0;

void check(bool cond, const char* name) {
    if (!cond) {
        std::fprintf(stderr, "FAIL: %s\n", name);
        ++gFailures;
    }
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}

QByteArray buildSyntheticSfo(const QString& title, const QString& titleId, const QString& cat, const QString& ver) {
    const QByteArray k("CATEGORY\0TITLE\0TITLE_ID\0VERSION\0", 32);
    const QByteArray vCat = cat.toUtf8() + '\0';
    const QByteArray vTitle = title.toUtf8() + '\0';
    const QByteArray vId = titleId.toUtf8() + '\0';
    const QByteArray vVer = ver.toUtf8() + '\0';

    const quint32 count = 4;
    const quint32 keysOff = 20 + count * 16;
    const quint32 dataOff = (keysOff + quint32(k.size()) + 3u) & ~3u;
    QByteArray out(qsizetype(dataOff), '\0');
    out.replace(0, 4, QByteArray("\0PSF", 4));
    qToLittleEndian<quint32>(0x00000101u, out.data() + 4);
    qToLittleEndian<quint32>(keysOff, out.data() + 8);
    qToLittleEndian<quint32>(dataOff, out.data() + 12);
    qToLittleEndian<quint32>(count, out.data() + 16);
    out.replace(qsizetype(keysOff), k.size(), k);

    struct Spec { quint16 keyRel; QByteArray val; };
    const Spec specs[] = {
        {0, vCat},
        {9, vTitle},
        {15, vId},
        {24, vVer},
    };
    quint32 rel = 0;
    for (quint32 i = 0; i < count; ++i) {
        const qsizetype e = 20 + qsizetype(i) * 16;
        qToLittleEndian<quint16>(specs[i].keyRel, out.data() + e);
        qToLittleEndian<quint16>(0x0204, out.data() + e + 2);
        qToLittleEndian<quint32>(quint32(specs[i].val.size()), out.data() + e + 4);
        qToLittleEndian<quint32>(quint32(specs[i].val.size()), out.data() + e + 8);
        qToLittleEndian<quint32>(rel, out.data() + e + 12);
        out.append(specs[i].val);
        rel += quint32(specs[i].val.size());
    }
    return out;
}

QByteArray buildSyntheticTropUsr(const QVector<QPair<int, quint64>>& unlockedIdsWithUnixSecs) {
    constexpr quint64 kDeltaSecs = 62135596800ULL;
    const quint32 count = quint32(unlockedIdsWithUnixSecs.size());
    const qsizetype headerSize = 0x30;
    const qsizetype tableHeaderSize = 0x20;
    const quint32 entryPayloadSize = 0x50; // TROPUSREntry6 is 0x60 total: 0x10 header + 0x50 body
    const quint32 stride = 0x60;
    const quint64 tableOffset = quint64(headerSize + tableHeaderSize);

    QByteArray out(qsizetype(tableOffset) + qsizetype(count) * qsizetype(stride), '\0');
    qToBigEndian<quint32>(0x818F54ADu, out.data());
    qToBigEndian<quint32>(1u, out.data() + 8); // tables_count = 1

    // TableHeader (type 6)
    qToBigEndian<quint32>(6u, out.data() + headerSize);
    qToBigEndian<quint32>(entryPayloadSize, out.data() + headerSize + 4);
    qToBigEndian<quint32>(1u, out.data() + headerSize + 8);
    qToBigEndian<quint32>(count, out.data() + headerSize + 12);
    qToBigEndian<quint64>(tableOffset, out.data() + headerSize + 16);

    for (quint32 i = 0; i < count; ++i) {
        const qsizetype rec = qsizetype(tableOffset + quint64(i) * stride);
        const int id = unlockedIdsWithUnixSecs[int(i)].first;
        const quint64 unixSecs = unlockedIdsWithUnixSecs[int(i)].second;
        const bool unlocked = unixSecs > 0;
        const quint64 tick = unlocked ? (unixSecs + kDeltaSecs) * 1000000ULL : 0ULL;
        qToBigEndian<quint32>(6u, out.data() + rec);
        qToBigEndian<quint32>(entryPayloadSize, out.data() + rec + 4);
        qToBigEndian<quint32>(i, out.data() + rec + 8);
        qToBigEndian<quint32>(quint32(id), out.data() + rec + 0x10);
        qToBigEndian<quint32>(unlocked ? 1u : 0u, out.data() + rec + 0x14);
        qToBigEndian<quint64>(tick, out.data() + rec + 0x20);
        qToBigEndian<quint64>(tick, out.data() + rec + 0x28);
    }
    return out;
}

} // namespace

int main() {
    QTemporaryDir temp;
    check(temp.isValid(), "temp-dir-valid");

    const QString flashRoot = temp.path() + QStringLiteral("/dev_flash");
    const QString hddRoot = temp.path() + QStringLiteral("/dev_hdd0");
    QDir().mkpath(flashRoot);
    QDir().mkpath(hddRoot);

    xmb::Assets assets(flashRoot);
    assets.setHddRoot(hddRoot);

    check(!xmb::hasTrophySets(hddRoot), "empty-hdd-has-no-trophies");
    check(!xmb::hasGameDataEntries(hddRoot), "empty-hdd-has-no-gamedata");

    // 1. Synthetic RPCS3 trophy set with binary 16-byte SFM prefix + hidden & unlocked entries.
    const QString setDir = hddRoot + QStringLiteral("/home/00000001/trophy/NPWR00001_00");
    const QByteArray sfmPrefix("TRPC\0\0\0\x01\0\0\0\0\0\0\0\0", 16);
    const QByteArray confXml =
        "<trophyconf version=\"1.1\">\n"
        "  <title-name>Test Trophy Title</title-name>\n"
        "  <title-detail>Synthetic RPCS3 set</title-detail>\n"
        "  <trophy id=\"0\" hidden=\"no\" ttype=\"P\"><name>All Clear</name><detail>Get everything.</detail></trophy>\n"
        "  <trophy id=\"1\" hidden=\"no\" ttype=\"G\"><name>Boss Down</name><detail>Defeat the boss.</detail></trophy>\n"
        "  <trophy id=\"2\" hidden=\"yes\" ttype=\"S\"><name>Secret</name><detail>Find the secret room.</detail></trophy>\n"
        "  <trophy id=\"3\" hidden=\"no\" ttype=\"B\"><name>First Step</name><detail>Start the adventure.</detail></trophy>\n"
        "</trophyconf>\n";
    check(writeBytes(setDir + QStringLiteral("/TROPCONF.SFM"), sfmPrefix + confXml), "write-tropconf");
    check(writeBytes(setDir + QStringLiteral("/TROPUSR.DAT"),
                     buildSyntheticTropUsr({{0, 0}, {1, 1700000000ULL}, {2, 0}, {3, 1699990000ULL}})),
          "write-tropusr");

    check(xmb::hasTrophySets(hddRoot), "detected-trophy-set");
    const auto sets = xmb::scanTrophySets(hddRoot);
    check(sets.size() == 1, "scanned-one-set");
    check(sets[0].titleName == QStringLiteral("Test Trophy Title"), "parsed-title-name");
    check(sets[0].trophies.size() == 4, "parsed-4-trophies");
    check(sets[0].unlockedCount == 2 && sets[0].gold == 1 && sets[0].bronze == 1, "tally-unlocked");
    // Points: G(90) + S(30) + B(15) = 135 total; earned G(90) + B(15) = 105 -> 77%
    check(sets[0].completionPercent() == 77, "completion-percent-77");

    QString colTitle;
    const auto colRows = xmb::trophyCollectionRows(assets, &colTitle);
    check(colRows.size() == 1 && colRows[0].action == xmb::Action::TrophySet, "collection-row-action");
    check(colRows[0].detail.contains(QStringLiteral("77%")) && colRows[0].detail.contains(QStringLiteral("2/4")), "collection-row-detail");

    QString setTitle;
    const auto itemRows = xmb::trophySetRows(assets, QStringLiteral("00000001/NPWR00001_00"), &setTitle);
    check(setTitle == QStringLiteral("Test Trophy Title") && itemRows.size() == 4, "set-rows-count");
    check(itemRows[1].detail.contains(QStringLiteral("Gold")) && itemRows[1].detail.contains(QStringLiteral("Earned")), "gold-unlocked-detail");
    check(itemRows[2].title == QStringLiteral("Hidden Trophy") && itemRows[2].detail.contains(QStringLiteral("Hidden")), "hidden-trophy-masked");

    // Worker-safe forms (no Assets) agree with the Assets wrappers, and a key that tries to leave home/ is refused.
    check(xmb::trophyCollectionRowsAt(hddRoot).size() == colRows.size(), "collection-rows-at-matches");
    QString atTitle;
    const auto atRows = xmb::trophySetRowsAt(hddRoot, QStringLiteral("00000001/NPWR00001_00"), QStringLiteral("Hidden Trophy"), &atTitle);
    check(atRows.size() == itemRows.size() && atTitle == setTitle, "set-rows-at-matches");
    check(xmb::trophySetRowsAt(hddRoot, QStringLiteral("00000001/../../game"), QStringLiteral("H")).isEmpty(), "set-key-traversal-refused");
    check(xmb::trophySetRowsAt(hddRoot, QStringLiteral("..\\x/NPWR00001_00"), QStringLiteral("H")).isEmpty(), "set-key-backslash-refused");
    check(xmb::trophySetRowsAt(hddRoot, QStringLiteral("00000001"), QStringLiteral("H")).isEmpty(), "set-key-missing-comm-refused");

    // 2. Synthetic Game Data Utility folder (CATEGORY=GD) + WAV preview audio.
    const QString gdDir = hddRoot + QStringLiteral("/game/BLUS30001DATA");
    check(writeBytes(gdDir + QStringLiteral("/PARAM.SFO"),
                     buildSyntheticSfo(QStringLiteral("Sample Game Patch"), QStringLiteral("BLUS30001"), QStringLiteral("GD"), QStringLiteral("01.02"))),
          "write-gd-sfo");
    check(writeBytes(gdDir + QStringLiteral("/PIC1.PNG"), QByteArray("stub")), "write-gd-pic1");
    check(writeBytes(gdDir + QStringLiteral("/SND0.WAV"), QByteArray("RIFFtest")), "write-gd-snd0");
    check(xmb::hasGameDataEntries(hddRoot), "detected-game-data");

    QString gdTitle;
    const auto gdRows = xmb::gameDataRows(assets, &gdTitle);
    check(gdRows.size() == 1, "gamedata-one-row");
    check(gdRows[0].title == QStringLiteral("Sample Game Patch"), "gamedata-title");
    check(gdRows[0].detail.contains(QStringLiteral("01.02")), "gamedata-version-in-detail");
    check(!gdRows[0].backgroundPath.isEmpty() && gdRows[0].backgroundPath.endsWith(QStringLiteral("PIC1.PNG")), "gamedata-pic1");
    check(!gdRows[0].previewAudioPath.isEmpty() && gdRows[0].previewAudioPath.endsWith(QStringLiteral("SND0.WAV")), "gamedata-snd0");
    check(xmb::gameDataRowsAt(hddRoot).size() == gdRows.size(), "gamedata-rows-at-matches");

    std::printf("trophy-tests: %s (%d failures)\n", gFailures ? "FAIL" : "PASS", gFailures);
    return gFailures ? 1 : 0;
}
