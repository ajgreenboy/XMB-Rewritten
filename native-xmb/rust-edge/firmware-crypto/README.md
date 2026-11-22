# firmware-crypto

Byte-for-byte parity Rust reimplementation of the native XMB firmware crypto
primitives, exposed over a C ABI as a `staticlib`.

This is **Milestone 1** of the Rust hardening plan (`native-xmb/audit/RUST-HARDENING.md`):
crate + byte-parity + C ABI only. It is **not** wired into CMake yet — that is a
later milestone, after the in-flight Linux input/audio work lands.

## Scope

Three primitives, matching `native-xmb/src/firmware_crypto.{h,cpp}` exactly:

| Rust function | C++ reference | Notes |
|---|---|---|
| `crypto::hmac_sha1(key, data) -> Vec<u8>` | `hmacSha1(QByteArray, const QByteArray&)` | HMAC-SHA1 (RFC 2104), 20-byte output. Keys > 64 bytes are pre-hashed with SHA-1, then zero-padded to 64. |
| `crypto::aes_ctr(data, key, iv) -> bool` | `aesCtr(QByteArray&, const QByteArray&, QByteArray)` | AES-128-CTR, in place. key/iv must be 16 bytes. Big-endian 16-byte counter increment after each block. |
| `crypto::aes_cbc(data, key, iv, encrypt) -> bool` | `aesCbc(QByteArray&, const QByteArray&, QByteArray, bool)` | AES-128/256-CBC, in place, raw (no padding). key 16 or 32 bytes, iv 16 bytes, data length a multiple of 16. |

The C++ AES is a standard byte-oriented FIPS-197 AES (key schedule + S-box +
MixColumns via GF(2^8) `mul`, ported from RPCS3/PolarSSL). RustCrypto's block
cipher is also standard FIPS-197, so the two produce byte-identical output.

## Decisions (recorded by the owner)

* **RustCrypto crates (`aes`, `hmac`, `sha1`), NOT `ring`** — `ring` provides no
  raw AES block cipher (only AEAD). For exact byte parity the raw `aes::Aes128`
  / `Aes256` block cipher is used, with the CTR/CBC *mode loops implemented
  manually to match the C++ control flow exactly (rather than the `ctr`/`cbc`
  mode wrappers — which would also be fine, but manual is the safer parity path).
* **`staticlib`** crate type; C ABI follows the §3.1 boundary rules (below).
* The pure Rust functions are kept separate from the FFI layer so the parity
  tests target the pure functions directly.

## C ABI

Header: [`include/firmware_crypto_abi.h`](include/firmware_crypto_abi.h).

Boundary rules (per `RUST-HARDENING.md` §3.1):

* **Owned, length-prefixed buffers.** Inputs are `const uint8_t*` + `size_t`.
  Outputs the caller must release are returned as an owned `fw_buffer`
  (`{ uint8_t* data; size_t len; }`) and freed via `fw_free_buffer` (or moved
  out with `fw_take_buffer`, which nulls the struct to prevent double-takes).
* **`int` error codes + optional error string.** `0` is success; negative codes
  distinguish argument/key/iv/data/alloc/panic failures. A thread-local error
  string is available via `fw_last_error()`.
* **No Qt types cross the boundary.**
* **`catch_unwind`** wraps every `#[no_mangle]` export, mapping panics to
  `FW_ERR_PANIC`.

Exports:

```c
int32_t  fw_hmac_sha1(const uint8_t* key, size_t key_len,
                      const uint8_t* data, size_t data_len,
                      uint8_t* out, size_t out_cap);        // writes 20 bytes, returns 20
fw_buffer fw_hmac_sha1_owned(const uint8_t* key, size_t key_len,
                             const uint8_t* data, size_t data_len); // caller-owned 20-byte MAC
int32_t  fw_aes_ctr(uint8_t* data, size_t data_len,
                    const uint8_t* key, size_t key_len,
                    const uint8_t* iv, size_t iv_len);      // in place
int32_t  fw_aes_cbc(uint8_t* data, size_t data_len,
                    const uint8_t* key, size_t key_len,
                    const uint8_t* iv, size_t iv_len,
                    int encrypt);                            // in place, raw
int32_t  fw_take_buffer(fw_buffer* buf, uint8_t** out_data, size_t* out_len);
void     fw_free_buffer(uint8_t* data, size_t len);
const char* fw_last_error(void);
```

Error codes: `FW_OK=0`, `FW_ERR_ARG=-1`, `FW_ERR_KEY=-2`, `FW_ERR_IV=-3`,
`FW_ERR_DATA=-4`, `FW_ERR_ALLOC=-5`, `FW_ERR_PANIC=-6`.

## Building and testing

```sh
cargo test              # byte-parity + known-answer + C-ABI tests
cargo build --release   # -> target/release/libfirmware_crypto.a
```

A minimal C smoke test (`parity/c_smoke.c`) proves the staticlib links from a
plain C toolchain and the ABI works end-to-end:

```sh
gcc -std=c11 parity/c_smoke.c -I include target/release/libfirmware_crypto.a \
    -lpthread -ldl -lm -o parity/c_smoke && ./parity/c_smoke
```

## Parity test procedure

The byte-parity acceptance gate is reproducible:

1. `parity/build.sh` compiles the C++ reference (`#include`s
   `native-xmb/src/firmware_crypto.cpp`) against Qt6Core and runs it over a
   fixed set of deterministic vectors, printing hex to `parity/outputs.txt`
   (checked in).
2. `tests/parity.rs` reproduces those exact inputs and asserts the Rust pure
   functions produce byte-identical output (the captured hex is hard-coded as
   expectations).
3. `tests/parity.rs` also asserts **known-answer vectors** (NIST SP 800-38A,
   FIPS-197, RFC 2202) so both implementations are independently confirmed to
   be *standard*, not merely mutually consistent.

Re-run the harness after any change to the reference to refresh `outputs.txt`:

```sh
bash parity/build.sh   # writes parity/outputs.txt
```

## Dependencies

| Crate | Version (Cargo.lock) | Purpose |
|---|---|---|
| `aes` | 0.9.4 | raw AES-128/256 block cipher |
| `hmac` | 0.13.0 | HMAC construction (over `sha1`) |
| `sha1` | 0.11.0 | SHA-1 digest for HMAC |
| `hex` (dev) | 0.4.3 | test-vector hex encode/decode |

Transitive (pinned in `Cargo.lock`): `cipher` 0.5.2, `hybrid-array` 0.4.15,
`digest` 0.11.3, `crypto-common` 0.2.2, and their dependencies.

## CMake integration (linux-port)

The crate is wired into the Linux build behind `-DXMB_RUST_FIRMWARE_CRYPTO=ON`
(default OFF). When ON, CMake builds the staticlib with cargo, links it into
`re-ps3-native` next to a thin `src/firmware_crypto_rust.{h,cpp}` shim, and adds
the `xmb-firmware-rust-parity-tests` byte-parity gate (C++ reference vs the Rust
shim). The default C++ `xmb::firmware` implementation remains the shipped path.

## Toolchain

Requires Rust **1.89+** (`rust-version` in Cargo.toml). CMake locates cargo via
`find_program`, preferring the rustup-managed toolchain at `~/.cargo/bin/cargo`
(`rustup toolchain install stable`); a plain `cargo` on `PATH` also works.

## Notes

* `RUST-HARDENING.md` §3.1 did not yet exist when this milestone was cut; the
  boundary rules above were reconstructed from the milestone brief and are the
  canonical record until the doc lands.
* The C++ reference files are **read-only** for this milestone — no changes.
