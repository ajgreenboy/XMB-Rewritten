// Tests for steam_store.h: the SteamStore response parsing (search rows and featuredcategories sections), the
// store-item data it produces (ids, titles, capsule choice, prices, discounts, platforms), the HTTP and body
// failure classes, request building, busy/cancel behaviour and the static hand-off/price helpers.
//
// The class talks to Valve's public store; here it talks to a loopback server (tests/steam_http_fixture.h) through
// the XMB_STEAM_STORE_BASE test seam, which only accepts a plain-http loopback origin. Every response body is a
// synthetic JSON document built in this file; no real store data and no network access beyond 127.0.0.1.

#include "steam_http_fixture.h"
#include "steam_store.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

using namespace xmb;
using steamtest::HttpFixture;
using steamtest::HttpReply;

namespace {

int failures = 0;
int checks = 0;
void check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
void checkText(const QString& got, const QString& want, const char* what) {
    ++checks;
    if (got != want) {
        std::fprintf(stderr, "FAIL: %s\n  want: %s\n  got:  %s\n", what, qPrintable(want), qPrintable(got));
        ++failures;
    }
}

struct Loaded {
    StoreItemList items;
    QString query;
    QString error;
};

struct Probe {
    explicit Probe(SteamStore& store) {
        QObject::connect(&store, &SteamStore::loaded, &store, [this](StoreItemList items, QString query, QString error) {
            results.push_back({items, query, error});
        });
        QObject::connect(&store, &SteamStore::busyChanged, &store, [this](bool busy) { busy_.push_back(busy); });
    }
    QVector<Loaded> results;
    QVector<bool> busy_;
    void reset() { results.clear(); busy_.clear(); }
};

QByteArray json(const QJsonObject& value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); }

QJsonObject row(int id, const QString& name) { return QJsonObject{{"id", id}, {"name", name}}; }

// What the live search endpoint returns, in the shape the parser reads.
QByteArray searchBody(const QJsonArray& items) { return json(QJsonObject{{"total", items.size()}, {"items", items}}); }

bool runOne(Probe& probe, const std::function<void()>& start) {
    probe.reset();
    start();
    return steamtest::waitFor([&] { return !probe.results.isEmpty(); });
}

void parsingTests(HttpFixture& server, SteamStore& store, Probe& probe) {
    QJsonObject full = row(220, "Fixture Half Game");
    full["tiny_image"] = "https://example.invalid/tiny.jpg";
    full["large_capsule_image"] = "https:\\/\\/example.invalid\\/large.jpg";   // doubly escaped: a literal backslash-slash
    full["price"] = QJsonObject{{"currency", "USD"}, {"initial", 1999}, {"final", 499}};
    full["discount_percent"] = 75;
    full["discounted"] = true;
    full["platforms"] = QJsonObject{{"windows", true}, {"mac", false}, {"linux", false}};

    QJsonObject headerOnly = row(221, "Fixture Header Only");
    headerOnly["header_image"] = "https://example.invalid/header.jpg";
    headerOnly["tiny_image"] = "https://example.invalid/tiny2.jpg";
    QJsonObject tinyOnly = row(222, "Fixture Tiny Only");
    tinyOnly["tiny_image"] = "https://example.invalid/tiny3.jpg";
    QJsonObject smallCapsule = row(223, "Fixture Small Capsule");
    smallCapsule["small_capsule_image"] = "https://example.invalid/small.jpg";
    QJsonObject noWindows = row(224, "Fixture Mac Only");
    noWindows["platforms"] = QJsonObject{{"windows", false}, {"mac", true}};
    QJsonObject freeRow = row(225, "Fixture Free");
    freeRow["price"] = QJsonObject{{"currency", "EUR"}, {"initial", 0}, {"final", 0}};
    QJsonObject stringId{{"id", "226"}, {"title", "Fixture String Id"}, {"windows_available", false},
                         {"original_price", 3000}, {"final_price", 1500}, {"currency", "GBP"}, {"discount_percent", "50"}};
    QJsonObject appidKey{{"appid", 227}, {"name", "  Fixture Trimmed  "}};
    QJsonObject noPrice = row(228, "Fixture No Price");

    const QJsonArray rows{full, headerOnly, tinyOnly, smallCapsule, noWindows, freeRow, stringId, appidKey, noPrice,
                          row(220, "Fixture Duplicate Id"),            // duplicate id: first wins
                          row(0, "Fixture Zero Id"),                   // id 0: dropped
                          QJsonObject{{"id", 229}},                    // no title: dropped
                          QJsonObject{{"id", 230}, {"name", "   "}},   // blank title: dropped
                          42, "not an object", QJsonValue()};          // non-object rows are skipped
    server.setHandler([&](const QByteArray&, const QByteArray&) { return HttpReply{200, searchBody(rows)}; });
    check(runOne(probe, [&] { store.search("Fixture"); }), "search: loaded() fires");
    if (probe.results.isEmpty()) return;
    const Loaded& r = probe.results.first();
    check(r.error.isEmpty(), "search: no error for a valid response");
    check(probe.results.size() == 1, "search: loaded() fires exactly once");
    checkText(r.query, QString::fromUtf8("Search \xE2\x80\x9C" "Fixture\xE2\x80\x9D"), "search: the label names the term in curly quotes");
    QStringList ids;
    for (const StoreItem& i : r.items) ids << i.appId;
    check(ids == QStringList({"220", "221", "222", "223", "224", "225", "226", "227", "228"}), "search: usable rows keep their order; id 0, missing/blank titles, non-objects are dropped");
    check(r.items.first().title == "Fixture Half Game", "search: a duplicate id is dropped, the first row wins");
    if (r.items.size() < 9) return;

    const StoreItem& a = r.items[0];
    check(a.currency == "USD" && a.originalPriceCents == 1999 && a.finalPriceCents == 499 && a.discountPercent == 75 && a.discounted, "item: price object, discount and flag are read");
    checkText(a.imageUrl, "https://example.invalid/large.jpg", "item: large capsule wins and escaped slashes are restored");
    check(a.windows, "item: platforms.windows true");
    checkText(SteamStore::priceText(a), "$4.99 (-75%)", "item: discounted USD price text");
    checkText(r.items[1].imageUrl, "https://example.invalid/header.jpg", "item: header_image beats tiny_image");
    checkText(r.items[2].imageUrl, "https://example.invalid/tiny3.jpg", "item: tiny_image is used when nothing better exists");
    checkText(r.items[3].imageUrl, "https://example.invalid/small.jpg", "item: small capsule is the last resort");
    check(!r.items[4].windows, "item: platforms.windows false");
    check(r.items[5].finalPriceCents == 0 && r.items[5].currency == "EUR", "item: a zero price is kept (free)");
    checkText(SteamStore::priceText(r.items[5]), "Free", "item: free text");
    const StoreItem& s = r.items[6];
    check(s.appId == "226" && s.title == "Fixture String Id" && s.originalPriceCents == 3000 && s.finalPriceCents == 1500 &&
              s.currency == "GBP" && s.discountPercent == 50 && !s.windows,
          "item: string ids, title, original_price/final_price, string discount and windows_available are read");
    checkText(SteamStore::priceText(s), "15.00 GBP", "item: non-USD price carries its currency code (not flagged discounted)");
    checkText(r.items[7].title, "Fixture Trimmed", "item: titles are trimmed and appid is accepted as the id key");
    check(r.items[8].finalPriceCents == -1 && r.items[8].originalPriceCents == -1 && SteamStore::priceText(r.items[8]).isEmpty(), "item: no price at all means -1 and empty price text");
    check(r.items[8].windows, "item: windows defaults to true when the store says nothing");

    // Request building.
    const QByteArray line = server.lastRequestLine();
    check(line.startsWith("GET /api/storesearch/?") && line.contains("term=Fixture") && line.contains("l=en") && line.contains("cc=US") && !line.contains("page="), "request: storesearch with term, l=en, cc=US and no page for page 1");
    check(server.lastHeaders().toLower().contains("accept: application/json") && server.lastHeaders().toLower().contains("user-agent: xmb-native"), "request: JSON Accept header and the xmb-native user agent");
    runOne(probe, [&] { store.search("  Half Life & Co? 100%  ", 3); });
    const QByteArray line2 = server.lastRequestLine();
    check(line2.contains("term=Half%20Life%20%26%20Co?%20100%25") && line2.contains("page=3"), "request: the term is trimmed and percent-encoded (a literal ? is legal in a query and stays); page 3 is sent");
    check(probe.busy_ == QVector<bool>({true, false}), "busyChanged: true then false once per request");

    // Row cap: 80 usable rows yield at most 60.
    QJsonArray many;
    for (int i = 0; i < 80; ++i) many.append(row(1000 + i, QString("Fixture Many %1").arg(i)));
    server.setHandler([&](const QByteArray&, const QByteArray&) { return HttpReply{200, searchBody(many)}; });
    runOne(probe, [&] { store.search("many"); });
    check(probe.results.first().items.size() == 60 && probe.results.first().items.last().appId == "1059", "the list is capped at 60 rows");
}

void featuredTests(HttpFixture& server, SteamStore& store, Probe& probe) {
    const QJsonObject body{
        {"top_sellers", QJsonObject{{"id", "cat_topsellers"}, {"name", "Top Sellers"},
                                    {"items", QJsonArray{QJsonObject{{"id", 301}, {"name", "Fixture Seller"}, {"discounted", false}, {"discount_percent", 0},
                                                                    {"original_price", 2999}, {"final_price", 2999}, {"currency", "USD"},
                                                                    {"large_capsule_image", "https://example.invalid/seller.jpg"}, {"windows_available", true}},
                                                         QJsonObject{{"id", 302}, {"name", "Fixture Second Seller"}}}}}},
        {"specials", QJsonObject{{"items", QJsonArray{QJsonObject{{"id", 401}, {"name", "Fixture Special"}, {"discounted", true},
                                                                  {"discount_percent", 60}, {"original_price", 5000}, {"final_price", 2000}, {"currency", "USD"}}}}}},
        {"new_releases", QJsonObject{{"items", QJsonArray{}}}},
    };
    server.setHandler([&](const QByteArray&, const QByteArray&) { return HttpReply{200, json(body)}; });
    check(runOne(probe, [&] { store.topSellers(); }), "featured: topSellers() loads");
    Loaded r = probe.results.first();
    check(server.lastRequestLine().startsWith("GET /api/featuredcategories?cc=US&l=en"), "featured: request line is featuredcategories?cc=US&l=en");
    check(r.error.isEmpty() && r.items.size() == 2 && r.items[0].appId == "301" && r.items[1].appId == "302", "featured: the top_sellers items are listed");
    checkText(r.query, "Steam Top Sellers", "featured: label for top sellers");
    checkText(SteamStore::priceText(r.items[0]), "$29.99", "featured: undiscounted USD price text");
    checkText(r.items[0].imageUrl, "https://example.invalid/seller.jpg", "featured: capsule image is read");

    runOne(probe, [&] { store.featured("specials"); });
    r = probe.results.first();
    check(r.items.size() == 1 && r.items[0].appId == "401" && r.items[0].discounted && r.items[0].discountPercent == 60, "featured: the specials section is read");
    checkText(r.query, "Steam Specials", "featured: label for specials");
    checkText(SteamStore::priceText(r.items[0]), "$20.00 (-60%)", "featured: discounted price text uses the store's percent");

    runOne(probe, [&] { store.featured("bogus_section"); });
    r = probe.results.first();
    check(r.items.size() == 2 && r.query == "Steam Top Sellers", "featured: an unknown section falls back to top sellers");

    runOne(probe, [&] { store.featured("new_releases"); });
    r = probe.results.first();
    check(r.items.isEmpty() && r.error.contains("listed nothing"), "featured: an empty section reports 'listed nothing', not an invented list");
    check(r.query == "Steam New Releases", "featured: label for new releases");

    server.setHandler([&](const QByteArray&, const QByteArray&) { return HttpReply{200, json(QJsonObject{{"coming_soon", QJsonObject{}}})}; });
    runOne(probe, [&] { store.featured("specials"); });
    check(probe.results.first().items.isEmpty() && probe.results.first().error.contains("listed nothing"), "featured: a response without the section is 'listed nothing'");
}

void failureTests(HttpFixture& server, SteamStore& store, Probe& probe) {
    // Empty search term: answered locally, no request, no busy flash.
    const int before = server.requestCount();
    probe.reset();
    store.search("   ");
    check(probe.results.size() == 1 && probe.results[0].items.isEmpty() && probe.results[0].error.contains("Enter a title"), "an empty term is answered locally");
    check(server.requestCount() == before && probe.busy_.isEmpty(), "an empty term sends no request");

    server.setHandler([&](const QByteArray&, const QByteArray&) { return HttpReply{500, "{}"}; });
    runOne(probe, [&] { store.search("x"); });
    check(probe.results.first().items.isEmpty() && !probe.results.first().error.isEmpty() && probe.results.first().query.contains("Search"), "HTTP 500: empty items, a message, and the label is kept");
    server.setHandler([&](const QByteArray&, const QByteArray&) { return HttpReply{404, "{}"}; });
    runOne(probe, [&] { store.search("x"); });
    check(probe.results.first().items.isEmpty() && !probe.results.first().error.isEmpty(), "HTTP 404 is a failure");
    server.setHandler([&](const QByteArray&, const QByteArray&) { return HttpReply{429, "{}"}; });
    runOne(probe, [&] { store.featured("specials"); });
    check(probe.results.first().items.isEmpty() && !probe.results.first().error.isEmpty() && probe.results.first().query == "Steam Specials", "HTTP 429 on a featured request fails with the section label");

    server.setHandler([&](const QByteArray&, const QByteArray&) { return HttpReply{200, "<html>maintenance</html>", "text/html"}; });
    runOne(probe, [&] { store.search("x"); });
    check(probe.results.first().error.contains("unexpected response") && probe.results.first().items.isEmpty(), "an HTML body is 'unexpected response'");
    server.setHandler([&](const QByteArray&, const QByteArray&) { return HttpReply{200, "[1,2,3]"}; });
    runOne(probe, [&] { store.search("x"); });
    check(probe.results.first().error.contains("unexpected response"), "a JSON array root is 'unexpected response'");
    server.setHandler([&](const QByteArray&, const QByteArray&) { return HttpReply{200, "{\"items\":[]}"}; });
    runOne(probe, [&] { store.search("x"); });
    check(probe.results.first().items.isEmpty() && probe.results.first().error.contains("listed nothing"), "no rows is 'listed nothing'");
    server.setHandler([&](const QByteArray&, const QByteArray&) { return HttpReply{200, "{\"items\":[{\"id\":0,\"name\":\"Fixture Zero\"}]}"}; });
    runOne(probe, [&] { store.search("x"); });
    check(probe.results.first().items.isEmpty() && !probe.results.first().error.isEmpty(), "only unusable rows is also 'listed nothing'");

    // A body over 4 MiB is refused before parsing.
    const QByteArray huge = "{\"items\":[],\"pad\":\"" + QByteArray(4 * 1024 * 1024 + 1024, 'a') + "\"}";
    server.setHandler([&](const QByteArray&, const QByteArray&) { return HttpReply{200, huge}; });
    runOne(probe, [&] { store.search("x"); });
    check(probe.results.size() == 1 && probe.results.first().error.contains("larger than expected") && probe.results.first().items.isEmpty(), "a response over 4 MiB fails once with 'larger than expected'");
    steamtest::settle();
    check(probe.results.size() == 1, "the oversize failure is not reported a second time");
}

void cancelTests(HttpFixture& server, SteamStore& store, Probe& probe) {
    // cancel() while a request is held open: busy true then false, never a loaded().
    server.setHandler([&](const QByteArray&, const QByteArray&) { HttpReply r; r.neverAnswer = true; return r; });
    probe.reset();
    const int before = server.requestCount();
    store.search("held");
    check(steamtest::waitFor([&] { return server.requestCount() > before; }), "the held request reaches the server");
    store.cancel();
    server.closeHeldConnections();
    steamtest::settle();
    check(probe.results.isEmpty(), "cancel(): no loaded() for the abandoned query");
    check(probe.busy_ == QVector<bool>({true, false}), "cancel(): busyChanged(false) is emitted");
    store.cancel();
    check(probe.busy_.size() == 2, "cancel() with nothing in flight emits nothing");

    // A newer request supersedes an older one that is still open: one loaded(), for the newer query.
    int served = 0;
    server.setHandler([&](const QByteArray& line, const QByteArray&) {
        ++served;
        if (line.contains("term=first")) { HttpReply r; r.neverAnswer = true; return r; }
        return HttpReply{200, searchBody(QJsonArray{row(501, "Fixture Newer")})};
    });
    probe.reset();
    store.search("first");
    steamtest::waitFor([&] { return served >= 1; });
    store.search("second");
    check(steamtest::waitFor([&] { return !probe.results.isEmpty(); }), "the newer query answers");
    server.closeHeldConnections();
    steamtest::settle();
    check(probe.results.size() == 1 && probe.results[0].items.size() == 1 && probe.results[0].items[0].appId == "501", "a superseded request never emits");
    check(!probe.busy_.isEmpty() && probe.busy_.last() == false, "busy ends false after a superseding request");

    // Unreachable server: the connection is refused.
    SteamStore other;
    Probe otherProbe(other);
    QByteArray deadOrigin;
    {
        HttpFixture temporary;
        temporary.listen();
        deadOrigin = temporary.origin();   // the server closes when `temporary` goes out of scope
    }
    qputenv("XMB_STEAM_STORE_BASE", deadOrigin);
    other.search("x");
    check(steamtest::waitFor([&] { return !otherProbe.results.isEmpty(); }) && otherProbe.results[0].items.isEmpty() && !otherProbe.results[0].error.isEmpty(),
          "a refused connection is an error with no items");
    qputenv("XMB_STEAM_STORE_BASE", server.origin());
}

void helperTests() {
    checkText(SteamStore::installUrl("220"), "steam://install/220", "installUrl");
    checkText(SteamStore::installUrl("  220 "), "steam://install/220", "installUrl trims");
    check(SteamStore::installUrl("").isEmpty() && SteamStore::installUrl("0").isEmpty() && SteamStore::installUrl("-5").isEmpty() &&
              SteamStore::installUrl("abc").isEmpty(),
          "installUrl refuses empty, zero, negative and non-numeric ids");
    checkText(SteamStore::storeUrl("220"), "https://store.steampowered.com/app/220", "storeUrl");
    check(SteamStore::storeUrl("").isEmpty() && SteamStore::storeUrl("0").isEmpty() && SteamStore::storeUrl("x").isEmpty(), "storeUrl refuses bad ids");

    StoreItem item;
    check(SteamStore::priceText(item).isEmpty(), "a default item (price -1) has no price text");
    item.finalPriceCents = 999;
    item.currency = "USD";
    checkText(SteamStore::priceText(item), "$9.99", "USD price");
    item.currency = "usd";
    checkText(SteamStore::priceText(item), "$9.99", "currency match is case-insensitive");
    item.currency = "EUR";
    checkText(SteamStore::priceText(item), "9.99 EUR", "other currencies get a code suffix");
    item.currency.clear();
    checkText(SteamStore::priceText(item), "9.99", "no currency: bare amount");
    item.currency = "USD";
    item.discounted = true;
    item.originalPriceCents = 999;
    checkText(SteamStore::priceText(item), "$9.99", "discounted flag with original == final shows no discount");
    item.originalPriceCents = 2000;
    item.finalPriceCents = 500;
    item.discountPercent = 0;
    checkText(SteamStore::priceText(item), "$5.00 (-75%)", "the percent is computed when the store sends none");
    item.discountPercent = 70;
    checkText(SteamStore::priceText(item), "$5.00 (-70%)", "the store's percent wins when present");
    item.discounted = false;
    checkText(SteamStore::priceText(item), "$5.00", "an undiscounted flag suppresses the suffix");
    item.finalPriceCents = 0;
    checkText(SteamStore::priceText(item), "Free", "final price 0 is Free");
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    HttpFixture server;
    if (!server.listen()) {
        std::fprintf(stderr, "cannot listen on the loopback interface\n");
        return 2;
    }
    qputenv("XMB_STEAM_STORE_BASE", server.origin());
    SteamStore store;
    Probe probe(store);
    parsingTests(server, store, probe);
    featuredTests(server, store, probe);
    failureTests(server, store, probe);
    cancelTests(server, store, probe);
    helperTests();
    std::printf("steam store tests: %d check(s), %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
