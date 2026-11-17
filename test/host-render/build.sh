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

# LovyanGFX (framebuffer platform: no SDL needed) and the U8g2 fonts, without warnings: not our code. The fonts are
# C on purpose: their string initialisers have no room for a terminating NUL.
OBJS=""
for f in src/util/fonts/u8g2_fonts.c "$L"/lgfx/utility/*.c "$L"/lgfx/Fonts/efont/*.c "$L"/lgfx/Fonts/IPA/*.c 	"$L"/lgfx/v1/*.cpp "$L"/lgfx/v1/misc/*.cpp "$L"/lgfx/v1/panel/Panel_Device.cpp "$L"/lgfx/v1/platforms/framebuffer/*.cpp; do
	o="$WORK/obj/$(echo "$f" | tr '/' '_').o"
	if [ ! -f "$o" ]; then
		mkdir -p "$WORK/obj"
		case "$f" in
		*.c) ${CC:-gcc} -std=gnu11 -O1 -w -I include -isystem "$L" -c "$f" -o "$o" ;;
		*) ${CXX:-g++} -std=c++17 -O1 -w -isystem "$L" -DLGFX_LINUX_FB -c "$f" -o "$o" ;;
		esac
	fi
	OBJS="$OBJS $o"
done

# This repo's code, with warnings as errors.
${CXX:-g++} -std=c++17 -O1 -Wall -Wextra -Werror -I include -isystem "$L" -DLGFX_LINUX_FB -DSCREEN_LAYOUT_FONT_COMPARISON \
	test/host-render/render_screens.cpp src/util/screen_layout.cpp $OBJS -lpthread -o "$WORK/render_screens"

"$WORK/render_screens" "$OUT"
