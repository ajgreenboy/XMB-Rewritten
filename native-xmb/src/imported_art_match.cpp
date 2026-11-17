#include "imported_art_match.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUrlQuery>

#include <algorithm>
#include <cstdlib>

namespace xmb::importedArt {
namespace {

// Words after which a spelled-out or roman number is a sequel number ("Part Two", "Chapter IX").
const QSet<QString>& numberKeywords() {
    static const QSet<QString> words{QStringLiteral("part"), QStringLiteral("chapter"), QStringLiteral("episode"),
        QStringLiteral("volume"), QStringLiteral("vol"), QStringLiteral("book"), QStringLiteral("act"),
        QStringLiteral("season")};
    return words;
}

QString mapNumber(const QString& token, bool afterKeyword) {
    static const QHash<QString, QString> roman{
        {QStringLiteral("ii"), QStringLiteral("2")}, {QStringLiteral("iii"), QStringLiteral("3")},
        {QStringLiteral("iv"), QStringLiteral("4")}, {QStringLiteral("v"), QStringLiteral("5")},
        {QStringLiteral("vi"), QStringLiteral("6")}, {QStringLiteral("vii"), QStringLiteral("7")},
        {QStringLiteral("viii"), QStringLiteral("8")}, {QStringLiteral("ix"), QStringLiteral("9")},
        {QStringLiteral("xi"), QStringLiteral("11")}, {QStringLiteral("xii"), QStringLiteral("12")},
        {QStringLiteral("xiii"), QStringLiteral("13")}, {QStringLiteral("xiv"), QStringLiteral("14")},
        {QStringLiteral("xv"), QStringLiteral("15")}, {QStringLiteral("xvi"), QStringLiteral("16")}};
    static const QHash<QString, QString> contextual{
        {QStringLiteral("i"), QStringLiteral("1")}, {QStringLiteral("x"), QStringLiteral("10")},
        {QStringLiteral("one"), QStringLiteral("1")}, {QStringLiteral("two"), QStringLiteral("2")},
        {QStringLiteral("three"), QStringLiteral("3")}, {QStringLiteral("four"), QStringLiteral("4")},
        {QStringLiteral("five"), QStringLiteral("5")}, {QStringLiteral("six"), QStringLiteral("6")},
        {QStringLiteral("seven"), QStringLiteral("7")}, {QStringLiteral("eight"), QStringLiteral("8")},
        {QStringLiteral("nine"), QStringLiteral("9")}, {QStringLiteral("ten"), QStringLiteral("10")}};
    if (const auto it = roman.constFind(token); it != roman.constEnd()) return it.value();
    if (afterKeyword) {
        if (const auto it = contextual.constFind(token); it != contextual.constEnd()) return it.value();
    }
    return token;
}

QStringList tokens(const QString& raw) {
    const QString text = raw.normalized(QString::NormalizationForm_D);
    QString cleaned;
    cleaned.reserve(text.size());
    for (const QChar ch : text) {
        const auto category = ch.category();
        if (category == QChar::Mark_NonSpacing || category == QChar::Mark_SpacingCombining ||
            category == QChar::Mark_Enclosing) continue;
        if (ch == QChar(0x2122) || ch == QChar(0x00AE) || ch == QChar(0x00A9)) continue;           // TM, R, C
        if (ch == QLatin1Char('\'') || ch == QChar(0x2019) || ch == QChar(0x2018) || ch == QLatin1Char('`')) continue;
        if (ch == QLatin1Char('&')) { cleaned += QStringLiteral(" and "); continue; }
        cleaned += ch;
    }
    static const QRegularExpression separators(QStringLiteral("[^\\p{L}\\p{N}]+"));
    const QStringList raws = cleaned.toCaseFolded().replace(separators, QStringLiteral(" ")).simplified()
        .split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList out;
    out.reserve(raws.size());
    bool afterKeyword = false;
    for (const QString& token : raws) {
        out.push_back(mapNumber(token, afterKeyword));
        afterKeyword = numberKeywords().contains(token);
    }
    return out;
}

bool isNumber(const QString& token) {
    if (token.isEmpty()) return false;
    return std::all_of(token.begin(), token.end(), [](QChar c) { return c.isDigit(); });
}

bool endsWith(const QStringList& list, const QStringList& tail) {
    if (list.size() < tail.size()) return false;
    for (int i = 0; i < tail.size(); ++i)
        if (list.at(list.size() - tail.size() + i) != tail.at(i)) return false;
    return true;
}

QStringList looseTokens(const QString& title) {
    static const QRegularExpression groups(QStringLiteral(R"(\([^)]*\)|\[[^\]]*\])"));
    QString text = title;
    text.remove(groups);
    QStringList list = tokens(text);
    static const QSet<QString> leading{QStringLiteral("the"), QStringLiteral("a"), QStringLiteral("an")};
    if (list.size() > 1 && leading.contains(list.first())) list.removeFirst();
    static const QSet<QString> editionWords{QStringLiteral("remastered"), QStringLiteral("remaster"),
        QStringLiteral("definitive"), QStringLiteral("edition"), QStringLiteral("enhanced"), QStringLiteral("deluxe"),
        QStringLiteral("complete"), QStringLiteral("ultimate"), QStringLiteral("standard"), QStringLiteral("goty"),
        QStringLiteral("anniversary"), QStringLiteral("gold"), QStringLiteral("premium")};
    static const QStringList yearEdition{QStringLiteral("game"), QStringLiteral("of"), QStringLiteral("the"),
        QStringLiteral("year")};
    static const QStringList directorsCut{QStringLiteral("directors"), QStringLiteral("cut")};
    for (bool changed = true; changed && list.size() > 1;) {
        changed = false;
        if (list.size() > 4 && endsWith(list, yearEdition)) {
            for (int i = 0; i < 4; ++i) list.removeLast();
            changed = true;
        } else if (list.size() > 2 && endsWith(list, directorsCut)) {
            list.removeLast();
            list.removeLast();
            changed = true;
        } else if (editionWords.contains(list.last())) {
            list.removeLast();
            changed = true;
        }
    }
    return list;
}

// A name that carries extra-content words the imported title does not: DLC, demo, soundtrack, upgrade packs.
bool additionalContent(const QString& candidateName, const QSet<QString>& titleTokens) {
    static const QSet<QString> extras{QStringLiteral("demo"), QStringLiteral("dlc"), QStringLiteral("soundtrack"),
        QStringLiteral("ost"), QStringLiteral("pack"), QStringLiteral("upgrade"), QStringLiteral("bonus"),
        QStringLiteral("outfit"), QStringLiteral("outfits"), QStringLiteral("costume"), QStringLiteral("costumes"),
        QStringLiteral("playtest"), QStringLiteral("beta"), QStringLiteral("expansion"), QStringLiteral("pass"),
        QStringLiteral("artbook"), QStringLiteral("bundle"), QStringLiteral("wallpaper"), QStringLiteral("trial"),
        QStringLiteral("skin"), QStringLiteral("skins"), QStringLiteral("content")};
    for (const QString& token : tokens(candidateName))
        if (extras.contains(token) && !titleTokens.contains(token)) return true;
    return false;
}

double jaccard(const QSet<QString>& a, const QSet<QString>& b) {
    if (a.isEmpty() || b.isEmpty()) return 0.0;
    const int common = QSet<QString>(a).intersect(b).size();
    return double(common) / double(a.size() + b.size() - common);
}

QSet<QString> numbersOf(const QSet<QString>& set) {
    QSet<QString> out;
    for (const QString& token : set)
        if (isNumber(token)) out.insert(token);
    return out;
}

Match reject(const QString& why) {
    Match match;
    match.reason = why;
    return match;
}

Match accept(const Candidate& candidate, const QString& reason) {
    Match match;
    match.ok = true;
    match.appId = candidate.appId;
    match.name = candidate.name;
    match.reason = reason;
    return match;
}

QString assetName(Asset asset) {
    switch (asset) {
    case Asset::Header: return QStringLiteral("header");
    case Asset::Hero: return QStringLiteral("hero");
    case Asset::Portrait: return QStringLiteral("portrait");
    }
    return QStringLiteral("header");
}

bool hasUserInfo(const QUrl& url) {
    return !url.userInfo().isEmpty() || url.authority().contains(QLatin1Char('@'));
}

QString validImage(const QString& path) {
    if (path.isEmpty()) return {};
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink() || info.size() <= 0 || info.size() > 8 * 1024 * 1024) return {};
    QImageReader reader(path);
    reader.setDecideFormatFromContent(true);
    const QSize size = reader.size();
    return size.isValid() && size.width() > 0 && size.height() > 0 && size.width() <= 8192 && size.height() <= 8192
        ? path : QString{};
}

} // namespace

QString normalizeTitle(const QString& title) {
    return tokens(title).join(QLatin1Char(' '));
}

QString looseKey(const QString& title) {
    return looseTokens(title).join(QLatin1Char(' '));
}

bool validAppId(const QString& appId) {
    static const QRegularExpression digits(QStringLiteral("^[0-9]{1,12}$"));
    return digits.match(appId).hasMatch() && appId.toULongLong() != 0;
}

QVector<Candidate> parseStoreSearch(const QByteArray& json) {
    QVector<Candidate> out;
    if (json.isEmpty() || json.size() > 4 * 1024 * 1024) return out;
    QJsonParseError error{};
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return out;
    const auto items = document.object().value(QStringLiteral("items")).toArray();
    for (const auto& value : items) {
        const auto object = value.toObject();
        Candidate candidate;
        const auto id = object.value(QStringLiteral("id"));
        candidate.appId = id.isString() ? id.toString().trimmed() : QString::number(qint64(id.toDouble(0)));
        candidate.name = object.value(QStringLiteral("name")).toString().trimmed();
        candidate.type = object.value(QStringLiteral("type")).toString().trimmed().toLower();
        if (!validAppId(candidate.appId) || candidate.name.isEmpty()) continue;
        out.push_back(candidate);
        if (out.size() >= 50) break;
    }
    return out;
}

Match selectMatch(const QString& title, const QVector<Candidate>& candidates) {
    const QString wanted = normalizeTitle(title);
    if (wanted.isEmpty()) return reject(QStringLiteral("empty title"));
    QVector<Candidate> usable;
    QSet<QString> seen;
    for (const Candidate& candidate : candidates) {
        if (!validAppId(candidate.appId) || candidate.name.isEmpty() || seen.contains(candidate.appId)) continue;
        if (!candidate.type.isEmpty() && candidate.type != QLatin1String("app") && candidate.type != QLatin1String("game")) continue;
        seen.insert(candidate.appId);
        usable.push_back(candidate);
    }
    if (usable.isEmpty()) return reject(QStringLiteral("no results"));

    // Tier 1: the same title once normalised.
    QVector<Candidate> exact;
    for (const Candidate& candidate : usable)
        if (normalizeTitle(candidate.name) == wanted) exact.push_back(candidate);
    if (!exact.isEmpty()) {
        Candidate chosen = exact.first();
        QString reason = QStringLiteral("exact");
        if (exact.size() > 1) {
            QVector<Candidate> games;
            for (const Candidate& candidate : exact)
                if (candidate.type == QLatin1String("game")) games.push_back(candidate);
            if (games.size() != 1) return reject(QStringLiteral("ambiguous: %1 exact matches").arg(exact.size()));
            chosen = games.first();
            reason = QStringLiteral("exact (type=game)");
        }
        // A bare title does not say which of "Foo", "Foo (2017)" and "Foo Remake" is installed.
        static const QRegularExpression sibling(QStringLiteral("^(?:19[0-9]{2}|20[0-9]{2}|remake)$"));
        for (const Candidate& candidate : usable) {
            if (candidate.appId == chosen.appId) continue;
            const QString name = normalizeTitle(candidate.name);
            if (name.startsWith(wanted + QLatin1Char(' ')) &&
                sibling.match(name.mid(wanted.size() + 1)).hasMatch())
                return reject(QStringLiteral("ambiguous: \"%1\" is a sibling release").arg(candidate.name));
        }
        return accept(chosen, reason);
    }

    const QStringList titleTokens = tokens(title);
    const QSet<QString> titleSet(titleTokens.begin(), titleTokens.end());

    // Tier 2: equal once articles, bracketed notes and trailing edition words are dropped.
    const QStringList wantedLoose = looseTokens(title);
    if (wantedLoose.isEmpty()) return reject(QStringLiteral("no comparable words"));
    QVector<Candidate> loose;
    for (const Candidate& candidate : usable)
        if (looseTokens(candidate.name) == wantedLoose && !additionalContent(candidate.name, titleSet))
            loose.push_back(candidate);
    if (loose.size() == 1) return accept(loose.first(), QStringLiteral("loose"));
    if (loose.size() > 1) return reject(QStringLiteral("ambiguous: %1 loose matches").arg(loose.size()));

    // Tier 3: one clearly best similar title. Different numbers (sequels) never qualify.
    const QSet<QString> wantedSet(wantedLoose.begin(), wantedLoose.end());
    if (wantedSet.size() < 3) return reject(QStringLiteral("no confident match"));
    const QSet<QString> wantedNumbers = numbersOf(wantedSet);
    double best = 0.0, second = 0.0;
    const Candidate* bestCandidate = nullptr;
    for (const Candidate& candidate : usable) {
        const QStringList list = looseTokens(candidate.name);
        const QSet<QString> set(list.begin(), list.end());
        if (set.size() < 3 || numbersOf(set) != wantedNumbers || additionalContent(candidate.name, titleSet)) continue;
        const double score = jaccard(wantedSet, set);
        if (score > best) { second = best; best = score; bestCandidate = &candidate; }
        else if (score > second) second = score;
    }
    if (bestCandidate && best >= 0.85 && best - second >= 0.15)
        return accept(*bestCandidate, QStringLiteral("similar %1").arg(best, 0, 'f', 2));
    return reject(QStringLiteral("no confident match"));
}

QUrl searchUrl(const QString& title) {
    QString term = title;
    term.remove(QChar(0x2122)).remove(QChar(0x00AE)).remove(QChar(0x00A9));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("term"), term.simplified());
    query.addQueryItem(QStringLiteral("l"), QStringLiteral("en"));
    query.addQueryItem(QStringLiteral("cc"), QStringLiteral("US"));
    QUrl url(QStringLiteral("https://store.steampowered.com/api/storesearch/"));
    url.setQuery(query);
    return url;
}

QUrl assetUrl(const QString& appId, Asset asset) {
    if (!validAppId(appId)) return {};
    const QString file = asset == Asset::Header ? QStringLiteral("header.jpg")
        : asset == Asset::Hero ? QStringLiteral("library_hero.jpg") : QStringLiteral("library_600x900.jpg");
    return QUrl(QStringLiteral("https://shared.steamstatic.com/store_item_assets/steam/apps/%1/%2").arg(appId, file));
}

bool allowedSearchUrl(const QUrl& url) {
    return url.isValid() && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 &&
           url.host().compare(QStringLiteral("store.steampowered.com"), Qt::CaseInsensitive) == 0 &&
           !hasUserInfo(url) && (url.port(-1) == -1 || url.port(-1) == 443);
}

bool allowedImageUrl(const QUrl& url) {
    const QString host = url.host().toLower();
    const bool steamStaticHost = host == QStringLiteral("steamstatic.com") || host.endsWith(QStringLiteral(".steamstatic.com"));
    return url.isValid() && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 &&
           steamStaticHost && !hasUserInfo(url) && (url.port(-1) == -1 || url.port(-1) == 443);
}

QString cacheKey(const QString& title) {
    const QString normal = normalizeTitle(title);
    if (normal.isEmpty()) return {};
    return QString::fromLatin1(QCryptographicHash::hash(normal.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
}

QString cacheDirectory(const QString& cacheRoot) {
    if (cacheRoot.trimmed().isEmpty()) return {};
    return QDir::cleanPath(QDir(cacheRoot).absoluteFilePath(QStringLiteral("imported-art")));
}

QString metaPath(const QString& cacheRoot, const QString& title) {
    const QString directory = cacheDirectory(cacheRoot), key = cacheKey(title);
    if (directory.isEmpty() || key.isEmpty()) return {};
    return QDir(directory).filePath(key + QStringLiteral(".json"));
}

QString assetPath(const QString& cacheRoot, const QString& appId, Asset asset) {
    const QString directory = cacheDirectory(cacheRoot);
    if (directory.isEmpty() || !validAppId(appId)) return {};
    return QDir(directory).filePath(QStringLiteral("%1_%2.image").arg(appId, assetName(asset)));
}

bool saveMeta(const QString& cacheRoot, const Meta& meta) {
    const QString path = metaPath(cacheRoot, meta.title);
    if (path.isEmpty() || !QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QJsonObject object{{QStringLiteral("version"), 1}, {QStringLiteral("title"), meta.title},
        {QStringLiteral("key"), cacheKey(meta.title)}, {QStringLiteral("noMatch"), meta.noMatch}};
    if (meta.noMatch) {
        object.insert(QStringLiteral("retryAfter"), meta.retryAfter);
        object.insert(QStringLiteral("reason"), meta.reason);
    } else {
        if (!validAppId(meta.appId)) return false;
        object.insert(QStringLiteral("appId"), meta.appId);
        object.insert(QStringLiteral("name"), meta.name);
        object.insert(QStringLiteral("reason"), meta.reason);
        object.insert(QStringLiteral("matchedAt"), meta.matchedAt);
        object.insert(QStringLiteral("heroMissing"), meta.heroMissing);
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Indented);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}

Meta loadMeta(const QString& cacheRoot, const QString& title) {
    Meta meta;
    const QString path = metaPath(cacheRoot, title);
    if (path.isEmpty()) return meta;
    QFile file(path);
    const QFileInfo info(file);
    if (!info.isFile() || info.isSymLink() || info.size() > 8192 || !file.open(QIODevice::ReadOnly)) return meta;
    const auto object = QJsonDocument::fromJson(file.read(8192)).object();
    if (object.value(QStringLiteral("key")).toString() != cacheKey(title)) return meta;
    meta.key = cacheKey(title);
    meta.title = object.value(QStringLiteral("title")).toString();
    meta.noMatch = object.value(QStringLiteral("noMatch")).toBool();
    meta.reason = object.value(QStringLiteral("reason")).toString();
    if (meta.noMatch) {
        meta.retryAfter = object.value(QStringLiteral("retryAfter")).toInteger();
        return meta;
    }
    meta.appId = object.value(QStringLiteral("appId")).toString();
    if (!validAppId(meta.appId)) return Meta{};
    meta.name = object.value(QStringLiteral("name")).toString();
    meta.matchedAt = object.value(QStringLiteral("matchedAt")).toInteger();
    meta.heroMissing = object.value(QStringLiteral("heroMissing")).toBool();
    return meta;
}

QString cachedHeader(const QString& cacheRoot, const QString& title) {
    const Meta meta = loadMeta(cacheRoot, title);
    if (meta.key.isEmpty() || meta.noMatch) return {};
    return validImage(assetPath(cacheRoot, meta.appId, Asset::Header));
}

QString cachedHero(const QString& cacheRoot, const QString& title) {
    const Meta meta = loadMeta(cacheRoot, title);
    if (meta.key.isEmpty() || meta.noMatch) return {};
    return validImage(assetPath(cacheRoot, meta.appId, Asset::Hero));
}

bool isGeneratedPlaceholder(const QImage& image) {
    if (image.isNull() || image.width() != 512 || image.height() != 288) return false;
    const auto close = [](QRgb pixel, int r, int g, int b, int tolerance) {
        return std::abs(qRed(pixel) - r) <= tolerance && std::abs(qGreen(pixel) - g) <= tolerance &&
               std::abs(qBlue(pixel) - b) <= tolerance;
    };
    // The accent bar (x 28..34, y 36..252) is flat blue; the corners are the gradient's two ends.
    for (const int y : {50, 100, 150, 200, 240})
        if (!close(image.pixel(31, y), 76, 137, 205, 3)) return false;
    return close(image.pixel(1, 1), 29, 42, 62, 6) && close(image.pixel(510, 286), 9, 12, 18, 6);
}

} // namespace xmb::importedArt
