#include "logo_tiles.h"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>
#include <QList>
#include <QPainter>
#include <QPair>
#include <QRegularExpression>
#include <algorithm>

namespace xmb::logoTiles {
namespace {
constexpr int MaxSearchDepth = 2;
constexpr int MaxFolders = 32;
constexpr int MaxEntriesPerFolder = 256;
constexpr int MaxImageDimension = 8192;
constexpr quint64 MaxImagePixels = 64ULL * 1024ULL * 1024ULL;
}

bool enabledFor(const QString& envValue) { return envValue.trimmed() != QLatin1String("0"); }
bool enabled() { return enabledFor(qEnvironmentVariable("XMB_PS3_LOGO_TILES")); }

Art chooseArt(bool lookEnabled, bool hasLogo, bool hasHeader) {
    if (lookEnabled && hasLogo) return Art::Logo;
    return hasHeader ? Art::Header : Art::None;
}

bool validAppId(const QString& appId) {
    static const QRegularExpression digits(QStringLiteral("^[0-9]{1,12}$"));
    return digits.match(appId).hasMatch() && appId.toULongLong() != 0;
}

QString clientLogoPath(const QString& steamRoot, const QString& appId) {
    if (steamRoot.trimmed().isEmpty() || !validAppId(appId)) return {};
    const QDir base(QDir(steamRoot).filePath(QStringLiteral("appcache/librarycache/%1").arg(appId)));
    if (!base.exists()) return {};
    const auto usable = [](const QFileInfo& info) { return info.isFile() && info.size() > 0; };
    const QFileInfo direct(base.filePath(QStringLiteral("logo.png")));
    if (usable(direct)) return direct.absoluteFilePath();
    // Breadth-first through hashed sub-folders, bounded in depth and in how many folders and entries are looked at.
    QList<QPair<QDir, int>> queue{{base, 0}};
    int folders = 0;
    for (qsizetype next = 0; next < queue.size(); ++next) {
        const QDir dir = queue[next].first;
        const int depth = queue[next].second;
        if (++folders > MaxFolders) break;
        const auto entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        int seen = 0;
        for (const QFileInfo& entry : entries) {
            if (++seen > MaxEntriesPerFolder) break;
            if (entry.isDir()) {
                if (!entry.isSymLink() && depth < MaxSearchDepth) queue.append({QDir(entry.absoluteFilePath()), depth + 1});
            } else if (entry.fileName().compare(QStringLiteral("logo.png"), Qt::CaseInsensitive) == 0 && usable(entry)) {
                return entry.absoluteFilePath();
            }
        }
    }
    return {};
}

QString cachedLogoPath(const QString& cacheDir, const QString& appId) {
    if (!validAppId(appId) || cacheDir.trimmed().isEmpty()) return {};
    return QDir::cleanPath(QDir(cacheDir).absoluteFilePath(appId + QStringLiteral("_logo.image")));
}

QString missMemoPath(const QString& cacheDir, const QString& appId) {
    if (!validAppId(appId) || cacheDir.trimmed().isEmpty()) return {};
    return QDir::cleanPath(QDir(cacheDir).absoluteFilePath(appId + QStringLiteral("_logo.miss")));
}

bool missMemoActive(qint64 memoMtimeSecs, qint64 nowSecs) {
    return memoMtimeSecs > 0 && nowSecs - memoMtimeSecs < MissMemoSeconds;
}

QUrl cdnLogoUrl(const QString& appId) {
    if (!validAppId(appId)) return {};
    return QUrl(QStringLiteral("https://cdn.cloudflare.steamstatic.com/steam/apps/%1/logo.png").arg(appId));
}

QImage trimTransparent(const QImage& image, int alphaThreshold, int pad) {
    if (image.isNull()) return {};
    if (!image.hasAlphaChannel()) return image;
    const QImage argb = image.format() == QImage::Format_ARGB32 ? image : image.convertToFormat(QImage::Format_ARGB32);
    int left = argb.width(), top = argb.height(), right = -1, bottom = -1;
    for (int y = 0; y < argb.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(argb.constScanLine(y));
        for (int x = 0; x < argb.width(); ++x) {
            if (qAlpha(row[x]) <= alphaThreshold) continue;
            left = std::min(left, x); right = std::max(right, x);
            top = std::min(top, y); bottom = std::max(bottom, y);
        }
    }
    if (right < left || bottom < top) return {};
    left = std::max(0, left - pad); top = std::max(0, top - pad);
    right = std::min(argb.width() - 1, right + pad); bottom = std::min(argb.height() - 1, bottom + pad);
    return argb.copy(left, top, right - left + 1, bottom - top + 1);
}

QImage loadLogo(const QString& path, QSize maxSize) {
    if (path.isEmpty()) return {};
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (!size.isValid() || size.width() > MaxImageDimension || size.height() > MaxImageDimension ||
        quint64(size.width()) * quint64(size.height()) > MaxImagePixels) return {};
    // Bound the decode of a very large logo; trimming and the final fit happen on what is left.
    if (size.width() > 1024 || size.height() > 576) reader.setScaledSize(size.scaled(QSize(1024, 576), Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (image.isNull()) return {};
    image = trimTransparent(image);
    if (image.isNull()) return {};
    if (image.width() > maxSize.width() || image.height() > maxSize.height())
        image = image.scaled(maxSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

QSizeF fitLogo(const QSizeF& source, const QSizeF& box) {
    if (source.width() <= 0 || source.height() <= 0 || box.width() <= 0 || box.height() <= 0) return {};
    return source.scaled(box, Qt::KeepAspectRatio);
}

QSizeF tileBox(const QSizeF& card) { return {card.width() * TileWidthShare, card.height() * TileHeightShare}; }

QImage onSquare(const QImage& logo, int side) {
    if (logo.isNull() || side <= 0) return {};
    QImage canvas(side, side, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    const double margin = side * 0.06;
    const QSizeF fitted = fitLogo(logo.size(), QSizeF(side - 2 * margin, side - 2 * margin));
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(QRectF((side - fitted.width()) / 2., (side - fitted.height()) / 2., fitted.width(), fitted.height()), logo);
    return canvas;
}

} // namespace xmb::logoTiles
