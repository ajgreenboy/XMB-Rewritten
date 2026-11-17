#pragma once
#include <QJsonObject>
#include <QUrl>
#include <QStringList>
#include <atomic>

namespace xmb {
class Preferences;
struct DebridTorrentInfo { QString status; QStringList links; QString filename; };
// Blocking, bounded network operations, like firmware_fetcher. Call from a worker for UI use.
// Preferences is borrowed and must outlive the client; read the active profile token per request.
class RealDebrid {
public:
    explicit RealDebrid(const Preferences& preferences,const std::atomic_bool* cancel=nullptr)
        :preferences_(preferences),cancel_(cancel) {}
    bool hasToken() const;
    QUrl unrestrict(const QUrl& directLink, QString* error=nullptr) const;
    QString addMagnet(const QString& magnet, QString* error=nullptr) const;
    DebridTorrentInfo torrentInfo(const QString& id, QString* error=nullptr) const;
    QJsonObject instantAvailability(const QString& infohash, QString* error=nullptr) const;
    bool selectFiles(const QString& id, const QString& files, QString* error=nullptr) const;
private:
    const Preferences& preferences_;
    const std::atomic_bool* cancel_;
    bool request(const QString& path, const QByteArray* form, QJsonObject* result, QString* error) const;
};
}
