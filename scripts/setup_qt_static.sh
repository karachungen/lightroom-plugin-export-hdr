#!/usr/bin/env bash
# Build a real static Qt kit. aqt's prebuilt desktop archives are shared builds
# and cannot produce the single-file plugin binary required by this project.
set -euo pipefail

QT_VERSION="${QT_VERSION:-6.11.0}"
PREFIX="${QT_STATIC_ROOT:-$HOME/Qt/$QT_VERSION-static}"
CACHE_DIR="${QT_SOURCE_CACHE:-$HOME/.cache/uhdr-qt-static}"
ARCHIVE="$CACHE_DIR/qt-everywhere-src-$QT_VERSION.tar.xz"
SOURCE_DIR="$CACHE_DIR/qt-everywhere-src-$QT_VERSION"
BUILD_DIR="$CACHE_DIR/build-$QT_VERSION-static"
QT_MINOR="${QT_VERSION%.*}"
URL="https://download.qt.io/archive/qt/$QT_MINOR/$QT_VERSION/single/qt-everywhere-src-$QT_VERSION.tar.xz"

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "This helper currently builds the macOS ARM64 static kit." >&2
  echo "On Windows, configure a static Qt 6.11 MSVC kit and set QT_STATIC_ROOT to its prefix." >&2
  exit 1
fi
if [[ "$(uname -m)" != "arm64" ]]; then
  echo "The plugin release kit requires Apple Silicon." >&2
  exit 1
fi

for command in cmake ninja curl tar; do
  command -v "$command" >/dev/null || {
    echo "Missing required command: $command" >&2
    exit 1
  }
done

mkdir -p "$CACHE_DIR"
if [[ ! -f "$ARCHIVE" ]]; then
  echo "==> Downloading Qt $QT_VERSION sources"
  curl --fail --location --retry 3 "$URL" --output "$ARCHIVE"
fi
if [[ ! -d "$SOURCE_DIR" ]]; then
  echo "==> Extracting Qt sources"
  tar -xf "$ARCHIVE" -C "$CACHE_DIR"
fi

echo "==> Configuring static Qt $QT_VERSION at $PREFIX"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
(
  cd "$BUILD_DIR"
  "$SOURCE_DIR/configure" \
    -prefix "$PREFIX" \
    -release \
    -static \
    -opensource \
    -confirm-license \
    -nomake examples \
    -nomake tests \
    -submodules qtbase,qtshadertools \
    -- \
    -GNinja \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
    -DQT_BUILD_TOOLS_BY_DEFAULT=ON \
    -DQT_BUILD_TESTS=OFF \
    -DQT_BUILD_EXAMPLES=OFF
)

echo "==> Building and installing static Qt (this can take a while)"
cmake --build "$BUILD_DIR" --parallel
cmake --install "$BUILD_DIR"

echo
echo "Static Qt installed. Build the single executable with:"
echo "  export QT_STATIC_ROOT=\"$PREFIX\""
echo "  ./scripts/build_plugin.sh build --clean"
