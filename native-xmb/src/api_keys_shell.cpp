#include "shell.h"
#include "api_keys.h"
#include "steam_owned.h"
#include "rawg_client.h"

#include "diagnostics.h"

#include <QDebug>

// Settings > Network Settings > API Keys. One row per key the app reads from the user, a page per key with
// Enter Key / Clear Key / where to get one. The key is typed on the XMB keyboard (masked), checked, then written to
// the file the reading code already looks at. It is never shown, logged, dumped or put in a URL: rows carry only the
// status text from apikeys::statusText(), and messages carry only the key's name and the format it must have.
namespace xmb {
namespace {
using apikeys::Id;
using apikeys::Info;

Id idFor(const Info& info) { return info.id; }

// Steam and RAWG keep a one-line file in the key folder; the Spotify Client ID is a field of spotify.json in the data folder.
apikeys::FileRead readStored(const Info& info, const QString& dataDir) {
    return info.id == Id::SpotifyClient ? apikeys::readSpotifyClientId(dataDir)
                                        : apikeys::readKeyFile(apikeys::keyDirectory(), QLatin1String(info.fileName));
}
apikeys::Status statusFor(const Info& info, const QString& dataDir) {
    const auto file = readStored(info, dataDir);
    return apikeys::deriveStatus(idFor(info), qEnvironmentVariable(info.envVar), file.key, file.exists);
}

// A settings row: the glass bullet row class every Settings list uses, with the status on the right.
Entry settingsRow(const QString& id, const QString& title, const QString& help, Action action, const QString& value,
                  const QString& shown) {
    Entry entry{id, title, help, QStringLiteral("qgl/icontex"), QStringLiteral("23n"), action, value};
    entry.themeIcon = QStringLiteral("icon_setting_item");
    entry.fieldValue = shown;
    return entry;
}

QString environmentNote(const Info& info) {
    return QObject::tr("%1 is set in the environment and is used instead of a saved key.").arg(QLatin1String(info.envVar));
}
}  // namespace

void Shell::openApiKeys() {
    if (pages_.size() >= 32) return;
    Page page;
    page.title = QObject::tr("API Keys");
    page.apiKey = QStringLiteral("list");
    pages_.push_back(std::move(page));
    refreshApiKeyPages();
    emit changed();
}

void Shell::openApiKey(const QString& slug) {
    const Info* info = apikeys::infoForSlug(slug);
    if (!info || pages_.size() >= 32) return;
    Page page;
    page.title = QLatin1String(info->title);
    page.apiKey = slug;
    pages_.push_back(std::move(page));
    refreshApiKeyPages();
    emit changed();
}

void Shell::refreshApiKeyPages() {
    for (auto& page : pages_) {
        if (page.apiKey.isEmpty()) continue;
        const QString focused = page.entries.isEmpty() ? QString{} : page.entries.value(page.selection).id;
        page.entries.clear();
        if (page.apiKey == QLatin1String("list")) {
            for (const Id id : {Id::SteamWeb, Id::Rawg, Id::SpotifyClient}) {
                const Info& info = apikeys::info(id);
                page.entries.push_back(settingsRow(QStringLiteral("apikey:") + QLatin1String(info.slug), QLatin1String(info.title),
                    QLatin1String(info.help), Action::ApiKeyPage, QLatin1String(info.slug), apikeys::statusText(statusFor(info, dataDirectory_))));
            }
        } else if (const Info* info = apikeys::infoForSlug(page.apiKey)) {
            const QString rowIdBase = QStringLiteral("apikey");   // row ids read "apikey:<slug>:<row>"
            const QString prefix = rowIdBase + QLatin1Char(':') + page.apiKey + QLatin1Char(':');
            const auto status = statusFor(*info, dataDirectory_);
            QString explanation;
            if (status.source == apikeys::Source::Environment)
                explanation = environmentNote(*info) + QLatin1Char(' ') + QObject::tr("Clear Key only removes the key saved on this PC.");
            else if (status.source == apikeys::Source::File && !status.fileUsable)
                explanation = QObject::tr("A key file is saved on this PC but it is not a valid key (%1). Enter the key again.").arg(QLatin1String(info->format));
            else if (status.source == apikeys::Source::File)
                explanation = QObject::tr("A key is saved on this PC for your Windows user. It is never shown.");
            else if (status.fileSaved)
                explanation = QObject::tr("A key file exists on this PC but holds no usable key. Enter the key again or clear it.");
            else
                explanation = QObject::tr("No key is set. Use Enter Key to add yours.");
            page.entries.push_back(settingsRow(prefix + QStringLiteral("status"), QObject::tr("Status"), explanation, Action::Information,
                QObject::tr("Status") + QStringLiteral("\n\n") + explanation, apikeys::statusText(status)));
            page.entries.push_back(settingsRow(prefix + QStringLiteral("enter"), QObject::tr("Enter Key"),
                info->id == Id::SpotifyClient ? QObject::tr("Cross to type the Client ID. Needs %1.").arg(QLatin1String(info->format))
                                            : QObject::tr("Cross to type the key. It is shown as dots. Needs %1.").arg(QLatin1String(info->format)),
                Action::ApiKeyEnter, page.apiKey, {}));
            auto clear = settingsRow(prefix + QStringLiteral("clear"), QObject::tr("Clear Key"),
                !status.fileSaved ? QObject::tr("There is no saved key to clear.")
                : status.source == apikeys::Source::Environment ? QObject::tr("Removes the key saved on this PC. The environment variable stays in effect.")
                : QObject::tr("Removes the key saved on this PC."),
                Action::ApiKeyClear, page.apiKey, {});
            clear.disabled = !status.fileSaved;
            page.entries.push_back(std::move(clear));
            page.entries.push_back(settingsRow(prefix + QStringLiteral("where"), QObject::tr("Where to Get a Key"), QLatin1String(info->help),
                Action::Information, QObject::tr("Where to Get a Key") + QStringLiteral("\n\n") + QLatin1String(info->help), QLatin1String(info->where)));
            if (info->id == Id::SpotifyClient) {
                const QString uri = spotify_.redirectUri();
                const QString nn = QString(2, QChar(0x0A));
                page.entries.push_back(settingsRow(prefix + QStringLiteral("redirect"), QObject::tr("Redirect URI"),
                    QObject::tr("Register exactly this redirect URI in your Spotify app."), Action::Information,
                    QObject::tr("Redirect URI") + nn + QObject::tr("Register exactly this redirect URI in your Spotify app (Edit Settings, Redirect URIs):") + QChar(0x0A) + uri
                        + nn + QObject::tr("Spotify signs in with PKCE, so only the Client ID is needed. There is no client secret."), uri));
            }
            if (info->id == Id::SteamWeb && !apiKeyResult_.isEmpty())
                page.entries.push_back(settingsRow(prefix + QStringLiteral("result"), QObject::tr("Steam Library"), apiKeyResult_,
                    Action::Information, QObject::tr("Steam Library") + QStringLiteral("\n\n") + apiKeyResult_, {}));
        }
        page.selection = 0;
        for (int i = 0; i < page.entries.size(); ++i)
            if (page.entries[i].id == focused) { page.selection = i; break; }
    }
}

void Shell::editApiKey(const QString& slug) {
    const Info* info = apikeys::infoForSlug(slug);
    if (!info) return;
    // Always an empty, masked editor: the existing key is never handed to the view or its capture diagnostics.
    // A Spotify Client ID is public (it is in the sign-in address), so its editor is plain text; the others are masked.
    emit requestJellyfinFieldEdit(QStringLiteral("apikey:") + slug, QLatin1String(info->title), {}, info->id != Id::SpotifyClient);
}

void Shell::setApiKeyFromField(const QString& slug, const QString& value) {
    const Info* info = apikeys::infoForSlug(slug);
    if (!info) return;
    const QString title = QLatin1String(info->title);
    const QString key = value.trimmed();
    if (key.isEmpty()) {
        showMessage(title, QObject::tr("No key was entered. The saved key is unchanged."), MessageKind::Page);
        emit changed();
        return;
    }
    if (!apikeys::validKey(info->id, key)) {
        interactionAudio_.play(InteractionAudio::Cue::Error);
        qInfo().noquote() << QStringLiteral("apikeys: %1 refused (wrong format, %2 character(s)); nothing saved").arg(QLatin1String(info->slug)).arg(key.size());
        showMessage(title, QObject::tr("That is not a valid %1. It must be %2. Nothing was saved.").arg(title, QLatin1String(info->format)), MessageKind::Page);
        emit changed();
        return;
    }
    QString error;
    bool restricted = false;
    const bool saved = info->id == Id::SpotifyClient ? apikeys::writeSpotifyClientId(dataDirectory_, key, error, &restricted)
                                                     : apikeys::writeKeyFile(apikeys::keyDirectory(), QLatin1String(info->fileName), key, error, &restricted);
    if (!saved) {
        interactionAudio_.play(InteractionAudio::Cue::Error);
        qWarning().noquote() << QStringLiteral("apikeys: %1 could not be saved").arg(QLatin1String(info->slug));
        showMessage(title, error, MessageKind::Page);
        emit changed();
        return;
    }
    qInfo().noquote() << QStringLiteral("apikeys: %1 saved (value masked, not logged; access limited to this Windows user: %2)")
        .arg(QLatin1String(info->slug), restricted ? QStringLiteral("yes") : QStringLiteral("no"));
    interactionAudio_.play(InteractionAudio::Cue::Accept);
    afterApiKeyChanged(slug, true);
}

void Shell::confirmClearApiKey(const QString& slug) {
    const Info* info = apikeys::infoForSlug(slug);
    if (!info) return;
    const QString title = QLatin1String(info->title);
    const auto file = readStored(*info, dataDirectory_);
    if (!file.exists) {
        showMessage(title, QObject::tr("There is no saved key to clear."), MessageKind::Page);
        emit changed();
        return;
    }
    const bool env = statusFor(*info, dataDirectory_).source == apikeys::Source::Environment;
    showConfirm(QObject::tr("Clear %1?").arg(title),
                env ? QObject::tr("This removes the saved %1 from this PC. %2 stays in effect because the environment variable still supplies a key.").arg(title, QLatin1String(info->envVar))
                    : QObject::tr("This removes the saved %1 from this PC. The features that use it stop working until you enter one again.").arg(title),
                [this, slug] {
                    const Info* current = apikeys::infoForSlug(slug);
                    if (!current) return;
                    QString error;
                    const bool cleared = current->id == Id::SpotifyClient ? apikeys::clearSpotifyClientId(dataDirectory_, error)
                                                          : apikeys::clearKeyFile(apikeys::keyDirectory(), QLatin1String(current->fileName), error);
                    if (!cleared) {
                        showMessage(QLatin1String(current->title), error, MessageKind::Page);
                        emit changed();
                        return;
                    }
                    qInfo().noquote() << QStringLiteral("apikeys: %1 cleared").arg(QLatin1String(current->slug));
                    afterApiKeyChanged(slug, false);
                });
    emit changed();
}

// The key just changed (saved or cleared): redraw the status rows and refresh whatever depends on it now.
void Shell::afterApiKeyChanged(const QString& slug, bool saved) {
    const Info* info = apikeys::infoForSlug(slug);
    if (!info) return;
    const bool envKey = !qEnvironmentVariable(info->envVar).trimmed().isEmpty() && (info->id != Id::Rawg || apikeys::validRawgKey(qEnvironmentVariable(info->envVar)));
    QString message;
    if (info->id == Id::SteamWeb) {
        steamOwned_.cancel();
        ownedInFlight_ = false;
        apiKeyAwaitOwned_ = false;
        if (SteamOwned::hasApiKey()) {
            // A key is in effect (the one just saved, or the environment's): ask Valve for the list now.
            apiKeyAwaitOwned_ = true;
            apiKeyResult_ = saved ? QObject::tr("Saved. Loading your Steam library") + QChar(0x2026)
                                  : QObject::tr("Saved key removed. Reloading your Steam library with the environment key") + QChar(0x2026);
            message = apiKeyResult_;
            refreshOwnedGames();
        } else {
            // No key anywhere: the purchased list can no longer be shown, so drop it instead of leaving it stale.
            ownedGames_.clear();
            ownedSignature_.clear();
            collections_.clear();
            apiKeyResult_ = QObject::tr("Key cleared. Your owned-games list is off until a Steam key is set.");
            message = apiKeyResult_;
            refreshContent();
        }
        if (saved && envKey) {
            apiKeyResult_ += QLatin1Char(' ') + environmentNote(*info);
            message = apiKeyResult_;
        }
    } else if (info->id == Id::SpotifyClient) {
        // A stored session belongs to the old Client ID, so end it, then read the configuration again (environment first,
        // then spotify.json) exactly as at start-up and try to restore a session. No restart is needed.
        spotify_.signOut();
        spotifyPlaylists_.clear();
        spotifyNowPlaying_ = {};
        const bool configured = spotify_.configure(dataDirectory_);
        if (configured && !diagnostics::safeMode()) spotify_.restoreSession();
        message = saved ? QObject::tr("Saved. Spotify now uses this Client ID. Open Spotify in the Music column and choose Sign In; no restart is needed.")
                        : configured ? QObject::tr("Saved Client ID removed. %1 still supplies one.").arg(QLatin1String(info->envVar))
                                     : QObject::tr("Client ID cleared. Spotify is off until a Client ID is set.");
        if (saved && envKey) message += QLatin1Char(' ') + environmentNote(*info);
        if (!configured && saved) message = spotify_.configurationError();
    } else {
        retryArtwork();
        message = saved ? QObject::tr("Saved. Missing game artwork will be fetched now.")
                        : QObject::tr("Key cleared. Missing game artwork is no longer fetched from RAWG.");
        if (saved && envKey) message += QLatin1Char(' ') + environmentNote(*info);
        if (!saved && envKey) message = QObject::tr("Saved key removed. %1 still supplies a key.").arg(QLatin1String(info->envVar));
    }
    refreshApiKeyPages();
    showMessage(QLatin1String(info->title), message, MessageKind::Page);
    emit changed();
}


// The owned-list answer for a key that was just saved: say what Valve answered, in the existing words (a rejected
// key, a private profile, an unreachable API), or how many games loaded. Only reached once per save.
void Shell::noteApiKeyOwnedResult(qsizetype gameCount, const QString& error) {
    if (!apiKeyAwaitOwned_) return;
    apiKeyAwaitOwned_ = false;
    const QString loading = apiKeyResult_;
    QString result = error.isEmpty() ? QObject::tr("Loaded %1 owned game(s) from your Steam library.").arg(gameCount) : error;
    if (!qEnvironmentVariable("STEAM_WEB_API_KEY").trimmed().isEmpty())
        result += QLatin1Char(' ') + QObject::tr("STEAM_WEB_API_KEY is set in the environment and is used instead of a saved key.");
    apiKeyResult_ = result;
    qInfo().noquote() << QStringLiteral("apikeys: steam library after key change: %1").arg(error.isEmpty() ? QStringLiteral("%1 game(s)").arg(gameCount) : QStringLiteral("not loaded"));
    refreshApiKeyPages();
    // The "Loading" box is still up: replace its text with the outcome instead of leaving it stale.
    if (!loading.isEmpty() && message_.endsWith(loading)) showMessage(QObject::tr("Steam Web API Key"), result, MessageKind::Page);
    emit changed();
}

}  // namespace xmb
