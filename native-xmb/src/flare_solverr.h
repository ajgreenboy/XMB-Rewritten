#pragma once
#include <QByteArray>
#include <QUrl>
#include <atomic>

namespace xmb {
// True when a fetched body looks like an HTML page (a bot-protection challenge or an error page)
// rather than the JSON the caller expected. Shared by the release-source importer and the Hydra
// library so both decide the FlareSolverr fallback with one rule.
inline bool looksLikeHtml(const QByteArray& bytes) {
    const QString head=QString::fromLatin1(bytes.left(512)).trimmed().toLower();
    return head.startsWith(QStringLiteral("<!doctype html")) || head.startsWith(QStringLiteral("<html"));
}
// FlareSolverr client: POSTs a URL to a local FlareSolverr instance and returns the challenge-solved
// page content. Blocking, like the other network clients; call from a worker. The endpoint is the
// configured base URL (e.g. "http://localhost:8191").
class FlareSolverr {
public:
    explicit FlareSolverr(const QString& endpoint):endpoint_(endpoint) {}
    bool request(const QUrl& url, QByteArray* response, QString* error=nullptr,
                 const std::atomic_bool* cancel=nullptr) const;
private:
    QString endpoint_;
};
} // namespace xmb
