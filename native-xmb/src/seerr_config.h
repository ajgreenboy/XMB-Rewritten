#pragma once

#include <QString>

namespace xmb::seerrConfig {
struct Config {
    QString server;
    QString apiKey;
};
Config load(const QString& dataDir);
bool save(const QString& dataDir, const Config& config, QString& error);
}
