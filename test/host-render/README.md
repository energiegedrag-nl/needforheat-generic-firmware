# Host renderer of the e-ink screens

`render_screens.cpp` draws every screen of `include/util/screen_layout.hpp` with sample data on a 200x200 1-bit
sprite, once per font set in `ScreenLayout::FontSets()`, and writes them as PBM images. `to_png.py` turns them into
PNG at 1:1 and x3, plus one overview sheet per font set. The same drawing code runs on the device, so what you see
here is what the CoreInk panel shows (1-bit, no grey, no anti-aliasing).

CI runs it in the job `screen-renders` of `.github/workflows/build.yml`; download the artifact `eink-screens` of a
run to look at the screens.

On Linux (gcc, g++, git, Python with Pillow), from the repository root:

```sh
test/host-render/build.sh build/screens
python test/host-render/to_png.py build/screens
```

`build.sh` fetches LovyanGFX 0.4.18 (the version `platformio.ini` resolves to) and uses its Linux framebuffer
platform only for the build; nothing is drawn on a real framebuffer.

Files are named `<screen>-font<n>`; `font0` is the font set in use on the device.
