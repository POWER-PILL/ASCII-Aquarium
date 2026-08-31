# ASCII Aquarium — Marauder Pancake port

A port of POWER-PILL's ASCII Aquarium (v2.39) to the **Marauder Pancake**
hardware: ESP32-C5, ST7796 320×480 panel, FT6336 capacitive touch. The aquarium
runs **full-screen in 480×320 landscape** (hold the device sideways).

## What changed vs. the stock CYD sketch

The sketch keeps all the original features and adds a new board profile,
`AQUARIUM_BOARD_PANCAKE` (defined at the very top of the `.ino`). Compared with
the CYD/ST7796U resistive-touch builds:

| Area | Pancake behaviour |
|------|-------------------|
| Display | ST7796, TFT_eSPI rotation 1 → 480×320, aquarium fills the whole panel |
| Render surface | Forced into internal RAM and rendered in horizontal strips (a full 480×320 canvas would land in slow PSRAM) |
| Touch | FT6336U over I2C (`ft6336_touch.h`), SDA=9 / SCL=10 / RST=8, addr 0x38 |
| SD card | Shares the FSPI bus with the TFT (SD_CS=7, shared MISO/MOSI/SCLK) |
| Backlight | GPIO 26 (PWM) |
| RGB ambient LEDs | none — those controls are inert on Pancake |
| BOOT-button screenshot | disabled (no button wired); use the on-screen **Capture** panel instead |
| UI panels | re-centred for the 480-wide screen via `UI_X_SHIFT` |

## Required libraries

> **Critical:** stock TFT_eSPI does **not** support the ESP32-C5. You must build
> against the **ESP32-C5 fork of TFT_eSPI** that ships with your Marauder Bible
> firmware:
> `.../MarauderBible/bible_firmware/libraries/TFT_eSPI-ESP32-C5`

Point the Arduino IDE at that library (or copy it into your Arduino
`libraries/` folder). Then configure it for the Pancake panel exactly as the
Bible firmware does:

In `TFT_eSPI-ESP32-C5/User_Setup_Select.h`, make sure this line is active:

```cpp
#include <User_Setup_marauder_pancake.h>
//#include <User_Setup_marauder_v8.h>
//#include <User_Setup_og_marauder.h>
```

A copy of that setup (ST7796, C5 pins, `TFT_INVERSION_ON`) is included here as
`User_Setup_marauder_pancake.h` for reference — the pin numbers in the sketch
match it. Other libraries used: `SD`, `Preferences`, `WiFi` (all bundled with the
ESP32 core). `XPT2046_Touchscreen` is **not** needed for the Pancake build.

## Arduino IDE settings (same core as the Bible firmware)

| Setting | Value |
|---------|-------|
| Board | ESP32C5 Dev Module |
| Flash Size | 8 MB |
| PSRAM | Enabled |
| Partition Scheme | Default (or any scheme with ≥1.5 MB app) |

This is a standalone app (it does **not** use the Bible's dual-boot
`partitions.csv`); a normal single-app partition scheme is fine.

## Build & flash

1. Open `ASCII_Aquarium_Pancake/ASCII_Aquarium_Pancake.ino` in the Arduino IDE.
2. Confirm `#define AQUARIUM_BOARD_PANCAKE` is present at the top (it is by default).
3. Make sure TFT_eSPI-ESP32-C5 is selected for the Pancake ST7796 setup (above).
4. Select the ESP32C5 Dev Module board and your serial port.
5. Upload.

On boot you should see a short diagnostic splash (render size, heap/PSRAM,
`Touch: OK`) then the aquarium.

## Building in GitHub Actions (no local toolchain needed)

A workflow at `.github/workflows/build-pancake.yml` compiles this sketch in the
cloud against the H4W9 ESP32-C5 fork of TFT_eSPI
(`https://github.com/H4W9/TFT_eSPI` branch `ESP32-C5`) and publishes the compiled
`.bin`/`.elf` as a downloadable artifact.

What it does:
1. Installs Arduino CLI + the ESP32 core (`ESP32_CORE_VERSION`, default `3.3.0`).
2. `git clone --branch ESP32-C5` of the TFT_eSPI fork into the sketchbook.
3. Forces the Pancake ST7796/C5 setup by copying
   `User_Setup_marauder_pancake.h` in as the library's default `User_Setup.h`
   (the file the fork's `User_Setup_Select.h` already loads by default). The
   Select header is left untouched because it also carries the driver-defines
   dispatch that pulls in `TFT_Drivers/ST7796_Defines.h`.
4. Compiles for `esp32:esp32:esp32c5` (`PartitionScheme=huge_app,PSRAM=enabled`).
5. Uploads the binaries as artifact `ascii-aquarium-pancake-<sha>`.

To run it: push this folder + the workflow to `H4W9/ASCII-Aquarium`, then either
push to `main` or trigger it manually from the repo's **Actions → Build ASCII
Aquarium (Pancake) → Run workflow**. Download the firmware from the run's
**Artifacts** section (`bin-pancake`).

Knobs live in the workflow's `env:` block (core version, TFT_eSPI repo/branch).
If the ESP32 core rejects a `--board-options` key, the **Show board options**
step in the log lists the valid keys for your core version.

### Optional release

Following the same template as `ESP32_FlipSocial`, the workflow can cut a GitHub
release. It triggers on either:

- a **`v*` tag push**, or
- a **manual run with `release` = true** (Actions → Run workflow → tick the box).

The release job downloads the build artifacts and publishes them with `gh release
create`, deleting/refreshing an existing release of the same tag. Binaries follow
the template naming, with the version read from `kSketchVersionLabel` in the `.ino`:

```
ASCII_Aquarium_v2_39_pancake.bin   # the app
ASCII_Aquarium_bootloader.bin
ASCII_Aquarium_partitions.bin
ASCII_Aquarium_merged.bin          # single-file image, if the core emits one
```

**Tag naming — one deliberate difference from the FlipSocial template.** This repo
is a fork of `POWER-PILL/ASCII-Aquarium`, which already ships tags `v2.20` / `v2.39`.
Releasing at a bare `v2.39` would reuse the *upstream* tag (pointing at upstream's
commit) and silently ignore `--target`. So the release tag is suffixed:

```
v<version>-pancake      →   v2.39-pancake
```

Change `TAG=` in the workflow's *Create / refresh release* step if you want a
different scheme.

## Clock pinch-to-zoom

When the clock is showing, a **two-finger pinch** scales the clock — and only the
clock — larger or smaller. The size persists across reboots (NVS key `clk_zoom`)
and applies to both the small-text and large ASCII clock styles.

- **Notches:** `CLOCK_ZOOM_NOTCHES` (5) discrete steps from `CLOCK_ZOOM_MIN_SCALE`
  (1.0× = original, the smallest) to `CLOCK_ZOOM_MAX_SCALE` (2.0× = biggest), i.e.
  1.00 / 1.25 / 1.50 / 1.75 / 2.00. Adjust those constants near the top of the `.ino`.
- **Fractional scaling:** `setTextSize` is integer-only, so the small-text clock is
  rendered once at 1× into the reusable clock sprite and **nearest-neighbour blitted**
  at the notch's scale (`drawSmallClockScaled`). The ASCII art clock, already large,
  uses the nearest integer size (1× or 2×).
- Uses the FT6336's second touch point (`ft6336_read_points()`); the finger spread
  snaps to the nearest notch. A pinch never registers as a tap/feed, and is a no-op
  when the clock is hidden.

## Touch orientation tuning

The FT6336 reports coordinates in the panel's native portrait frame; the sketch
rotates them into the 480×320 landscape scene in `readCapTouchPoint()`. If taps
land **mirrored** on your unit, flip the flags near that function:

```cpp
static const bool CAP_TOUCH_INVERT_X = false;  // long (480) axis
static const bool CAP_TOUCH_INVERT_Y = false;  // short (320) axis
```

- Taps mirror **left↔right** → set `CAP_TOUCH_INVERT_X = true`.
- Taps mirror **up↕down** → set `CAP_TOUCH_INVERT_Y = true`.

The **Flip Display** option (Tank settings) also swaps the whole scene between
TFT rotation 1 and 3; touch follows it automatically.

## Wi-Fi (dual-band ESP32-C5)

The C5 is a **dual-band 2.4/5 GHz** part, unlike the classic ESP32 this sketch
was written for. The stock code just called `WiFi.begin(ssid, pass)` and let the
chip pick an AP — on a **combined 2.4/5 GHz SSID** it could latch onto the 5 GHz
side and time out, which looked like a plain "Connecting…" failure even with the
right password.

The port keeps **both bands enabled** and makes connecting reliable by targeting
a specific access point:

- `ensureWifiRadioStarted()` sets `esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO)`
  so scans cover 2.4 **and** 5 GHz. (Change `AQUARIUM_WIFI_BAND_MODE` near the top
  of the `.ino` to `_2G_ONLY` / `_5G_ONLY` to force a band.)
- The scan collapses a combined SSID to one list row but remembers the **strongest
  AP's BSSID + channel**, so tapping the name connects to whichever band has the
  better signal — you get 5 GHz when you're close to it, 2.4 GHz otherwise.
- Connecting uses `WiFi.begin(ssid, pass, channel, bssid)` to associate with that
  exact AP instead of re-guessing the band.
- The AP you actually associate with is remembered, so **reconnects** target the
  same BSSID/band. If that AP has roamed/changed, the remembered target is dropped
  after a timeout and the next attempt falls back to a plain scan-and-connect.

If a connect still fails, open the serial monitor — the code logs the band-mode
result and connection status.

## SD-card capture

Screenshots / frame sequences (BMP) save to the SD card over the shared FSPI
bus. Open the HUD (tap the top-left corner) → **Capture**. Capture is slow by
design (the sim is throttled so no frames are skipped).

## Known trade-offs

- **Frame rate:** 480×320 is ~2× the CYD's pixel count pushed at 27 MHz, so
  expect a calmer ~10–15 fps. Fine for an ambient aquarium. If your panel is
  stable at a higher `SPI_FREQUENCY`, raise it in `User_Setup_marauder_pancake.h`.
- If the render surface can't fit even the smallest strip in internal RAM
  (heavy WiFi use), the splash shows `Sprite alloc failed`; disable Wi-Fi or
  lower `BACKGROUND_GRADIENT_H`.
