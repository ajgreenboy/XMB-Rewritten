#pragma once
#include "edge_decode.h"
#include "gtf_decode.h"
#include "image_subtitles.h"

#include <QHash>

namespace xmb::image_rust {
struct QrcTocEntry {
    quint32 offset = 0;
    quint32 size = 0;
    quint32 expanded = 0;
};
QImage decodeGtf(const QByteArray& gtf, QString* error = nullptr);
bool decodeEdge(const QByteArray& edge, QVector<EdgeVertex>& vertices, QVector<quint32>& indices, QString* error = nullptr);
bool parsePgs(const QByteArray& sup, QVector<ImageCue>& out, QString* reason = nullptr);
bool parseVobSub(const QByteArray& idx, const QByteArray& sub, int track, QVector<ImageCue>& out, QString* reason = nullptr);
QImage decodeGim(const QByteArray& gim, QString* error = nullptr);
QByteArray vagToWav(const QByteArray& left, const QByteArray& right = {});
bool parseQrc(const QByteArray& rawQrc, QByteArray& unpackedQrcf, quint32& dataBase, QHash<QString, QrcTocEntry>& entries, QString* error = nullptr);
bool inflateQrcEntry(const char* ptr, qsizetype len, quint32 expanded, QByteArray& out, QString* error = nullptr);
} // namespace xmb::image_rust
