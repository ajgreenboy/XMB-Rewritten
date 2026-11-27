#pragma once
#include "steam_vdf.h"

namespace xmb::steamvdf_rust {
steamvdf::Document parse(const QByteArray& input, const steamvdf::Options& options);
} // namespace xmb::steamvdf_rust
