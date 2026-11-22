/* Minimal C smoke test: proves the staticlib links from a plain C toolchain and
 * the ABI works end-to-end (not just from Rust). Not part of `cargo test`. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../include/firmware_crypto_abi.h"

static int hex_eq(const uint8_t *a, size_t n, const char *hex) {
    char buf[3];
    for (size_t i = 0; i < n; i++) {
        snprintf(buf, sizeof buf, "%02x", a[i]);
        if (memcmp(buf, hex + 2 * i, 2) != 0) return 0;
    }
    return hex[2 * n] == '\0';
}

int main(void) {
    /* HMAC-SHA1 (RFC 2202 case 2) */
    const char *key = "Jefe";
    const char *data = "what do ya want for nothing?";
    fw_buffer mac = fw_hmac_sha1_owned((const uint8_t *)key, strlen(key),
                                       (const uint8_t *)data, strlen(data));
    if (mac.data == NULL || mac.len != 20) {
        fprintf(stderr, "FAIL: hmac_sha1_owned returned null/len\n");
        return 1;
    }
    if (!hex_eq(mac.data, mac.len, "effcdf6ae5eb2fa2d27416d5f184df9c259a7c79")) {
        fprintf(stderr, "FAIL: hmac mismatch\n");
        return 1;
    }
    fw_free_buffer(mac.data, mac.len);
    printf("hmac_sha1_owned: OK\n");

    /* AES-128-CTR in place */
    uint8_t key16[16], iv16[16], buf[16];
    for (int i = 0; i < 16; i++) { key16[i] = (uint8_t)i; iv16[i] = (uint8_t)(0x10 + i); buf[i] = (uint8_t)(0x20 + i); }
    if (fw_aes_ctr(buf, 16, key16, 16, iv16, 16) != 0) {
        fprintf(stderr, "FAIL: fw_aes_ctr error\n");
        return 1;
    }
    if (!hex_eq(buf, 16, "27dfcd57c5f02549b827c43aa2b9bcbc")) {
        fprintf(stderr, "FAIL: ctr mismatch\n");
        return 1;
    }
    printf("fw_aes_ctr: OK\n");

    printf("fw_last_error (expect null after success): %s\n", fw_last_error() ? "NOT NULL" : "null");
    printf("ALL OK\n");
    return 0;
}
