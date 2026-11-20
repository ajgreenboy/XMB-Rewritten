#include "release_index.h"
#include "release_source.h"
#include "hydra_library.h"
#include "async_identity.h"
#include "real_debrid.h"
#include "preferences.h"
#include "release_job_state.h"
#include "release_archive_cleanup.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QProcess>
#include <QElapsedTimer>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>
#include <cstdio>

namespace {
int checks=0,failures=0;
void check(bool condition,const char* label) {
    ++checks;if(!condition){++failures;std::fprintf(stderr,"FAIL: %s\n",label);}
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    using namespace xmb;
    struct SizeCase { const char* label; double low; double high; double unit; };
    const double mb=1024.0*1024,gb=mb*1024;
    const SizeCase cases[]={
        {"2 GB",2,-1,gb},{"23.3 GB\n",23.3,-1,gb},{"13.3/14.7 GB",13.3,14.7,gb},
        {"4~4.3 GB",4,4.3,gb},{"13,3 GB",13.3,-1,gb},{"2,4 GB",2.4,-1,gb},
        {"19,9 GB",19.9,-1,gb},{"13,38 GB",13.38,-1,gb},{"215/374 MB",215,374,mb},
        {"8,8~9,5 GB SELECTIVE DOWNLOAD",8.8,9.5,gb},{"980 MB/1,3 GB (Selective Download)",980*mb/gb,1.3,gb},
        {"69 MB",69,-1,mb},{"1 KB",1,-1,1024},{"1 TB",1,-1,gb*1024},{"2 B",2,-1,1}};
    for(const auto& item:cases) {
        const auto size=parseSize(QString::fromLatin1(item.label));
        check(size.sizeBytes==qint64(std::round(item.low*item.unit)),item.label);
        check(size.sizeBytesMax==(item.high<0?-1:qint64(std::round(item.high*item.unit))),"size range max");
        check(size.sizeLabel==QString::fromLatin1(item.label),"original size label");
    }
    for(const auto& text:{QStringLiteral(" GB"),QStringLiteral("DIGIT NOT FOUND MB"),QStringLiteral("XX.VIII GB\n"),QStringLiteral("100"),QStringLiteral("999999999999999999 TB")})
        check(parseSize(text).sizeBytes==-1,"unknown/overflow size");
    check(parseSize(QJsonValue()).sizeBytes==-1,"absent size");
    const auto bare=parseMagnet(QStringLiteral("magnet:?xt=urn:btih:ABC012"));
    check(bare.infohash==QStringLiteral("abc012") && bare.trackers.isEmpty() && bare.displayName.isEmpty(),"bare magnet lowercase");
    const auto full=parseMagnet(QStringLiteral("MAGNET:?xt=urn:btmh:CaseSensitive&dn=Test+Game%20One&tr=https%3A%2F%2Fa.invalid&tr=udp%3A%2F%2Fb.invalid"));
    check(full.infohash==QStringLiteral("CaseSensitive") && full.displayName==QStringLiteral("Test Game One") && full.trackers.size()==2,"full v2 magnet");
    check(parseMagnet(QStringLiteral("magnet:?xt=urn:btmh:V2&xt=urn:btih:ABC")).infohash==QStringLiteral("abc"),"v1 preferred over v2");
    check(classifyUri(QStringLiteral("https://example.invalid/file.torrent?x=1"))==QStringLiteral("torrent"),"HTTP torrent priority");
    check(classifyUri(QStringLiteral("file.torrent"))==QStringLiteral("torrent"),"torrent");
    check(classifyUri(QStringLiteral("https://example.invalid/file"))==QStringLiteral("direct"),"direct");
    check(classifyUri(QStringLiteral("other"))==QStringLiteral("unknown"),"unknown");
    check(classifyUri(bare.uri)==QStringLiteral("magnet"),"magnet classification");
    check(asStringArray(QStringLiteral("one")).size()==1,"string array singleton");
    check(asStringArray(QJsonArray{1,QStringLiteral("two"),false}).size()==1,"string array filtering");
    check(asStringArray(QJsonValue()).isEmpty(),"absent string array");
    check(parseDate(QStringLiteral("2026-11-11T12:00:00Z")).isValid(),"ISO date");
    check(parseDate(QStringLiteral("Wed, 11 Nov 2026 12:00:00 +0000")).isValid(),"RFC date");
    check(parseDate(QJsonValue(1234567890.0)).toMSecsSinceEpoch()==1234567890,"millisecond date");
    check(!parseDate(QStringLiteral("not a date")).isValid(),"invalid date");
    const auto mirrors=groupToMirrors(QJsonArray{QJsonArray{QStringLiteral("https://example.invalid/game.part10.rar"),QStringLiteral("https://example.invalid/readme"),QStringLiteral("https://example.invalid/game.part2.rar")},QJsonArray{}});
    check(mirrors.size()==1 && mirrors.first().host==QStringLiteral("example.invalid") && mirrors.first().parts[0].uri.endsWith(QStringLiteral("part2.rar")) && mirrors.first().parts[1].uri.endsWith(QStringLiteral("readme")),"mirror numbered slots");
    const auto heuristic=QJsonDocument::fromJson(R"({"records":[{"label":"Example Game","size":"2 GB","links":["https://example.invalid/file"],"duplicate":"https://example.invalid/file"}]})");
    QString error;
    const auto guessed=parseReleaseIndex(heuristic,&error);
    check(error.isEmpty() && guessed.size()==1 && guessed.first().title==QStringLiteral("Example Game") && guessed.first().downloads.size()==1 && guessed.first().sizeBytes==qint64(2*gb),"heuristic adapter");
    const auto named=parseReleaseIndex(QJsonDocument::fromJson(R"([{"name":"Named Example","date":"2026-11-11T00:00:00Z","size":"215/374 MB"}])"));
    check(named.size()==1 && named.first().title==QStringLiteral("Named Example") && named.first().publishedAt.isValid() && named.first().sizeBytesMax==qint64(374*mb),"heuristic date and named title");
    if(argc>1) {
        QFile fixture(QString::fromLocal8Bit(argv[1]));check(fixture.open(QIODevice::ReadOnly),"fixture read");
        const auto releases=parseReleaseIndex(QJsonDocument::fromJson(fixture.readAll()),&error);
        check(error.isEmpty() && releases.size()==6,"FitGirl fixture six releases");
        for(const auto& release:releases)check(releaseToJson(releaseFromJson(releaseToJson(release)))==releaseToJson(release),"normalized roundtrip");
    }
    QTemporaryDir temporary;check(temporary.isValid(),"temp data directory");
    Preferences preferences(temporary.path());RealDebrid debrid(preferences);
    check(!debrid.hasToken(),"no configured token");
    debrid.unrestrict(QUrl(QStringLiteral("https://example.invalid/file")),&error);
    check(error==QStringLiteral("no Real-Debrid token configured"),"unrestrict missing token");
    debrid.addMagnet(bare.uri,&error);check(error==QStringLiteral("no Real-Debrid token configured"),"addMagnet missing token");
    debrid.torrentInfo(QStringLiteral("id"),&error);check(error==QStringLiteral("no Real-Debrid token configured"),"torrentInfo missing token");
    debrid.instantAvailability(QStringLiteral("abc"),&error);check(error==QStringLiteral("no Real-Debrid token configured"),"availability missing token");
    check(!debrid.selectFiles(QStringLiteral("id"),QStringLiteral("all"),&error) && error==QStringLiteral("no Real-Debrid token configured"),"selectFiles missing token");
    QTcpServer server;check(server.listen(QHostAddress::LocalHost,0),"local download server");
    QObject::connect(&server,&QTcpServer::newConnection,&app,[&] {
        auto* socket=server.nextPendingConnection();
        QObject::connect(socket,&QTcpSocket::readyRead,socket,[socket]{
            socket->readAll();socket->write("HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\nfixture");socket->disconnectFromHost();
        });
        QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
    });
    const QString direct=QStringLiteral("http://127.0.0.1:%1/test.zip").arg(server.serverPort());
    const QJsonObject raw{{QStringLiteral("name"),QStringLiteral("Local Test")},{QStringLiteral("downloads"),QJsonArray{
        QJsonObject{{QStringLiteral("title"),QStringLiteral("CON: Test/Game")},{QStringLiteral("uris"),direct}},
        QJsonObject{{QStringLiteral("title"),QStringLiteral("Magnet Only")},{QStringLiteral("uris"),bare.uri}}}}};
    const QString input=QDir(temporary.path()).filePath(QStringLiteral("index.json"));QFile file(input);
    check(file.open(QIODevice::WriteOnly),"source fixture open");file.write(QJsonDocument(raw).toJson());file.close();
    ReleaseSources store(temporary.path(),preferences);
    check(store.importSource(QUrl::fromLocalFile(input),&error) && error.isEmpty(),"local source import");
    const auto stored=store.listSources(&error);check(stored.size()==1 && stored.first().releases.size()==2 && error.isEmpty(),"offline source readback");
    if(!stored.isEmpty()) {
        check(store.downloadedPath(stored.first().slug,QStringLiteral("CON: Test/Game")).isEmpty(),"undownloaded release has no local path");
        check(store.downloadRelease(stored.first().slug,QStringLiteral("Magnet Only"),&error).isEmpty() && error==QStringLiteral("no Real-Debrid token configured"),"orchestration missing token");
        qint64 received=0;
        const auto path=store.downloadRelease(stored.first().slug,QStringLiteral("CON: Test/Game"),&error,[&](qint64 a,qint64){received=a;});
        QFile downloaded(path);check(!path.isEmpty() && error.isEmpty() && path.endsWith(QStringLiteral(".zip")) && downloaded.open(QIODevice::ReadOnly) && downloaded.readAll()==QByteArray("fixture") && received==7,"direct fallback streaming/progress/safe filename");
        check(store.downloadedPath(stored.first().slug,QStringLiteral("CON: Test/Game"))==path,"download lookup uses the same sanitized title");
        downloaded.close();check(downloaded.remove(),"remove completed fixture");
        check(store.downloadedPath(stored.first().slug,QStringLiteral("CON: Test/Game")).isEmpty(),"deleted download is not treated as local");
    }
    check(preferences.setValue(QStringLiteral("debrid/realDebridToken"),QStringLiteral("synthetic-test-value")),"token preference accepted");
    check(preferences.setValue(QStringLiteral("downloads/lastGameDrive"),QStringLiteral("C:/")),"last-used install drive accepted");
    Preferences reloaded(temporary.path());
    check(reloaded.value(QStringLiteral("debrid/realDebridToken")).toString()==QStringLiteral("synthetic-test-value"),"token preference survives reload");
    check(reloaded.value(QStringLiteral("downloads/lastGameDrive")).toString()==QStringLiteral("C:/"),"last-used install drive survives reload");
    // Background game transfer state machine (release_job_state.h): the Triangle menu's Cancel and the
    // detail-page Cancel row reuse these predicates. Circle/Esc (Back) never cancels a release job: it
    // only navigates and leaves the download running. Pausing must be cancellable (the pause already
    // raised the cancel flag; cancelling then drops the pause flag so the job lands Cancelled, not Paused).
    for(int s=0;s<=int(xmb::ReleaseJobState::Cancelled);++s) {
        const auto st=static_cast<xmb::ReleaseJobState>(s);
        const bool pausable=xmb::releaseJobCanPause(st);
        const bool cancellable=xmb::releaseJobCanCancel(st);
        const bool isActive=xmb::releaseJobActive(st);
        check(pausable==(st==xmb::ReleaseJobState::Queued || st==xmb::ReleaseJobState::Resolving ||
                         st==xmb::ReleaseJobState::Downloading),"releaseJobCanPause set");
        check(cancellable==(pausable || st==xmb::ReleaseJobState::Paused || st==xmb::ReleaseJobState::Extracting ||
                            st==xmb::ReleaseJobState::Pausing),"releaseJobCanCancel set (Pausing cancellable)");
        check(isActive==(st==xmb::ReleaseJobState::Resolving || st==xmb::ReleaseJobState::Downloading ||
                         st==xmb::ReleaseJobState::Pausing || st==xmb::ReleaseJobState::Extracting ||
                         st==xmb::ReleaseJobState::Cancelling),"releaseJobActive set");
    }
    check(xmb::releaseJobCanCancel(xmb::ReleaseJobState::Pausing),"Pausing is cancellable (Triangle Cancel + detail Cancel)");
    check(!xmb::releaseJobCanPause(xmb::ReleaseJobState::Pausing),"Pausing is not re-pausable");
    check(!xmb::releaseJobCanCancel(xmb::ReleaseJobState::Cancelling),"Cancelling is not re-cancellable");
    check(!xmb::releaseJobCanCancel(xmb::ReleaseJobState::Finished) &&
          !xmb::releaseJobCanCancel(xmb::ReleaseJobState::Failed) &&
          !xmb::releaseJobCanCancel(xmb::ReleaseJobState::Cancelled),"terminal states are not cancellable");
    // JobControl flag semantics + the production settle decision (releaseJobSettledState, the mapping
    // pumpReleaseJobs()'s finished handler uses). Cancelling after a pause request must clear the pause
    // flag so the worker settles to Cancelled, not Paused.
    check(xmb::releaseJobSettledState(false,false)==xmb::ReleaseJobState::Failed,"settle: neither -> Failed");
    check(xmb::releaseJobSettledState(true,false)==xmb::ReleaseJobState::Paused,"settle: paused -> Paused");
    check(xmb::releaseJobSettledState(true,true)==xmb::ReleaseJobState::Paused,"settle: pause wins over cancel");
    check(xmb::releaseJobSettledState(false,true)==xmb::ReleaseJobState::Cancelled,"settle: cancel -> Cancelled");
    // releaseJobComplete(): only Finished (registered) and Cancelled (cleaned up) are auto-forgotten from
    // Download Management; every other state - including Failed and Paused - stays for retry/resume.
    for(int s=0;s<=int(xmb::ReleaseJobState::Cancelled);++s) {
        const auto st=static_cast<xmb::ReleaseJobState>(s);
        const bool complete=xmb::releaseJobComplete(st);
        check(complete==(st==xmb::ReleaseJobState::Finished || st==xmb::ReleaseJobState::Cancelled),
              "releaseJobComplete set (Finished and Cancelled only)");
    }
    xmb::JobControl control;
    check(!control.cancel.load() && !control.pausing.load(),"control starts clear");
    check(control.requestPause(),"pause requested");
    check(control.cancel.load() && control.pausing.load(),"pause raises cancel and pausing");
    check(xmb::releaseJobSettledState(control.pausing.load(),control.cancel.load())==xmb::ReleaseJobState::Paused,
          "Pausing worker settles to Paused while the pause stands");
    control.requestCancel();   // the Triangle/detail Cancel during Pausing
    check(control.cancel.load() && !control.pausing.load(),"cancel after pause drops the pause flag");
    check(xmb::releaseJobSettledState(control.pausing.load(),control.cancel.load())==xmb::ReleaseJobState::Cancelled,
          "Pausing -> requestCancel settles to Cancelled");
    xmb::JobControl directControl;directControl.requestCancel();
    check(directControl.cancel.load() && !directControl.pausing.load(),"direct cancel sets cancel only");
    check(!directControl.beginExtraction(),"a cancelled control refuses extraction");
    xmb::JobControl freshControl;
    check(freshControl.beginExtraction(),"a fresh control begins extraction");
    check(!freshControl.requestPause(),"pause is refused once extraction began");
    // Row-id decoding (releaseJobIdFromRow): the list row and the detail page's status row both decode to
    // the job id; foreign prefixes and an empty id are rejected, so the Triangle menu's focus can never
    // pick up a SteamCMD row, a Steam queue row or a malformed id.
    const QString uuid=QStringLiteral("0123456789abcdef0123456789abcdef");
    check(xmb::releaseJobIdFromRow(QStringLiteral("download:release:")+uuid)==uuid,"list row decodes to job id");
    check(xmb::releaseJobIdFromRow(QStringLiteral("download:release:")+uuid+QStringLiteral(":status"))==uuid,"detail status row decodes to job id");
    check(xmb::releaseJobIdFromRow(QStringLiteral("download:steamcmd:7")).isEmpty(),"SteamCMD row is not a release job");
    check(xmb::releaseJobIdFromRow(QStringLiteral("download:1001")).isEmpty(),"Steam queue row is not a release job");
    check(xmb::releaseJobIdFromRow(QStringLiteral("download:release:")).isEmpty(),"empty job id rejected");
    check(xmb::releaseJobIdFromRow(QString()).isEmpty(),"empty row id rejected");
    // Definitive-cancel archive cleanup (release_archive_cleanup.h): a cancelled job must not leave its
    // completed archive orphaned in Download Archives/<id>/. The helper removes only the job-owned
    // artifacts and preserves unrelated content; a pause/failure keeps the archive (the caller never
    // invokes it then), so nothing here affects resume/retry. The scratch root is created directly
    // (QTemporaryDir is unreliable in some sandboxes) so these checks run deterministically.
    {
        const QString games = QDir::tempPath() + QStringLiteral("/xmb-release-cleanup-") +
            QString::number(QCoreApplication::applicationPid());
        QDir(games).removeRecursively();
        check(QDir().mkpath(games),"archive cleanup scratch root");
        const QString id=QStringLiteral("0123456789abcdef0123456789abcdef");
        const QDir dir(QDir(games).filePath(QStringLiteral("Download Archives/")+id));
        check(QDir().mkpath(dir.path()),"archive dir created");
        QFile archive(dir.filePath(QStringLiteral("Broken Archive.zip")));
        check(archive.open(QIODevice::WriteOnly),"archive file written");(void)archive.write("fixture");archive.close();
        QFile partial(dir.filePath(QStringLiteral(".xmb-game.part")));
        (void)partial.open(QIODevice::WriteOnly);(void)partial.write("x");partial.close();
        QFile meta(dir.filePath(QStringLiteral(".xmb-game.resume.json")));
        (void)meta.open(QIODevice::WriteOnly);(void)meta.write("{}");meta.close();
        check(releaseJobDiscardArchive(games,id,archive.fileName()),"definitive cancel discards archive dir");
        check(!QFileInfo::exists(dir.path()),"archive dir removed after cancel");
        // Unrelated content is preserved: the helper removes the owned archive and reports it gone
        // (so the caller clears its stale reference), but leaves the directory (and any unrecognised
        // file) in place.
        const QDir dir2(QDir(games).filePath(QStringLiteral("Download Archives/")+id));
        check(QDir().mkpath(dir2.path()),"second archive dir created");
        QFile keep(dir2.filePath(QStringLiteral("keep.txt")));
        (void)keep.open(QIODevice::WriteOnly);(void)keep.write("user");keep.close();
        QFile arch2(dir2.filePath(QStringLiteral("Game.zip")));
        (void)arch2.open(QIODevice::WriteOnly);(void)arch2.write("z");arch2.close();
        check(releaseJobDiscardArchive(games,id,arch2.fileName()),"owned archive reported gone despite unrelated content");
        check(QFileInfo::exists(dir2.filePath(QStringLiteral("keep.txt"))),"unrelated file preserved");
        check(QFileInfo::exists(dir2.path()),"directory retained for unrelated content");
        check(!QFileInfo::exists(arch2.fileName()),"owned archive still removed");
        // Malformed job id and a relative root are refused without touching anything.
        check(!releaseJobDiscardArchive(games,QStringLiteral("not-an-id"),archive.fileName()),"invalid id refused");
        check(!releaseJobDiscardArchive(QStringLiteral("Download Archives"),id,archive.fileName()),"relative root refused");
        // A missing archive directory is already clean.
        check(releaseJobDiscardArchive(games,QStringLiteral("fedcba9876543210fedcba9876543210"),QString()),
              "absent archive dir is clean");
        // Cancel-during-pause residue: the pause wind-down flushed the checkpoint but never published the
        // archive, so only a partial and its checkpoint exist. An empty archivePath still removes the
        // job-owned partial + checkpoint and drops the directory, so the definitive-cancel cleanup path
        // leaves nothing orphaned (audit/PHASES-GAME-SEARCH-2026-11-17.md, §8).
        {
            const QString id2=QStringLiteral("abcdef0123456789abcdef0123456789");
            const QDir pdir(QDir(games).filePath(QStringLiteral("Download Archives/")+id2));
            check(QDir().mkpath(pdir.path()),"pause-residue archive dir created");
            QFile part(pdir.filePath(QStringLiteral(".xmb-game.part")));
            (void)part.open(QIODevice::WriteOnly);(void)part.write("partial");part.close();
            QFile resume(pdir.filePath(QStringLiteral(".xmb-game.resume.json")));
            (void)resume.open(QIODevice::WriteOnly);(void)resume.write("{}");resume.close();
            check(releaseJobDiscardArchive(games,id2,QString()),"pause-residue partial + checkpoint discarded");
            check(!QFileInfo::exists(pdir.path()),"pause-residue archive dir removed");
        }
#ifdef Q_OS_WIN
        // Junction defense: QFileInfo::isSymLink() is false for an NTFS junction, so the helper must
        // also check isJunction() and refuse to delete through it. mklink /J normally needs no
        // elevation; if this sandbox forbids it, the check is skipped rather than failed.
        {
            const QString jId=QStringLiteral("fedcba9876543210fedcba9876543210");
            const QString jTarget = QDir::tempPath() + QStringLiteral("/xmb-release-cleanup-jtarget-") +
                QString::number(QCoreApplication::applicationPid());
            QDir().mkpath(jTarget);
            QFile jKeep(jTarget + QStringLiteral("/keep.txt"));
            (void)jKeep.open(QIODevice::WriteOnly);(void)jKeep.write("user");jKeep.close();
            const QString jLink = QDir(games).filePath(QStringLiteral("Download Archives/")+jId);
            QProcess mklink;
            mklink.start(QStringLiteral("cmd"),{QStringLiteral("/C"),QStringLiteral("mklink"),QStringLiteral("/J"),
                                               QDir::toNativeSeparators(jLink),QDir::toNativeSeparators(jTarget)});
            mklink.waitForFinished(10000);
            if(mklink.exitCode()==0 && QFileInfo(jLink).exists() && QFileInfo(jLink).isJunction()) {
                check(!releaseJobDiscardArchive(games,jId,QDir(jLink).filePath(QStringLiteral("keep.txt"))),
                      "junction archive dir refused");
                check(QFileInfo::exists(jKeep.fileName()),"junction target content preserved");
            }
            // Remove the junction (reparse point) first so removeRecursively never walks through it.
            QDir().rmdir(jLink);
            QDir(games).removeRecursively();
            QDir(jTarget).removeRecursively();
        }
#endif
        QDir(games).removeRecursively();
    }
    // JSON game-source catalogue semantics (hydra_library): every source is included (direct and
    // torrent), a game is listed once per distinct name, each source contributes one option carrying
    // its display name and download size, and editions that differ in name stay separate.
    {
        QVector<HydraSource> sources;
        HydraSource alpha; alpha.title=QStringLiteral("Alpha"); alpha.installs=100;
        HydraSource beta;  beta.title =QStringLiteral("Beta");  beta.installs =10;
        sources.append(alpha); sources.append(beta);
        const auto record=[&](const QString& name,const QString& source,const QString& url,
                              const QString& size,const QString& originalTitle) {
            GameRecord r; r.name=name; r.source=source; r.sourceUrl=url; r.fileSize=size;
            r.originalTitle=originalTitle; r.title=originalTitle.isEmpty()?name:originalTitle;
            return r;
        };
        QVector<GameRecord> records;
        records.append(record(QStringLiteral("Game One"),QStringLiteral("Beta"),QStringLiteral("https://beta.invalid/f.json"),QStringLiteral("9 GB"),QStringLiteral("Game One")));
        records.append(record(QStringLiteral("Game One"),QStringLiteral("Alpha"),QStringLiteral("https://alpha.invalid/f.json"),QStringLiteral("8 GB"),QStringLiteral("Game One")));
        records.append(record(QStringLiteral("Game One"),QStringLiteral("Alpha"),QStringLiteral("https://alpha.invalid/f.json"),QStringLiteral("8 GB"),QStringLiteral("Game One"))); // duplicate
        records.append(record(QStringLiteral("Game One — Deluxe Edition"),QStringLiteral("Alpha"),QStringLiteral("https://alpha.invalid/f.json"),QStringLiteral("10 GB"),QStringLiteral("Game One — Deluxe Edition")));
        records.append(record(QString(),QStringLiteral("Alpha"),QStringLiteral("https://alpha.invalid/f.json"),QStringLiteral("1 GB"),QString())); // empty name: dropped
        const auto catalog=buildGameCatalog(records,sources);
        check(catalog.size()==2,"catalogue merges duplicates by name and keeps editions apart");
        const GameResult* base=nullptr; const GameResult* deluxe=nullptr;
        for(const auto& game:catalog) {
            if(game.name==QStringLiteral("Game One"))base=&game;
            else if(game.name==QStringLiteral("Game One — Deluxe Edition"))deluxe=&game;
        }
        check(base!=nullptr && deluxe!=nullptr,"base and deluxe editions both listed once");
        if(base) {
            check(base->options.size()==2,"duplicate same-source record deduplicated to one option");
            bool hasAlpha=false,hasBeta=false;
            for(const auto& option:base->options) {
                if(option.source==QStringLiteral("Alpha") && option.fileSize==QStringLiteral("8 GB"))hasAlpha=true;
                if(option.source==QStringLiteral("Beta")  && option.fileSize==QStringLiteral("9 GB"))hasBeta=true;
            }
            check(hasAlpha && hasBeta,"each option carries its source name and download size");
            check(base->options.first().source==QStringLiteral("Beta"),"options preserve first-seen order");
        }
        if(deluxe) {
            check(deluxe->options.size()==1 && deluxe->options.first().fileSize==QStringLiteral("10 GB"),
                  "edition keeps its own source option and size");
        }
    }
    {
        // Unknown/empty size passes through untouched (the shell renders "Size unknown"); a record
        // whose sourceUrl is absent still deduplicates by source name.
        QVector<GameRecord> records;
        GameRecord r; r.name=QStringLiteral("Sized"); r.source=QStringLiteral("Host"); r.fileSize=QString(); r.title=QStringLiteral("Sized");
        records.append(r);
        GameRecord dup=r; dup.fileSize=QStringLiteral(""); records.append(dup);
        const auto catalog=buildGameCatalog(records,{});
        check(catalog.size()==1 && catalog.first().options.size()==1 && catalog.first().options.first().fileSize.isEmpty(),
              "empty size is preserved and same-source (no url) duplicates collapse");
    }
    {
        // The popular feed fans out one /games response per hot title. A fixture with forty
        // distinct source URLs proves the merge does not cap or silently discard choices when
        // those responses arrive through the bounded worker pool.
        QVector<GameRecord> records;
        QVector<HydraSource> sources;
        for(int i=0;i<40;++i) {
            HydraSource source;source.title=QStringLiteral("Fixture Source %1").arg(i);sources.append(source);
            GameRecord record;record.name=QStringLiteral("Forty Source Game");
            record.title=record.name;record.source=sources.last().title;
            record.sourceUrl=QStringLiteral("https://fixture.invalid/source-%1.json").arg(i);
            record.fileSize=QStringLiteral("%1 GB").arg(i+1);records.append(record);
        }
        const auto catalog=buildGameCatalog(records,sources);
        check(catalog.size()==1 && catalog.first().options.size()==40,
              "forty-source fixture keeps every source choice");
        check(catalog.first().options.first().source==QStringLiteral("Fixture Source 0") &&
              catalog.first().options.last().fileSize==QStringLiteral("40 GB"),
              "forty-source fixture preserves deterministic response order and sizes");
    }
    {
        check(asyncIdentityMatches(4,4,8,8,12,12),"async identity accepts current page request");
        check(!asyncIdentityMatches(4,5,8,8,12,12) && !asyncIdentityMatches(4,4,8,9,12,12) &&
              !asyncIdentityMatches(4,4,8,8,12,13),"async identity rejects stale page/request/tile");
    }
    {
        // Sources pagination: parse one page and decide whether to keep fetching, so no source is
        // silently dropped past page one.
        const auto page1=parseSourcesPage(QJsonDocument::fromJson(
            R"({"total":3,"limit":2,"sources":[{"title":"One","url":"https://one.invalid/f.json","stats":{"installs":1}},{"title":"Two","url":"https://two.invalid/f.json","stats":{"installs":2}}]})").object());
        check(page1.sources.size()==2 && page1.total==3 && page1.limit==2,"sources page parse");
        check(page1.sources[0].title==QStringLiteral("One") && page1.sources[0].installs==1,"source title and installs parsed");
        check(hasMoreSources(1,page1.sources.size(),page1),"more sources after a partial page");
        const auto page2=parseSourcesPage(QJsonDocument::fromJson(
            R"({"total":3,"limit":2,"sources":[{"title":"Three","url":"https://three.invalid/f.json","stats":{"installs":3}}]})").object());
        check(!hasMoreSources(2,3,page2),"pagination stops once accumulated reaches total");
        const auto emptyPage=parseSourcesPage(QJsonDocument::fromJson(R"({"total":5,"limit":2,"sources":[]})").object());
        check(!hasMoreSources(1,0,emptyPage),"an empty page stops pagination");
        const auto zeroLimit=parseSourcesPage(QJsonDocument::fromJson(R"({"total":5,"limit":0,"sources":[{"title":"X","url":"https://x.invalid/f.json"}]})").object());
        check(!hasMoreSources(1,1,zeroLimit),"a zero limit stops pagination");
        auto capped=parseSourcesPage(QJsonDocument::fromJson(R"({"total":200,"limit":100,"sources":[]})").object());
        capped.sources.append(HydraSource{});
        check(!hasMoreSources(100,100,capped),"pagination is capped at 100 pages");
    }
    {
        // Deterministic local HTTP fixture for the progressive popular feed: forty hot entries are
        // returned in ranking order, including one normalized duplicate and a separate edition.
        // Game responses complete out of order, title 39 is deliberately
        // slow, and title 7 fails once before succeeding on retry. This exercises the same public
        // Hydra calls used by the Store without real network or game media.
        QTcpServer fixture;
        check(fixture.listen(QHostAddress::LocalHost,0),"popular HTTP fixture listen");
        QHash<int,int> attempts;
        int rateLimitRequests=0;
        QObject::connect(&fixture,&QTcpServer::newConnection,&app,[&] {
            auto* socket=fixture.nextPendingConnection();
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket] {
                const QByteArray request=socket->readAll();
                int title=-1;
                for(int i=0;i<40;++i)if(request.contains(QStringLiteral("Hot%20%1").arg(i,2,10,QChar('0')).toLatin1())){title=i;break;}
                const bool hot=request.contains("/hot") || request.contains("-hot");
                const bool sources=request.contains("/sources");
                const bool search=request.contains("/search");
                const bool duplicate=request.contains("hot%20%20%20");
                QByteArray body;
                int delay=hot?0:(title==39?900:(title%5)*15);
                bool fail=false;
                if(!hot && title==7)fail=++attempts[title]==1;
                if(!hot && duplicate)fail=++attempts[-1]==1;
                if(sources) {
                    if(request.contains("/sources-malformed"))
                        body=QByteArrayLiteral("{\"total\":1,\"limit\":100,\"sources\":[{\"title\":\"Good\",\"url\":\"https://good.invalid/index.json\"},null]}");
                    else if(request.contains("/sources-empty"))
                        body=QByteArrayLiteral("{\"total\":0,\"limit\":100,\"sources\":[]}");
                    else
                        body=QByteArrayLiteral("{\"total\":2,\"limit\":100,\"sources\":[{\"title\":\"Fixture Source A\",\"url\":\"https://fixture.invalid/a.json\",\"stats\":{\"installs\":3}},{\"title\":\"Fixture Source B\",\"url\":\"https://fixture.invalid/b.json\",\"stats\":{\"installs\":2}}]}");
                } else if(search) {
                    if(request.contains("/search-malformed"))
                        body=QByteArrayLiteral("{\"filteredCount\":2,\"games\":[{\"name\":\"Good Game\",\"source\":\"Fixture Source A\",\"sourceUrl\":\"https://fixture.invalid/a.json\"},null]}");
                    else if(request.contains("/search-empty"))
                        body=QByteArrayLiteral("{\"filteredCount\":0,\"games\":[]}");
                    else
                        body=QByteArrayLiteral("{\"filteredCount\":2,\"games\":[{\"name\":\"Fixture Search Game\",\"title\":\"Fixture Search Game\",\"source\":\"Fixture Source A\",\"sourceUrl\":\"https://fixture.invalid/a.json\",\"fileSize\":\"1 GB\"},{\"name\":\"Fixture Search Game\",\"title\":\"Fixture Search Game\",\"source\":\"Fixture Source B\",\"sourceUrl\":\"https://fixture.invalid/b.json\",\"fileSize\":\"2 GB\"}]}");
                } else if(hot && request.contains("/error-hot")) {
                    fail=true;
                } else if(hot && request.contains("/malformed-hot")) {
                    body=QByteArrayLiteral("{malformed");
                } else if(hot && request.contains("/wrong-hot")) {
                    body=QByteArrayLiteral("{}");
                } else if(hot && request.contains("/empty-hot")) {
                    body=QByteArrayLiteral("[]");
                } else if(hot) {
                    QJsonArray rows;
                    for(int i=0;i<40;++i) {
                        QString title=QStringLiteral("Hot %1").arg(i,2,10,QChar('0'));
                        if(i==11)title=QStringLiteral("  hot   10  ");
                        if(i==12)title=QStringLiteral("Hot 10 — Deluxe Edition");
                        rows.append(QJsonObject{{QStringLiteral("title"),title}});
                    }
                    body=QJsonDocument(rows).toJson(QJsonDocument::Compact);
                } else if(!fail && title==20) {
                    fail=true;
                } else if(!fail && title==21) {
                    ++rateLimitRequests;
                    QTimer::singleShot(delay,socket,[socket] {
                        const QByteArray response=QByteArrayLiteral("HTTP/1.1 429 Too Many Requests\r\nRetry-After: 7\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}");
                        socket->write(response);socket->disconnectFromHost();
                    });
                    return;
                } else if(!fail && title==22) {
                    body=QByteArrayLiteral("{malformed");
                } else if(!fail && title==23) {
                    body=QByteArrayLiteral("{\"games\":{}}");
                } else if(!fail && title==24) {
                    body=QJsonDocument(QJsonObject{{QStringLiteral("games"),QJsonArray{}}}).toJson(QJsonDocument::Compact);
                } else if(!fail && title==25) {
                    body=QByteArrayLiteral("{\"games\":[{\"source\":\"Fixture Direct\",\"sourceUrl\":\"https://fixture/direct.json\"},null]}");
                } else if(!fail) {
                    const bool deluxe=request.contains("Deluxe");
                    const QString first=deluxe?QStringLiteral("Fixture Deluxe A"):(duplicate?QStringLiteral("Fixture Duplicate Direct"):QStringLiteral("Fixture Direct"));
                    const QString firstUrl=deluxe?QStringLiteral("https://fixture/deluxe-a.json"):(duplicate?QStringLiteral("https://fixture/duplicate-direct.json"):QStringLiteral("https://fixture/direct.json"));
                    const QString second=deluxe?QStringLiteral("Fixture Deluxe B"):(duplicate?QStringLiteral("Fixture Duplicate Torrent"):QStringLiteral("Fixture Torrent"));
                    const QString secondUrl=deluxe?QStringLiteral("https://fixture/deluxe-b.json"):(duplicate?QStringLiteral("https://fixture/duplicate-torrent.json"):QStringLiteral("https://fixture/torrent.json"));
                    body=QJsonDocument(QJsonObject{{QStringLiteral("games"),QJsonArray{
                        QJsonObject{{QStringLiteral("source"),first},{QStringLiteral("sourceUrl"),firstUrl},{QStringLiteral("fileSize"),QStringLiteral("1 GB")}},
                        QJsonObject{{QStringLiteral("source"),second},{QStringLiteral("sourceUrl"),secondUrl},{QStringLiteral("fileSize"),QStringLiteral("2 GB")}}
                    }}}).toJson(QJsonDocument::Compact);
                }
                QTimer::singleShot(delay,socket,[socket,body,fail] {
                    const QByteArray status=fail?QByteArrayLiteral("500 Internal Server Error"):QByteArrayLiteral("200 OK");
                    const QByteArray response="HTTP/1.1 "+status+"\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body;
                    socket->write(response);socket->disconnectFromHost();
                });
            });
            QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        });
        const QString base=QStringLiteral("http://127.0.0.1:%1").arg(fixture.serverPort());
        qputenv("XMB_HYDRA_TEST_FIXTURE","1");
        qputenv("XMB_HYDRA_HOT_URL",(base+QStringLiteral("/hot")).toUtf8());
        qputenv("XMB_HYDRA_GAMES_URL",(base+QStringLiteral("/games")).toUtf8());
        QVector<PopularTitle> titles;QString fixtureError;
        check(HydraLibrary().popularTitles(&titles,&fixtureError) && titles.size()==40,"HTTP fixture returns forty raw hot entries");
        check(titles.first().title==QStringLiteral("Hot 00") && titles.last().title==QStringLiteral("Hot 39"),"hot order is deterministic");
        const auto groups=groupPopularTitles(titles);
        check(groups.size()==39 && groups[10].entries.size()==2 && groups[11].entries.size()==1,
              "duplicate hot entries group into one tile while edition stays separate");
        QThreadPool pool;pool.setMaxThreadCount(6);
        QVector<QFuture<GameResult>> futures;
        for(const auto& title:titles)futures.append(QtConcurrent::run(&pool,[title]{GameResult game;HydraLibrary().popularGame(title,&game,nullptr);return game;}));
        QElapsedTimer clock;clock.start();bool early=false;
        for(;;) {
            int done=0;for(const auto& future:futures)if(future.isFinished())++done;
            if(done>0 && clock.elapsed()<900)early=true;
            if(done==futures.size())break;
            QCoreApplication::processEvents(QEventLoop::AllEvents,10);
        }
        check(early,"at least one game completes before the slow response");
        int complete=0;for(const auto& future:futures)if(future.result().options.size()==2)++complete;
        check(complete==32 && futures[11].result().options.isEmpty(),"failed or empty titles are not exposed with partial source options");
        GameResult retry;check(HydraLibrary().popularGame(titles[7],&retry,&fixtureError) && retry.options.size()==2,"failed title succeeds on explicit retry");
        GameResult baseGame,duplicateGame,deluxe;
        check(HydraLibrary().popularGame(titles[10],&baseGame,&fixtureError) && baseGame.options.size()==2 &&
              baseGame.options[0].source==QStringLiteral("Fixture Direct") && baseGame.options[1].source==QStringLiteral("Fixture Torrent"),
              "base raw title keeps exact all-source order");
        check(HydraLibrary().popularGame(titles[11],&duplicateGame,&fixtureError) && duplicateGame.options.size()==2 &&
              duplicateGame.options[0].source==QStringLiteral("Fixture Duplicate Direct") && duplicateGame.options[1].source==QStringLiteral("Fixture Duplicate Torrent"),
              "duplicate raw title returns distinct source records");
        const auto merged=mergePopularGameResults(groups[10].name,groups[10].coverUrl,{baseGame,duplicateGame});
        check(merged.options.size()==4 && merged.options[0].source==QStringLiteral("Fixture Direct") &&
              merged.options[1].source==QStringLiteral("Fixture Torrent") && merged.options[2].source==QStringLiteral("Fixture Duplicate Direct") &&
              merged.options[3].source==QStringLiteral("Fixture Duplicate Torrent") && merged.options[3].fileSize==QStringLiteral("2 GB"),
              "duplicate hot tile unions all source records in stable raw and record order");
        check(HydraLibrary().popularGame(titles[12],&deluxe,&fixtureError) && deluxe.options.size()==2 &&
              deluxe.options[0].source==QStringLiteral("Fixture Deluxe A") && deluxe.options[1].source==QStringLiteral("Fixture Deluxe B"),
              "edition keeps its distinct exact all-source order");
        GameResult invalid; fixtureError.clear();
        check(!HydraLibrary().popularGame(titles[20],&invalid,&fixtureError) && invalid.options.isEmpty() &&
              fixtureError.contains(QStringLiteral("HTTP 500")) && fixtureError.contains(QStringLiteral("retry")) &&
              !fixtureError.contains(QStringLiteral("127.0.0.1")),"HTTP 500 is actionable without URL leakage");
        const int rateLimitBefore=rateLimitRequests;
        fixtureError.clear();
        check(!HydraLibrary().popularGame(titles[21],&invalid,&fixtureError) && fixtureError.contains(QStringLiteral("HTTP 429")),
              "HTTP 429 is surfaced as retryable");
        check(rateLimitRequests==rateLimitBefore+1,"429 Retry-After does not trigger an automatic retry");
        fixtureError.clear();
        check(!HydraLibrary().popularGame(titles[22],&invalid,&fixtureError) && fixtureError.contains(QStringLiteral("invalid JSON")),
              "malformed JSON is not treated as an empty game list");
        fixtureError.clear();
        check(!HydraLibrary().popularGame(titles[23],&invalid,&fixtureError) && fixtureError.contains(QStringLiteral("invalid games response")),
              "wrong top-level shape is rejected");
        GameResult emptyGame;fixtureError=QStringLiteral("stale error");
        check(HydraLibrary().popularGame(titles[24],&emptyGame,&fixtureError) && emptyGame.options.isEmpty() && fixtureError.isEmpty(),
              "valid empty games response clears a previous error");
        check(!HydraLibrary().popularGame(titles[25],&invalid,&fixtureError) && invalid.options.isEmpty() &&
              fixtureError.contains(QStringLiteral("invalid game entry")),
              "one malformed game member fails the entire popular source result");
        const auto progressiveFuture=QtConcurrent::run([] {
            QVector<GameResult> games;QString error;
            HydraLibrary().popularGames(&games,&error);
            return qMakePair(games,error);
        });
        while(!progressiveFuture.isFinished())QCoreApplication::processEvents(QEventLoop::AllEvents,10);
        const auto progressiveResult=progressiveFuture.result();
        check(progressiveResult.first.isEmpty() && !progressiveResult.second.isEmpty(),
              "progressive catalogue does not publish partial tiles after a source failure");
        qputenv("XMB_HYDRA_HOT_URL",(base+QStringLiteral("/empty-hot")).toUtf8());
        titles.clear();fixtureError=QStringLiteral("stale error");
        const bool emptyHotOk=HydraLibrary().popularTitles(&titles,&fixtureError);
        check(emptyHotOk && titles.isEmpty() && fixtureError.isEmpty(),
              "valid empty hot catalogue clears a previous error");
        for(const auto& path:{QStringLiteral("/error-hot"),QStringLiteral("/malformed-hot"),QStringLiteral("/wrong-hot")}) {
            qputenv("XMB_HYDRA_HOT_URL",(base+path).toUtf8());titles.clear();fixtureError.clear();
            check(!HydraLibrary().popularTitles(&titles,&fixtureError) && titles.isEmpty() && !fixtureError.isEmpty(),
                  "invalid hot catalogue response is actionable");
        }
        qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources")).toUtf8());
        qputenv("XMB_HYDRA_SEARCH_URL",(base+QStringLiteral("/search")).toUtf8());
        QVector<HydraSource> fixtureSources;fixtureError=QStringLiteral("stale error");
        check(HydraLibrary().listSources(&fixtureSources,&fixtureError) && fixtureSources.size()==2 && fixtureError.isEmpty(),
              "public listSources returns all valid fixture sources and clears prior error");
        qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources-malformed")).toUtf8());
        fixtureSources.append(HydraSource{});fixtureError.clear();
        check(!HydraLibrary().listSources(&fixtureSources,&fixtureError) && fixtureSources.isEmpty() &&
              fixtureError.contains(QStringLiteral("invalid source entry")),
              "malformed source member fails the entire sources response");
        qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources-empty")).toUtf8());
        fixtureSources.append(HydraSource{});fixtureError=QStringLiteral("stale error");
        check(HydraLibrary().listSources(&fixtureSources,&fixtureError) && fixtureSources.isEmpty() && fixtureError.isEmpty(),
              "valid empty sources array clears a previous error");
        qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources")).toUtf8());
        QVector<GameResult> searchResults;fixtureError=QStringLiteral("stale error");
        check(HydraLibrary().searchGames(QStringLiteral("fixture"),&searchResults,&fixtureError) && searchResults.size()==1 &&
              searchResults.first().options.size()==2 && fixtureError.isEmpty(),
              "public searchGames returns the complete all-source result");
        qputenv("XMB_HYDRA_SEARCH_URL",(base+QStringLiteral("/search-malformed")).toUtf8());
        searchResults.append(GameResult{});fixtureError.clear();
        check(!HydraLibrary().searchGames(QStringLiteral("fixture"),&searchResults,&fixtureError) && searchResults.isEmpty() &&
              fixtureError.contains(QStringLiteral("invalid game entry")),
              "malformed search game member fails without publishing partial records");
        qputenv("XMB_HYDRA_SEARCH_URL",(base+QStringLiteral("/search-empty")).toUtf8());
        searchResults.append(GameResult{});fixtureError=QStringLiteral("stale error");
        check(HydraLibrary().searchGames(QStringLiteral("fixture"),&searchResults,&fixtureError) && searchResults.isEmpty() && fixtureError.isEmpty(),
              "valid empty search games array clears a previous error");
        qunsetenv("XMB_HYDRA_TEST_FIXTURE");qunsetenv("XMB_HYDRA_HOT_URL");qunsetenv("XMB_HYDRA_GAMES_URL");
        qunsetenv("XMB_HYDRA_SOURCES_URL");qunsetenv("XMB_HYDRA_SEARCH_URL");fixture.close();
    }
    {
        // Malformed source JSON: a null document and unparseable bytes report an error; an empty
        // object yields no releases instead of fabricating one.
        QString catalogError;
        check(parseReleaseIndex(QJsonDocument(),&catalogError).isEmpty() && !catalogError.isEmpty(),"null release index reports an error");
        const auto malformed=QJsonDocument::fromJson(QByteArrayLiteral("{not json"));
        check(malformed.isNull(),"unparseable JSON is null");
        catalogError.clear();
        check(parseReleaseIndex(malformed,&catalogError).isEmpty() && catalogError==QStringLiteral("Invalid release index JSON"),
              "malformed JSON reports 'Invalid release index JSON'");
        check(parseReleaseIndex(QJsonDocument::fromJson(QByteArrayLiteral("{}"))).isEmpty(),"empty object yields no releases");
    }
    std::printf("Release backend: %d checks, %d failures\n",checks,failures);return failures?2:0;
}
