#pragma once
#include <QString>
namespace xmb {
bool extractPup(const QString& pupPath, const QString& destDir, QString* error = nullptr);
// Reads the firmware version string (PUP entry 0x100, e.g. "4.93") without decrypting or verifying anything;
// extractPup does the HMAC verification. Reads only the header, the file table and that one entry.
bool pupVersion(const QString& pupPath, QString* version, QString* error = nullptr);
}
