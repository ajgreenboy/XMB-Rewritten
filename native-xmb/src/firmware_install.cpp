#include "firmware_install.h"
#include "firmware_extract.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>

namespace xmb {
namespace {
FirmwareInstallResult failed(const QString& why) { return {FirmwareInstallOutcome::Failed, why}; }
}

bool firmwareDevFlashPresent(const QString& dir) {
    return !dir.isEmpty() && QFileInfo(dir + QStringLiteral("/vsh/resource/explore_plugin_full.rco")).isFile();
}

bool commitFirmwareDirectory(const QString& staging, const QString& dest, QString* error) {
    const auto fail = [&](const QString& text) { if(error) *error = text; return false; };
    if(!QFileInfo(staging).isDir()) return fail(QStringLiteral("Staging folder is missing"));
    const QString aside = dest + QStringLiteral(".old");
    if(QFileInfo::exists(aside) && !QDir(aside).removeRecursively()) return fail(QStringLiteral("Cannot clear %1").arg(aside));
    if(!QDir().mkpath(QFileInfo(dest).absolutePath())) return fail(QStringLiteral("Cannot create the parent folder of %1").arg(dest));
    const bool hadOld = QFileInfo::exists(dest);
    if(hadOld && !QDir().rename(dest, aside)) return fail(QStringLiteral("Cannot move the existing %1 aside").arg(dest));
    if(!QDir().rename(staging, dest)) {
        if(hadOld) QDir().rename(aside, dest);
        return fail(QStringLiteral("Cannot move the new firmware into %1").arg(dest));
    }
    if(hadOld) QDir(aside).removeRecursively();   // best effort; a leftover .old is cleared by the next install
    return true;
}

FirmwareInstallResult installFirmware(const FirmwareInstallOptions& options, const FirmwareInstallHooks& hooks, const FirmwareProgress& progress) {
    const auto say = [&](const QString& stage, int percent) { if(progress) progress(stage, percent); };
    if(options.destDir.trimmed().isEmpty()) return failed(QStringLiteral("No destination folder given"));
    const QString dest = QDir::cleanPath(QFileInfo(options.destDir).absoluteFilePath());
    if(!options.force && firmwareDevFlashPresent(dest))
        return {FirmwareInstallOutcome::AlreadyInstalled, QStringLiteral("dev_flash already installed at %1").arg(QDir::toNativeSeparators(dest))};

    QString error;
    say(QStringLiteral("Looking up the PS3 system software on Sony's update server"), -1);
    const FirmwareRelease release = hooks.latest(&error);
    if(release.url.isEmpty()) return failed(error.isEmpty() ? QStringLiteral("Sony's update list had no firmware") : error);
    if(release.version != options.expectedVersion)
        return failed(QStringLiteral("Sony currently offers firmware %1; this build needs %2").arg(release.version, options.expectedVersion));

    int lastPercent = -2;
    say(QStringLiteral("Downloading the PS3 system software"), 0);
    QString pup;
    if(!hooks.download(release.url, &pup, &error, [&](qint64 got, qint64 total) {
           const int percent = total > 0 ? int(std::clamp<qint64>(got * 100 / total, 0, 100)) : -1;
           if(percent != lastPercent) { lastPercent = percent; say(QStringLiteral("Downloading the PS3 system software"), percent); }
       })) {
        if(!pup.isEmpty()) QFile::remove(pup);
        return failed(error.isEmpty() ? QStringLiteral("Download failed") : QStringLiteral("Download failed: %1").arg(error));
    }
    const auto cleanup = [&](const QString& staging) { QFile::remove(pup); if(!staging.isEmpty()) QDir(staging).removeRecursively(); };

    say(QStringLiteral("Checking the downloaded file"), -1);
    QString found;
    if(!pupVersion(pup, &found, &error)) { cleanup({}); return failed(QStringLiteral("Downloaded file is not a usable PS3 update: %1").arg(error)); }
    if(found != options.expectedVersion) { cleanup({}); return failed(QStringLiteral("Downloaded firmware is %1, not %2").arg(found, options.expectedVersion)); }

    const QString staging = dest + QStringLiteral(".partial");
    if(QFileInfo::exists(staging) && !QDir(staging).removeRecursively()) { cleanup({}); return failed(QStringLiteral("Cannot clear %1").arg(QDir::toNativeSeparators(staging))); }
    say(QStringLiteral("Verifying and unpacking the system software (a few minutes)"), -1);
    if(!hooks.extract(pup, staging, &error)) { cleanup(staging); return failed(QStringLiteral("Unpacking failed: %1").arg(error)); }
    if(!firmwareDevFlashPresent(staging)) { cleanup(staging); return failed(QStringLiteral("Unpacked firmware has no vsh/resource/explore_plugin_full.rco")); }
    say(QStringLiteral("Putting the files in place"), -1);
    if(!commitFirmwareDirectory(staging, dest, &error)) { cleanup(staging); return failed(error); }
    QFile::remove(pup);
    return {FirmwareInstallOutcome::Installed, QStringLiteral("dev_flash %1 installed at %2").arg(found, QDir::toNativeSeparators(dest))};
}
}
