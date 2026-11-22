#include "credential_store.h"

#include <QDebug>
#include <QHash>
#include <atomic>
#include <mutex>
#include <string>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dpapi.h>
#pragma comment(lib, "crypt32.lib")
#endif

#if defined(Q_OS_LINUX)
// glib's GDBus headers declare a struct member named `signals`, which Qt's `signals` macro mangles.
// Undefine the Qt keywords before pulling in glib; this file uses neither. (libsecret is included
// after the Qt headers, so the Qt keywords are already defined by the time this runs.)
#undef signals
#undef slots
#undef emit
#include <libsecret/secret.h>
#endif

namespace xmb {
namespace {

// ---- test seam (backend choice) ------------------------------------------------------------------
// 0 = Auto (production), 1 = Keyring (in-memory stand-in), 2 = Plaintext (clearly-marked fallback).
std::atomic<int> backendState{0};

CredentialStoreBackend currentBackend() {
    return static_cast<CredentialStoreBackend>(backendState.load());
}

// ---- sealed-blob markers (Linux) -----------------------------------------------------------------
// The sealed bytes the caller persists start with one of these, so unseal knows how to reverse them.
// A keyring-sealed blob carries only the marker (the value lives in the Secret Service under
// recordKey); a plaintext blob carries the marker followed by the value itself.
const QByteArray KeyringMarker = QByteArrayLiteral("XMB-SECRET-SERVICE-1\n");
const QByteArray PlaintextMarker = QByteArrayLiteral("XMB-PLAINTEXT-1\n");

#ifdef Q_OS_LINUX
// ---- libsecret (Secret Service) backend ----------------------------------------------------------
// A stable schema with one string attribute: the record key. libsecret values are UTF-8 strings, so
// arbitrary binary plaintext is base64-encoded before it is handed to the keyring and decoded after.
const SecretSchema kCredentialSchema = {
    "org.xmb.Credential", SECRET_SCHEMA_NONE,
    {
        { "record", SECRET_SCHEMA_ATTRIBUTE_STRING },
        { nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING }
    }
};

// The D-Bus object path of an unlocked Secret Service collection we can store into, or empty when the
// service is unreachable or has no unlocked collection. Probed once and cached so a headless machine
// (or a locked keyring) fails over to the plaintext fallback immediately. We deliberately resolve an
// unlocked collection ourselves instead of using SECRET_COLLECTION_DEFAULT, because that alias prompts
// (via the GUI) to create a collection when none exists — which blocks forever headless.
std::atomic<int> collectionProbed{-1};
QString unlockedCollectionPath;
std::mutex collectionMutex;

QString usableCollection() {
    std::lock_guard<std::mutex> guard(collectionMutex);
    if (collectionProbed.load() >= 0) return unlockedCollectionPath;
    collectionProbed.store(1);
    GError* error = nullptr;
    SecretService* service = secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &error);
    if (error) { g_error_free(error); error = nullptr; }
    if (service) {
        if (secret_service_load_collections_sync(service, nullptr, &error)) {
            GList* collections = secret_service_get_collections(service);
            for (GList* it = collections; it; it = it->next) {
                SecretCollection* collection = SECRET_COLLECTION(it->data);
                if (!secret_collection_get_locked(collection)) {
                    unlockedCollectionPath = QString::fromUtf8(g_dbus_proxy_get_object_path(G_DBUS_PROXY(collection)));
                    break;
                }
            }
            g_list_free(collections);
        }
        if (error) { g_error_free(error); error = nullptr; }
        g_object_unref(service);
    }
    return unlockedCollectionPath;
}

bool libsecretStore(const QByteArray& key, const QByteArray& value) {
    const QByteArray collection = usableCollection().toUtf8();
    if (collection.isEmpty()) return false;
    GError* error = nullptr;
    const gboolean ok = secret_password_store_sync(
        &kCredentialSchema, collection.constData(), "XMB credential",
        value.constData(), nullptr, &error,
        "record", key.constData(), nullptr);
    if (error) {
        qWarning().noquote() << "credential_store: libsecret store failed:"
                             << QString::fromUtf8(error->message);
        g_error_free(error);
    }
    return ok;
}

QByteArray libsecretLookup(const QByteArray& key) {
    if (usableCollection().isEmpty()) return {};
    GError* error = nullptr;
    gchar* secret = secret_password_lookup_sync(&kCredentialSchema, nullptr, &error,
                                                "record", key.constData(), nullptr);
    if (error) {
        qWarning().noquote() << "credential_store: libsecret lookup failed:"
                             << QString::fromUtf8(error->message);
        g_error_free(error);
    }
    if (!secret) return {};
    QByteArray result(secret);
    secret_password_free(secret);
    return result;
}
#else
// Non-Linux, non-Windows: no Secret Service binding. The keyring path is never reachable, so every
// seal falls through to the clearly-marked plaintext fallback.
bool libsecretStore(const QByteArray&, const QByteArray&) { return false; }
QByteArray libsecretLookup(const QByteArray&) { return {}; }
#endif

// ---- in-memory keyring stand-in (test-only) ------------------------------------------------------
// Backs the Keyring test backend so a unit test can exercise the marker + base64 round-trip without a
// real Secret Service. Mirrors the libsecret contract (UTF-8 base64 values keyed by the record).
QHash<QByteArray, QByteArray>& fakeKeyring() {
    static QHash<QByteArray, QByteArray> store;
    return store;
}

std::mutex fakeKeyringMutex;

bool fakeKeyringStore(const QByteArray& key, const QByteArray& value) {
    std::lock_guard<std::mutex> guard(fakeKeyringMutex);
    fakeKeyring()[key] = value;
    return true;
}

QByteArray fakeKeyringLookup(const QByteArray& key) {
    std::lock_guard<std::mutex> guard(fakeKeyringMutex);
    return fakeKeyring().value(key);
}

bool keyringStore(const QByteArray& key, const QByteArray& value) {
    if (currentBackend() == CredentialStoreBackend::Auto) return libsecretStore(key, value);
    return fakeKeyringStore(key, value);
}

QByteArray keyringLookup(const QByteArray& key) {
    if (currentBackend() == CredentialStoreBackend::Auto) return libsecretLookup(key);
    return fakeKeyringLookup(key);
}

} // namespace

void setCredentialStoreBackendForTest(CredentialStoreBackend backend) {
    backendState.store(static_cast<int>(backend));
    qInfo().noquote() << "credential_store: backend forced to"
                      << (backend == CredentialStoreBackend::Auto ? "auto"
                          : backend == CredentialStoreBackend::Keyring ? "keyring"
                                                                       : "plaintext");
}

#ifdef Q_OS_WIN
QByteArray sealCredential(const QString& recordKey, const QByteArray& plain, const QString& description) {
    const QByteArray entropy = recordKey.toUtf8();
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.constData()))};
    DATA_BLOB extra{static_cast<DWORD>(entropy.size()), reinterpret_cast<BYTE*>(const_cast<char*>(entropy.constData()))};
    DATA_BLOB out{0, nullptr};
    const std::wstring desc = description.toStdWString();
    if (!CryptProtectData(&in, desc.c_str(), &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    QByteArray result(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}

QByteArray unsealCredential(const QString& recordKey, const QByteArray& sealed) {
    const QByteArray entropy = recordKey.toUtf8();
    DATA_BLOB in{static_cast<DWORD>(sealed.size()), reinterpret_cast<BYTE*>(const_cast<char*>(sealed.constData()))};
    DATA_BLOB extra{static_cast<DWORD>(entropy.size()), reinterpret_cast<BYTE*>(const_cast<char*>(entropy.constData()))};
    DATA_BLOB out{0, nullptr};
    if (!CryptUnprotectData(&in, nullptr, &extra, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
    QByteArray result(reinterpret_cast<const char*>(out.pbData), int(out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return result;
}
#else
QByteArray sealCredential(const QString& recordKey, const QByteArray& plain, const QString& description) {
    if (currentBackend() == CredentialStoreBackend::Plaintext) return PlaintextMarker + plain;

    const QByteArray encoded = plain.toBase64();
    if (keyringStore(recordKey.toUtf8(), encoded)) return KeyringMarker;

    if (currentBackend() == CredentialStoreBackend::Keyring) {
        // The in-memory stand-in never fails; this branch only guards against a misuse of the seam.
        qWarning().noquote() << "credential_store: keyring store failed for" << recordKey;
        return {};
    }
    qWarning().noquote() << "credential_store: no usable Secret Service collection; storing"
                         << description << "in the clear (plaintext fallback)";
    return PlaintextMarker + plain;
}

QByteArray unsealCredential(const QString& recordKey, const QByteArray& sealed) {
    if (sealed.startsWith(PlaintextMarker)) return sealed.mid(PlaintextMarker.size());
    if (sealed.startsWith(KeyringMarker)) {
        const QByteArray encoded = keyringLookup(recordKey.toUtf8());
        if (encoded.isEmpty()) return {};
        return QByteArray::fromBase64(encoded);
    }
    return {};
}
#endif

} // namespace xmb
