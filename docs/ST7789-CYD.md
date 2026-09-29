# ST7789 ESP32-2432S028R CYD support

This documents a tested ESP32-2432S028R Cheap Yellow Display variant that looks like the common CYD but uses an **ST7789** LCD controller instead of the usual ILI9341.

## Confirmed hardware

- Board: ESP32-2432S028R
- ESP32 module: ESP-WROOM-32
- Touch: XPT2046
- LCD marking: TPM408-2.8
- LCD sticker: `2.8 TFT`, `240(RGB)x320`, driver `7789`
- Native LCD geometry: 240x320

The stock ILI9341 firmware can boot on this board but produces mirrored/corrupted output. Using the ST7789 driver with the native panel geometry fixes the display.

## Generate the ST7789 board variant

From the repository root run:

```bash
python tools/make_st7789_variant.py
```

This creates:

```text
ASCII_Aquarium_CYD_ST7789/
  ASCII_Aquarium_CYD_ST7789.ino
  User_Setup_ST7789_CYD.h
```

The generator deliberately derives the variant from the current v2.39 sketch instead of maintaining a second 280 KB copy. That makes future upstream updates much easier to merge and causes the script to stop with an error if the expected board-specific code has changed.

The generated variant identifies itself as `CYD 2.8 ST7789` and uses landscape rotation **3** by default. The existing Flip Display option uses rotation **1**, preserving the same 180-degree flip behaviour as the normal CYD build.

## TFT_eSPI configuration

Copy `User_Setup_ST7789_CYD.h` from the generated folder (or the repository root) over `TFT_eSPI/User_Setup.h`.

Keep TFT_eSPI's stock `User_Setup_Select.h` and ensure its normal `#include <User_Setup.h>` selection is enabled.

The important settings are:

```cpp
#define ST7789_DRIVER
#define TFT_INVERSION_OFF
#define TFT_RGB_ORDER TFT_BGR
#define TFT_WIDTH  240
#define TFT_HEIGHT 320
```

`CGRAM_OFFSET` was **not** required on the tested panel.

## Rotation and touch

The tested ST7789 board needs both display and touch at rotation 3 in the normal landscape orientation. ASCII Aquarium already routes display and touch through the same `displayRotation()` helper, so the generated variant changes that helper from the ILI9341 rotation pair `1/3` to the ST7789 pair `3/1`.

That means no manual hunting for `tft.setRotation()` or `touch.setRotation()` calls is required.

## Build and test

1. Run `python tools/make_st7789_variant.py`.
2. Install/copy the generated `User_Setup_ST7789_CYD.h` as TFT_eSPI's `User_Setup.h`.
3. Open `ASCII_Aquarium_CYD_ST7789/ASCII_Aquarium_CYD_ST7789.ino` in Arduino IDE.
4. Compile and upload using the same ESP32 settings used for the standard CYD build.
5. Verify the aquarium fills the screen in 320x240 landscape orientation.
6. Tap several positions to confirm touch alignment and tap-to-feed.
7. Test **Flip Display** once to confirm the alternate rotation is also correct.

## Result on the tested board

ASCII Aquarium runs normally with correct colours, full-screen 320x240 landscape rendering, and correctly aligned tap-to-feed touch input.

## Notes

This configuration was hardware-tested on the panel described above. Other ST7789 CYD variants may use different colour order, inversion, offsets, or touch calibration.
