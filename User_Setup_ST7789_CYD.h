// ESP32-2432S028R CYD variant with ST7789 2.8-inch TFT
// Tested panel marking: TPM408-2.8
// Panel sticker: 2.8 TFT, 240(RGB)x320, driver 7789
//
// Copy this file over TFT_eSPI/User_Setup.h before compiling.
// Keep the stock TFT_eSPI User_Setup_Select.h with #include <User_Setup.h> enabled.
//
// The tested ASCII Aquarium sketch also uses:
//   tft.setRotation(3);
//   touch.setRotation(3);

#define USER_SETUP_INFO "ESP32 CYD ST7789 240x320"
#define USER_SETUP_ID 2432

// Display driver
#define ST7789_DRIVER
#define TFT_INVERSION_OFF

// Tested colour order
#define TFT_RGB_ORDER TFT_BGR

// ESP32 TFT pin mapping (ESP32-2432S028R)
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  4
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH

// Native panel geometry. ASCII Aquarium rotates this to 320x240 landscape.
#define TFT_WIDTH  240
#define TFT_HEIGHT 320

#define SPI_FREQUENCY       40000000
#define SPI_READ_FREQUENCY  20000000
#define SPI_TOUCH_FREQUENCY 2500000

// Fonts
#define LOAD_GLCD
#define LOAD_FONT2
#define LOAD_FONT4
#define LOAD_FONT6
#define LOAD_FONT7
#define LOAD_FONT8
#define LOAD_GFXFF
#define SMOOTH_FONT
