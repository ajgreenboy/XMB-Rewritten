#pragma once

#include <QString>
#include <QVector>

namespace xmb {

struct PendingInstaller {
    QString id;
    QString title;
    QString installerPath;
    QString archivePath;
    QString destination;
    QString libraryRoot;
    QString sha256;
    QString status = QStringLiteral("pending");
    QString error;
    qint64 helperPid = 0;
    QString helperCreationTime;
    QVector<qint64> retainedProcessIds;
    QStringList retainedProcessCreationTimes;
};

// Only paths owned by the download job may be accepted.  The inspection is
// deliberately conservative: one supported setup candidate is required;
// auxiliary executables never become a game registration.
bool inspectPendingInstaller(const QString& extractedRoot, QString* installerPath,
                             QString* error = nullptr);
bool sha256File(const QString& path, QString* hash, QString* error = nullptr);
bool validatePendingInstallerRecord(PendingInstaller& entry, QString* error = nullptr);
// Canonicalizes an installation destination whose leaf may not exist yet. The
// parent must already exist and canonicalize (resolving junctions/symlinks) to
// the canonical library root itself (the direct-child layout the launcher
// enforces), so a reparse escape is rejected instead of being masked by a
// lexical prefix. Returns the canonical destination or an empty string.
QString canonicalInstallDestination(const QString& libraryRoot, const QString& destination, QString* error = nullptr);
// Stages an extracted pending-installer folder into its final
// Pending Installers/<id>/ location: writes a recovery journal (one retry) and
// renames the folder. Returns an empty string on success, or a translated error
// message. Test hooks XMB_PENDING_STAGING_JOURNAL_FAIL and
// XMB_PENDING_STAGING_RENAME_FAIL force each stage to fail. On failure the
// staging folder is left in place so the caller decides whether to keep it.
QString stagePendingInstallerFolder(const QString& stagingPath, const QString& pendingRoot, const QByteArray& recoveryBytes);
// Reports whether a pending-installer record or an orphaned recovery manifest
// exists. Only the caller-supplied library roots are scanned for recovery
// manifests (never all drives), so the routine UI refresh cannot block on a
// disconnected network drive.
bool pendingInstallerArtifactsAvailable(const QString& registryPath, const QStringList& libraryRoots);
bool verifyAndRegisterPendingInstaller(const QString& registryPath, const QString& appsPath,
                                       const PendingInstaller& entry, QString* error = nullptr,
                                       const QString& selectedExecutable = {});

class PendingInstallerRegistry {
public:
    static bool load(const QString& path, QVector<PendingInstaller>& entries, QString& error);
    static bool upsert(const QString& path, const PendingInstaller& entry, QString& error);
    static bool remove(const QString& path, const QString& id, QString& error);
};

} // namespace xmb
