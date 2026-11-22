#pragma once
#include <QByteArray>

// Rust-backed firmware crypto shim.
//
// Byte-for-byte parity wrappers around the Rust `firmware-crypto` staticlib
// (native-xmb/rust-edge/firmware-crypto), exposing the same three operations as
// the C++ reference in firmware_crypto.h but under a distinct namespace so the
// two can coexist. This exists to prove link/ABI correctness when
// XMB_RUST_FIRMWARE_CRYPTO is ON; it does NOT replace the default C++
// implementation (xmb::firmware) anywhere in the app yet.
namespace xmb::firmware_rust {
QByteArray hmacSha1(QByteArray key, const QByteArray& data);
bool aesCtr(QByteArray& data, const QByteArray& key, QByteArray iv);
bool aesCbc(QByteArray& data, const QByteArray& key, QByteArray iv, bool encrypt = false);
}
