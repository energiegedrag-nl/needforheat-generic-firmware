# Notes for working in this repository

Shared firmware library for the NeedForHeat measurement devices (ESP32, ESP-IDF, PlatformIO). Device firmwares
use it through `lib_deps = https://github.com/energiegedrag-nl/needforheat-generic-firmware#<tag>`.

## Branches and versions

* `release/v3`: the v3 line (tags v3.x) that the devices run on during the 2026–2027 heating season. Keep it stable;
  fixes only.
* `main`: the v4 line (build flags `CONFIG_NFH_*` instead of `CONFIG_TWOMES_*`, cleanups). Devices move to it after
  the heating season.
* Work on a feature branch and open a pull request; never commit directly to `main` or `release/v3`.

## Where things are

* Open work: `TODO.md`. Items that touch several repositories are written out in one lead repository; others link to it.
* Refactoring plan (moving shared code from device repositories into this library): `docs/developing/refactoring-plan.md`.
* Documentation site: `docs/` with `mkdocs.yml`.
* Test project used by CI: `test/generic-test`; host tests: `test/host`.

## Building and CI

* Build the test project: `pio run -e M5STACK_COREINK -d test/generic-test` (also `ESP32DEV`).
* `.github/workflows/build.yml` builds the test project; `build-devices.yml` builds every device firmware against the
  commit under test (blocking on `release/v3`, informative on `main`); `release.yml` makes a draft release from a tag.

## Related repositories

Device firmware: needforheat-living-room-module-firmware, needforheat-p1-reader-firmware,
needforheat-presence-detector-firmware, needforheat-boiler-monitor-firmware. Hardware with firmware impact:
needforheat-p1-base-hardware (P1-BASE v3), needforheat-battery-base-hardware (BatteryBASE). All under
github.com/energiegedrag-nl.

## Conventions

* Repository text (code, comments, docs, commit messages) in English.
* No references to non-public sources (internal boards, ticket numbers) in this public repository.
* Do not rewrite published history.
