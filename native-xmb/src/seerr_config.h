#pragma once

#include <QString>

namespace xmb::seerrConfig {
struct Config {
    QString server;
    QString apiKey;
};
struct SabConfig {
    QString server;
    QString apiKey;
    friend bool operator==(const SabConfig&,const SabConfig&) = default;
};
SabConfig loadSab(const QString& dataDir);
bool saveSab(const QString& dataDir,const SabConfig& config,QString& error);
Config load(const QString& dataDir);
bool save(const QString& dataDir, const Config& config, QString& error);
}
