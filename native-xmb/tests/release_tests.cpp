#include "release_index.h"
#include "release_source.h"
#include "hydra_library.h"
#include "hydra_catalogue_cache.h"
#include "async_identity.h"
#include "real_debrid.h"
#include "preferences.h"
#include "release_job_state.h"
#include "release_archive_cleanup.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QProcess>
#include <QElapsedTimer>
#include <QTimer>
#include <QSharedPointer>
#include <QtConcurrent/QtConcurrentRun>
#include <cmath>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib,"psapi.lib")
#endif

namespace {
int checks=0,failures=0;
void check(bool condition,const char* label) {
    ++checks;if(!condition){++failures;std::fprintf(stderr,"FAIL: %s\n",label);}
}
// Peak working set of this process in bytes (0 where unsupported), to bound a large-feed parse.
qint64 peakWorkingSetBytes() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters{};
    if(GetProcessMemoryInfo(GetCurrentProcess(),&counters,sizeof(counters)))return qint64(counters.PeakWorkingSetSize);
#endif
    return 0;
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
    const auto torrentQuery=parseReleaseIndex(QJsonDocument::fromJson(R"([{"name":"Torrent Query","links":["https://example.invalid/game.torrent?download=1"]}])"));
    check(torrentQuery.size()==1 && torrentQuery.first().downloads.size()==1 && torrentQuery.first().downloads.first().kind==QStringLiteral("torrent"),"heuristic torrent query link");
    const auto torrentText=parseReleaseIndex(QJsonDocument::fromJson(R"([{"name":"readme.torrent?download=1","links":["game.torrent?download=1"],"note":"title.torrent?download=1"}])"));
    check(torrentText.size()==1 && torrentText.first().downloads.isEmpty(),"heuristic rejects bare torrent-looking text");
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
    std::atomic_bool slowSeen{false};
    std::atomic_bool boundedGoodSeen{false};
    std::atomic_bool resumeBSawRange{false};
    QObject::connect(&server,&QTcpServer::newConnection,&app,[&] {
        auto* socket=server.nextPendingConnection();
        QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket]{
            const QByteArray request=socket->readAll();
            const bool bad=request.contains(" /bad");
            if(request.contains(" /bounded-good"))boundedGoodSeen.store(true);
            if(request.contains(" /slow")) {
                slowSeen.store(true);
                QTimer::singleShot(5000,socket,[socket] {
                    socket->write("HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\nfixture");
                    socket->disconnectFromHost();
                });
                return;
            }
            if(request.contains(" /resume-a")) {
                socket->write("HTTP/1.1 200 OK\r\nETag: \"a\"\r\nContent-Length: 14\r\nConnection: close\r\n\r\npartial");
                socket->flush();
                QTimer::singleShot(5000,socket,[socket] {
                    socket->write("payload");socket->disconnectFromHost();
                });
                return;
            }
            if(request.contains(" /resume-b")) {
                const bool ranged=request.contains("Range: bytes=");
                resumeBSawRange.store(ranged);
                const QByteArray body=QByteArrayLiteral("B-PAYLOAD");
                const bool validRange=ranged && request.contains("If-Range: \"b\"");
                const QByteArray status=validRange?QByteArrayLiteral("206 Partial Content"):QByteArrayLiteral("200 OK");
                const QByteArray range=validRange?QByteArrayLiteral("Content-Range: bytes 4-8/9\r\n"):QByteArray();
                socket->write("HTTP/1.1 "+status+"\r\nETag: \"b\"\r\n"+range+"Content-Length: "+QByteArray::number(validRange?5:body.size())+"\r\nConnection: close\r\n\r\n"+(validRange?body.mid(4):body));
                socket->disconnectFromHost();return;
            }
            QByteArray status=QByteArrayLiteral("200 OK");
            if(bad || request.contains(" /status404"))status=QByteArrayLiteral("404 Not Found");
            else if(request.contains(" /status500"))status=QByteArrayLiteral("500 Internal Server Error");
            else if(request.contains(" /status503"))status=QByteArrayLiteral("503 Service Unavailable");
            else if(request.contains(" /status401"))status=QByteArrayLiteral("401 Unauthorized");
            else if(request.contains(" /status403"))status=QByteArrayLiteral("403 Forbidden");
            else if(request.contains(" /redirect")) {
                socket->write("HTTP/1.1 302 Found\r\nLocation: file:///not-a-download.zip\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                socket->disconnectFromHost();return;
            }
            const QByteArray body=status==QByteArrayLiteral("200 OK")?QByteArrayLiteral("fixture"):QByteArray();
            socket->write("HTTP/1.1 "+status+"\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);
            socket->disconnectFromHost();
        });
        QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
    });
    const QString direct=QStringLiteral("http://127.0.0.1:%1/test.zip").arg(server.serverPort());
    const QString badDirect=QStringLiteral("http://127.0.0.1:%1/bad.zip").arg(server.serverPort());
    const QString goodFallback=QStringLiteral("http://127.0.0.1:%1/good.zip").arg(server.serverPort());
    const QJsonObject raw{{QStringLiteral("name"),QStringLiteral("Local Test")},{QStringLiteral("downloads"),QJsonArray{
        QJsonObject{{QStringLiteral("title"),QStringLiteral("CON: Test/Game")},{QStringLiteral("uris"),direct}},
        QJsonObject{{QStringLiteral("title"),QStringLiteral("Fallback Test")},{QStringLiteral("uris"),QJsonArray{badDirect,goodFallback}}},
        QJsonObject{{QStringLiteral("title"),QStringLiteral("Magnet Only")},{QStringLiteral("uris"),bare.uri}}}}};
    const QString input=QDir(temporary.path()).filePath(QStringLiteral("index.json"));QFile file(input);
    check(file.open(QIODevice::WriteOnly),"source fixture open");file.write(QJsonDocument(raw).toJson());file.close();
    ReleaseSources store(temporary.path(),preferences);
    check(store.importSource(QUrl::fromLocalFile(input),&error) && error.isEmpty(),"local source import");
    const auto stored=store.listSources(&error);check(stored.size()==1 && stored.first().releases.size()==3 && error.isEmpty(),"offline source readback");
    if(!stored.isEmpty()) {
        check(store.downloadedPath(stored.first().slug,QStringLiteral("CON: Test/Game")).isEmpty(),"undownloaded release has no local path");
        check(store.downloadRelease(stored.first().slug,QStringLiteral("Magnet Only"),&error).isEmpty() && error==QStringLiteral("no Real-Debrid token configured"),"orchestration missing token");
        qint64 received=0;
        const auto path=store.downloadRelease(stored.first().slug,QStringLiteral("CON: Test/Game"),&error,[&](qint64 a,qint64){received=a;});
        QFile downloaded(path);check(!path.isEmpty() && error.isEmpty() && path.endsWith(QStringLiteral(".zip")) && downloaded.open(QIODevice::ReadOnly) && downloaded.readAll()==QByteArray("fixture") && received==7,"direct fallback streaming/progress/safe filename");
        check(store.downloadedPath(stored.first().slug,QStringLiteral("CON: Test/Game"))==path,"download lookup uses the same sanitized title");
        downloaded.close();check(downloaded.remove(),"remove completed fixture");
        check(store.downloadedPath(stored.first().slug,QStringLiteral("CON: Test/Game")).isEmpty(),"deleted download is not treated as local");
        const auto fallbackPath=store.downloadRelease(stored.first().slug,QStringLiteral("Fallback Test"),&error);
        QFile fallback(fallbackPath);
        check(!fallbackPath.isEmpty() && error.isEmpty() && fallback.open(QIODevice::ReadOnly) && fallback.readAll()==QByteArray("fixture"),"404 direct candidate falls back in order");
        fallback.close();check(fallback.remove(),"remove fallback fixture");
        auto reviewed=[&](const QString& name,const QVector<QString>& urls,const QString& destination) {
            QVector<Download> links;for(const auto& url:urls)links.append(normalizeDownload(url));
            QString reviewedError;
            return qMakePair(store.downloadGame(QString(),QUrl(),name,links,destination,&reviewedError,{},nullptr),reviewedError);
        };
        QVector<QString> unsupported;for(int i=0;i<9;++i)unsupported.append(QStringLiteral("https://example.invalid/ignored%1.torrent?slot=%1").arg(i));
        unsupported.append(goodFallback);
        const auto unsupportedResult=reviewed(QStringLiteral("Unsupported Prefix"),unsupported,QDir(temporary.path()).filePath(QStringLiteral("unsupported-prefix")));
        check(!unsupportedResult.first.isEmpty() && unsupportedResult.second.isEmpty(),"unsupported candidates do not consume bounded attempts");
        QVector<QString> bounded;for(int i=0;i<9;++i)bounded.append(QStringLiteral("http://127.0.0.1:%1/bad%2.zip").arg(server.serverPort()).arg(i));
        bounded.append(QStringLiteral("http://127.0.0.1:%1/bounded-good.zip").arg(server.serverPort()));
        const auto boundedResult=reviewed(QStringLiteral("Bounded Direct"),bounded,QDir(temporary.path()).filePath(QStringLiteral("bounded-direct")));
        check(boundedResult.first.isEmpty() && boundedResult.second==QStringLiteral("Server returned HTTP 404") && !boundedGoodSeen.load(),"bounded fallback stops after eight supported candidates");
        const auto status500=reviewed(QStringLiteral("HTTP 500 Fallback"),{QStringLiteral("http://127.0.0.1:%1/status500.zip").arg(server.serverPort()),goodFallback},QDir(temporary.path()).filePath(QStringLiteral("status500")));
        const auto status503=reviewed(QStringLiteral("HTTP 503 Fallback"),{QStringLiteral("http://127.0.0.1:%1/status503.zip").arg(server.serverPort()),goodFallback},QDir(temporary.path()).filePath(QStringLiteral("status503")));
        check(!status500.first.isEmpty() && status500.second.isEmpty() && !status503.first.isEmpty() && status503.second.isEmpty(),"HTTP 500 and 503 candidates fall back");
        for(const auto& terminal:QStringList{QStringLiteral("status401"),QStringLiteral("status403"),QStringLiteral("redirect")}) {
            const auto result=reviewed(QStringLiteral("Terminal ")+terminal,{QStringLiteral("http://127.0.0.1:%1/%2.zip").arg(server.serverPort()).arg(terminal),goodFallback},QDir(temporary.path()).filePath(terminal));
            check(result.first.isEmpty() && !result.second.isEmpty(),"non-404/5xx HTTP candidate is terminal");
        }
        const auto network=reviewed(QStringLiteral("Network Terminal"),{QStringLiteral("http://127.0.0.1:1/network.zip"),goodFallback},QDir(temporary.path()).filePath(QStringLiteral("network")));
        check(network.first.isEmpty() && network.second.startsWith(QStringLiteral("Network error:")),"network failure is terminal");
        const auto invalid=store.downloadGame(QString(),QUrl(),QStringLiteral("Validation Terminal"),
            QVector<Download>{{QStringLiteral("direct"),QStringLiteral("file:///not-a-download.zip")},normalizeDownload(goodFallback)},
            QDir(temporary.path()).filePath(QStringLiteral("validation")),&error,{},nullptr);
        check(invalid.isEmpty() && error==QStringLiteral("Invalid HTTP(S) URL"),"validation failure is terminal");
        const QString blockedDir=QDir(temporary.path()).filePath(QStringLiteral("blocked-destination"));
        QFile blocked(blockedDir);check(blocked.open(QIODevice::WriteOnly),"blocked destination fixture");blocked.write("not a directory");blocked.close();
        const auto pathFailure=reviewed(QStringLiteral("Path Terminal"),{goodFallback},blockedDir);
        check(pathFailure.first.isEmpty() && pathFailure.second==QStringLiteral("Cannot create download directory"),"path failure is terminal");
        const QString slow=QStringLiteral("http://127.0.0.1:%1/slow.zip").arg(server.serverPort());
        const QString cancelDir=QDir(temporary.path()).filePath(QStringLiteral("cancel-download"));
        std::atomic_bool cancelled{false};QString cancelError;
        QFuture<QString> pending=QtConcurrent::run([&] {
            return store.downloadGame(QString(),QUrl(),QStringLiteral("Cancelled Test"),
                                       QVector<Download>{normalizeDownload(slow)},cancelDir,&cancelError,{},&cancelled);
        });
        QElapsedTimer cancelClock;cancelClock.start();
        while(!slowSeen.load() && !pending.isFinished() && cancelClock.elapsed()<3000)QCoreApplication::processEvents(QEventLoop::AllEvents,10);
        cancelled.store(true);
        while(!pending.isFinished() && cancelClock.elapsed()<5000)QCoreApplication::processEvents(QEventLoop::AllEvents,10);
        check(pending.isFinished() && pending.result().isEmpty() && cancelError==QStringLiteral("Download cancelled"),"cancellation aborts active candidate");
        check(QDir(cancelDir).entryList(QDir::Files|QDir::NoDotAndDotDot).isEmpty(),"cancellation leaves no partial download");

        const QString resumeA=QStringLiteral("http://127.0.0.1:%1/resume-a.zip").arg(server.serverPort());
        const QString resumeB=QStringLiteral("http://127.0.0.1:%1/resume-b.zip").arg(server.serverPort());
        const QString pauseDir=QDir(temporary.path()).filePath(QStringLiteral("pause-download"));
        std::atomic_bool pauseCancel{false},pausing{false},pauseBytes{false};QString pauseError;
        QFuture<QString> pauseFuture=QtConcurrent::run([&] {
            return store.downloadGame(QString(),QUrl(),QStringLiteral("Paused Test"),{normalizeDownload(resumeA)},pauseDir,&pauseError,
                                       [&](qint64 received,qint64){if(received>=7)pauseBytes.store(true);},&pauseCancel,&pausing);
        });
        QElapsedTimer pauseClock;pauseClock.start();
        while(!pauseBytes.load() && !pauseFuture.isFinished() && pauseClock.elapsed()<3000)QCoreApplication::processEvents(QEventLoop::AllEvents,10);
        pausing.store(true);pauseCancel.store(true);
        while(!pauseFuture.isFinished() && pauseClock.elapsed()<5000)QCoreApplication::processEvents(QEventLoop::AllEvents,10);
        const QString pausePart=QDir(pauseDir).filePath(QStringLiteral(".xmb-game.part"));
        const QString pauseMeta=QDir(pauseDir).filePath(QStringLiteral(".xmb-game.resume.json"));
        check(pauseFuture.isFinished() && pauseError==QStringLiteral("Download paused") && QFileInfo(pausePart).size()==7 && QFileInfo(pauseMeta).exists(),"pausing preserves resumable partial and checkpoint");
        resumeBSawRange.store(false);
        std::atomic_bool bCancel{false},bPausing{false};QString bError;
        const auto resumed=store.downloadGame(QString(),QUrl(),QStringLiteral("Paused Test"),{normalizeDownload(resumeB)},pauseDir,&bError,{},&bCancel,&bPausing);
        QFile resumedFile(resumed);
        check(!resumed.isEmpty() && bError.isEmpty() && resumedFile.open(QIODevice::ReadOnly) && resumedFile.readAll()==QByteArray("B-PAYLOAD") && !resumeBSawRange.load() && !QFileInfo::exists(pausePart) && !QFileInfo::exists(pauseMeta),"URL B does not resume URL A checkpoint");
        resumedFile.close();QFile::remove(resumed);
        const QString partialOnlyDir=QDir(temporary.path()).filePath(QStringLiteral("partial-only"));QDir().mkpath(partialOnlyDir);
        QFile partialOnly(QDir(partialOnlyDir).filePath(QStringLiteral(".xmb-game.part")));(void)partialOnly.open(QIODevice::WriteOnly);(void)partialOnly.write("AAAA");partialOnly.close();
        resumeBSawRange.store(false);bError.clear();
        const auto partialOnlyResult=store.downloadGame(QString(),QUrl(),QStringLiteral("Partial Only"),{normalizeDownload(resumeB)},partialOnlyDir,&bError,{},&bCancel,&bPausing);
        check(!partialOnlyResult.isEmpty() && bError.isEmpty() && !resumeBSawRange.load() && !QFileInfo::exists(QDir(partialOnlyDir).filePath(QStringLiteral(".xmb-game.part"))),"URL B ignores URL A partial without checkpoint");
        QFile::remove(partialOnlyResult);
        const QString mismatchDir=QDir(temporary.path()).filePath(QStringLiteral("mismatched-validator"));QDir().mkpath(mismatchDir);
        QFile mismatchPart(QDir(mismatchDir).filePath(QStringLiteral(".xmb-game.part")));(void)mismatchPart.open(QIODevice::WriteOnly);(void)mismatchPart.write("AAAA");mismatchPart.close();
        const QByteArray bResource=QCryptographicHash::hash(QUrl(resumeB).toEncoded(),QCryptographicHash::Sha256).toHex();
        QFile mismatchMeta(QDir(mismatchDir).filePath(QStringLiteral(".xmb-game.resume.json")));(void)mismatchMeta.open(QIODevice::WriteOnly);
        (void)mismatchMeta.write(QJsonDocument(QJsonObject{{QStringLiteral("resource"),QString::fromLatin1(bResource)},
            {QStringLiteral("etag"),QString::fromLatin1(QByteArray("\"wrong\"").toBase64())},{QStringLiteral("length"),QStringLiteral("9")},{QStringLiteral("written"),QStringLiteral("4")}}).toJson(QJsonDocument::Compact));mismatchMeta.close();
        resumeBSawRange.store(false);bError.clear();
        const auto mismatchResult=store.downloadGame(QString(),QUrl(),QStringLiteral("Mismatched Validator"),{normalizeDownload(resumeB)},mismatchDir,&bError,{},&bCancel,&bPausing);
        QFile mismatchFile(mismatchResult);
        check(!mismatchResult.isEmpty() && bError.isEmpty() && mismatchFile.open(QIODevice::ReadOnly) && mismatchFile.readAll()==QByteArray("B-PAYLOAD") && resumeBSawRange.load() && !QFileInfo::exists(QDir(mismatchDir).filePath(QStringLiteral(".xmb-game.part"))) && !QFileInfo::exists(QDir(mismatchDir).filePath(QStringLiteral(".xmb-game.resume.json"))),"mismatched validator restarts URL B safely");
        mismatchFile.close();QFile::remove(mismatchResult);
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
        // Genre tag parsing and grouping (pure, no network). Missing, non-array and mixed members
        // are dropped without invalidating the game; genuine tags are trimmed and case-insensitively
        // de-duplicated. groupGenres counts distinct games and ranks by descending count then name.
        check(parseGenreTags(QJsonValue()).isEmpty(),"absent genres are empty");
        check(parseGenreTags(QJsonValue(QStringLiteral("Action"))).isEmpty(),"a non-array genres field is empty");
        check(parseGenreTags(QJsonArray{1,false,QJsonObject{}}).isEmpty(),"mixed non-string genre members are dropped");
        const auto cleaned=parseGenreTags(QJsonArray{QStringLiteral("  Action "),QStringLiteral("action"),QStringLiteral(""),QStringLiteral("RPG"),7});
        check(cleaned==(QStringList{QStringLiteral("Action"),QStringLiteral("RPG")}),"genre tags are trimmed and de-duplicated case-insensitively");
        PopularTitleGroup ga; ga.name=QStringLiteral("Game A"); ga.genres={QStringLiteral("Action"),QStringLiteral("Action"),QStringLiteral("RPG")};
        PopularTitleGroup gb; gb.name=QStringLiteral("Game B"); gb.genres={QStringLiteral("RPG")};
        PopularTitleGroup gc; gc.name=QStringLiteral("Game C"); gc.genres={QStringLiteral("Adventure")};
        const auto genres=groupGenres({ga,gb,gc});
        check(genres.size()==3,"distinct genres are counted once across games");
        check(genres[0].name==QStringLiteral("RPG") && genres[0].count==2,"most common genre ranks first with its game count");
        check(genres[1].name==QStringLiteral("Action") && genres[1].count==1,"single-game genre keeps its count");
        check(genres[2].name==QStringLiteral("Adventure") && genres[2].count==1,"least common genre ranks last");
        check(groupGenres({PopularTitleGroup{}}).isEmpty(),"a genre-less game never fabricates a genre");
    }
    {
        // Local ranked-catalogue cache (pure, no network): the persisted entry carries the complete
        // PopularTitle (title, cover URL and structured genres), is versioned, and is keyed by the
        // resolved endpoint. Genres round-trip verbatim, missing/empty genres stay empty, and an
        // old-schema, corrupt, oversized, expired or identity-mismatched file is always a miss.
        QTemporaryDir cacheDir;
        check(cacheDir.isValid(),"hydra catalogue cache tempdir");
        HydraCatalogueCache cache(cacheDir.path());
        const QString endpointA=QStringLiteral("https://example.invalid/hot");
        const QString endpointB=QStringLiteral("https://example.invalid/other");
        QVector<PopularTitle> titles;
        PopularTitle one;one.title=QStringLiteral("Alpha");one.genres={QStringLiteral("Action"),QStringLiteral("RPG")};titles.append(one);
        PopularTitle two;two.title=QStringLiteral("Beta");two.coverUrl=QStringLiteral("https://c.invalid/b.png");titles.append(two);
        const qint64 now=QDateTime::currentMSecsSinceEpoch();
        check(cache.store(endpointA,titles,now),"cache store succeeds");
        QVector<PopularTitle> hit;
        check(cache.load(endpointA,&hit,now) && hit.size()==2 && hit[0].title==QStringLiteral("Alpha") &&
              hit[0].genres==(QStringList{QStringLiteral("Action"),QStringLiteral("RPG")}) &&
              hit[1].coverUrl==QStringLiteral("https://c.invalid/b.png") && hit[1].genres.isEmpty(),
              "cache hit round-trips structured genres and preserves a missing genre list");
        QVector<PopularTitle> inside;
        check(cache.load(endpointA,&inside,now+HydraCatalogueCache::ttlMs()-1),"cache hit just inside the TTL window");
        QVector<PopularTitle> expired;
        check(!cache.load(endpointA,&expired,now+HydraCatalogueCache::ttlMs()+1) && expired.isEmpty(),
              "cache miss once past the TTL window");
        check(QFile::copy(cache.pathFor(endpointA),cache.pathFor(endpointB)),"identity mismatch fixture copy");
        QVector<PopularTitle> mismatch;
        check(!cache.load(endpointB,&mismatch,now) && mismatch.isEmpty(),"cache miss on in-file endpoint identity mismatch");
        { QFile f(cache.pathFor(endpointA));check(f.open(QIODevice::WriteOnly),"old-schema fixture open");
          f.write(QByteArrayLiteral("{\"endpoint\":\"https://example.invalid/hot\",\"storedAtMs\":")+
                  QByteArray::number(now)+QByteArrayLiteral(",\"titles\":[{\"title\":\"Old Schema Game\"}]}")); }
        QVector<PopularTitle> oldSchema;
        check(!cache.load(endpointA,&oldSchema,now) && oldSchema.isEmpty(),"an unversioned old-schema entry is a miss");
        { QFile f(cache.pathFor(endpointA));check(f.open(QIODevice::WriteOnly),"corrupt fixture open");f.write("not json"); }
        QVector<PopularTitle> corrupt;
        check(!cache.load(endpointA,&corrupt,now) && corrupt.isEmpty(),"corrupt cache file is a miss, not an error");
        { QFile f(cache.pathFor(endpointA));check(f.open(QIODevice::WriteOnly),"oversize fixture open");f.write(QByteArray(int(HydraCatalogueCache::maxBytes())+1,'x')); }
        QVector<PopularTitle> oversize;
        check(!cache.load(endpointA,&oversize,now),"oversized cache file is rejected as a miss");
        check(cache.store(endpointB,titles,now-HydraCatalogueCache::ttlMs()-1000),"stale store for a later miss");
        QVector<PopularTitle> stale;
        check(!cache.load(endpointB,&stale,now) && stale.isEmpty(),"a stored stale entry is a miss");
        check(!cache.store(endpointA,QVector<PopularTitle>{},now),"an empty result is never persisted");
    }
    {
        // Ranked-catalogue cache opt-in wiring (pure): the per-profile directory resolver only ever
        // returns a path when the opt-in is on, and it scopes the path by profile so no profile can
        // read another's cached catalogue.
        check(hydraCatalogueCacheDirectory(QStringLiteral("C:/data"),QStringLiteral("alice"),false).isEmpty(),
              "cache dir is empty when the opt-in is off (default)");
        check(hydraCatalogueCacheDirectory(QString(),QStringLiteral("alice"),true).isEmpty(),
              "cache dir is empty without a data directory");
        check(hydraCatalogueCacheDirectory(QStringLiteral("C:/data"),QStringLiteral("alice"),true)==
              QStringLiteral("C:/data/cache/hydra-catalogue/alice"),
              "cache dir is scoped to the active profile");
        check(hydraCatalogueCacheDirectory(QStringLiteral("C:/data"),QStringLiteral("bob"),true)==
              QStringLiteral("C:/data/cache/hydra-catalogue/bob"),
              "cache dir differs per profile (profile separation)");
        check(hydraCatalogueCacheDirectory(QStringLiteral("C:/data"),QString(),true)==
              QStringLiteral("C:/data/cache/hydra-catalogue/local"),
              "cache dir falls back to the reserved local scope with no profile selected");
    }
    {
        // The durable per-profile opt-in (Preferences "hydra/catalogueCache"): default OFF, persists
        // under the active profile, and is enforced per profile so one user never leaks to another.
        QTemporaryDir prefsDir;
        check(prefsDir.isValid(),"hydra cache preferences tempdir");
        const QString key=QStringLiteral("hydra/catalogueCache");
        Preferences prefs(prefsDir.path());
        check(!prefs.value(key).toBool(),"cache opt-in defaults to off");
        check(prefs.setProfile(QStringLiteral("alice")),"select profile alice");
        check(prefs.setValue(key,true),"alice opts the cache in");
        check(prefs.value(key).toBool(),"alice reads the opt-in on");
        check(prefs.setProfile(QStringLiteral("bob")),"select profile bob");
        check(!prefs.value(key).toBool(),"bob is off: the opt-in is per profile, never shared");
        check(prefs.setValue(key,true),"bob opts the cache in");
        check(prefs.setProfile(QStringLiteral("alice")) && prefs.value(key).toBool(),"alice remains on after switching back");
        Preferences reloaded(prefsDir.path());
        check(reloaded.activeProfile()==QStringLiteral("alice") && reloaded.value(key).toBool(),
              "the opt-in is durable and resolves against the active profile");
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
        int source500ThenSuccessRequests=0;
        int source500AlwaysRequests=0;
        int source404Requests=0;
        int source403Requests=0;
        int hot500ThenSuccessRequests=0;
        int hot500AlwaysRequests=0;
        int hot403Requests=0;
        int hot429Requests=0;
        int source401Requests=0;
        int source503RetryAfterRequests=0;
        int sourceStallRequests=0;
        int source429Requests=0;
        int source503RetryAfterShortRequests=0;
        int source503RetryAfterMalformedRequests=0;
        int sourceDripFeedRequests=0;
        int sourceDripFeedOkRequests=0;
        // Directory (/sources) fixture counters: requests per route and per page, so the tests can prove
        // exactly how many attempts the real public listSources made.
        int dirFlakyRequests=0;          // /sources-500-then-success
        QHash<int,int> dirPagedRequests; // /sources-paged-flaky, by page number
        QHash<int,int> dirDownRequests;  // /sources-paged-down, by page number
        QHash<int,int> dirBudgetRequests;// /sources-budget, by page number
        int dir403Requests=0;
        int dir429Requests=0;
        int sourceFeedRequests=0;
        int sourcesListRequests=0;
        int searchRequests=0;
        int hotRequests=0;
        int hotCache500ThenSuccessRequests=0;
        QObject::connect(&fixture,&QTcpServer::newConnection,&app,[&] {
            auto* socket=fixture.nextPendingConnection();
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket] {
                const QByteArray request=socket->readAll();
                int title=-1;
                for(int i=0;i<40;++i)if(request.contains(QStringLiteral("Hot%20%1").arg(i,2,10,QChar('0')).toLatin1())){title=i;break;}
                const bool hot=request.contains("/hot") || request.contains("-hot");
                const bool sources=request.contains("/sources");
                const bool search=request.contains("/search");
                const bool sourceFeed=request.contains("/source-feed") || request.contains("/source-just-over-16m") || request.contains("/source-malformed");
                const bool duplicate=request.contains("hot%20%20%20");
                QByteArray body;
                int delay=hot?0:(title==39?900:(title%5)*15);
                bool fail=false;
                // Hot catalogue retry fixture: one transient 500 is retried to a valid 40-entry
                // array; repeated 500 stays bounded; 403/429 are terminal (never auto-retried).
                if(request.contains("/hot-500-then-success")) {
                    ++hot500ThenSuccessRequests;
                    if(hot500ThenSuccessRequests==1) {
                        socket->write("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();return;
                    }
                    QJsonArray rows;
                    for(int i=0;i<40;++i)rows.append(QJsonObject{{QStringLiteral("title"),QStringLiteral("Hot %1").arg(i,2,10,QChar('0'))}});
                    const QByteArray hotBody=QJsonDocument(rows).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(hotBody.size())+"\r\nConnection: close\r\n\r\n"+hotBody);
                    socket->disconnectFromHost();return;
                }
                if(request.contains("/hot-500-always")) {
                    ++hot500AlwaysRequests;
                    socket->write("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();return;
                }
                // A cache-scoped transient 500 that retries to a genre-bearing success, so the
                // cache composition check is independent of the earlier retry counter's state.
                if(request.contains("/hot-cache-500-then-success")) {
                    ++hotCache500ThenSuccessRequests;
                    if(hotCache500ThenSuccessRequests==1) {
                        socket->write("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();return;
                    }
                    QJsonArray rows;
                    for(int i=0;i<40;++i)rows.append(QJsonObject{{QStringLiteral("title"),QStringLiteral("Hot %1").arg(i,2,10,QChar('0'))},{QStringLiteral("genres"),QJsonArray{QStringLiteral("Action")}}});
                    const QByteArray hotBody=QJsonDocument(rows).toJson(QJsonDocument::Compact);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(hotBody.size())+"\r\nConnection: close\r\n\r\n"+hotBody);
                    socket->disconnectFromHost();return;
                }
                if(request.contains("/hot-403")) {
                    ++hot403Requests;
                    socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();return;
                }
                if(request.contains("/hot-429")) {
                    ++hot429Requests;
                    socket->write("HTTP/1.1 429 Too Many Requests\r\nRetry-After: 7\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();return;
                }
                if(request.contains("/source-redirect")) {
                    const QByteArray response=QByteArrayLiteral("HTTP/1.1 302 Found\r\nLocation: /source-feed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->write(response);socket->disconnectFromHost();return;
                }
                if(request.contains("/source-unsafe-redirect")) {
                    const QByteArray response=QByteArrayLiteral("HTTP/1.1 302 Found\r\nLocation: file:///not-a-source\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->write(response);socket->disconnectFromHost();return;
                }
                if(request.contains("/source-over-64m")) {
                    // A declared length well past the 64 MiB source cap must be rejected from the
                    // header alone, before any large body is transferred (keeps the fixture tiny).
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 67108865\r\nConnection: close\r\n\r\n");
                    socket->write(QByteArray(8192,'x'));
                    socket->flush();return;
                }
                if(request.contains("/source-over-64m-chunked")) {
                    const QByteArray first(32*1024*1024,'x'),second(32*1024*1024+1,'x');
                    const QByteArray header=QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
                    socket->write(header+QByteArray::number(first.size(),16)+"\r\n"+first+"\r\n");
                    QTimer::singleShot(25,socket,[socket,second]{socket->write(QByteArray::number(second.size(),16)+"\r\n"+second+"\r\n0\r\n\r\n");socket->disconnectFromHost();});return;
                }
                if(request.contains("/sources-oversized")) {
                    // The directory listing keeps its 16 MiB cap even though source feeds allow 64 MiB.
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 16777217\r\nConnection: close\r\n\r\n");
                    socket->write(QByteArray(8192,'x'));
                    socket->flush();return;
                }
                if(request.contains("/source-500-then-success")) {
                    ++source500ThenSuccessRequests;
                    if(source500ThenSuccessRequests==1) {
                        socket->write("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();return;
                    }
                    const QByteArray feed=QByteArrayLiteral("[{\"title\":\"Retry Then Success Game\",\"size\":\"1 GB\",\"uri\":\"https://fixture/retry.zip\"}]");
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(feed.size())+"\r\nConnection: close\r\n\r\n"+feed);
                    socket->disconnectFromHost();return;
                }
                if(request.contains("/source-500-always")) {
                    ++source500AlwaysRequests;
                    socket->write("HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();return;
                }
                if(request.contains("/source-403")) {
                    ++source403Requests;
                    socket->write("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();return;
                }
                if(request.contains("/source-401")) {
                    ++source401Requests;
                    socket->write("HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();return;
                }
                if(request.contains("/source-404")) {
                    ++source404Requests;
                    socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();return;
                }
                if(request.contains("/source-503-retry-after")) {
                    ++source503RetryAfterRequests;
                    socket->write("HTTP/1.1 503 Service Unavailable\r\nRetry-After: 3600\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();return;
                }
                if(request.contains("/source-stall-then-success")) {
                    ++sourceStallRequests;
                    if(sourceStallRequests==1)return;   // stall: hold the connection open, send nothing
                    const QByteArray feed=QByteArrayLiteral("[{\"title\":\"Stall Then Success Game\",\"size\":\"1 GB\",\"uri\":\"https://fixture/stall.zip\"}]");
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(feed.size())+"\r\nConnection: close\r\n\r\n"+feed);
                    socket->disconnectFromHost();return;
                }
                if(request.contains("/source-429")) {
                    ++source429Requests;
                    socket->write("HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();return;
                }
                if(request.contains("/source-retryafter-short")) {
                    ++source503RetryAfterShortRequests;
                    if(source503RetryAfterShortRequests==1) {
                        socket->write("HTTP/1.1 503 Service Unavailable\r\nRetry-After: 1\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                        socket->disconnectFromHost();return;
                    }
                    const QByteArray feed=QByteArrayLiteral("[{\"title\":\"Retry After Honored Game\",\"size\":\"1 GB\",\"uri\":\"https://fixture/retryafter.zip\"}]");
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(feed.size())+"\r\nConnection: close\r\n\r\n"+feed);
                    socket->disconnectFromHost();return;
                }
                if(request.contains("/source-retryafter-malformed")) {
                    ++source503RetryAfterMalformedRequests;
                    socket->write("HTTP/1.1 503 Service Unavailable\r\nRetry-After: nonsense\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();return;
                }
                if(request.contains("/source-drip-feed")) {
                    // Dribble a valid release-index body over ~5s in tiny chunks: each slice arrives
                    // well inside any reasonable idle deadline, but the whole body would outlive a
                    // shortened test budget. Only the absolute deadline should cut it off. `socket`
                    // is each timer's context, so the whole schedule dies if the client aborts.
                    ++sourceDripFeedRequests;
                    const QByteArray body=QByteArrayLiteral("[{\"title\":\"Drip Feed Game\",\"size\":\"1 GB\",\"uri\":\"https://fixture/drip.zip\"}]");
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
                    const int slice=2,tick=150;
                    int delay=0;
                    for(int offset=0;offset<body.size();offset+=slice,delay+=tick) {
                        const QByteArray chunk=body.mid(offset,slice);
                        QTimer::singleShot(delay,socket,[socket,chunk]{ socket->write(QByteArray::number(chunk.size(),16)+"\r\n"+chunk+"\r\n"); });
                    }
                    QTimer::singleShot(delay,socket,[socket]{ socket->write("0\r\n\r\n"); socket->disconnectFromHost(); });
                    return;
                }
                if(request.contains("/source-progress-ok")) {
                    // A progressing feed that finishes inside the budget: dribbled fast enough to
                    // complete normally without tripping the idle or absolute deadline.
                    ++sourceDripFeedOkRequests;
                    const QByteArray body=QByteArrayLiteral("[{\"title\":\"Drip Feed Ok Game\",\"size\":\"1 GB\",\"uri\":\"https://fixture/dripok.zip\"}]");
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n");
                    const int slice=12,tick=100;
                    int delay=0;
                    for(int offset=0;offset<body.size();offset+=slice,delay+=tick) {
                        const QByteArray chunk=body.mid(offset,slice);
                        QTimer::singleShot(delay,socket,[socket,chunk]{ socket->write(QByteArray::number(chunk.size(),16)+"\r\n"+chunk+"\r\n"); });
                    }
                    QTimer::singleShot(delay,socket,[socket]{ socket->write("0\r\n\r\n"); socket->disconnectFromHost(); });
                    return;
                }
                // Source-directory routes (api /sources, paged). Each answers a real HTTP status so the tests drive the
                // public listSources path exactly as the Browse Sources screen does.
                if(request.contains("/sources-500-then-success") || request.contains("/sources-paged") || request.contains("/sources-budget")
                   || request.contains("/sources-403") || request.contains("/sources-429")) {
                    const auto reply=[socket](const QByteArray& status,const QByteArray& extra,const QByteArray& payload) {
                        socket->write("HTTP/1.1 "+status+"\r\n"+extra+"Content-Type: application/json\r\nContent-Length: "+QByteArray::number(payload.size())+"\r\nConnection: close\r\n\r\n"+payload);
                        socket->disconnectFromHost();
                    };
                    int dirPage=1;
                    const int pageAt=request.indexOf("page=");
                    if(pageAt>=0) {   // digits only: "page=2&limit=100" must read 2, not fail on the "&"
                        int end=pageAt+5;
                        while(end<request.size() && request.at(end)>='0' && request.at(end)<='9')++end;
                        dirPage=request.mid(pageAt+5,end-pageAt-5).toInt();
                    }
                    const QByteArray full=QByteArrayLiteral("{\"total\":2,\"limit\":100,\"sources\":[{\"title\":\"Fixture Source A\",\"url\":\"https://fixture.invalid/a.json\"},{\"title\":\"Fixture Source B\",\"url\":\"https://fixture.invalid/b.json\"}]}");
                    const QByteArray pageOne=QByteArrayLiteral("{\"total\":3,\"limit\":2,\"sources\":[{\"title\":\"Paged A\",\"url\":\"https://fixture.invalid/pa.json\"},{\"title\":\"Paged B\",\"url\":\"https://fixture.invalid/pb.json\"}]}");
                    const QByteArray pageTwo=QByteArrayLiteral("{\"total\":3,\"limit\":2,\"sources\":[{\"title\":\"Paged C\",\"url\":\"https://fixture.invalid/pc.json\"}]}");
                    const QByteArray serverError=QByteArrayLiteral("500 Internal Server Error");
                    if(request.contains("/sources-500-then-success")) {
                        if(++dirFlakyRequests==1) { reply(serverError,{},{}); return; }
                        reply("200 OK",{},full); return;
                    }
                    if(request.contains("/sources-paged-flaky")) {
                        const int count=++dirPagedRequests[dirPage];
                        if(dirPage==2 && count==1) { reply(serverError,{},{}); return; }
                        reply("200 OK",{},dirPage==1?pageOne:pageTwo); return;
                    }
                    if(request.contains("/sources-paged-down")) {
                        ++dirDownRequests[dirPage];
                        if(dirPage>=2) { reply(serverError,{},{}); return; }
                        reply("200 OK",{},pageOne); return;
                    }
                    if(request.contains("/sources-budget")) {
                        const int count=++dirBudgetRequests[dirPage];
                        if(dirPage==1 && count==1) { reply(serverError,{},{}); return; }
                        if(dirPage>=2) { reply(serverError,{},{}); return; }
                        reply("200 OK",{},pageOne); return;
                    }
                    if(request.contains("/sources-403")) { ++dir403Requests; reply("403 Forbidden",{},{}); return; }
                    if(request.contains("/sources-429")) { ++dir429Requests; reply("429 Too Many Requests","Retry-After: 2\r\n",{}); return; }
                }
                if(!hot && title==7)fail=++attempts[title]==1;
                if(!hot && duplicate)fail=++attempts[-1]==1;
                if(sources) {
                    ++sourcesListRequests;
                    if(request.contains("/sources-fail"))fail=true;
                    if(request.contains("/sources-malformed"))
                        body=QByteArrayLiteral("{\"total\":1,\"limit\":100,\"sources\":[{\"title\":\"Good\",\"url\":\"https://good.invalid/index.json\"},null]}");
                    else if(request.contains("/sources-empty"))
                        body=QByteArrayLiteral("{\"total\":0,\"limit\":100,\"sources\":[]}");
                    else
                        body=QByteArrayLiteral("{\"total\":2,\"limit\":100,\"sources\":[{\"title\":\"Fixture Source A\",\"url\":\"https://fixture.invalid/a.json\",\"stats\":{\"installs\":3}},{\"title\":\"Fixture Source B\",\"url\":\"https://fixture.invalid/b.json\",\"stats\":{\"installs\":2}}]}");
                } else if(sourceFeed) {
                    ++sourceFeedRequests;
                    body=QByteArrayLiteral("[{\"title\":\"Same Game Deluxe\",\"size\":\"1 GB\",\"uri\":\"https://fixture/same.zip\"},{\"title\":\"Same  Game  Deluxe\",\"size\":\"2 GB\",\"uri\":\"magnet:?xt=urn:btih:same\"},{\"title\":\"Same Game Deluxe\",\"size\":\"3 GB\",\"uri\":\"https://fixture/same.torrent\"}]");
                    if(request.contains("/source-feed-large")) {
                        QJsonArray downloads;
                        for(int i=0;i<50;++i)
                            downloads.append(QJsonObject{{QStringLiteral("title"),QStringLiteral("Large Game %1").arg(i)},
                                {QStringLiteral("uris"),QJsonArray{QStringLiteral("https://fixture/large%1.zip").arg(i)}},
                                {QStringLiteral("fileSize"),QStringLiteral("1 GB")}});
                        body=QJsonDocument(QJsonObject{{QStringLiteral("downloads"),downloads}}).toJson(QJsonDocument::Compact);
                    }
                    if(request.contains("/source-feed-100k")) {
                        // A synthetic 100k-entry release index (~50 MB), built once: the real large feeds are
                        // 68k-110k games and 16.8-53 MB. Every entry carries a distinct name and one direct link.
                        static QByteArray big;
                        if(big.isEmpty()) {
                            const QString pad(380,QLatin1Char('x'));
                            big.reserve(55*1024*1024);
                            big.append("{\"downloads\":[");
                            for(int i=0;i<100000;++i) {
                                if(i)big.append(',');
                                big.append(QStringLiteral("{\"title\":\"Bulk Game %1\",\"uris\":[\"https://fixture/bulk%1.zip\"],\"fileSize\":\"1 GB\",\"uploadDate\":\"2024-01-01\",\"note\":\"%2\"}").arg(i,6,10,QLatin1Char('0')).arg(pad).toUtf8());
                            }
                            big.append("]}");
                        }
                        body=big;
                    }
                    if(request.contains("/source-just-over-16m")) {
                        // A valid release-index body JUST over the old 16 MiB cap, now accepted by the
                        // 64 MiB source cap. The padding sits in a top-level field the fitgirl adapter
                        // ignores, so the body parses to a single direct-download entry.
                        const QByteArray pad(16*1024*1024+8,'a');
                        body=QByteArrayLiteral("{\"downloads\":[{\"title\":\"Just Over 16 MiB Game\",\"uris\":[\"https://fixture/justover.zip\"],\"fileSize\":\"1 GB\"}],\"padding\":\"")+pad+QByteArrayLiteral("\"}");
                    }
                    if(request.contains("/source-malformed"))body=QByteArrayLiteral("{malformed");
                } else if(search) {
                    ++searchRequests;
                    if(request.contains("/search-malformed"))
                        body=QByteArrayLiteral("{\"filteredCount\":2,\"games\":[{\"name\":\"Good Game\",\"source\":\"Fixture Source A\",\"sourceUrl\":\"https://fixture.invalid/a.json\"},null]}");
                    else if(request.contains("/search-empty"))
                        body=QByteArrayLiteral("{\"filteredCount\":0,\"games\":[]}");
                    else
                        body=QByteArrayLiteral("{\"filteredCount\":2,\"games\":[{\"name\":\"Fixture Search Game\",\"title\":\"Fixture Search Game\",\"source\":\"Fixture Source A\",\"sourceUrl\":\"https://fixture.invalid/a.json\",\"fileSize\":\"1 GB\"},{\"name\":\"Fixture Search Game\",\"title\":\"Fixture Search Game\",\"source\":\"Fixture Source B\",\"sourceUrl\":\"https://fixture.invalid/b.json\",\"fileSize\":\"2 GB\"}]}");
                } else if(hot && request.contains("/hot-retry") && ++attempts[-2]==1) {
                    body=QByteArrayLiteral("{malformed");
                } else if(hot && request.contains("/hot-null")) {
                    body=QByteArrayLiteral("[null]");
                } else if(hot && request.contains("/hot-empty-object")) {
                    body=QByteArrayLiteral("[{}]");
                } else if(hot && request.contains("/hot-mixed")) {
                    body=QByteArrayLiteral("[{\"title\":\"Good Title\"},null]");
                } else if(hot && request.contains("/error-hot")) {
                    fail=true;
                } else if(hot && request.contains("/malformed-hot")) {
                    body=QByteArrayLiteral("{malformed");
                } else if(hot && request.contains("/wrong-hot")) {
                    body=QByteArrayLiteral("{}");
                } else if(hot && request.contains("/empty-hot")) {
                    body=QByteArrayLiteral("[]");
                } else if(hot) {
                    ++hotRequests;
                    QJsonArray rows;
                    for(int i=0;i<40;++i) {
                        QString title=QStringLiteral("Hot %1").arg(i,2,10,QChar('0'));
                        if(i==11)title=QStringLiteral("  hot   10  ");
                        if(i==12)title=QStringLiteral("Hot 10 — Deluxe Edition");
                        QJsonArray genres;
                        if(i<=9) genres.append(QStringLiteral("Action"));
                        else if(i==10 || i==11) { genres.append(QStringLiteral("RPG")); genres.append(QStringLiteral("Action")); }
                        else if(i==12) genres.append(QStringLiteral("RPG"));
                        else genres.append(QStringLiteral("Adventure"));
                        rows.append(QJsonObject{{QStringLiteral("title"),title},{QStringLiteral("genres"),genres}});
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
                } else if(!fail && request.contains("Same%20Source")) {
                    body=QByteArrayLiteral("{\"games\":[{\"source\":\"Same Source\",\"sourceUrl\":\"https://fixture/same-source.json\",\"originalTitle\":\"Same Source Standard\",\"fileSize\":\"1 GB\"},{\"source\":\"Same Source\",\"sourceUrl\":\"https://fixture/same-source.json\",\"originalTitle\":\"Same Source Deluxe\",\"fileSize\":\"2 GB\"},{\"source\":\"Same Source\",\"sourceUrl\":\"https://fixture/same-source.json\",\"originalTitle\":\"Same Source Standard\",\"fileSize\":\"1 GB\"}]}");
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
        // Genre metadata rides the same hot snapshot: parsed tags, the group union, and the ordered
        // distinct-genre list with counts. No download source is resolved to build this list.
        check(titles[0].genres==(QStringList{QStringLiteral("Action")}),"hot genre tag parsed on the first title");
        check(titles[10].genres==(QStringList{QStringLiteral("RPG"),QStringLiteral("Action")}),"multi-genre title keeps its tag order");
        check(titles[12].genres==(QStringList{QStringLiteral("RPG")}),"edition carries its own genre tag");
        check(groups[10].genres==(QStringList{QStringLiteral("RPG"),QStringLiteral("Action")}),"duplicate hot entries union their genres into one group");
        const auto genreList=groupGenres(groups);
        check(genreList.size()==3,"three distinct genres across the forty-title fixture");
        check(genreList[0].name==QStringLiteral("Adventure") && genreList[0].count==27,"Adventure ranks first with 27 games");
        check(genreList[1].name==QStringLiteral("Action") && genreList[1].count==11,"Action ranks second with 11 games (10 solo plus one shared)");
        check(genreList[2].name==QStringLiteral("RPG") && genreList[2].count==2,"RPG ranks last with 2 games (base tile plus edition)");
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
         PopularTitle sameSourceTitle;sameSourceTitle.title=QStringLiteral("Same Source");GameResult sameSourceGame;
         check(HydraLibrary().popularGame(sameSourceTitle,&sameSourceGame,&fixtureError) && sameSourceGame.options.size()==2 &&
               sameSourceGame.options[0].releaseTitle==QStringLiteral("Same Source Standard") &&
               sameSourceGame.options[1].releaseTitle==QStringLiteral("Same Source Deluxe"),
               "popularGame preserves distinct editions sharing one source URL and collapses only the exact duplicate");
        // Genre-scoped resolution reuses the already-fetched snapshot: only the matching groups resolve,
        // preserving hot order and the all-options/edition union. RPG selects the base tile (two duplicate
        // entries) and its edition, nothing else. The fan-out runs off the main thread and the loop pumps
        // the fixture's event loop, exactly like the progressive popularGames() check below.
        {
            const auto genreFuture=QtConcurrent::run([groups] {
                QVector<GameResult> games; QString error;
                HydraLibrary().popularGames(groups,QStringLiteral("RPG"),&games,&error);
                return qMakePair(games,error);
            });
            while(!genreFuture.isFinished())QCoreApplication::processEvents(QEventLoop::AllEvents,10);
            const auto rpg=genreFuture.result();
            check(rpg.second.isEmpty() && rpg.first.size()==2,"genre-scoped resolution succeeds over the hot snapshot");
            if(rpg.first.size()==2) {
                check(rpg.first[0].name==QStringLiteral("Hot 10") && rpg.first[0].options.size()==4,
                      "RPG base tile keeps the duplicate source union in hot order");
                check(rpg.first[1].name==QStringLiteral("Hot 10 — Deluxe Edition") && rpg.first[1].options.size()==2,
                      "RPG edition keeps its distinct sources, separate from the base");
            }
            const auto lowerFuture=QtConcurrent::run([groups] {
                QVector<GameResult> games; QString error;
                HydraLibrary().popularGames(groups,QStringLiteral("rpg"),&games,&error);
                return qMakePair(games,error);
            });
            while(!lowerFuture.isFinished())QCoreApplication::processEvents(QEventLoop::AllEvents,10);
            const auto lower=lowerFuture.result();
            check(lower.second.isEmpty() && lower.first.size()==2,"genre matching is case-insensitive");
            QVector<GameResult> noneGames; QString noneError;
            check(HydraLibrary().popularGames(groups,QStringLiteral("Simulation"),&noneGames,&noneError) && noneGames.isEmpty() && noneError.isEmpty(),
                  "an absent genre yields an empty, error-free result");
        }
        GameResult invalid; fixtureError.clear();
        check(!HydraLibrary().popularGame(titles[20],&invalid,&fixtureError) && invalid.options.isEmpty() &&
              fixtureError.contains(QStringLiteral("HTTP 500")) && fixtureError.contains(QStringLiteral("retry")) &&
              fixtureError.contains(QStringLiteral("library")) && !fixtureError.contains(QStringLiteral("catalogue")) &&
              !fixtureError.contains(QStringLiteral("127.0.0.1")),"HTTP 500 is actionable without URL leakage and names the library stage");
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
        for(const auto& path:{QStringLiteral("/hot-null"),QStringLiteral("/hot-empty-object"),QStringLiteral("/hot-mixed")}) {
            qputenv("XMB_HYDRA_HOT_URL",(base+path).toUtf8());titles.clear();fixtureError=QStringLiteral("stale error");
            check(!HydraLibrary().popularTitles(&titles,&fixtureError) && titles.isEmpty() &&
                  fixtureError.contains(QStringLiteral("invalid popular title entry")),
                  "malformed hot member fails without publishing partial titles");
        }
        qputenv("XMB_HYDRA_HOT_URL",(base+QStringLiteral("/hot-retry")).toUtf8());
        titles.clear();fixtureError=QStringLiteral("stale error");
        const bool firstRetry=HydraLibrary().popularTitles(&titles,&fixtureError);
        check(!firstRetry && titles.isEmpty() && !fixtureError.isEmpty(),"malformed hot response exposes retry state");
        titles.clear();
        const bool secondRetry=HydraLibrary().popularTitles(&titles,&fixtureError);
        check(secondRetry && titles.size()==40 && fixtureError.isEmpty(),"explicit hot retry succeeds and clears a reused error");
        for(const auto& path:{QStringLiteral("/error-hot"),QStringLiteral("/malformed-hot"),QStringLiteral("/wrong-hot")}) {
            qputenv("XMB_HYDRA_HOT_URL",(base+path).toUtf8());titles.clear();fixtureError.clear();
            check(!HydraLibrary().popularTitles(&titles,&fixtureError) && titles.isEmpty() && !fixtureError.isEmpty(),
                  "invalid hot catalogue response is actionable");
        }
        // The single initial Popular /catalogue/hot GET shares the selected-feed bounded retry:
        // a transient 500 is retried to success; a repeated 500 is bounded with no partial output and
        // a catalogue-stage error; 403/429 stay terminal. The error names the stage, so an empty
        // Popular page distinguishes a hot-catalogue failure from a per-title library failure.
        qputenv("XMB_HYDRA_HOT_URL",(base+QStringLiteral("/hot-500-then-success")).toUtf8());
        titles.clear();fixtureError=QStringLiteral("stale error");
        check(HydraLibrary().popularTitles(&titles,&fixtureError) && titles.size()==40 && fixtureError.isEmpty(),
              "transient HTTP 500 on the hot catalogue is retried once and then succeeds");
        check(hot500ThenSuccessRequests==2,"a single hot 500 costs exactly one automatic retry (two requests)");
        qputenv("XMB_HYDRA_HOT_URL",(base+QStringLiteral("/hot-500-always")).toUtf8());
        titles.append(PopularTitle{});fixtureError.clear();
        QElapsedTimer hot500Clock;hot500Clock.start();
        check(!HydraLibrary().popularTitles(&titles,&fixtureError) && titles.isEmpty() &&
              fixtureError.contains(QStringLiteral("HTTP 500")) && fixtureError.contains(QStringLiteral("catalogue")) &&
              !fixtureError.contains(QStringLiteral("library")) && !fixtureError.contains(QStringLiteral("127.0.0.1")),
              "repeated hot 500 fails bounded, names the catalogue stage, and leaks no URL");
        check(hot500AlwaysRequests==3,"repeated hot 500 is bounded to the retry attempt cap");
        check(hot500Clock.elapsed()<10000,"repeated hot 500 returns within the short backoff budget");
        qputenv("XMB_HYDRA_HOT_URL",(base+QStringLiteral("/hot-403")).toUtf8());
        titles.clear();fixtureError.clear();
        check(!HydraLibrary().popularTitles(&titles,&fixtureError) && titles.isEmpty() &&
              fixtureError.contains(QStringLiteral("HTTP 403")) && fixtureError.contains(QStringLiteral("refused access")) &&
              !fixtureError.contains(QStringLiteral("retry")),
              "hot catalogue 403 is terminal and worded as an access refusal, not a retryable failure");
        check(hot403Requests==1,"hot catalogue 403 is never auto-retried");
        qputenv("XMB_HYDRA_HOT_URL",(base+QStringLiteral("/hot-429")).toUtf8());
        titles.clear();fixtureError.clear();
        check(!HydraLibrary().popularTitles(&titles,&fixtureError) && titles.isEmpty() &&
              fixtureError.contains(QStringLiteral("HTTP 429")),
              "hot catalogue 429 rate limit is terminal");
        check(hot429Requests==1,"hot catalogue 429 is never auto-retried");
        // Local ranked-catalogue cache (opt-in): genres survive a cache hit, live vs cached
        // grouping/rank/genre counts are identical, an old-schema entry refetches, the bounded hot
        // retry stores only its eventual success, and a 403 stays terminal with no stale fallback.
        {
            QTemporaryDir cacheDir;
            check(cacheDir.isValid(),"hydra cache integration tempdir");

            qputenv("XMB_HYDRA_HOT_URL",(base+QStringLiteral("/hot")).toUtf8());
            const int hotBefore=hotRequests;
            QVector<PopularTitle> live;QString cerr;
            check(HydraLibrary(QString{},cacheDir.path()).popularTitles(&live,&cerr) && live.size()==40 && cerr.isEmpty(),
                  "cache: live fetch stores the ranked catalogue");
            check(hotRequests==hotBefore+1,"cache: live fetch issues exactly one request");
            QVector<PopularTitle> cached;cerr.clear();
            check(HydraLibrary(QString{},cacheDir.path()).popularTitles(&cached,&cerr) && cached.size()==40 && cerr.isEmpty(),
                  "cache: second call within TTL returns the cached catalogue");
            check(hotRequests==hotBefore+1,"cache: a fresh hit issues no new request");
            check(cached[0].genres==(QStringList{QStringLiteral("Action")}),"cache: genres survive a hit (first title)");
            check(cached[10].genres==(QStringList{QStringLiteral("RPG"),QStringLiteral("Action")}),"cache: multi-genre order survives a hit");
            check(cached[12].genres==(QStringList{QStringLiteral("RPG")}),"cache: edition genre survives a hit");
            const auto liveGroups=groupPopularTitles(live);
            const auto cachedGroups=groupPopularTitles(cached);
            check(liveGroups.size()==cachedGroups.size(),"cache: grouped tile count matches live");
            check(cachedGroups[10].genres==(QStringList{QStringLiteral("RPG"),QStringLiteral("Action")}),"cache: grouped genre union survives a hit");
            const auto liveGenres=groupGenres(liveGroups);
            const auto cachedGenres=groupGenres(cachedGroups);
            check(liveGenres.size()==cachedGenres.size() && cachedGenres.size()==3,"cache: distinct genre count matches live");
            check(cachedGenres[0].name==QStringLiteral("Adventure") && cachedGenres[0].count==27,"cache: ranked genre counts match live (Adventure 27)");
            check(cachedGenres[1].name==QStringLiteral("Action") && cachedGenres[1].count==11,"cache: ranked genre counts match live (Action 11)");
            check(cachedGenres[2].name==QStringLiteral("RPG") && cachedGenres[2].count==2,"cache: ranked genre counts match live (RPG 2)");

            const QString hotUrl=base+QStringLiteral("/hot");
            { QFile f(HydraCatalogueCache(cacheDir.path()).pathFor(hotUrl));check(f.open(QIODevice::WriteOnly),"old-schema fixture open");
              f.write(QByteArrayLiteral("{\"endpoint\":\"")+hotUrl.toUtf8()+QByteArrayLiteral("\",\"storedAtMs\":")+
                      QByteArray::number(QDateTime::currentMSecsSinceEpoch())+
                      QByteArrayLiteral(",\"titles\":[{\"title\":\"Old Schema Game\"}]}")); }
            const int oldBefore=hotRequests;
            QVector<PopularTitle> refetched;cerr.clear();
            check(HydraLibrary(QString{},cacheDir.path()).popularTitles(&refetched,&cerr) && refetched.size()==40 && cerr.isEmpty(),
                  "cache: an old-schema entry is a miss and refetches");
            check(hotRequests==oldBefore+1,"cache: the old-schema miss still costs exactly one refetch");
            check(refetched[0].genres==(QStringList{QStringLiteral("Action")}),"cache: refetch repopulates genres under the current schema");

            qputenv("XMB_HYDRA_HOT_URL",(base+QStringLiteral("/hot-cache-500-then-success")).toUtf8());
            const int retryBefore=hotCache500ThenSuccessRequests;
            QVector<PopularTitle> r1;cerr.clear();
            check(HydraLibrary(QString{},cacheDir.path()).popularTitles(&r1,&cerr) && r1.size()==40 && cerr.isEmpty() &&
                  r1[0].genres==(QStringList{QStringLiteral("Action")}),
                  "cache: a transient 500 is retried to a genre-bearing success and stored");
            check(hotCache500ThenSuccessRequests==retryBefore+2,"cache: the 500->success path costs two requests then stores");
            QVector<PopularTitle> r2;cerr.clear();
            check(HydraLibrary(QString{},cacheDir.path()).popularTitles(&r2,&cerr) && r2.size()==40 && cerr.isEmpty() &&
                  r2[0].genres==(QStringList{QStringLiteral("Action")}),
                  "cache: the retried success (with genres) is served from cache on the next call");
            check(hotCache500ThenSuccessRequests==retryBefore+2,"cache: the cached retried success issues no new request");

            qputenv("XMB_HYDRA_HOT_URL",(base+QStringLiteral("/hot-403")).toUtf8());
            const QString hot403=base+QStringLiteral("/hot-403");
            QVector<PopularTitle> seed;PopularTitle st;st.title=QStringLiteral("Stale Cached");seed.append(st);
            check(HydraCatalogueCache(cacheDir.path()).store(hot403,seed,
                  QDateTime::currentMSecsSinceEpoch()-HydraCatalogueCache::ttlMs()-1000),
                  "cache: seed a stale, identity-matched entry for the 403 endpoint");
            const int c403Before=hot403Requests;
            QVector<PopularTitle> c3;cerr.clear();
            check(!HydraLibrary(QString{},cacheDir.path()).popularTitles(&c3,&cerr) && c3.isEmpty() &&
                  cerr.contains(QStringLiteral("403")) && cerr.contains(QStringLiteral("refused access")),
                  "cache: a 403 surfaces the access refusal and never serves the stale copy");
            check(hot403Requests==c403Before+1,"cache: the 403 fetch still issues its request");
            QVector<PopularTitle> c4;cerr.clear();
            check(!HydraLibrary(QString{},cacheDir.path()).popularTitles(&c4,&cerr) && c4.isEmpty() && cerr.contains(QStringLiteral("403")),
                  "cache: 403 is never persisted, so a retry still fails");
            check(hot403Requests==c403Before+2,"cache: a second 403 fetch issues another request");
        }
        qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources")).toUtf8());
        qputenv("XMB_HYDRA_SEARCH_URL",(base+QStringLiteral("/search")).toUtf8());
        QVector<HydraSource> fixtureSources;fixtureError=QStringLiteral("stale error");
         check(HydraLibrary().listSources(&fixtureSources,&fixtureError) && fixtureSources.size()==2 && fixtureError.isEmpty(),
               "public listSources returns all valid fixture sources and clears prior error");
         qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources-fail")).toUtf8());fixtureSources.clear();fixtureError.clear();
         check(!HydraLibrary().listSources(&fixtureSources,&fixtureError) && fixtureSources.isEmpty() && fixtureError.contains(QStringLiteral("retry")),
               "HTTP source-list failure is actionable and does not publish an empty success");
        qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources-malformed")).toUtf8());
        fixtureSources.append(HydraSource{});fixtureError.clear();
        check(!HydraLibrary().listSources(&fixtureSources,&fixtureError) && fixtureSources.isEmpty() &&
              fixtureError.contains(QStringLiteral("invalid source entry")),
              "malformed source member fails the entire sources response");
        qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources-empty")).toUtf8());
        fixtureSources.append(HydraSource{});fixtureError=QStringLiteral("stale error");
        check(HydraLibrary().listSources(&fixtureSources,&fixtureError) && fixtureSources.isEmpty() && fixtureError.isEmpty(),
              "valid empty sources array clears a previous error");
        qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources-oversized")).toUtf8());
        fixtureSources.clear();fixtureError.clear();
        check(!HydraLibrary().listSources(&fixtureSources,&fixtureError) && fixtureSources.isEmpty() &&
              fixtureError.contains(QStringLiteral("16 MiB")),
              "directory listing still rejects a body over 16 MiB (source feeds get a separate 64 MiB cap)");
        qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources")).toUtf8());
        {
            // Browse Sources opens with the public listSources directory request. A transient 500 there used to
            // end the screen with an error on the first attempt; it is now retried inside one shared budget.
            // Backoff is shortened (fixture-only) so these cases stay fast; the budget case sets its own.
            qputenv("XMB_HYDRA_RETRY_BACKOFF_MS","50");
            QVector<HydraSource> dirSources;QString dirError;
            qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources-500-then-success")).toUtf8());
            dirSources.append(HydraSource{});dirError=QStringLiteral("stale error");
            const bool dirFlakyOk=HydraLibrary().listSources(&dirSources,&dirError);
            check(dirFlakyOk && dirSources.size()==2 && dirError.isEmpty(),"directory: a transient 500 recovers to the complete source list");
            check(dirFlakyRequests==2,"directory: a single 500 costs exactly one automatic retry (two requests)");
            // The search path shares the directory request, so it gets the same recovery.
            dirFlakyRequests=0;
            qputenv("XMB_HYDRA_SEARCH_URL",(base+QStringLiteral("/search")).toUtf8());
            QVector<GameResult> dirSearch;dirError=QStringLiteral("stale error");
            check(HydraLibrary().searchGames(QStringLiteral("fixture"),&dirSearch,&dirError) && dirSearch.size()==1 && dirError.isEmpty(),
                  "directory: game search also recovers from a transient directory 500");

            // A 500 on a LATER page retries that page only: pages already read are kept and the list is complete.
            qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources-paged-flaky")).toUtf8());
            dirSources.clear();dirError.clear();
            const bool pagedOk=HydraLibrary().listSources(&dirSources,&dirError);
            check(pagedOk && dirSources.size()==3 && dirError.isEmpty(),"directory: a page-two 500 recovers to the complete paginated list");
            check(dirSources.size()==3 && dirSources[0].title==QStringLiteral("Paged A") && dirSources[1].title==QStringLiteral("Paged B") && dirSources[2].title==QStringLiteral("Paged C"),
                  "directory: recovered pages keep their order with no duplicates");
            check(dirPagedRequests.value(1)==1 && dirPagedRequests.value(2)==2,"directory: only the failed page is retried (page one is not re-fetched)");

            // A page that keeps failing fails the WHOLE listing: no partial source list is ever returned.
            qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources-paged-down")).toUtf8());
            dirSources.clear();dirSources.append(HydraSource{});dirError.clear();
            QElapsedTimer downClock;downClock.start();
            const bool downOk=HydraLibrary().listSources(&dirSources,&dirError);
            check(!downOk && dirSources.isEmpty(),"directory: repeated failure returns zero entries, never a partial list");
            check(dirError.contains(QStringLiteral("source directory")) && dirError.contains(QStringLiteral("HTTP 500")) && dirError.contains(QStringLiteral("retry")),
                  "directory: the error names the source directory, the HTTP status and that retry is possible");
            check(dirDownRequests.value(1)==1 && dirDownRequests.value(2)==3,"directory: repeated 500 is bounded to the attempt cap on the failing page");
            check(downClock.elapsed()<10000,"directory: repeated 500 returns promptly inside the budget");

            // Terminal statuses are never retried automatically.
            qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources-403")).toUtf8());
            dirSources.clear();dirError.clear();
            check(!HydraLibrary().listSources(&dirSources,&dirError) && dirSources.isEmpty() && dirError.contains(QStringLiteral("HTTP 403")) && dirError.contains(QStringLiteral("source directory")),
                  "directory: HTTP 403 is surfaced with its status");
            check(dir403Requests==1,"directory: HTTP 403 is terminal (exactly one request)");
            {
                HydraFailure directoryFailure;dirSources.clear();dirError.clear();
                HydraLibrary().listSources(&dirSources,&dirError,&directoryFailure);
                check(directoryFailure.kind==HydraFailureKind::Refused && directoryFailure.status==403 && !dirError.contains(QStringLiteral("retry")),
                      "directory: a 403 is classified Refused and its error makes no retry promise");
            }
            qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources-429")).toUtf8());
            dirSources.clear();dirError.clear();
            QElapsedTimer rateClock;rateClock.start();
            check(!HydraLibrary().listSources(&dirSources,&dirError) && dirSources.isEmpty() && dirError.contains(QStringLiteral("HTTP 429")),
                  "directory: HTTP 429 is surfaced with its status");
            check(dir429Requests==1 && rateClock.elapsed()<1500,"directory: HTTP 429 with Retry-After is terminal and does not wait");

            // ONE budget covers every page and backoff of the directory action: page one burns part of it on its
            // own retry, so the always-failing page two cannot start a fresh budget (three attempts) of its own.
            qputenv("XMB_HYDRA_RETRY_BACKOFF_MS","400");
            qputenv("XMB_HYDRA_RETRY_BUDGET_MS","1000");
            qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources-budget")).toUtf8());
            dirSources.clear();dirSources.append(HydraSource{});dirError.clear();
            QElapsedTimer budgetClock;budgetClock.start();
            const bool budgetOk=HydraLibrary().listSources(&dirSources,&dirError);
            const qint64 budgetMs=budgetClock.elapsed();
            check(!budgetOk && dirSources.isEmpty() && dirError.contains(QStringLiteral("HTTP 500")),"directory: budget exhaustion fails the whole listing with the last status");
            check(dirBudgetRequests.value(1)==2 && dirBudgetRequests.value(2)>=1 && dirBudgetRequests.value(2)<3,
                  "directory: the budget is shared across pages (the failing page gets fewer than a full attempt cap)");
            check(budgetMs<1000+1500,"directory: total time stays within the shared budget plus scheduling slack");
            qunsetenv("XMB_HYDRA_RETRY_BUDGET_MS");qunsetenv("XMB_HYDRA_RETRY_BACKOFF_MS");
            qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources")).toUtf8());
        }
        QVector<GameResult> searchResults;fixtureError=QStringLiteral("stale error");
         check(HydraLibrary().searchGames(QStringLiteral("fixture"),&searchResults,&fixtureError) && searchResults.size()==1 &&
               searchResults.first().options.size()==2 && fixtureError.isEmpty(),
               "public searchGames returns the complete all-source result");
         QVector<GameResult> ranked;
         for(const auto& name:{QStringLiteral("Fixture Game Deluxe"),QStringLiteral("Fixture Game"),QStringLiteral("A Fixture Game"),QStringLiteral("Fixture Games"),QStringLiteral("Unrelated")}) { GameResult game;game.name=name;ranked.append(game); }
         rankGameResults(&ranked,QStringLiteral("fixture game"));
         check(ranked.size()==5 && ranked[0].name==QStringLiteral("Fixture Game") && ranked[1].name==QStringLiteral("Fixture Game Deluxe"),
               "search ranking places exact then contained matches before broad results");
         check(ranked[2].name==QStringLiteral("Fixture Games") && ranked[3].name==QStringLiteral("A Fixture Game") && ranked[4].name==QStringLiteral("Unrelated"),
               "search ranking is stable within matching tiers and never drops unmatched rows");
         QVector<GameResult> doomRanked;for(const auto& name:{QStringLiteral("a doom"),QStringLiteral("doom2"),QStringLiteral("doom")}){GameResult game;game.name=name;doomRanked.append(game);}rankGameResults(&doomRanked,QStringLiteral("doom"));
         check(doomRanked[0].name==QStringLiteral("doom") && doomRanked[1].name==QStringLiteral("doom2") && doomRanked[2].name==QStringLiteral("a doom"),
               "literal prefix ranking keeps doom2 ahead of broad a-doom matches");
         HydraSource feed;feed.title=QStringLiteral("Fixture Feed");feed.url=QUrl(base+QStringLiteral("/source-feed"));
         QVector<GameResult> feedGames;fixtureError.clear();
         const bool feedOk=HydraLibrary().browseSource(feed,&feedGames,&fixtureError);
         check(feedOk && feedGames.size()==1 &&
               feedGames.first().options.size()==3 && feedGames.first().options[0].releaseTitle!=feedGames.first().options[1].releaseTitle,
               "source feed keeps two normalized-name editions selectable under one source URL");
         bool hasDirect=false,hasMagnet=false,hasUnavailable=false;
         for(const auto& option:feedGames.first().options) {
             hasDirect|=option.downloads.size()==1 && option.downloads.first().kind==QStringLiteral("direct");
             hasMagnet|=option.downloads.size()==1 && option.downloads.first().kind==QStringLiteral("magnet");
             hasUnavailable|=option.downloads.isEmpty() && option.sourceStatus.contains(QStringLiteral("no supported"));
         }
         check(hasDirect && hasMagnet && hasUnavailable,"source editions preserve direct/magnet identity and visibly mark unsupported links unavailable");
         feed.url=QUrl(base+QStringLiteral("/source-malformed"));feedGames.append(GameResult{});fixtureError.clear();
         check(!HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.isEmpty() && fixtureError.contains(QStringLiteral("invalid JSON")),
               "malformed source feed fails without partial titles");
         feed.url=QUrl(base+QStringLiteral("/source-just-over-16m"));feedGames.clear();fixtureError=QStringLiteral("stale error");
         check(HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.size()==1 &&
               feedGames.first().name==QStringLiteral("Just Over 16 MiB Game") &&
               feedGames.first().options.size()==1 && feedGames.first().options.first().downloads.size()==1 &&
               feedGames.first().options.first().downloads.first().kind==QStringLiteral("direct") && fixtureError.isEmpty(),
               "a valid source feed just over the old 16 MiB cap is accepted by the 64 MiB source cap");
         feed.url=QUrl(base+QStringLiteral("/source-over-64m"));feedGames.append(GameResult{});fixtureError.clear();
         check(!HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.isEmpty() && fixtureError.contains(QStringLiteral("64 MiB")),
               "single-response source body over 64 MiB is rejected by its declared length");
         feed.url=QUrl(base+QStringLiteral("/source-over-64m-chunked"));fixtureError.clear();
         check(!HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.isEmpty() && fixtureError.contains(QStringLiteral("64 MiB")),
               "chunked source body over 64 MiB is rejected while receiving");
         feed.url=QUrl(base+QStringLiteral("/source-redirect"));fixtureError.clear();
         check(HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.size()==1,
               "HTTP source redirect remains bounded and follows only a safe same-scheme target");
         feed.url=QUrl(base+QStringLiteral("/source-unsafe-redirect"));fixtureError.clear();feedGames.append(GameResult{});
         check(!HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.isEmpty() && fixtureError.contains(QStringLiteral("retry")),
               "non-HTTP(S) redirect target is rejected before it can be followed");
         // Automatic retry on the selected-feed GET: one transient 500 is retried and the feed
         // succeeds; repeated 500 is bounded with a useful error and no partial output; a permanent
         // 404 and a gateway 503 whose Retry-After exceeds the budget are terminal (not retried).
         feed.url=QUrl(base+QStringLiteral("/source-500-then-success"));feedGames.clear();fixtureError=QStringLiteral("stale error");
         check(HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.size()==1 &&
               feedGames.first().name==QStringLiteral("Retry Then Success Game") &&
               feedGames.first().options.size()==1 && feedGames.first().options.first().downloads.size()==1 &&
               feedGames.first().options.first().downloads.first().kind==QStringLiteral("direct") && fixtureError.isEmpty(),
               "transient HTTP 500 on a selected feed is retried once and then succeeds");
         check(source500ThenSuccessRequests==2,"a single 500 costs exactly one automatic retry (two requests)");
         feed.url=QUrl(base+QStringLiteral("/source-500-always"));feedGames.append(GameResult{});fixtureError.clear();
         QElapsedTimer retryClock;retryClock.start();
         check(!HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.isEmpty() &&
               fixtureError.contains(QStringLiteral("HTTP 500")) && fixtureError.contains(QStringLiteral("retry")) &&
               !fixtureError.contains(QStringLiteral("127.0.0.1")),
               "repeated HTTP 500 fails bounded with a useful error and no partial output");
         check(source500AlwaysRequests==3,"repeated 500 is bounded to the retry attempt cap");
         check(retryClock.elapsed()<10000,"repeated 500 returns within the short backoff budget");
         feed.url=QUrl(base+QStringLiteral("/source-404"));fixtureError.clear();
         check(!HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.isEmpty() &&
               fixtureError.contains(QStringLiteral("HTTP 404")) && source404Requests==1,
               "permanent HTTP 404 on a selected feed is terminal (never retried)");
         feed.url=QUrl(base+QStringLiteral("/source-503-retry-after"));fixtureError.clear();
         const int retryAfterBefore=source503RetryAfterRequests;
         QElapsedTimer retryAfterClock;retryAfterClock.start();
         check(!HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.isEmpty() &&
               fixtureError.contains(QStringLiteral("HTTP 503")),
               "gateway 503 with Retry-After is surfaced as retryable");
         check(source503RetryAfterRequests==retryAfterBefore+1 && retryAfterClock.elapsed()<2000,
               "Retry-After beyond the budget stops retries without early repetition or an unbounded wait");
         // Inactivity deadline: a stalled first GET is aborted at the idle deadline and retried,
         // not allowed to consume the whole budget. A short test-only inactivity keeps this fast.
         qputenv("XMB_HYDRA_RETRY_INACTIVITY_MS","500");
         feed.url=QUrl(base+QStringLiteral("/source-stall-then-success"));feedGames.clear();fixtureError=QStringLiteral("stale error");
         QElapsedTimer stallClock;stallClock.start();
         check(HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.size()==1 &&
               feedGames.first().name==QStringLiteral("Stall Then Success Game") && fixtureError.isEmpty(),
               "a stalled first GET times out at the inactivity deadline and is retried to success");
         check(sourceStallRequests==2,"the stalled feed costs one inactivity abort plus one successful retry");
         check(stallClock.elapsed()>=700 && stallClock.elapsed()<5000,
               "the stall test uses the short test inactivity deadline, not the full budget");
         qunsetenv("XMB_HYDRA_RETRY_INACTIVITY_MS");
         // A 429 rate limit is terminal: never auto-retried (the explicit Retry action remains).
         feed.url=QUrl(base+QStringLiteral("/source-429"));feedGames.append(GameResult{});fixtureError.clear();
         check(!HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.isEmpty() &&
               fixtureError.contains(QStringLiteral("HTTP 429")) && source429Requests==1,
               "HTTP 429 rate limit on a selected feed is terminal (never auto-retried)");
         // Failure categories drive the on-screen reason and whether a Retry row is offered. Refusals are
         // reported as refusals (no "retry" promise), classified, and never replayed.
         {
             HydraFailure failure;
             feed.url=QUrl(base+QStringLiteral("/source-403"));feedGames.append(GameResult{});fixtureError.clear();
             check(!HydraLibrary().browseSource(feed,&feedGames,&fixtureError,&failure) && feedGames.isEmpty() &&
                   failure.kind==HydraFailureKind::Refused && failure.status==403 && source403Requests==1,
                   "a 403 on a selected feed is classified Refused and requested exactly once (no automatic retry)");
             check(fixtureError.contains(QStringLiteral("refused access (HTTP 403)")) && !fixtureError.contains(QStringLiteral("retry")),
                   "a refused feed's error does not promise that retrying helps");
             failure=HydraFailure{};
             feed.url=QUrl(base+QStringLiteral("/source-401"));feedGames.append(GameResult{});fixtureError.clear();
             check(!HydraLibrary().browseSource(feed,&feedGames,&fixtureError,&failure) &&
                   failure.kind==HydraFailureKind::Refused && failure.status==401 && source401Requests==1,
                   "a 401 on a selected feed is classified Refused and requested exactly once");
             failure=HydraFailure{};
             feed.url=QUrl(base+QStringLiteral("/source-429"));feedGames.append(GameResult{});fixtureError.clear();
             check(!HydraLibrary().browseSource(feed,&feedGames,&fixtureError,&failure) &&
                   failure.kind==HydraFailureKind::RateLimited && failure.status==429 && fixtureError.contains(QStringLiteral("try again later")),
                   "a 429 on a selected feed is classified RateLimited and asks to try again later");
             failure=HydraFailure{};
             feed.url=QUrl(base+QStringLiteral("/source-over-64m"));feedGames.append(GameResult{});fixtureError.clear();
             check(!HydraLibrary().browseSource(feed,&feedGames,&fixtureError,&failure) &&
                   failure.kind==HydraFailureKind::TooLarge && !fixtureError.contains(QStringLiteral("retry")),
                   "a feed over the 64 MiB cap is classified TooLarge without a retry promise");
             failure=HydraFailure{};
             feed.url=QUrl(base+QStringLiteral("/source-500-always"));feedGames.append(GameResult{});fixtureError.clear();
             check(!HydraLibrary().browseSource(feed,&feedGames,&fixtureError,&failure) &&
                   failure.kind==HydraFailureKind::Server && failure.status==500 && fixtureError.contains(QStringLiteral("retry")),
                   "a persistent 5xx is classified Server and keeps the retry wording");
             failure=HydraFailure{};
             feed.url=QUrl(base+QStringLiteral("/source-feed-large"));feedGames.clear();fixtureError=QStringLiteral("stale error");
             check(HydraLibrary().browseSource(feed,&feedGames,&fixtureError,&failure) && failure.kind==HydraFailureKind::None,
                   "a successful open leaves the failure category at None");
         }
         // Source-health note: persisted, ordered, and a refusal is asked again at most once a day.
         {
             QTemporaryDir healthDir;
             const QString healthPath=healthDir.filePath(QStringLiteral("source_health.json"));
             const qint64 t0=1800000000;
             const auto url=[](const char* text){return QUrl(QString::fromLatin1(text));};
             HydraFailure refused{HydraFailureKind::Refused,403},none{},large{HydraFailureKind::TooLarge,0},server{HydraFailureKind::Server,503};
             {
                 SourceHealth health(healthPath);
                 check(!health.load() && health.feedCount()==0,"a missing health note loads as empty");
                 health.record(url("https://blocked.invalid/a.json"),refused,t0);
                 health.record(url("https://blocked.invalid/b.json"),refused,t0);
                 health.record(url("https://fine.invalid/ok.json"),none,t0);
                 health.record(url("https://fine.invalid/big.json"),large,t0);
                 health.record(url("https://flaky.invalid/f.json"),server,t0);
                 check(health.save(),"the health note saves");
             }
             SourceHealth health(healthPath);
             check(health.load() && health.feedCount()==5,"the health note reloads every recorded feed");
             check(health.standing(url("https://blocked.invalid/a.json"),t0+60)==SourceHealth::Standing::Blocked &&
                   health.standing(url("https://fine.invalid/ok.json"),t0+60)==SourceHealth::Standing::Working &&
                   health.standing(url("https://fine.invalid/big.json"),t0+60)==SourceHealth::Standing::TooLarge &&
                   health.standing(url("https://flaky.invalid/f.json"),t0+60)==SourceHealth::Standing::Failing,
                   "reloaded standings: blocked, working, too large and failing");
             check(health.standing(url("https://blocked.invalid/never-opened.json"),t0+60)==SourceHealth::Standing::LikelyBlocked,
                   "a never-opened feed on a host that refused two feeds is marked likely blocked");
             check(health.standing(url("https://elsewhere.invalid/x.json"),t0+60)==SourceHealth::Standing::Unknown &&
                   health.standing(url("https://blocked.invalid/never-opened.json"),t0+SourceHealth::kReprobeSeconds+60)==SourceHealth::Standing::Unknown,
                   "an unknown host stays unknown, and the per-host hint expires after a day");
             QVector<HydraSource> listed;
             for(const char* text:{"https://blocked.invalid/a.json","https://blocked.invalid/never-opened.json","https://elsewhere.invalid/x.json",
                                   "https://fine.invalid/big.json","https://flaky.invalid/f.json","https://fine.invalid/ok.json"}) {
                 HydraSource source;source.url=url(text);source.title=QString::fromLatin1(text);listed.append(source);
             }
             const auto sorted=health.ordered(listed,t0+60);
             QStringList order;for(const auto& source:sorted)order.append(source.url.path());
             check(order==QStringList({QStringLiteral("/ok.json"),QStringLiteral("/x.json"),QStringLiteral("/f.json"),
                                       QStringLiteral("/never-opened.json"),QStringLiteral("/big.json"),QStringLiteral("/a.json")}),
                   "working feeds sort first, then unknown, failing, likely blocked, too large and blocked");
             QFile noteFile(healthPath);const bool noteOpened=noteFile.open(QIODevice::ReadOnly);
             const QByteArray noteBytes=noteFile.readAll();noteFile.close();
             check(noteOpened && !noteBytes.contains("invalid") && !noteBytes.contains("http") && !noteBytes.contains("a.json"),
                   "the health note holds no URLs or host names");
             check(health.skipProbe(url("https://blocked.invalid/a.json"),t0+SourceHealth::kReprobeSeconds-1) &&
                   !health.skipProbe(url("https://blocked.invalid/a.json"),t0+SourceHealth::kReprobeSeconds) &&
                   !health.skipProbe(url("https://flaky.invalid/f.json"),t0+60) && !health.skipProbe(url("https://fine.invalid/ok.json"),t0+60),
                   "a refusal is not re-asked within a day, then is due again; transient and working feeds always probe");
             health.record(url("https://blocked.invalid/a.json"),none,t0+SourceHealth::kReprobeSeconds+10);
             check(health.standing(url("https://blocked.invalid/a.json"),t0+SourceHealth::kReprobeSeconds+20)==SourceHealth::Standing::Working &&
                   health.standing(url("https://blocked.invalid/never-opened.json"),t0+SourceHealth::kReprobeSeconds+20)==SourceHealth::Standing::Unknown,
                   "a feed that recovers is working again and clears its host's blocked hint");
             {   // a damaged note is forgotten, not fatal
                 QFile damaged(healthPath);const bool damagedOpened=damaged.open(QIODevice::WriteOnly|QIODevice::Truncate);check(damagedOpened,"the damaged-note fixture file opens");damaged.write("{not json");damaged.close();
                 SourceHealth broken(healthPath);
                 check(!broken.load() && broken.feedCount()==0 && broken.standing(url("https://blocked.invalid/a.json"),t0)==SourceHealth::Standing::Unknown,
                       "a damaged health note loads as empty");
                 broken.record(url("https://fine.invalid/ok.json"),none,t0);
                 check(broken.save() && SourceHealth(healthPath).load(),"a damaged health note is overwritten by the next save");
             }
             // Through the real open path: the first 403 asks the host once; reopening within a day does not ask
             // again and still reports the refusal; after a day it asks again. Nothing retries on its own.
             SourceHealth noted(healthPath);noted.load();
             HydraSource refusing;refusing.title=QStringLiteral("Refusing");refusing.url=QUrl(base+QStringLiteral("/source-403"));
             const int refusedBefore=source403Requests;
             QVector<GameResult> noteGames;QString noteError;HydraFailure noteFailure;bool fromNote=true;
             check(!HydraLibrary().browseSourceNoted(refusing,&noteGames,&noteError,&noteFailure,&noted,t0,&fromNote) && !fromNote &&
                   noteFailure.kind==HydraFailureKind::Refused && source403Requests==refusedBefore+1,
                   "the first open of a refusing feed asks the host once and records the refusal");
             check(!HydraLibrary().browseSourceNoted(refusing,&noteGames,&noteError,&noteFailure,&noted,t0+3600,&fromNote) && fromNote &&
                   noteFailure.kind==HydraFailureKind::Refused && noteFailure.status==403 && source403Requests==refusedBefore+1 &&
                   !noteError.contains(QStringLiteral("retry")),
                   "reopening within a day reports the stored refusal without another request");
             check(!HydraLibrary().browseSourceNoted(refusing,&noteGames,&noteError,&noteFailure,&noted,t0+SourceHealth::kReprobeSeconds+1,&fromNote) &&
                   !fromNote && source403Requests==refusedBefore+2,
                   "after a day the refusal is re-checked once");
             HydraSource recovering;recovering.title=QStringLiteral("Recovering");recovering.url=QUrl(base+QStringLiteral("/source-feed-large"));
             noted.record(recovering.url,refused,t0);
             check(HydraLibrary().browseSourceNoted(recovering,&noteGames,&noteError,&noteFailure,&noted,t0+SourceHealth::kReprobeSeconds+1,&fromNote) &&
                   !fromNote && noteGames.size()==50 && noted.standing(recovering.url,t0+SourceHealth::kReprobeSeconds+2)==SourceHealth::Standing::Working,
                   "a feed that was refused yesterday and opens now is re-probed and marked working");
         }
         // A positive Retry-After is honoured: the retry waits the server's stated delay, not just the short backoff.
         feed.url=QUrl(base+QStringLiteral("/source-retryafter-short"));feedGames.clear();fixtureError=QStringLiteral("stale error");
         QElapsedTimer honoredClock;honoredClock.start();
         check(HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.size()==1 &&
               feedGames.first().name==QStringLiteral("Retry After Honored Game") && fixtureError.isEmpty(),
               "a 503 with a small Retry-After is retried after the stated delay and succeeds");
         check(source503RetryAfterShortRequests==2 && honoredClock.elapsed()>=900,
               "the positive Retry-After delay is actually waited (>= Retry-After seconds) before retrying");
         // A malformed Retry-After is treated conservatively: no premature retry.
         feed.url=QUrl(base+QStringLiteral("/source-retryafter-malformed"));fixtureError.clear();
         check(!HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.isEmpty() &&
               fixtureError.contains(QStringLiteral("HTTP 503")) && source503RetryAfterMalformedRequests==1,
               "an unsupported/malformed Retry-After does not trigger a premature retry");
         // Absolute deadline: a progressing (drip-fed) body that would outlive the budget is cut off
         // at the budget (idle deadline set far above it, so the cutoff is unambiguously absolute),
         // with no partial output and no further attempt once the budget is exhausted.
         qputenv("XMB_HYDRA_RETRY_BUDGET_MS","1000");
         qputenv("XMB_HYDRA_RETRY_INACTIVITY_MS","5000");
         feed.url=QUrl(base+QStringLiteral("/source-drip-feed"));feedGames.clear();fixtureError=QStringLiteral("stale error");
         QElapsedTimer dripClock;dripClock.start();
         check(!HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.isEmpty() &&
               fixtureError.contains(QStringLiteral("timed out")) && sourceDripFeedRequests==1,
               "a drip-fed body past the budget is cut off by the absolute deadline, with no partial output and no retry");
         check(dripClock.elapsed()<4000,"the absolute deadline bounds a progressing feed well short of its full drip");
         qunsetenv("XMB_HYDRA_RETRY_BUDGET_MS");qunsetenv("XMB_HYDRA_RETRY_INACTIVITY_MS");
         // Within-budget progressing feed: a drip that finishes inside the budget completes normally.
         qputenv("XMB_HYDRA_RETRY_BUDGET_MS","3000");
         qputenv("XMB_HYDRA_RETRY_INACTIVITY_MS","800");
         feed.url=QUrl(base+QStringLiteral("/source-progress-ok"));feedGames.clear();fixtureError=QStringLiteral("stale error");
         check(HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.size()==1 &&
               feedGames.first().name==QStringLiteral("Drip Feed Ok Game") && fixtureError.isEmpty(),
               "a progressing feed that finishes within the budget completes normally");
         check(sourceDripFeedOkRequests==1,"the within-budget progressing feed succeeds on the first attempt");
         qunsetenv("XMB_HYDRA_RETRY_BUDGET_MS");qunsetenv("XMB_HYDRA_RETRY_INACTIVITY_MS");
        qputenv("XMB_HYDRA_SEARCH_URL",(base+QStringLiteral("/search-malformed")).toUtf8());
        searchResults.append(GameResult{});fixtureError.clear();
        check(!HydraLibrary().searchGames(QStringLiteral("fixture"),&searchResults,&fixtureError) && searchResults.isEmpty() &&
              fixtureError.contains(QStringLiteral("invalid game entry")),
              "malformed search game member fails without publishing partial records");
        qputenv("XMB_HYDRA_SEARCH_URL",(base+QStringLiteral("/search-empty")).toUtf8());
        searchResults.append(GameResult{});fixtureError=QStringLiteral("stale error");
        check(HydraLibrary().searchGames(QStringLiteral("fixture"),&searchResults,&fixtureError) && searchResults.isEmpty() && fixtureError.isEmpty(),
              "valid empty search games array clears a previous error");
        // Lazy resolution: opening a source fetches only its index (no directory/search fan-out),
        // and resolving one selected game runs exactly one search (one directory fetch), not N.
        qputenv("XMB_HYDRA_SEARCH_URL",(base+QStringLiteral("/search")).toUtf8());
        feed.url=QUrl(base+QStringLiteral("/source-feed-large"));feedGames.clear();fixtureError=QStringLiteral("stale error");
        const int largeFeedBefore=sourceFeedRequests;
        const int largeSourcesBefore=sourcesListRequests;
        const int largeSearchBefore=searchRequests;
        check(HydraLibrary().browseSource(feed,&feedGames,&fixtureError) && feedGames.size()==50 && fixtureError.isEmpty(),
              "browsing a 50-game source returns every entry");
        check(sourceFeedRequests==largeFeedBefore+1 && sourcesListRequests==largeSourcesBefore && searchRequests==largeSearchBefore,
              "opening the large source costs exactly one feed GET and no directory/search requests");
        // A 100k-entry (~50 MB) feed must open: above the 16 MiB directory cap, under the 64 MiB feed cap,
        // with one feed GET and no per-game directory/search fan-out, in bounded time and memory.
        {
            feed.url=QUrl(base+QStringLiteral("/source-feed-100k"));feedGames.clear();fixtureError=QStringLiteral("stale error");
            const int bigFeedBefore=sourceFeedRequests,bigSourcesBefore=sourcesListRequests,bigSearchBefore=searchRequests;
            QElapsedTimer bigClock;bigClock.start();
            const bool bigOk=HydraLibrary().browseSource(feed,&feedGames,&fixtureError);
            const qint64 bigMs=bigClock.elapsed();
            const qint64 peakMb=peakWorkingSetBytes()/(1024*1024);
            std::fprintf(stderr,"note: 100k-entry feed: %d games in %lld ms, peak working set %lld MiB (fixture body included)\n",
                         int(feedGames.size()),qint64(bigMs),qint64(peakMb));
            check(bigOk && feedGames.size()==100000 && fixtureError.isEmpty(),"a 100k-entry (~50 MB) source feed opens with every entry");
            check(sourceFeedRequests==bigFeedBefore+1 && sourcesListRequests==bigSourcesBefore && searchRequests==bigSearchBefore,
                  "opening a 100k-entry feed costs one feed GET and no per-game directory/search fan-out");
            check(bigMs<60000,"a 100k-entry feed parses in bounded time");
            check(peakMb==0 || peakMb<3072,"a 100k-entry feed stays within a bounded memory footprint");
            feedGames.clear();
        }
        GameResult sourceOnly;sourceOnly.name=QStringLiteral("Fixture Search Game");
        GameSourceOption feedStandard;feedStandard.source=QStringLiteral("Fixture Feed");feedStandard.sourceUrl=QStringLiteral("https://fixture/feed.json");
        feedStandard.releaseTitle=QStringLiteral("Fixture Search Game Standard");feedStandard.fileSize=QStringLiteral("3 GB");
        feedStandard.sourceStatus=QStringLiteral("Only this source is known; other sources unavailable");
        feedStandard.downloads={normalizeDownload(QStringLiteral("https://fixture/std.zip"))};
        GameSourceOption feedDeluxe;feedDeluxe.source=QStringLiteral("Fixture Feed");feedDeluxe.sourceUrl=QStringLiteral("https://fixture/feed.json");
        feedDeluxe.releaseTitle=QStringLiteral("Fixture Search Game Deluxe");feedDeluxe.fileSize=QStringLiteral("4 GB");
        feedDeluxe.sourceStatus=QStringLiteral("Only this source is known; other sources unavailable");
        feedDeluxe.downloads={normalizeDownload(QStringLiteral("https://fixture/dlx.zip"))};
        sourceOnly.options={feedStandard,feedDeluxe};
        const int resolveSourcesBefore=sourcesListRequests;
        GameResult resolved;QString resolveError;
        check(HydraLibrary().resolveGameChoices(sourceOnly,&resolved,&resolveError) && resolveError.isEmpty(),
              "resolving a selected game's choices succeeds");
        check(resolved.options.size()==4,"the union keeps both feed editions plus both cross-source choices");
        check(resolved.options[0].source==QStringLiteral("Fixture Feed") && resolved.options[0].releaseTitle==QStringLiteral("Fixture Search Game Standard"),
              "known feed options stay first in order");
        check(resolved.options[1].source==QStringLiteral("Fixture Feed") && resolved.options[1].releaseTitle==QStringLiteral("Fixture Search Game Deluxe"),
              "same-source editions stay separate and ordered");
        check(resolved.options[2].source==QStringLiteral("Fixture Source A") && resolved.options[3].source==QStringLiteral("Fixture Source B"),
              "cross-source choices follow in catalogue order");
        bool allCleared=true;for(const auto& option:resolved.options)if(!option.sourceStatus.isEmpty())allCleared=false;
        check(allCleared,"multi-source status is cleared after a successful union");
        check(sourcesListRequests==resolveSourcesBefore+1,
              "one selected-game resolution costs exactly one directory fetch (one search)");
        GameResult noMatch;noMatch.name=QStringLiteral("Not In Catalogue");
        GameSourceOption only;only.source=QStringLiteral("Fixture Feed");only.sourceUrl=QStringLiteral("https://fixture/feed.json");
        only.releaseTitle=QStringLiteral("Not In Catalogue Edition");only.fileSize=QStringLiteral("5 GB");
        only.sourceStatus=QStringLiteral("Only this source is known; no supported direct or magnet download is available");
        noMatch.options={only};
        GameResult noMatchResolved;QString noMatchError;
        check(HydraLibrary().resolveGameChoices(noMatch,&noMatchResolved,&noMatchError) && noMatchError.isEmpty(),
              "a successful no-match resolves to the source-only game without error");
        check(noMatchResolved.options.size()==1 && noMatchResolved.options[0].sourceStatus.startsWith(QStringLiteral("Only this source is known")),
              "source-only messaging is preserved on a successful no-match");
        qputenv("XMB_HYDRA_SEARCH_URL",(base+QStringLiteral("/search-malformed")).toUtf8());
        GameResult failResolved;QString failError;
        check(!HydraLibrary().resolveGameChoices(sourceOnly,&failResolved,&failError) && failResolved.options.isEmpty() && !failError.isEmpty(),
              "a failed search reports an error instead of a source-only success");
        qputenv("XMB_HYDRA_SEARCH_URL",(base+QStringLiteral("/search")).toUtf8());
        qunsetenv("XMB_HYDRA_TEST_FIXTURE");qunsetenv("XMB_HYDRA_HOT_URL");qunsetenv("XMB_HYDRA_GAMES_URL");
        qunsetenv("XMB_HYDRA_SOURCES_URL");qunsetenv("XMB_HYDRA_SEARCH_URL");fixture.close();
    }
    {
        // FlareSolverr PRIMARY for source feeds (Hydra layer). The mock solver on /v1 (FlareSolverr's
        // own POST endpoint) returns the real JSON, and the direct GETs serve payloads distinct from the
        // solver's, so a feed that came from the solver can never be confused with one from the direct
        // endpoint. Proves: a configured endpoint makes browseSource() call the solver FIRST and return
        // its result without also hitting the direct endpoint; a solver failure falls back to the direct
        // fetch; an empty endpoint never contacts the solver. The catalogue API paths keep their
        // last-resort fallback (listSources and popularTitles below). Loopback only; no real FlareSolverr.
        QTcpServer solverFixture;
        check(solverFixture.listen(QHostAddress::LocalHost,0),"FlareSolverr fixture listen");
        int solverPosts=0;
        int primaryDirectGets=0,fallbackDirectGets=0,directOnlyGets=0;
        const QByteArray directoryJson=QByteArrayLiteral("{\"total\":1,\"limit\":100,\"sources\":[{\"title\":\"Solver Source\",\"url\":\"https://solver.invalid/feed.json\"}]}");
        const QByteArray feedJson=QByteArrayLiteral("[{\"title\":\"Solver Game\",\"size\":\"1 GB\",\"uri\":\"https://solver.invalid/game.zip\"}]");
        const QByteArray primaryDirectJson=QByteArrayLiteral("[{\"title\":\"Direct Primary Game\",\"size\":\"1 GB\",\"uri\":\"https://solver.invalid/primary.zip\"}]");
        const QByteArray fallbackDirectJson=QByteArrayLiteral("[{\"title\":\"Fallback Direct Game\",\"size\":\"1 GB\",\"uri\":\"https://solver.invalid/fallback.zip\"}]");
        const QByteArray directOnlyJson=QByteArrayLiteral("[{\"title\":\"Direct Only Game\",\"size\":\"1 GB\",\"uri\":\"https://solver.invalid/direct.zip\"}]");
        const QByteArray hotJson=QByteArrayLiteral("[{\"title\":\"Solver Hot Game\"}]");
        const auto respond=[&](QTcpSocket* socket,const QByteArray& status,const QByteArray& contentType,const QByteArray& payload) {
            socket->write("HTTP/1.1 "+status+"\r\nContent-Type: "+contentType+"\r\nContent-Length: "+QByteArray::number(payload.size())+"\r\nConnection: close\r\n\r\n"+payload);
            socket->disconnectFromHost();
        };
        QObject::connect(&solverFixture,&QTcpServer::newConnection,&app,[&] {
            auto* socket=solverFixture.nextPendingConnection();
            auto buffer=QSharedPointer<QByteArray>::create();
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket,buffer] {
                buffer->append(socket->readAll());
                const int headerEnd=buffer->indexOf("\r\n\r\n");
                if(headerEnd<0)return;
                const QByteArray headers=buffer->left(headerEnd);
                if(buffer->startsWith("POST /v1")) {
                    // FlareSolverr::request POSTs {"cmd":"request.get","url":...}; only answer once the
                    // whole body is present (loopback may split it across readyRead emissions).
                    const QByteArray body=buffer->mid(headerEnd+4);
                    int contentLength=0;
                    const int clAt=headers.indexOf("Content-Length:");
                    if(clAt>=0) {
                        const int start=clAt+15;
                        const int end=headers.indexOf("\r\n",start);
                        contentLength=headers.mid(start,(end<0?headers.size():end)-start).trimmed().toInt();
                    }
                    if(contentLength>0 && body.size()<contentLength)return;
                    ++solverPosts;
                    const QString target=QJsonDocument::fromJson(body).object().value(QStringLiteral("url")).toString();
                    // /feed-fallback asks the solver to fail (a down/broken solver) so the caller must
                    // fall back to the direct fetch.
                    if(target.contains(QStringLiteral("/feed-fallback"))) {
                        respond(socket,"200 OK","application/json",QJsonDocument(QJsonObject{
                            {QStringLiteral("status"),QStringLiteral("error")},
                            {QStringLiteral("message"),QStringLiteral("solver down")}}).toJson(QJsonDocument::Compact));
                        return;
                    }
                    QByteArray solved;
                    if(target.contains(QStringLiteral("/sources-challenge")))solved=directoryJson;
                    else if(target.contains(QStringLiteral("/feed-primary")))solved=feedJson;
                    else if(target.contains(QStringLiteral("/hot-challenge")))solved=hotJson;
                    else solved=QByteArrayLiteral("[]");
                    const QByteArray payload=QJsonDocument(QJsonObject{
                        {QStringLiteral("status"),QStringLiteral("ok")},
                        {QStringLiteral("solution"),QJsonObject{{QStringLiteral("response"),QString::fromUtf8(solved)}}}
                    }).toJson(QJsonDocument::Compact);
                    respond(socket,"200 OK","application/json",payload);
                    return;
                }
                if(headers.contains("/sources-challenge")) { respond(socket,"403 Forbidden","text/plain",{}); return; }
                if(headers.contains("/feed-primary")) { ++primaryDirectGets; respond(socket,"200 OK","application/json",primaryDirectJson); return; }
                if(headers.contains("/feed-fallback")) { ++fallbackDirectGets; respond(socket,"200 OK","application/json",fallbackDirectJson); return; }
                if(headers.contains("/feed-direct")) { ++directOnlyGets; respond(socket,"200 OK","application/json",directOnlyJson); return; }
                if(headers.contains("/hot-challenge")) { respond(socket,"200 OK","text/html",QByteArrayLiteral("<!doctype html><html><body>challenge</body></html>")); return; }
                respond(socket,"404 Not Found","text/plain",{});
            });
            QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        });
        const QString base=QStringLiteral("http://127.0.0.1:%1").arg(solverFixture.serverPort());
        const QString solverEndpoint=base;   // FlareSolverr::request POSTs <endpoint>/v1
        qputenv("XMB_HYDRA_TEST_FIXTURE","1");
        qputenv("XMB_HYDRA_SOURCES_URL",(base+QStringLiteral("/sources-challenge")).toUtf8());
        qputenv("XMB_HYDRA_HOT_URL",(base+QStringLiteral("/hot-challenge")).toUtf8());

        // Catalogue API keeps the last-resort fallback (unchanged).
        // 1. No endpoint -> the direct 403 is terminal and FlareSolverr is never contacted.
        solverPosts=0;
        QVector<HydraSource> noSolverSources;QString noSolverError;
        check(!HydraLibrary().listSources(&noSolverSources,&noSolverError) && noSolverSources.isEmpty() &&
              noSolverError.contains(QStringLiteral("HTTP 403")) && noSolverError.contains(QStringLiteral("source directory")),
              "no FlareSolverr endpoint: a challenged directory fails with the unchanged 403 error");
        check(solverPosts==0,"no FlareSolverr endpoint: the solver is never contacted");

        // 2. Configured endpoint -> the challenged directory is solved once and parsed (fallback).
        solverPosts=0;
        QVector<HydraSource> solvedSources;QString solvedError;
        check(HydraLibrary(solverEndpoint).listSources(&solvedSources,&solvedError) && solvedSources.size()==1 &&
              solvedSources.first().title==QStringLiteral("Solver Source") && solvedError.isEmpty(),
              "configured FlareSolverr solves a challenged directory and the source list is parsed");
        check(solverPosts==1,"a challenged directory falls back to FlareSolverr exactly once");

        // 3. The popular hot catalogue path is covered: an HTML challenge is solved once and parsed.
        solverPosts=0;
        QVector<PopularTitle> solvedTitles;QString hotError;
        check(HydraLibrary(solverEndpoint).popularTitles(&solvedTitles,&hotError) && solvedTitles.size()==1 &&
              solvedTitles.first().title==QStringLiteral("Solver Hot Game") && hotError.isEmpty(),
              "configured FlareSolverr solves an HTML-challenged hot catalogue and the title is parsed");
        check(solverPosts==1,"an HTML-challenged hot catalogue falls back to FlareSolverr exactly once");

        // Source-feed PRIMARY semantics.
        // 4. With the endpoint configured, browseSource() calls the solver FIRST, returns its result,
        // and does NOT also hit the direct endpoint when the solver succeeds.
        solverPosts=0;primaryDirectGets=0;
        HydraSource primaryFeed;primaryFeed.title=QStringLiteral("Primary Feed");primaryFeed.url=QUrl(base+QStringLiteral("/feed-primary"));
        QVector<GameResult> primaryGames;QString primaryError;
        check(HydraLibrary(solverEndpoint).browseSource(primaryFeed,&primaryGames,&primaryError) && primaryGames.size()==1 &&
              primaryGames.first().name==QStringLiteral("Solver Game") && primaryError.isEmpty(),
              "configured FlareSolverr fetches a source feed through the solver first and parses its result");
        check(solverPosts==1,"a configured source feed calls the solver exactly once (first)");
        check(primaryDirectGets==0,"a solver success does not also hit the direct endpoint");

        // 5. A solver failure falls back to the direct fetch.
        solverPosts=0;fallbackDirectGets=0;
        HydraSource fallbackFeed;fallbackFeed.title=QStringLiteral("Fallback Feed");fallbackFeed.url=QUrl(base+QStringLiteral("/feed-fallback"));
        QVector<GameResult> fallbackGames;QString fallbackError;
        check(HydraLibrary(solverEndpoint).browseSource(fallbackFeed,&fallbackGames,&fallbackError) && fallbackGames.size()==1 &&
              fallbackGames.first().name==QStringLiteral("Fallback Direct Game") && fallbackError.isEmpty(),
              "a solver failure falls back to the direct source feed fetch and parses it");
        check(solverPosts==1 && fallbackDirectGets==1,"a solver failure calls the solver once then falls back to one direct fetch");

        // 6. With no endpoint the solver is never contacted and the direct fetch is used unchanged.
        solverPosts=0;directOnlyGets=0;
        HydraSource directFeed;directFeed.title=QStringLiteral("Direct Feed");directFeed.url=QUrl(base+QStringLiteral("/feed-direct"));
        QVector<GameResult> directGames;QString directError;
        check(HydraLibrary().browseSource(directFeed,&directGames,&directError) && directGames.size()==1 &&
              directGames.first().name==QStringLiteral("Direct Only Game") && directError.isEmpty(),
              "with no endpoint a source feed is fetched directly and parsed");
        check(solverPosts==0 && directOnlyGets==1,"an empty endpoint never contacts the solver (direct fetch only)");

        // 7. ReleaseSources::fetchSourceJson is also PRIMARY (exercised through importSource): with the
        // endpoint configured, a source import fetches through the solver first and never hits direct.
        QTemporaryDir releaseTemp;check(releaseTemp.isValid(),"release temp dir for solver-primary import");
        Preferences solverPrefs(releaseTemp.path());
        check(solverPrefs.setValue(QStringLiteral("debrid/flareSolverrUrl"),solverEndpoint),"solver URL preference accepted");
        ReleaseSources solverStore(releaseTemp.path(),solverPrefs);
        solverPosts=0;primaryDirectGets=0;
        QString importError;
        check(solverStore.importSource(QUrl(base+QStringLiteral("/feed-primary")),&importError) && importError.isEmpty(),
              "configured FlareSolverr imports a source feed through the solver first");
        check(solverPosts==1 && primaryDirectGets==0,"a source import calls the solver first and does not hit the direct endpoint");

        qunsetenv("XMB_HYDRA_TEST_FIXTURE");qunsetenv("XMB_HYDRA_SOURCES_URL");qunsetenv("XMB_HYDRA_HOT_URL");
        solverFixture.close();
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
