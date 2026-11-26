// Byte-parity and extraction-safety acceptance gate for Rust Phase 1
// (`firmware-crypto` + `firmware-extract`).
//
// Asserts:
//   1. Byte-identical output between `xmb::firmware` (C++) and
//      `xmb::firmware_rust` (Rust) across HMAC-SHA1, AES-128-CTR, and
//      AES-128/256-CBC vectors.
//   2. End-to-end parity between `xmb::extractPup` / `xmb::pupVersion` (C++)
//      and `xmb::firmware_rust::extractPup` / `pupVersion` (Rust) on synthetic
//      encrypted+compressed PUP fixtures, tampered HMACs, TAR path-traversal
//      and device-alias attacks, TAR symlinks, overflowing offsets, missing
//      keys, and transactional rollback.

#include "firmware_crypto.h"
#include "firmware_crypto_rust.h"
#include "firmware_extract.h"
#include "firmware_extract_rust.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <cstdio>

namespace {
QByteArray seq(int n, int start = 0) {
    QByteArray b;
    for (int i = 0; i < n; ++i) b += char((start + i) & 0xff);
    return b;
}
void put32(QByteArray& b, int at, quint32 v) { qToBigEndian(v, b.data() + at); }
void put64(QByteArray& b, int at, quint64 v) { qToBigEndian(v, b.data() + at); }
bool writeBytes(const QString& path, const QByteArray& bytes) {
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
}
QByteArray tarArchive(const QByteArray& name, const QByteArray& contents) {
    QByteArray h(512, 0);
    h.replace(0, name.size(), name);
    h.replace(100, 8, QByteArray("0000644\0", 8));
    h.replace(124, 12, QByteArray::number(contents.size(), 8).rightJustified(11, '0') + '\0');
    h.replace(148, 8, QByteArray(8, ' '));
    h[156] = '0';
    h.replace(257, 8, QByteArray("ustar", 5) + '\0' + "00");
    unsigned sum = 0;
    for (char c : h) sum += static_cast<unsigned char>(c);
    h.replace(148, 8, QByteArray::number(sum, 8).rightJustified(6, '0') + QByteArray("\0 ", 2));
    return h + contents + QByteArray((512 - contents.size() % 512) % 512, 0) + QByteArray(1024, 0);
}
QByteArray scePackage(const QByteArray& tar) {
    const QByteArray compressed = qCompress(tar).mid(4);
    QByteArray info(64, 0);
    info.replace(0, 16, seq(16, 32));
    info.replace(32, 16, seq(16, 48));
    QByteArray metadata(208, 0);
    put32(metadata, 12, 3);
    put32(metadata, 16, 2);
    for (int i = 0; i < 3; ++i) {
        const int h = 32 + i * 48;
        put64(metadata, h, 304);
        put32(metadata, h + 32, 1);
        put32(metadata, h + 44, 1);
    }
    put64(metadata, 128, 304);
    put64(metadata, 136, quint64(compressed.size()));
    put32(metadata, 160, 3);
    put32(metadata, 164, 0);
    put32(metadata, 168, 1);
    put32(metadata, 172, 2);
    metadata.replace(176, 16, seq(16, 64));
    metadata.replace(192, 16, seq(16, 80));
    xmb::firmware::aesCtr(metadata, info.left(16), info.mid(32, 16));
    xmb::firmware::aesCbc(info, seq(32), seq(16, 16), true);
    QByteArray encrypted = compressed;
    xmb::firmware::aesCtr(encrypted, seq(16, 64), seq(16, 80));
    QByteArray sce(32, 0);
    sce.replace(0, 4, QByteArray("SCE\0", 4));
    put32(sce, 4, 2);
    put64(sce, 16, 304);
    put64(sce, 24, quint64(encrypted.size()));
    return sce + info + metadata + encrypted;
}
QByteArray buildPup(const QByteArray& update) {
    const QByteArray version("4.93\0", 5);
    QByteArray b(176, 0);
    b.replace(0, 8, QByteArray("SCEUF\0\0\0", 8));
    put64(b, 8, 1);
    put64(b, 16, 1);
    put64(b, 24, 2);
    put64(b, 32, 176);
    put64(b, 40, quint64(version.size() + update.size()));
    put64(b, 48, 0x100);
    put64(b, 56, 176);
    put64(b, 64, quint64(version.size()));
    put64(b, 80, 0x300);
    put64(b, 88, quint64(176 + version.size()));
    put64(b, 96, quint64(update.size()));
    put64(b, 112, 0x100);
    b.replace(120, 20, xmb::firmware::hmacSha1(seq(64), version));
    put64(b, 144, 0x300);
    b.replace(152, 20, xmb::firmware::hmacSha1(seq(64), update));
    return b + version + update;
}
}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("%s: %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };

    // ---- 1. Crypto byte-parity ------------------------------------------
    const struct { const char* name; QByteArray key; QByteArray data; } hmacCases[] = {
        {"hmac-empty", QByteArray(), QByteArray()},
        {"hmac-rfc2202-2", QByteArray("Jefe"), QByteArray("what do ya want for nothing?")},
        {"hmac-key16", seq(16, 0x00), seq(16, 0x20)},
        {"hmac-key32", seq(32, 0x00), seq(32, 0x20)},
        {"hmac-key64", seq(64, 0x00), seq(48, 0x40)},
        {"hmac-key65", seq(65, 0x00), seq(16, 0x00)},
        {"hmac-key100", seq(100, 0x00), QByteArray::fromHex("aa")},
    };
    for (const auto& c : hmacCases) {
        const QByteArray cpp = xmb::firmware::hmacSha1(c.key, c.data);
        const QByteArray rust = xmb::firmware_rust::hmacSha1(c.key, c.data);
        check(cpp.size() == 20 && cpp == rust, c.name);
    }

    const QByteArray key16 = seq(16, 0x00);
    const QByteArray iv16 = seq(16, 0x10);
    const QByteArray ivff = QByteArray::fromHex("ffffffffffffffffffffffffffffffff");
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

    // ---- 2. firmware-extract parity & safety ----------------------------
    qunsetenv("XMB_FIRMWARE_KEYS");
    QString missingErr;
    check(!xmb::firmware_rust::extractPup(QStringLiteral("missing.pup"), QStringLiteral("unused"), &missingErr) &&
              missingErr == QStringLiteral("decryption keys not provided (create native-xmb/src/firmware_keys.inc)"),
          "extract-missing-key-fallback");

    QTemporaryDir temp;
    check(temp.isValid(), "extract-tempdir");
    QByteArray declarations;
    for (const auto& pair : {qMakePair(QByteArray("PUP_KEY"), seq(64)),
                             qMakePair(QByteArray("SCEPKG_ERK"), seq(32)),
                             qMakePair(QByteArray("SCEPKG_RIV"), seq(16, 16))}) {
        declarations += "static const unsigned char " + pair.first + '[' + QByteArray::number(pair.second.size()) + "] = {";
        for (char c : pair.second) declarations += QByteArray::number(static_cast<unsigned char>(c)) + ',';
        declarations += "};\n";
    }
    const QString keyPath = temp.path() + "/test.inc";
    check(writeBytes(keyPath, declarations), "extract-synthetic-keys");
    qputenv("XMB_FIRMWARE_KEYS", keyPath.toUtf8());

    const QString source = temp.path() + "/PS3UPDAT.PUP";
    const QString destCpp = temp.path() + "/flash-cpp";
    const QString destRust = temp.path() + "/flash-rust";
    QByteArray goodPup = buildPup(tarArchive(
        "dev_flash_synthetic",
        scePackage(tarArchive("dev_flash/vsh/resource/explore_plugin_full.rco", "fixture-bytes-493"))));
    check(writeBytes(source, goodPup), "extract-write-good-pup");

    QString errCpp, errRust;
    check(xmb::extractPup(source, destCpp, &errCpp), "extract-cpp-good-pup");
    check(xmb::firmware_rust::extractPup(source, destRust, &errRust), "extract-rust-good-pup");
    QFile fCpp(destCpp + "/vsh/resource/explore_plugin_full.rco");
    QFile fRust(destRust + "/vsh/resource/explore_plugin_full.rco");
    check(fCpp.open(QIODevice::ReadOnly) && fRust.open(QIODevice::ReadOnly) &&
              fCpp.readAll() == fRust.readAll() && fRust.size() == 17,
          "extract-byte-identical-output");

    QString verCpp, verRust;
    check(xmb::pupVersion(source, &verCpp, &errCpp) &&
              xmb::firmware_rust::pupVersion(source, &verRust, &errRust) &&
              verCpp == QStringLiteral("4.93") && verCpp == verRust,
          "pupVersion-parity-4.93");

    QByteArray tampered = goodPup;
    tampered[120] = char(tampered[120] ^ 1);
    writeBytes(source, tampered);
    check(!xmb::firmware_rust::extractPup(source, temp.path() + "/bad-hmac", &errRust) &&
              errRust.contains(QStringLiteral("HMAC")) &&
              !QFileInfo::exists(temp.path() + "/bad-hmac"),
          "extract-rust-rejects-bad-hmac-cleanly");

    const QByteArray normal = tarArchive("dev_flash/vsh/normal", "ok");
    const QByteArray traversalPup = buildPup(tarArchive(
        "dev_flash_synthetic",
        scePackage(normal.left(normal.size() - 1024) + tarArchive("../evil", "escape"))));
    writeBytes(source, traversalPup);
    check(!xmb::firmware_rust::extractPup(source, temp.path() + "/unsafe", &errRust) &&
              errRust.contains(QStringLiteral("Unsafe TAR path")) &&
              !QFile::exists(temp.path() + "/evil") &&
              !QFileInfo::exists(temp.path() + "/unsafe"),
          "extract-rust-rejects-tar-traversal-before-any-writes");

    for (const QByteArray& bad : {QByteArray("/absolute"), QByteArray("C:/escape"),
                                  QByteArray("dir\\..\\evil"), QByteArray("dir/file:stream"),
                                  QByteArray("CON")}) {
        writeBytes(source, buildPup(tarArchive("dev_flash_synthetic", scePackage(tarArchive(bad, "escape")))));
        check(!xmb::firmware_rust::extractPup(source, temp.path() + "/unsafe", &errRust) &&
                  errRust.contains(QStringLiteral("Unsafe TAR path")),
              bad.constData());
    }

    QByteArray linked = tarArchive("dev_flash/vsh/link", "target");
    linked[156] = '2';
    linked.replace(148, 8, QByteArray(8, ' '));
    unsigned checksum = 0;
    for (int i = 0; i < 512; ++i) checksum += static_cast<unsigned char>(linked[i]);
    linked.replace(148, 8, QByteArray::number(checksum, 8).rightJustified(6, '0') + QByteArray("\0 ", 2));
    writeBytes(source, buildPup(tarArchive("dev_flash_synthetic", scePackage(linked))));
    check(!xmb::firmware_rust::extractPup(source, temp.path() + "/unsafe", &errRust) &&
              errRust.contains(QStringLiteral("links/devices forbidden")),
          "extract-rust-rejects-symlink");

    QByteArray overflowPup = buildPup(tarArchive("ordinary", "hello"));
    put64(overflowPup, 88, ~quint64(0));
    writeBytes(source, overflowPup);
    check(!xmb::firmware_rust::extractPup(source, temp.path() + "/unsafe", &errRust) &&
              errRust.contains(QStringLiteral("bounds")),
          "extract-rust-rejects-overflow-bounds");

    writeBytes(source, buildPup(tarArchive("ordinary", "hello")));
    check(!xmb::firmware_rust::extractPup(source, temp.path() + "/empty", &errRust) &&
              errRust.contains(QStringLiteral("2 entries")),
          "extract-rust-rejects-no-package-pup");

    writeBytes(source, QByteArray("not a pup, but long enough to have a header....................."));
    check(!xmb::firmware_rust::pupVersion(source, &verRust, &errRust) && verRust.isEmpty(),
          "pupVersion-rust-rejects-non-pup");

    std::printf("firmware-rust-parity: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
