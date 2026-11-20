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
    QString sha256;
    QString status = QStringLiteral("pending");
    QString error;
};

// Only paths owned by the download job may be accepted.  The inspection is
// deliberately conservative: one supported setup candidate is required;
// auxiliary executables never become a game registration.
bool inspectPendingInstaller(const QString& extractedRoot, QString* installerPath,
                             QString* error = nullptr);
bool sha256File(const QString& path, QString* hash, QString* error = nullptr);

class PendingInstallerRegistry {
public:
    static bool load(const QString& path, QVector<PendingInstaller>& entries, QString& error);
    static bool upsert(const QString& path, const PendingInstaller& entry, QString& error);
    static bool remove(const QString& path, const QString& id, QString& error);
};

} // namespace xmb
