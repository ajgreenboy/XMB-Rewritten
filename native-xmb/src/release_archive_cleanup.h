#pragma once
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QString>

namespace xmb {

// Removes a cancelled release job's archive staging directory
// "<gamesDirectory>/Download Archives/<jobId>" safely and narrowly. This is the only
// place a background game transfer ever writes its completed archive and partial download
// files, and the directory name is the job's own 32-hex UUID, so a validated path cannot
// collide with another job's directory or a user's content.
//
// Deletion is deliberately conservative:
//   - the job id must be the exact 32-hex format the queue restores, and gamesDirectory
//     must be absolute, so a malformed or relative path is refused outright;
//   - the directory must be a real directory and not a symlink or NTFS junction (the latter
//     is not reported by QFileInfo::isSymLink(), so isJunction() is checked explicitly);
//   - only the job's known artifacts are removed — the published archive (when it resolves
//     to this exact directory), the partial download (.xmb-game.part) and its resume
//     checkpoint (.xmb-game.resume.json) — and the directory is dropped only when empty.
// Any unrecognised entry is left in place, so unrelated or user files are preserved.
//
// Callers must invoke this only after a worker has settled on definitive cancellation
// (never on pause, failure, or a queued resume), when no transfer is still writing there.
//
// Returns true when the job's completed archive is gone — removed, or never produced, or
// already absent — which is exactly the condition under which a caller may clear its
// archive reference. It returns false (refusing to delete) when the directory or an
// artifact is a symlink/junction or the archive resolves outside the job's directory.
inline bool releaseJobDiscardArchive(const QString& gamesDirectory, const QString& jobId,
                                     const QString& archivePath) {
    static const QRegularExpression idFormat(QStringLiteral("^[a-f0-9]{32}$"));
    if (!QDir::isAbsolutePath(gamesDirectory) || !idFormat.match(jobId).hasMatch()) return false;
    const auto isLink = [](const QFileInfo& info) { return info.isSymLink() || info.isJunction(); };
    const QDir archiveDir(QDir(gamesDirectory).filePath(QStringLiteral("Download Archives/") + jobId));
    const QFileInfo archiveDirInfo(archiveDir.path());
    if (!archiveDirInfo.exists()) return true;                        // already clean
    if (!archiveDirInfo.isDir() || isLink(archiveDirInfo)) return false;  // refuse symlink/junction dir
    const QString dirCanonical = archiveDirInfo.canonicalFilePath();
    if (dirCanonical.isEmpty()) return false;
    // Remove the published archive only when it is a plain file inside this job's directory.
    bool archiveGone = archivePath.isEmpty();
    if (!archivePath.isEmpty()) {
        const QFileInfo file(archivePath);
        const QFileInfo parent(file.absolutePath());
        if (file.isFile() && !isLink(file) && !isLink(parent)) {
            const QString parentCanonical = parent.canonicalFilePath();
            if (!parentCanonical.isEmpty() && parentCanonical == dirCanonical) {
                QFile::remove(file.absoluteFilePath());
                archiveGone = true;
            }
            // else: resolves outside the directory -> refuse (archiveGone stays false).
        } else if (!file.exists()) {
            archiveGone = true;                                        // already absent
        }
        // else: symlink/junction/non-file -> refuse (archiveGone stays false).
    }
    const auto removeOwned = [&](const QString& name) {
        const QFileInfo info(archiveDir.filePath(name));
        if (info.isFile() && !isLink(info)) QFile::remove(info.absoluteFilePath());
    };
    removeOwned(QStringLiteral(".xmb-game.part"));
    removeOwned(QStringLiteral(".xmb-game.resume.json"));
    QDir().rmdir(archiveDir.path());   // drops the directory only when empty
    return archiveGone;
}

} // namespace xmb
