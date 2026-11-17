#pragma once
#include "release_index.h"
#include <QUrl>
#include <functional>
#include <atomic>
#include <QStringList>

namespace xmb {
class Preferences;
struct ReleaseSource {
    QString slug;
    QString name;
    QUrl url;
    QVector<IndexedRelease> releases;
};
using ReleaseProgress=std::function<void(qint64 received,qint64 total)>;
// Blocking store/network facade. UI callers should run operations on a worker, as for firmware fetch.
// A successful download returns its local path; failures return an empty string and an error.
class ReleaseSources {
public:
    ReleaseSources(const QString& dataDirectory,const Preferences& preferences);
    bool importSource(const QUrl& jsonUrl,QString* error=nullptr);
    // Refresh all imported remote indexes. Each source is replaced atomically only
    // after a successful fetch and parse; failures preserve its last good data.
    int refreshSources(QStringList* errors=nullptr,bool force=false);
    bool removeSource(const QString& slug,QString* error=nullptr);
    QVector<ReleaseSource> listSources(QString* error=nullptr) const;
    QString downloadedPath(const QString& source,const QString& title) const;
    QString downloadRelease(const QString& source,const QString& title,QString* error=nullptr,
                            const ReleaseProgress& progress={}) const;
    // Download a game found via the Hydra search: fetch the source index, find the title, resolve the
    // link through the debrid manager, and save the archive into destDir.
    QString downloadGameByUrl(const QString& title,const QUrl& sourceUrl,const QString& destDir,
                              QString* error=nullptr,const ReleaseProgress& progress={}) const;
    // Exact release selection for a game job. Reviewed candidate links take precedence over a lookup.
    QString downloadGame(const QString& sourceSlug,const QUrl& sourceUrl,const QString& releaseTitle,
                         const QVector<Download>& reviewed,const QString& destDir,QString* error,
                         const ReleaseProgress& progress,const std::atomic_bool* cancel,const std::atomic_bool* pausing=nullptr) const;
    static void discardGamePartial(const QString& destDir);
private:
    bool fetchSourceJson(const QUrl& jsonUrl,QVector<IndexedRelease>* releases,QString* name,QString* error,
                         const std::atomic_bool* cancel=nullptr) const;
    QString downloadResolved(const QVector<Download>& candidates,const QString& title,const QString& destDir,
                             QString* error,const ReleaseProgress& progress,
                             const std::atomic_bool* cancel=nullptr,const std::atomic_bool* pausing=nullptr) const;
    QString dataDirectory_;
    const Preferences& preferences_;
};
}
