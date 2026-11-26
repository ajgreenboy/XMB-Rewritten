#pragma once
#include <QString>

// Rust-backed PUP verification, SCE decryption, and TAR extraction shim
// (`native-xmb/rust-edge/firmware-extract`, Phase 1 of `audit/RUST-HARDENING.md`).
namespace xmb::firmware_rust {
bool extractPup(const QString& pupPath, const QString& destDir, QString* error = nullptr);
bool pupVersion(const QString& pupPath, QString* version, QString* error = nullptr);
}
