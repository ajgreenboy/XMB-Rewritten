// PS3 logo tiles: the pure logic only (no window, no network). Logo path resolution in a synthetic Steam root,
// the logo / header / none choice, the opt-out value, cache and memo paths, transparent trimming and aspect fit.
//   xmb-logo-tiles-tests
#include "logo_tiles.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>

using namespace xmb::logoTiles;

namespace {
int checks = 0, failures = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}
bool near(double a, double b) { return std::abs(a - b) < 0.01; }

bool writePng(const QString& path, const QImage& image) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    return image.save(path, "PNG");
}
QImage inkOnTransparent(int w, int h, QRect ink) {
    QImage image(w, h, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    for (int y = ink.top(); y <= ink.bottom(); ++y)
        for (int x = ink.left(); x <= ink.right(); ++x) image.setPixel(x, y, qRgba(240, 40, 40, 255));
    return image;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // ---- opt-out -----------------------------------------------------------------------------------------------
    check(enabledFor(QString()), "env: unset leaves the look on");
    check(enabledFor(QStringLiteral("1")) && enabledFor(QStringLiteral("yes")), "env: any other value leaves it on");
    check(!enabledFor(QStringLiteral("0")) && !enabledFor(QStringLiteral(" 0 ")), "env: 0 restores the old look");

    // ---- art choice --------------------------------------------------------------------------------------------
    check(chooseArt(true, true, true) == Art::Logo, "choice: logo beats header");
    check(chooseArt(true, true, false) == Art::Logo, "choice: logo without header");
    check(chooseArt(true, false, true) == Art::Header, "choice: header when there is no logo");
    check(chooseArt(true, false, false) == Art::None, "choice: nothing when neither exists");
    check(chooseArt(false, true, true) == Art::Header, "choice: opt-out keeps the header");
    check(chooseArt(false, true, false) == Art::None, "choice: opt-out never shows a logo");

    // ---- app ids, urls, cache and memo paths -------------------------------------------------------------------
    check(validAppId(QStringLiteral("730")) && validAppId(QStringLiteral("1245620")), "appid: digits are valid");
    check(!validAppId(QString()) && !validAppId(QStringLiteral("0")) && !validAppId(QStringLiteral("../1")) &&
          !validAppId(QStringLiteral("12a")) && !validAppId(QStringLiteral("1234567890123")), "appid: anything else is not");
    check(cdnLogoUrl(QStringLiteral("730")).toString() == QLatin1String("https://cdn.cloudflare.steamstatic.com/steam/apps/730/logo.png"),
          "url: the CDN logo for an app");
    check(cdnLogoUrl(QStringLiteral("../x")).isEmpty(), "url: an invalid id gives no url");
    check(cachedLogoPath(QStringLiteral("cache/steam"), QStringLiteral("730")).endsWith(QLatin1String("cache/steam/730_logo.image")),
          "cache: logo file name");
    check(missMemoPath(QStringLiteral("cache/steam"), QStringLiteral("730")).endsWith(QLatin1String("cache/steam/730_logo.miss")),
          "cache: miss memo file name");
    check(cachedLogoPath(QStringLiteral("cache/steam"), QStringLiteral("x")).isEmpty() &&
          cachedLogoPath(QString(), QStringLiteral("730")).isEmpty(), "cache: bad input gives no path");
    check(missMemoActive(1000, 1000 + MissMemoSeconds - 1), "memo: active inside the window");
    check(!missMemoActive(1000, 1000 + MissMemoSeconds), "memo: expired after the window");
    check(!missMemoActive(0, 5), "memo: absent file (mtime 0) is not active");

    // ---- client logo resolution --------------------------------------------------------------------------------
    QTemporaryDir scratch;
    check(scratch.isValid(), "scratch directory");
    const QString root = scratch.path();
    const QImage logo = inkOnTransparent(64, 32, QRect(8, 4, 48, 20));
    check(writePng(root + QStringLiteral("/appcache/librarycache/100/logo.png"), logo), "setup: direct logo");
    check(writePng(root + QStringLiteral("/appcache/librarycache/200/0123abcd/logo.png"), logo), "setup: hashed logo");
    check(writePng(root + QStringLiteral("/appcache/librarycache/300/aa/bb/Logo.PNG"), logo), "setup: two levels deep");
    check(writePng(root + QStringLiteral("/appcache/librarycache/400/header.jpg"), logo), "setup: header only");
    check(writePng(root + QStringLiteral("/appcache/librarycache/500/a/b/c/d/logo.png"), logo), "setup: too deep");
    QDir().mkpath(root + QStringLiteral("/appcache/librarycache/600"));
    QFile empty(root + QStringLiteral("/appcache/librarycache/600/logo.png"));
    check(empty.open(QIODevice::WriteOnly), "setup: empty logo file");
    empty.close();
    // a logo in another app's folder must never be picked up
    check(writePng(root + QStringLiteral("/appcache/librarycache/101/logo.png"), logo), "setup: neighbour app");

    check(clientLogoPath(root, QStringLiteral("100")).endsWith(QLatin1String("librarycache/100/logo.png")), "client: direct logo.png");
    check(clientLogoPath(root, QStringLiteral("200")).endsWith(QLatin1String("librarycache/200/0123abcd/logo.png")), "client: hashed sub-folder");
    check(!clientLogoPath(root, QStringLiteral("300")).isEmpty(), "client: two levels deep, any letter case");
    check(clientLogoPath(root, QStringLiteral("400")).isEmpty(), "client: a header is not a logo");
    check(clientLogoPath(root, QStringLiteral("500")).isEmpty(), "client: the search is depth-bounded");
    check(clientLogoPath(root, QStringLiteral("600")).isEmpty(), "client: an empty file is not a logo");
    check(clientLogoPath(root, QStringLiteral("999")).isEmpty(), "client: unknown app");
    check(clientLogoPath(root, QStringLiteral("10")).isEmpty(), "client: no prefix matching on app ids");
    check(clientLogoPath(QString(), QStringLiteral("100")).isEmpty() && clientLogoPath(root, QStringLiteral("../100")).isEmpty(),
          "client: empty root and traversal are rejected");

    // ---- trimming ----------------------------------------------------------------------------------------------
    {
        const QImage trimmed = trimTransparent(logo, 8, 0);
        check(trimmed.size() == QSize(48, 20), "trim: crops to the ink bounds");
        const QImage padded = trimTransparent(logo, 8, 2);
        check(padded.size() == QSize(52, 24), "trim: pad grows the box");
        const QImage clamped = trimTransparent(inkOnTransparent(10, 10, QRect(0, 0, 10, 10)), 8, 5);
        check(clamped.size() == QSize(10, 10), "trim: pad is clamped to the image");
        QImage blank(20, 20, QImage::Format_ARGB32);
        blank.fill(Qt::transparent);
        check(trimTransparent(blank).isNull(), "trim: a fully transparent image has no logo");
        QImage faint = blank;
        faint.setPixel(3, 3, qRgba(255, 255, 255, 4));
        check(trimTransparent(faint).isNull(), "trim: faint noise under the threshold is ignored");
        QImage opaque(20, 10, QImage::Format_RGB32);
        opaque.fill(Qt::red);
        check(trimTransparent(opaque).size() == QSize(20, 10), "trim: an opaque image is returned whole");
        check(trimTransparent(QImage()).isNull(), "trim: null in, null out");
    }

    // ---- loading ----------------------------------------------------------------------------------------------
    {
        const QImage loaded = loadLogo(clientLogoPath(root, QStringLiteral("100")));
        check(loaded.size() == QSize(52, 24), "load: trimmed with the default pad, not enlarged");
        QImage big = inkOnTransparent(2000, 1000, QRect(0, 0, 2000, 1000));
        check(writePng(root + QStringLiteral("/big.png"), big), "setup: large logo");
        const QImage shrunk = loadLogo(root + QStringLiteral("/big.png"), QSize(512, 288));
        check(!shrunk.isNull() && shrunk.width() <= 512 && shrunk.height() <= 288 &&
              near(double(shrunk.width()) / shrunk.height(), 2.0), "load: shrunk into the box keeping aspect");
        check(loadLogo(root + QStringLiteral("/appcache/librarycache/600/logo.png")).isNull(), "load: an empty file gives null");
        check(loadLogo(QString()).isNull() && loadLogo(root + QStringLiteral("/missing.png")).isNull(), "load: missing file gives null");
    }

    // ---- aspect fit --------------------------------------------------------------------------------------------
    {
        const QSizeF wide = fitLogo(QSizeF(400, 100), QSizeF(200, 100));
        check(near(wide.width(), 200) && near(wide.height(), 50), "fit: wide logo is width-limited and letterboxed");
        const QSizeF tall = fitLogo(QSizeF(100, 400), QSizeF(200, 100));
        check(near(tall.width(), 25) && near(tall.height(), 100), "fit: tall logo is height-limited");
        const QSizeF small = fitLogo(QSizeF(20, 10), QSizeF(200, 100));
        check(near(small.width(), 200) && near(small.height(), 100), "fit: a small logo fills the box at its own aspect");
        const QSizeF odd = fitLogo(QSizeF(300, 70), QSizeF(193, 167));
        check(near(odd.width() / odd.height(), 300. / 70.) && odd.width() <= 193.01 && odd.height() <= 167.01, "fit: aspect kept inside any box");
        check(fitLogo(QSizeF(0, 10), QSizeF(10, 10)).isEmpty() && fitLogo(QSizeF(10, 10), QSizeF(0, 0)).isEmpty(), "fit: degenerate sizes give an empty size");
        const QSizeF box = tileBox(QSizeF(320, 176));
        check(near(box.width(), 320 * TileWidthShare) && near(box.height(), 176 * TileHeightShare), "tile: the logo box is a share of the card");
        check(box.width() < 320 * 0.95, "tile: narrower than the boxed header, as on the console");
    }

    // ---- square canvas for rows inside a page --------------------------------------------------------------------
    {
        const QImage square = onSquare(inkOnTransparent(300, 100, QRect(0, 0, 300, 100)), 200);
        check(square.size() == QSize(200, 200), "square: canvas is square (never lays out as a wide tile)");
        check(qAlpha(square.pixel(0, 0)) == 0 && qAlpha(square.pixel(100, 100)) > 200, "square: transparent margin, ink in the middle");
        check(qAlpha(square.pixel(100, 5)) == 0 && qAlpha(square.pixel(100, 195)) == 0, "square: the logo is letterboxed, not cropped");
        check(onSquare(QImage()).isNull(), "square: null in, null out");
    }

    std::printf("logo-tiles tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
