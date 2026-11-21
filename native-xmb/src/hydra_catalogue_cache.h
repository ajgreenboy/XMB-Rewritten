#pragma once
#include "hydra_library.h"
#include <QtGlobal>
#include <QString>
#include <QVector>

namespace xmb {
// Local disk cache for the validated Hydra ranked ("hot") catalogue metadata only: the ordered
// PopularTitle list (title + cover URL + structured genre tags) a successful normal response yields.
// It never holds a download link, a magnet, or any selected-feed body, and it never holds an error:
// the only thing ever persisted is a parsed, validated, non-empty successful result.
//
// Entries are keyed by the resolved catalogue endpoint (the loopback URL under the test fixture,
// the production endpoint otherwise), so a test-fixture cache can never be served in production and
// vice versa. The serialized form carries a schema version; a file from any other version, a
// missing/empty genre list, an identity mismatch, a corrupt file or an expired entry all read as a
// miss, never an error and never a stale fallback. A fixed TTL bounds freshness and a fixed byte
// ceiling bounds storage; writes go through QSaveFile (temp file + atomic rename).
class HydraCatalogueCache {
public:
    explicit HydraCatalogueCache(const QString& directory);

    // Returns true and fills `out` only when a cache entry exists for `endpoint`, was written by
    // this schema version, and is younger than ttlMs(). `nowMs==0` uses the wall clock; a non-zero
    // value is a deterministic test seam.
    bool load(const QString& endpoint, QVector<PopularTitle>* out, qint64 nowMs=0) const;

    // Atomically persists a validated successful result for `endpoint`. `nowMs==0` stamps the wall
    // clock; a non-zero value is a deterministic test seam. Returns false (and leaves any existing
    // entry untouched) when the list is empty or the write fails.
    bool store(const QString& endpoint, const QVector<PopularTitle>& titles, qint64 nowMs=0) const;

    QString pathFor(const QString& endpoint) const;

    static qint64 ttlMs();          // freshness window for a cached catalogue entry
    static qint64 maxBytes();       // byte ceiling on the serialized entry
    static int schemaVersion();     // current serialized schema version

    QString directory() const { return directory_; }

private:
    QString directory_;
};

// Resolve the per-profile on-disk cache directory for the ranked catalogue. `dataDirectory` is the
// console-wide data directory, `profileId` the active profile id (empty before the first switch,
// otherwise "local" or a lower-case UUID), and `enabled` the opt-in flag (default OFF). Returns an
// empty string when disabled, so a disabled cache never touches the disk; the profile id scopes the
// directory so no profile can ever read another profile's cached catalogue.
QString hydraCatalogueCacheDirectory(const QString& dataDirectory, const QString& profileId, bool enabled);
} // namespace xmb
