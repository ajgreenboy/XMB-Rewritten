#pragma once
#include "edge_decode.h"
#include "gtf_decode.h"
#include "image_subtitles.h"

namespace xmb::image_rust {
QImage decodeGtf(const QByteArray& gtf, QString* error = nullptr);
bool decodeEdge(const QByteArray& edge, QVector<EdgeVertex>& vertices, QVector<quint32>& indices, QString* error = nullptr);
bool parsePgs(const QByteArray& sup, QVector<ImageCue>& out, QString* reason = nullptr);
bool parseVobSub(const QByteArray& idx, const QByteArray& sub, int track, QVector<ImageCue>& out, QString* reason = nullptr);
} // namespace xmb::image_rust
