#include "pending_installers.h"
#include "archive_extract.h"
#include "content_registry.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace xmb {
namespace {
constexpr int MaxEntries = 256;
bool write(const QString& path, const QVector<PendingInstaller>& entries, QString& error) {
    QJsonArray array;
    for (const auto& e : entries) array.append(QJsonObject{{"id",e.id},{"title",e.title},
        {"installer",e.installerPath},{"archive",e.archivePath},{"destination",e.destination},{"libraryRoot",e.libraryRoot},
        {"sha256",e.sha256},{"status",e.status},{"error",e.error.left(1024)},
        {"helperPid",double(e.helperPid)},{"helperCreationTime",e.helperCreationTime},
        {"retainedProcessIds",QJsonArray::fromVariantList([&]{QVariantList v;for(const auto pid:e.retainedProcessIds)v.append(pid);return v;}())},{"retainedProcessCreationTimes",QJsonArray::fromStringList(e.retainedProcessCreationTimes)}});
    QSaveFile file(path);
    if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !file.open(QIODevice::WriteOnly) ||
        file.write(QJsonDocument(QJsonObject{{"version",1},{"entries",array}}).toJson(QJsonDocument::Compact))<0 ||
        !file.commit()) { error = QStringLiteral("Could not save pending installers: %1").arg(file.errorString()); return false; }
    return true;
}
QString canonicalInside(const QString& root, const QString& path) {
    const QString base = QDir(root).canonicalPath();
    const QString file = QFileInfo(path).canonicalFilePath();
    if (base.isEmpty() || file.isEmpty() || file.compare(base, Qt::CaseInsensitive) == 0 ||
        !file.startsWith(base + QLatin1Char('/'), Qt::CaseInsensitive)) return {};
    return file;
}
bool installerName(const QFileInfo& info) {
    if (info.suffix().compare(QStringLiteral("exe"), Qt::CaseInsensitive) != 0) return false;
    const QString n = info.completeBaseName().toLower();
    return n == QStringLiteral("setup") || n == QStringLiteral("install") || n == QStringLiteral("installer");
}
}

bool inspectPendingInstaller(const QString& root, QString* installerPath, QString* error) {
    if (installerPath) installerPath->clear();
    const QString canonicalRoot = QDir(root).canonicalPath();
    if (canonicalRoot.isEmpty()) { if (error) *error = QStringLiteral("Installer staging folder is unavailable."); return false; }
    QVector<QString> candidates;
    QList<QDir> dirs{QDir(canonicalRoot)};
    while (!dirs.isEmpty()) {
        const QDir dir = dirs.takeFirst();
        for (const auto& info : dir.entryInfoList(QDir::Files|QDir::Dirs|QDir::NoDotAndDotDot|QDir::NoSymLinks,QDir::Name)) {
            if (info.isDir()) { dirs.append(QDir(info.absoluteFilePath())); continue; }
            if (installerName(info)) candidates.append(info.absoluteFilePath());
        }
    }
    if (candidates.size() != 1) {
        if (error) *error = candidates.isEmpty() ? QStringLiteral("No supported installer was found in the download.")
                                                   : QStringLiteral("More than one installer candidate was found; choose one explicitly.");
        return false;
    }
    const QString verified = canonicalInside(canonicalRoot, candidates.first());
    if (verified.isEmpty()) { if (error) *error = QStringLiteral("Installer candidate escaped its download folder."); return false; }
    if (installerPath) *installerPath = verified;
    return true;
}

bool sha256File(const QString& path, QString* hash, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return false; }
    QCryptographicHash digest(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(1024 * 1024);
        if (chunk.isEmpty() && !file.atEnd()) { if (error) *error = file.errorString(); return false; }
        digest.addData(chunk);
    }
    if (hash) *hash = QString::fromLatin1(digest.result().toHex());
    return true;
}

bool validatePendingInstallerRecord(PendingInstaller& e, QString* error) {
    static const QRegularExpression hex(QStringLiteral("^[0-9a-fA-F]{64}$"));
    const auto fail=[&](const QString& message){if(error)*error=message;return false;};
    if(e.id.isEmpty() || e.title.isEmpty() || !QDir::isAbsolutePath(e.installerPath) ||
       !QDir::isAbsolutePath(e.archivePath) || !QDir::isAbsolutePath(e.libraryRoot) ||
       !hex.match(e.sha256).hasMatch()) return fail(QStringLiteral("Pending installer record has invalid identity, roots, or hash."));
    const QString pendingRoot=QDir(QFileInfo(e.archivePath).absolutePath()).canonicalPath();
    const QString libraryRoot=QDir(e.libraryRoot).canonicalPath();
    const QString installer=QFileInfo(e.installerPath).canonicalFilePath();
    if(pendingRoot.isEmpty() || libraryRoot.isEmpty()) return fail(QStringLiteral("Pending installer roots are unavailable."));
    if(!installer.isEmpty() && !installer.startsWith(pendingRoot+QLatin1Char('/'),Qt::CaseInsensitive)) return fail(QStringLiteral("Installer is outside its job-owned pending folder."));
    if(!e.destination.isEmpty()) {
        if(!QDir::isAbsolutePath(e.destination)) return fail(QStringLiteral("Installation destination is not absolute."));
        const QString destination=QDir(e.destination).canonicalPath();
        if(destination.isEmpty() || destination==libraryRoot || !destination.startsWith(libraryRoot+QLatin1Char('/'),Qt::CaseInsensitive)) return fail(QStringLiteral("Installation destination is outside its library root."));
    }
    if(e.status==QStringLiteral("registered") && e.destination.isEmpty()) return fail(QStringLiteral("Registered installer has no verified destination."));
    if(e.status==QStringLiteral("launching") && e.helperPid<=0) return fail(QStringLiteral("Launching installer has no process identity."));
    if(installer.isEmpty() || !QFileInfo::exists(installer)) {
        if(e.status==QStringLiteral("pending") || e.status==QStringLiteral("launching")) e.status=QStringLiteral("unavailable");
        if(e.status!=QStringLiteral("unavailable") && e.status!=QStringLiteral("helper-failed") && e.status!=QStringLiteral("timeout")) return fail(QStringLiteral("Installer source is missing."));
    }
    return true;
}

bool PendingInstallerRegistry::load(const QString& path, QVector<PendingInstaller>& entries, QString& error) {
    entries.clear(); error.clear(); QFile file(path);
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) { error = QStringLiteral("Pending installer registry cannot be read."); return false; }
    const auto doc = QJsonDocument::fromJson(file.readAll());
    const auto root = doc.object();
    if (root.value("version").toInt() != 1 || !root.value("entries").isArray() || root.value("entries").toArray().size() > MaxEntries) { error = QStringLiteral("Pending installer registry is invalid."); return false; }
    QSet<QString> ids;
    for (const auto value : root.value("entries").toArray()) {
        const auto o = value.toObject(); PendingInstaller e;
        e.id=o.value("id").toString(); e.title=o.value("title").toString(); e.installerPath=o.value("installer").toString();
        e.archivePath=o.value("archive").toString(); e.destination=o.value("destination").toString(); e.libraryRoot=o.value("libraryRoot").toString(); e.sha256=o.value("sha256").toString();
        e.status=o.value("status").toString(); e.error=o.value("error").toString().left(1024); e.helperPid=qint64(o.value("helperPid").toDouble()); e.helperCreationTime=o.value("helperCreationTime").toString();
        for(const auto value:o.value("retainedProcessIds").toArray())e.retainedProcessIds.append(qint64(value.toDouble()));
        for(const auto value:o.value("retainedProcessCreationTimes").toArray())e.retainedProcessCreationTimes.append(value.toString());
        if(ids.contains(e.id) || (e.status!=QStringLiteral("pending") && e.status!=QStringLiteral("launching") && e.status!=QStringLiteral("launch-failed") && e.status!=QStringLiteral("helper-exited-needs-verification") && e.status!=QStringLiteral("helper-failed") && e.status!=QStringLiteral("timeout") && e.status!=QStringLiteral("process-still-running") && e.status!=QStringLiteral("no-window") && e.status!=QStringLiteral("nonzero") && e.status!=QStringLiteral("helper-failed-or-still-running") && e.status!=QStringLiteral("unavailable") && e.status!=QStringLiteral("registered")) || !validatePendingInstallerRecord(e,&error)) { if(error.isEmpty())error=QStringLiteral("Pending installer entry is invalid."); entries.clear(); return false; }
        ids.insert(e.id); entries.append(e);
    }
    return true;
}
bool PendingInstallerRegistry::upsert(const QString& path, const PendingInstaller& entry, QString& error) {
    if(qEnvironmentVariableIsSet("XMB_PENDING_REGISTRY_FAIL_WRITE")) { error=QStringLiteral("Injected pending registry write failure."); return false; }
    PendingInstaller validated=entry; if(!validatePendingInstallerRecord(validated,&error))return false;
    QVector<PendingInstaller> entries; if (!load(path,entries,error)) return false;
    auto it=std::find_if(entries.begin(),entries.end(),[&](const auto& e){return e.id==validated.id;});
    if (it==entries.end()) entries.append(validated); else *it=validated;
    if (entries.size()>MaxEntries) { error=QStringLiteral("Pending installer registry is full."); return false; }
    return write(path,entries,error);
}
bool PendingInstallerRegistry::remove(const QString& path, const QString& id, QString& error) {
    QVector<PendingInstaller> entries; if (!load(path,entries,error)) return false;
    const auto it=std::find_if(entries.begin(),entries.end(),[&](const auto& e){return e.id==id;});
    if (it==entries.end()) { error=QStringLiteral("Pending installer was not found."); return false; }
    entries.erase(it); return write(path,entries,error);
}

bool verifyAndRegisterPendingInstaller(const QString& registryPath, const QString& appsPath,
                                       const PendingInstaller& entry, QString* error, const QString& selectedExecutable) {
    const QString root=QDir(entry.libraryRoot).canonicalPath();
    const QString destination=QDir(entry.destination).canonicalPath();
    if(root.isEmpty() || destination.isEmpty() || destination==root || !destination.startsWith(root+QLatin1Char('/'),Qt::CaseInsensitive)) { if(error)*error=QStringLiteral("Installation destination is outside its recorded library root."); return false; }
    const QString archiveRoot=QFileInfo(entry.archivePath).absolutePath();
    const QString installer=QFileInfo(entry.installerPath).canonicalFilePath();
    if(entry.status!=QStringLiteral("registered")) {
        if(installer.isEmpty() || archiveRoot.isEmpty() || !installer.startsWith(QDir(archiveRoot).canonicalPath()+QLatin1Char('/'),Qt::CaseInsensitive)) { if(error)*error=QStringLiteral("Pending installer source is missing or no longer job-owned."); return false; }
        QString hash; if(!sha256File(installer,&hash,error) || hash.compare(entry.sha256,Qt::CaseInsensitive)!=0) { if(error && error->isEmpty())*error=QStringLiteral("Installer hash changed."); return false; }
    }
    const QStringList candidates=findGameExecutables(destination);
    QString chosen;
    if(selectedExecutable.isEmpty()) {
        if(candidates.size()!=1) { if(error)*error=candidates.isEmpty()?QStringLiteral("No game executable was found in the destination."):QStringLiteral("More than one plausible game executable was found; choose one explicitly."); return false; }
        chosen=candidates.first();
    } else {
        chosen=QFileInfo(selectedExecutable).canonicalFilePath();
        if(chosen.isEmpty() || !candidates.contains(chosen)) { if(error)*error=QStringLiteral("The selected executable is not a verified game candidate."); return false; }
    }
    ContentApp app; app.id=QStringLiteral("release-game-")+entry.id; app.title=entry.title; app.kind=app.category=QStringLiteral("game"); app.executable=chosen; app.workingDirectory=QFileInfo(app.executable).absolutePath();
    if(!ContentRegistry::upsert(appsPath,app,*error))return false;
    PendingInstaller updated=entry; updated.status=QStringLiteral("registered");
    QString ignored; return PendingInstallerRegistry::upsert(registryPath,updated,ignored);
}
} // namespace xmb
