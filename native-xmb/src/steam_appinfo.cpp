#include "steam_appinfo.h"

#include <QFile>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <cstring>

namespace xmb {
namespace {

constexpr quint32 MagicBase = 0x07564400u;   // "DV\x07" in the high three bytes
constexpr qint64 MaximumEntryBytes = 16LL * 1024 * 1024;
constexpr qint64 MaximumStringTableBytes = 32LL * 1024 * 1024;
constexpr qint64 MaximumStringTableCount = 4LL * 1000 * 1000;
constexpr qsizetype MaximumStringBytes = 64 * 1024;
constexpr int MaximumDepth = 24;

quint32 u32(const char* p) {
    quint32 value = 0;
    std::memcpy(&value, p, 4);   // the host is little endian on every platform this build targets
    return value;
}

// One entry's binary KeyValues. Everything reads through bounds-checked helpers; any shortfall fails the entry.
struct Walk {
    const QByteArray& data;
    const QVector<QByteArray>* table = nullptr; // v29 key strings; null for v27/v28
    qsizetype pos = 0;
    QStringList path;
    SteamAppInfoEntry out;
    bool haveName = false;
    bool haveType = false;
    bool finished() const { return haveName && haveType; }

    bool need(qsizetype count) const { return count >= 0 && pos + count <= data.size(); }
    bool readCString(QByteArray* value) {
        const qsizetype end = data.indexOf('\0', pos);
        if (end < 0 || end - pos > MaximumStringBytes) return false;
        if (value) *value = data.mid(pos, end - pos);
        pos = end + 1;
        return true;
    }
    bool readKey(QByteArray* key) {
        if (!table) return readCString(key);
        if (!need(4)) return false;
        const quint32 index = u32(data.constData() + pos);
        pos += 4;
        if (index >= quint32(table->size())) return false;
        if (key) *key = table->at(qsizetype(index));
        return true;
    }
    bool skip(qsizetype count) {
        if (!need(count)) return false;
        pos += count;
        return true;
    }
    bool readWide() {
        // UTF-16LE, NUL-terminated; only skipped (no field this module reads is a wide string).
        while (true) {
            if (!need(2)) return false;
            const bool terminator = data.at(pos) == 0 && data.at(pos + 1) == 0;
            pos += 2;
            if (terminator) return true;
        }
    }

    bool atCommon() const {
        return path.size() == 2 && path.at(0) == QLatin1String("appinfo") && path.at(1) == QLatin1String("common");
    }

    // Reads children until the object's 0x08 (or the end of the data at the root). False when the data is malformed.
    bool object(int depth) {
        if (depth > MaximumDepth) return false;
        while (true) {
            if (finished()) return true;
            if (pos >= data.size()) return depth == 0;   // a root object may simply end with the buffer
            const quint8 type = quint8(data.at(pos++));
            if (type == 0x08) return true;
            QByteArray key;
            if (!readKey(&key)) return false;
            const QString name = QString::fromUtf8(key).toLower();
            switch (type) {
            case 0x00:
                path.append(name);
                if (!object(depth + 1)) return false;
                path.removeLast();
                break;
            case 0x01: {
                QByteArray value;
                if (!readCString(&value)) return false;
                if (atCommon()) {
                    if (name == QLatin1String("name") && !haveName) {
                        out.title = QString::fromUtf8(value).trimmed();
                        haveName = true;
                    } else if (name == QLatin1String("type") && !haveType) {
                        out.type = QString::fromUtf8(value).trimmed();
                        haveType = true;
                    }
                }
                break;
            }
            case 0x02: case 0x03: case 0x04: case 0x06:
                if (!skip(4)) return false;
                break;
            case 0x05:
                if (!readWide()) return false;
                break;
            case 0x07: case 0x0A:
                if (!skip(8)) return false;
                break;
            default:
                return false;   // an unknown type byte: the stream cannot be followed any further
            }
        }
    }
};

bool readExact(QFile& file, char* into, qint64 count) {
    return file.read(into, count) == count;
}

} // namespace

bool steamAppTypeIsGame(const QString& type) {
    return type.trimmed().compare(QLatin1String("game"), Qt::CaseInsensitive) == 0;
}

QHash<QString, SteamAppInfoEntry> readSteamAppInfo(const QString& path, const QSet<QString>& wanted, QString* error) {
    if (error) error->clear();
    QHash<QString, SteamAppInfoEntry> result;
    if (wanted.isEmpty()) return result;
    const auto fail = [&](const QString& why) {
        if (error) *error = why;
        return result;
    };
    QFile file(path);
    if (!file.exists()) return fail(QStringLiteral("Steam's appinfo.vdf was not found."));
    if (!file.open(QIODevice::ReadOnly)) return fail(file.errorString());
    const qint64 fileSize = file.size();

    char header[16];
    if (fileSize < 8 || !readExact(file, header, 8)) return fail(QStringLiteral("Steam's appinfo.vdf is too short."));
    const quint32 magic = u32(header);
    if ((magic & 0xffffff00u) != MagicBase) return fail(QStringLiteral("Steam's appinfo.vdf has an unknown signature."));
    const quint32 version = magic & 0xffu;
    if (version < 0x27 || version > 0x29) return fail(QStringLiteral("Steam's appinfo.vdf version is not supported."));
    const bool v29 = version >= 0x29;
    const qint64 fixedBytes = version >= 0x28 ? 60 : 40;

    QVector<QByteArray> table;
    qint64 entriesEnd = fileSize;
    if (v29) {
        if (!readExact(file, header + 8, 8)) return fail(QStringLiteral("Steam's appinfo.vdf is too short."));
        qint64 tableOffset = 0;
        std::memcpy(&tableOffset, header + 8, 8);
        if (tableOffset < 16 || tableOffset + 4 > fileSize)
            return fail(QStringLiteral("Steam's appinfo.vdf string table offset is invalid."));
        if (fileSize - tableOffset > MaximumStringTableBytes)
            return fail(QStringLiteral("Steam's appinfo.vdf string table is larger than expected."));
        if (!file.seek(tableOffset)) return fail(QStringLiteral("Steam's appinfo.vdf could not be positioned."));
        const QByteArray bytes = file.read(fileSize - tableOffset);
        if (bytes.size() < 4) return fail(QStringLiteral("Steam's appinfo.vdf string table is truncated."));
        const qint64 count = u32(bytes.constData());
        if (count > MaximumStringTableCount) return fail(QStringLiteral("Steam's appinfo.vdf string table is larger than expected."));
        table.reserve(qsizetype(std::min<qint64>(count, 200000)));
        qsizetype at = 4;
        while (qint64(table.size()) < count && at < bytes.size()) {
            const qsizetype end = bytes.indexOf('\0', at);
            if (end < 0) break;
            table.append(bytes.mid(at, end - at));
            at = end + 1;
        }
        if (qint64(table.size()) != count) return fail(QStringLiteral("Steam's appinfo.vdf string table is truncated."));
        entriesEnd = tableOffset;
    }
    if (!file.seek(v29 ? 16 : 8)) return fail(QStringLiteral("Steam's appinfo.vdf could not be positioned."));

    qint64 position = v29 ? 16 : 8;
    int remaining = int(wanted.size());
    while (position + 8 <= entriesEnd && remaining > 0) {
        char head[8];
        if (!readExact(file, head, 8)) return fail(QStringLiteral("Steam's appinfo.vdf ended inside an entry header."));
        const quint32 appId = u32(head);
        if (appId == 0) break;   // the end marker
        const qint64 size = u32(head + 4);
        position += 8;
        if (size < fixedBytes || size > MaximumEntryBytes || position + size > entriesEnd)
            return fail(QStringLiteral("Steam's appinfo.vdf has an entry with an impossible size."));
        const QString id = QString::number(appId);
        if (!wanted.contains(id) || result.contains(id)) {
            position += size;
            if (!file.seek(position)) return fail(QStringLiteral("Steam's appinfo.vdf could not be positioned."));
            continue;
        }
        const QByteArray entry = file.read(size);
        if (entry.size() != size) return fail(QStringLiteral("Steam's appinfo.vdf ended inside an entry."));
        position += size;

        Walk walk{entry};
        walk.table = v29 ? &table : nullptr;
        walk.pos = qsizetype(fixedBytes);
        const bool good = walk.object(0);
        // An entry that names the app is useful even when something after the name is damaged.
        if (walk.haveName || walk.haveType) result.insert(id, walk.out);
        if (!good && !walk.finished()) {
            // One damaged entry must not hide the others: the size field already told us where the next one starts.
            if (error && error->isEmpty()) *error = QStringLiteral("Steam's appinfo.vdf has an entry that could not be read (app %1).").arg(id);
        }
        --remaining;
    }
    return result;
}

} // namespace xmb
