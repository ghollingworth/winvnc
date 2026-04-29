#!/bin/bash
set -euo pipefail

# Build winvnc and all dependencies for Windows using MinGW cross-compilation.
#
# This builds a minimal neatvnc without TLS, H.264, GBM, or WebSocket support.
# Only Raw, ZRLE, and Tight (JPEG) encodings are included.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="$SCRIPT_DIR/prefix"
CC=x86_64-w64-mingw32-gcc
AR=x86_64-w64-mingw32-ar

NEATVNC="$SCRIPT_DIR/neatvnc"
AML="$SCRIPT_DIR/aml"
WINVNC="$SCRIPT_DIR"

CFLAGS="-O2 -Wall -std=gnu11"
CFLAGS="$CFLAGS -I$PREFIX/include -I$PREFIX/include/aml1 -I$PREFIX/include/pixman-1"
CFLAGS="$CFLAGS -I$NEATVNC/include -I$AML/include"
CFLAGS="$CFLAGS -I$NEATVNC/include/compat -I$NEATVNC/include/compat"
# Only force-include neatvnc's platform.h for neatvnc files.
# aml's compat header has close/read/write macros that collide with
# neatvnc's struct member names.
CFLAGS="$CFLAGS -include $NEATVNC/include/compat/platform.h"
CFLAGS="$CFLAGS -DAML_UNSTABLE_API=1"
CFLAGS="$CFLAGS -DHAVE_GETRANDOM=1"
VERSION_FLAG='-DPROJECT_VERSION="0.10-win"'
CFLAGS="$CFLAGS -DHAVE_JPEG=1"

LDFLAGS="-L$PREFIX/lib"
LIBS="-laml -lpixman-1 -lturbojpeg -ljpeg -lz -lws2_32 -lbcrypt -lpthread"

mkdir -p "$SCRIPT_DIR/build"

echo "=== Generating config.h ==="
cat > "$SCRIPT_DIR/build/config.h" << 'EOF'
/* Minimal config.h for Windows build of neatvnc */
#define HAVE_JPEG 1
/* No TLS, no GBM, no H264, no USDT, no WebSocket */
EOF

echo "=== Compiling neatvnc ==="
NEATVNC_SRCS=(
    src/auth/common.c
    src/server.c
    src/vec.c
    src/enc/zrle.c
    src/enc/raw.c
    src/pixels.c
    src/buffer.c
    src/buffer-pool.c
    src/frame.c
    src/frame-pool.c
    src/rcbuf.c
    src/stream/interface.c
    src/stream/common.c
    src/stream/tcp.c
    src/desktop-layout.c
    src/display.c
    src/enc/tight.c
    src/enc/util.c
    src/qnum-to-evdev.c
    src/transform-util.c
    src/damage-refinery.c
    src/enc/interface.c
    src/cursor.c
    src/logging.c
    src/base64.c
    src/bandwidth.c
    src/parallel-deflate.c
    src/compositor.c
    src/region.c
    src/crypto/random.c
)

OBJS=""
for src in "${NEATVNC_SRCS[@]}"; do
    obj="$SCRIPT_DIR/build/$(echo $src | tr '/' '_' | sed 's/\.c$/.obj/')"
    echo "  CC $src"
    $CC $CFLAGS "$VERSION_FLAG" -I"$SCRIPT_DIR/build" -c "$NEATVNC/$src" -o "$obj" 2>&1 || {
        echo "FAILED: $src"
        exit 1
    }
    OBJS="$OBJS $obj"
done

echo "  AR libneatvnc.a"
$AR rcs "$SCRIPT_DIR/build/libneatvnc.a" $OBJS

echo "=== Compiling winvnc ==="
for src in src/main.c src/dxgi-capture.c src/win-input.c; do
    obj="$SCRIPT_DIR/build/$(echo $src | tr '/' '_' | sed 's/\.c$/.obj/')"
    echo "  CC $src"
    $CC $CFLAGS -I"$WINVNC/include" -I"$SCRIPT_DIR/build" -c "$WINVNC/$src" -o "$obj"
done

echo "=== Linking winvnc.exe ==="
$CC -o "$SCRIPT_DIR/build/winvnc.exe" \
    "$SCRIPT_DIR/build/src_main.obj" \
    "$SCRIPT_DIR/build/src_dxgi-capture.obj" \
    "$SCRIPT_DIR/build/src_win-input.obj" \
    "$SCRIPT_DIR/build/libneatvnc.a" \
    $LDFLAGS $LIBS \
    -ld3d11 -ldxgi -static

echo ""
echo "=== Done ==="
ls -la "$SCRIPT_DIR/build/winvnc.exe"
