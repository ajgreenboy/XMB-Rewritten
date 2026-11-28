#pragma once
#include <QByteArray>
#include <QHash>
#include <QString>

namespace xmb {
// A parsed PS3 RCO (resource container, "\0FRP" big-endian): the raw file, the table section (the file itself, or
// the 0xA4-byte header followed by the zlib-inflated tables), the label section offset, the image data offset and
// the image name -> table offset index. Shared by Assets (assets.h) and the C++/Rust parity tests.
struct RcoContainer {
    QByteArray file, tables;
    quint32 labels = 0, imageData = 0;
    QHash<QString, quint32> entries;
};

namespace rco {
// The C++ reference parsers. Both throw std::runtime_error with the messages the Rust port (rco.rs) reproduces.
// parseContainer: header, optional table compression, label section bounds and the image entry chain.
RcoContainer parseContainer(const QByteArray& file);
// readEnglish: the English (language 1) string table, label -> text, from the (optionally compressed) text section.
QHash<QString, QString> readEnglish(const RcoContainer& container);
} // namespace rco
} // namespace xmb
