#pragma once

// Artwork for imported (xmb-manager) games without any API key: map the game's title to a Steam app id through
// Valve's public store search, then use that app's public CDN art. This header is the pure part: title
// normalisation, candidate selection, cache paths and metadata. Nothing in it touches the network
// (imported_art.h is the fetcher).

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

namespace xmb::importedArt {

struct Candidate {
    QString appId;
    QString name;
    QString type;   // storesearch "type": "app" for games, DLC and demos alike; "game" is preferred when present
};

struct Match {
    bool ok = false;
    QString appId;
    QString name;     // the store's own name for the chosen app
    QString reason;   // "exact", "exact (type=game)", "loose", "similar 0.91"; on a rejection, why
};

// Case folded, trademark marks and diacritics removed, "&" -> "and", punctuation -> spaces, roman numerals
// II..XVI -> digits, and after part/chapter/episode/volume/book/act/season the words one..ten and the numerals
// I and X -> digits ("Part Two" and "Part II" both become "part 2").
QString normalizeTitle(const QString& title);
// normalizeTitle() of the title without (...) / [...] groups, a leading article and trailing edition words
// ("remastered", "definitive edition", "game of the year edition", "director's cut", ...). Used only for the
// second and third matching tiers, never to make an ambiguous result unique.
QString looseKey(const QString& title);

QVector<Candidate> parseStoreSearch(const QByteArray& json);
// Conservative choice: an exact normalised match, else a unique loose match, else one clearly best similar title
// (token Jaccard >= 0.85, same numbers, a margin over the runner-up). Anything ambiguous returns ok=false.
Match selectMatch(const QString& title, const QVector<Candidate>& candidates);

// ---- endpoints (allowed hosts only: the store for the search, steamstatic.com for images) -------------
enum class Asset { Header, Hero, Portrait };
QUrl searchUrl(const QString& title);
QUrl assetUrl(const QString& appId, Asset asset);
bool allowedSearchUrl(const QUrl& url);
bool allowedImageUrl(const QUrl& url);
bool validAppId(const QString& appId);

// ---- cache: <cache>/imported-art/<key>.json plus <appId>_header.image / _hero.image / _portrait.image --------
struct Meta {
    QString title;
    QString key;
    QString appId;      // empty with noMatch
    QString name;
    QString reason;
    qint64 matchedAt = 0;
    qint64 retryAfter = 0;   // noMatch only: epoch seconds before which the title is not looked up again
    bool noMatch = false;
    bool heroMissing = false;   // the CDN has no library_hero.jpg for this app
};

QString cacheKey(const QString& title);
QString cacheDirectory(const QString& cacheRoot);   // <cache>/imported-art
QString metaPath(const QString& cacheRoot, const QString& title);
QString assetPath(const QString& cacheRoot, const QString& appId, Asset asset);
bool saveMeta(const QString& cacheRoot, const Meta& meta);
Meta loadMeta(const QString& cacheRoot, const QString& title);   // empty key when absent or damaged
// The cached header / hero of the title's recorded match, or an empty string when there is none.
QString cachedHeader(const QString& cacheRoot, const QString& title);
QString cachedHero(const QString& cacheRoot, const QString& title);

// xmb-manager writes this generated 512x288 card (dark gradient, blue bar, title text) when an import found no art.
bool isGeneratedPlaceholder(const QImage& image);

} // namespace xmb::importedArt
