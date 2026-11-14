# Refactoring plan: more into the generic firmware

Status: plan (November 2026). Step 1 is done in the pull request that adds this page.

## Why

The NeedForHeat device firmwares use this library through `lib_deps` in their `platformio.ini`, pinned to a tag.
In November 2026 the four devices pinned four different versions, while the library itself had reached v4.0.0:

| Device firmware | Pinned version |
|---|---|
| needforheat-living-room-module-firmware | v3.4.0-rc.2 |
| needforheat-p1-reader-firmware | v3.3.0 |
| needforheat-presence-detector-firmware | v3.1.0 |
| needforheat-boiler-monitor-firmware | v3.1.0 |

This is partly deliberate: the devices run on the **v3 line** (branch `release/v3`, tags v3.x), which is kept
stable for the 2026–2027 heating season, while `main` is the **v4 line** (renamed build flags `CONFIG_NFH_*`,
cleanups). Still, improvements made here do not reach the devices, and improvements made in a device repository
stay there:

* The living-room module has its own copy of `i2c_hal.c` (`i2c_hal_port_1.c`), only with another I²C port and other
  pins (the SCD41 on the Grove port, G32/G33, or on a proto HAT, G25/G26).
* The P1 reader has its own driver for the P1-BASE v3 (`p1base_v3.c`): charging the CoreInk battery from the meter
  and measuring the meter voltage. The planned BatteryBASE (3 × AAA) needs the same kind of power management, and
  so does every CoreInk device that can sit on either BASE.
* The e-ink screens of all CoreInk devices share one layout (status bar with Wi-Fi, clock, server and power source;
  full and partial refresh), but each device would implement it again.
* The library's own CI builds only `test/generic-test`, so a change here can break a device without anyone noticing
  until that device moves its pin.

## Rule

Anything that is not tied to one sensor or protocol goes into this library first, through a pull request here.
Device repositories keep only their own sensors and protocols (SCD41, P1/DSMR, OpenTherm, DS18B20, ...).

## Steps

### 1. Build every device against the library under test (done)

`.github/workflows/build-devices.yml` checks out each device firmware, points its `lib_deps` at the commit under
test (instead of the pinned tag) and builds it. It runs on every pull request, on `main`, weekly and on demand.

* On `release/v3` it must pass: a change to the v3 line may not break a device. Make it blocking there.
* On `main` it shows how far the devices are from v4; it is not blocking until the devices have moved (step 2).

First result (November 2026): all four devices compile up to `generic_esp_32.cpp` and then stop at the same point:
v4 expects the server to be chosen with `CONFIG_NFH_SERVER_HOSTNAME`, `CONFIG_NFH_TEST_SERVER` or
`CONFIG_NFH_PRODUCTION_SERVER`, while the devices still set the v3 flags (`CONFIG_TWOMES_*`). This is expected (the devices
are on the v3 line); renaming those flags in each device's `platformio.ini` is the first task of the move to v4.

### 2. One version for all devices

First on the v3 line: move every device to the latest v3 tag (they pin v3.1.0 to v3.4.0-rc.2). After the heating
season, move them to v4 (`main`), one pull request per device, with the build from step 1 as the check. Fix in the
library (not in the device) whatever breaks.

### 3. Move shared code into the library

In this order, each as its own pull request with the devices updated after it:

1. **I²C with configurable port and pins**: port 0 on the internal bus (G21/G22, shared with the RTC), port 1 on the
   Grove port or a HAT. Removes the copy in the living-room module.
2. **Power manager** for CoreInk devices:
   * power sources: USB, internal LiPo, P1-BASE (P1 charging), BatteryBASE (3 × AAA);
   * detection of the BASEs at run time (I²C address for the BatteryBASE, meter voltage for the P1-BASE) instead of
     build flags;
   * coordination of the chargers: at most one BASE charges at a time, the total stays below ≈ 0.5 C of the 390 mAh
     cell (the CoreInk's own charger delivers ≈ 200 mA on USB);
   * state of charge and remaining hours, a warning to replace or recharge cells, and the measurements uploaded as
     properties.
   The P1-BASE charging control moves here from the P1 reader; reading and parsing P1 telegrams stays in the P1
   reader.
3. **E-ink screen layout**: the status bar, the refresh policy (partial refresh for measurements and the clock, a
   full refresh after ten partial ones) and drawing in a frame buffer; the device fills in the content area.

### 4. Version updates in the devices

When a version of this library is released, a small workflow opens a pull request in every device repository that
updates the pin; the device's own CI checks it.

## Related hardware

* [needforheat-p1-base-hardware](https://github.com/energiegedrag-nl/needforheat-p1-base-hardware): P1-BASE v3,
  with P1 charging of the CoreInk battery.
* [needforheat-battery-base-hardware](https://github.com/energiegedrag-nl/needforheat-battery-base-hardware):
  BatteryBASE, 3 × AAA cells that charge the CoreInk battery.
