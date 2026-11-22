#pragma once
// ==== credential_store: one seam for sealing credentials to the current OS user ===================
//
// The app seals long-lived credentials (Jellyfin/Spotify/Discord/Home Assistant session tokens, the
// Seerr/SABnzbd connection and the download queue) before writing them to disk. Every one of those
// call sites used to hand-roll the same Windows DPAPI blob; this file is the single shared seam.
//
//   sealCredential(recordKey, plain, description) -> bytes the caller persists (e.g. session.bin)
//   unsealCredential(recordKey, sealed)          -> the original plaintext back
//
//   recordKey   — a stable, application-unique label for the record (e.g.
//                 "xmb-native-jellyfin-session-v1"). On Windows this is DPAPI's per-record "additional
//                 entropy" (the extra key material that ties a blob to this record); on Linux it is the
//                 libsecret attribute value that keeps records from colliding.
//   description — a human-readable label. On Windows it is DPAPI's szDataDescr (metadata shown in the
//                 credential UI); on Linux it is only used in the plaintext-fallback warning.
//
// Both functions return an empty QByteArray on failure. Callers already treat an empty result as
// "could not protect / not present", so the contract is unchanged.
//
// Windows: CryptProtectData/CryptUnprotectData, exactly as before — existing sealed records stay
// readable and the per-record entropy is preserved byte-for-byte.
//
// Linux: the Secret Service via libsecret (secret_password_store_sync / secret_password_lookup_sync
// with a stable schema), when a service is reachable. There is no home-grown crypto: if no Secret
// Service collection can be used (a headless machine, a locked keyring, no keyring daemon), the value
// is stored as a clearly-marked plaintext blob so the app still runs headless. The blob's leading
// marker records which path produced it, so unseal picks the matching reversal.
#include <QByteArray>
#include <QString>

namespace xmb {

QByteArray sealCredential(const QString& recordKey, const QByteArray& plain, const QString& description);
QByteArray unsealCredential(const QString& recordKey, const QByteArray& sealed);

// ---- test seam -----------------------------------------------------------------------------------
// Forces which Linux backend the seal/unseal pair uses, so a unit test can prove the Secret Service
// round-trip (against an in-memory stand-in) and the plaintext fallback deterministically, without a
// real keyring daemon. Auto — the production default — probes the Secret Service on first use and
// falls back to plaintext when it is unreachable. No effect on Windows.
enum class CredentialStoreBackend { Auto, Keyring, Plaintext };
void setCredentialStoreBackendForTest(CredentialStoreBackend backend);

} // namespace xmb
