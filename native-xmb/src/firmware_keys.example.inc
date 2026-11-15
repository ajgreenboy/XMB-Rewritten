// firmware_keys.inc is committed and holds the publicly known PS3 firmware keys (the same values as RPCS3's
// Crypto/key_vault.h). This file is the zero-key template, kept for reference; XMB_FIRMWARE_KEYS accepts an alternative
// file with this same syntax.
static const unsigned char PUP_KEY[64] = {0};
static const unsigned char SCEPKG_ERK[32] = {0};
static const unsigned char SCEPKG_RIV[16] = {0};
