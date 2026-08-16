#!/usr/bin/env bash
# Clone and build the godot-cpp GDExtension bindings for the Godot backend.
#
# Usage: get-godot-cpp.sh [options]
#   --dir DIR         godot-cpp checkout (default: $GODOT_CPP_DIR, else ~/godot-cpp)
#   --tag TAG         git tag/branch to check out (default: godot-4.5-stable)
#   --platform NAME   macos | linux | windows (default: host)
#   --arch NAME       x86_64 | arm64 (default: host)
#   --target NAME     template_debug | template_release (default: template_debug)
#   --build-dir DIR   build tree (default: <dir>/build for a native build,
#                     <dir>/build-<platform>-<arch> when cross-compiling)
#   --jobs N          parallel jobs (default: all cores)
#
# The archive lands in <build-dir>/bin as
#   libgodot-cpp.<platform>.<target>.<arch>.a
# and the generated headers in <build-dir>/gen/include, which is exactly where
# `make GODOT=1` and the CMake GODOT target look for them.
#
# Windows builds are produced by cross-compiling with MinGW-w64; install
# `mingw-w64` (Debian/Ubuntu) or the equivalent for your distribution.

set -euo pipefail

GODOT_CPP_TAG_DEFAULT="godot-4.5-stable"

DIR="${GODOT_CPP_DIR:-$HOME/godot-cpp}"
TAG="$GODOT_CPP_TAG_DEFAULT"
PLATFORM=""
ARCH=""
TARGET="template_debug"
BUILD_DIR=""
JOBS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --dir) DIR="$2"; shift 2 ;;
        --tag) TAG="$2"; shift 2 ;;
        --platform) PLATFORM="$2"; shift 2 ;;
        --arch) ARCH="$2"; shift 2 ;;
        --target) TARGET="$2"; shift 2 ;;
        --build-dir) BUILD_DIR="$2"; shift 2 ;;
        --jobs) JOBS="$2"; shift 2 ;;
        -h|--help) awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "$0"; exit 0 ;;
        *) echo "error: unknown option '$1'" >&2; exit 1 ;;
    esac
done

# Host platform / arch, used as the default target.
case "$(uname -s)" in
    Darwin) HOST_PLATFORM=macos ;;
    Linux) HOST_PLATFORM=linux ;;
    MINGW*|MSYS*|CYGWIN*) HOST_PLATFORM=windows ;;
    *) echo "error: unsupported host '$(uname -s)'" >&2; exit 1 ;;
esac

case "$(uname -m)" in
    arm64|aarch64) HOST_ARCH=arm64 ;;
    x86_64|amd64) HOST_ARCH=x86_64 ;;
    *) echo "error: unsupported host arch '$(uname -m)'" >&2; exit 1 ;;
esac

PLATFORM="${PLATFORM:-$HOST_PLATFORM}"
ARCH="${ARCH:-$HOST_ARCH}"

if [ -z "$JOBS" ]; then
    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
fi

CROSS_COMPILING=0
if [ "$PLATFORM" != "$HOST_PLATFORM" ] || [ "$ARCH" != "$HOST_ARCH" ]; then
    CROSS_COMPILING=1
fi

if [ -z "$BUILD_DIR" ]; then
    if [ "$CROSS_COMPILING" -eq 1 ]; then
        BUILD_DIR="$DIR/build-$PLATFORM-$ARCH"
    else
        BUILD_DIR="$DIR/build"
    fi
fi

# Clone or update the checkout at the requested tag.
if [ -d "$DIR/.git" ]; then
    echo "==> reusing godot-cpp checkout at $DIR"
    if ! git -C "$DIR" describe --tags --exact-match 2>/dev/null | grep -qx "$TAG"; then
        echo "==> fetching $TAG"
        git -C "$DIR" fetch --depth 1 origin "refs/tags/$TAG:refs/tags/$TAG" --force
        git -C "$DIR" checkout --detach "$TAG"
    fi
else
    echo "==> cloning godot-cpp $TAG into $DIR"
    git clone --depth 1 --branch "$TAG" https://github.com/godotengine/godot-cpp.git "$DIR"
fi

CMAKE_ARGS=(
    -S "$DIR"
    -B "$BUILD_DIR"
    -DCMAKE_BUILD_TYPE=Release
    -DGODOTCPP_TARGET="$TARGET"
)

if command -v ninja >/dev/null 2>&1; then
    CMAKE_ARGS+=(-G Ninja)
fi

case "$PLATFORM" in
    macos)
        # A single-arch build; the Makefile matches the archive by arch name.
        CMAKE_ARGS+=(-DCMAKE_OSX_ARCHITECTURES="$ARCH")
        ;;
    linux)
        if [ "$CROSS_COMPILING" -eq 1 ]; then
            echo "error: cross-compiling to linux/$ARCH is not supported by this script" >&2
            exit 1
        fi
        ;;
    windows)
        if [ "$HOST_PLATFORM" != "windows" ]; then
            # MinGW-w64 cross-compile. godot-cpp's cmake/windows.cmake already
            # handles the non-MSVC path (static libgcc/libstdc++).
            case "$ARCH" in
                x86_64) MINGW_PREFIX=x86_64-w64-mingw32 ;;
                *) echo "error: unsupported windows arch '$ARCH'" >&2; exit 1 ;;
            esac
            if ! command -v "$MINGW_PREFIX-g++" >/dev/null 2>&1; then
                echo "error: $MINGW_PREFIX-g++ not found; install mingw-w64" >&2
                exit 1
            fi
            CMAKE_ARGS+=(
                -DCMAKE_SYSTEM_NAME=Windows
                -DCMAKE_SYSTEM_PROCESSOR="$ARCH"
                -DCMAKE_C_COMPILER="$MINGW_PREFIX-gcc"
                -DCMAKE_CXX_COMPILER="$MINGW_PREFIX-g++"
                -DCMAKE_RC_COMPILER="$MINGW_PREFIX-windres"
                -DCMAKE_FIND_ROOT_PATH="/usr/$MINGW_PREFIX"
                -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER
                -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY
                -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY
            )
        fi
        ;;
    *)
        echo "error: unsupported platform '$PLATFORM'" >&2
        exit 1
        ;;
esac

echo "==> configuring $PLATFORM/$ARCH ($TARGET) in $BUILD_DIR"
cmake "${CMAKE_ARGS[@]}"

echo "==> building with $JOBS jobs"
cmake --build "$BUILD_DIR" --parallel "$JOBS"

EXPECTED="$BUILD_DIR/bin/libgodot-cpp.$PLATFORM.$TARGET.$ARCH.a"
if [ ! -f "$EXPECTED" ]; then
    echo "error: expected archive not produced: $EXPECTED" >&2
    echo "built instead:" >&2
    ls -1 "$BUILD_DIR/bin" >&2 || true
    exit 1
fi

echo
echo "godot-cpp ready:"
echo "  archive:  $EXPECTED"
echo "  headers:  $BUILD_DIR/gen/include"
echo
echo "Build the extension with:"
if [ "$CROSS_COMPILING" -eq 1 ]; then
    echo "  make GODOT=1 GODOT_CPP_DIR=$DIR GODOT_CPP_BUILD_DIR=$BUILD_DIR \\"
    echo "       CROSS=x86_64-w64-mingw32- -j$JOBS"
else
    echo "  make GODOT=1 GODOT_CPP_DIR=$DIR -j$JOBS"
fi
