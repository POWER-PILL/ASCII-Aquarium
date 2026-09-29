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

## TFT_eSPI configuration

Copy `User_Setup_ST7789_CYD.h` from the repository root over `TFT_eSPI/User_Setup.h`.

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

## ASCII Aquarium rotation

The tested ST7789 board needs both display and touch set to rotation 3:

```cpp
tft.setRotation(3);
touch.setRotation(3);
```

The upstream CYD sketch currently uses rotation 1 for both. Changing both values to 3 produced the correct landscape display orientation and aligned touch coordinates on the tested ST7789 panel.

## Result

ASCII Aquarium runs normally with correct colours, full-screen 320x240 landscape rendering, and correctly aligned tap-to-feed touch input.

## Notes

This configuration was hardware-tested on the panel described above. Other ST7789 CYD variants may use different colour order, inversion, offsets, or touch calibration.
