// Deterministic fixture tests for the RAWG client's game-detail parser and its
// no-key fast-fail path. No network is ever touched: the parser is pure and the
// no-key client check returns before any request is made. The fixture document
// is synthetic (a fictional game) so no real copyrighted description is
// reproduced.

#include "rawg_client.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QTemporaryDir>

#include <cstdio>

#ifndef XMB_RAWG_DETAILS_FIXTURE
#define XMB_RAWG_DETAILS_FIXTURE ""
#endif

namespace {
int checks = 0;
int failures = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}

xmb::RawgGameDetails parse(const char* json) {
    return xmb::parseRawgGameDetails(QJsonDocument::fromJson(json).object());
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    using namespace xmb;

    // 1. Absent / unreadable fields are never fabricated.
    const RawgGameDetails empty = parseRawgGameDetails(QJsonObject());
    check(empty.id == 0, "absent document: id zero");
    check(empty.name.isEmpty(), "absent document: name empty");
    check(empty.description.isEmpty(), "absent document: description empty");
    check(empty.releaseYear == 0, "absent document: release year zero");
    check(empty.developers.isEmpty() && empty.publishers.isEmpty(), "absent document: empty credits");
    check(empty.genres.isEmpty() && empty.platforms.isEmpty(), "absent document: empty genres/platforms");

    // 2. The committed synthetic fixture parses to its exact values.
    const QString fixturePath = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                         : QString::fromLocal8Bit(XMB_RAWG_DETAILS_FIXTURE);
    QFile fixture(fixturePath);
    check(fixture.open(QIODevice::ReadOnly), "fixture file readable");
    if (fixture.isOpen()) {
        const QJsonDocument document = QJsonDocument::fromJson(fixture.readAll());
        check(document.isObject(), "fixture is a JSON object");
        const RawgGameDetails details = parseRawgGameDetails(document.object());
        check(details.id == 3498, "fixture id");
        check(details.name == QStringLiteral("Synthetic Quest"), "fixture name");
        check(details.releaseYear == 2013, "fixture release year");
        check(details.description.startsWith(QStringLiteral("Synthetic Quest is a fictional")),
              "fixture description_raw preferred");
        check(details.developers == (QStringList{QStringLiteral("Fixture Labs"),
                                                 QStringLiteral("Placeholder Interactive")}),
              "fixture developers");
        check(details.publishers == (QStringList{QStringLiteral("Fixture Publishing")}),
              "fixture publishers");
        check(details.genres == (QStringList{QStringLiteral("Action"), QStringLiteral("Adventure")}),
              "fixture genres");
        check(details.platforms == (QStringList{QStringLiteral("PC"), QStringLiteral("PlayStation 5")}),
              "fixture platforms deduplicated");
    }

    // 3. release year: only a real four-digit year is surfaced.
    check(parse(R"({"released":"2013-09-17"})").releaseYear == 2013, "year from full date");
    check(parse(R"({"released":"2013"})").releaseYear == 2013, "year from bare year");
    check(parse(R"({"released":"  2014-02-01"})").releaseYear == 2014, "year trims whitespace");
    check(parse(R"({"released":"TBA"})").releaseYear == 0, "TBA has no year");
    check(parse(R"({"released":null})").releaseYear == 0, "null has no year");
    check(parse(R"({"released":""})").releaseYear == 0, "empty has no year");
    check(parse(R"({"released":"1899-01-01"})").releaseYear == 0, "implausible year rejected");

    // 4. description: description_raw wins; otherwise the HTML description is
    //    stripped to plain text with entities decoded.
    check(parse(R"({"description_raw":"Raw text.","description":"<p>Html.</p>"})").description ==
              QStringLiteral("Raw text."),
          "description_raw preferred over description");
    check(parse(R"({"description":"<p>Hello &amp; goodbye.</p><p>Second line.</p>"})").description ==
              QStringLiteral("Hello & goodbye.\nSecond line."),
          "HTML description stripped to plain text");
    check(parse(R"({"description":"<br>No paragraphs"})").description ==
              QStringLiteral("No paragraphs"),
          "br tag becomes no leading newline after trim");

    // 5. names are bounded, ordered as returned, and de-duplicated case-insensitively.
    check(parse(R"({"id":7,"genres":[{"name":"Action"},{"name":"action"},{"name":"RPG"}]})").genres ==
              (QStringList{QStringLiteral("Action"), QStringLiteral("RPG")}),
          "genre names deduplicated case-insensitively");
    check(parse(R"({"id":7,"platforms":[{"platform":{"name":"PC"}},{"platform":{"name":"PS4"}}]})").platforms ==
              (QStringList{QStringLiteral("PC"), QStringLiteral("PS4")}),
          "platforms read from the nested platform object");

    // 6. id is validated the same way as the search list.
    check(parse(R"({"id":"not-a-number"})").id == 0, "non-numeric id rejected");
    check(parse(R"({"id":0})").id == 0, "zero id rejected");
    check(parse(R"({"id":42})").id == 42, "valid id kept");

    // 7. No-key fallback: fetchDetails fails fast and never reaches the network.
    //    Guarded so a developer machine that happens to have a key file for the
    //    test executable still runs cleanly; CI (clean environment) exercises it.
    qunsetenv("RAWG_API_KEY");
    if (!RawgClient::hasApiKey()) {
        QTemporaryDir temporary;
        check(temporary.isValid(), "temp cache directory");
        RawgClient client(QDir(temporary.path()).filePath(QStringLiteral("cache")));
        bool failed = false;
        QString message;
        QObject::connect(&client, &RawgClient::failed,
                         [&](const QString& safeError) { failed = true; message = safeError; });
        client.fetchDetails(3498);
        check(failed && !message.isEmpty(), "fetchDetails without a key fails fast with a message");
    }

    std::fprintf(stderr, "%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
