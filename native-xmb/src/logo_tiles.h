#pragma once

// PS3-style game tiles: a game is drawn as its transparent logo artwork, unframed, instead of a boxed header
// capsule. This is the pure part (no network, no shell state): where a logo lives, which art a row wears, and how
// a logo is trimmed and fitted. The downloads live in steam_artwork.cpp and imported_art.cpp.
// XMB_PS3_LOGO_TILES=0 restores the boxed header look.

#include <QImage>
#include <QSizeF>
#include <QString>
#include <QUrl>

namespace xmb::logoTiles {

enum class Art { Logo, Header, None };

// "0" turns the look off; anything else (including unset) leaves it on.
bool enabledFor(const QString& envValue);
bool enabled();

// Which art a row wears: the logo when the look is on and one exists, else the header, else nothing.
Art chooseArt(bool lookEnabled, bool hasLogo, bool hasHeader);

bool validAppId(const QString& appId);

// The Steam client's own cached logo: <steamRoot>/appcache/librarycache/<appId>/logo.png, or the same file name in a
// hashed sub-folder (newer clients). The search is bounded (depth, folders and entries); empty when none exists.
QString clientLogoPath(const QString& steamRoot, const QString& appId);

// This build's CDN copy: <cacheDir>/<appId>_logo.image, and the memo left by a 404 (<appId>_logo.miss).
QString cachedLogoPath(const QString& cacheDir, const QString& appId);
QString missMemoPath(const QString& cacheDir, const QString& appId);
// A memo stops the download for this long; network errors are not memoised (they are remembered per session).
constexpr qint64 MissMemoSeconds = 30LL * 24 * 60 * 60;
bool missMemoActive(qint64 memoMtimeSecs, qint64 nowSecs);
QUrl cdnLogoUrl(const QString& appId);

// Smallest rectangle holding every pixel whose alpha exceeds the threshold, grown by pad and clamped. An image
// without an alpha channel is returned whole; a fully transparent one gives a null image.
QImage trimTransparent(const QImage& image, int alphaThreshold = 8, int pad = 2);
// Decode, trim and shrink (never enlarge) a logo file to fit maxSize with its aspect ratio kept. Null when the file
// is unreadable, oversized or empty of ink.
QImage loadLogo(const QString& path, QSize maxSize = QSize(512, 288));

// Aspect fit of a source into a box: never distorts, never leaves the box (upscales small sources).
QSizeF fitLogo(const QSizeF& source, const QSizeF& box);
// The share of the ICON0 card a logo may use: about 130 px wide on a 1280x720 frame at the focused slot.
constexpr double TileWidthShare = 0.62;
constexpr double TileHeightShare = 0.95;
QSizeF tileBox(const QSizeF& card);

// A logo centred on a transparent square, for rows inside a page (those take the item-icon layout, which treats a
// wide image as a tile): the logo spans the width less a margin and keeps its aspect ratio.
QImage onSquare(const QImage& logo, int side = 288);

} // namespace xmb::logoTiles
