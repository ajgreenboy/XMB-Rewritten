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

} // namespace xmb::image_rust
