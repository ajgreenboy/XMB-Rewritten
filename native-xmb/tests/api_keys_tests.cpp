// API Keys (Settings > Network Settings > API Keys): key-format validation, status derivation (environment / file /
// none), and the atomic write / clear helpers against a caller-supplied directory.
//   xmb-api-keys-tests [work-dir]
// work-dir defaults to ./api-keys-test-work; it is created and removed by the test. Only fake keys are used.
#include "api_keys.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#endif

using namespace xmb::apikeys;

namespace {
int checks = 0, failures = 0;
void check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", label);
    }
}
QString fakeSteamKey() { return QString(32, QLatin1Char('0')); }

#ifdef Q_OS_WIN
// The file's DACL has exactly one entry and does not inherit: only the current user can read it.
bool onlyCurrentUser(const QString& path) {
    const std::wstring wide = QDir::toNativeSeparators(path).toStdWString();
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (GetNamedSecurityInfoW(wide.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr, &descriptor) != ERROR_SUCCESS)
        return false;
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    const bool protectedDacl = GetSecurityDescriptorControl(descriptor, &control, &revision) && (control & SE_DACL_PROTECTED);
    ACL_SIZE_INFORMATION sizes{};
    const bool sized = dacl && GetAclInformation(dacl, &sizes, sizeof(sizes), AclSizeInformation);
    const bool ok = protectedDacl && sized && sizes.AceCount == 1;
    LocalFree(descriptor);
    return ok;
}
#endif
}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QString work = QDir::cleanPath(argc > 1 ? QString::fromLocal8Bit(argv[1]) : QDir::current().filePath(QStringLiteral("api-keys-test-work")));
    QDir(work).removeRecursively();
    QDir().mkpath(work);

    // --- format validation: the readers' own rules -------------------------------------------------------
    check(validSteamWebKey(fakeSteamKey()), "32 zeros is a valid Steam key shape");
    check(validSteamWebKey(QStringLiteral("0123456789abcdefABCDEF0123456789")), "hex digits of either case are accepted");
    check(!validSteamWebKey(QString(31, QLatin1Char('a'))), "31 characters is too short");
    check(!validSteamWebKey(QString(33, QLatin1Char('a'))), "33 characters is too long");
    check(!validSteamWebKey(QString(31, QLatin1Char('a')) + QLatin1Char('g')), "a non-hex character is refused");
    check(!validSteamWebKey(QString(32, QLatin1Char('a')) + QLatin1Char('\n')), "a trailing newline is not part of a key");
    check(!validSteamWebKey(QString{}), "empty is refused");
    check(!validSteamWebKey(QStringLiteral(" ") + QString(31, QLatin1Char('a'))), "leading space is refused (callers trim first)");
    check(validRawgKey(QString(16, QLatin1Char('a'))) && validRawgKey(QString(128, QLatin1Char('A'))), "RAWG accepts 16 to 128");
    check(!validRawgKey(QString(15, QLatin1Char('a'))) && !validRawgKey(QString(129, QLatin1Char('a'))), "RAWG refuses outside 16..128");
    check(validRawgKey(QStringLiteral("abc_DEF-0123456789")) && !validRawgKey(QStringLiteral("abc def 0123456789x")), "RAWG charset");
    check(validKey(Id::SteamWeb, fakeSteamKey()) && !validKey(Id::Rawg, QStringLiteral("short")), "validKey dispatches by id");
    check(infoForSlug(QStringLiteral("steam")) == &info(Id::SteamWeb) && infoForSlug(QStringLiteral("rawg")) == &info(Id::Rawg), "slugs resolve");
    check(infoForSlug(QStringLiteral("nope")) == nullptr, "unknown slug resolves to nothing");
    check(QLatin1String(info(Id::SteamWeb).fileName) == QLatin1String("steam_api_key") && QLatin1String(info(Id::Rawg).fileName) == QLatin1String("rawg_api_key"),
          "file names are the ones the readers use");
    check(QLatin1String(info(Id::SteamWeb).envVar) == QLatin1String("STEAM_WEB_API_KEY") && QLatin1String(info(Id::Rawg).envVar) == QLatin1String("RAWG_API_KEY"),
          "environment variables are the ones the readers use");

    // --- status derivation -------------------------------------------------------------------------------
    const QString key = fakeSteamKey();
    check(deriveStatus(Id::SteamWeb, {}, {}, false).source == Source::None, "nothing anywhere: not set");
    check(statusText(deriveStatus(Id::SteamWeb, {}, {}, false)) == QStringLiteral("Not set"), "status text: Not set");
    check(deriveStatus(Id::SteamWeb, {}, key, true).source == Source::File && statusText(deriveStatus(Id::SteamWeb, {}, key, true)) == QStringLiteral("Set"), "file key: Set");
    check(deriveStatus(Id::SteamWeb, key, {}, false).source == Source::Environment, "environment key: environment");
    check(statusText(deriveStatus(Id::SteamWeb, key, key, true)) == QStringLiteral("Set by environment variable"), "environment wins over the file");
    check(deriveStatus(Id::SteamWeb, key, key, true).fileSaved, "with both, the file still counts as saved (Clear stays available)");
    check(deriveStatus(Id::SteamWeb, QStringLiteral("   "), {}, false).source == Source::None, "a blank environment value is no key");
    check(deriveStatus(Id::SteamWeb, QStringLiteral(" abc "), {}, false).source == Source::Environment, "Steam's reader takes any non-empty environment value");
    check(deriveStatus(Id::Rawg, QStringLiteral("short"), {}, false).source == Source::None, "RAWG's reader ignores an invalid environment value");
    check(deriveStatus(Id::Rawg, QStringLiteral("short"), QString(20, QLatin1Char('a')), true).source == Source::File, "an invalid RAWG environment value falls back to the file");
    const auto badFile = deriveStatus(Id::SteamWeb, {}, QStringLiteral("not-a-key"), true);
    check(badFile.source == Source::File && !badFile.fileUsable && statusText(badFile) == QStringLiteral("Set, not a valid key"), "a hand-written bad file is reported as not valid");
    const auto emptyFile = deriveStatus(Id::SteamWeb, {}, {}, true);
    check(emptyFile.source == Source::None && emptyFile.fileSaved && statusText(emptyFile) != QStringLiteral("Not set"), "an empty file is not 'Not set' and can be cleared");
    for (const QString& text : {statusText(deriveStatus(Id::SteamWeb, key, key, true)), statusText(deriveStatus(Id::SteamWeb, {}, key, true)), statusText(badFile)})
        check(!text.contains(key.left(8)), "status text never contains any part of the key");

    // --- key directory -----------------------------------------------------------------------------------
    qputenv("XMB_API_KEY_DIR", work.toUtf8());
    check(keyDirectory() == work, "XMB_API_KEY_DIR redirects the folder");
    check(keyFilePath(Id::SteamWeb) == QDir(work).filePath(QStringLiteral("steam_api_key")), "key file path is inside the folder");
    qunsetenv("XMB_API_KEY_DIR");
    check(!keyDirectory().isEmpty() && !keyDirectory().startsWith(work), "unset, the folder is the application data folder");

    // --- atomic write / read / clear ---------------------------------------------------------------------
    const QString dir = QDir(work).filePath(QStringLiteral("nested/keys"));
    QString error;
    bool restricted = false;
    check(!readKeyFile(dir, QStringLiteral("steam_api_key")).exists, "nothing to read before the first save");
    check(writeKeyFile(dir, QStringLiteral("steam_api_key"), key, error, &restricted), "write creates the folder and the file");
    check(error.isEmpty(), "no error on success");
    const auto first = readKeyFile(dir, QStringLiteral("steam_api_key"));
    check(first.exists && first.key == key, "the key reads back");
    QFile raw(QDir(dir).filePath(QStringLiteral("steam_api_key")));
    check(raw.open(QIODevice::ReadOnly) && raw.readAll() == key.toUtf8(), "the file holds exactly the key, no newline");
    raw.close();
    check(!QFileInfo::exists(QDir(dir).filePath(QStringLiteral("steam_api_key.tmp"))), "no temporary file is left behind");
#ifdef Q_OS_WIN
    check(restricted && onlyCurrentUser(QDir(dir).filePath(QStringLiteral("steam_api_key"))), "the file's access is limited to the current user (protected DACL, one entry)");
#else
    check(restricted, "access limited to the owner");
#endif
    // Replace: a second save swaps the content in one step.
    const QString second = QStringLiteral("0123456789abcdef0123456789abcdef");
    check(writeKeyFile(dir, QStringLiteral("steam_api_key"), second, error, nullptr), "a second write replaces the file");
    check(readKeyFile(dir, QStringLiteral("steam_api_key")).key == second, "the replacement reads back");
    check(!QFileInfo::exists(QDir(dir).filePath(QStringLiteral("steam_api_key.tmp"))), "still no temporary file");
    // A stale temporary file from an earlier crash does not block a save.
    QFile stale(QDir(dir).filePath(QStringLiteral("steam_api_key.tmp")));
    check(stale.open(QIODevice::WriteOnly) && stale.write("stale") == 5, "plant a stale temporary file");
    stale.close();
    check(writeKeyFile(dir, QStringLiteral("steam_api_key"), key, error, nullptr) && readKeyFile(dir, QStringLiteral("steam_api_key")).key == key, "a stale temporary file is replaced");
    // The reader's rules: first line, trimmed; a hand-written file with a trailing newline reads the same.
    QFile hand(QDir(dir).filePath(QStringLiteral("rawg_api_key")));
    check(hand.open(QIODevice::WriteOnly) && hand.write("  abcdefghijklmnop \r\nsecond line\n") > 0, "hand-written file");
    hand.close();
    check(readKeyFile(dir, QStringLiteral("rawg_api_key")).key == QStringLiteral("abcdefghijklmnop"), "first line, trimmed");
    QFile big(QDir(dir).filePath(QStringLiteral("big")));
    check(big.open(QIODevice::WriteOnly) && big.write(QByteArray(5000, 'a')) == 5000, "oversized file");
    big.close();
    const auto bigRead = readKeyFile(dir, QStringLiteral("big"));
    check(bigRead.exists && bigRead.key.isEmpty(), "a file over 4096 bytes is present but yields no key (as the reader does)");
    // Refusals never put the key in the message.
    check(!writeKeyFile(QString{}, QStringLiteral("steam_api_key"), key, error, nullptr) && !error.isEmpty() && !error.contains(key), "no folder: refused, message has no key");
    check(!writeKeyFile(dir, QStringLiteral("steam_api_key"), QString{}, error, nullptr), "an empty key is refused");
    check(!writeKeyFile(dir, QStringLiteral("steam_api_key"), QString(300, QLatin1Char('a')), error, nullptr), "an oversized key is refused");
    // A folder that cannot be created: a file is in the way.
    QFile blocker(QDir(work).filePath(QStringLiteral("blocker")));
    check(blocker.open(QIODevice::WriteOnly) && blocker.write("x") == 1, "plant a file where a folder is needed");
    blocker.close();
    check(!writeKeyFile(QDir(work).filePath(QStringLiteral("blocker/sub")), QStringLiteral("steam_api_key"), key, error, nullptr) && !error.contains(key), "unwritable folder: refused, no key in the message");
    check(readKeyFile(dir, QStringLiteral("steam_api_key")).key == key, "a refused write leaves the saved key as it was");
    // Clear.
    check(clearKeyFile(dir, QStringLiteral("steam_api_key"), error) && !QFileInfo::exists(QDir(dir).filePath(QStringLiteral("steam_api_key"))), "clear removes the file");
    check(clearKeyFile(dir, QStringLiteral("steam_api_key"), error) && error.isEmpty(), "clearing again succeeds quietly");
    check(clearKeyFile(QString{}, QStringLiteral("steam_api_key"), error), "clearing with no folder succeeds");
    const auto after = readKeyFile(dir, QStringLiteral("steam_api_key"));
    check(!after.exists && deriveStatus(Id::SteamWeb, {}, after.key, after.exists).source == Source::None, "after clear the status is Not set");

    // --- Spotify Client ID (spotify.json in the data folder) ---------------------------------------------
    const QString data = QDir(work).filePath(QStringLiteral("data"));
    const QString clientId = QStringLiteral("0123456789abcdef0123456789abcdef");
    check(validSpotifyClientId(clientId) && !validSpotifyClientId(QStringLiteral("0123")) && validKey(Id::SpotifyClient, clientId), "Spotify Client ID is 32 hex characters");
    check(!validKey(Id::SpotifyClient, QString(32, QLatin1Char('z'))), "a non-hex Client ID is refused");
    check(QLatin1String(info(Id::SpotifyClient).envVar) == QLatin1String("SPOTIFY_CLIENT_ID") && QLatin1String(info(Id::SpotifyClient).fileName) == QLatin1String("spotify.json"), "Spotify env and file match the reader");
    check(infoForSlug(QStringLiteral("spotify")) == &info(Id::SpotifyClient), "spotify slug resolves");
    check(deriveStatus(Id::SpotifyClient, clientId, {}, false).source == Source::Environment, "Spotify: environment wins");
    check(spotifyJsonWithClientId({}, clientId).contains("clientId"), "json for a new file");
    check(spotifyJsonWithClientId("{\"redirectPort\": 9999, \"clientId\": \"old\"}", clientId).contains("9999"), "other fields are kept when the id is set");
    check(spotifyJsonWithClientId("{\"redirectPort\": 9999, \"clientId\": \"old\"}", QString{}).contains("9999") && !spotifyJsonWithClientId("{\"clientId\": \"old\"}", QString{}).contains("clientId"), "clearing removes only the id");
    check(spotifyJsonWithClientId("{\"clientId\": \"old\"}", QString{}).isEmpty(), "nothing left means no file content");
    check(!readSpotifyClientId(data).exists, "no spotify.json yet");
    check(writeSpotifyClientId(data, clientId, error, &restricted), "Spotify Client ID saves");
    check(readSpotifyClientId(data).key == clientId, "Spotify Client ID reads back");
    QFile port(QDir(data).filePath(QStringLiteral("spotify.json")));
    check(port.open(QIODevice::WriteOnly) && port.write("{\"redirectPort\": 9999}") > 0, "a file with only a port");
    port.close();
    check(writeSpotifyClientId(data, clientId, error, nullptr) && readSpotifyClientId(data).key == clientId, "save keeps working over an existing file");
    check(spotifyJsonWithClientId(QByteArray("{\"redirectPort\": 9999}"), clientId).contains("9999"), "port survives a save");
    QFile after2(QDir(data).filePath(QStringLiteral("spotify.json")));
    check(after2.open(QIODevice::ReadOnly) && QString::fromUtf8(after2.readAll()).contains(QStringLiteral("clientId")), "file holds the id");
    after2.close();
    check(clearSpotifyClientId(data, error) && readSpotifyClientId(data).exists && readSpotifyClientId(data).key.isEmpty(), "clearing removes the id and keeps the port");
    QFile(QDir(data).filePath(QStringLiteral("spotify.json"))).remove();
    check(!writeSpotifyClientId(QString{}, clientId, error, nullptr) && !error.contains(clientId), "no folder: refused without the id in the message");
    check(clearSpotifyClientId(data, error), "clearing again is fine");

    QDir(work).removeRecursively();
    std::printf("api-keys tests: %d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
