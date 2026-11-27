#include "steam_vdf_rust.h"
#include "steam_vdf_abi.h"
#include <QtEndian>
#include <limits>

namespace xmb::steamvdf_rust {
namespace {

struct SvdfBufferGuard {
    uint8_t* ptr = nullptr;
    size_t len = 0;
    ~SvdfBufferGuard() {
        if (ptr) svdf_free_buffer(ptr, len);
    }
};

QString lastError() {
    const size_t len = svdf_last_error(nullptr, 0);
    if (len == 0) return QStringLiteral("Rust KeyValues parser failed");
    QByteArray utf8(static_cast<qsizetype>(std::min<size_t>(len, 4096)), Qt::Uninitialized);
    const size_t copied = svdf_last_error(reinterpret_cast<uint8_t*>(utf8.data()), static_cast<size_t>(utf8.size()));
    utf8.truncate(static_cast<qsizetype>(copied));
    return QString::fromUtf8(utf8);
}

bool unpackEntry(const uint8_t* data, size_t size, size_t& pos, steamvdf::Entry& out, int depth) {
    if (depth > 128 || pos >= size) return false;
    const uint8_t isBlock = data[pos++];
    if (pos + 4 > size) return false;
    const uint32_t keyLen = qFromLittleEndian<uint32_t>(data + pos);
    pos += 4;
    if (keyLen > size - pos) return false;
    out.key = QString::fromUtf8(reinterpret_cast<const char*>(data + pos), static_cast<qsizetype>(keyLen));
    pos += keyLen;
    out.block = (isBlock != 0);
    if (out.block) {
        if (pos + 4 > size) return false;
        const uint32_t childCount = qFromLittleEndian<uint32_t>(data + pos);
        pos += 4;
        out.children.reserve(static_cast<qsizetype>(std::min<uint32_t>(childCount, 32768)));
        for (uint32_t i = 0; i < childCount; ++i) {
            steamvdf::Entry child;
            if (!unpackEntry(data, size, pos, child, depth + 1)) return false;
            out.children.append(std::move(child));
        }
    } else {
        if (pos + 4 > size) return false;
        const uint32_t valLen = qFromLittleEndian<uint32_t>(data + pos);
        pos += 4;
        if (valLen > size - pos) return false;
        out.value = QString::fromUtf8(reinterpret_cast<const char*>(data + pos), static_cast<qsizetype>(valLen));
        pos += valLen;
    }
    return true;
}

} // namespace

steamvdf::Document parse(const QByteArray& input, const steamvdf::Options& options) {
    steamvdf::Document doc;
    svdf_options rawOpts{};
    rawOpts.skip_bom = options.skipBom ? 1 : 0;
    rawOpts.quoted_braces_are_structural = options.quotedBracesAreStructural ? 1 : 0;
    rawOpts.max_token_bytes = options.maxTokenBytes > 0 ? static_cast<size_t>(options.maxTokenBytes) : 0;
    rawOpts.max_tokens = options.maxTokens > 0 ? static_cast<size_t>(options.maxTokens) : 0;
    rawOpts.max_entries = options.maxEntries > 0 ? static_cast<size_t>(options.maxEntries) : 0;
    rawOpts.max_depth = options.maxDepth;

    SvdfBufferGuard guard;
    const auto* ptr = input.isEmpty() ? nullptr : reinterpret_cast<const uint8_t*>(input.constData());
    const int rc = svdf_parse(ptr, static_cast<size_t>(input.size()), &rawOpts, &guard.ptr, &guard.len);
    if (rc != 0) {
        doc.error = lastError();
        return doc;
    }
    if (!guard.ptr || guard.len < 4) {
        doc.error = QStringLiteral("corrupt Rust VDF buffer");
        return doc;
    }
    size_t pos = 0;
    const uint32_t count = qFromLittleEndian<uint32_t>(guard.ptr);
    pos += 4;
    doc.entries.reserve(static_cast<qsizetype>(std::min<uint32_t>(count, 32768)));
    for (uint32_t i = 0; i < count; ++i) {
        steamvdf::Entry entry;
        if (!unpackEntry(guard.ptr, guard.len, pos, entry, 0)) {
            doc.entries.clear();
            doc.error = QStringLiteral("corrupt Rust VDF entry payload");
            return doc;
        }
        doc.entries.append(std::move(entry));
    }
    if (pos != guard.len) {
        doc.entries.clear();
        doc.error = QStringLiteral("trailing bytes in Rust VDF buffer");
    }
    return doc;
}

} // namespace xmb::steamvdf_rust
