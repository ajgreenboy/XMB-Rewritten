// Byte-parity and extraction/decoder-safety acceptance gate for Rust Phases 1-3
// (`firmware-crypto`, `firmware-extract`, `steam-vdf`, and `image-decode` [GTF, Edge, PGS, VobSub]).

#include "edge_decode.h"
#include "firmware_crypto.h"
#include "firmware_crypto_rust.h"
#include "firmware_extract.h"
#include "firmware_extract_rust.h"
#include "gtf_decode.h"
#include "image_decode_rust.h"
#include "image_subtitles.h"
#include "steam_vdf.h"
#include "steam_vdf_rust.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtEndian>
#include <bit>
#include <cstdio>

namespace {
inline char bchar(int v) { return static_cast<char>(static_cast<unsigned char>(v & 0xff)); }
QByteArray seq(int n, int start = 0) {
    QByteArray b;
    for (int i = 0; i < n; ++i) b += bchar(start + i);
    return b;
}
void put16(QByteArray& b, int at, quint16 v) { qToBigEndian(v, b.data() + at); }
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

bool sameVdfEntries(const QVector<xmb::steamvdf::Entry>& a, const QVector<xmb::steamvdf::Entry>& b) {
    if (a.size() != b.size()) return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (a[i].key != b[i].key || a[i].value != b[i].value || a[i].block != b[i].block ||
            !sameVdfEntries(a[i].children, b[i].children))
            return false;
    }
    return true;
}

QByteArray makeGtf(int format, int width, int height, const QByteArray& pixels, quint32 pitch = 0) {
    QByteArray b(0x80, '\0');
    put32(b, 0, 0x01020200);
    put32(b, 8, 1);
    put32(b, 0x10, 0x80);
    put32(b, 0x14, quint32(pixels.size()));
    b[0x18] = char(format);
    b[0x19] = 1;
    b[0x1a] = 2;
    put16(b, 0x20, quint16(width));
    put16(b, 0x22, quint16(height));
    put32(b, 0x28, pitch);
    return b + pixels;
}

QByteArray makeEdge(quint32 segments = 1, bool wrongDescriptor = false) {
    const int seg = 0x10, pos = 0x100, normal = 0x140, uv = 0x160, index = 0x180, desc = 0x1a0, uvDesc = 0x1c0;
    QByteArray b(0x1e0, '\0');
    put32(b, 0, segments);
    put32(b, 4, seg);
    put16(b, seg + 8, 4);
    put16(b, seg + 10, 6);
    put32(b, seg + 0x10, index);
    put16(b, seg + 0x14, 12);
    put32(b, seg + 0x18, pos);
    put32(b, seg + 0x1c, normal);
    put32(b, seg + 0x2c, uv);
    put32(b, seg + 0x70, desc);
    put32(b, seg + 0x74, uvDesc);
    const float p[4][3] = {{1, 2, 3}, {-1, 0, 0}, {0, 1, 0}, {0.5f, -0.5f, 2}};
    for (int v = 0; v < 4; ++v)
        for (int c = 0; c < 3; ++c) put32(b, pos + v * 12 + c * 4, std::bit_cast<quint32>(p[v][c]));
    for (int v = 0; v < 4; ++v) put32(b, normal + v * 4, 1023u);
    for (int v = 0; v < 4; ++v) {
        put16(b, uv + v * 4, 0x3c00);
        put16(b, uv + v * 4 + 2, 0x3800);
    }
    put16(b, index, 1);
    put16(b, index + 2, 0);
    put16(b, index + 4, 1);
    b[index + 6] = 4;
    b[index + 7] = 0;
    b[index + 8] = char(0x10);
    b[index + 9] = QByteArray::fromHex("c0").at(0);
    b[index + 10] = char(0x30);
    QByteArray d1 = QByteArray::fromHex("0210020000000000000203010c0000000c06010204020000");
    const QByteArray d2 = QByteArray::fromHex("01040100000000000003020504080000");
    if (wrongDescriptor) d1[2] = char(0x7f);
    b.replace(desc, d1.size(), d1);
    b.replace(uvDesc, d2.size(), d2);
    return b;
}

QByteArray pgsSegment(quint32 ptsTicks, uchar type, const QByteArray& body) {
    QByteArray seg(13, '\0');
    seg[0] = 'P';
    seg[1] = 'G';
    put32(seg, 2, ptsTicks);
    put32(seg, 6, 0);
    seg[10] = char(type);
    put16(seg, 11, quint16(body.size()));
    return seg + body;
}

QByteArray makeSyntheticPgs() {
    // 1920x1080 HD display set at t=1000 ms (90000 ticks), cleared at t=3500 ms (315000 ticks).
    // Palette 0: entry 0 transparent, entry 1 bright yellow/white (Y=235, Cr=140, Cb=100, A=255).
    QByteArray pds(2 + 10, '\0');
    pds[0] = 0; pds[1] = 0;
    pds[2] = 0; pds[3] = 16; pds[4] = bchar(128); pds[5] = bchar(128); pds[6] = 0;
    pds[7] = 1; pds[8] = bchar(235); pds[9] = bchar(140); pds[10] = bchar(100); pds[11] = bchar(255);

    // 4x2 object (id 0): row 0 = [0, 1, 1, 0], row 1 = [0, 1, 1, 0] -> trims to 2x2 at +(1,0)!
    QByteArray rle;
    rle += QByteArray::fromHex("0001010100010000"); // 1x0, 1, 1, 1x0, EOL
    rle += QByteArray::fromHex("0001010100010000"); // 1x0, 1, 1, 1x0, EOL
    QByteArray ods(11, '\0');
    put16(ods, 0, 0);
    ods[2] = 0;
    ods[3] = bchar(0xC0); // first + last
    const quint32 objLen = quint32(rle.size() + 4);
    ods[4] = bchar(int((objLen >> 16) & 0xff));
    ods[5] = bchar(int((objLen >> 8) & 0xff));
    ods[6] = bchar(int(objLen & 0xff));
    put16(ods, 7, 4);
    put16(ods, 9, 2);
    ods += rle;

    QByteArray wds(10, '\0');
    wds[0] = 1;
    wds[1] = 0;
    put16(wds, 2, 100);
    put16(wds, 4, 800);
    put16(wds, 6, 400);
    put16(wds, 8, 100);

    QByteArray pcs(19, '\0');
    put16(pcs, 0, 1920);
    put16(pcs, 2, 1080);
    pcs[4] = 0x10;
    pcs[7] = bchar(0x80); // epoch start
    pcs[9] = 0;
    pcs[10] = 1;
    put16(pcs, 11, 0);
    pcs[13] = 0;
    pcs[14] = bchar(0x40); // forced
    put16(pcs, 15, 200);
    put16(pcs, 17, 820);

    QByteArray clearPcs(11, '\0');
    put16(clearPcs, 0, 1920);
    put16(clearPcs, 2, 1080);
    clearPcs[10] = 0;

    return pgsSegment(90000, 0x16, pcs) +
           pgsSegment(90000, 0x17, wds) +
           pgsSegment(90000, 0x14, pds) +
           pgsSegment(90000, 0x15, ods) +
           pgsSegment(90000, 0x80, {}) +
           pgsSegment(315000, 0x16, clearPcs) +
           pgsSegment(315000, 0x80, {});
}

QByteArray makeSyntheticVobSubSub() {
    // 4x2 SPU: top field (y=0) = 4 pixels of colour 1 (`0x11` -> run=4, col=1);
    // bottom field (y=1) = 4 pixels of colour 2 (`0x12` -> run=4, col=2).
    // Control sequence starts at offset 6:
    //   seq 0 (date 0): 0x03 (colours 3,2,1,0), 0x04 (alpha 15,15,15,0), 0x05 (x 10..13, y 20..21),
    //                   0x06 (top=4, bottom=5), 0x01 (start display), 0xFF
    //   seq 1 (date 180 -> 2048 ms): 0x02 (stop display), 0xFF
    QByteArray spu(4, '\0');
    spu += bchar(0x11); // offset 4: top field
    spu += bchar(0x12); // offset 5: bottom field
    const int ctrl0 = spu.size(); // 6
    QByteArray s0;
    s0 += QByteArray::fromHex("0000"); // date = 0
    s0 += QByteArray::fromHex("0000"); // next (patched below)
    s0 += QByteArray::fromHex("033210");
    s0 += QByteArray::fromHex("04fff0");
    // x1=10 (0x00a), x2=13 (0x00d) -> 00 a0 0d; y1=20 (0x014), y2=21 (0x015) -> 01 40 15
    s0 += QByteArray::fromHex("0500a00d014015");
    s0 += QByteArray::fromHex("0600040005");
    s0 += QByteArray::fromHex("01ff");
    const int ctrl1 = ctrl0 + s0.size();
    put16(s0, 2, quint16(ctrl1));
    QByteArray s1(4, '\0');
    put16(s1, 0, 180);
    put16(s1, 2, quint16(ctrl1)); // self next terminates loop
    s1 += QByteArray::fromHex("02ff");
    spu += s0 + s1;
    put16(spu, 0, quint16(spu.size()));
    put16(spu, 2, quint16(ctrl0));

    // Wrap inside an MPEG-2 private_stream_1 PES packet (`00 00 01 BD`) with substream 0x20.
    QByteArray pesPayload;
    pesPayload += bchar(0x80);
    pesPayload += bchar(0x80); // PTS present
    pesPayload += bchar(5);
    // PTS = 90000 (1000 ms):
    const qint64 pts = 90000;
    pesPayload += bchar(int(0x21 | (((pts >> 30) & 7) << 1)));
    pesPayload += bchar(int((pts >> 22) & 0xff));
    pesPayload += bchar(int(0x01 | (((pts >> 15) & 0x7f) << 1)));
    pesPayload += bchar(int((pts >> 7) & 0xff));
    pesPayload += bchar(int(0x01 | ((pts & 0x7f) << 1)));
    pesPayload += bchar(0x20); // substream 0
    pesPayload += spu;

    QByteArray pack = QByteArray::fromHex("000001ba4400040004010189c3f8");
    QByteArray pes(6, '\0');
    pes[0] = 0; pes[1] = 0; pes[2] = 1; pes[3] = bchar(0xBD);
    put16(pes, 4, quint16(pesPayload.size()));
    return pack + pes + pesPayload;
}

bool sameCues(const QVector<xmb::ImageCue>& a, const QVector<xmb::ImageCue>& b) {
    if (a.size() != b.size()) return false;
    for (qsizetype i = 0; i < a.size(); ++i) {
        if (a[i].startMs != b[i].startMs || a[i].endMs != b[i].endMs || a[i].rect != b[i].rect ||
            a[i].videoSize != b[i].videoSize || a[i].forced != b[i].forced ||
            a[i].image.size() != b[i].image.size() || a[i].image.colorTable() != b[i].image.colorTable())
            return false;
        for (int y = 0; y < a[i].image.height(); ++y) {
            if (std::memcmp(a[i].image.constScanLine(y), b[i].image.constScanLine(y), size_t(a[i].image.width())) != 0)
                return false;
        }
    }
    return true;
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

    // ---- 3. Phase 2: steam-vdf parity -----------------------------------
    {
        xmb::steamvdf::Options accountOpts;
        accountOpts.skipBom = true;
        xmb::steamvdf::Options libOpts;
        libOpts.quotedBracesAreStructural = true;
        libOpts.maxTokenBytes = 2 * 1024 * 1024;
        libOpts.maxTokens = 100000;
        libOpts.maxEntries = 20000;
        libOpts.maxDepth = 32;

        const QByteArray samples[] = {
            QByteArray("\xef\xbb\xbf\"users\" { \"76561198000000001\" { \"AccountName\" \"a\\\"b\\\\c\\nd\" MostRecent 1//c\n } }"),
            QByteArray("\"libraryfolders\" \"{\" \"0\" \"{\" \"path\" \"C:\\\\Steam\" \"}\" \"}\""),
            QByteArray("users\0{}", 8),
            QByteArray("\"unterminated"),
            QByteArray("\"a\" { \"b\" { \"c\" { \"d\" { \"e\" { \"f\" { \"g\" { \"h\" { \"i\" { \"k\" \"v\" } } } } } } } } }"),
            QByteArray("}"),
        };
        int idx = 0;
        for (const QByteArray& sample : samples) {
            for (const auto& opts : {accountOpts, libOpts}) {
                const auto cppDoc = xmb::steamvdf::parse(sample, opts);
                const auto rustDoc = xmb::steamvdf_rust::parse(sample, opts);
                const QByteArray label = QByteArray("vdf-parity-") + QByteArray::number(idx++);
                check(cppDoc.error == rustDoc.error && sameVdfEntries(cppDoc.entries, rustDoc.entries), label.constData());
            }
        }
    }

    // ---- 4. Phase 3: GTF, Edge, PGS, and VobSub parity ------------------
    {
        QByteArray argb;
        for (int n = 0; n < 16; ++n) argb += QByteArray::fromHex("ff") + char(n) + char(n * 3) + char(255 - n * 4);
        const QByteArray gtfInputs[] = {
            makeGtf(0xa1, 2, 2, QByteArray::fromHex("1020ffff3040ffff"), 4),
            makeGtf(0x85, 4, 4, argb),
            makeGtf(0xa5, 4, 4, argb),
            makeGtf(0x86, 4, 4, QByteArray::fromHex("00f81f0000550000")),
            makeGtf(0x86, 4, 4, QByteArray::fromHex("1f0000f8ffffffff")),
            makeGtf(0x87, 4, 4, QByteArray::fromHex("8888888888888888ffffffff00000000")),
            makeGtf(0x88, 4, 4, QByteArray::fromHex("ff00499224499224ffffffff00000000")),
            QByteArray(16, '\0'),
            makeGtf(0x83, 2, 2, QByteArray(16, '\0')),
            makeGtf(0x85, 3, 4, QByteArray(48, '\0')),
            makeGtf(0x85, 4, 4, QByteArray(10, '\0')),
        };
        int gi = 0;
        for (const QByteArray& g : gtfInputs) {
            QString eCpp, eRust;
            const QImage iCpp = xmb::decodeGtf(g, &eCpp);
            const QImage iRust = xmb::image_rust::decodeGtf(g, &eRust);
            const QByteArray label = QByteArray("gtf-parity-") + QByteArray::number(gi++);
            check(iCpp.isNull() == iRust.isNull() && iCpp == iRust && eCpp == eRust, label.constData());
        }
    }

    {
        const QByteArray edgeInputs[] = {makeEdge(), makeEdge(2), makeEdge(1, true), makeEdge().left(0x40)};
        int ei = 0;
        for (const QByteArray& e : edgeInputs) {
            QVector<xmb::EdgeVertex> vCpp, vRust;
            QVector<quint32> iCpp, iRust;
            QString eCpp, eRust;
            const bool okCpp = xmb::decodeEdge(e, vCpp, iCpp, &eCpp);
            const bool okRust = xmb::image_rust::decodeEdge(e, vRust, iRust, &eRust);
            bool sameVerts = vCpp.size() == vRust.size();
            for (qsizetype k = 0; sameVerts && k < vCpp.size(); ++k)
                sameVerts = vCpp[k].position == vRust[k].position && vCpp[k].normal == vRust[k].normal && vCpp[k].uv == vRust[k].uv;
            const QByteArray label = QByteArray("edge-parity-") + QByteArray::number(ei++);
            check(okCpp == okRust && sameVerts && iCpp == iRust && eCpp == eRust, label.constData());
        }
    }

    {
        const QByteArray pgs = makeSyntheticPgs();
        QVector<xmb::ImageCue> cCpp, cRust;
        QString rCpp, rRust;
        const bool okCpp = xmb::parsePgs(pgs, cCpp, &rCpp);
        const bool okRust = xmb::image_rust::parsePgs(pgs, cRust, &rRust);
        check(okCpp && okRust && cCpp.size() == 1 && sameCues(cCpp, cRust) && rCpp == rRust, "pgs-parity-hd-cue");

        const bool badCpp = xmb::parsePgs(QByteArray("NOTPGS"), cCpp, &rCpp);
        const bool badRust = xmb::image_rust::parsePgs(QByteArray("NOTPGS"), cRust, &rRust);
        check(!badCpp && !badRust && rCpp == rRust, "pgs-parity-reject-non-pgs");
    }

    {
        const QByteArray idx =
            "size: 720x480\n"
            "org: 4, 8\n"
            "palette: 000000, ff0000, 00ff00, 0000ff, ffffff, 808080, 404040, 202020, 101010, 010203, 040506, 070809, 0a0b0c, 0d0e0f, 112233, 445566\n"
            "id: en, index: 0\n"
            "timestamp: 00:00:01:000, filepos: 000000000\n";
        const QByteArray sub = makeSyntheticVobSubSub();
        QVector<xmb::ImageCue> cCpp, cRust;
        QString rCpp, rRust;
        const bool okCpp = xmb::parseVobSub(idx, sub, 0, cCpp, &rCpp);
        const bool okRust = xmb::image_rust::parseVobSub(idx, sub, 0, cRust, &rRust);
        check(okCpp && okRust && cCpp.size() == 1 && sameCues(cCpp, cRust) && rCpp == rRust, "vobsub-parity-cue");

        const bool badCpp = xmb::parseVobSub(QByteArray("garbage\n"), sub, 0, cCpp, &rCpp);
        const bool badRust = xmb::image_rust::parseVobSub(QByteArray("garbage\n"), sub, 0, cRust, &rRust);
        check(!badCpp && !badRust && rCpp == rRust, "vobsub-parity-reject-bad-idx");
    }

    std::printf("rust-edge-parity: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
