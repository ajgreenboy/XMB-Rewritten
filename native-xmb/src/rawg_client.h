#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

class QJsonObject;
class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace xmb {

struct RawgGame {
    int id = 0;
    QString name;
    QString slug;
    QString released;
    QStringList platforms;
    QStringList genres;
    QUrl imageUrl;
    QUrl pageUrl;
};

// The RAWG /api/games/{id} document, reduced to the fields the XMB's game
// detail page needs. Every field is taken verbatim from the API and only
// populated when the API actually returned it: nothing here is synthesized.
struct RawgGameDetails {
    int id = 0;
    QString name;
    QString description;   // plain text (description_raw, or description with tags stripped)
    int releaseYear = 0;   // four-digit year from "released"; 0 when absent/TBA/unreadable
    QStringList developers;
    QStringList publishers;
    QStringList genres;
    QStringList platforms;
};

// Pure parser for the RAWG /api/games/{id} JSON object. No network and no
// fabrication: absent or unreadable fields are left empty/zero. Exposed so the
// deterministic fixture tests and the visual layer share one definition.
RawgGameDetails parseRawgGameDetails(const QJsonObject& object);

using RawgGameList = QVector<RawgGame>;

// Unicode-normalized, case-folded title key shared by the detail lookup and the
// artwork lookup, so the same name resolves to the same id everywhere. Trademark
// and quote marks are removed and every run of non-letter/number characters
// collapses to one space.
QString normalizeRawgTitle(QString title);

// Conservative title -> RAWG id resolution for the game-detail lookup. Returns the
// unique id whose normalized name exactly equals the normalized `title`, or 0 when
// no result matches or the match is ambiguous (two distinct ids share the name).
// Never breaks a tie by result order or by artwork availability, so a same-name
// release (e.g. a 2006 and a 2017 "Prey") yields no details rather than a wrong
// description. Exact match only: a result whose name is an edition suffix of the
// wanted title ("Prey Remastered" for "Prey") is a different game and is not taken.
int resolveRawgGameId(const RawgGameList& games, const QString& title);

// A single display line for the detail page's metadata strip: the fields that are
// actually present (genre, release year, developer, platform) joined with a wide
// separator. Empty when none are present, so the view falls back to its honest
// "Not available" copy. Values are the verbatim parsed names; nothing is synthesized.
QString rawgMetadataSummary(const RawgGameDetails& details);

class RawgClient final : public QObject {
    Q_OBJECT
public:
    explicit RawgClient(QString cacheDirectory, QObject* parent = nullptr,
                        QNetworkAccessManager* network = nullptr);
    ~RawgClient() override;

    static QString apiKeyFilePath();
    static bool hasApiKey();
    static bool hasEnvironmentKey();
    static bool saveApiKey(const QString& apiKey, QString& error);

    void search(const QString& query);
    // Fetches the full game-detail document (description, genres, release year,
    // developers, publishers, platforms) for one RAWG id. With no key configured
    // this fails fast without touching the network, so the game catalogue is
    // never blocked on it.
    void fetchDetails(int gameId);
    void downloadArtwork(const RawgGame& game);
    void cancel();

signals:
    void searchReady(QVector<xmb::RawgGame> games);
    void detailsReady(xmb::RawgGameDetails details);
    void artworkReady(xmb::RawgGame game, QString path, QImage preview);
    void failed(QString safeError);

private:
    enum class RequestRole { Metadata, Details, Image };
    static bool jsonRole(RequestRole role);
    static qsizetype roleByteLimit(RequestRole role);

    void beginRequest(const QUrl& url, RequestRole role, quint64 generation, int redirects = 0);
    void finishRequest(QNetworkReply* reply, RequestRole role, quint64 generation,
                       int redirects, const QUrl& requestUrl);
    void finishMetadata(const QByteArray& bytes, quint64 generation);
    void finishDetails(const QByteArray& bytes, quint64 generation);
    void finishImage(const QByteArray& bytes, quint64 generation);
    QString artworkCachePath(const RawgGame& game) const;
    void failIfCurrent(quint64 generation, const QString& message);

    QString cacheDirectory_;
    QNetworkAccessManager* network_ = nullptr;
    QNetworkReply* reply_ = nullptr;
    QTimer* deadline_ = nullptr;
    QByteArray response_;
    RawgGame artworkGame_;
    quint64 generation_ = 0;
};

} // namespace xmb

Q_DECLARE_METATYPE(xmb::RawgGame)
Q_DECLARE_METATYPE(xmb::RawgGameDetails)
Q_DECLARE_METATYPE(xmb::RawgGameList)
