#include "api_keys.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#endif

namespace xmb::apikeys {
namespace {
#ifdef Q_OS_WIN
// Replace the file's DACL with a single entry: full control for the current user, inheritance off. Returns
// false (leaving the file as it was) when any step fails, so the caller can say the narrowing did not happen.
bool restrictToCurrentUser(const QString& path) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray buffer(int(size), 0);
    const bool ok = size > 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    if (!ok) return false;
    PSID sid = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_ALL_ACCESS;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(sid);
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) return false;
    const std::wstring wide = QDir::toNativeSeparators(path).toStdWString();
    const DWORD result = SetNamedSecurityInfoW(const_cast<LPWSTR>(wide.c_str()), SE_FILE_OBJECT,
                                               DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                               nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    return result == ERROR_SUCCESS;
}
bool replaceFile(const QString& from, const QString& to) {
    return MoveFileExW(QDir::toNativeSeparators(from).toStdWString().c_str(), QDir::toNativeSeparators(to).toStdWString().c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
#else
bool restrictToCurrentUser(const QString& path) {
    return QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}
bool replaceFile(const QString& from, const QString& to) {
    QFile::remove(to);
    return QFile::rename(from, to);
}
#endif
}  // namespace

FileRead readKeyFile(const QString& dir, const QString& fileName) {
    FileRead result;
    if (dir.isEmpty()) return result;
    QFile file(QDir(dir).filePath(fileName));
    if (!file.exists()) return result;
    result.exists = true;
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4096) return result;
    const QString text = QString::fromUtf8(file.readAll()).trimmed();
    const int newline = text.indexOf(QRegularExpression(QStringLiteral("[\\r\\n]")));
    result.key = (newline > 0 ? text.left(newline) : text).trimmed();
    return result;
}

namespace {
bool atomicWrite(const QString& dir, const QString& fileName, const QByteArray& bytes, QString& error, bool* restricted) {
    error.clear();
    if (restricted) *restricted = false;
    if (dir.isEmpty() || fileName.isEmpty() || bytes.isEmpty() || bytes.size() > 4096) {
        error = QStringLiteral("The key could not be saved: there is no folder to keep it in.");
        return false;
    }
    if (!QDir().mkpath(dir)) {
        error = QStringLiteral("The key could not be saved: its folder could not be created.");
        return false;
    }
    const QString target = QDir(dir).filePath(fileName);
    const QString temporary = target + QStringLiteral(".tmp");
    QFile::remove(temporary);
    QFile file(temporary);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        error = QStringLiteral("The key could not be saved: its file could not be created.");
        return false;
    }
    // Narrow access before the key is written, so the key is never in a wider file.
    const bool narrowed = restrictToCurrentUser(temporary);
    const bool written = file.write(bytes) == bytes.size() && file.flush();
    file.close();
    if (!written || !replaceFile(temporary, target)) {
        QFile::remove(temporary);
        error = QStringLiteral("The key could not be saved: writing its file failed. Nothing was changed.");
        return false;
    }
    if (restricted) *restricted = narrowed;
    return true;
}
}  // namespace

bool writeKeyFile(const QString& dir, const QString& fileName, const QString& key, QString& error, bool* restricted) {
    if (key.isEmpty() || key.size() > 256) {
        error = QStringLiteral("The key could not be saved: it is empty or too long.");
        if (restricted) *restricted = false;
        return false;
    }
    return atomicWrite(dir, fileName, key.toUtf8(), error, restricted);
}

bool clearKeyFile(const QString& dir, const QString& fileName, QString& error) {
    error.clear();
    if (dir.isEmpty()) return true;
    const QString target = QDir(dir).filePath(fileName);
    if (!QFileInfo::exists(target)) return true;
    if (!QFile::remove(target)) {
        error = QStringLiteral("The saved key could not be removed. Close anything using the file and try again.");
        return false;
    }
    return true;
}

QByteArray spotifyJsonWithClientId(const QByteArray& existing, const QString& clientId) {
    QJsonObject object = QJsonDocument::fromJson(existing).object();
    if (clientId.isEmpty()) object.remove(QStringLiteral("clientId"));
    else object.insert(QStringLiteral("clientId"), clientId);
    return object.isEmpty() ? QByteArray{} : QJsonDocument(object).toJson(QJsonDocument::Indented);
}

FileRead readSpotifyClientId(const QString& dataDir) {
    FileRead result;
    if (dataDir.isEmpty()) return result;
    QFile file(QDir(dataDir).filePath(QStringLiteral("spotify.json")));
    if (!file.exists()) return result;
    result.exists = true;
    if (file.size() > 4096 || !file.open(QIODevice::ReadOnly)) return result;
    result.key = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("clientId")).toString().trimmed();
    return result;
}

bool writeSpotifyClientId(const QString& dataDir, const QString& clientId, QString& error, bool* restricted) {
    if (clientId.isEmpty() || dataDir.isEmpty()) {
        error = QStringLiteral("The Client ID could not be saved: there is no folder to keep it in.");
        if (restricted) *restricted = false;
        return false;
    }
    QByteArray existing;
    QFile file(QDir(dataDir).filePath(QStringLiteral("spotify.json")));
    if (file.exists() && file.size() <= 4096 && file.open(QIODevice::ReadOnly)) existing = file.readAll();
    file.close();
    return atomicWrite(dataDir, QStringLiteral("spotify.json"), spotifyJsonWithClientId(existing, clientId), error, restricted);
}

bool clearSpotifyClientId(const QString& dataDir, QString& error) {
    error.clear();
    if (dataDir.isEmpty()) return true;
    const QString path = QDir(dataDir).filePath(QStringLiteral("spotify.json"));
    QFile file(path);
    if (!file.exists()) return true;
    QByteArray existing;
    if (file.size() <= 4096 && file.open(QIODevice::ReadOnly)) existing = file.readAll();
    file.close();
    const QByteArray rest = spotifyJsonWithClientId(existing, QString{});
    if (rest.isEmpty()) return clearKeyFile(dataDir, QStringLiteral("spotify.json"), error);   // nothing else in it: remove the file
    return atomicWrite(dataDir, QStringLiteral("spotify.json"), rest, error, nullptr);
}

}  // namespace xmb::apikeys
