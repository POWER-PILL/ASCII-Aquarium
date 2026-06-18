// CYD (ESP32-2432S028R / Cheap Yellow Display) setup for TFT_eSPI
// Copy this file to:
//   /Users/kertgartner/Documents/Arduino/libraries/TFT_eSPI/User_Setup.h
// Keep the stock User_Setup_Select.h (or restore backup) so TFT_eSPI can load
// its driver command definitions correctly.

#define USER_SETUP_INFO "ESP32 CYD ILI9341 320x240"
#define USER_SETUP_ID 2432

// Display driver
#define ILI9341_2_DRIVER
#define TFT_INVERSION_ON

// ESP32 TFT pin mapping (common ESP32-2432S028R board)
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS   15
#define TFT_DC   2
#define TFT_RST  4
#define TFT_BL   21
#define TFT_BACKLIGHT_ON HIGH

// Display geometry and speed
#define TFT_WIDTH  320
#define TFT_HEIGHT 240

#define SPI_FREQUENCY       40000000
//#define SPI_FREQUENCY       55000000
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
