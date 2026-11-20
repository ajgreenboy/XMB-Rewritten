#pragma once
#include <QString>
#include <QStringList>
namespace xmb {
// Extract an archive into destDir using the system tar (zip/tar) or 7-Zip (rar/7z). Returns false with a
// message on failure. Blocking; call from a worker.
// Breadth-first search for the shallowest game .exe, skipping uninstallers/setup/install stubs.
QString findGameExecutable(const QString& folder, QString* error=nullptr);
QStringList findGameExecutables(const QString& folder);
} // namespace xmb
