#!/bin/sh
# Builds and runs the host renderer of the e-ink screens on Linux (see README.md in this folder).
# Usage: test/host-render/build.sh [output folder]   (run from the repository root)
set -eu

LGFX_VERSION=0.4.18 # the same LovyanGFX as lib_deps in platformio.ini resolves to
OUT=${1:-build/screens}
WORK=build/host-render
mkdir -p "$WORK" "$OUT"

if [ ! -d "$WORK/LovyanGFX" ]; then
	git clone --quiet --depth 1 --branch "$LGFX_VERSION" https://github.com/lovyan03/LovyanGFX.git "$WORK/LovyanGFX"
fi
L="$WORK/LovyanGFX/src"

# C sources: U8g2 fonts (C on purpose: their string initialisers have no room for a terminating NUL) and the
# LovyanGFX utilities; then the C++ sources of LovyanGFX (framebuffer platform: no SDL needed) and of this repo.
OBJS=""
for c in src/util/fonts/u8g2_fonts.c "$L"/lgfx/utility/*.c "$L"/lgfx/Fonts/efont/*.c "$L"/lgfx/Fonts/IPA/*.c; do
	o="$WORK/$(echo "$c" | tr '/' '_').o"
	${CC:-gcc} -O1 -c -I include -isystem "$L" "$c" -o "$o"
	OBJS="$OBJS $o"
done
${CXX:-g++} -std=c++17 -O1 -Wall -Wextra -I include -isystem "$L" -DLGFX_LINUX_FB \
	test/host-render/render_screens.cpp src/util/screen_layout.cpp \
	"$L"/lgfx/v1/*.cpp "$L"/lgfx/v1/misc/*.cpp "$L"/lgfx/v1/panel/Panel_Device.cpp \
	"$L"/lgfx/v1/platforms/framebuffer/*.cpp $OBJS -lpthread -o "$WORK/render_screens"

"$WORK/render_screens" "$OUT"
