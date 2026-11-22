#!/usr/bin/env bash
set -euo pipefail

# Build and run the byte-parity harness for the firmware-crypto crate.
#
# The harness compiles the C++ reference (native-xmb/src/firmware_crypto.cpp)
# against Qt6Core and prints the hex output of every primitive over a fixed set
# of deterministic vectors. stdout is captured to outputs.txt, which is checked
# in. The same hex is hard-coded in tests/parity.rs.

QT_PREFIX="${QT_PREFIX:-/opt/qt/6.12.0/gcc_64}"
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PARITY_DIR="$SRC_DIR/parity"

export PKG_CONFIG_PATH="$QT_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

echo "Qt prefix:      $QT_PREFIX"
echo "Parity dir:     $PARITY_DIR"

echo "Compiling harness ..."
g++ -std=c++20 -fPIC \
    $(pkg-config --cflags Qt6Core) \
    "$PARITY_DIR/harness.cpp" \
    $(pkg-config --libs Qt6Core) \
    -Wl,-rpath,"$QT_PREFIX/lib" \
    -o "$PARITY_DIR/harness"

echo "Running harness -> outputs.txt"
"$PARITY_DIR/harness" > "$PARITY_DIR/outputs.txt"
cat "$PARITY_DIR/outputs.txt"
echo "Done."
