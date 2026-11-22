#include "credential_store.h"
#include <QCoreApplication>
#include <cstdio>

// Credential-sealing seam tests. Build with -DXMB_CREDENTIAL_STORE_BUILD_TESTS=ON; exit code 0 = all
// pass. Two deterministic paths are driven through the test seam (no real keyring daemon needed):
//   * Plaintext — the clearly-marked fallback used when no Secret Service collection is reachable.
//   * Keyring   — the Secret Service round-trip backed by an in-memory stand-in (base64 + marker).
// Auto (the production backend) is also exercised and must round-trip whichever marker it chose; on a
// headless machine with no keyring daemon that is the plaintext fallback.
using namespace xmb;

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
    };

    // ---- clearly-marked plaintext fallback (deterministic) --------------------------------------
    setCredentialStoreBackendForTest(CredentialStoreBackend::Plaintext);
    {
        const QByteArray plain = QByteArrayLiteral("{\"v\":1,\"token\":\"a-secret-token\"}");
        const QByteArray sealed = sealCredential(QStringLiteral("record-a"), plain, QStringLiteral("A record"));
        check(!sealed.isEmpty(), "plaintext seal returns non-empty");
        check(sealed.startsWith("XMB-PLAINTEXT-1\n"), "plaintext seal is clearly marked");
        check(unsealCredential(QStringLiteral("record-a"), sealed) == plain, "plaintext round-trip");

        const QByteArray sealedB = sealCredential(QStringLiteral("record-b"), QByteArrayLiteral("other"), QStringLiteral("B"));
        check(unsealCredential(QStringLiteral("record-b"), sealedB) == QByteArrayLiteral("other"), "distinct records round-trip independently");

        check(unsealCredential(QStringLiteral("record-a"), QByteArrayLiteral("foreign-bytes")).isEmpty(), "unknown sealed blob yields empty");
    }

    // ---- Secret Service round-trip through the in-memory stand-in (deterministic) ----------------
    setCredentialStoreBackendForTest(CredentialStoreBackend::Keyring);
    {
        QByteArray binary;
        binary.append(char(0));
        binary.append(char(0xff));
        binary.append(QByteArrayLiteral("binary-token"));
        const QByteArray sealed = sealCredential(QStringLiteral("record-k"), binary, QStringLiteral("K"));
        check(sealed == QByteArrayLiteral("XMB-SECRET-SERVICE-1\n"), "keyring seal returns the keyring marker");
        check(unsealCredential(QStringLiteral("record-k"), sealed) == binary, "keyring binary round-trip (base64)");

        // The value lives in the keyring under the record key, so a different record finds nothing.
        check(unsealCredential(QStringLiteral("record-other"), sealed).isEmpty(), "keyring lookup is keyed by the record");
    }

    // ---- production backend (Auto) must round-trip whichever marker it chose ---------------------
    setCredentialStoreBackendForTest(CredentialStoreBackend::Auto);
    {
        const QByteArray plain = QByteArrayLiteral("auto-round-trip-value");
        const QByteArray sealed = sealCredential(QStringLiteral("record-auto"), plain, QStringLiteral("Auto record"));
        check(!sealed.isEmpty(), "Auto seal returns non-empty");
        check(sealed.startsWith("XMB-PLAINTEXT-1\n") || sealed.startsWith("XMB-SECRET-SERVICE-1\n"),
              "Auto seal produces a recognized marker");
        check(unsealCredential(QStringLiteral("record-auto"), sealed) == plain, "Auto round-trip");
    }

    std::fprintf(stdout, failures ? "credential-store tests: %d FAILED\n" : "credential-store tests: all passed\n", failures);
    return failures ? 1 : 0;
}
