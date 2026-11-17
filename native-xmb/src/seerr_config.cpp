#include "seerr_config.h"
#include "seerr_client.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dpapi.h>
#pragma comment(lib, "crypt32.lib")
#endif

namespace xmb::seerrConfig {
namespace {
constexpr qint64 MaxFile = 65536;
QString path(const QString& dataDir) { return QDir(dataDir).filePath(QStringLiteral("seerr-connection.bin")); }

bool validKey(const QString& key) {
    if (key.isEmpty() || key.size() > 512) return false;
    for (const QChar character : key)
        if (character.unicode() < 0x21 || character.unicode() > 0x7e) return false;
    return true;
}

QByteArray protect(const QByteArray& input, bool encrypt) {
#ifdef Q_OS_WIN
    QByteArray entropy = QByteArrayLiteral("xmb-seerr-connection-v1");
    DATA_BLOB source{DWORD(input.size()), reinterpret_cast<BYTE*>(const_cast<char*>(input.constData()))};
    DATA_BLOB additional{DWORD(entropy.size()), reinterpret_cast<BYTE*>(entropy.data())};
    DATA_BLOB output{};
    const BOOL ok = encrypt
        ? CryptProtectData(&source, L"XMB Seerr connection", &additional, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)
        : CryptUnprotectData(&source, nullptr, &additional, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output);
    if (!ok) return {};
    const QByteArray result(reinterpret_cast<const char*>(output.pbData), output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return result;
#else
    Q_UNUSED(input);
    Q_UNUSED(encrypt);
    return {};
#endif
}
}

Config load(const QString& dataDir) {
    QFile file(path(dataDir));
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxFile) return {};
    const QJsonObject values = QJsonDocument::fromJson(protect(file.read(MaxFile + 1), false)).object();
    if (values.value(QStringLiteral("version")).toInt() != 1) return {};
    Config config{values.value(QStringLiteral("server")).toString(), values.value(QStringLiteral("key")).toString()};
    if (SeerrClient::normalizeServer(config.server).isEmpty() || !validKey(config.apiKey)) return {};
    return config;
}

bool save(const QString& dataDir, const Config& config, QString& error) {
    error.clear();
    const QUrl server = SeerrClient::normalizeServer(config.server);
    if (server.isEmpty() || !validKey(config.apiKey)) {
        error = QObject::tr("Enter a valid Seerr server address and API key.");
        return false;
    }
    const QByteArray bytes = protect(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
        {QStringLiteral("server"), server.toString(QUrl::FullyEncoded)}, {QStringLiteral("key"), config.apiKey}})
        .toJson(QJsonDocument::Compact), true);
    if (bytes.isEmpty()) {
        error = QObject::tr("Could not protect the Seerr connection for this Windows user.");
        return false;
    }
    QSaveFile file(path(dataDir));
    if (!QDir().mkpath(dataDir) || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        error = QObject::tr("Could not save the Seerr connection.");
        return false;
    }
    return true;
}

SabConfig loadSab(const QString& dataDir) {
    QFile file(QDir(dataDir).filePath(QStringLiteral("seerr-download-client.bin")));
    if(!file.open(QIODevice::ReadOnly) || file.size()>MaxFile)return {};
    const auto values=QJsonDocument::fromJson(protect(file.read(MaxFile+1),false)).object();
    if(values.value(QStringLiteral("kind")).toString()!=QLatin1String("sabnzbd-v1"))return {};
    const SabConfig config{values.value(QStringLiteral("server")).toString(),values.value(QStringLiteral("key")).toString()};
    return validKey(config.apiKey) && !SeerrClient::normalizeServer(config.server).isEmpty()?config:SabConfig{};
}

bool saveSab(const QString& dataDir,const SabConfig& config,QString& error) {
    error.clear();
    const auto url=SeerrClient::normalizeServer(config.server);
    const bool disabled=config.server.isEmpty() && config.apiKey.isEmpty();
    if(!disabled && (url.isEmpty() || !validKey(config.apiKey))) {
        error=QObject::tr("Enter the SABnzbd address and API key.");
        return false;
    }
    const auto bytes=protect(QJsonDocument(QJsonObject{{QStringLiteral("kind"),QStringLiteral("sabnzbd-v1")},
        {QStringLiteral("server"),url.toString(QUrl::FullyEncoded)},
        {QStringLiteral("key"),config.apiKey}}).toJson(QJsonDocument::Compact),true);
    if(bytes.isEmpty()) {
        error=QObject::tr("Could not protect the download client configuration for this Windows user.");
        return false;
    }
    QSaveFile file(QDir(dataDir).filePath(QStringLiteral("seerr-download-client.bin")));
    file.setDirectWriteFallback(false);
    if(!QDir().mkpath(dataDir) || !file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()) {
        error=QObject::tr("Could not save the download client configuration.");
        return false;
    }
    return true;
}

}
