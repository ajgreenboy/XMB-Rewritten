#include "firmware_extract_rust.h"

#include "firmware_extract_abi.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <cstdio>

namespace xmb::firmware_rust {
namespace {
#if !defined(XMB_FIRMWARE_NO_EMBEDDED_KEYS) && __has_include("firmware_keys.inc")
#include "firmware_keys.inc"
#else
#include "firmware_keys.example.inc"
#endif

struct KeyMaterial {
    QByteArray pup;
    QByteArray erk;
    QByteArray riv;
};

bool fail(QString* error, const QString& text) {
    if (error) *error = text;
    return false;
}

bool loadKeys(KeyMaterial& k, QString* error) {
    QString path = qEnvironmentVariable("XMB_FIRMWARE_KEYS");
    if (path.isEmpty() &&
        QByteArray(reinterpret_cast<const char*>(PUP_KEY), 64) == QByteArray(64, 0) &&
        QCoreApplication::instance()) {
        const QString beside = QCoreApplication::applicationDirPath() + QStringLiteral("/firmware_keys.inc");
        if (QFileInfo(beside).isFile()) path = beside;
    }
    if (path.isEmpty()) {
        k = {QByteArray(reinterpret_cast<const char*>(PUP_KEY), 64),
             QByteArray(reinterpret_cast<const char*>(SCEPKG_ERK), 32),
             QByteArray(reinterpret_cast<const char*>(SCEPKG_RIV), 16)};
    } else {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return fail(error, QStringLiteral("Cannot read XMB_FIRMWARE_KEYS: %1").arg(f.errorString()));
        if (f.size() > 65536) return fail(error, QStringLiteral("Key file is too large"));
        QString text = QString::fromUtf8(f.readAll());
        text.remove(QRegularExpression(QStringLiteral("//[^\n]*|/\\*[\\s\\S]*?\\*/")));
        const auto parse = [&](const QString& name, int length, QByteArray& out) {
            const auto m = QRegularExpression(name + QStringLiteral("\\s*\\[[^\\]]*\\]\\s*=\\s*\\{([^}]*)\\}")).match(text);
            if (!m.hasMatch()) return false;
            const auto tokens = m.captured(1).split(',', Qt::SkipEmptyParts);
            if (tokens.size() != length) return false;
            for (const auto& token : tokens) {
                bool ok = false;
                const uint v = token.trimmed().toUInt(&ok, 0);
                if (!ok || v > 255) return false;
                out.append(char(v));
            }
            return true;
        };
        if (!parse(QStringLiteral("PUP_KEY"), 64, k.pup) ||
            !parse(QStringLiteral("SCEPKG_ERK"), 32, k.erk) ||
            !parse(QStringLiteral("SCEPKG_RIV"), 16, k.riv))
            return fail(error, QStringLiteral("Invalid key declarations in XMB_FIRMWARE_KEYS"));
    }
    return true;
}
}  // namespace

bool extractPup(const QString& pupPath, const QString& destDir, QString* error) {
    if (error) error->clear();
    KeyMaterial k;
    if (!loadKeys(k, error)) return false;
    const QByteArray pupUtf8 = pupPath.toUtf8();
    const QByteArray dstUtf8 = destDir.toUtf8();
    fw_extract_stats stats{};
    const int32_t rc = fw_extract_pup(
        reinterpret_cast<const uint8_t*>(pupUtf8.constData()), size_t(pupUtf8.size()),
        reinterpret_cast<const uint8_t*>(dstUtf8.constData()), size_t(dstUtf8.size()),
        reinterpret_cast<const uint8_t*>(k.pup.constData()), size_t(k.pup.size()),
        reinterpret_cast<const uint8_t*>(k.erk.constData()), size_t(k.erk.size()),
        reinterpret_cast<const uint8_t*>(k.riv.constData()), size_t(k.riv.size()),
        &stats);
    if (rc != FW_EXTRACT_OK) {
        const char* msg = fw_extract_last_error();
        return fail(error, msg ? QString::fromUtf8(msg) : QStringLiteral("Rust firmware extraction failed"));
    }
    const QString root = QDir::cleanPath(QFileInfo(destDir).absoluteFilePath());
    if (!stats.explore_rco_present)
        std::fprintf(stderr, "Firmware extraction warning: explore_plugin_full.rco absent (layout may differ)\n");
    std::fprintf(stdout, "Firmware extracted: %llu entries verified, %llu dev_flash packages, %llu bytes written\nDestination: %s\n",
                 static_cast<unsigned long long>(stats.entries_verified),
                 static_cast<unsigned long long>(stats.packages),
                 static_cast<unsigned long long>(stats.bytes_written),
                 root.toUtf8().constData());
    return true;
}

bool pupVersion(const QString& pupPath, QString* version, QString* error) {
    if (error) error->clear();
    if (version) version->clear();
    const QByteArray pupUtf8 = pupPath.toUtf8();
    uint8_t buf[256]{};
    const int32_t n = fw_pup_version(
        reinterpret_cast<const uint8_t*>(pupUtf8.constData()), size_t(pupUtf8.size()),
        buf, sizeof(buf));
    if (n <= 0) {
        const char* msg = fw_extract_last_error();
        return fail(error, msg ? QString::fromUtf8(msg) : QStringLiteral("Rust PUP version read failed"));
    }
    if (version) *version = QString::fromUtf8(reinterpret_cast<const char*>(buf), n);
    return true;
}

}  // namespace xmb::firmware_rust
