#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <functional>
#include <optional>

class QNetworkAccessManager;

namespace xmb {

class SabnzbdClient final : public QObject {
public:
    using StatusResult = std::function<void(QHash<QString, bool> pausedById, QString error)>;
    using Result = std::function<void(QString error)>;

    explicit SabnzbdClient(QObject* parent = nullptr);

    void configure(QUrl base, QString apiKey);
    void status(QStringList ids, StatusResult callback);
    void setPaused(QStringList ids, bool paused, Result callback);
    void cancel(QStringList ids, Result callback);

    static QUrl normalizeServer(const QString& server);
    static bool isJobId(const QString& id);

private:
    using ApiResult = std::function<void(QJsonObject, QString)>;

    void change(QStringList ids, const QByteArray& action, std::optional<bool> paused, Result callback);
    void send(const QByteArray& parameters, ApiResult callback);

    QNetworkAccessManager* network_;
    QUrl base_;
    QString apiKey_;
    quint64 generation_ = 0;
};

} // namespace xmb
