#include "firmware_crypto_rust.h"

#include "firmware_crypto_abi.h"

#include <cstddef>
#include <cstdint>

// Thin C ABI wrappers over the Rust `firmware-crypto` staticlib. These mirror
// the xmb::firmware_rust signatures declared in firmware_crypto_rust.h; the
// actual work happens in Rust behind the fw_* exports in firmware_crypto_abi.h.
namespace xmb::firmware_rust {

QByteArray hmacSha1(QByteArray key, const QByteArray& data) {
    std::uint8_t out[20];
    const std::int32_t n = fw_hmac_sha1(
        reinterpret_cast<const std::uint8_t*>(key.constData()), static_cast<std::size_t>(key.size()),
        reinterpret_cast<const std::uint8_t*>(data.constData()), static_cast<std::size_t>(data.size()),
        out, sizeof(out));
    if (n != 20) return QByteArray();
    return QByteArray(reinterpret_cast<const char*>(out), 20);
}

bool aesCtr(QByteArray& data, const QByteArray& key, QByteArray iv) {
    const std::int32_t rc = fw_aes_ctr(
        reinterpret_cast<std::uint8_t*>(data.data()), static_cast<std::size_t>(data.size()),
        reinterpret_cast<const std::uint8_t*>(key.constData()), static_cast<std::size_t>(key.size()),
        reinterpret_cast<const std::uint8_t*>(iv.constData()), static_cast<std::size_t>(iv.size()));
    return rc == FW_OK;
}

bool aesCbc(QByteArray& data, const QByteArray& key, QByteArray iv, bool encrypt) {
    const std::int32_t rc = fw_aes_cbc(
        reinterpret_cast<std::uint8_t*>(data.data()), static_cast<std::size_t>(data.size()),
        reinterpret_cast<const std::uint8_t*>(key.constData()), static_cast<std::size_t>(key.size()),
        reinterpret_cast<const std::uint8_t*>(iv.constData()), static_cast<std::size_t>(iv.size()),
        encrypt ? 1 : 0);
    return rc == FW_OK;
}

}  // namespace xmb::firmware_rust
