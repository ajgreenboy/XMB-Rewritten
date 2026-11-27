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
#include "qrc.h"
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

// ---- Phase 4 synthetic GIM and VAG builders + C++ reference decoders (matching assets.cpp & p3t.cpp) ----

QByteArray gimBlock(quint16 type, quint32 dataOff, const QByteArray& body) {
    QByteArray hdr(16, '\0');
    put16(hdr, 0, type);
    put32(hdr, 4, quint32(16 + body.size()));
    put32(hdr, 8, 16);
    put32(hdr, 12, dataOff);
    return hdr + body;
}

QByteArray makeGimImageBlock(quint16 format, quint16 w, quint16 h, quint16 align, const QByteArray& pixels, quint16 order = 0) {
    QByteArray info(0x30, '\0');
    put16(info, 4, format);
    put16(info, 6, order);
    put16(info, 8, w);
    put16(info, 10, h);
    put16(info, 14, align);
    put32(info, 0x1c, 0x30);
    return gimBlock(4, 16, info + pixels);
}

QByteArray makeGimPaletteBlock(quint16 format, quint16 count, const QByteArray& entries) {
    QByteArray info(0x30, '\0');
    put16(info, 4, format);
    put16(info, 6, 0);
    put16(info, 8, count);
    put16(info, 10, 1);
    put32(info, 0x1c, 0x30);
    return gimBlock(5, 16, info + entries);
}

QByteArray wrapGimRoot(const QByteArray& pictureChildren) {
    const QByteArray pic = gimBlock(3, 16, pictureChildren);
    const QByteArray root = gimBlock(2, 16, pic);
    return QByteArray(".GIM1.00\0\0\0\0\0\0\0\0", 16) + root;
}

QImage cppDecodeGim(const QByteArray& b, QString* error) {
    if (error) error->clear();
    const auto fail = [&](const QString& msg) -> QImage {
        if (error) *error = msg;
        return {};
    };
    const auto inRange = [&](quint64 p, quint64 n) {
        return p <= quint64(b.size()) && n <= quint64(b.size()) - p;
    };
    const auto u16 = [&](quint64 p, bool& ok) -> quint16 {
        if (!inRange(p, 2)) { ok = false; return 0; }
        return qFromBigEndian<quint16>(b.constData() + p);
    };
    const auto u32 = [&](quint64 p, bool& ok) -> quint32 {
        if (!inRange(p, 4)) { ok = false; return 0; }
        return qFromBigEndian<quint32>(b.constData() + p);
    };
    const auto c565 = [](quint16 v) {
        const int r = v >> 11, g = (v >> 5) & 63, bl = v & 31;
        return qRgb((r << 3) | (r >> 2), (g << 2) | (g >> 4), (bl << 3) | (bl >> 2));
    };
    const auto c5551 = [](quint16 v) {
        const int r = (v >> 10) & 31, g = (v >> 5) & 31, bl = v & 31;
        return qRgba((r << 3) | (r >> 2), (g << 3) | (g >> 2), (bl << 3) | (bl >> 2), (v & 0x8000) ? 255 : 0);
    };
    const auto c4444 = [](quint16 v) {
        return qRgba(((v >> 12) & 15) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17);
    };
    if (!b.startsWith(".GIM1.00")) return fail(QStringLiteral("Unsupported GIM byte order"));
    quint64 at = 0x10, pictureData = 0, pictureEnd = quint64(b.size());
    for (int blocks = 0; blocks < 128; ++blocks) {
        bool ok = true;
        const auto type = u16(at, ok);
        const auto size = u32(at + 4, ok), data = u32(at + 12, ok);
        if (!ok) return fail(QStringLiteral("Asset offset exceeds its container"));
        if (size < 16 || data < 16 || data >= size) return fail(QStringLiteral("Invalid GIM block"));
        if (!inRange(at, size)) return fail(QStringLiteral("Asset offset exceeds its container"));
        if (type == 2 || type == 3) {
            if (type == 3) { pictureData = at + data; pictureEnd = at + size; }
            at += data;
            continue;
        }
        if (type != 4) { at += size; continue; }
        const quint64 info = at + data;
        if (!inRange(info, 0x30)) return fail(QStringLiteral("Asset offset exceeds its container"));
        const auto format = u16(info + 4, ok), order = u16(info + 6, ok), w = u16(info + 8, ok), h = u16(info + 10, ok);
        const auto alignment = u16(info + 14, ok);
        if (order != 0) return fail(QStringLiteral("Unsupported GIM pixel order"));
        if (!w || !h || w > 4096 || h > 4096 || !alignment || alignment > 4096) return fail(QStringLiteral("Invalid GIM dimensions"));
        const quint64 pixels = info + u32(info + 0x1c, ok);
        if (pixels < info + 0x30) return fail(QStringLiteral("GIM pixels overlap image metadata"));
        const quint64 end = at + size;
        if (format >= 8 && format <= 10) {
            const quint64 blockBytes = format == 8 ? 8 : 16;
            const quint64 rowBytes = quint64((w + 3) / 4) * blockBytes;
            const quint64 stride = ((rowBytes + alignment - 1) / alignment) * alignment;
            const quint64 length = stride * ((h + 3) / 4);
            if (pixels > end || length > end - pixels) return fail(QStringLiteral("DXT blocks exceed GIM image"));
            if (!inRange(pixels, length)) return fail(QStringLiteral("Asset offset exceeds its container"));
            QImage result(w, h, QImage::Format_ARGB32);
            for (int by = 0; by < h; by += 4) for (int bx = 0; bx < w; bx += 4) {
                const auto* block = reinterpret_cast<const uchar*>(b.constData() + pixels + (by / 4) * stride + (bx / 4) * blockBytes);
                const auto* colours = block + (format == 8 ? 0 : 8);
                const quint16 c0 = qFromLittleEndian<quint16>(colours), c1 = qFromLittleEndian<quint16>(colours + 2);
                QRgb palette[4]{c565(c0), c565(c1), 0, 0};
                if (format != 8 || c0 > c1) {
                    for (int i = 0; i < 2; ++i)
                        palette[2 + i] = qRgb(((2 - i) * qRed(palette[0]) + (1 + i) * qRed(palette[1])) / 3,
                                              ((2 - i) * qGreen(palette[0]) + (1 + i) * qGreen(palette[1])) / 3,
                                              ((2 - i) * qBlue(palette[0]) + (1 + i) * qBlue(palette[1])) / 3);
                } else {
                    palette[2] = qRgb((qRed(palette[0]) + qRed(palette[1])) / 2,
                                      (qGreen(palette[0]) + qGreen(palette[1])) / 2,
                                      (qBlue(palette[0]) + qBlue(palette[1])) / 2);
                    palette[3] = qRgba(0, 0, 0, 0);
                }
                const quint32 indices = qFromLittleEndian<quint32>(colours + 4);
                int alphas[8]{};
                quint64 alphaIndices = 0;
                if (format == 10) {
                    alphas[0] = block[0]; alphas[1] = block[1];
                    if (alphas[0] > alphas[1]) for (int i = 1; i <= 6; ++i) alphas[i + 1] = ((7 - i) * alphas[0] + i * alphas[1]) / 7;
                    else { for (int i = 1; i <= 4; ++i) alphas[i + 1] = ((5 - i) * alphas[0] + i * alphas[1]) / 5; alphas[6] = 0; alphas[7] = 255; }
                    for (int i = 0; i < 6; ++i) alphaIndices |= quint64(block[2 + i]) << (i * 8);
                }
                for (int y = 0; y < 4 && by + y < h; ++y) for (int x = 0; x < 4 && bx + x < w; ++x) {
                    const int pixel = y * 4 + x;
                    const auto colour = palette[(indices >> (pixel * 2)) & 3];
                    const int alpha = format == 10 ? alphas[(alphaIndices >> (pixel * 3)) & 7]
                                    : format == 9 ? ((block[pixel / 2] >> ((pixel & 1) * 4)) & 15) * 17
                                    : qAlpha(colour);
                    reinterpret_cast<QRgb*>(result.scanLine(by + y))[bx + x] = qRgba(qRed(colour), qGreen(colour), qBlue(colour), alpha);
                }
            }
            return result;
        }
        if (format >= 4 && format <= 6) {
            QVector<QRgb> palColours;
            for (quint64 blk = pictureData ? pictureData : end; blk + 16 <= pictureEnd && blk + 16 <= quint64(b.size());) {
                const auto ptype = u16(blk, ok);
                const auto psize = u32(blk + 4, ok), pdata = u32(blk + 12, ok);
                if (!ok) return fail(QStringLiteral("Asset offset exceeds its container"));
                if (psize < 16 || pdata < 16 || pdata >= psize || blk + psize > pictureEnd || blk + psize > quint64(b.size())) {
                    if (ptype == 5) return fail(QStringLiteral("Invalid GIM palette block"));
                    break;
                }
                if (ptype != 5) { blk += psize; continue; }
                const quint64 pinfo = blk + pdata;
                if (!inRange(pinfo, 0x30)) return fail(QStringLiteral("Asset offset exceeds its container"));
                const auto pfmt = u16(pinfo + 4, ok), porder = u16(pinfo + 6, ok);
                const quint64 pcount = quint64(u16(pinfo + 8, ok)) * u16(pinfo + 10, ok);
                const quint64 entryBytes = pfmt <= 2 ? 2 : pfmt == 3 ? 4 : 0;
                if (!entryBytes) return fail(QStringLiteral("Unsupported GIM palette format 0x%1").arg(pfmt, 0, 16));
                if (porder != 0) return fail(QStringLiteral("Unsupported GIM palette order"));
                if (!pcount || pcount > 65536) return fail(QStringLiteral("Invalid GIM palette dimensions"));
                const quint64 pstart = pinfo + u32(pinfo + 0x1c, ok);
                if (pstart < pinfo + 0x30 || pstart > blk + psize || pcount * entryBytes > blk + psize - pstart)
                    return fail(QStringLiteral("GIM palette exceeds its block"));
                if (!inRange(pstart, pcount * entryBytes)) return fail(QStringLiteral("Asset offset exceeds its container"));
                palColours.reserve(int(pcount));
                for (quint64 i = 0; i < pcount; ++i) {
                    const quint64 pat = pstart + i * entryBytes;
                    if (pfmt == 3) palColours.push_back(qRgba(uchar(b[int(pat)]), uchar(b[int(pat) + 1]), uchar(b[int(pat) + 2]), uchar(b[int(pat) + 3])));
                    else {
                        const auto pv = u16(pat, ok);
                        palColours.push_back(pfmt == 0 ? c565(pv) : pfmt == 1 ? c5551(pv) : c4444(pv));
                    }
                }
                break;
            }
            const quint64 bits = format == 4 ? 4 : format == 5 ? 8 : 16;
            const quint64 rowBytes = (quint64(w) * bits + 7) / 8;
            const quint64 stride = ((rowBytes + alignment - 1) / alignment) * alignment;
            const quint64 length = stride * quint64(h);
            if (pixels > end || length > end - pixels) return fail(QStringLiteral("GIM index rows exceed image block"));
            if (!inRange(pixels, length)) return fail(QStringLiteral("Asset offset exceeds its container"));
            QImage result(w, h, QImage::Format_ARGB32);
            const int entries = int(palColours.size());
            for (int y = 0; y < h; ++y) {
                auto* row = reinterpret_cast<QRgb*>(result.scanLine(y));
                const auto* line = reinterpret_cast<const uchar*>(b.constData() + pixels + quint64(y) * stride);
                for (int x = 0; x < w; ++x) {
                    quint32 index = format == 4 ? ((x & 1) ? (line[x >> 1] & 15) : (line[x >> 1] >> 4))
                                  : format == 5 ? line[x]
                                  : qFromBigEndian<quint16>(line + x * 2);
                    if (entries) {
                        if (index >= quint32(entries))
                            return fail(QStringLiteral("GIM palette index %1 exceeds %2 entries").arg(index).arg(entries));
                        row[x] = palColours[int(index)];
                    } else {
                        const int cov = format == 4 ? int(index) * 17 : format == 5 ? int(index) : int(index >> 8);
                        row[x] = qRgba(255, 255, 255, cov);
                    }
                }
            }
            return result;
        }
        if (format != 3) return fail(QStringLiteral("Unsupported GIM pixel format 0x%1").arg(format, 0, 16));
        const quint64 stride = ((quint64(w) * 4 + alignment - 1) / alignment) * alignment;
        if (pixels < info || pixels + stride * h > end) return fail(QStringLiteral("GIM pixels exceed image block"));
        if (!inRange(pixels, stride * h)) return fail(QStringLiteral("Asset offset exceeds its container"));
        QImage result(w, h, QImage::Format_ARGB32);
        for (int y = 0; y < h; ++y) {
            auto* row = reinterpret_cast<QRgb*>(result.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const auto v = u32(pixels + y * stride + x * 4, ok);
                row[x] = qRgba(v >> 24, (v >> 16) & 255, (v >> 8) & 255, v & 255);
            }
        }
        return result;
    }
    return fail(QStringLiteral("GIM image block not found"));
}

QByteArray cppVagToWav(const QByteArray& left, const QByteArray& right = {}) {
    const auto decode = [](const QByteArray& v, quint32& rate) {
        qsizetype p = 0;
        rate = 44100;
        if (v.startsWith("VAGp") && v.size() >= 20) {
            const quint32 r = qFromBigEndian<quint32>(v.constData() + 16);
            if (r && r != 0xFFFFFFFFu) rate = r;
            p = 48;
        }
        static constexpr double k[5][2]{{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
        double s1 = 0, s2 = 0;
        QVector<qint16> out;
        for (; p + 16 <= v.size(); p += 16) {
            const auto* b = reinterpret_cast<const uchar*>(v.constData() + p);
            if (b[1] == 7) break;
            const int shift = std::min(b[0] & 15, 12);
            const auto& c = k[std::min(b[0] >> 4, 4)];
            for (int i = 0; i < 28; ++i) {
                const int nibble = i % 2 ? b[2 + i / 2] >> 4 : b[2 + i / 2] & 15;
                const int raw = qint16(quint16(nibble << 12)) >> 4;
                const double x = std::clamp(std::round(double((raw * 16) >> shift) + (s1 * c[0] + s2 * c[1]) / 64.), -32768., 32767.);
                out.push_back(qint16(x));
                s2 = s1;
                s1 = x;
            }
            if (b[1] == 1) break;
        }
        return out;
    };
    quint32 rate = 44100, unused = 0;
    const auto l = decode(left, rate);
    if (l.isEmpty()) return {};
    const auto r = right.isEmpty() ? QVector<qint16>{} : decode(right, unused);
    const quint16 channels = r.isEmpty() ? 1 : 2;
    const qsizetype frames = r.isEmpty() ? l.size() : std::min(l.size(), r.size());
    const quint32 bytes = quint32(frames) * channels * 2;
    QByteArray w;
    w.reserve(44 + bytes);
    const auto u32 = [&](quint32 v) { char b[4]; qToLittleEndian(v, b); w.append(b, 4); };
    const auto u16 = [&](quint16 v) { char b[2]; qToLittleEndian(v, b); w.append(b, 2); };
    w += "RIFF"; u32(36 + bytes); w += "WAVEfmt "; u32(16); u16(1); u16(channels); u32(rate); u32(rate * channels * 2); u16(channels * 2); u16(16);
    w += "data"; u32(bytes);
    for (qsizetype i = 0; i < frames; ++i) { u16(quint16(l[i])); if (channels == 2) u16(quint16(r[i])); }
    return w;
}

QByteArray makeVagStream(quint32 sampleRate, bool withHeader, uchar endFlag, int seed) {
    QByteArray out;
    if (withHeader) {
        out.resize(48, '\0');
        out.replace(0, 4, "VAGp");
        put32(out, 16, sampleRate);
    }
    for (int blk = 0; blk < 5; ++blk) {
        QByteArray b(16, '\0');
        b[0] = bchar(((blk % 5) << 4) | ((blk + 1) & 7));
        b[1] = bchar(blk == 4 ? endFlag : 0);
        for (int i = 0; i < 14; ++i) {
            const int lo = (seed + blk * 7 + i * 3) & 15;
            const int hi = (15 - ((seed + blk * 5 + i) & 15)) & 15;
            b[2 + i] = bchar((hi << 4) | lo);
        }
        out += b;
    }
    return out;
}

QByteArray buildSyntheticQrc(const QByteArray& id, const QByteArray& payload, bool compressEntry, bool wrapQrcc, quint32 backRefOverride = 0) {
    const QByteArray storedPayload = compressEntry ? qCompress(payload).mid(4) : payload;
    const QByteArray strings("id\0src\0size\0", 12);
    QByteArray ids(4, '\0');
    put32(ids, 0, backRefOverride);
    ids += id + '\0';
    const quint32 attrCount = compressEntry ? 3 : 2;
    QByteArray toc(28 + int(attrCount) * 16, '\0');
    put32(toc, 4, attrCount);
    // attr 0: id (name off 0, type 7, a=0, b=0)
    put32(toc, 28 + 0, 0); put32(toc, 28 + 4, 7); put32(toc, 28 + 8, 0); put32(toc, 28 + 12, 0);
    // attr 1: src (name off 3, type 6, a=0, b=storedPayload.size())
    put32(toc, 44 + 0, 3); put32(toc, 44 + 4, 6); put32(toc, 44 + 8, 0); put32(toc, 44 + 12, quint32(storedPayload.size()));
    if (compressEntry) {
        // attr 2: size (name off 7, type 1, a=payload.size(), b=0)
        put32(toc, 60 + 0, 7); put32(toc, 60 + 4, 1); put32(toc, 60 + 8, quint32(payload.size())); put32(toc, 60 + 12, 0);
    }
    const quint32 tocOff = 56;
    const quint32 idsOff = tocOff + quint32(toc.size());
    const quint32 stringsOff = idsOff + quint32(ids.size());
    const quint32 dataOff = stringsOff + quint32(strings.size());
    QByteArray qrcf(56, '\0');
    qrcf.replace(0, 4, "QRCF");
    put32(qrcf, 4, 0x110);
    put32(qrcf, 8, tocOff); put32(qrcf, 12, quint32(toc.size()));
    put32(qrcf, 16, idsOff); put32(qrcf, 20, quint32(ids.size()));
    put32(qrcf, 24, stringsOff); put32(qrcf, 28, quint32(strings.size()));
    put32(qrcf, 48, dataOff); put32(qrcf, 52, quint32(storedPayload.size()));
    qrcf += toc + ids + strings + storedPayload;
    if (!wrapQrcc) return qrcf;
    QByteArray qrcc(8, '\0');
    qrcc.replace(0, 4, "QRCC");
    put32(qrcc, 4, quint32(qrcf.size()));
    qrcc += qCompress(qrcf).mid(4);
    return qrcc;
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

    // ---- 5. Phase 4: GIM (.GIM1.00) and VAG (.vag / VAGp) parity --------
    {
        const QByteArray gimInputs[] = {
            // RGBA8888 (format 3) with 8-byte row alignment
            wrapGimRoot(makeGimImageBlock(3, 2, 2, 8, QByteArray::fromHex("10203040ff0080ff50607080010203ff"))),
            // INDEX4 (format 4) with RGB565 palette (format 0)
            wrapGimRoot(makeGimImageBlock(4, 2, 2, 4, QByteArray::fromHex("0100000010000000")) +
                        makeGimPaletteBlock(0, 2, QByteArray::fromHex("f80007e0"))),
            // INDEX8 (format 5) with RGBA8888 palette (format 3)
            wrapGimRoot(makeGimImageBlock(5, 2, 2, 4, QByteArray::fromHex("0001000001020000")) +
                        makeGimPaletteBlock(3, 3, QByteArray::fromHex("ff00008000ff00ff0000ffff"))),
            // INDEX8 (format 5) palette-less alpha coverage mask (OSK RCO mask form)
            wrapGimRoot(makeGimImageBlock(5, 2, 2, 4, QByteArray::fromHex("0040000080ff0000"))),
            // INDEX16 (format 6) with RGBA4444 palette (format 2)
            wrapGimRoot(makeGimImageBlock(6, 2, 1, 4, QByteArray::fromHex("00000001")) +
                        makeGimPaletteBlock(2, 2, QByteArray::fromHex("f00f0f0f"))),
            // DXT1 4-colour (format 8, c0 > c1)
            wrapGimRoot(makeGimImageBlock(8, 4, 4, 8, QByteArray::fromHex("00f81f000055aaff"))),
            // DXT1 3-colour + transparent black (format 8, c0 <= c1)
            wrapGimRoot(makeGimImageBlock(8, 4, 4, 8, QByteArray::fromHex("1f0000f8ffffffff"))),
            // DXT3 explicit 4-bit alpha (format 9)
            wrapGimRoot(makeGimImageBlock(9, 4, 4, 16, QByteArray::fromHex("8888888888888888ffffffff00000000"))),
            // DXT5 interpolated alpha (format 10)
            wrapGimRoot(makeGimImageBlock(10, 4, 4, 16, QByteArray::fromHex("ff00499224499224ffffffff00000000"))),
            // Refusals: wrong magic, bad block size, out-of-range palette index
            QByteArray("MIG.00.1PSP\0\0\0\0\0", 16),
            QByteArray(".GIM1.00\0\0\0\0\0\0\0\0\0\x02\0\0\0\0\0\x04\0\0\0\x10\0\0\0\x10", 32),
            wrapGimRoot(makeGimImageBlock(5, 2, 1, 4, QByteArray::fromHex("00050000")) +
                        makeGimPaletteBlock(0, 2, QByteArray::fromHex("f80007e0"))),
        };
        int gimi = 0;
        for (const QByteArray& g : gimInputs) {
            QString eCpp, eRust;
            const QImage iCpp = cppDecodeGim(g, &eCpp);
            const QImage iRust = xmb::image_rust::decodeGim(g, &eRust);
            const QByteArray label = QByteArray("gim-parity-") + QByteArray::number(gimi++);
            check(iCpp.isNull() == iRust.isNull() && iCpp == iRust && eCpp == eRust, label.constData());
        }

        const QByteArray vagL = makeVagStream(48000, true, 1, 3);
        const QByteArray vagR = makeVagStream(48000, true, 1, 11);
        const QByteArray vagRaw = makeVagStream(44100, false, 7, 5);
        check(cppVagToWav(vagL) == xmb::image_rust::vagToWav(vagL) && !cppVagToWav(vagL).isEmpty(), "vag-parity-mono-vagp");
        check(cppVagToWav(vagL, vagR) == xmb::image_rust::vagToWav(vagL, vagR) && !cppVagToWav(vagL, vagR).isEmpty(), "vag-parity-stereo-vagp");
        check(cppVagToWav(vagRaw) == xmb::image_rust::vagToWav(vagRaw) && !cppVagToWav(vagRaw).isEmpty(), "vag-parity-headless-flag7");
        check(cppVagToWav(QByteArray()).isEmpty() && xmb::image_rust::vagToWav(QByteArray()).isEmpty(), "vag-parity-empty");
    }

    // ---- 6. Phase 5: Sony CXML QRC (QRCC/QRCF) container parity ----------
    {
        const QByteArray payload("#MNU_1.0\nCOLOR:float:0.75\n");
        const QByteArray qrcc = buildSyntheticQrc("override/day/ICONS.mnu", payload, true, true);
        const QString qrcPath = temp.path() + "/test.qrc";
        check(writeBytes(qrcPath, qrcc), "qrc-write-synthetic-qrcc");
        xmb::QrcArchive cppArc(qrcPath);
        const QByteArray cppEntry = cppArc.file(QStringLiteral("override/day/ICONS.mnu"));

        QByteArray rustUnpacked;
        quint32 rustDataBase = 0;
        QHash<QString, xmb::image_rust::QrcTocEntry> rustEntries;
        QString rustErr;
        const bool okRust = xmb::image_rust::parseQrc(qrcc, rustUnpacked, rustDataBase, rustEntries, &rustErr);
        QByteArray rustEntry;
        if (okRust && rustEntries.contains(QStringLiteral("override/day/ICONS.mnu"))) {
            const auto e = rustEntries.value(QStringLiteral("override/day/ICONS.mnu"));
            xmb::image_rust::inflateQrcEntry(rustUnpacked.constData() + rustDataBase + e.offset, e.size, e.expanded, rustEntry, &rustErr);
        }
        check(okRust && cppEntry == payload && rustEntry == cppEntry && cppArc.names() == rustEntries.keys(), "qrc-parity-qrcc-compressed-entry");

        const QByteArray qrcfRaw = buildSyntheticQrc("lib/raf/vert0.vpo", QByteArray("vpo-bytes", 9), false, false);
        check(writeBytes(qrcPath, qrcfRaw), "qrc-write-synthetic-qrcf");
        xmb::QrcArchive cppArcRaw(qrcPath);
        check(xmb::image_rust::parseQrc(qrcfRaw, rustUnpacked, rustDataBase, rustEntries, &rustErr) &&
                  rustEntries.contains(QStringLiteral("lib/raf/vert0.vpo")) &&
                  rustUnpacked.mid(rustDataBase + rustEntries.value(QStringLiteral("lib/raf/vert0.vpo")).offset,
                                   rustEntries.value(QStringLiteral("lib/raf/vert0.vpo")).size) ==
                      cppArcRaw.file(QStringLiteral("lib/raf/vert0.vpo")),
              "qrc-parity-qrcf-uncompressed-entry");

        const QByteArray badBackRef = buildSyntheticQrc("lib/raf/vert0.vpo", QByteArray("cg"), false, false, 99);
        bool cppThrew = false;
        QString cppThrowMsg;
        writeBytes(qrcPath, badBackRef);
        try { xmb::QrcArchive badArc(qrcPath); } catch (const std::exception& ex) { cppThrew = true; cppThrowMsg = QString::fromUtf8(ex.what()); }
        const bool rustBad = !xmb::image_rust::parseQrc(badBackRef, rustUnpacked, rustDataBase, rustEntries, &rustErr);
        check(cppThrew && rustBad && cppThrowMsg == rustErr, "qrc-parity-reject-bad-backref");

        const QByteArray badVer("QRCF\0\0\x02\0", 8);
        cppThrew = false;
        writeBytes(qrcPath, badVer);
        try { xmb::QrcArchive badArc(qrcPath); } catch (const std::exception& ex) { cppThrew = true; cppThrowMsg = QString::fromUtf8(ex.what()); }
        check(cppThrew && !xmb::image_rust::parseQrc(badVer, rustUnpacked, rustDataBase, rustEntries, &rustErr) && cppThrowMsg == rustErr,
              "qrc-parity-reject-unsupported-version");
    }

    std::printf("rust-edge-parity: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
