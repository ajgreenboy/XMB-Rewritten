#include "pending_installers.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>

namespace xmb {
namespace {
constexpr int MaxEntries = 256;
bool write(const QString& path, const QVector<PendingInstaller>& entries, QString& error) {
    QJsonArray array;
    for (const auto& e : entries) array.append(QJsonObject{{"id",e.id},{"title",e.title},
        {"installer",e.installerPath},{"archive",e.archivePath},{"destination",e.destination},
        {"sha256",e.sha256},{"status",e.status},{"error",e.error.left(1024)}});
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
    return n == QStringLiteral("setup") || n == QStringLiteral("install") ||
           n.contains(QStringLiteral("setup")) || n.contains(QStringLiteral("installer"));
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

bool PendingInstallerRegistry::load(const QString& path, QVector<PendingInstaller>& entries, QString& error) {
    entries.clear(); error.clear(); QFile file(path);
    if (!file.exists()) return true;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 1024 * 1024) { error = QStringLiteral("Pending installer registry cannot be read."); return false; }
    const auto doc = QJsonDocument::fromJson(file.readAll());
    const auto root = doc.object();
    if (root.value("version").toInt() != 1 || !root.value("entries").isArray() || root.value("entries").toArray().size() > MaxEntries) { error = QStringLiteral("Pending installer registry is invalid."); return false; }
    for (const auto value : root.value("entries").toArray()) {
        const auto o = value.toObject(); PendingInstaller e;
        e.id=o.value("id").toString(); e.title=o.value("title").toString(); e.installerPath=o.value("installer").toString();
        e.archivePath=o.value("archive").toString(); e.destination=o.value("destination").toString(); e.sha256=o.value("sha256").toString();
        e.status=o.value("status").toString(); e.error=o.value("error").toString().left(1024);
        if (e.id.isEmpty() || e.title.isEmpty() || e.installerPath.isEmpty() || e.archivePath.isEmpty() || e.sha256.size()!=64) { error=QStringLiteral("Pending installer entry is invalid."); entries.clear(); return false; }
        entries.append(e);
    }
    return true;
}
bool PendingInstallerRegistry::upsert(const QString& path, const PendingInstaller& entry, QString& error) {
    QVector<PendingInstaller> entries; if (!load(path,entries,error)) return false;
    auto it=std::find_if(entries.begin(),entries.end(),[&](const auto& e){return e.id==entry.id;});
    if (it==entries.end()) entries.append(entry); else *it=entry;
    if (entries.size()>MaxEntries) { error=QStringLiteral("Pending installer registry is full."); return false; }
    return write(path,entries,error);
}
bool PendingInstallerRegistry::remove(const QString& path, const QString& id, QString& error) {
    QVector<PendingInstaller> entries; if (!load(path,entries,error)) return false;
    const auto it=std::find_if(entries.begin(),entries.end(),[&](const auto& e){return e.id==id;});
    if (it==entries.end()) { error=QStringLiteral("Pending installer was not found."); return false; }
    entries.erase(it); return write(path,entries,error);
}
} // namespace xmb
