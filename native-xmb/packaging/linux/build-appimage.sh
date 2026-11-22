#!/usr/bin/env bash
# Build a distributable AppImage of the RE-PS3 XMB (re-ps3-native) for x86_64 Linux.
#
# The AppImage bundles the Qt 6.12 runtime (libraries, plugins, translations) plus the
# dependency closure linuxdeploy resolves, so it launches from a clean desktop environment.
# It deliberately does NOT bundle any PS3 firmware (dev_flash): the user points the app at
# their own RPCS3 dev_flash with --dev-flash (or the first-run setup), exactly like the
# Windows build. See README.md next to this script for the full notes.
#
# Usage:
#   ./build-appimage.sh            # uses native-xmb/build-linux + /opt/qt/6.12.0/gcc_64
#   QT_ROOT=/opt/qt/6.12.0/gcc_64 BUILD_DIR=... ./build-appimage.sh
#
# Produces: out/appimage/XMB-Rewritten-x86_64.AppImage
set -euo pipefail

# ---------------------------------------------------------------------------
# Paths / configuration
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # native-xmb/packaging/linux
NATIVE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"                # native-xmb
REPO_ROOT="$(cd "$NATIVE_DIR/.." && pwd)"                    # repo root

QT_ROOT="${QT_ROOT:-/opt/qt/6.12.0/gcc_64}"
BUILD_DIR="${BUILD_DIR:-$NATIVE_DIR/build-linux}"
OUT_DIR="${OUT_DIR:-$REPO_ROOT/out/appimage}"
TOOLS_DIR="$SCRIPT_DIR/tools"                                # gitignored scratch

APP_NAME="re-ps3-native"
APP_TITLE="RE-PS3 XMB"
OUTPUT_NAME="${OUTPUT_NAME:-XMB-Rewritten-x86_64.AppImage}"

# Version is the single source of truth in CMakeLists.txt (project(... VERSION ...)).
APP_VERSION="${APP_VERSION:-$(sed -n 's/^project(RePS3Native VERSION \([0-9.]*\).*/\1/p' "$NATIVE_DIR/CMakeLists.txt")}"
if [ -z "$APP_VERSION" ]; then echo "could not parse project version from CMakeLists.txt" >&2; exit 1; fi

# ---------------------------------------------------------------------------
# Pinned AppImage tooling (exact versions + recorded sha256). The releases do not
# publish standalone sha256 files, so the sums below are recorded at pin time and
# verified on every run; linuxdeploy additionally publishes a .zsync (SHA-1).
# ---------------------------------------------------------------------------
LINUXDEPLOY_VERSION="1-alpha-20251107-1"
LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/${LINUXDEPLOY_VERSION}/linuxdeploy-x86_64.AppImage"
LINUXDEPLOY_SHA256="c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d"

LINUXDEPLOY_QT_VERSION="1-alpha-20250213-1"
LINUXDEPLOY_QT_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/${LINUXDEPLOY_QT_VERSION}/linuxdeploy-plugin-qt-x86_64.AppImage"
LINUXDEPLOY_QT_SHA256="15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724"

APPIMAGETOOL_VERSION="1.9.1"
APPIMAGETOOL_URL="https://github.com/AppImage/appimagetool/releases/download/${APPIMAGETOOL_VERSION}/appimagetool-x86_64.AppImage"
APPIMAGETOOL_SHA256="ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log()  { printf '\033[1;36m[appimage]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[appimage] error:\033[0m %s\n' "$*" >&2; exit 1; }

log "packaging re-ps3-native $APP_VERSION (Qt at $QT_ROOT)"

verify_sha256() {  # file expected
  local file="$1" expected="$2" actual
  actual="$(sha256sum "$file" | awk '{print $1}')"
  if [ "$actual" != "$expected" ]; then
    die "checksum mismatch for $file: expected $expected, got $actual"
  fi
  log "verified sha256 of $(basename "$file")"
}

# Download a pinned tool AppImage and extract it (FUSE-free, deterministic) so the
# packaging itself never needs FUSE.
ensure_tool() {  # name url version sha256
  local name="$1" url="$2" version="$3" sha="$4"
  local appimage="$TOOLS_DIR/$name-$version.AppImage"
  local dest="$TOOLS_DIR/$name"

  if [ -x "$dest/AppRun" ]; then
    log "tool $name $version already present"
    return 0
  fi

  mkdir -p "$TOOLS_DIR"
  if [ ! -f "$appimage" ]; then
    log "downloading $name $version"
    curl -fSL --retry 3 -o "$appimage" "$url"
  fi
  verify_sha256 "$appimage" "$sha"

  chmod +x "$appimage"
  rm -rf "$dest"
  local tmp
  tmp="$(mktemp -d)"
  ( cd "$tmp" && "$appimage" --appimage-extract >/dev/null 2>&1 )
  mv "$tmp/squashfs-root" "$dest"
  rmdir "$tmp"
  log "extracted $name $version -> $dest"
}

# ---------------------------------------------------------------------------
# Prerequisite checks
# ---------------------------------------------------------------------------
[ -x "$QT_ROOT/bin/qmake" ] || die "qmake not found under QT_ROOT=$QT_ROOT"
for b in re-ps3-native xmb-manager xmb-updater; do
  [ -x "$BUILD_DIR/$b" ] || die "missing built binary: $BUILD_DIR/$b (build the linux-port tree first)"
done
command -v curl >/dev/null || die "curl is required"
command -v sha256sum >/dev/null || die "sha256sum is required"

# ---------------------------------------------------------------------------
# Fetch / verify tooling
# ---------------------------------------------------------------------------
ensure_tool linuxdeploy "$LINUXDEPLOY_URL" "$LINUXDEPLOY_VERSION" "$LINUXDEPLOY_SHA256"
ensure_tool linuxdeploy-plugin-qt "$LINUXDEPLOY_QT_URL" "$LINUXDEPLOY_QT_VERSION" "$LINUXDEPLOY_QT_SHA256"
ensure_tool appimagetool "$APPIMAGETOOL_URL" "$APPIMAGETOOL_VERSION" "$APPIMAGETOOL_SHA256"

# linuxdeploy discovers plugins by looking for "linuxdeploy-plugin-<name>" on $PATH.
mkdir -p "$TOOLS_DIR/bin"
cat > "$TOOLS_DIR/bin/linuxdeploy-plugin-qt" <<EOF
#!/usr/bin/env bash
exec "$TOOLS_DIR/linuxdeploy-plugin-qt/AppRun" "\$@"
EOF
chmod +x "$TOOLS_DIR/bin/linuxdeploy-plugin-qt"

# ---------------------------------------------------------------------------
# Assemble the AppDir skeleton
# ---------------------------------------------------------------------------
APPDIR="$OUT_DIR/AppDir"
rm -rf "$OUT_DIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/applications"

cp "$BUILD_DIR/re-ps3-native" "$APPDIR/usr/bin/"
cp "$BUILD_DIR/xmb-manager"   "$APPDIR/usr/bin/"
cp "$BUILD_DIR/xmb-updater"   "$APPDIR/usr/bin/"

cat > "$APPDIR/usr/share/applications/re-ps3-native.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$APP_TITLE
Comment=Native reimplementation of the PlayStation 3 XrossMediaBar
Exec=$APP_NAME
Icon=$APP_NAME
Categories=Game;Emulator;
Terminal=false
X-AppImage-Version=$APP_VERSION
EOF

# Icon (hicolor set + top-level copies linuxdeploy/appimagetool expect).
ICON_DIR="$NATIVE_DIR/packaging/linux"
for sz in 64 128 256 512; do
  mkdir -p "$APPDIR/usr/share/icons/hicolor/${sz}x${sz}/apps"
  cp "$ICON_DIR/$APP_NAME-$sz.png" "$APPDIR/usr/share/icons/hicolor/${sz}x${sz}/apps/$APP_NAME.png"
done
mkdir -p "$APPDIR/usr/share/icons/hicolor/scalable/apps"
cp "$ICON_DIR/$APP_NAME.svg" "$APPDIR/usr/share/icons/hicolor/scalable/apps/$APP_NAME.svg"
cp "$APPDIR/usr/share/applications/re-ps3-native.desktop" "$APPDIR/re-ps3-native.desktop"
cp "$ICON_DIR/$APP_NAME-256.png" "$APPDIR/$APP_NAME.png"

# ---------------------------------------------------------------------------
# Deploy the Qt runtime + plugins into the AppDir (linuxdeploy + plugin-qt)
# ---------------------------------------------------------------------------
export QMAKE="$QT_ROOT/bin/qmake"
export PATH="$QT_ROOT/bin:$TOOLS_DIR/bin:$PATH"
export LD_LIBRARY_PATH="$QT_ROOT/lib:${LD_LIBRARY_PATH:-}"
export QT_PLUGIN_PATH="$QT_ROOT/plugins"

log "deploying Qt runtime (linuxdeploy --plugin qt)"
"$TOOLS_DIR/linuxdeploy/AppRun" \
  --appdir "$APPDIR" \
  --executable "$APPDIR/usr/bin/re-ps3-native" \
  --desktop-file "$APPDIR/usr/share/applications/re-ps3-native.desktop" \
  --icon-file "$APPDIR/usr/share/icons/hicolor/512x512/apps/re-ps3-native.png" \
  --plugin qt \
  >"$OUT_DIR/linuxdeploy.log" 2>&1

# ---------------------------------------------------------------------------
# AppRun: launch from a clean environment by pinning the bundled Qt runtime and
# plugins, so a stale QT_* / system Qt can never shadow them.
# ---------------------------------------------------------------------------
log "writing AppRun entry point"
cat > "$APPDIR/AppRun" <<'EOF'
#!/usr/bin/env bash
set -e
HERE="$(readlink -f "$(dirname "$0")")"
export APPDIR="$HERE"
export PATH="$APPDIR/usr/bin:$PATH"

# Bundled Qt first, everywhere. The linuxdeploy rpath ($ORIGIN/../lib) already finds
# the bundled libs; these are belt-and-suspenders for a clean launch and make the
# bundled plugin set authoritative.
export LD_LIBRARY_PATH="$APPDIR/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$APPDIR/usr/plugins${QT_PLUGIN_PATH:+:$QT_PLUGIN_PATH}"
export QT_QML2_IMPORT_PATH="$APPDIR/usr/qml${QT_QML2_IMPORT_PATH:+:$QT_QML2_IMPORT_PATH}"
export XDG_DATA_DIRS="$APPDIR/usr/share${XDG_DATA_DIRS:+:$XDG_DATA_DIRS}"

# QT_QPA_PLATFORM is intentionally left alone: Qt auto-selects xcb (X11) or wayland.
# Override it yourself (e.g. QT_QPA_PLATFORM=offscreen for headless/CI) if needed.

exec "$APPDIR/usr/bin/re-ps3-native" "$@"
EOF
chmod +x "$APPDIR/AppRun"

# ---------------------------------------------------------------------------
# Produce the AppImage
# ---------------------------------------------------------------------------
log "packing $OUTPUT_NAME with appimagetool $APPIMAGETOOL_VERSION"
"$TOOLS_DIR/appimagetool/AppRun" "$APPDIR" "$OUT_DIR/$OUTPUT_NAME" >"$OUT_DIR/appimagetool.log" 2>&1

log "done: $OUT_DIR/$OUTPUT_NAME"
ls -la "$OUT_DIR/$OUTPUT_NAME"
sha256sum "$OUT_DIR/$OUTPUT_NAME"
