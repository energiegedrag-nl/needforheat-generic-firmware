"""Converts the PBM screens of the host renderer to PNG: each screen at 1:1 and x3, and one overview sheet per
font set (all screens side by side at x2), to compare the font sets. Needs Pillow.

Usage: python test/host-render/to_png.py build/screens
"""

import pathlib
import sys

from PIL import Image, ImageDraw

SCALE = 3
SHEET_SCALE = 2
GAP = 12
COLUMNS = 5


def main(folder: pathlib.Path) -> int:
    pbms = sorted(folder.glob("*.pbm"))
    if not pbms:
        print(f"No PBM files in {folder}", file=sys.stderr)
        return 1

    sets: dict[str, list[tuple[str, Image.Image]]] = {}
    for pbm in pbms:
        image = Image.open(pbm).convert("1")
        image.save(pbm.with_suffix(".png"))
        image.resize((image.width * SCALE, image.height * SCALE), Image.NEAREST).save(
            pbm.with_name(pbm.stem + f"-x{SCALE}.png")
        )
        screen, _, font = pbm.stem.rpartition("-")
        sets.setdefault(font, []).append((screen, image))

    for font, screens in sets.items():
        w = screens[0][1].width * SHEET_SCALE
        h = screens[0][1].height * SHEET_SCALE
        rows = (len(screens) + COLUMNS - 1) // COLUMNS
        sheet = Image.new("L", (COLUMNS * (w + GAP) + GAP, rows * (h + GAP + 16) + GAP), 160)
        draw = ImageDraw.Draw(sheet)
        for i, (screen, image) in enumerate(screens):
            x = GAP + (i % COLUMNS) * (w + GAP)
            y = GAP + (i // COLUMNS) * (h + GAP + 16)
            draw.text((x, y), screen, fill=0)
            sheet.paste(image.resize((w, h), Image.NEAREST).convert("L"), (x, y + 16))
        sheet.save(folder / f"sheet-{font}.png")
    print(f"{len(pbms)} screens, {len(sets)} font sets")
    return 0


if __name__ == "__main__":
    sys.exit(main(pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "build/screens")))
