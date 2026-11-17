#include "shell.h"
#include "archive_extract.h"
#include "content_registry.h"
#include "diagnostics.h"
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
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
const QString JobPrefix = QStringLiteral("download:release:");

enum class State { Queued, Paused, Resolving, Downloading, Pausing, Extracting, Cancelling, Finished, Failed, Cancelled };
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
};

struct Job {
    QString id;
    Request request;
    State state = State::Queued;
    int progress = 0;
    Result result;
    QString archiveForResume;
};

struct JobControl {
    std::atomic_bool cancel{false};
    std::atomic_bool pausing{false};

    bool requestPause() {
        std::lock_guard lock(extractionMutex);
        if(extracting)return false;
        pausing.store(true);
        cancel.store(true);
        return true;
    }

    void requestShutdown() {
        std::lock_guard lock(extractionMutex);
        if(!cancel.load()) {
            pausing.store(true);
            cancel.store(true);
        }
    }

    bool beginExtraction() {
        std::lock_guard lock(extractionMutex);
        if(cancel.load())return false;
        extracting = true;
        return true;
    }

private:
    std::mutex extractionMutex;
    bool extracting = false;
};

bool canPause(State state) {
    return state == State::Queued || state == State::Resolving || state == State::Downloading;
}

bool canCancel(State state) {
    return canPause(state) || state == State::Paused || state == State::Extracting;
}

bool active(State state) {
    return state == State::Resolving || state == State::Downloading || state == State::Pausing ||
           state == State::Extracting || state == State::Cancelling;
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
    const bool canRetry = job.state==State::Failed || job.state==State::Cancelled;
    if(canRetry)add(QStringLiteral("retry"),QObject::tr("Retry Download"));
    if(!job.result.archive.isEmpty())add(QStringLiteral("folder"),QObject::tr("Open Download Folder"));
    if(canRetry || job.state==State::Finished)add(QStringLiteral("dismiss"),QObject::tr("Dismiss"));
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
        if(job.state==State::Cancelling)job.state=State::Cancelled;
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
    if(job->state==State::Failed || job->state==State::Cancelled)
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

void Shell::enqueueReleaseGame(const QString& title,const QString& releaseTitle,const QUrl& sourceUrl,
                              const QString& sourceSlug,const QString& gamesDirectory,const QVector<Download>& candidates) {
    if(diagnostics::safeMode()) {
        showMessage(tr("Downloads"),tr("Safe mode does not use the network."),MessageKind::Page);
        emit changed();
        return;
    }
    if(title.trimmed().isEmpty() || releaseTitle.trimmed().isEmpty() || !QDir::isAbsolutePath(gamesDirectory) ||
       (sourceSlug.isEmpty() && sourceUrl.isEmpty() && candidates.isEmpty())) {
        showMessage(tr("Downloads"),tr("Choose a release and an absolute game library folder."),MessageKind::Page);
        emit changed();
        return;
    }
    if(!releaseJobs_)releaseJobs_ = std::make_shared<ReleaseGameQueue>();
    const QString library = QDir::cleanPath(gamesDirectory);
    const QString volume=libraryVolume(library);
    if(volume.isEmpty() || releaseJobs_->storageBlocked) {
        showMessage(tr("Downloads"),volume.isEmpty()?tr("Choose an available game library drive.")
            :tr("The saved download queue is unavailable."),MessageKind::Page);
        emit changed();
        return;
    }
    for(const auto& job:releaseJobs_->jobs) {
        if(job.request.releaseTitle==releaseTitle && job.request.sourceSlug==sourceSlug && job.request.sourceUrl==sourceUrl &&
           job.request.gamesDirectory==library && (job.state==State::Queued || job.state==State::Paused || active(job.state))) {
            showMessage(tr("Downloads"),tr("This release already has a job in Download Management."),MessageKind::Page);
            emit changed();
            return;
        }
    }
    if(releaseJobs_->jobs.size()>=MaxReleaseJobs) {
        showMessage(tr("Downloads"),tr("Download Management is full. Dismiss a finished job and try again."),MessageKind::Page);
        emit changed();
        return;
    }
    Job job;
    job.id = QUuid::createUuid().toString(QUuid::Id128);
    job.request = {title,releaseTitle,sourceUrl,sourceSlug,library,candidates,volume};
    releaseJobs_->jobs.prepend(std::move(job));
    if(!saveReleaseJobs()) {
        releaseJobs_->jobs.removeFirst();
        showMessage(tr("Downloads"),tr("Could not save the download queue."),MessageKind::Page);
        emit changed();
        return;
    }
    notify(tr("Download Management"),tr("%1 queued").arg(title));
    refreshReleaseJobRows();
    pumpReleaseJobs();
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
        if(releaseJobs_->shuttingDown)return;
        auto& jobs = releaseJobs_->jobs;
        const auto job = std::find_if(jobs.begin(),jobs.end(),[&](const Job& job) { return job.id==id; });
        if(job==jobs.end()) {
            pumpReleaseJobs();
            return;
        }
        const bool paused = control->pausing.load();
        const bool cancelled = control->cancel.load();
        job->state = paused ? State::Paused : cancelled ? State::Cancelled : State::Failed;
        job->archiveForResume = paused ? result.archive : QString();
        if(!cancelled && result.folder) {
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
        if(paused || cancelled)result.error.clear();
        if(job->state==State::Failed && result.error.isEmpty())result.error = tr("Could not prepare the game.");
        job->result.archive = result.archive.isEmpty() ? job->result.archive : result.archive;
        job->result.error = result.error;
        if(!saveReleaseJobs())notify(tr("Download Management"),tr("Could not save the download queue."));
        notify(tr("Download Management"),tr("%1 · %2").arg(job->request.title,statusText(*job)));
        refreshReleaseJobRows();
        pumpReleaseJobs();
    });
    watcher->setFuture(QtConcurrent::run([data=dataDirectory_,request,id,archiveForResume,control](QPromise<Result>& promise) {
        promise.addResult(prepareGame(data,request,id,archiveForResume,*control,promise));
    }));
    refreshReleaseJobRows();
}

void Shell::stopReleaseJobs() {
    if(!releaseJobs_ || releaseJobs_->shuttingDown)return;
    releaseJobs_->shuttingDown=true;
    if(releaseJobs_->control)releaseJobs_->control->requestShutdown();
}

void Shell::activateReleaseJob(const Entry& entry) {
    if(!releaseJobs_)return;
    const QString id = entry.id.mid(JobPrefix.size()).section(':',0,0);
    auto& jobs = releaseJobs_->jobs;
    const auto job = std::find_if(jobs.begin(),jobs.end(),[&](const Job& job) { return job.id==id; });
    if(job==jobs.end())return;
    const bool retryable = job->state==State::Failed || job->state==State::Cancelled;
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
        if(!canCancel(job->state))return;
        if(job->state==State::Queued || job->state==State::Paused) {
            const QString archiveDir=gameArchiveDirectory(job->request,id);
            if(destinationReady(job->request,archiveDir))ReleaseSources::discardGamePartial(archiveDir);
            job->archiveForResume.clear();
            job->state = State::Cancelled;
        } else if(releaseJobs_->control) {
            job->state = State::Cancelling;
            releaseJobs_->control->cancel.store(true);
        }
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
        } else {
            job->state=State::Queued;
            job->archiveForResume.clear();
        }
        job->result.error.clear();
        job->progress=0;
    } else if(entry.value==QLatin1String("dismiss")) {
        if(!retryable && job->state!=State::Finished)return;
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
}
