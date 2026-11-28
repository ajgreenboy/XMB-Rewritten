// Tests for steam_downloads.h: scanSteamDownloads() over a synthetic Steam root (appmanifest_*.acf files, a
// libraryfolders.vdf in both layouts, a steamapps/downloading staging copy), the classification of Steam's
// StateFlags bitmask, the ordering and the 256-row cap, and the Download Management row text/percent
// (steamDownloadStateText, SteamDownload::fraction), which the shell turns into the rows verbatim.
//
// Every fixture is built here from fake ids and "Fixture ..." titles; no real Steam data is read. The live Steam
// registry is switched off (XMB_STEAM_REGISTRY=0) and the Steam root is always passed in or named by
// XMB_STEAM_ROOT, so the machine's own Steam never leaks in. The scratch directory is the first argument
// (QTemporaryDir is avoided: it fails on some machines for an environmental reason).

#include "steam_downloads.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cstdio>

using namespace xmb;

namespace {

int failures = 0;
int checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
void checkText(const QString& got, const QString& want, const char* what) {
    ++checks;
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s\n  want: %s\n  got:  %s\n", what, qPrintable(want), qPrintable(got));
        ++failures;
    }
}

QString scratchRoot;

bool writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return file.write(bytes) == bytes.size();
}

struct Acf {
    int id = 0;
    QByteArray name;          // empty: no "name" key
    QByteArray flags;         // empty: no StateFlags key
    qint64 downloaded = -1;   // -1: key absent
    qint64 toDownload = -1;
    qint64 staged = -1;
    qint64 toStage = -1;
    bool withId = true;
};

QByteArray acf(const Acf& a) {
    QByteArray b = "\"AppState\"\n{\n";
    if (a.withId) b += "\t\"appid\"\t\t\"" + QByteArray::number(a.id) + "\"\n";
    if (!a.name.isEmpty()) b += "\t\"name\"\t\t\"" + a.name + "\"\n";
    if (!a.flags.isEmpty()) b += "\t\"StateFlags\"\t\t\"" + a.flags + "\"\n";
    b += "\t\"installdir\"\t\t\"Dir" + QByteArray::number(a.id) + "\"\n";
    if (a.downloaded >= 0) b += "\t\"BytesDownloaded\"\t\t\"" + QByteArray::number(a.downloaded) + "\"\n";
    if (a.toDownload >= 0) b += "\t\"BytesToDownload\"\t\t\"" + QByteArray::number(a.toDownload) + "\"\n";
    if (a.staged >= 0) b += "\t\"BytesStaged\"\t\t\"" + QByteArray::number(a.staged) + "\"\n";
    if (a.toStage >= 0) b += "\t\"BytesToStage\"\t\t\"" + QByteArray::number(a.toStage) + "\"\n";
    // A nested section must not leak its keys into the app's own pairs.
    b += "\t\"InstalledDepots\"\n\t{\n\t\t\"1\"\n\t\t{\n\t\t\t\"name\"\t\t\"nested-must-be-ignored\"\n\t\t\t\"StateFlags\"\t\t\"99\"\n\t\t}\n\t}\n";
    return b + "}\n";
}

bool putManifest(const QString& library, const Acf& a) {
    return writeFile(library + QStringLiteral("/steamapps/appmanifest_%1.acf").arg(a.id), acf(a));
}

QString makeRoot(const char* name) {
    const QString root = scratchRoot + QLatin1Char('/') + QLatin1String(name);
    QDir(root).removeRecursively();
    QDir().mkpath(root + QStringLiteral("/steamapps"));
    return root;
}

const SteamDownload* find(const QVector<SteamDownload>& list, const char* appId) {
    for (const SteamDownload& d : list)
        if (d.appId == QLatin1String(appId)) return &d;
    return nullptr;
}

QStringList ids(const QVector<SteamDownload>& list) {
    QStringList out;
    for (const SteamDownload& d : list) out << d.appId;
    return out;
}

void classificationTests() {
    const QString root = makeRoot("classify");
    putManifest(root, {1001, "Fixture Downloading", "1032", 500, 1000});             // started + queued, mid download
    putManifest(root, {1002, "Fixture Staging", "1024", 1000, 1000, 120, 1000});     // download done, staging 12%
    putManifest(root, {1003, "Fixture Updating", "1024"});                           // started, no counters
    putManifest(root, {1004, "Fixture Paused", "1536", 100, 1000});                  // paused + started: paused wins
    putManifest(root, {1005, "Fixture Queued", "8"});                                // waiting to start
    putManifest(root, {1006, "Fixture Pending", "6"});                               // installed + update required
    putManifest(root, {1007, "Fixture Installed", "4"});                             // nothing to show
    putManifest(root, {1008, "Fixture Optional", "20"});                             // installed + update optional
    putManifest(root, {1009, "Fixture Missing", "38"});                              // files missing
    putManifest(root, {1010, "Fixture Corrupt", "132"});                             // files corrupt
    putManifest(root, {1011, "Fixture Running", "8196"});                            // installed + app running: not a download
    putManifest(root, {1012, "Fixture Running Flag", "260", 10, 100});               // 256 UpdateRunning counts as active
    putManifest(root, {1013, "Fixture Queued Paused", "520"});                       // queued + paused: paused, not queued
    putManifest(root, {1014, "Fixture Shared Only", "64"});                          // bit 64 alone is not interesting
    QString error = QStringLiteral("stale");
    const auto got = scanSteamDownloads(root, &error);
    check(error.isEmpty(), "a readable root clears the error");

    const auto* d = find(got, "1001");
    check(d && d->active && !d->paused && !d->queued && d->state == "downloading", "1001: started+queued is downloading and NOT queued");
    check(d && d->bytesDownloaded == 500 && d->bytesToDownload == 1000, "1001: byte counters are read");
    checkText(d ? steamDownloadStateText(*d) : QString(), "Downloading 50%", "1001: row text");
    check(d && qAbs(d->fraction() - 0.5) < 1e-9, "1001: fraction is downloaded/total");

    d = find(got, "1002");
    check(d && d->active && d->state == "staging", "1002: download complete but staging incomplete is staging");
    checkText(d ? steamDownloadStateText(*d) : QString(), "Staging 12%", "1002: staging row text uses staged bytes");
    check(d && qAbs(d->fraction() - 0.12) < 1e-9, "1002: fraction follows the staging phase");

    d = find(got, "1003");
    check(d && d->active && d->state == "updating", "1003: started without counters is updating");
    checkText(d ? steamDownloadStateText(*d) : QString(), "Updating", "1003: row text");
    check(d && d->fraction() == 0.0, "1003: no counters means 0 progress");

    d = find(got, "1004");
    check(d && d->paused && !d->active && !d->queued && d->state == "paused", "1004: paused outranks started");
    checkText(d ? steamDownloadStateText(*d) : QString(), "Paused", "1004: row text");

    d = find(got, "1005");
    check(d && d->queued && !d->active && !d->paused && d->state == "queued", "1005: bare Update Queued is queued");
    checkText(d ? steamDownloadStateText(*d) : QString(), "Queued", "1005: row text");

    d = find(got, "1006");
    check(d && !d->active && !d->paused && !d->queued && d->state == "update-pending", "1006: update required on an installed app is pending");
    checkText(d ? steamDownloadStateText(*d) : QString(), "Update pending", "1006: row text");

    check(!find(got, "1007"), "1007: fully installed is not listed");
    check(find(got, "1008") && find(got, "1008")->state == "update-pending", "1008: update optional is pending");
    check(find(got, "1009") && find(got, "1009")->state == "update-pending", "1009: files missing is pending");
    check(find(got, "1010") && find(got, "1010")->state == "update-pending", "1010: files corrupt is pending");
    check(!find(got, "1011"), "1011: an installed app that is merely running is not a download");
    check(find(got, "1012") && find(got, "1012")->active && find(got, "1012")->state == "downloading", "1012: bit 256 marks the app active");
    d = find(got, "1013");
    check(d && d->paused && !d->queued, "1013: queued+paused reports paused only");
    check(!find(got, "1014"), "1014: Shared Only alone is not listed");

    // Order: active -> paused -> queued -> pending, then title (case-insensitive).
    const QStringList wantOrder{"1001", "1012", "1002", "1003", "1004", "1013", "1005", "1010", "1009", "1008", "1006"};
    check(ids(got) == wantOrder, "rows sort active, paused, queued, pending, then by title");
    if (ids(got) != wantOrder) std::fprintf(stderr, "  got order: %s\n", qPrintable(ids(got).join(',')));
}

void percentTests() {
    SteamDownload d;
    d.state = "downloading";
    d.active = true;
    d.bytesToDownload = 1000;
    d.bytesDownloaded = 5000;   // counters can overshoot while Steam rebases
    check(d.fraction() == 1.0, "fraction clamps above 1");
    checkText(steamDownloadStateText(d), "Downloading 99%", "the row never says 100% while still downloading");
    d.bytesDownloaded = 999;
    checkText(steamDownloadStateText(d), "Downloading 99%", "99.9% rounds up to 100 and is shown as 99");
    d.bytesDownloaded = 0;
    checkText(steamDownloadStateText(d), "Downloading 0%", "0 bytes downloaded is 0%");
    d.bytesToDownload = 0;
    checkText(steamDownloadStateText(d), "Downloading", "no byte total: just 'Downloading'");
    d.state = "staging";
    d.bytesToStage = 0;
    checkText(steamDownloadStateText(d), "Staging", "no staging total: just 'Staging'");
    d.bytesToStage = 400;
    d.bytesStaged = 100;
    checkText(steamDownloadStateText(d), "Staging 25%", "staging percent");
    SteamDownload neg;
    neg.state = "downloading";
    neg.bytesToDownload = 1000;
    neg.bytesDownloaded = -50;
    check(neg.fraction() == 0.0, "fraction clamps below 0");
    SteamDownload p;
    p.paused = true;
    p.state = "downloading";
    p.bytesToDownload = 10;
    p.bytesDownloaded = 5;
    checkText(steamDownloadStateText(p), "Paused", "paused text wins over a download state");
    SteamDownload idle;
    idle.state = "idle";
    checkText(steamDownloadStateText(idle), "Update pending", "any other state falls to 'Update pending'");
    SteamDownload stageOnly;   // no download total, only staging counters, state not "staging"
    stageOnly.state = "updating";
    stageOnly.bytesToStage = 200;
    stageOnly.bytesStaged = 50;
    check(qAbs(stageOnly.fraction() - 0.25) < 1e-9, "fraction falls back to staging counters when there is no download total");
}

void parsingTests() {
    const QString root = makeRoot("parsing");
    // Escaped quote and comment in a name; flags valid.
    writeFile(root + "/steamapps/appmanifest_2001.acf",
              "// a comment line\n\"AppState\"\n{\n\t\"appid\"\t\t\"2001\"\n\t\"name\"\t\t\"Fixture \\\"Quoted\\\" Name\"\n\t\"StateFlags\"\t\t\"1026\"\n}\n");
    // No appid key: the file name supplies it.
    writeFile(root + "/steamapps/appmanifest_2002.acf",
              "\"AppState\"\n{\n\t\"name\"\t\t\"Fixture From File Name\"\n\t\"StateFlags\"\t\t\"1024\"\n}\n");
    // No StateFlags: unusable.
    putManifest(root, {2003, "Fixture No Flags", ""});
    // Non-numeric StateFlags: unusable.
    putManifest(root, {2004, "Fixture Bad Flags", "abc"});
    // Zero / non-numeric appid and a file name that gives none: unusable.
    writeFile(root + "/steamapps/appmanifest_zero.acf", "\"AppState\"\n{\n\t\"appid\"\t\t\"0\"\n\t\"StateFlags\"\t\t\"1024\"\n}\n");
    // Truncated file (unterminated string): unusable, and must not crash the scan.
    writeFile(root + "/steamapps/appmanifest_2005.acf", "\"AppState\"\n{\n\t\"appid\"\t\t\"2005\"\n\t\"name\"\t\t\"Fixture Trunc");
    // Empty file.
    writeFile(root + "/steamapps/appmanifest_2006.acf", "");
    // Missing name: the app id is the title.
    putManifest(root, {2007, "", "1024"});
    // Negative / garbage counters read as 0.
    {
        Acf a{2008, "Fixture Bad Counters", "1024"};
        QByteArray bytes = acf(a);
        bytes.replace("\"installdir\"", "\"BytesDownloaded\"\t\t\"-5\"\n\t\"BytesToDownload\"\t\t\"lots\"\n\t\"installdir\"");
        writeFile(root + "/steamapps/appmanifest_2008.acf", bytes);
    }
    // Over the 1 MiB manifest limit: ignored.
    {
        QByteArray big = acf({2009, "Fixture Huge", "1024"});
        big.insert(big.size() - 2, "\t\"pad\"\t\t\"" + QByteArray(1100 * 1024, 'x') + "\"\n");
        writeFile(root + "/steamapps/appmanifest_2009.acf", big);
    }
    // Files that are not manifests are ignored.
    writeFile(root + "/steamapps/other_2010.acf", acf({2010, "Fixture Other", "1024"}));
    const auto got = scanSteamDownloads(root);
    const auto* d = find(got, "2001");
    check(d && d->title == "Fixture \"Quoted\" Name" && d->active, "an escaped quote in the name is unescaped (flags 1026 = started + update required: active)");
    d = find(got, "2002");
    check(d && d->title == "Fixture From File Name" && d->active, "the app id falls back to the manifest file name");
    check(!find(got, "2003"), "a manifest without StateFlags is skipped");
    check(!find(got, "2004"), "a manifest with non-numeric StateFlags is skipped");
    check(!find(got, "0") && !find(got, "zero"), "an app id of 0 is skipped");
    check(!find(got, "2005") && !find(got, "2006"), "truncated and empty manifests are skipped");
    d = find(got, "2007");
    check(d && d->title == "2007" && d->active, "a missing name falls back to the app id");
    d = find(got, "2008");
    check(d && d->bytesDownloaded == 0 && d->bytesToDownload == 0 && d->state == "updating", "negative or non-numeric counters read as 0");
    check(!find(got, "2009"), "a manifest over 1 MiB is skipped");
    check(!find(got, "2010"), "a file that is not appmanifest_*.acf is not scanned");
    // Nested InstalledDepots keys (name, StateFlags=99) never replace the app's own values.
    check(find(got, "2002") && find(got, "2002")->title != "nested-must-be-ignored", "nested sections do not leak into the app's keys");
}

void librariesTests() {
    const QString root = makeRoot("libs-main");
    const QString lib2 = makeRoot("libs-second");
    const QString lib3 = makeRoot("libs-third-oldformat");
    const QString ghost = scratchRoot + "/libs-ghost-does-not-exist";
    putManifest(root, {3001, "Fixture Main", "1024"});
    putManifest(lib2, {3002, "Fixture Second Library", "1024"});
    putManifest(lib3, {3003, "Fixture Old Format Library", "1024"});
    // Current nested format: "libraryfolders" > "<n>" > "path"; one entry repeats the root, one is gone.
    const QByteArray nested =
        "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"" + root.toUtf8() + "\"\n\t\t\"label\"\t\t\"\"\n\t}\n"
        "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"" + lib2.toUtf8() + "\"\n\t\t\"apps\"\n\t\t{\n\t\t\t\"3002\"\t\t\"123\"\n\t\t}\n\t}\n"
        "\t\"2\"\n\t{\n\t\t\"path\"\t\t\"" + ghost.toUtf8() + "\"\n\t}\n}\n";
    writeFile(root + "/steamapps/libraryfolders.vdf", nested);
    auto got = scanSteamDownloads(root);
    check(find(got, "3001") && find(got, "3002") && !find(got, "3003"), "the nested libraryfolders.vdf adds the second library");
    int main = 0;
    for (const auto& d : got) main += d.appId == "3001";
    check(main == 1, "a library that repeats the root is scanned once");
    // Pre-2020 format: numbered keys at the top level whose values are paths.
    const QByteArray old = "\"LibraryFolders\"\n{\n\t\"TimeNextStatsReport\"\t\t\"1\"\n\t\"ContentStatsID\"\t\t\"2\"\n\t\"1\"\t\t\"" + lib3.toUtf8() + "\"\n}\n";
    writeFile(root + "/steamapps/libraryfolders.vdf", old);
    got = scanSteamDownloads(root);
    check(find(got, "3001") && find(got, "3003") && !find(got, "3002"), "the numbered pre-2020 format is read too");
    // No libraryfolders.vdf at all: just the root.
    QFile::remove(root + "/steamapps/libraryfolders.vdf");
    got = scanSteamDownloads(root);
    check(ids(got) == QStringList{"3001"}, "without libraryfolders.vdf only the root is scanned");
}

void stagingCopyTests() {
    const QString root = makeRoot("staging");
    putManifest(root, {4001, "Fixture Staged Copy", "4", 0, 0});   // installed manifest says nothing is happening
    writeFile(root + "/steamapps/downloading/4001/appmanifest_4001.acf", acf({4001, "", "1024", 300, 1000}));
    writeFile(root + "/steamapps/downloading/4002/appmanifest_4002.acf", acf({4002, "Fixture Staging Only", "1536", 10, 100}));
    const auto got = scanSteamDownloads(root);
    const auto* d = find(got, "4001");
    check(d && d->active && d->title == "Fixture Staged Copy" && d->bytesDownloaded == 300 && d->bytesToDownload == 1000 && d->state == "downloading",
          "a steamapps/downloading copy is merged: flags are OR-ed, counters take the larger value, the install title is kept");
    d = find(got, "4002");
    check(d && d->paused, "a manifest that exists only under steamapps/downloading is still reported");
}

void capAndEnvTests() {
    const QString root = makeRoot("cap");
    for (int i = 0; i < 300; ++i) putManifest(root, {5000 + i, QByteArray("Fixture Many ") + QByteArray::number(1000 + i), "1024"});
    const auto got = scanSteamDownloads(root);
    check(got.size() == 256, "the scan returns at most 256 rows");

    // Root resolution: an empty argument tries XMB_STEAM_ROOT first.
    const QString envRoot = makeRoot("env-root");
    putManifest(envRoot, {6001, "Fixture Env Root", "1024"});
    qputenv("XMB_STEAM_ROOT", QDir::toNativeSeparators(envRoot).toLocal8Bit());
    QString error;
    auto viaEnv = scanSteamDownloads(QString(), &error);
    check(find(viaEnv, "6001") && error.isEmpty(), "an empty root argument uses XMB_STEAM_ROOT");
    // An explicit argument beats the environment.
    const QString explicitRoot = makeRoot("explicit-root");
    putManifest(explicitRoot, {6002, "Fixture Explicit Root", "1024"});
    auto viaArg = scanSteamDownloads(explicitRoot);
    check(find(viaArg, "6002") && !find(viaArg, "6001"), "an explicit root beats XMB_STEAM_ROOT");
    // XMB_STEAM_ROOT naming a directory without steamapps: not found, error text set, no fallback to a real Steam.
    const QString bare = scratchRoot + "/no-steamapps";
    QDir().mkpath(bare);
    qputenv("XMB_STEAM_ROOT", QDir::toNativeSeparators(bare).toLocal8Bit());
    error.clear();
    const auto none = scanSteamDownloads(QString(), &error);
    check(none.isEmpty() && !error.isEmpty(), "a root with no steamapps folder is reported as Steam not found");
    qunsetenv("XMB_STEAM_ROOT");
    // An explicit root without steamapps behaves the same.
    error.clear();
    check(scanSteamDownloads(bare, &error).isEmpty() && error.contains("Steam was not found"), "an explicit root without steamapps gives the 'Steam was not found' message");
    // A root with a steamapps folder and nothing downloading: empty, and NO error.
    const QString quiet = makeRoot("quiet");
    putManifest(quiet, {7001, "Fixture Idle", "4"});
    error = "stale";
    check(scanSteamDownloads(quiet, &error).isEmpty() && error.isEmpty(), "nothing downloading is an empty list with no error");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    qputenv("XMB_STEAM_REGISTRY", "0");   // never read the real Steam registry
    qunsetenv("XMB_STEAM_ROOT");
    scratchRoot = QDir::fromNativeSeparators(argc > 1 ? QString::fromLocal8Bit(argv[1])
                                                       : QDir::current().filePath(QStringLiteral("steam-downloads-scratch")));
    QDir(scratchRoot).removeRecursively();
    QDir().mkpath(scratchRoot);
    if (!QFileInfo(scratchRoot).isDir()) {
        std::fprintf(stderr, "cannot create the scratch directory %s\n", qPrintable(scratchRoot));
        return 2;
    }
    classificationTests();
    percentTests();
    parsingTests();
    librariesTests();
    stagingCopyTests();
    capAndEnvTests();
    QDir(scratchRoot).removeRecursively();
    std::printf("steam downloads tests: %d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
