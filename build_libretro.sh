#!/bin/sh
set -e

# Base directory setup
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Configurable environment variables (uses existing value if set)
OUTDIR="${OUTDIR:-$SCRIPT_DIR/out}"
APP_OUTDIR="${APP_OUTDIR:-$OUTDIR/app}"
HB_OUTDIR="${HB_OUTDIR:-$OUTDIR/harfbuzz}"
SB_OUTDIR="${SB_OUTDIR:-$OUTDIR/sheenbidi}"

MIPS="${MIPS:-$HOME/sf3000-work/sf3000toolchain/mipsel-buildroot-linux-gnu_sdk-buildroot/opt/ext-toolchain/bin/mips-mti-linux-gnu-}"
SYSROOT="${SYSROOT:-$HOME/sf3000-work/sf3000toolchain/mipsel-buildroot-linux-gnu_sdk-buildroot/mipsel-buildroot-linux-gnu/sysroot}"
HARFBUZZ_DIR="${HARFBUZZ_DIR:-$SCRIPT_DIR/external/harfbuzz}"
SHEENBIDI_DIR="${SHEENBIDI_DIR:-$SCRIPT_DIR/external/SheenBidi}"

CC="${MIPS}gcc"

cd "$SCRIPT_DIR"

# Source and Object definitions
HB_SRC="$HARFBUZZ_DIR/src/harfbuzz.cc"
HB_OBJ="$HB_OUTDIR/harfbuzz.lo"

SB_SRC="$SHEENBIDI_DIR/Source/SheenBidi.c"
SB_OBJ="$SB_OUTDIR/SheenBidi.lo"

TARGET="$OUTDIR/frogui_libretro.so"

# Submodule initialization check
if [ ! -f "$HB_SRC" ] || [ ! -f "$SB_SRC" ]; then
    echo "Initializing git submodules..."
    git submodule update --init --recursive
fi

# Ensure output directories exist
mkdir -p "$OUTDIR" "$APP_OUTDIR" "$HB_OUTDIR" "$SB_OUTDIR"

# Compiler & Linker Flags
INCLUDES="-I. \
          -I./common \
          -I$SYSROOT/usr/include \
          -I$SYSROOT/usr/include/freetype2 \
          -I$HARFBUZZ_DIR/src \
          -I$SHEENBIDI_DIR \
          -I$SHEENBIDI_DIR/Headers \
          -I$SHEENBIDI_DIR/Source"

CFLAGS="-mips32r2 -march=mips32r2 -mtune=24kc -mfp32 -mhard-float -mlong-calls -EL \
        --sysroot=$SYSROOT -fPIC -G0 -Wall $INCLUDES -Ofast \
        -ffunction-sections -fdata-sections \
        -DPLATFORM_SF3000 -DNDEBUG -D__LIBRETRO__"

# Size-optimized HarfBuzz flags (Thread-safety preserved)
HB_CFLAGS="$CFLAGS \
           -Os \
           -fno-exceptions \
           -fno-rtti \
           -DHB_MINI \
           -DHB_LEAN \
           -DHB_NO_CDECL \
           -DHB_NO_SETLOCALE \
           -DHB_NO_OT_FONT \
           -DHB_NO_FALLBACK_SHAPE \
           -DHAVE_FREETYPE"

LDFLAGS="-mips32r2 -mhard-float -mfp32 -EL -fPIC -shared -nostdlib -Wl,--no-undefined \
         -Wl,--gc-sections \
         --sysroot=$SYSROOT -L$SYSROOT/usr/lib"

LDLIBS="-lfreetype -lm -lc -ldl -lpthread -lstdc++ -lgcc"

# Compile Application Objects
APP_SRCS="frogui_libretro.c render.c font.c recent_games.c theme.c favorites.c banner.c backlight.c input.c core_override.c ext_filter.c common/i18n.c"
OBJS=""

for src in $APP_SRCS; do
    base="$(basename "${src%.c}")"
    obj="$APP_OUTDIR/$base.lo"
    echo "CC $src"
    $CC $CFLAGS -c -o "$obj" "$src"
    OBJS="$OBJS $obj"
done

# Compile SheenBidi Unity
echo "CC Unity SheenBidi $SB_SRC"
$CC $CFLAGS -DSB_CONFIG_UNITY -w -c -o "$SB_OBJ" "$SB_SRC"
OBJS="$OBJS $SB_OBJ"

# Compile HarfBuzz Bundle
echo "CXX (HarfBuzz) $HB_SRC"
$CC $HB_CFLAGS -c -o "$HB_OBJ" "$HB_SRC"
OBJS="$OBJS $HB_OBJ"

# Copy License Files
echo "Copying HarfBuzz License..."
if [ -f "$HARFBUZZ_DIR/COPYING" ]; then
    cp "$HARFBUZZ_DIR/COPYING" "$OUTDIR/frogui_libretro.harfbuzz.LICENSE"
else
    echo "HarfBuzz license file not found!" && exit 1
fi

echo "Copying SheenBidi License..."
if [ -f "$SHEENBIDI_DIR/LICENSE" ]; then
    cp "$SHEENBIDI_DIR/LICENSE" "$OUTDIR/frogui_libretro.sheenbidi.LICENSE"
else
    echo "SheenBidi license file not found!" && exit 1
fi

# Link Target
echo "Linking $TARGET..."
$CC $LDFLAGS $OBJS $LDLIBS -o "$TARGET"

ls -la "$TARGET"
echo "Build successful!"
