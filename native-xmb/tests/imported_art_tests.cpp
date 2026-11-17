// Imported-game artwork without a key: title normalisation, conservative selection from canned Steam store search
// JSON, the allowed endpoints, cache paths and metadata, and the manager's generated-card detector.
// Pure logic only: no window and no network (the store is never contacted).
//   xmb-imported-art-tests <scratch dir ending in imported-art>
#include "imported_art_match.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QBrush>
#include <QColor>
#include <QImage>
#include <QPainter>
#include <cstdio>

namespace {
int checks = 0, failures = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}
void checkText(const QString& actual, const QString& expected, const char* label) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n  expected: %s\n  actual:   %s\n", label,
                     qPrintable(expected), qPrintable(actual));
    }
}

// Shapes taken from real storesearch answers (type is "app" for games, demos and DLC alike).
QByteArray searchJson(const QStringList& rows) {
    return QByteArray("{\"total\":") + QByteArray::number(rows.size()) + ",\"items\":[" + rows.join(',').toUtf8() + "]}";
}
QString row(const QString& id, const QString& name, const QString& type = QStringLiteral("app")) {
    QString escaped = name;
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QStringLiteral("{\"type\":\"%1\",\"name\":\"%2\",\"id\":%3,\"price\":{\"currency\":\"USD\",\"initial\":3999,\"final\":3999},"
                          "\"tiny_image\":\"https:\\/\\/shared.akamai.steamstatic.com\\/store_item_assets\\/steam\\/apps\\/%3\\/capsule_231x87.jpg\","
                          "\"platforms\":{\"windows\":true,\"mac\":false,\"linux\":false}}").arg(type, escaped, id);
}

using xmb::importedArt::Candidate;
xmb::importedArt::Match pick(const QString& title, const QStringList& rows) {
    return xmb::importedArt::selectMatch(title, xmb::importedArt::parseStoreSearch(searchJson(rows)));
}

QImage generatedCard() {
    // The same drawing as xmb-manager's writeIcon(): gradient, a flat accent bar, text.
    QImage image(512, 288, QImage::Format_ARGB32);
    image.fill(QColor(10, 14, 21));
    QPainter painter(&image);
    QLinearGradient gradient(0, 0, image.width(), image.height());
    gradient.setColorAt(0.0, QColor(29, 42, 62));
    gradient.setColorAt(1.0, QColor(9, 12, 18));
    painter.fillRect(image.rect(), gradient);
    painter.fillRect(QRect(28, 36, 6, image.height() - 72), QColor(76, 137, 205));
    painter.end();
    return image;
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace xmb::importedArt;
    if (argc < 2) {
        std::fprintf(stderr, "usage: xmb-imported-art-tests <scratch dir>\n");
        return 2;
    }
    const QString scratch = QDir(QString::fromLocal8Bit(argv[1])).absolutePath();
    if (!scratch.endsWith(QLatin1String("imported-art"))) {
        std::fprintf(stderr, "scratch dir name must end in imported-art\n");
        return 2;
    }
    QDir(scratch).removeRecursively();
    QDir().mkpath(scratch);

    // ---- normalisation -------------------------------------------------------------------------------------
    checkText(normalizeTitle(QString::fromUtf8("LEGO\xC2\xAE Batman\xE2\x84\xA2: Legacy of the Dark Knight")),
              QStringLiteral("lego batman legacy of the dark knight"), "normalize: trademark marks, colon and case");
    checkText(normalizeTitle(QStringLiteral("The Last of Us: Part Two Remastered")),
              normalizeTitle(QString::fromUtf8("The Last of Us\xE2\x84\xA2 Part II Remastered")), "normalize: Part Two == Part II");
    checkText(normalizeTitle(QStringLiteral("The Last of Us Part Two")), QStringLiteral("the last of us part 2"), "normalize: Part Two -> part 2");
    checkText(normalizeTitle(QStringLiteral("The Last of Us Part I")), QStringLiteral("the last of us part 1"), "normalize: Part I -> part 1");
    check(normalizeTitle(QStringLiteral("The Last of Us Part I")) != normalizeTitle(QStringLiteral("The Last of Us Part II")),
          "normalize: Part I and Part II stay different");
    checkText(normalizeTitle(QStringLiteral("Final Fantasy VII")), QStringLiteral("final fantasy 7"), "normalize: roman numeral");
    checkText(normalizeTitle(QStringLiteral("Mega Man X")), QStringLiteral("mega man x"), "normalize: a bare X is not ten");
    checkText(normalizeTitle(QStringLiteral("Chapter Ten")), QStringLiteral("chapter 10"), "normalize: spelled number after a keyword");
    checkText(normalizeTitle(QStringLiteral("The One")), QStringLiteral("the one"), "normalize: spelled number elsewhere is a word");
    checkText(normalizeTitle(QString::fromUtf8("Pok\xC3\xA9mon Legends & Co.")), QStringLiteral("pokemon legends and co"),
              "normalize: diacritics and ampersand");
    checkText(normalizeTitle(QString::fromUtf8("Assassin\xE2\x80\x99s Creed")), normalizeTitle(QStringLiteral("Assassin's Creed")),
              "normalize: apostrophes");
    check(normalizeTitle(QStringLiteral("  ")).isEmpty() && normalizeTitle(QString()).isEmpty(), "normalize: blank is empty");
    checkText(looseKey(QStringLiteral("The Last of Us Part II Remastered")), QStringLiteral("last of us part 2"), "loose: article and edition");
    checkText(looseKey(QStringLiteral("Doom (2016) [GOG]")), QStringLiteral("doom"), "loose: bracketed notes");
    checkText(looseKey(QStringLiteral("Foo Game of the Year Edition")), QStringLiteral("foo"), "loose: game of the year edition");
    checkText(looseKey(QStringLiteral("Foo Director's Cut")), QStringLiteral("foo"), "loose: director's cut");
    checkText(looseKey(QStringLiteral("Remastered")), QStringLiteral("remastered"), "loose: never reduced to nothing");
    checkText(looseKey(QStringLiteral("Football Manager 2024")), QStringLiteral("football manager 2024"), "loose: years are kept");

    // ---- parsing -------------------------------------------------------------------------------------------
    {
        const auto items = parseStoreSearch(searchJson({row(QStringLiteral("1522160"), QStringLiteral("Disney Epic Mickey: Rebrushed")),
                                                        row(QStringLiteral("0"), QStringLiteral("Zero id")),
                                                        row(QStringLiteral("5"), QString())}));
        check(items.size() == 1 && items[0].appId == QLatin1String("1522160") && items[0].type == QLatin1String("app"),
              "parse: id, name and type; zero ids and empty names are dropped");
        check(parseStoreSearch("not json").isEmpty() && parseStoreSearch(QByteArray()).isEmpty() &&
              parseStoreSearch("{\"total\":0}").isEmpty() && parseStoreSearch("[1,2]").isEmpty(), "parse: garbage gives nothing");
    }

    // ---- the four titles from the user's library, against the shapes the real store returned ---------------------
    {
        const auto mickey = pick(QStringLiteral("Disney Epic Mickey: Rebrushed"),
            {row(QStringLiteral("1522160"), QStringLiteral("Disney Epic Mickey: Rebrushed")),
             row(QStringLiteral("3043870"), QStringLiteral("Disney Epic Mickey: Rebrushed Demo")),
             row(QStringLiteral("2791920"), QStringLiteral("Disney Epic Mickey: Rebrushed - Costume Pack"))});
        check(mickey.ok && mickey.appId == QLatin1String("1522160") && mickey.reason == QLatin1String("exact"), "mickey: exact, not the demo or the pack");

        const auto lego = pick(QStringLiteral("LEGO Batman: Legacy of the Dark Knight"),
            {row(QStringLiteral("2215200"), QString::fromUtf8("LEGO\xC2\xAE Batman\xE2\x84\xA2: Legacy of the Dark Knight")),
             row(QStringLiteral("4468750"), QString::fromUtf8("LEGO\xC2\xAE Batman\xE2\x84\xA2: Legacy of the Dark Knight Deluxe Edition Upgrade"))});
        check(lego.ok && lego.appId == QLatin1String("2215200") && lego.reason == QLatin1String("exact"), "lego: exact through the trademark marks");

        const auto tlou = pick(QStringLiteral("The Last of Us: Part Two Remastered"),
            {row(QStringLiteral("2531310"), QString::fromUtf8("The Last of Us\xE2\x84\xA2 Part II Remastered"))});
        check(tlou.ok && tlou.appId == QLatin1String("2531310") && tlou.reason == QLatin1String("exact"), "tlou: Part Two matches Part II");

        const auto quarry = pick(QStringLiteral("The Quarry"),
            {row(QStringLiteral("1577120"), QStringLiteral("The Quarry")),
             row(QStringLiteral("2101870"), QString::fromUtf8("The Quarry - \xE2\x80\x98" "50s Throwback Character Outfits")),
             row(QStringLiteral("1891950"), QStringLiteral("The Quarry - Full Game")),
             row(QStringLiteral("1877111"), QString::fromUtf8("The Quarry \xE2\x80\x93 Deluxe Bonus Content Pack")),
             row(QStringLiteral("1877110"), QString::fromUtf8("The Quarry \xE2\x80\x93 Horror History Visual Filter Pack")),
             row(QStringLiteral("528450"), QStringLiteral("Hunt: The Unknown Quarry")),
             row(QStringLiteral("571610"), QStringLiteral("Hunt: The Unknown Quarry - Full Version"))});
        check(quarry.ok && quarry.appId == QLatin1String("1577120"), "quarry: the game, not the packs or Hunt");
    }

    // ---- sequels and editions ------------------------------------------------------------------------------
    {
        const QStringList tlou = {row(QStringLiteral("2531310"), QString::fromUtf8("The Last of Us\xE2\x84\xA2 Part II Remastered"))};
        const auto bare = pick(QStringLiteral("The Last of Us Part Two"), tlou);
        check(bare.ok && bare.appId == QLatin1String("2531310") && bare.reason == QLatin1String("loose"),
              "sequel: a title without Remastered matches the only edition, as loose");
        check(!pick(QStringLiteral("The Last of Us Part I"), tlou).ok, "sequel: Part I never matches Part II Remastered");
        check(!pick(QStringLiteral("Borderlands 2"), {row(QStringLiteral("1"), QStringLiteral("Borderlands 3"))}).ok, "sequel: 2 never matches 3");
        const auto b2 = pick(QStringLiteral("Borderlands 2"), {row(QStringLiteral("1"), QStringLiteral("Borderlands 3")),
                                                               row(QStringLiteral("49520"), QStringLiteral("Borderlands 2"))});
        check(b2.ok && b2.appId == QLatin1String("49520"), "sequel: the exact sequel wins when both are listed");
        check(!pick(QStringLiteral("LEGO Batman: The Videogame"),
                    {row(QStringLiteral("2"), QString::fromUtf8("LEGO\xC2\xAE Batman\xE2\x84\xA2: Legacy of the Dark Knight")),
                     row(QStringLiteral("3"), QString::fromUtf8("LEGO\xC2\xAE Batman\xE2\x84\xA2 2: DC Super Heroes")),
                     row(QStringLiteral("4"), QString::fromUtf8("LEGO\xC2\xAE Batman\xE2\x84\xA2 3: Beyond Gotham"))}).ok,
              "lego: another LEGO Batman never matches");
        check(!pick(QStringLiteral("LEGO Batman 2"), {row(QStringLiteral("3"), QString::fromUtf8("LEGO\xC2\xAE Batman\xE2\x84\xA2 2: DC Super Heroes"))}).ok,
              "lego: a shortened title is not guessed");
        check(!pick(QStringLiteral("Disney Epic Mickey Rebrushed"), {row(QStringLiteral("3043870"), QStringLiteral("Disney Epic Mickey: Rebrushed Demo"))}).ok,
              "dlc: a demo is never the game");
        check(!pick(QStringLiteral("The Quarry"), {row(QStringLiteral("1877111"), QString::fromUtf8("The Quarry \xE2\x80\x93 Deluxe Bonus Content Pack"))}).ok,
              "dlc: a content pack is never the game");
        const auto year = pick(QStringLiteral("Doom (2016)"), {row(QStringLiteral("379720"), QStringLiteral("DOOM")),
                                                              row(QStringLiteral("782330"), QStringLiteral("DOOM Eternal"))});
        check(year.ok && year.appId == QLatin1String("379720") && year.reason == QLatin1String("loose"), "loose: a bracketed year is ignored");
    }

    // ---- ambiguity must be rejected ------------------------------------------------------------------------
    {
        check(!pick(QStringLiteral("Prey"), {row(QStringLiteral("1"), QStringLiteral("Prey")), row(QStringLiteral("2"), QStringLiteral("Prey"))}).ok,
              "ambiguous: two identical names are rejected");
        const auto typed = pick(QStringLiteral("Prey"), {row(QStringLiteral("1"), QStringLiteral("Prey")),
                                                         row(QStringLiteral("2"), QStringLiteral("Prey"), QStringLiteral("game"))});
        check(typed.ok && typed.appId == QLatin1String("2") && typed.reason == QLatin1String("exact (type=game)"),
              "ambiguous: type=game breaks an exact tie");
        check(!pick(QStringLiteral("Prey"), {row(QStringLiteral("1"), QStringLiteral("Prey")), row(QStringLiteral("2"), QStringLiteral("Prey (2006)"))}).ok,
              "ambiguous: a bare title next to a dated sibling is rejected");
        check(!pick(QStringLiteral("Resident Evil 4"), {row(QStringLiteral("1"), QStringLiteral("Resident Evil 4")),
                                                        row(QStringLiteral("2"), QStringLiteral("Resident Evil 4 Remake"))}).ok,
              "ambiguous: a bare title next to a Remake is rejected");
        check(!pick(QStringLiteral("Foo"), {row(QStringLiteral("1"), QStringLiteral("Foo Definitive Edition")),
                                            row(QStringLiteral("2"), QStringLiteral("Foo Remastered"))}).ok,
              "ambiguous: two loose matches are rejected");
        check(!pick(QStringLiteral("Foo"), {}).ok && !pick(QString(), {row(QStringLiteral("1"), QStringLiteral("Foo"))}).ok, "no results or no title");
        check(!pick(QStringLiteral("Foo"), {row(QStringLiteral("1"), QStringLiteral("Foo"), QStringLiteral("sub")),
                                            row(QStringLiteral("2"), QStringLiteral("Foo"), QStringLiteral("bundle"))}).ok,
              "type: packages and bundles are not games");
        const auto first = pick(QStringLiteral("Doom"), {row(QStringLiteral("379720"), QStringLiteral("DOOM")),
                                                         row(QStringLiteral("782330"), QStringLiteral("DOOM Eternal"))});
        check(first.ok && first.appId == QLatin1String("379720"), "unambiguous: DOOM beside DOOM Eternal");
        check(!pick(QStringLiteral("Totally Unrelated"), {row(QStringLiteral("1"), QStringLiteral("Something Else Entirely"))}).ok, "unrelated results");
    }

    // ---- the similarity tier -------------------------------------------------------------------------------
    {
        const auto near = pick(QStringLiteral("Alpha Beta Gamma Delta Epsilon Zeta"),
                               {row(QStringLiteral("10"), QStringLiteral("Alpha Beta Gamma Delta Epsilon Zeta Eta"))});
        check(near.ok && near.appId == QLatin1String("10") && near.reason.startsWith(QLatin1String("similar ")), "similar: six of seven words, one candidate");
        check(!pick(QStringLiteral("Alpha Beta Gamma Delta Epsilon Zeta"),
                    {row(QStringLiteral("10"), QStringLiteral("Alpha Beta Gamma Delta Epsilon Zeta Eta")),
                     row(QStringLiteral("11"), QStringLiteral("Alpha Beta Gamma Delta Epsilon Zeta Theta"))}).ok,
              "similar: two equally close candidates are rejected");
        check(!pick(QStringLiteral("Alpha Beta Gamma Delta Epsilon 2"),
                    {row(QStringLiteral("10"), QStringLiteral("Alpha Beta Gamma Delta Epsilon 3"))}).ok, "similar: different numbers never qualify");
        check(!pick(QStringLiteral("Alpha Beta Gamma Delta"), {row(QStringLiteral("10"), QStringLiteral("Alpha Beta Gamma Delta Epsilon"))}).ok,
              "similar: four of five words is below the threshold");
    }

    // ---- endpoints -----------------------------------------------------------------------------------------
    {
        const QUrl search = searchUrl(QString::fromUtf8("LEGO\xC2\xAE Batman\xE2\x84\xA2: Legacy"));
        check(search.toString().startsWith(QLatin1String("https://store.steampowered.com/api/storesearch/?")), "url: the public store search");
        check(!search.toString().contains(QChar(0x2122)) && search.toString().contains(QLatin1String("cc=US")) &&
              search.toString().contains(QLatin1String("l=en")), "url: marks removed, English/US");
        check(allowedSearchUrl(search) && !allowedSearchUrl(QUrl(QStringLiteral("http://store.steampowered.com/api/storesearch/"))) &&
              !allowedSearchUrl(QUrl(QStringLiteral("https://evil.example/api/storesearch/"))) &&
              !allowedSearchUrl(QUrl(QStringLiteral("https://user@store.steampowered.com/"))), "url: search host is pinned");
        check(assetUrl(QStringLiteral("1522160"), Asset::Header).toString() ==
              QLatin1String("https://shared.steamstatic.com/store_item_assets/steam/apps/1522160/header.jpg"), "url: header");
        check(assetUrl(QStringLiteral("1522160"), Asset::Hero).toString().endsWith(QLatin1String("/1522160/library_hero.jpg")), "url: hero");
        check(assetUrl(QStringLiteral("1522160"), Asset::Portrait).toString().endsWith(QLatin1String("/1522160/library_600x900.jpg")), "url: portrait");
        check(assetUrl(QStringLiteral("../x"), Asset::Header).isEmpty() && assetUrl(QString(), Asset::Header).isEmpty(), "url: app id must be digits");
        check(allowedImageUrl(assetUrl(QStringLiteral("5"), Asset::Header)) &&
              allowedImageUrl(QUrl(QStringLiteral("https://cdn.akamai.steamstatic.com/a.jpg"))) &&
              !allowedImageUrl(QUrl(QStringLiteral("http://shared.steamstatic.com/a.jpg"))) &&
              !allowedImageUrl(QUrl(QStringLiteral("https://steamstatic.com.evil.example/a.jpg"))) &&
              !allowedImageUrl(QUrl(QStringLiteral("https://shared.steamstatic.com:8443/a.jpg"))), "url: images only from steamstatic.com over https");
    }

    // ---- cache paths and metadata --------------------------------------------------------------------------
    {
        const QString title = QString::fromUtf8("Disney Epic Mickey: Rebrushed");
        check(!cacheKey(title).isEmpty() && cacheKey(title) == cacheKey(QStringLiteral("disney epic mickey rebrushed")) &&
              cacheKey(title) != cacheKey(QStringLiteral("The Quarry")) && cacheKey(QString()).isEmpty(), "cache: key follows the normalised title");
        check(cacheDirectory(scratch) == QDir::cleanPath(scratch + QLatin1String("/imported-art")), "cache: under <cache>/imported-art");
        check(metaPath(scratch, title).startsWith(cacheDirectory(scratch)) && metaPath(scratch, title).endsWith(QLatin1String(".json")), "cache: metadata path");
        check(assetPath(scratch, QStringLiteral("1522160"), Asset::Header).endsWith(QLatin1String("/1522160_header.image")) &&
              assetPath(scratch, QStringLiteral("1522160"), Asset::Hero).endsWith(QLatin1String("/1522160_hero.image")), "cache: asset paths");
        check(assetPath(scratch, QStringLiteral("x/../y"), Asset::Header).isEmpty() && assetPath(QString(), QStringLiteral("5"), Asset::Header).isEmpty(),
              "cache: no path from a bad app id or an empty root");

        check(loadMeta(scratch, title).key.isEmpty() && cachedHeader(scratch, title).isEmpty(), "cache: empty before anything is stored");
        Meta meta;
        meta.title = title;
        meta.appId = QStringLiteral("1522160");
        meta.name = QStringLiteral("Disney Epic Mickey: Rebrushed");
        meta.reason = QStringLiteral("exact");
        meta.matchedAt = 1700000000;
        check(saveMeta(scratch, meta), "cache: metadata saves");
        const Meta loaded = loadMeta(scratch, title);
        check(loaded.appId == meta.appId && loaded.name == meta.name && loaded.reason == QLatin1String("exact") &&
              loaded.matchedAt == 1700000000 && !loaded.noMatch && !loaded.heroMissing, "cache: metadata round trip records app id and reason");
        check(cachedHeader(scratch, title).isEmpty(), "cache: a match without an image file has no header");

        QImage header(460, 215, QImage::Format_RGB32);
        header.fill(Qt::darkGreen);
        const QString headerFile = assetPath(scratch, meta.appId, Asset::Header);
        check(header.save(headerFile, "PNG") && cachedHeader(scratch, title) == headerFile, "cache: the stored header is found offline");
        check(cachedHero(scratch, title).isEmpty(), "cache: no hero yet");
        meta.heroMissing = true;
        saveMeta(scratch, meta);
        check(loadMeta(scratch, title).heroMissing, "cache: heroMissing is remembered");
        QFile broken(assetPath(scratch, meta.appId, Asset::Hero));
        check(broken.open(QIODevice::WriteOnly) && broken.write("not an image") > 0, "cache: write a damaged hero");
        broken.close();
        check(cachedHero(scratch, title).isEmpty(), "cache: a damaged image is not used");

        Meta none;
        none.title = QStringLiteral("Some Unknown Game");
        none.noMatch = true;
        none.retryAfter = 1800000000;
        none.reason = QStringLiteral("no confident match");
        check(saveMeta(scratch, none), "cache: a no-match entry saves");
        const Meta noneLoaded = loadMeta(scratch, none.title);
        check(noneLoaded.noMatch && noneLoaded.retryAfter == 1800000000 && noneLoaded.appId.isEmpty() &&
              cachedHeader(scratch, none.title).isEmpty(), "cache: a no-match entry round trips and gives no art");

        Meta bad;
        bad.title = QStringLiteral("Bad");
        bad.appId = QStringLiteral("../evil");
        check(!saveMeta(scratch, bad), "cache: an invalid app id is refused");
        QFile damaged(metaPath(scratch, QStringLiteral("Damaged")));
        QDir().mkpath(cacheDirectory(scratch));
        check(damaged.open(QIODevice::WriteOnly) && damaged.write("{\"key\":\"x\"") > 0, "cache: write damaged metadata");
        damaged.close();
        check(loadMeta(scratch, QStringLiteral("Damaged")).key.isEmpty(), "cache: damaged metadata is ignored");
        QFile foreign(metaPath(scratch, QStringLiteral("Foreign")));
        check(foreign.open(QIODevice::WriteOnly) &&
              foreign.write("{\"key\":\"0123\",\"appId\":\"7\",\"noMatch\":false}") > 0, "cache: write metadata with another key");
        foreign.close();
        check(loadMeta(scratch, QStringLiteral("Foreign")).key.isEmpty(), "cache: metadata for another title is ignored");
    }

    // ---- the manager's generated card ----------------------------------------------------------------------
    {
        check(isGeneratedPlaceholder(generatedCard()), "placeholder: the manager's generated card is recognised");
        QImage real(512, 288, QImage::Format_ARGB32);
        real.fill(QColor(200, 30, 30));
        check(!isGeneratedPlaceholder(real), "placeholder: real art of the same size is not");
        QImage wide(460, 215, QImage::Format_ARGB32);
        wide.fill(QColor(76, 137, 205));
        check(!isGeneratedPlaceholder(wide) && !isGeneratedPlaceholder(QImage()), "placeholder: other sizes and null are not");
    }

    std::printf("imported-art tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
