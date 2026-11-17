#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QUrlQuery>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace xmb {

// Small asynchronous Seerr API client. The caller owns presentation and persistence of configuration.
class SeerrClient final : public QObject {
    Q_OBJECT
public:
    using Result = std::function<void(QJsonObject, QString)>;

    explicit SeerrClient(QObject* parent = nullptr);
    void configure(QUrl base, QString apiKey);
    static QUrl normalizeServer(QString server);
    void search(QString query, int page, Result cb);
    void details(QString type, int tmdbId, Result cb);
    void requests(int skip, Result cb, bool processingOnly=false);
    void submit(QString type, int tmdbId, QList<int> seasons, Result cb);
    void check(Result cb);

private:
    QNetworkAccessManager* network_ = nullptr;
    QUrl base_;
    QString apiKey_;
    quint64 generation_ = 0;
    QList<QNetworkReply*> pending_;

    void get(const QString& endpoint, const QUrlQuery& query, Result cb);
    void post(const QString& endpoint, const QJsonObject& body, Result cb);
    void send(const QString& endpoint, const QUrlQuery& query, const QByteArray& body, bool isPost, Result cb);
};

}
