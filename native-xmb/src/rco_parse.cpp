#include "rco_parse.h"
#include <QStringDecoder>
#include <QtEndian>
#include <stdexcept>

namespace xmb::rco {
namespace {
void range(const QByteArray& b, quint64 p, quint64 n) {
    if (p > quint64(b.size()) || n > quint64(b.size()) - p) throw std::runtime_error("Asset offset exceeds its container");
}
quint32 u32(const QByteArray& b, quint64 p) { range(b, p, 4); return qFromBigEndian<quint32>(b.constData() + p); }
quint16 u16(const QByteArray& b, quint64 p) { range(b, p, 2); return qFromBigEndian<quint16>(b.constData() + p); }
QByteArray inflate(const QByteArray& b, quint64 p, quint32 packed, quint32 expanded) {
    if (!expanded || expanded > 32 * 1024 * 1024) throw std::runtime_error("Asset expansion exceeds 32 MiB");
    range(b, p, packed);
    QByteArray framed(4, Qt::Uninitialized);
    qToBigEndian(expanded, framed.data());
    framed.append(b.constData() + p, packed);
    auto result = qUncompress(framed);
    if (result.size() != expanded) throw std::runtime_error("Asset zlib length mismatch");
    return result;
}
} // namespace

RcoContainer parseContainer(const QByteArray& file) {
    RcoContainer c;
    c.file = file;
    if (u32(c.file, 0) != 0x46525000) throw std::runtime_error("Expected PS3 big-endian RCO");
    const auto compression = u32(c.file, 12) >> 4;
    if (compression == 0) c.tables = c.file;
    else if (compression == 1) {
        // Both header reads come first: the argument order of the old one-line form was unspecified, and either
        // read failing gives the same message.
        const quint32 packed = u32(c.file, 0xa4), expanded = u32(c.file, 0xa8);
        c.tables = c.file.left(0xa4) + inflate(c.file, 0xb0, packed, expanded);
    } else throw std::runtime_error("Unsupported RCO header compression");
    c.labels = u32(c.file, 0x40);
    c.imageData = u32(c.file, 0x80);
    const quint32 labelLength = u32(c.file, 0x44), root = u32(c.file, 0x24);
    range(c.tables, c.labels, labelLength);
    if (root == 0xffffffff) return c;
    const auto count = u32(c.tables, quint32(root + 16));   // 32-bit sum, as the original expression computed it
    if (count > 65536) throw std::runtime_error("RCO image count exceeds limit");
    quint64 at = quint64(root) + 0x28;
    for (quint32 i = 0; i < count; ++i) {
        range(c.tables, at, 0x38);
        if (u16(c.tables, at) != 0x0401) throw std::runtime_error("Unexpected RCO image entry");
        const auto label = u32(c.tables, at + 4);
        if (label >= labelLength) throw std::runtime_error("RCO label is out of range");
        const auto end = c.tables.indexOf('\0', c.labels + label);
        if (end < 0 || quint64(end) >= quint64(c.labels) + labelLength) throw std::runtime_error("Unterminated RCO label");
        c.entries.insert(QString::fromUtf8(c.tables.constData() + c.labels + label, end - c.labels - label), quint32(at));
        const auto next = u32(c.tables, at + 20);
        if (i + 1 < count && next < 0x38) throw std::runtime_error("Invalid RCO image sibling offset");
        at += next;
    }
    return c;
}

QHash<QString, QString> readEnglish(const RcoContainer& c) {
    QHash<QString, QString> result;
    const auto root = u32(c.file, 0x18), textData = u32(c.file, 0x38), textLength = u32(c.file, 0x3c);
    if (root == 0xffffffff || textData == 0xffffffff || !textLength) return result;
    range(c.file, textData, textLength);
    QByteArray english;
    const bool compressed = (u32(c.file, 12) >> 4) != 0;
    if (compressed) {
        quint64 chunk = textData;
        for (int i = 0; i < 64; ++i) {
            range(c.file, chunk, 16);
            if (chunk < textData || chunk + 16 > quint64(textData) + textLength) throw std::runtime_error("RCO language header exceeds text section");
            const auto lang = u16(c.file, chunk);
            const auto next = u32(c.file, chunk + 4), packed = u32(c.file, chunk + 8);
            if (packed > quint64(textData) + textLength - chunk - 16) throw std::runtime_error("RCO language data exceeds text section");
            if (lang == 1) { english = inflate(c.file, chunk + 16, packed, u32(c.file, chunk + 12)); break; }
            if (!next) break;
            if (next < 16 + quint64(packed)) throw std::runtime_error("Invalid RCO language sibling offset");
            chunk += next;
        }
    } else english = c.file.mid(textData, textLength);
    if (english.isEmpty()) return result;
    const auto languages = u32(c.tables, quint32(root + 16));
    if (languages > 64) throw std::runtime_error("Invalid RCO language count");
    quint64 at = quint64(root) + 0x28;
    for (quint32 language = 0; language < languages; ++language) {
        range(c.tables, at, 0x30);
        const quint64 info = at + 0x28;
        if (u16(c.tables, info) == 1) {
            const auto format = u16(c.tables, info + 2);
            const auto count = u32(c.tables, info + 4);
            if (count > 65536) throw std::runtime_error("Invalid RCO text count");
            range(c.tables, info + 8, quint64(count) * 12);
            for (quint32 i = 0; i < count; ++i) {
                const quint64 index = info + 8 + quint64(i) * 12;
                const auto label = u32(c.tables, index), length = u32(c.tables, index + 4), offset = u32(c.tables, index + 8);
                if (label >= u32(c.file, 0x44)) throw std::runtime_error("RCO text label exceeds label section");
                const quint64 labelAt = quint64(c.labels) + label;
                const auto end = c.tables.indexOf('\0', labelAt);
                if (end < 0 || quint64(end) >= quint64(c.labels) + u32(c.file, 0x44)) throw std::runtime_error("Unterminated RCO text label");
                range(english, offset, length);
                const QByteArrayView bytes(english.constData() + offset, length);
                QString value;
                if (format == 0) value = QString::fromUtf8(bytes);
                else if (format == 1) { QStringDecoder decoder(QStringDecoder::Utf16BE); value = decoder(bytes); if (decoder.hasError()) throw std::runtime_error("Invalid RCO UTF-16 text"); }
                else if (format == 2) { QStringDecoder decoder(QStringDecoder::Utf32BE); value = decoder(bytes); if (decoder.hasError()) throw std::runtime_error("Invalid RCO UTF-32 text"); }
                else throw std::runtime_error("Unsupported RCO text encoding");
                while (value.endsWith(QChar(0))) value.chop(1);
                result.insert(QString::fromUtf8(c.tables.constData() + labelAt, end - labelAt), value);
            }
            return result;
        }
        const auto next = u32(c.tables, at + 20);
        if (!next) break;
        if (next < 0x30) throw std::runtime_error("Invalid RCO text sibling offset");
        at += next;
    }
    return result;
}
} // namespace xmb::rco
