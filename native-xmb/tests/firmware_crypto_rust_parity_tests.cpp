// Byte-parity acceptance gate for the Rust firmware-crypto shim.
//
// When XMB_RUST_FIRMWARE_CRYPTO is ON, this runs the C++ reference
// (xmb::firmware, src/firmware_crypto.cpp) and the Rust-backed shim
// (xmb::firmware_rust, src/firmware_crypto_rust.cpp) over the same fixed,
// deterministic vectors and asserts byte-identical output for all three
// primitives. The vectors match parity/outputs.txt and tests/parity.rs.

#include "firmware_crypto.h"
#include "firmware_crypto_rust.h"

#include <QCoreApplication>
#include <cstdio>

namespace {
QByteArray seq(int n, int start) {
    QByteArray b;
    for (int i = 0; i < n; ++i) b += char((start + i) & 0xff);
    return b;
}
}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };

    // ---- HMAC-SHA1 -------------------------------------------------------
    const struct { const char* name; QByteArray key; QByteArray data; } hmacCases[] = {
        {"hmac-empty", QByteArray(), QByteArray()},
        {"hmac-rfc2202-2", QByteArray("Jefe"), QByteArray("what do ya want for nothing?")},
        {"hmac-key16", seq(16, 0x00), seq(16, 0x20)},
        {"hmac-key32", seq(32, 0x00), seq(32, 0x20)},
        {"hmac-key64", seq(64, 0x00), seq(48, 0x40)},
        {"hmac-key65", seq(65, 0x00), seq(16, 0x00)},
        {"hmac-key100", seq(100, 0x00), QByteArray(1, char(0xaa))},
    };
    for (const auto& c : hmacCases) {
        const QByteArray cpp = xmb::firmware::hmacSha1(c.key, c.data);
        const QByteArray rust = xmb::firmware_rust::hmacSha1(c.key, c.data);
        check(cpp.size() == 20 && cpp == rust, c.name);
    }

    // ---- AES-128-CTR -----------------------------------------------------
    const QByteArray key16 = seq(16, 0x00);
    const QByteArray iv16 = seq(16, 0x10);
    const QByteArray ivff = QByteArray(16, char(0xff));
    const struct { const char* name; QByteArray iv; QByteArray data; } ctrCases[] = {
        {"ctr-len0", iv16, QByteArray()},
        {"ctr-len16", iv16, seq(16, 0x20)},
        {"ctr-len32", iv16, seq(32, 0x20)},
        {"ctr-len20", iv16, seq(20, 0x20)},
        {"ctr-len48-carry", ivff, seq(48, 0x00)},
    };
    for (const auto& c : ctrCases) {
        QByteArray cpp = c.data, rust = c.data;
        const bool okCpp = xmb::firmware::aesCtr(cpp, key16, c.iv);
        const bool okRust = xmb::firmware_rust::aesCtr(rust, key16, c.iv);
        check(okCpp == okRust && cpp == rust, c.name);
    }

    // ---- AES-128-CBC (encrypt + decrypt round-trip) ----------------------
    {
        const QByteArray plain = seq(32, 0x20);
        QByteArray cpp = plain, rust = plain;
        const bool okCpp = xmb::firmware::aesCbc(cpp, key16, iv16, true);
        const bool okRust = xmb::firmware_rust::aesCbc(rust, key16, iv16, true);
        check(okCpp == okRust && cpp == rust, "cbc128-enc");
        QByteArray cppDec = cpp, rustDec = rust;
        const bool dCpp = xmb::firmware::aesCbc(cppDec, key16, iv16, false);
        const bool dRust = xmb::firmware_rust::aesCbc(rustDec, key16, iv16, false);
        check(dCpp == dRust && cppDec == rustDec && cppDec == plain, "cbc128-dec-roundtrip");
    }

    // ---- AES-256-CBC (encrypt + decrypt round-trip) ----------------------
    {
        const QByteArray key32 = seq(32, 0x00);
        const QByteArray iv32 = seq(16, 0x20);
        const QByteArray plain = seq(48, 0x30);
        QByteArray cpp = plain, rust = plain;
        const bool okCpp = xmb::firmware::aesCbc(cpp, key32, iv32, true);
        const bool okRust = xmb::firmware_rust::aesCbc(rust, key32, iv32, true);
        check(okCpp == okRust && cpp == rust, "cbc256-enc");
        QByteArray cppDec = cpp, rustDec = rust;
        const bool dCpp = xmb::firmware::aesCbc(cppDec, key32, iv32, false);
        const bool dRust = xmb::firmware_rust::aesCbc(rustDec, key32, iv32, false);
        check(dCpp == dRust && cppDec == rustDec && cppDec == plain, "cbc256-dec-roundtrip");
    }

    std::printf("firmware-rust-parity: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
