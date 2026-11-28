#include "steam_shortcuts.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QtDebug>

#include <array>

namespace xmb {
namespace {

constexpr qint64 MaxShortcutsBytes = 16LL * 1024 * 1024;
constexpr int MaxDepth = 8;
constexpr int MaxShortcuts = 4096;
constexpr int MaxNodes = 200000;
constexpr qsizetype MaxStringBytes = 64 * 1024;
constexpr qint64 SteamId64Base = 76561197960265728LL;
constexpr quint64 ShortcutGameIdTag = 0x02000000ULL;

enum : quint8 { TypeMap = 0x00, TypeString = 0x01, TypeInt32 = 0x02, TypeFloat = 0x03, TypePointer = 0x04,
                TypeColor = 0x06, TypeUInt64 = 0x07, TypeEnd = 0x08, TypeInt64 = 0x0A };

struct Node {
    quint8 type = TypeEnd;
    QString key;
    QString text;
    quint64 number = 0;
    QVector<Node> children;
};

class Reader {
public:
    explicit Reader(const QByteArray& bytes) : data_(bytes) {}
    bool atEnd() const { return pos_ >= data_.size(); }
    bool byte(quint8* out) {
        if (pos_ >= data_.size()) return fail(QStringLiteral("the file ends in the middle of an entry"));
        *out = quint8(data_.at(pos_++));
        return true;
    }
    // A NUL-terminated UTF-8 string. The terminator must exist inside the file and the text must be at most
    // MaxStringBytes long: a length that would run past the end is an error, never a read past the buffer.
    bool string(QString* out) {
        if (pos_ >= data_.size()) return fail(QStringLiteral("the file ends in the middle of a string"));
        const qsizetype end = data_.indexOf('\0', pos_);
        if (end < 0) return fail(QStringLiteral("a string is not terminated"));
        if (end - pos_ > MaxStringBytes) return fail(QStringLiteral("a string is longer than %1 bytes").arg(MaxStringBytes));
        *out = QString::fromUtf8(data_.constData() + pos_, end - pos_);
        pos_ = end + 1;
        return true;
    }
    bool fixed(int count, quint64* out) {
        if (count < 0 || count > 8 || data_.size() - pos_ < count) return fail(QStringLiteral("a number is cut off"));
        quint64 value = 0;
        for (int i = 0; i < count; ++i) value |= quint64(quint8(data_.at(pos_ + i))) << (8 * i);
        pos_ += count;
        *out = value;
        return true;
    }
    bool fail(const QString& message) { if (error_.isEmpty()) error_ = message; return false; }
    const QString& error() const { return error_; }
private:
    const QByteArray& data_;
    qsizetype pos_ = 0;
    QString error_;
};

bool readValue(Reader& reader, quint8 type, Node* node) {
    switch (type) {
    case TypeString: return reader.string(&node->text);
    case TypeInt32: case TypeFloat: case TypePointer: case TypeColor: return reader.fixed(4, &node->number);
    case TypeUInt64: case TypeInt64: return reader.fixed(8, &node->number);
    default: return reader.fail(QStringLiteral("an entry has the unsupported type 0x%1").arg(type, 2, 16, QLatin1Char('0')));
    }
}

// The entries of one map up to its 0x08 terminator.
bool readMap(Reader& reader, int depth, QVector<Node>* children, int* nodes) {
    for (;;) {
        quint8 type = 0;
        if (!reader.byte(&type)) return false;
        if (type == TypeEnd) return true;
        Node node;
        node.type = type;
        if (!reader.string(&node.key)) return false;
        if (++*nodes > MaxNodes) return reader.fail(QStringLiteral("the file has too many entries"));
        if (type == TypeMap) {
            if (depth >= MaxDepth) return reader.fail(QStringLiteral("entries are nested more than %1 deep").arg(MaxDepth));
            if (!readMap(reader, depth + 1, &node.children, nodes)) return false;
        } else if (!readValue(reader, type, &node)) {
            return false;
        }
        if (children) children->push_back(std::move(node));
    }
}

const Node* field(const QVector<Node>& entries, const char* name) {
    for (const auto& node : entries)
        if (node.key.compare(QLatin1String(name), Qt::CaseInsensitive) == 0) return &node;
    return nullptr;
}
QString textField(const QVector<Node>& entries, const char* name) {
    const Node* node = field(entries, name);
    return node && node->type == TypeString ? node->text : QString();
}
bool numberField(const QVector<Node>& entries, const char* name, quint32* out) {
    const Node* node = field(entries, name);
    if (!node || (node->type != TypeInt32 && node->type != TypeUInt64 && node->type != TypeInt64)) return false;
    *out = quint32(node->number & 0xFFFFFFFFULL);
    return true;
}

// One shortcut map; false when it carries neither a name nor an executable.
bool toShortcut(const Node& map, SteamShortcut* out) {
    SteamShortcut s;
    s.appName = textField(map.children, "appname").trimmed();
    s.exe = textField(map.children, "exe").trimmed();
    s.startDir = textField(map.children, "StartDir").trimmed();
    s.icon = textField(map.children, "icon").trimmed();
    s.launchOptions = textField(map.children, "LaunchOptions").trimmed();
    if (const Node* tags = field(map.children, "tags"); tags && tags->type == TypeMap)
        for (const auto& tag : tags->children)
            if (tag.type == TypeString && !tag.text.trimmed().isEmpty()) s.tags << tag.text.trimmed();
    quint32 stored = 0;
    if (numberField(map.children, "appid", &stored) && stored != 0) {
        s.appId = stored;
        s.appIdStored = true;
    } else {
        s.appId = steamShortcutComputedAppId(textField(map.children, "exe"), textField(map.children, "appname"));
    }
    quint32 last = 0;
    if (numberField(map.children, "LastPlayTime", &last)) s.lastPlayTime = last;
    quint32 hidden = 0;
    s.hidden = numberField(map.children, "IsHidden", &hidden) && hidden == 1;
    if (s.appName.isEmpty() && s.exePath().isEmpty()) return false;
    *out = std::move(s);
    return true;
}

QString normalisedPath(QString path) {
    path = path.trimmed();
    if (path.size() >= 2 && path.startsWith(QLatin1Char('"')) && path.endsWith(QLatin1Char('"'))) path = path.mid(1, path.size() - 2).trimmed();
    return path.replace(QLatin1Char('\\'), QLatin1Char('/'));
}

const std::array<quint32, 256>& crcTable() {
    static const auto table = [] {
        std::array<quint32, 256> t{};
        for (quint32 i = 0; i < 256; ++i) {
            quint32 c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();
    return table;
}

QString firstImage(const QString& directory, const QString& stem) {
    for (const char* extension : {"png", "jpg", "jpeg", "webp"}) {
        const QFileInfo info(QDir(directory).filePath(stem + QLatin1Char('.') + QLatin1String(extension)));
        if (info.isFile() && info.size() > 0 && info.size() <= 32LL * 1024 * 1024) return info.absoluteFilePath();
    }
    return {};
}

} // namespace

QString SteamShortcut::gameId() const { return QString::number(steamShortcutGameId64(appId)); }
QString SteamShortcut::rowId() const { return QStringLiteral("steam-shortcut:") + QString::number(appId); }
QString SteamShortcut::exePath() const {
    QString path = exe.trimmed();
    if (path.size() >= 2 && path.startsWith(QLatin1Char('"')) && path.endsWith(QLatin1Char('"'))) path = path.mid(1, path.size() - 2).trimmed();
    return path;
}
QString SteamShortcut::displayName() const {
    if (!appName.isEmpty()) return appName;
    return QFileInfo(normalisedPath(exe)).completeBaseName();
}

quint32 steamCrc32(const QByteArray& bytes) {
    const auto& table = crcTable();
    quint32 crc = 0xFFFFFFFFu;
    for (const char c : bytes) crc = table[(crc ^ quint8(c)) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}
quint32 steamShortcutComputedAppId(const QString& exe, const QString& appName) {
    return steamCrc32((exe + appName).toUtf8()) | 0x80000000u;
}
quint64 steamShortcutGameId64(quint32 appId) { return (quint64(appId) << 32) | ShortcutGameIdTag; }
bool steamShortcutAppIdFromGameId(const QString& gameId, quint32* appId) {
    bool ok = false;
    const quint64 value = gameId.trimmed().toULongLong(&ok);
    if (!ok || (value >> 32) == 0 || (value & 0xFFFFFFFFULL) != ShortcutGameIdTag) return false;
    if (appId) *appId = quint32(value >> 32);
    return true;
}
bool isSteamShortcutGameId(const QString& gameId) { return steamShortcutAppIdFromGameId(gameId, nullptr); }
QString steamShortcutLaunchUri(const QString& gameId) {
    return isSteamShortcutGameId(gameId) ? QStringLiteral("steam://rungameid/") + gameId.trimmed() : QString();
}
QString steamShortcutRegistryAppId(const QString& gameId) {
    quint32 appId = 0;
    return steamShortcutAppIdFromGameId(gameId, &appId) ? QString::number(appId) : QString();
}

SteamShortcutList parseSteamShortcuts(const QByteArray& bytes) {
    SteamShortcutList list;
    if (bytes.isEmpty()) return list;   // an empty file is simply no shortcuts
    Reader reader(bytes);
    int nodes = 0;
    QSet<quint32> seen;
    bool sawShortcuts = false;
    // The root is a map; the entry named "shortcuts" holds one map per shortcut. Anything else at the root is skipped.
    for (bool done = false; !done;) {
        quint8 type = 0;
        if (!reader.byte(&type)) break;
        if (type == TypeEnd) break;
        QString key;
        if (!reader.string(&key)) break;
        if (type != TypeMap || key.compare(QStringLiteral("shortcuts"), Qt::CaseInsensitive) != 0) {
            Node ignored;
            if (type == TypeMap ? !readMap(reader, 1, nullptr, &nodes) : !readValue(reader, type, &ignored)) break;
            continue;
        }
        sawShortcuts = true;
        for (;;) {
            quint8 entryType = 0;
            if (!reader.byte(&entryType)) { done = true; break; }
            if (entryType == TypeEnd) break;
            Node entry;
            entry.type = entryType;
            if (!reader.string(&entry.key)) { done = true; break; }
            if (entryType == TypeMap) {
                if (!readMap(reader, 2, &entry.children, &nodes)) { done = true; break; }
            } else if (!readValue(reader, entryType, &entry)) {
                done = true;
                break;
            }
            if (entryType != TypeMap) continue;
            if (list.shortcuts.size() >= MaxShortcuts) { reader.fail(QStringLiteral("more than %1 shortcuts").arg(MaxShortcuts)); done = true; break; }
            SteamShortcut shortcut;
            if (!toShortcut(entry, &shortcut) || shortcut.hidden) { ++list.skipped; continue; }
            if (seen.contains(shortcut.appId)) { ++list.duplicates; continue; }
            seen.insert(shortcut.appId);
            list.shortcuts.push_back(std::move(shortcut));
        }
    }
    if (!reader.error().isEmpty()) list.error = reader.error();
    else if (!sawShortcuts && !bytes.isEmpty()) list.error = QStringLiteral("the file has no \"shortcuts\" entry");
    return list;
}

SteamShortcutList scanSteamShortcuts(const QString& steamRoot, const QString& steamId64) {
    SteamShortcutList list;
    bool ok = false;
    const qint64 value = steamId64.trimmed().toLongLong(&ok);
    if (steamRoot.isEmpty() || !ok || value <= SteamId64Base || value - SteamId64Base > 0xFFFFFFFFLL) return list;
    QFile file(QDir(steamRoot).filePath(QStringLiteral("userdata/%1/config/shortcuts.vdf").arg(value - SteamId64Base)));
    if (!file.exists()) return list;
    if (!file.open(QIODevice::ReadOnly)) { list.error = file.errorString(); return list; }
    if (file.size() > MaxShortcutsBytes) { list.error = QStringLiteral("shortcuts.vdf exceeds the supported size"); return list; }
    const QByteArray bytes = file.read(MaxShortcutsBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes.size() > MaxShortcutsBytes) { list.error = QStringLiteral("shortcuts.vdf could not be read completely"); return list; }
    return parseSteamShortcuts(bytes);
}

SteamShortcutArt steamShortcutGridArtwork(const QString& steamRoot, const QString& steamId64, const SteamShortcut& shortcut) {
    SteamShortcutArt art;
    bool ok = false;
    const qint64 value = steamId64.trimmed().toLongLong(&ok);
    if (steamRoot.isEmpty() || !ok || value <= SteamId64Base || value - SteamId64Base > 0xFFFFFFFFLL) return art;
    const QString grid = QDir(steamRoot).filePath(QStringLiteral("userdata/%1/config/grid").arg(value - SteamId64Base));
    if (!QFileInfo(grid).isDir()) return art;
    for (const QString& id : {QString::number(shortcut.appId), shortcut.gameId()}) {
        if (art.logo.isEmpty()) art.logo = firstImage(grid, id + QStringLiteral("_logo"));
        if (art.wide.isEmpty()) art.wide = firstImage(grid, id);
        if (art.portrait.isEmpty()) art.portrait = firstImage(grid, id + QLatin1Char('p'));
        if (art.hero.isEmpty()) art.hero = firstImage(grid, id + QStringLiteral("_hero"));
        if (art.icon.isEmpty()) art.icon = firstImage(grid, id + QStringLiteral("_icon"));
    }
    return art;
}

bool steamShortcutExeMatches(const SteamShortcut& shortcut, const QString& imagePath) {
    const QString exe = normalisedPath(shortcut.exe);
    const QString image = normalisedPath(imagePath);
    if (exe.isEmpty() || image.isEmpty()) return false;
    return exe.compare(image, Qt::CaseInsensitive) == 0;
}

} // namespace xmb
