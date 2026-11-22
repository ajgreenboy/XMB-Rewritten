// Byte-parity harness for the Rust `firmware-crypto` crate.
//
// Compiles the C++ reference (`firmware_crypto.cpp`) directly and prints the
// hex output of every primitive over a fixed, deterministic set of vectors.
// The captured stdout is checked in as `outputs.txt`, and the same hex is
// hard-coded as expectations in the Rust integration test `tests/parity.rs`.
//
// Build (see parity/build.sh for the exact command):
//   g++ -std=c++20 -fPIC \
//       -I/opt/qt/6.12.0/gcc_64/include -I/opt/qt/6.12.0/gcc_64/include/QtCore \
//       parity/harness.cpp \
//       -L/opt/qt/6.12.0/gcc_64/lib -lQt6Core \
//       -Wl,-rpath,/opt/qt/6.12.0/gcc_64/lib -o parity/harness

#include <QByteArray>
#include <QCryptographicHash>
#include <cstdio>

// Reference implementation (namespace xmb::firmware), relative to this file:
// parity/ -> firmware-crypto/ -> rust-edge/ -> native-xmb/src/firmware_crypto.cpp
#include "../../../src/firmware_crypto.cpp"

namespace {

// Deterministic byte sequence: n bytes starting at `start`, wrapping at 0xff.
QByteArray seq(int n, unsigned char start) {
    QByteArray b;
    b.resize(n);
    for (int i = 0; i < n; ++i) b[i] = char((start + i) & 0xff);
    return b;
}

void printLine(const char* name, const QByteArray& result) {
    std::printf("%s %s\n", name, result.toHex().constData());
}

void hmacCase(const char* name, const QByteArray& key, const QByteArray& data) {
    printLine(name, xmb::firmware::hmacSha1(key, data));
}

void ctrCase(const char* name, const QByteArray& key, const QByteArray& iv, QByteArray data) {
    xmb::firmware::aesCtr(data, key, iv);
    printLine(name, data);
}

void cbcCase(const char* name, const QByteArray& key, const QByteArray& iv, QByteArray data, bool encrypt) {
    xmb::firmware::aesCbc(data, key, iv, encrypt);
    printLine(name, data);
}

} // namespace

int main() {
    // ---- HMAC-SHA1 -------------------------------------------------------
    hmacCase("hmac-empty", QByteArray(), QByteArray());

    // RFC 2202 test case 2 (also a known-answer vector).
    hmacCase("hmac-rfc2202-2", QByteArray("Jefe"), QByteArray("what do ya want for nothing?"));

    // key length < 64 (no pre-hash)
    hmacCase("hmac-key16", seq(16, 0x00), seq(16, 0x20));
    hmacCase("hmac-key32", seq(32, 0x00), seq(32, 0x20));

    // key == 64 bytes (exactly the block size)
    hmacCase("hmac-key64", seq(64, 0x00), seq(48, 0x40));

    // key > 64 bytes -> pre-hashed with SHA-1
    hmacCase("hmac-key65", seq(65, 0x00), seq(16, 0x00));
    hmacCase("hmac-key100", seq(100, 0x00), QByteArray(1, '\xaa'));

    // ---- AES-128-CTR -----------------------------------------------------
    const QByteArray key16 = seq(16, 0x00);
    const QByteArray iv16 = seq(16, 0x10);
    const QByteArray ivff = QByteArray(16, '\xff');

    ctrCase("ctr-len0", key16, iv16, QByteArray());
    ctrCase("ctr-len16", key16, iv16, seq(16, 0x20));
    ctrCase("ctr-len32", key16, iv16, seq(32, 0x20));
    ctrCase("ctr-len20", key16, iv16, seq(20, 0x20)); // non-multiple of 16
    ctrCase("ctr-len48-carry", key16, ivff, seq(48, 0x00)); // counter carry

    // ---- AES-128-CBC (encrypt + decrypt round-trip) ----------------------
    const QByteArray data32 = seq(32, 0x20);
    QByteArray cbc128_ct = data32;
    xmb::firmware::aesCbc(cbc128_ct, key16, iv16, true);
    printLine("cbc128-enc", cbc128_ct);
    QByteArray cbc128_pt = cbc128_ct;
    xmb::firmware::aesCbc(cbc128_pt, key16, iv16, false);
    printLine("cbc128-dec", cbc128_pt);

    // ---- AES-256-CBC (encrypt + decrypt round-trip) ----------------------
    const QByteArray key32 = seq(32, 0x00);
    const QByteArray iv32 = seq(16, 0x20);
    const QByteArray data48 = seq(48, 0x30);
    QByteArray cbc256_ct = data48;
    xmb::firmware::aesCbc(cbc256_ct, key32, iv32, true);
    printLine("cbc256-enc", cbc256_ct);
    QByteArray cbc256_pt = cbc256_ct;
    xmb::firmware::aesCbc(cbc256_pt, key32, iv32, false);
    printLine("cbc256-dec", cbc256_pt);

    return 0;
}
