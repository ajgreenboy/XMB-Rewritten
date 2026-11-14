#pragma once
// One firmware install: Sony list -> download -> version check -> extract -> atomic swap into place.
// Shared by the setup wizard's "Download from Sony" row and the headless --firmware-install command.
// The network and extraction steps are injected (Hooks) so the policy around them (4.93 only, idempotence,
// staging directory then rename, cleanup) is testable offline.
#include "firmware_fetcher.h"
#include <QString>
#include <functional>

namespace xmb {
struct FirmwareInstallHooks {
    std::function<FirmwareRelease(QString* error)> latest;
    std::function<bool(const QString& url, QString* path, QString* error, const std::function<void(qint64, qint64)>& progress)> download;
    std::function<bool(const QString& pup, const QString& destDir, QString* error)> extract;
};
struct FirmwareInstallOptions {
    QString destDir;
    QString expectedVersion = QStringLiteral("4.93");
    bool force = false;   // replace a dev_flash that is already complete
};
enum class FirmwareInstallOutcome { Installed, AlreadyInstalled, Failed };
struct FirmwareInstallResult { FirmwareInstallOutcome outcome = FirmwareInstallOutcome::Failed; QString message; };
// stage is a short phrase; percent is 0..100 while a stage has a measurable length, otherwise -1.
using FirmwareProgress = std::function<void(const QString& stage, int percent)>;

// A dev_flash counts as installed when vsh/resource/explore_plugin_full.rco exists in it.
bool firmwareDevFlashPresent(const QString& dir);
// Replace dest with staging (a sibling directory on the same volume). The old dest is moved aside first and put back
// if the swap fails, so dest is always either the old tree or the complete new one.
bool commitFirmwareDirectory(const QString& staging, const QString& dest, QString* error);
FirmwareInstallResult installFirmware(const FirmwareInstallOptions& options, const FirmwareInstallHooks& hooks, const FirmwareProgress& progress = {});
// The real network and extraction steps (latestFirmware, downloadPup, extractPup); defined in firmware_fetcher.cpp.
FirmwareInstallHooks networkFirmwareHooks();
}
