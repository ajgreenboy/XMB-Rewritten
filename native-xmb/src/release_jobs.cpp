#include "shell.h"
#include "release_job_state.h"
#include "release_archive_cleanup.h"
#include "archive_extract.h"
#include "content_registry.h"
#include "pending_installers.h"
#include "diagnostics.h"
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFuture>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QUuid>
#include <QtConcurrentRun>
#include <algorithm>
#include <atomic>
#include <mutex>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dpapi.h>
#pragma comment(lib, "crypt32.lib")
#endif

namespace xmb {
namespace {
constexpr qsizetype MaxReleaseJobs = 32;
const QString& JobPrefix = xmb::releaseJobRowPrefix();

using State = xmb::ReleaseJobState;
struct Request {
    QString title;
    QString releaseTitle;
    QUrl sourceUrl;
    QString sourceSlug;
    QString gamesDirectory;
    QVector<Download> candidates;
    QString volume;
};

struct Result {
    QString archive;
    QString error;
    QString relativeExecutable;
    std::shared_ptr<QTemporaryDir> folder;
    QString pendingInstaller;
};

struct Job {
    QString id;
    Request request;
    State state = State::Queued;
    int progress = 0;
    Result result;
    QString archiveForResume;
};

bool canPause(State state) {
    return xmb::releaseJobCanPause(state);
}

bool canCancel(State state) {
    return xmb::releaseJobCanCancel(state);
}

bool active(State state) {
    return xmb::releaseJobActive(state);
}

QString gameArchiveDirectory(const Request& request,const QString& id) {
    return QDir(request.gamesDirectory).filePath(QStringLiteral("Download Archives/") + id);
}

constexpr qint64 MaxQueueBytes=2*1024*1024;
const QString InstallReceipt=QStringLiteral(".xmb-release-install.json");

QString queuePath(const QString& data) {
    return QDir(data).filePath(QStringLiteral("release-download-queue.bin"));
}

QString installedDirectory(const Job& job) {
    return QDir(job.request.gamesDirectory).filePath(QStringLiteral("game-")+job.id);
}
QString pendingInstallerDirectory(const Job& job) {
    return QDir(job.request.gamesDirectory).filePath(QStringLiteral("Pending Installers/")+job.id);
}
QString pendingRegistryPath(const QString& data) {
    return QDir(data).filePath(QStringLiteral("pending-installers.json"));
}

QString finalWindowsPath(const QString& path) {
#ifdef Q_OS_WIN
    const auto native=QDir::toNativeSeparators(path).toStdWString();
    const HANDLE directory=CreateFileW(native.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                       nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
    if(directory==INVALID_HANDLE_VALUE)return {};
    wchar_t resolved[32768];
    const DWORD count=GetFinalPathNameByHandleW(directory,resolved,DWORD(std::size(resolved)),FILE_NAME_NORMALIZED);
    CloseHandle(directory);
    if(count==0 || count>=std::size(resolved))return {};
    const QString value=QDir::fromNativeSeparators(QString::fromWCharArray(resolved,int(count)));
    return value.endsWith(QLatin1Char('/'))?value:value+QLatin1Char('/');
#else
    Q_UNUSED(path)
    return {};
#endif
}

// Match both the backing volume and the drive's resolved mount path. A reused
// drive letter or redirected library must never receive another drive's job.
QString libraryVolume(const QString& path) {
#ifdef Q_OS_WIN
    const QString clean=QDir::cleanPath(QDir::fromNativeSeparators(path));
    if(clean.size()<3 || !clean[0].isLetter() || clean.mid(1,2)!=QLatin1String(":/"))return {};
    const QString root=clean.left(3);
    const auto nativeRoot=QDir::toNativeSeparators(root).toStdWString();
    DWORD serial=0;
    if(!GetVolumeInformationW(nativeRoot.c_str(),nullptr,0,&serial,nullptr,nullptr,nullptr,0))return {};
    const QString mount=finalWindowsPath(root);
    if(mount.isEmpty())return {};
    // Check the deepest existing ancestor before creating any missing children.
    QString existing=clean;
    while(existing!=root && !QFileInfo::exists(existing))
        existing=QFileInfo(existing).absolutePath();
    const QString expected=mount+(existing==root?QString():existing.mid(3)+QLatin1Char('/'));
    if(finalWindowsPath(existing).compare(expected,Qt::CaseInsensitive)!=0)return {};
    return mount.toCaseFolded()+QLatin1Char('#')+QString::number(serial,16);
#else
    Q_UNUSED(path)
    return {};
#endif
}

bool destinationReady(const Request& request,const QString& path) {
    return !request.volume.isEmpty() && libraryVolume(path)==request.volume;
}

QByteArray protectQueue(const QByteArray& input,bool encrypt) {
#ifdef Q_OS_WIN
    if(input.isEmpty() || input.size()>MaxQueueBytes)return {};
    QByteArray entropy=QByteArrayLiteral("xmb-release-download-queue-v1");
    DATA_BLOB source{DWORD(input.size()),reinterpret_cast<BYTE*>(const_cast<char*>(input.constData()))};
    DATA_BLOB additional{DWORD(entropy.size()),reinterpret_cast<BYTE*>(entropy.data())};
    DATA_BLOB output{};
    const BOOL ok=encrypt
        ? CryptProtectData(&source,L"XMB download queue",&additional,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output)
        : CryptUnprotectData(&source,nullptr,&additional,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output);
    if(!ok)return {};
    const QByteArray result(reinterpret_cast<const char*>(output.pbData),output.cbData);
    SecureZeroMemory(output.pbData,output.cbData);
    LocalFree(output.pbData);
    return result;
#else
    Q_UNUSED(input)
    Q_UNUSED(encrypt)
    return {};
#endif
}

bool saveAtomic(const QString& path,const QByteArray& bytes) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size() && file.commit();
}

QJsonObject storedJob(const Job& job) {
    QJsonArray links;
    for(const auto& candidate:job.request.candidates)links.append(candidate.uri);
    return {{"id",job.id},{"state",int(job.state)},{"title",job.request.title},
            {"release",job.request.releaseTitle},{"source",job.request.sourceUrl.toString(QUrl::FullyEncoded)},
            {"slug",job.request.sourceSlug},{"directory",job.request.gamesDirectory},{"volume",job.request.volume},
            {"links",links},{"error",job.result.error.left(1024)}};
}

Job recoveredJob(const QJsonObject& entry) {
    Job job;
    static const QRegularExpression idFormat(QStringLiteral("^[a-f0-9]{32}$"));
    job.id=entry.value("id").toString();
    if(!idFormat.match(job.id).hasMatch())return {};
    auto& request=job.request;
    request.title=entry.value("title").toString();
    request.releaseTitle=entry.value("release").toString();
    request.sourceUrl=QUrl(entry.value("source").toString(),QUrl::StrictMode);
    request.sourceSlug=entry.value("slug").toString();
    request.gamesDirectory=entry.value("directory").toString();
    request.volume=entry.value("volume").toString();
    for(const auto item:entry.value("links").toArray()) {
        const QString link=item.toString();
        if(link.isEmpty() || link.size()>32768 || request.candidates.size()>=32)return {};
        request.candidates.append(normalizeDownload(link));
    }
    if(request.title.isEmpty() || request.releaseTitle.isEmpty() || request.volume.isEmpty() ||
       !QDir::isAbsolutePath(request.gamesDirectory) ||
       (request.sourceUrl.isEmpty() && request.sourceSlug.isEmpty() && request.candidates.isEmpty()))return {};
    const int state=entry.value("state").toInt(-1);
    if(state<0 || state>int(State::Cancelled))return {};
    job.state=State(state);
    job.result.error=entry.value("error").toString().left(1024);
    return job;
}

QString completedArchive(const Job& job) {
    const QString archiveDir=gameArchiveDirectory(job.request,job.id);
    if(!destinationReady(job.request,archiveDir))return {};
    const auto files=QDir(archiveDir).entryInfoList(QDir::Files|QDir::NoSymLinks);
    if(files.size()!=1 || files.first().size()<=0 || files.first().fileName().startsWith(QLatin1Char('.')))return {};
    return files.first().absoluteFilePath();
}

bool registerDownloadedGame(const QString& data,const Job& job,const QString& executable,QString& error) {
    const QDir installed(installedDirectory(job));
    const QFileInfo file(installed.filePath(executable));
    const QString canonical=QDir(installed.canonicalPath()).absolutePath();
    if(executable.isEmpty() || QDir::isAbsolutePath(executable) || !file.isFile() || file.isSymLink() ||
       file.suffix().compare(QLatin1String("exe"),Qt::CaseInsensitive)!=0 || installed.canonicalPath().isEmpty() ||
       !file.canonicalFilePath().startsWith(canonical+QLatin1Char('/'),Qt::CaseInsensitive)) {
        error=QObject::tr("Could not verify the installed game executable.");
        return false;
    }
    ContentApp app;
    app.id=QStringLiteral("release-game-")+job.id;
    app.title=job.request.title;
    app.kind=app.category=QStringLiteral("game");
    app.executable=file.absoluteFilePath();
    app.workingDirectory=file.absolutePath();
    return ContentRegistry::upsert(QDir(data).filePath(QStringLiteral("apps.json")),app,error);
}

// This receipt moves atomically with the staged folder. If the app exits before
// updating apps.json, it can finish that transaction without extracting again.
QString readInstallReceipt(const Job& job) {
    QFile receipt(QDir(installedDirectory(job)).filePath(InstallReceipt));
    if(QFileInfo(receipt).isSymLink() || !receipt.open(QIODevice::ReadOnly) || receipt.size()>65536)return {};
    const auto data=QJsonDocument::fromJson(receipt.readAll()).object();
    return data.value("id").toString()==job.id?data.value("executable").toString():QString{};
}

QString statusText(const Job& job) {
    switch(job.state) {
    case State::Queued: return QObject::tr("Queued");
    case State::Paused: return QObject::tr("Paused");
    case State::Resolving: return QObject::tr("Connecting to source");
    case State::Downloading:
        return job.progress > 0 ? QObject::tr("Downloading %1%").arg(job.progress) : QObject::tr("Downloading");
    case State::Pausing: return QObject::tr("Pausing");
    case State::Extracting: return QObject::tr("Preparing game files");
    case State::Cancelling: return QObject::tr("Cancelling");
    case State::Finished: return QObject::tr("Installed");
    case State::Failed: return QObject::tr("Failed");
    case State::Cancelled: return QObject::tr("Cancelled");
    }
    return {};
}

Entry jobRow(const QString& id,const QString& title,const QString& command) {
    Entry row;
    row.id = id;
    row.title = title;
    row.action = Action::ReleaseGameJob;
    row.value = command;
    row.texture = QStringLiteral("game_tex_default_ps3");
    row.container = QStringLiteral("explore_plugin_full");
    return row;
}

QVector<Entry> jobActions(const Job& job) {
    const QString prefix = JobPrefix + job.id + ':';
    Entry status = jobRow(prefix + QStringLiteral("status"),statusText(job),QString());
    status.action = Action::Unavailable;
    status.detail = job.result.error;
    QVector<Entry> rows{status};
    const auto add = [&](const QString& command,const QString& title) {
        rows.append(jobRow(prefix + command,title,command));
    };
    if(canPause(job.state))add(QStringLiteral("pause"),QObject::tr("Pause"));
    if(job.state==State::Paused)add(QStringLiteral("resume"),QObject::tr("Resume"));
    if(canCancel(job.state))add(QStringLiteral("cancel"),QObject::tr("Cancel Download"));
    // Only a failed job lingers for a manual decision (Retry or Dismiss). Finished and Cancelled jobs are
    // auto-forgotten once finalized, so they never reach their detail page.
    if(job.state==State::Failed)add(QStringLiteral("retry"),QObject::tr("Retry Download"));
    if(!job.result.archive.isEmpty())add(QStringLiteral("folder"),QObject::tr("Open Download Folder"));
    if(job.state==State::Failed)add(QStringLiteral("dismiss"),QObject::tr("Dismiss"));
    return rows;
}

#ifdef Q_OS_WIN
bool runWindowsTar(const QString& tar,const QStringList& arguments,const std::atomic_bool& cancel,
                   QByteArray* output,QString& error) {
    QProcess process;
    auto environment=QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("LC_ALL"),QStringLiteral("C"));
    process.setProcessEnvironment(environment);
    process.start(tar,arguments);
    if(!process.waitForStarted(5000)) {
        error=QObject::tr("Could not start the Windows archive extractor.");
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    while(process.state()!=QProcess::NotRunning) {
        process.waitForFinished(100);
        if(output)output->append(process.readAllStandardOutput());
        if(cancel.load() || timer.elapsed()>=30*60*1000 || (output && output->size()>16*1024*1024)) {
            process.kill();
            process.waitForFinished(5000);
            error=cancel.load()?QObject::tr("Download cancelled."):QObject::tr("Archive extraction exceeded its time or listing limit.");
            return false;
        }
    }
    if(output)output->append(process.readAllStandardOutput());
    if(process.exitStatus()!=QProcess::NormalExit || process.exitCode()!=0 || (output && output->size()>16*1024*1024)) {
        const auto detail=QString::fromLocal8Bit(process.readAllStandardError()).trimmed().left(240);
        error=QObject::tr("The Windows archive extractor could not read this file. %1").arg(detail);
        return false;
    }
    return true;
}

bool extractWindowsArchive(const QString& archive,const QString& directory,const std::atomic_bool& cancel,QString& error) {
    const QString system=qEnvironmentVariable("SystemRoot");
    const QString tar=QDir(system).filePath(QStringLiteral("System32/tar.exe"));
    if(system.isEmpty() || !QFileInfo(tar).isFile()) {
        error=QObject::tr("This version of Windows is missing its built-in archive extractor. The archive is still saved in Download Archives.");
        return false;
    }
    const QString file=QFileInfo(archive).absoluteFilePath();
    QByteArray paths,details;
    if(!runWindowsTar(tar,{QStringLiteral("-tf"),file},cancel,&paths,error) ||
       !runWindowsTar(tar,{QStringLiteral("-tvf"),file},cancel,&details,error))return false;
    auto names=QString::fromUtf8(paths).split(QLatin1Char('\n'),Qt::SkipEmptyParts);
    auto entries=QString::fromUtf8(details).split(QLatin1Char('\n'),Qt::SkipEmptyParts);
    static const QRegularExpression invalidPart(QStringLiteral(R"([<>:"|?*\\\x00-\x1f]|[ .]$|^(?:\.{1,2}|CON|PRN|AUX|NUL|COM[1-9\x{b9}\x{b2}\x{b3}]|LPT[1-9\x{b9}\x{b2}\x{b3}]) *(?:\.|$))"),QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression listing(QStringLiteral(R"(^[-d]\S{9}\s+\d+\s+\S+\s+\S+\s+(\d+)\s)"));
    QSet<QString> used;
    constexpr qint64 maxBytes=256LL*1024*1024*1024;
    qint64 total=0;
    if(names.isEmpty() || names.size()!=entries.size() || names.size()>100000) {
        error=QObject::tr("The archive listing is incomplete or contains too many entries.");
        return false;
    }
    for(int i=0;i<names.size();++i) {
        QString path=names.at(i);
        if(path.endsWith(QLatin1Char('\r')))path.chop(1);
        if(path.endsWith(QLatin1Char('/')))path.chop(1);
        const auto fields=listing.match(entries.at(i));
        bool sizeValid=false;
        const qint64 size=fields.captured(1).toLongLong(&sizeValid);
        if(!sizeValid || size<0 || size>maxBytes-total || entries.at(i).contains(QStringLiteral(" link to "))) {
            error=QObject::tr("The archive contains a link, special file, or oversized entry.");
            return false;
        }
        total+=size;
        for(const auto& part:path.split(QLatin1Char('/')))if(part.isEmpty() || invalidPart.match(part).hasMatch()) {
            error=QObject::tr("The archive contains an unsafe file path.");
            return false;
        }
        const QString key=path.toCaseFolded();
        if(used.contains(key)) {
            error=QObject::tr("The archive contains duplicate file paths.");
            return false;
        }
        used.insert(key);
    }
    const QStorageInfo disk(directory);
    if(disk.isValid() && disk.isReady() && total>disk.bytesAvailable()) {
        error=QObject::tr("There is not enough space to extract this archive.");
        return false;
    }
    return runWindowsTar(tar,{QStringLiteral("--safe-writes"),QStringLiteral("--no-same-owner"),QStringLiteral("--no-same-permissions"),
        QStringLiteral("-xkf"),file,QStringLiteral("-C"),directory},cancel,nullptr,error);
}
#endif


bool extractGameArchive(const QString& archive,const QString& directory,const std::atomic_bool& cancel,QString& error) {
    QFile file(archive);
    if(!file.open(QIODevice::ReadOnly)) {
        error = QObject::tr("Could not read the downloaded archive.");
        return false;
    }
    const QByteArray signature=file.read(8);
    file.close();
#ifdef Q_OS_WIN
    if(signature.startsWith(QByteArray::fromHex("377abcaf271c")) || signature.startsWith(QByteArray::fromHex("526172211a07")))
        return extractWindowsArchive(archive,directory,cancel,error);
    if(!signature.startsWith(QByteArray("PK\x03\x04",4))) {
        error=QObject::tr("Automatic installation supports ZIP, 7Z and RAR archives. Open the download folder to inspect the file.");
        return false;
    }
    // Use the system ZIP library. Copy validated regular entries to a private directory, with a
    // per-entry byte count so mismatched metadata cannot fill the drive.
    const QString script = QStringLiteral(R"PS(
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$root = [IO.Path]::GetFullPath($env:XMB_RELEASE_FOLDER).TrimEnd('\') + '\'
$zip = [IO.Compression.ZipFile]::OpenRead($env:XMB_RELEASE_ARCHIVE)
try {
    if ($zip.Entries.Count -gt 100000) { throw 'The archive contains too many files.' }
    $total = [long]0
    $limit = [long](256GB)
    foreach ($entry in $zip.Entries) {
        $name = $entry.FullName.Replace('\','/')
        $isDirectory = $name.EndsWith('/')
        if ($isDirectory) { $name = $name.Substring(0, $name.Length - 1) }
        foreach ($part in $name.Split('/')) {
            if (!$part -or $part -match '(^\.{1,2}$|[<>:"|?*\x00-\x1f]|[ .]$|^(CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³]) *(?:\.|$))') {
                throw 'The archive contains an unsafe path.'
            }
        }
        $mode = ($entry.ExternalAttributes -shr 16) -band 61440
        if (($mode -ne 0 -and $mode -ne 16384 -and $mode -ne 32768) -or ($entry.ExternalAttributes -band 1024)) {
            throw 'The archive contains a link or special file.'
        }
        $path = [IO.Path]::GetFullPath([IO.Path]::Combine($root, $name))
        if (!$path.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'The archive contains an unsafe path.'
        }
        if ($isDirectory) { [IO.Directory]::CreateDirectory($path) > $null; continue }
        if ($entry.Length -lt 0 -or $entry.Length -gt ($limit - $total)) { throw 'The archive is too large.' }
        $total += $entry.Length
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) > $null
        $inputFile = $entry.Open()
        try {
            $output = [IO.File]::Open($path, [IO.FileMode]::CreateNew)
            try {
                $buffer = [byte[]]::new(65536)
                $remaining = $entry.Length
                while (($read = $inputFile.Read($buffer, 0, $buffer.Length)) -gt 0) {
                    $remaining -= $read
                    if ($remaining -lt 0) { throw 'The ZIP entry exceeded its declared size.' }
                    $output.Write($buffer, 0, $read)
                }
                if ($remaining -ne 0) { throw 'The ZIP entry is incomplete.' }
            } finally { $output.Dispose() }
        } finally { $inputFile.Dispose() }
    }
} finally { $zip.Dispose() }
)PS");
    QProcess process;
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("XMB_RELEASE_ARCHIVE"),QFileInfo(archive).absoluteFilePath());
    environment.insert(QStringLiteral("XMB_RELEASE_FOLDER"),directory);
    process.setProcessEnvironment(environment);
    const QString systemDirectory = environment.value(QStringLiteral("SystemRoot"));
    if(systemDirectory.isEmpty()) {
        error = QObject::tr("Windows system directory is unavailable.");
        return false;
    }
    const QString powershell = QDir(systemDirectory).filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe"));
    // -EncodedCommand avoids Windows command-line parsing of quotation marks in the fixed script.
    QByteArray encoded;
    for(const QChar character:script) {
        encoded.append(char(character.unicode() & 0xff));
        encoded.append(char(character.unicode() >> 8));
    }
    process.start(powershell,{QStringLiteral("-NoProfile"),QStringLiteral("-NonInteractive"),
                            QStringLiteral("-EncodedCommand"),QString::fromLatin1(encoded.toBase64())});
    if(!process.waitForStarted(5000)) {
        error = QObject::tr("Could not start the Windows ZIP extractor.");
        return false;
    }
    QElapsedTimer timer;
    timer.start();
    while(process.state()!=QProcess::NotRunning) {
        if(cancel.load() || timer.elapsed() >= 30*60*1000) {
            process.kill();
            process.waitForFinished(5000);
            error = cancel.load() ? QObject::tr("Download cancelled.") : QObject::tr("ZIP extraction timed out.");
            return false;
        }
        process.waitForFinished(100);
    }
    if(process.exitStatus()!=QProcess::NormalExit || process.exitCode()!=0) {
        const QString detail = QString::fromLocal8Bit(process.readAllStandardError()).section('\n',0,0).trimmed().left(300);
        error = QObject::tr("Could not safely extract the ZIP. %1").arg(detail);
        return false;
    }
    return true;
#else
    Q_UNUSED(directory)
    Q_UNUSED(cancel)
    error = QObject::tr("Automatic ZIP installation is available on Windows. Open the download folder to inspect the file.");
    return false;
#endif
}

Result prepareGame(const QString& data,const Request& request,const QString& id,
                   const QString& archiveForResume,JobControl& control,QPromise<Result>& promise) {
    Result result;
    if(!destinationReady(request,gameArchiveDirectory(request,id))) {
        result.error=QObject::tr("Reconnect the original game library drive and retry.");
        return result;
    }
    const QDir library(request.gamesDirectory);
    promise.setProgressRange(0,1002);
    if(QFileInfo(archiveForResume).isFile()) {
        result.archive = archiveForResume;
    } else {
        Preferences preferences(data);
        ReleaseSources sources(data,preferences);
        const auto progress = [&promise](qint64 received,qint64 total) {
            if(received <= 0 && total <= 0)return;
            const double fraction = total > 0 ? std::clamp(double(received)/double(total),0.0,1.0) : 0.0;
            promise.setProgressValue(1 + int(fraction*1000));
        };
        result.archive = sources.downloadGame(request.sourceSlug,request.sourceUrl,request.releaseTitle,
                                             request.candidates,gameArchiveDirectory(request,id),&result.error,
                                             progress,&control.cancel,&control.pausing);
    }
    if(result.archive.isEmpty() || !control.beginExtraction())return result;
    if(!destinationReady(request,result.archive)) {
        result.error=QObject::tr("Reconnect the original game library drive and retry.");
        return result;
    }
    promise.setProgressValue(1002);
    auto stage = std::make_shared<QTemporaryDir>(library.filePath(QStringLiteral(".xmb-install-XXXXXX")));
    if(!stage->isValid()) {
        result.error = QObject::tr("Could not create a staging folder in the selected game library.");
        return result;
    }
    if(!extractGameArchive(result.archive,stage->path(),control.cancel,result.error) || control.cancel.load())return result;
    QString installer;
    QString installerError;
    if (inspectPendingInstaller(stage->path(),&installer,&installerError)) {
        result.pendingInstaller = QDir(stage->path()).relativeFilePath(installer);
        result.folder = std::move(stage);
        return result;
    }
    // Multiple installer candidates are never silently treated as a portable
    // game.  A single setup candidate is handled above; this branch only
    // rejects an ambiguous installer archive.
    if (installerError.contains(QStringLiteral("More than one installer candidate"))) {
        result.error = installerError;
        return result;
    }
    result.error.clear();
    const QString executable = findGameExecutable(stage->path(),&result.error);
    if(executable.isEmpty())return result;
    result.relativeExecutable = QDir(stage->path()).relativeFilePath(executable);
    const QString receipt=QDir(stage->path()).filePath(InstallReceipt);
    const auto bytes=QJsonDocument(QJsonObject{{"id",id},{"executable",result.relativeExecutable}})
        .toJson(QJsonDocument::Compact);
    if(QFileInfo::exists(receipt) || !saveAtomic(receipt,bytes)) {
        result.error=QObject::tr("Could not write the game installation receipt.");
        return result;
    }
    result.folder = std::move(stage);
    return result;
}
}

struct ReleaseGameQueue {
    QVector<Job> jobs;
    std::shared_ptr<JobControl> control;
    QFuture<Result> future;
    bool shuttingDown=false;
    bool storageBlocked=false;
};

bool Shell::saveReleaseJobs() const {
    if(!releaseJobs_ || releaseJobs_->storageBlocked)return false;
    QJsonArray records;
    for(const auto& job:releaseJobs_->jobs)records.append(storedJob(job));
    auto clear=QJsonDocument(QJsonObject{{"version",1},{"jobs",records}}).toJson(QJsonDocument::Compact);
    const QByteArray protectedBytes=protectQueue(clear,true);
    clear.fill('\0');
    if(protectedBytes.isEmpty() || protectedBytes.size()>MaxQueueBytes || !QDir().mkpath(dataDirectory_) ||
       !saveAtomic(queuePath(dataDirectory_),protectedBytes)) {
        qWarning("Could not save the XMB download queue");
        return false;
    }
    return true;
}

void Shell::restoreReleaseJobs() {
    if(releaseJobs_ || diagnostics::safeMode())return;
    releaseJobs_=std::make_shared<ReleaseGameQueue>();
    QFile file(queuePath(dataDirectory_));
    if(!file.exists())return;
    if(!file.open(QIODevice::ReadOnly) || file.size()<=0 || file.size()>MaxQueueBytes) {
        releaseJobs_->storageBlocked=true;
        qWarning("Could not read the XMB download queue");
        return;
    }
    auto clear=protectQueue(file.read(MaxQueueBytes+1),false);
    file.close();
    const auto saved=QJsonDocument::fromJson(clear).object();
    clear.fill('\0');
    if(saved.value("version").toInt()!=1 || !saved.value("jobs").isArray() ||
       saved.value("jobs").toArray().size()>MaxReleaseJobs) {
        releaseJobs_->storageBlocked=true;
        qWarning("Could not decrypt or validate the XMB download queue");
        return;
    }
    QSet<QString> ids;
    for(const auto value:saved.value("jobs").toArray()) {
        auto job=recoveredJob(value.toObject());
        if(job.id.isEmpty() || ids.contains(job.id)) {
            releaseJobs_->storageBlocked=true;
            releaseJobs_->jobs.clear();
            qWarning("Invalid job in the XMB download queue");
            return;
        }
        ids.insert(job.id);
        releaseJobs_->jobs.append(std::move(job));
    }
    bool installedAny=false;
    for(auto& job:releaseJobs_->jobs) {
        job.result.archive=completedArchive(job);
        if(job.state==State::Cancelling) {
            job.state=State::Cancelled;
            // The worker was interrupted (shutdown or crash) before its finished handler ran, so the
            // cancellation never got its cleanup pass. It is now definitive: discard the orphan
            // rather than leave it behind. A pause/failure/queued job keeps its archive below. The
            // delete is gated on the drive still being the job's own volume, so a reused drive letter
            // or a redirected library never receives another drive's cleanup.
            if(destinationReady(job.request,gameArchiveDirectory(job.request,job.id)) &&
               releaseJobDiscardArchive(job.request.gamesDirectory,job.id,job.result.archive))
                job.result.archive.clear();
        }
        const bool runnable=job.state==State::Queued || job.state==State::Resolving ||
            job.state==State::Downloading || job.state==State::Extracting;
        if((runnable || job.state==State::Failed) &&
           destinationReady(job.request,installedDirectory(job)) && QFileInfo::exists(installedDirectory(job))) {
            if(registerDownloadedGame(dataDirectory_,job,readInstallReceipt(job),job.result.error)) {
                job.state=State::Finished;
                job.result.error.clear();
                installedAny=true;
            } else job.state=State::Failed;
        } else if(job.state==State::Pausing) {
            job.state=State::Paused;
        } else if(runnable) {
            job.state=State::Queued;
            job.result.error.clear();
            job.archiveForResume=job.result.archive;
        } else if(job.state==State::Paused) {
            job.archiveForResume=job.result.archive;
        }
    }
    // Finished (registered above) and Cancelled (cleaned up above) jobs are final: forget them so a
    // restored queue does not re-list completed work. Failed and paused jobs stay for retry/resume.
    releaseJobs_->jobs.erase(std::remove_if(releaseJobs_->jobs.begin(),releaseJobs_->jobs.end(),
        [](const Job& job){ return xmb::releaseJobComplete(job.state); }), releaseJobs_->jobs.end());
    if(!saveReleaseJobs())releaseJobs_->storageBlocked=true;
    if(installedAny)refreshContent();
    refreshReleaseJobRows();
    pumpReleaseJobs();
}

bool Shell::hasPausableReleaseJobs() const {
    if(!releaseJobs_ || releaseJobs_->storageBlocked)return false;
    return std::any_of(releaseJobs_->jobs.cbegin(),releaseJobs_->jobs.cend(),[](const Job& job){return canPause(job.state);});
}

bool Shell::hasPausedReleaseJobs() const {
    if(!releaseJobs_ || releaseJobs_->storageBlocked || diagnostics::safeMode())return false;
    return std::any_of(releaseJobs_->jobs.cbegin(),releaseJobs_->jobs.cend(),[](const Job& job){return job.state==State::Paused;});
}

void Shell::pauseAllReleaseJobs() {
    if(!hasPausableReleaseJobs())return;
    for(auto& job:releaseJobs_->jobs) {
        if(job.state==State::Queued)job.state=State::Paused;
        else if(canPause(job.state) && releaseJobs_->control && releaseJobs_->control->requestPause())
            job.state=State::Pausing;
    }
    if(!saveReleaseJobs())notify(tr("Download Management"),tr("Could not save the paused queue."));
    refreshReleaseJobRows();
}

void Shell::resumeAllReleaseJobs() {
    if(!hasPausedReleaseJobs())return;
    QVector<Job*> resumed;
    for(auto& job:releaseJobs_->jobs) {
        if(job.state!=State::Paused)continue;
        resumed.append(&job);
        job.state=State::Queued;
    }
    if(!saveReleaseJobs()) {
        for(auto* job:resumed)job->state=State::Paused;
        notify(tr("Download Management"),tr("Could not save the resumed queue."));
        return;
    }
    refreshReleaseJobRows();
    pumpReleaseJobs();
}

QVector<Entry> Shell::releaseJobRows() const {
    QVector<Entry> rows;
    if(!releaseJobs_)return rows;
    for(const auto& job:releaseJobs_->jobs) {
        Entry row = jobRow(JobPrefix + job.id,job.request.title,QStringLiteral("details"));
        row.container=QStringLiteral("explore_category_network");
        row.texture=QStringLiteral("tex_dl_dst_game");
        row.detail = statusText(job);
        row.setting = job.request.title + "\n\n" + row.detail;
        if(!job.result.error.isEmpty())row.setting += "\n" + job.result.error;
        row.setting += "\n" + QDir::toNativeSeparators(job.request.gamesDirectory);
        rows.append(std::move(row));
    }
    return rows;
}

void Shell::addReleaseDownloadOptions(const Entry& entry) {
    if(!releaseJobs_)return;
    const QString id=entry.id.mid(JobPrefix.size()).section(':',0,0);
    const auto& jobs=releaseJobs_->jobs;
    const auto job=std::find_if(jobs.cbegin(),jobs.cend(),[&](const Job& value){return value.id==id;});
    if(job==jobs.cend())return;
    if(canPause(job->state))
        addOption({"",tr("Pause")},OptionCommand::ReleaseDownloadPause,"download-job");
    if(job->state==State::Paused)
        addOption({"",tr("Resume")},OptionCommand::ReleaseDownloadResume,"download-job");
    if(canCancel(job->state))
        addOption({"",tr("Cancel")},OptionCommand::ReleaseDownloadCancel,"download-job");
    if(job->state==State::Failed)
        addOption({"",tr("Retry")},OptionCommand::ReleaseDownloadRetry,"download-job");
}

void Shell::refreshReleaseJobRows() {
    for(auto& page:pages_) {
        const QString focus = page.entries.value(page.selection).id;
        if(page.downloads) {
            page.entries.removeIf([](const Entry& row) { return row.id.startsWith(JobPrefix); });
            page.entries = releaseJobRows() + page.entries;
        } else {
            const QString statusId = page.entries.value(0).id;
            if(!statusId.startsWith(JobPrefix) || !statusId.endsWith(QLatin1String(":status")))continue;
            const QString id = statusId.mid(JobPrefix.size()).section(':',0,0);
            const auto& jobs = releaseJobs_->jobs;
            const auto job = std::find_if(jobs.cbegin(),jobs.cend(),[&](const Job& job) { return job.id==id; });
            if(job==jobs.cend())continue;
            page.entries = jobActions(*job);
        }
        for(int i=0;i<page.entries.size();++i) {
            if(page.entries[i].id==focus){page.selection=i;break;}
        }
        page.selection = std::clamp(page.selection,0,std::max(0,int(page.entries.size())-1));
    }
    emit changed();
}

bool Shell::enqueueReleaseGame(const QString& title,const QString& releaseTitle,const QUrl& sourceUrl,
                              const QString& sourceSlug,const QString& gamesDirectory,const QVector<Download>& candidates) {
    if(diagnostics::safeMode()) {
        showMessage(tr("Downloads"),tr("Safe mode does not use the network."),MessageKind::Page);
        emit changed();
        return false;
    }
    if(title.trimmed().isEmpty() || releaseTitle.trimmed().isEmpty() || !QDir::isAbsolutePath(gamesDirectory) ||
       (sourceSlug.isEmpty() && sourceUrl.isEmpty() && candidates.isEmpty())) {
        showMessage(tr("Downloads"),tr("Choose a release and an absolute game library folder."),MessageKind::Page);
        emit changed();
        return false;
    }
    if(!releaseJobs_)releaseJobs_ = std::make_shared<ReleaseGameQueue>();
    const QString library = QDir::cleanPath(gamesDirectory);
    const QString volume=libraryVolume(library);
    if(volume.isEmpty() || releaseJobs_->storageBlocked) {
        showMessage(tr("Downloads"),volume.isEmpty()?tr("Choose an available game library drive.")
            :tr("The saved download queue is unavailable."),MessageKind::Page);
        emit changed();
        return false;
    }
    for(const auto& job:releaseJobs_->jobs) {
        if(job.request.releaseTitle==releaseTitle && job.request.sourceSlug==sourceSlug && job.request.sourceUrl==sourceUrl &&
           job.request.gamesDirectory==library && (job.state==State::Queued || job.state==State::Paused || active(job.state))) {
            showMessage(tr("Downloads"),tr("This release already has a job in Download Management."),MessageKind::Page);
            emit changed();
            return false;
        }
    }
    if(releaseJobs_->jobs.size()>=MaxReleaseJobs) {
        showMessage(tr("Downloads"),tr("Download Management is full. Dismiss a finished job and try again."),MessageKind::Page);
        emit changed();
        return false;
    }
    Job job;
    job.id = QUuid::createUuid().toString(QUuid::Id128);
    job.request = {title,releaseTitle,sourceUrl,sourceSlug,library,candidates,volume};
    releaseJobs_->jobs.prepend(std::move(job));
    if(!saveReleaseJobs()) {
        releaseJobs_->jobs.removeFirst();
        showMessage(tr("Downloads"),tr("Could not save the download queue."),MessageKind::Page);
        emit changed();
        return false;
    }
    notify(tr("Download Management"),tr("%1 queued").arg(title));
    refreshReleaseJobRows();
    pumpReleaseJobs();
    // A successful enqueue opens Download Management so the new job is visible immediately. Reuse the
    // existing page path; if Download Management is already the top page (a defensive case), refresh it in
    // place instead of stacking a duplicate page.
    if(!pages_.isEmpty() && pages_.last().downloads)reloadSteamDownloads();
    else openSteamDownloads();
    return true;
}

void Shell::pumpReleaseJobs() {
    if(!releaseJobs_ || releaseJobs_->control || releaseJobs_->shuttingDown || releaseJobs_->storageBlocked || diagnostics::safeMode())return;
    auto& jobs = releaseJobs_->jobs;
    const auto next = std::find_if(jobs.rbegin(),jobs.rend(),[](const Job& job) { return job.state==State::Queued; });
    if(next==jobs.rend())return;
    next->state = State::Resolving;
    if(!saveReleaseJobs()) {
        next->state=State::Queued;
        notify(tr("Download Management"),tr("Could not save the download queue."));
        return;
    }
    const QString id = next->id;
    const Request request = next->request;
    const QString archiveForResume = next->archiveForResume;
    const auto control = std::make_shared<JobControl>();
    releaseJobs_->control = control;
    auto* watcher = new QFutureWatcher<Result>(this);
    connect(watcher,&QFutureWatcher<Result>::progressValueChanged,this,[this,id](int value) {
        auto& jobs = releaseJobs_->jobs;
        const auto job = std::find_if(jobs.begin(),jobs.end(),[&](const Job& job) { return job.id==id; });
        if(job==jobs.end() || (job->state!=State::Resolving && job->state!=State::Downloading) || value<=0)return;
        const State stage = value==1002 ? State::Extracting : State::Downloading;
        const int percent = std::min((value-1)/10,100);
        if(job->state==stage && job->progress==percent)return;
        job->state = stage;
        job->progress = percent;
        refreshReleaseJobRows();
    });
    connect(watcher,&QFutureWatcher<Result>::finished,this,[this,id,control,watcher] {
        Result result = watcher->result();
        watcher->deleteLater();
        releaseJobs_->control.reset();
        releaseJobs_->future = QFuture<Result>();
        if(releaseJobs_->shuttingDown)return;
        auto& jobs = releaseJobs_->jobs;
        const auto job = std::find_if(jobs.begin(),jobs.end(),[&](const Job& job) { return job.id==id; });
        if(job==jobs.end()) {
            pumpReleaseJobs();
            return;
        }
        const bool paused = control->pausing.load();
        const bool cancelled = control->cancel.load();
        job->state = xmb::releaseJobSettledState(paused, cancelled);
        job->archiveForResume = paused ? result.archive : QString();
        if(!cancelled && result.folder) {
            if(!result.pendingInstaller.isEmpty()) {
                const QString pendingRoot=pendingInstallerDirectory(*job);
                QDir().mkpath(QFileInfo(pendingRoot).absolutePath());
                if(!destinationReady(job->request,pendingRoot) || !QDir().rename(result.folder->path(),pendingRoot)) {
                    result.error=tr("Could not preserve the pending installer files in the selected library.");
                } else {
                    result.folder->setAutoRemove(false);
                    const QString installer=QDir(pendingRoot).filePath(result.pendingInstaller);
                    const QString recovery=QDir(pendingRoot).filePath(QStringLiteral(".xmb-pending-recovery.json"));
                    saveAtomic(recovery,QJsonDocument(QJsonObject{{"id",job->id},{"title",job->request.title},{"installer",installer},{"archive",result.archive},{"libraryRoot",job->request.gamesDirectory}}).toJson(QJsonDocument::Compact));
                    QString hash;
                    PendingInstaller pending;
                    pending.id=job->id; pending.title=job->request.title; pending.archivePath=result.archive;
                    pending.installerPath=installer; pending.destination.clear(); pending.libraryRoot=job->request.gamesDirectory;
                    if(!sha256File(installer,&hash,&result.error)) {
                        result.error=tr("Could not verify the pending installer: %1").arg(result.error);
                    } else {
                        pending.sha256=hash;
                        QString registryError;
                        if(!PendingInstallerRegistry::upsert(pendingRegistryPath(dataDirectory_),pending,registryError))
                            result.error=registryError;
                        else {
                            QFile::remove(recovery);
                            job->state=State::Finished;
                            result.error.clear();
                            notify(tr("Pending Installers"),tr("%1 is ready to install manually.").arg(job->request.title));
                        }
                    }
                }
            } else {
            const QString installed=installedDirectory(*job);
            if(!destinationReady(job->request,installed) || !QDir().rename(result.folder->path(),installed)) {
                result.error=tr("Could not move the game files into the selected library.");
            } else {
                result.folder->setAutoRemove(false);
                if(registerDownloadedGame(dataDirectory_,*job,result.relativeExecutable,result.error)) {
                    job->state=State::Finished;
                    refreshContent();
                    fetchGameArtwork(QStringLiteral("release-game-")+id,job->request.title);
                } else result.error+="\n"+tr("Game files are still in %1.").arg(QDir::toNativeSeparators(installed));
            }
            }
        }
        if(paused || cancelled)result.error.clear();
        if(job->state==State::Failed && result.error.isEmpty())result.error = tr("Could not prepare the game.");
        job->result.archive = result.archive.isEmpty() ? job->result.archive : result.archive;
        // Definitive cancellation only (a cancel with no standing pause): the worker has finished
        // and the completed archive is no longer resumable, so discard it rather than leave an
        // orphan in Download Archives/<id>/. A pause or failure keeps the archive for resume/retry.
        if(cancelled && !paused &&
           releaseJobDiscardArchive(job->request.gamesDirectory,id,result.archive))
            job->result.archive.clear();
        job->result.error = result.error;
        // A finished (installed + registered) or cancelled (archive cleaned up) job is complete: forget it
        // so it no longer occupies a queue slot or lingers in Download Management (the console clears both
        // from the list). Capture the row text first, then forget; failed and paused jobs stay for
        // retry/resume. A finished job's downloaded archive is NOT discarded here (unlike the definitive
        // cancellation above): it is the user's downloaded file and remains in Download Archives/<id>/,
        // reachable from the OS rather than from Download Management, exactly as the previous manual Dismiss
        // left it.
        const bool complete = xmb::releaseJobComplete(job->state);
        const QString title = job->request.title;
        const QString state = statusText(*job);
        if(complete)forgetReleaseJob(id);
        if(!saveReleaseJobs())notify(tr("Download Management"),tr("Could not save the download queue."));
        notify(tr("Download Management"),tr("%1 · %2").arg(title,state));
        refreshReleaseJobRows();
        pumpReleaseJobs();
    });
    watcher->setFuture(QtConcurrent::run([data=dataDirectory_,request,id,archiveForResume,control](QPromise<Result>& promise) {
        promise.addResult(prepareGame(data,request,id,archiveForResume,*control,promise));
    }));
    releaseJobs_->future = watcher->future();
    refreshReleaseJobRows();
}

void Shell::stopReleaseJobs() {
    if(!releaseJobs_ || releaseJobs_->shuttingDown)return;
    releaseJobs_->shuttingDown=true;
    if(releaseJobs_->control)releaseJobs_->control->requestShutdown();
    // Defensively join the in-flight worker before the process tears the global thread pool down: the
    // transfer is cancelled (or paused) here, and waiting on its future makes app.quit() exit
    // deterministically instead of leaving the worker to wind down against QThreadPool's waitForDone()
    // at static destruction. The worker is self-contained (its download loop runs its own nested event
    // loop and QNetworkAccessManager in the worker thread), and its cancel checks bound the wind-down to
    // at most a few seconds (debrid polling and archive extraction check less often than the download
    // loop), so the join cannot deadlock the GUI thread.
    if(releaseJobs_->future.isRunning())releaseJobs_->future.waitForFinished();
}

void Shell::activateReleaseJob(const Entry& entry) {
    if(!releaseJobs_)return;
    const QString id = entry.id.mid(JobPrefix.size()).section(':',0,0);
    auto& jobs = releaseJobs_->jobs;
    const auto job = std::find_if(jobs.begin(),jobs.end(),[&](const Job& job) { return job.id==id; });
    if(job==jobs.end())return;
    const bool retryable = job->state==State::Failed;
    if(entry.value==QLatin1String("pause")) {
        if(!canPause(job->state))return;
        if(job->state==State::Queued) {
            job->state = State::Paused;
        } else {
            const auto& control = releaseJobs_->control;
            if(!control || !control->requestPause())return;
            job->state = State::Pausing;
        }
    } else if(entry.value==QLatin1String("resume")) {
        if(job->state!=State::Paused || diagnostics::safeMode())return;
        job->state = State::Queued;
    } else if(entry.value==QLatin1String("cancel")) {
        if(!cancelReleaseJob(id))return;   // the tail below persists, pops back and refreshes
    } else if(entry.value==QLatin1String("retry")) {
        if(!retryable || diagnostics::safeMode())return;
        if(destinationReady(job->request,installedDirectory(*job)) && QFileInfo::exists(installedDirectory(*job))) {
            if(!registerDownloadedGame(dataDirectory_,*job,readInstallReceipt(*job),job->result.error)) {
                notify(tr("Download Management"),job->result.error);
                refreshReleaseJobRows();
                return;
            }
            job->state=State::Finished;
            refreshContent();
            // Registration finalized: the retried install is complete, so forget it rather than leaving a
            // Finished row in Download Management. forgetReleaseJob() also drops the detail page, so the
            // shared tail below must not pop again (it sees the list, not the detail page, afterward).
            forgetReleaseJob(id);
        } else {
            job->state=State::Queued;
            job->archiveForResume.clear();
            job->result.error.clear();
            job->progress=0;
        }
    } else if(entry.value==QLatin1String("dismiss")) {
        if(job->state!=State::Failed)return;
        jobs.erase(job);
    } else if(entry.value==QLatin1String("folder")) {
        if(job->result.archive.isEmpty())return;
        const QString folder = QFileInfo(job->result.archive).absolutePath();
        if(!QDesktopServices::openUrl(QUrl::fromLocalFile(folder))) {
            showMessage(tr("Downloads"),tr("Could not open the download folder."),MessageKind::Page);
        }
        emit changed();
        return;
    } else {
        if(pages_.size()>=32)return;
        Page page;
        page.title = job->request.title;
        page.entries = jobActions(*job);
        page.selection = page.entries.size()>1 ? 1 : 0;
        pages_.append(std::move(page));
        emit changed();
        return;
    }
    if(!saveReleaseJobs())notify(tr("Download Management"),tr("Could not save the download queue."));
    if(!pages_.isEmpty() && !pages_.last().downloads)pages_.removeLast();
    refreshReleaseJobRows();
    pumpReleaseJobs();
}

// The release job whose detail page is the top page, or empty. A release-job detail page is the one
// activateReleaseJob() pushes for a job's "details" row: its first row is the status row, id
// `download:release:<id>:status`. The Download Management list (page.downloads) and every other page
// return empty.
QString Shell::releaseJobDetailId() const {
    if(pages_.isEmpty() || pages_.last().downloads)return {};
    const auto& rows = pages_.last().entries;
    if(rows.isEmpty())return {};
    if(!rows.first().id.endsWith(QLatin1String(":status")))return {};
    return xmb::releaseJobIdFromRow(rows.first().id);
}

// The single source of a release job's cancellation state change. Both the detail page's Cancel row and
// the Triangle menu's Cancel (activateReleaseJob) go through here, so the mutation is written once.
// Back/Esc never reaches this: it only navigates and leaves the download running. Mutates only: callers
// own saveReleaseJobs / refreshReleaseJobRows / pumpReleaseJobs. Returns true when a cancellation was
// applied.
bool Shell::cancelReleaseJob(const QString& id) {
    if(!releaseJobs_)return false;
    auto& jobs = releaseJobs_->jobs;
    const auto job = std::find_if(jobs.begin(),jobs.end(),[&](const Job& value) { return value.id==id; });
    if(job==jobs.end() || !canCancel(job->state))return false;
    const State prior = job->state;
    if(prior==State::Queued || prior==State::Paused) {
        const QString archiveDir = gameArchiveDirectory(job->request,id);
        if(destinationReady(job->request,archiveDir))ReleaseSources::discardGamePartial(archiveDir);
        // A queued/paused cancel finalizes immediately: the partial is discarded above, so the job is
        // complete and clears Download Management at once (the console clears a cancelled download).
        // forgetReleaseJob() also drops the detail page, so the caller's shared tail must not pop again.
        forgetReleaseJob(id);
    } else if(releaseJobs_->control) {
        job->state = State::Cancelling;
        // requestCancel() also clears pausing, so a job cancelled during Pausing settles to Cancelled
        // rather than Paused. If the pause's wind-down already flushed its checkpoint first, the
        // archive (completed or partial) remains in Download Archives/<id>/ until the worker's
        // finished handler discards it as part of the definitive cancellation (see pumpReleaseJobs).
        releaseJobs_->control->requestCancel();
    } else {
        // Defensive: an active job with no in-flight control cannot be cancelled cleanly.
        return false;
    }
    return true;
}

// Forgets a release job whose work is complete (installed and registered, or cancelled and cleaned up).
// It removes the job from the queue so it no longer occupies a slot or lingers in Download Management,
// and drops the job's detail page if it is still open. It does not navigate otherwise and is safe to call
// while the Download Management list is the top page. Callers must finalize the job first (registration
// success for Finished, archive cleanup for Cancelled); see releaseJobComplete().
void Shell::forgetReleaseJob(const QString& id) {
    if(!releaseJobs_)return;
    auto& jobs = releaseJobs_->jobs;
    const auto job = std::find_if(jobs.begin(),jobs.end(),[&](const Job& value){ return value.id==id; });
    if(job==jobs.end())return;
    jobs.erase(job);
    if(releaseJobDetailId()==id)pages_.removeLast();
}
}
