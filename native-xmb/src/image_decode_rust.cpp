#include "image_decode_rust.h"
#include "image_decode_abi.h"
#include <QtEndian>
#include <algorithm>
#include <bit>
#include <cstring>

namespace xmb::image_rust {
namespace {

struct ImgBufferGuard {
    uint8_t* ptr = nullptr;
    size_t len = 0;
    ~ImgBufferGuard() {
        if (ptr) img_free_buffer(ptr, len);
    }
};

QString lastMessage() {
    const size_t len = img_last_error(nullptr, 0);
    if (len == 0) return {};
    QByteArray utf8(static_cast<qsizetype>(std::min<size_t>(len, 4096)), Qt::Uninitialized);
    const size_t copied = img_last_error(reinterpret_cast<uint8_t*>(utf8.data()), static_cast<size_t>(utf8.size()));
    utf8.truncate(static_cast<qsizetype>(copied));
    return QString::fromUtf8(utf8);
}

bool unpackCues(const uint8_t* data, size_t size, QVector<ImageCue>& out) {
    out.clear();
    if (!data || size < 4) return false;
    size_t pos = 0;
    const uint32_t count = qFromLittleEndian<uint32_t>(data + pos);
    pos += 4;
    out.reserve(static_cast<qsizetype>(std::min<uint32_t>(count, 20000)));
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 43 > size) { out.clear(); return false; }
        ImageCue cue;
        cue.startMs = qFromLittleEndian<qint64>(data + pos); pos += 8;
        cue.endMs = qFromLittleEndian<qint64>(data + pos); pos += 8;
        const qint32 rx = qFromLittleEndian<qint32>(data + pos); pos += 4;
        const qint32 ry = qFromLittleEndian<qint32>(data + pos); pos += 4;
        const qint32 rw = qFromLittleEndian<qint32>(data + pos); pos += 4;
        const qint32 rh = qFromLittleEndian<qint32>(data + pos); pos += 4;
        const qint32 vw = qFromLittleEndian<qint32>(data + pos); pos += 4;
        const qint32 vh = qFromLittleEndian<qint32>(data + pos); pos += 4;
        cue.forced = (data[pos++] != 0);
        const uint16_t palLen = qFromLittleEndian<uint16_t>(data + pos); pos += 2;
        if (rw <= 0 || rh <= 0 || palLen > 256 || size_t(palLen) * 4 > size - pos) { out.clear(); return false; }
        QVector<QRgb> table(palLen);
        for (uint16_t k = 0; k < palLen; ++k) {
            table[k] = qFromLittleEndian<quint32>(data + pos);
            pos += 4;
        }
        if (pos + 4 > size) { out.clear(); return false; }
        const uint32_t pxLen = qFromLittleEndian<uint32_t>(data + pos); pos += 4;
        const uint64_t expectedPx = uint64_t(uint32_t(rw)) * uint64_t(uint32_t(rh));
        if (pxLen != expectedPx || pxLen > size - pos) { out.clear(); return false; }
        QImage image(rw, rh, QImage::Format_Indexed8);
        if (image.isNull()) { out.clear(); return false; }
        image.setColorTable(table);
        for (int y = 0; y < rh; ++y) {
            std::memcpy(image.scanLine(y), data + pos + size_t(y) * size_t(rw), size_t(rw));
        }
        pos += pxLen;
        cue.image = std::move(image);
        cue.rect = QRect(rx, ry, rw, rh);
        cue.videoSize = QSize(vw, vh);
        out.push_back(std::move(cue));
    }
    if (pos != size) { out.clear(); return false; }
    return true;
}

} // namespace

QImage decodeGtf(const QByteArray& gtf, QString* error) {
    if (error) error->clear();
    uint32_t width = 0, height = 0;
    ImgBufferGuard guard;
    const auto* ptr = gtf.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(gtf.constData());
    const int rc = img_decode_gtf(ptr, static_cast<size_t>(gtf.size()), &width, &height, &guard.ptr, &guard.len);
    if (rc != 0) {
        if (error) *error = lastMessage();
        return {};
    }
    const uint64_t rowBytes = uint64_t(width) * 4u;
    if (!guard.ptr || width == 0 || height == 0 || guard.len != rowBytes * uint64_t(height)) {
        if (error) *error = QStringLiteral("corrupt Rust GTF payload");
        return {};
    }
    QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_ARGB32);
    if (image.isNull()) {
        if (error) *error = QStringLiteral("cannot allocate GTF image");
        return {};
    }
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(image.scanLine(static_cast<int>(y)), guard.ptr + size_t(y) * size_t(rowBytes), size_t(rowBytes));
    }
    return image;
}

bool decodeEdge(const QByteArray& edge, QVector<EdgeVertex>& vertices, QVector<quint32>& indices, QString* error) {
    vertices.clear();
    indices.clear();
    if (error) error->clear();
    ImgBufferGuard guard;
    const auto* ptr = edge.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(edge.constData());
    const int rc = img_decode_edge(ptr, static_cast<size_t>(edge.size()), &guard.ptr, &guard.len);
    if (rc != 0) {
        if (error) *error = lastMessage();
        return false;
    }
    if (!guard.ptr || guard.len < 8) {
        if (error) *error = QStringLiteral("corrupt Rust Edge payload");
        return false;
    }
    const uint32_t vCount = qFromLittleEndian<uint32_t>(guard.ptr);
    const uint32_t iCount = qFromLittleEndian<uint32_t>(guard.ptr + 4);
    const uint64_t expected = 8u + uint64_t(vCount) * 32u + uint64_t(iCount) * 4u;
    if (guard.len != expected) {
        if (error) *error = QStringLiteral("corrupt Rust Edge payload length");
        return false;
    }
    const auto readF32 = [&](size_t off) {
        return std::bit_cast<float>(qFromLittleEndian<uint32_t>(guard.ptr + off));
    };
    vertices.reserve(static_cast<qsizetype>(vCount));
    size_t pos = 8;
    for (uint32_t i = 0; i < vCount; ++i, pos += 32) {
        EdgeVertex v;
        v.position = {readF32(pos), readF32(pos + 4), readF32(pos + 8)};
        v.normal = {readF32(pos + 12), readF32(pos + 16), readF32(pos + 20)};
        v.uv = {readF32(pos + 24), readF32(pos + 28)};
        vertices.push_back(v);
    }
    indices.reserve(static_cast<qsizetype>(iCount));
    for (uint32_t i = 0; i < iCount; ++i, pos += 4) {
        indices.push_back(qFromLittleEndian<uint32_t>(guard.ptr + pos));
    }
    return true;
}

bool parsePgs(const QByteArray& sup, QVector<ImageCue>& out, QString* reason) {
    out.clear();
    if (reason) reason->clear();
    ImgBufferGuard guard;
    const auto* ptr = sup.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(sup.constData());
    const int rc = img_parse_pgs(ptr, static_cast<size_t>(sup.size()), &guard.ptr, &guard.len);
    const QString msg = lastMessage();
    if (rc != 0) {
        if (reason) *reason = msg;
        return false;
    }
    if (!unpackCues(guard.ptr, guard.len, out)) {
        if (reason) *reason = QStringLiteral("corrupt Rust PGS cue payload");
        return false;
    }
    if (reason) *reason = msg;
    return true;
}

bool parseVobSub(const QByteArray& idx, const QByteArray& sub, int track, QVector<ImageCue>& out, QString* reason) {
    out.clear();
    if (reason) reason->clear();
    ImgBufferGuard guard;
    const auto* idxPtr = idx.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(idx.constData());
    const auto* subPtr = sub.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(sub.constData());
    const int rc = img_parse_vobsub(
        idxPtr, static_cast<size_t>(idx.size()), subPtr, static_cast<size_t>(sub.size()), track, &guard.ptr, &guard.len);
    const QString msg = lastMessage();
    if (rc != 0) {
        if (reason) *reason = msg;
        return false;
    }
    if (!unpackCues(guard.ptr, guard.len, out)) {
        if (reason) *reason = QStringLiteral("corrupt Rust VobSub cue payload");
        return false;
    }
    if (reason) *reason = msg;
    return true;
}

QImage decodeGim(const QByteArray& gim, QString* error) {
    if (error) error->clear();
    uint32_t width = 0, height = 0;
    ImgBufferGuard guard;
    const auto* ptr = gim.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(gim.constData());
    const int rc = img_decode_gim(ptr, static_cast<size_t>(gim.size()), &width, &height, &guard.ptr, &guard.len);
    if (rc != 0) {
        if (error) *error = lastMessage();
        return {};
    }
    const uint64_t rowBytes = uint64_t(width) * 4u;
    if (!guard.ptr || width == 0 || height == 0 || guard.len != rowBytes * uint64_t(height)) {
        if (error) *error = QStringLiteral("corrupt Rust GIM payload");
        return {};
    }
    QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_ARGB32);
    if (image.isNull()) {
        if (error) *error = QStringLiteral("cannot allocate GIM image");
        return {};
    }
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(image.scanLine(static_cast<int>(y)), guard.ptr + size_t(y) * size_t(rowBytes), size_t(rowBytes));
    }
    return image;
}

QByteArray vagToWav(const QByteArray& left, const QByteArray& right) {
    ImgBufferGuard guard;
    const auto* lPtr = left.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(left.constData());
    const auto* rPtr = right.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(right.constData());
    const int rc = img_vag_to_wav(
        lPtr, static_cast<size_t>(left.size()), rPtr, static_cast<size_t>(right.size()), &guard.ptr, &guard.len);
    if (rc != 0 || !guard.ptr || guard.len == 0) return {};
    return QByteArray(reinterpret_cast<const char*>(guard.ptr), static_cast<qsizetype>(guard.len));
}

bool parseQrc(const QByteArray& rawQrc, QByteArray& unpackedQrcf, quint32& dataBase, QHash<QString, QrcTocEntry>& entries, QString* error) {
    unpackedQrcf.clear();
    dataBase = 0;
    entries.clear();
    if (error) error->clear();
    ImgBufferGuard unpackedGuard, tocGuard;
    const auto* ptr = rawQrc.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(rawQrc.constData());
    const int rc = img_parse_qrc(
        ptr, static_cast<size_t>(rawQrc.size()), &unpackedGuard.ptr, &unpackedGuard.len, &tocGuard.ptr, &tocGuard.len);
    if (rc != 0) {
        if (error) *error = lastMessage();
        return false;
    }
    if (!unpackedGuard.ptr || !tocGuard.ptr || tocGuard.len < 8) {
        if (error) *error = QStringLiteral("corrupt Rust QRC TOC payload");
        return false;
    }
    dataBase = qFromLittleEndian<quint32>(tocGuard.ptr);
    const quint32 count = qFromLittleEndian<quint32>(tocGuard.ptr + 4);
    size_t pos = 8;
    entries.reserve(static_cast<qsizetype>(std::min<quint32>(count, 65536)));
    for (quint32 i = 0; i < count; ++i) {
        if (pos + 4 > tocGuard.len) { entries.clear(); return false; }
        const quint32 idLen = qFromLittleEndian<quint32>(tocGuard.ptr + pos);
        pos += 4;
        if (idLen > tocGuard.len - pos || pos + idLen + 12 > tocGuard.len) { entries.clear(); return false; }
        const QString id = QString::fromUtf8(reinterpret_cast<const char*>(tocGuard.ptr + pos), static_cast<qsizetype>(idLen));
        pos += idLen;
        QrcTocEntry e;
        e.offset = qFromLittleEndian<quint32>(tocGuard.ptr + pos); pos += 4;
        e.size = qFromLittleEndian<quint32>(tocGuard.ptr + pos); pos += 4;
        e.expanded = qFromLittleEndian<quint32>(tocGuard.ptr + pos); pos += 4;
        entries.insert(id, e);
    }
    if (pos != tocGuard.len) { entries.clear(); return false; }
    unpackedQrcf = QByteArray(reinterpret_cast<const char*>(unpackedGuard.ptr), static_cast<qsizetype>(unpackedGuard.len));
    return true;
}

bool inflateQrcEntry(const char* ptr, qsizetype len, quint32 expanded, QByteArray& out, QString* error) {
    out.clear();
    if (error) error->clear();
    ImgBufferGuard guard;
    const auto* uptr = (len <= 0 || !ptr) ? nullptr : reinterpret_cast<const uint8_t*>(ptr);
    const int rc = img_inflate_qrc_entry(uptr, static_cast<size_t>(std::max<qsizetype>(0, len)), expanded, &guard.ptr, &guard.len);
    if (rc != 0) {
        if (error) *error = lastMessage();
        return false;
    }
    if (!guard.ptr || guard.len != size_t(expanded)) {
        if (error) *error = QStringLiteral("QRC expanded length mismatch");
        return false;
    }
    out = QByteArray(reinterpret_cast<const char*>(guard.ptr), static_cast<qsizetype>(guard.len));
    return true;
}

bool parseCxml(const QByteArray& rawCxml, ParsedCxmlContainer& out, QString* error) {
    out = {};
    if (error) error->clear();
    ImgBufferGuard unpackedGuard, treeGuard;
    const auto* ptr = rawCxml.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(rawCxml.constData());
    const int rc = img_parse_cxml(
        ptr, static_cast<size_t>(rawCxml.size()), &unpackedGuard.ptr, &unpackedGuard.len, &treeGuard.ptr, &treeGuard.len);
    if (rc != 0) {
        if (error) *error = lastMessage();
        return false;
    }
    if (!unpackedGuard.ptr || !treeGuard.ptr || treeGuard.len < 58) {
        if (error) *error = QStringLiteral("corrupt Rust CXML tree payload");
        return false;
    }
    const uint8_t* p = treeGuard.ptr;
    const size_t n = treeGuard.len;
    size_t pos = 0;
    const uint8_t containerKind = p[pos++];
    out.container = containerKind == 0 ? QStringLiteral("P3TF")
                  : containerKind == 2 ? QStringLiteral("_RAF")
                                       : QStringLiteral("RAFO");
    out.compressedScene = p[pos++] != 0;
    out.version = qFromLittleEndian<quint32>(p + pos);
    pos += 4;
    out.sections.reserve(6);
    for (int i = 0; i < 6; ++i) {
        const quint32 off = qFromLittleEndian<quint32>(p + pos);
        const quint32 sz = qFromLittleEndian<quint32>(p + pos + 4);
        pos += 8;
        out.sections.push_back(P3t::Raf::Section{off, sz});
    }
    const quint32 elemCount = qFromLittleEndian<quint32>(p + pos);
    pos += 4;
    out.elements.reserve(static_cast<qsizetype>(std::min<quint32>(elemCount, 200000)));
    for (quint32 ei = 0; ei < elemCount; ++ei) {
        if (pos + 12 > n) return false;
        P3t::Element elem;
        elem.depth = qFromLittleEndian<qint32>(p + pos);
        pos += 4;
        const quint32 nameLen = qFromLittleEndian<quint32>(p + pos);
        pos += 4;
        if (nameLen > n - pos || pos + nameLen + 4 > n) return false;
        elem.name = QString::fromUtf8(reinterpret_cast<const char*>(p + pos), static_cast<qsizetype>(nameLen));
        pos += nameLen;
        const quint32 attrCount = qFromLittleEndian<quint32>(p + pos);
        pos += 4;
        for (quint32 ai = 0; ai < attrCount; ++ai) {
            if (pos + 5 > n) return false;
            const quint32 keyLen = qFromLittleEndian<quint32>(p + pos);
            pos += 4;
            if (keyLen > n - pos || pos + keyLen + 1 > n) return false;
            const QString key = QString::fromUtf8(reinterpret_cast<const char*>(p + pos), static_cast<qsizetype>(keyLen));
            pos += keyLen;
            const uint8_t kind = p[pos++];
            P3t::Attribute attr;
            if (kind == 0) {
                if (pos + 8 > n) return false;
                attr.kind = P3t::Attribute::Kind::Int;
                attr.integer = qFromLittleEndian<qint64>(p + pos);
                pos += 8;
            } else if (kind == 1) {
                if (pos + 8 > n) return false;
                attr.kind = P3t::Attribute::Kind::Float;
                uint64_t bits = qFromLittleEndian<quint64>(p + pos);
                std::memcpy(&attr.real, &bits, sizeof(double));
                pos += 8;
            } else if (kind == 2) {
                if (pos + 4 > n) return false;
                const quint32 sLen = qFromLittleEndian<quint32>(p + pos);
                pos += 4;
                if (sLen > n - pos) return false;
                attr.kind = P3t::Attribute::Kind::String;
                attr.text = QString::fromUtf8(reinterpret_cast<const char*>(p + pos), static_cast<qsizetype>(sLen));
                pos += sLen;
            } else if (kind == 3) {
                if (pos + 4 > n) return false;
                const quint32 count = qFromLittleEndian<quint32>(p + pos);
                pos += 4;
                if (uint64_t(count) * 8u > n - pos) return false;
                attr.kind = P3t::Attribute::Kind::Ints;
                attr.integers.reserve(static_cast<qsizetype>(count));
                for (quint32 k = 0; k < count; ++k) {
                    attr.integers.push_back(qFromLittleEndian<qint64>(p + pos));
                    pos += 8;
                }
            } else if (kind == 4) {
                if (pos + 4 > n) return false;
                const quint32 count = qFromLittleEndian<quint32>(p + pos);
                pos += 4;
                if (uint64_t(count) * 8u > n - pos) return false;
                attr.kind = P3t::Attribute::Kind::Floats;
                attr.reals.reserve(static_cast<qsizetype>(count));
                for (quint32 k = 0; k < count; ++k) {
                    uint64_t bits = qFromLittleEndian<quint64>(p + pos);
                    double d = 0.0;
                    std::memcpy(&d, &bits, sizeof(double));
                    attr.reals.push_back(d);
                    pos += 8;
                }
            } else if (kind == 5) {
                if (pos + 8 > n) return false;
                attr.kind = P3t::Attribute::Kind::File;
                attr.offset = qFromLittleEndian<quint32>(p + pos);
                attr.size = qFromLittleEndian<quint32>(p + pos + 4);
                pos += 8;
            } else {
                return false;
            }
            elem.attributes.insert(key, std::move(attr));
        }
        out.elements.push_back(std::move(elem));
    }
    if (pos != n) return false;
    out.unpacked = QByteArray(reinterpret_cast<const char*>(unpackedGuard.ptr), static_cast<qsizetype>(unpackedGuard.len));
    return true;
}

QByteArray inflateCxmlFile(const QByteArray& packed) {
    if (packed.isEmpty()) return {};
    ImgBufferGuard guard;
    const auto* ptr = reinterpret_cast<const uint8_t*>(packed.constData());
    const int rc = img_inflate_cxml_file(ptr, static_cast<size_t>(packed.size()), &guard.ptr, &guard.len);
    if (rc != 0 || !guard.ptr || guard.len == 0) return {};
    return QByteArray(reinterpret_cast<const char*>(guard.ptr), static_cast<qsizetype>(guard.len));
}

bool parseSfo(const QByteArray& rawSfo, QHash<QString, QString>& out, QString* error) {
    out.clear();
    if (error) error->clear();
    ImgBufferGuard guard;
    const auto* ptr = rawSfo.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(rawSfo.constData());
    const int rc = img_parse_sfo(ptr, static_cast<size_t>(rawSfo.size()), &guard.ptr, &guard.len);
    if (rc != 0) {
        if (error) *error = lastMessage();
        return false;
    }
    if (!guard.ptr || guard.len < 4) {
        if (error) *error = QStringLiteral("corrupt Rust PARAM.SFO payload");
        return false;
    }
    const quint32 count = qFromLittleEndian<quint32>(guard.ptr);
    size_t pos = 4;
    out.reserve(static_cast<qsizetype>(std::min<quint32>(count, 256)));
    for (quint32 i = 0; i < count; ++i) {
        if (pos + 4 > guard.len) { out.clear(); return false; }
        const quint32 kLen = qFromLittleEndian<quint32>(guard.ptr + pos);
        pos += 4;
        if (kLen > guard.len - pos || pos + kLen + 4 > guard.len) { out.clear(); return false; }
        const QString key = QString::fromUtf8(reinterpret_cast<const char*>(guard.ptr + pos), static_cast<qsizetype>(kLen));
        pos += kLen;
        const quint32 vLen = qFromLittleEndian<quint32>(guard.ptr + pos);
        pos += 4;
        if (vLen > guard.len - pos) { out.clear(); return false; }
        const QString val = QString::fromUtf8(reinterpret_cast<const char*>(guard.ptr + pos), static_cast<qsizetype>(vLen));
        pos += vLen;
        out.insert(key, val);
    }
    if (pos != guard.len) { out.clear(); return false; }
    return true;
}

QImage decodeDds(const QByteArray& dds, QString* error) {
    if (error) error->clear();
    uint32_t width = 0, height = 0;
    ImgBufferGuard guard;
    const auto* ptr = dds.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(dds.constData());
    const int rc = img_decode_dds(ptr, static_cast<size_t>(dds.size()), &width, &height, &guard.ptr, &guard.len);
    if (rc != 0) {
        if (error) *error = lastMessage();
        return {};
    }
    const uint64_t rowBytes = uint64_t(width) * 4u;
    if (!guard.ptr || width == 0 || height == 0 || guard.len != rowBytes * uint64_t(height)) {
        if (error) *error = QStringLiteral("corrupt Rust DDS payload");
        return {};
    }
    QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_ARGB32);
    if (image.isNull()) {
        if (error) *error = QStringLiteral("cannot allocate DDS image");
        return {};
    }
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(image.scanLine(static_cast<int>(y)), guard.ptr + size_t(y) * size_t(rowBytes), size_t(rowBytes));
    }
    return image;
}

QImage decodeTga(const QByteArray& tga, QString* error) {
    if (error) error->clear();
    uint32_t width = 0, height = 0;
    ImgBufferGuard guard;
    const auto* ptr = tga.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(tga.constData());
    const int rc = img_decode_tga(ptr, static_cast<size_t>(tga.size()), &width, &height, &guard.ptr, &guard.len);
    if (rc != 0) {
        if (error) *error = lastMessage();
        return {};
    }
    const uint64_t rowBytes = uint64_t(width) * 4u;
    if (!guard.ptr || width == 0 || height == 0 || guard.len != rowBytes * uint64_t(height)) {
        if (error) *error = QStringLiteral("corrupt Rust TGA payload");
        return {};
    }
    QImage image(static_cast<int>(width), static_cast<int>(height), QImage::Format_ARGB32);
    if (image.isNull()) {
        if (error) *error = QStringLiteral("cannot allocate TGA image");
        return {};
    }
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(image.scanLine(static_cast<int>(y)), guard.ptr + size_t(y) * size_t(rowBytes), size_t(rowBytes));
    }
    return image;
}

bool parseRco(const QByteArray& file, RcoContainer& out, QString* error) {
    out = {};
    if (error) error->clear();
    ImgBufferGuard tablesGuard, tocGuard;
    const auto* ptr = file.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(file.constData());
    const int rc = img_parse_rco(ptr, static_cast<size_t>(file.size()), &tablesGuard.ptr, &tablesGuard.len, &tocGuard.ptr, &tocGuard.len);
    if (rc != 0) {
        if (error) *error = lastMessage();
        return false;
    }
    if (!tablesGuard.ptr || !tocGuard.ptr || tocGuard.len < 12) {
        if (error) *error = QStringLiteral("corrupt Rust RCO payload");
        return false;
    }
    const uint8_t* p = tocGuard.ptr;
    const size_t n = tocGuard.len;
    out.labels = qFromLittleEndian<quint32>(p);
    out.imageData = qFromLittleEndian<quint32>(p + 4);
    const quint32 count = qFromLittleEndian<quint32>(p + 8);
    size_t pos = 12;
    out.entries.reserve(static_cast<qsizetype>(std::min<quint32>(count, 65536)));
    for (quint32 i = 0; i < count; ++i) {
        if (pos + 4 > n) { out = {}; return false; }
        const quint32 nameLen = qFromLittleEndian<quint32>(p + pos);
        pos += 4;
        if (nameLen > n - pos || pos + nameLen + 4 > n) { out = {}; return false; }
        const QString name = QString::fromUtf8(reinterpret_cast<const char*>(p + pos), static_cast<qsizetype>(nameLen));
        pos += nameLen;
        out.entries.insert(name, qFromLittleEndian<quint32>(p + pos));
        pos += 4;
    }
    if (pos != n) { out = {}; if (error) *error = QStringLiteral("corrupt Rust RCO payload length"); return false; }
    out.file = file;
    out.tables = QByteArray(reinterpret_cast<const char*>(tablesGuard.ptr), static_cast<qsizetype>(tablesGuard.len));
    return true;
}

bool rcoEnglish(const RcoContainer& container, QHash<QString, QString>& out, QString* error) {
    out.clear();
    if (error) error->clear();
    ImgBufferGuard guard;
    const auto* filePtr = container.file.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(container.file.constData());
    const auto* tablesPtr = container.tables.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(container.tables.constData());
    const int rc = img_rco_english(filePtr, static_cast<size_t>(container.file.size()), tablesPtr,
                                   static_cast<size_t>(container.tables.size()), container.labels, &guard.ptr, &guard.len);
    if (rc != 0) {
        if (error) *error = lastMessage();
        return false;
    }
    if (!guard.ptr || guard.len < 4) {
        if (error) *error = QStringLiteral("corrupt Rust RCO text payload");
        return false;
    }
    const uint8_t* p = guard.ptr;
    const size_t n = guard.len;
    const quint32 count = qFromLittleEndian<quint32>(p);
    size_t pos = 4;
    out.reserve(static_cast<qsizetype>(std::min<quint32>(count, 65536)));
    for (quint32 i = 0; i < count; ++i) {
        if (pos + 4 > n) { out.clear(); return false; }
        const quint32 keyLen = qFromLittleEndian<quint32>(p + pos);
        pos += 4;
        if (keyLen > n - pos || pos + keyLen + 4 > n) { out.clear(); return false; }
        const QString key = QString::fromUtf8(reinterpret_cast<const char*>(p + pos), static_cast<qsizetype>(keyLen));
        pos += keyLen;
        const quint32 units = qFromLittleEndian<quint32>(p + pos);
        pos += 4;
        if (uint64_t(units) * 2u > n - pos) { out.clear(); return false; }
        QString value(static_cast<qsizetype>(units), Qt::Uninitialized);
        for (quint32 u = 0; u < units; ++u) value[u] = QChar(qFromLittleEndian<quint16>(p + pos + size_t(u) * 2));
        pos += size_t(units) * 2;
        out.insert(key, value);
    }
    if (pos != n) { out.clear(); if (error) *error = QStringLiteral("corrupt Rust RCO text payload length"); return false; }
    return true;
}

} // namespace xmb::image_rust
