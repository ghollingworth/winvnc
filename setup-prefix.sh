#!/bin/bash
set -euo pipefail

# Build the C-side static dependencies (aml, pixman, libjpeg-turbo, zlib) for
# the x86_64-w64-mingw32 target and install them into ./prefix/. After this
# completes, ./build-win.sh has everything it needs to link winvnc.exe.
#
# Idempotent: each library is skipped if its primary .a file already exists
# in prefix/lib/. Pass --rebuild to force a clean rebuild of all four.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="$SCRIPT_DIR/prefix"
BUILD="$SCRIPT_DIR/build-deps"
CROSS="$SCRIPT_DIR/win64-cross.ini"
TARGET=x86_64-w64-mingw32

REBUILD=0
for arg in "$@"; do
    case "$arg" in
        --rebuild) REBUILD=1 ;;
        *) echo "unknown arg: $arg" >&2; exit 1 ;;
    esac
done

# --- prerequisite check ---
need() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "ERROR: '$1' not found on PATH. Install it first." >&2
        echo "       (Ubuntu: $2)" >&2
        exit 1
    fi
}
need ${TARGET}-gcc      "apt install gcc-mingw-w64-x86-64"
need ${TARGET}-ar       "apt install gcc-mingw-w64-x86-64"
need ${TARGET}-ranlib   "apt install gcc-mingw-w64-x86-64"
need meson              "apt install meson"
need ninja              "apt install ninja-build"
need cmake              "apt install cmake"
need make               "apt install make"

mkdir -p "$PREFIX/lib" "$PREFIX/include" "$BUILD"

skip_if_built() {
    local name="$1" lib="$2"
    if [[ "$REBUILD" -eq 0 && -f "$PREFIX/lib/$lib" ]]; then
        echo "[$name] skipping (prefix/lib/$lib already present; --rebuild to force)"
        return 0
    fi
    return 1
}

# pkg-config search path is local to this prefix; cross-file no longer hardcodes
# a user-specific path, so set it via env var instead.
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH=

# --- aml --------------------------------------------------------------------
if ! skip_if_built aml libaml.a; then
    echo "[aml] building"
    rm -rf "$BUILD/aml"
    meson setup --cross-file="$CROSS" \
        --prefix="$PREFIX" --buildtype=release \
        --default-library=static \
        "$BUILD/aml" "$SCRIPT_DIR/aml"
    meson install -C "$BUILD/aml"
fi

# --- pixman -----------------------------------------------------------------
if ! skip_if_built pixman libpixman-1.a; then
    echo "[pixman] building"
    rm -rf "$BUILD/pixman"
    meson setup --cross-file="$CROSS" \
        --prefix="$PREFIX" --buildtype=release \
        --default-library=static \
        -Dlibpng=disabled -Dgtk=disabled -Ddemos=disabled -Dtests=disabled \
        "$BUILD/pixman" "$SCRIPT_DIR/deps/pixman"
    meson install -C "$BUILD/pixman"
fi

# --- libjpeg-turbo (provides libjpeg.a + libturbojpeg.a) --------------------
if ! skip_if_built libjpeg-turbo libturbojpeg.a; then
    echo "[libjpeg-turbo] building"
    rm -rf "$BUILD/libjpeg-turbo"
    cmake -B "$BUILD/libjpeg-turbo" -S "$SCRIPT_DIR/deps/libjpeg-turbo" \
        -DCMAKE_SYSTEM_NAME=Windows \
        -DCMAKE_SYSTEM_PROCESSOR=AMD64 \
        -DCMAKE_C_COMPILER=${TARGET}-gcc \
        -DCMAKE_RC_COMPILER=${TARGET}-windres \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_SHARED=OFF \
        -DENABLE_STATIC=ON \
        -DWITH_JPEG8=ON \
        -DWITH_TURBOJPEG=ON \
        -DWITH_TOOLS=OFF \
        -DWITH_TESTS=OFF \
        -GNinja
    cmake --build "$BUILD/libjpeg-turbo"
    cmake --install "$BUILD/libjpeg-turbo"
fi

# --- zlib --------------------------------------------------------------------
if ! skip_if_built zlib libz.a; then
    echo "[zlib] building"
    (
        cd "$SCRIPT_DIR/deps/zlib"
        # Out-of-tree build dir for zlib's Makefile.gcc isn't supported, so we
        # have to use the source dir but we can clean it.
        make -f win32/Makefile.gcc clean >/dev/null 2>&1 || true
        PREFIX_TC=${TARGET}- \
            make -f win32/Makefile.gcc \
            CC=${TARGET}-gcc \
            AR=${TARGET}-ar \
            RC=${TARGET}-windres \
            STRIP=${TARGET}-strip \
            libz.a
        # Manual install — Makefile.gcc's install target is fussy.
        cp libz.a       "$PREFIX/lib/libz.a"
        cp zlib.h zconf.h "$PREFIX/include/"
    )
fi

echo
echo "==> Done. prefix/ contents:"
ls -la "$PREFIX/lib"/*.a 2>/dev/null
echo
echo "Next: ./build-win.sh"
