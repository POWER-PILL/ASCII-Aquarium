// ASCII_Aquarium_Pancake.ino | Version: v2.41 (Marauder Pancake port)

// ─────────────────────────────────────────────────────────────────────────────
// Board profile. This file is the Marauder Pancake port:
//   ESP32-C5, ST7796 320x480 panel driven in 480x320 landscape, FT6336 cap touch,
//   SD sharing the FSPI bus with the TFT.
// Leave AQUARIUM_BOARD_PANCAKE defined for Pancake hardware. Comment it out to
// build the stock CYD/ST7796U resistive-touch profiles instead.
// ─────────────────────────────────────────────────────────────────────────────
#define AQUARIUM_BOARD_PANCAKE

// Optional manual edit notes: America/Winnipeg (Central Time) — use 12-hour style (e.g. 3:45 PM), not 24-hour.
#include <Arduino.h>
#include <cstring>
#include <FS.h>
#include <SPI.h>
#include <Preferences.h>
#include <SD.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#if !defined(AQUARIUM_BOARD_PANCAKE)
#include <XPT2046_Touchscreen.h>   // resistive touch (CYD / ST7796U); Pancake uses I2C FT6336
#else
#include "ft6336_touch.h"          // FT6336 capacitive touch over I2C
#include "ds3231_rtc.h"            // DS3231 RTC over the same I2C bus (optional)
#endif
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#include "esp_wifi.h"              // esp_wifi_set_band_mode() — C5 is dual-band 2.4/5 GHz
#endif
#include <time.h>

// The ESP32-C5 is dual-band (2.4 + 5 GHz). Keep both bands enabled and let the
// scan/connect logic pick the best AP. Combined-SSID connect reliability comes
// from targeting the exact BSSID + channel the scan found (see startWifiConnectTo),
// not from disabling a band. Set _2G_ONLY or _5G_ONLY here to force a band.
#if defined(CONFIG_IDF_TARGET_ESP32C5)
#define AQUARIUM_WIFI_BAND_MODE WIFI_BAND_MODE_AUTO
// Regulatory domain for association. ieee80211d makes the STA follow the AP's
// advertised country, so this is mostly a starting point — set it to your
// region if you like ("CA", "US", "GB", ...).
#define AQUARIUM_WIFI_COUNTRY "US"
#endif

/*
  Desktop ASCII Aquarium — Marauder Pancake port
  MCU:     ESP32-C5 (needs the ESP32-C5 fork of TFT_eSPI — stock TFT_eSPI has no C5)
  Display: ST7796 320x480 panel, run in 480x320 landscape via TFT_eSPI rotation 1
  Touch:   FT6336U capacitive over I2C (see ft6336_touch.h)

  Pancake pin reference (shared FSPI bus for TFT + SD):
    TFT_MISO = 4      TFT_MOSI = 24     TFT_SCLK = 23
    TFT_CS   = 5      TFT_DC   = 3      TFT_RST  = 2      TFT_BL = 26
    SD_CS    = 7      (SD shares MISO/MOSI/SCLK with the TFT)

    Touch (FT6336, I2C): SDA = 9   SCL = 10   RST = 8   addr 0x38

  NOTE:
  Display/SPI pins are configured in the TFT_eSPI User_Setup (ST7796, ESP32-C5).
  The pins above must match that setup. See README_Pancake.md for build steps.
*/

/** Shown run label in HUD title (must match line-1 banner when you release builds). */
static constexpr const char* kSketchVersionLabel = "v2.41";

#if defined(AQUARIUM_BOARD_PANCAKE)
static constexpr const char* kBoardProfileName = "Marauder Pancake";
#elif defined(AQUARIUM_BOARD_ST7796U35)
static constexpr const char* kBoardProfileName = "ST7796U 3.5";
#else
static constexpr const char* kBoardProfileName = "CYD 2.8";
#endif

// ------------------------------ Board Touch Pins -----------------------------
// Pancake: FT6336 is I2C-only (SDA=9, SCL=10, RST=8) — see ft6336_touch.h.
// These SPI touch pins are unused on Pancake but kept defined for shared code.
static const int TOUCH_CS_PIN = 33;
static const int TOUCH_IRQ_PIN = 36;
#if defined(AQUARIUM_BOARD_ST7796U35)
static const int TOUCH_CLK_PIN = 14;
static const int TOUCH_MISO_PIN = 12;
static const int TOUCH_MOSI_PIN = 13;
#else
static const int TOUCH_CLK_PIN = 25;
static const int TOUCH_MISO_PIN = 39;
static const int TOUCH_MOSI_PIN = 32;
#endif

// Touch calibration (adjust if needed for your panel)
static const int TOUCH_RAW_MIN_X = 250;
static const int TOUCH_RAW_MAX_X = 3850;
static const int TOUCH_RAW_MIN_Y = 220;
static const int TOUCH_RAW_MAX_Y = 3850;

// ------------------------------ Button / SD Capture --------------------------
#if defined(AQUARIUM_BOARD_PANCAKE)
static const int BOOT_BUTTON_PIN = -1;   // no usable BOOT button wired for captures; use the on-screen capture panel
#else
static const int BOOT_BUTTON_PIN = 0;
#endif
static const unsigned long BOOT_BUTTON_DEBOUNCE_MS = 180UL;
#if defined(AQUARIUM_BOARD_PANCAKE)
static const int TFT_BACKLIGHT_PIN = 26;  // Pancake TFT_BL (matches TFT_eSPI User_Setup)
static const int AMBIENT_LED_RED_PIN = -1; // no RGB ambient LEDs exposed on Pancake
static const int AMBIENT_LED_GREEN_PIN = -1;
static const int AMBIENT_LED_BLUE_PIN = -1;
#elif defined(AQUARIUM_BOARD_ST7796U35)
static const int TFT_BACKLIGHT_PIN = 27;
static const int AMBIENT_LED_RED_PIN = -1;
static const int AMBIENT_LED_GREEN_PIN = -1;
static const int AMBIENT_LED_BLUE_PIN = -1;
#else
static const int TFT_BACKLIGHT_PIN = 21;
static const int AMBIENT_LED_RED_PIN = 4;
static const int AMBIENT_LED_GREEN_PIN = 16;
static const int AMBIENT_LED_BLUE_PIN = 17;
#endif
static const bool AMBIENT_LED_ACTIVE_LOW = true;
static const uint32_t BACKLIGHT_PWM_FREQ = 12000;
static const uint8_t BACKLIGHT_PWM_BITS = 8;

// Common display and microSD wiring. If your board revision differs, adjust these only.
#if defined(AQUARIUM_BOARD_PANCAKE)
// Pancake (ESP32-C5): TFT + SD share one FSPI bus. Pins must match the TFT_eSPI
// User_Setup (MISO=4, MOSI=24, SCLK=23, TFT_CS=5); SD has its own CS on GPIO7.
static const int AQUARIUM_TFT_CS_PIN = 5;
static const int AQUARIUM_TFT_SCK_PIN = 23;
static const int AQUARIUM_TFT_MISO_PIN = 4;
static const int AQUARIUM_TFT_MOSI_PIN = 24;
static const int SD_CS_PIN = 7;
static const int SD_SCK_PIN = 23;
static const int SD_MISO_PIN = 4;
static const int SD_MOSI_PIN = 24;
static const uint32_t SD_SPI_FREQUENCY = 20000000UL;
#else
static const int AQUARIUM_TFT_CS_PIN = 15;
static const int AQUARIUM_TFT_SCK_PIN = 14;
static const int AQUARIUM_TFT_MISO_PIN = 12;
static const int AQUARIUM_TFT_MOSI_PIN = 13;
static const int SD_CS_PIN = 5;
static const int SD_SCK_PIN = 18;
static const int SD_MISO_PIN = 19;
static const int SD_MOSI_PIN = 23;
static const uint32_t SD_SPI_FREQUENCY = 1000000UL;
#endif
#if defined(SPI_FREQUENCY)
static const uint32_t TFT_SPI_FREQUENCY = SPI_FREQUENCY;
#else
static const uint32_t TFT_SPI_FREQUENCY = 40000000UL;
#endif
static constexpr const char* CAPTURE_DIR = "/AQCAP";
static constexpr const char* SEQUENCE_DIR = "/AQSEQ";
static const unsigned long CAPTURE_RECORD_FRAME_MS = 50UL;  // 20 fps virtual time while recording.

// ------------------------------ Display Geometry -----------------------------
#if defined(AQUARIUM_BOARD_PANCAKE)
// Pancake ST7796 driven in landscape: the aquarium fills the whole 480x320 panel.
static const int PHYSICAL_SCREEN_W = 480;
static const int PHYSICAL_SCREEN_H = 320;
static const int SCREEN_W = 480;
static const int SCREEN_H = 320;
#elif defined(AQUARIUM_BOARD_ST7796U35)
static const int PHYSICAL_SCREEN_W = 480;
static const int PHYSICAL_SCREEN_H = 320;
static const int SCREEN_W = 320;
static const int SCREEN_H = 240;
#else
static const int PHYSICAL_SCREEN_W = 320;
static const int PHYSICAL_SCREEN_H = 240;
static const int SCREEN_W = 320;
static const int SCREEN_H = 240;
#endif
// Horizontal shift used to re-centre the 320-wide UI panels on a wider screen.
// Every settings/wifi/clock panel is centred on x=160 in the stock layout, so a
// uniform shift keeps them centred (0 on the 320-wide boards).
static const int UI_X_SHIFT = (SCREEN_W - 320) / 2;
static const uint16_t BG_COLOR = TFT_BLACK;  // #000000
static const int SEA_LEVEL_Y = SCREEN_H - 36;
static const int BACKGROUND_GRADIENT_H = (SCREEN_H >= 240) ? 80 : (SCREEN_H / 3);
static const int BACKGROUND_DITHER_AMPLITUDE = 18;
static const int MAIN_RENDER_SURFACE_H = SCREEN_H;
static const int MAIN_SPRITE_COLOR_DEPTH = 16;
static const int CAPTURE_RENDER_STRIP_H = 20;
static const int STRIP_RENDER_SURFACE_H_1 = 160;
static const int STRIP_RENDER_SURFACE_H_2 = 120;
static const int STRIP_RENDER_SURFACE_H_3 = 96;
static const int STRIP_RENDER_SURFACE_H_4 = 80;
static const int STRIP_RENDER_SURFACE_H_5 = 64;
static const int DISPLAY_OFFSET_X = (PHYSICAL_SCREEN_W - SCREEN_W) / 2;
static const int DISPLAY_OFFSET_Y = (PHYSICAL_SCREEN_H - SCREEN_H) / 2;

// ------------------------------ Settings UI ----------------------------------
static const int SETTINGS_PANEL_X = 40 + UI_X_SHIFT;
static const int SETTINGS_PANEL_Y = 24;
static const int SETTINGS_PANEL_W = 240;
static const int SETTINGS_PANEL_H = 208;
static const int SETTINGS_CLOSE_X = SETTINGS_PANEL_X + SETTINGS_PANEL_W - 34;
static const int SETTINGS_CLOSE_Y = SETTINGS_PANEL_Y + 8;
static const int SETTINGS_CLOSE_W = 24;
static const int SETTINGS_CLOSE_H = 24;
static const int SETTINGS_VALUE_RIGHT_X = SETTINGS_PANEL_X + 156;
static const int SETTINGS_MINUS_X = SETTINGS_PANEL_X + 166;
static const int SETTINGS_PLUS_X = SETTINGS_PANEL_X + 202;
static const int SETTINGS_BUTTON_W = 30;
static const int SETTINGS_BUTTON_H = 24;
static const int SETTINGS_ACTION_X = SETTINGS_PANEL_X + 134;
static const int SETTINGS_ACTION_W = 98;
static const int SETTINGS_ROW_START_Y = SETTINGS_PANEL_Y + 36;
static const int SETTINGS_ROW_GAP = 34;
static const int SETTINGS_TAB_Y = SETTINGS_PANEL_Y + SETTINGS_PANEL_H - 30;
static const int SETTINGS_TAB_H = 22;
static const int SETTINGS_TAB_GAP = 4;
static const int SETTINGS_TANK_TAB_X = SETTINGS_PANEL_X + 5;
static const int SETTINGS_TANK_TAB_W = 48;
static const int SETTINGS_SEAWEED_TAB_X = SETTINGS_TANK_TAB_X + SETTINGS_TANK_TAB_W + SETTINGS_TAB_GAP;
static const int SETTINGS_SEAWEED_TAB_W = 62;
static const int SETTINGS_CLOCK_TAB_X = SETTINGS_SEAWEED_TAB_X + SETTINGS_SEAWEED_TAB_W + SETTINGS_TAB_GAP;
static const int SETTINGS_CLOCK_TAB_W = 50;
static const int SETTINGS_BACKGROUND_TAB_X = SETTINGS_CLOCK_TAB_X + SETTINGS_CLOCK_TAB_W + SETTINGS_TAB_GAP;
static const int SETTINGS_BACKGROUND_TAB_W = 58;
static const int EVENTS_PANEL_X = 36 + UI_X_SHIFT;
static const int EVENTS_PANEL_Y = 46;
static const int EVENTS_PANEL_W = 248;
static const int EVENTS_PANEL_H = 154;
static const int EVENTS_CLOSE_X = EVENTS_PANEL_X + EVENTS_PANEL_W - 32;
static const int EVENTS_CLOSE_Y = EVENTS_PANEL_Y + 8;
static const int EVENTS_CLOSE_W = 24;
static const int EVENTS_CLOSE_H = 22;
static const int EVENTS_ROW_START_Y = EVENTS_PANEL_Y + 40;
static const int EVENTS_ROW_GAP = 31;
static const int EVENTS_LABEL_X = EVENTS_PANEL_X + 14;
static const int EVENTS_VALUE_RIGHT_X = EVENTS_PANEL_X + 154;
static const int EVENTS_MINUS_X = EVENTS_PANEL_X + 166;
static const int EVENTS_PLUS_X = EVENTS_PANEL_X + 202;
static const int EVENTS_BUTTON_W = 30;
static const int EVENTS_BUTTON_H = 24;
static const int EVENTS_ACTION_X = EVENTS_PANEL_X + 142;
static const int EVENTS_ACTION_W = 90;
static const int CREATURES_PANEL_X = 30 + UI_X_SHIFT;
static const int CREATURES_PANEL_Y = 28;
static const int CREATURES_PANEL_W = 260;
static const int CREATURES_PANEL_H = 190;
static const int CREATURES_CLOSE_X = CREATURES_PANEL_X + CREATURES_PANEL_W - 32;
static const int CREATURES_CLOSE_Y = CREATURES_PANEL_Y + 8;
static const int CREATURES_CLOSE_W = 24;
static const int CREATURES_CLOSE_H = 22;
static const int CREATURES_ROW_START_Y = CREATURES_PANEL_Y + 42;
static const int CREATURES_ROW_GAP = 29;
static const int CREATURES_LABEL_X = CREATURES_PANEL_X + 14;
static const int CREATURES_VALUE_RIGHT_X = CREATURES_PANEL_X + 162;
static const int CREATURES_MINUS_X = CREATURES_PANEL_X + 174;
static const int CREATURES_PLUS_X = CREATURES_PANEL_X + 214;
static const int CREATURES_BUTTON_W = 30;
static const int CREATURES_BUTTON_H = 24;
static const int BACKLIGHT_PANEL_X = 40 + UI_X_SHIFT;
static const int BACKLIGHT_PANEL_Y = 30;
static const int BACKLIGHT_PANEL_W = 240;
static const int BACKLIGHT_PANEL_H = 198;
static const int BACKLIGHT_CLOSE_X = BACKLIGHT_PANEL_X + BACKLIGHT_PANEL_W - 34;
static const int BACKLIGHT_CLOSE_Y = BACKLIGHT_PANEL_Y + 8;
static const int BACKLIGHT_CLOSE_W = 24;
static const int BACKLIGHT_CLOSE_H = 24;
static const int BACKLIGHT_ROW_START_Y = BACKLIGHT_PANEL_Y + 36;
static const int BACKLIGHT_ROW_GAP = 30;
static const int BACKLIGHT_LABEL_X = BACKLIGHT_PANEL_X + 12;
static const int BACKLIGHT_VALUE_RIGHT_X = BACKLIGHT_PANEL_X + 156;
static const int BACKLIGHT_MINUS_X = BACKLIGHT_PANEL_X + 166;
static const int BACKLIGHT_PLUS_X = BACKLIGHT_PANEL_X + 202;
static const int BACKLIGHT_BUTTON_W = 30;
static const int BACKLIGHT_BUTTON_H = 24;
static const int BACKLIGHT_ACTION_X = BACKLIGHT_PANEL_X + 134;
static const int BACKLIGHT_ACTION_W = 98;
static const int BACKLIGHT_COLOR_SWATCH_X = BACKLIGHT_PANEL_X + 96;
static const int BACKLIGHT_COLOR_SWATCH_W = 28;
static const int BACKLIGHT_BOTTOM_BUTTON_Y = BACKLIGHT_PANEL_Y + BACKLIGHT_PANEL_H - 34;
static const int BACKLIGHT_COLOUR_MODE_X = BACKLIGHT_PANEL_X + 14;
static const int BACKLIGHT_SCHEDULE_X = BACKLIGHT_PANEL_X + 128;
static const int BACKLIGHT_BOTTOM_BUTTON_W = 98;
static const int LIGHT_COLOR_PANEL_X = BACKLIGHT_PANEL_X;
static const int LIGHT_COLOR_PANEL_Y = 54;
static const int LIGHT_COLOR_PANEL_W = BACKLIGHT_PANEL_W;
static const int LIGHT_COLOR_PANEL_H = 122;
static const int LIGHT_COLOR_CLOSE_X = LIGHT_COLOR_PANEL_X + LIGHT_COLOR_PANEL_W - 34;
static const int LIGHT_COLOR_CLOSE_Y = LIGHT_COLOR_PANEL_Y + 8;
static const int LIGHT_COLOR_CLOSE_W = 24;
static const int LIGHT_COLOR_CLOSE_H = 24;
static const int LIGHT_COLOR_ROW_START_Y = LIGHT_COLOR_PANEL_Y + 40;
static const int LIGHT_COLOR_ROW_GAP = 34;
static const int LIGHT_SCHEDULE_PANEL_X = 28 + UI_X_SHIFT;
static const int LIGHT_SCHEDULE_PANEL_Y = 24;
static const int LIGHT_SCHEDULE_PANEL_W = 264;
static const int LIGHT_SCHEDULE_PANEL_H = 208;
static const int LIGHT_SCHEDULE_CLOSE_X = LIGHT_SCHEDULE_PANEL_X + LIGHT_SCHEDULE_PANEL_W - 34;
static const int LIGHT_SCHEDULE_CLOSE_Y = LIGHT_SCHEDULE_PANEL_Y + 8;
static const int LIGHT_SCHEDULE_CLOSE_W = 24;
static const int LIGHT_SCHEDULE_CLOSE_H = 24;
static const int LIGHT_SCHEDULE_ROW_START_Y = LIGHT_SCHEDULE_PANEL_Y + 38;
static const int LIGHT_SCHEDULE_ROW_GAP = 31;
static const int LIGHT_SCHEDULE_LABEL_X = LIGHT_SCHEDULE_PANEL_X + 12;
static const int LIGHT_SCHEDULE_VALUE_RIGHT_X = LIGHT_SCHEDULE_PANEL_X + 154;
static const int LIGHT_SCHEDULE_MINUS_X = LIGHT_SCHEDULE_PANEL_X + 166;
static const int LIGHT_SCHEDULE_PLUS_X = LIGHT_SCHEDULE_PANEL_X + 202;
static const int LIGHT_SCHEDULE_BUTTON_W = 30;
static const int LIGHT_SCHEDULE_BUTTON_H = 24;
static const int CLOCK_ROW_1_Y = SETTINGS_PANEL_Y + 36;
static const int CLOCK_ROW_2_Y = SETTINGS_PANEL_Y + 60;
static const int CLOCK_ROW_3_Y = SETTINGS_PANEL_Y + 84;
static const int CLOCK_ROW_4_Y = SETTINGS_PANEL_Y + 108;
static const int CLOCK_ROW_5_Y = SETTINGS_PANEL_Y + 132;
static const int CLOCK_FIELD_NAV_Y = SETTINGS_PANEL_Y + 157;
static const int CLOCK_FIELD_PREV_X = SETTINGS_PANEL_X + 12;
static const int CLOCK_FIELD_NEXT_X = SETTINGS_PANEL_X + 76;
static const int CLOCK_FIELD_NAV_W = 56;
static const int CLOCK_FIELD_NAV_H = 18;
static const int CLOCK_STYLE_BUTTON_X = SETTINGS_PANEL_X + 92;
static const int CLOCK_STYLE_BUTTON_W = 66;
static const int CLOCK_STYLE_PANEL_X = 46 + UI_X_SHIFT;
static const int CLOCK_STYLE_PANEL_Y = 54;
static const int CLOCK_STYLE_PANEL_W = 228;
static const int CLOCK_STYLE_PANEL_H = 176;
static const int CLOCK_STYLE_CLOSE_X = CLOCK_STYLE_PANEL_X + CLOCK_STYLE_PANEL_W - 32;
static const int CLOCK_STYLE_CLOSE_Y = CLOCK_STYLE_PANEL_Y + 8;
static const int CLOCK_STYLE_CLOSE_W = 24;
static const int CLOCK_STYLE_CLOSE_H = 22;
static const int CLOCK_STYLE_ROW_1_Y = CLOCK_STYLE_PANEL_Y + 40;
static const int CLOCK_STYLE_ROW_2_Y = CLOCK_STYLE_PANEL_Y + 74;
static const int CLOCK_STYLE_ROW_3_Y = CLOCK_STYLE_PANEL_Y + 104;
static const int CLOCK_STYLE_ROW_4_Y = CLOCK_STYLE_PANEL_Y + 134;
static const int CLOCK_STYLE_LABEL_X = CLOCK_STYLE_PANEL_X + 14;
static const int CLOCK_STYLE_LEFT_X = CLOCK_STYLE_PANEL_X + 104;
static const int CLOCK_STYLE_RIGHT_X = CLOCK_STYLE_PANEL_X + 164;
static const int CLOCK_STYLE_CHOICE_W = 54;
static const int CLOCK_STYLE_SWATCH_X = CLOCK_STYLE_LEFT_X;
static const int CLOCK_STYLE_SWATCH_W = 28;
static const int CLOCK_STYLE_COLOR_BUTTON_X = CLOCK_STYLE_RIGHT_X;
static const int CLOCK_STYLE_COLOR_BUTTON_W = 54;
static const int CLOCK_STYLE_FONT_VALUE_RIGHT_X = CLOCK_STYLE_PANEL_X + 150;
static const int CLOCK_STYLE_FONT_MINUS_X = CLOCK_STYLE_PANEL_X + 156;
static const int CLOCK_STYLE_FONT_PLUS_X = CLOCK_STYLE_PANEL_X + 192;
static const int CLOCK_STYLE_FONT_BUTTON_W = 28;
static const int CLOCK_COLOR_PANEL_X = 20 + UI_X_SHIFT;
static const int CLOCK_COLOR_PANEL_Y = 38;
static const int CLOCK_COLOR_PANEL_W = 280;
static const int CLOCK_COLOR_PANEL_H = 164;
static const int CLOCK_COLOR_CLOSE_X = CLOCK_COLOR_PANEL_X + CLOCK_COLOR_PANEL_W - 32;
static const int CLOCK_COLOR_CLOSE_Y = CLOCK_COLOR_PANEL_Y + 8;
static const int CLOCK_COLOR_CLOSE_W = 24;
static const int CLOCK_COLOR_CLOSE_H = 22;
static const int CLOCK_COLOR_GRID_X = CLOCK_COLOR_PANEL_X + 14;
static const int CLOCK_COLOR_GRID_Y = CLOCK_COLOR_PANEL_Y + 46;
static const int CLOCK_COLOR_SWATCH_W = 26;
static const int CLOCK_COLOR_SWATCH_H = 22;
static const int CLOCK_COLOR_SWATCH_GAP_X = 7;
static const int CLOCK_COLOR_SWATCH_GAP_Y = 10;
static const int CLOCK_COLOR_SWATCH_COLS = 8;
static const int BACKGROUND_COLOR_SWATCH_X = SETTINGS_PANEL_X + 96;
static const int BACKGROUND_COLOR_SWATCH_W = 28;
static const int BACKGROUND_COLOR_BUTTON_X = SETTINGS_ACTION_X;
static const int BACKGROUND_COLOR_BUTTON_W = SETTINGS_ACTION_W;
static const int BACKGROUND_COLOR_PANEL_X = CLOCK_COLOR_PANEL_X;
static const int BACKGROUND_COLOR_PANEL_Y = CLOCK_COLOR_PANEL_Y;
static const int BACKGROUND_COLOR_PANEL_W = CLOCK_COLOR_PANEL_W;
static const int BACKGROUND_COLOR_PANEL_H = CLOCK_COLOR_PANEL_H;
static const int BACKGROUND_COLOR_CLOSE_X = CLOCK_COLOR_CLOSE_X;
static const int BACKGROUND_COLOR_CLOSE_Y = CLOCK_COLOR_CLOSE_Y;
static const int BACKGROUND_COLOR_CLOSE_W = CLOCK_COLOR_CLOSE_W;
static const int BACKGROUND_COLOR_CLOSE_H = CLOCK_COLOR_CLOSE_H;
static const int BACKGROUND_COLOR_GRID_X = CLOCK_COLOR_GRID_X;
static const int BACKGROUND_COLOR_GRID_Y = CLOCK_COLOR_GRID_Y;
static const int BACKGROUND_COLOR_SWATCH_GRID_W = CLOCK_COLOR_SWATCH_W;
static const int BACKGROUND_COLOR_SWATCH_GRID_H = CLOCK_COLOR_SWATCH_H;
static const int BACKGROUND_COLOR_SWATCH_GAP_X = CLOCK_COLOR_SWATCH_GAP_X;
static const int BACKGROUND_COLOR_SWATCH_GAP_Y = CLOCK_COLOR_SWATCH_GAP_Y;
static const int BACKGROUND_COLOR_SWATCH_COLS = CLOCK_COLOR_SWATCH_COLS;
static const int AUTO_SKY_PICK_X = SETTINGS_PANEL_X + 116;
static const int AUTO_SKY_PICK_W = 42;
static const int AUTO_SKY_PANEL_X = 34 + UI_X_SHIFT;
static const int AUTO_SKY_PANEL_Y = 44;
static const int AUTO_SKY_PANEL_W = 252;
static const int AUTO_SKY_PANEL_H = 152;
static const int AUTO_SKY_CLOSE_X = AUTO_SKY_PANEL_X + AUTO_SKY_PANEL_W - 32;
static const int AUTO_SKY_CLOSE_Y = AUTO_SKY_PANEL_Y + 8;
static const int AUTO_SKY_CLOSE_W = 24;
static const int AUTO_SKY_CLOSE_H = 22;
static const int AUTO_SKY_BUTTON_W = 96;
static const int AUTO_SKY_BUTTON_H = 28;
static const int AUTO_SKY_BUTTON_LEFT_X = AUTO_SKY_PANEL_X + 22;
static const int AUTO_SKY_BUTTON_RIGHT_X = AUTO_SKY_PANEL_X + 134;
static const int AUTO_SKY_BUTTON_TOP_Y = AUTO_SKY_PANEL_Y + 58;
static const int AUTO_SKY_BUTTON_BOTTOM_Y = AUTO_SKY_PANEL_Y + 94;
static const int AMBIENT_COLOR_PANEL_X = CLOCK_COLOR_PANEL_X;
static const int AMBIENT_COLOR_PANEL_Y = CLOCK_COLOR_PANEL_Y;
static const int AMBIENT_COLOR_PANEL_W = CLOCK_COLOR_PANEL_W;
static const int AMBIENT_COLOR_PANEL_H = CLOCK_COLOR_PANEL_H;
static const int AMBIENT_COLOR_CLOSE_X = CLOCK_COLOR_CLOSE_X;
static const int AMBIENT_COLOR_CLOSE_Y = CLOCK_COLOR_CLOSE_Y;
static const int AMBIENT_COLOR_CLOSE_W = CLOCK_COLOR_CLOSE_W;
static const int AMBIENT_COLOR_CLOSE_H = CLOCK_COLOR_CLOSE_H;
static const int AMBIENT_COLOR_GRID_X = CLOCK_COLOR_GRID_X;
static const int AMBIENT_COLOR_GRID_Y = CLOCK_COLOR_GRID_Y;
static const int AMBIENT_COLOR_SWATCH_W = CLOCK_COLOR_SWATCH_W;
static const int AMBIENT_COLOR_SWATCH_H = CLOCK_COLOR_SWATCH_H;
static const int AMBIENT_COLOR_SWATCH_GAP_X = CLOCK_COLOR_SWATCH_GAP_X;
static const int AMBIENT_COLOR_SWATCH_GAP_Y = CLOCK_COLOR_SWATCH_GAP_Y;
static const int AMBIENT_COLOR_SWATCH_COLS = CLOCK_COLOR_SWATCH_COLS;
static const int ASCII_CLOCK_ROWS = 11;
static const int ASCII_CLOCK_GLYPH_GAP = 1;
static const int ASCII_CLOCK_CHAR_W = 6;
static const int ASCII_CLOCK_ROW_H = 10;
static const int ASCII_CLOCK_Y = 82;
static const int CLOCK_FLIP_SPRITE_W = 192;
static const int CLOCK_FLIP_SPRITE_H = 20;
static const uint16_t CLOCK_FLIP_TRANSPARENT = 0x0801;  // Dark magenta key colour, outside the clock palette.

static const int CORNER_BUTTON_W = 28;
static const int CORNER_BUTTON_H = 20;
static const int CORNER_BUTTON_Y = 3;
static const int DEBUG_BUTTON_X = 7;
static const int BACKLIGHT_BUTTON_X = DEBUG_BUTTON_X + CORNER_BUTTON_W + 4;
static const int WIFI_BUTTON_X = SCREEN_W - 70;
static const int SETTINGS_CORNER_BUTTON_X = SCREEN_W - 35;
static const int RESPAWN_BUTTON_Y = CORNER_BUTTON_Y + CORNER_BUTTON_H + 4;
static const int SEAHORSE_TEST_BUTTON_X = SETTINGS_CORNER_BUTTON_X;
static const int SEAHORSE_TEST_BUTTON_Y = RESPAWN_BUTTON_Y + CORNER_BUTTON_H + 4;
static const int OCTOPUS_TEST_BUTTON_X = SETTINGS_CORNER_BUTTON_X;
static const int OCTOPUS_TEST_BUTTON_Y = SEAHORSE_TEST_BUTTON_Y + CORNER_BUTTON_H + 4;
static const int SNAIL_TEST_BUTTON_X = SETTINGS_CORNER_BUTTON_X;
static const int SNAIL_TEST_BUTTON_Y = OCTOPUS_TEST_BUTTON_Y + CORNER_BUTTON_H + 4;
static const int CTHULHU_TEST_BUTTON_X = SETTINGS_CORNER_BUTTON_X;
static const int CTHULHU_TEST_BUTTON_Y = SNAIL_TEST_BUTTON_Y + CORNER_BUTTON_H + 4;
static const int JELLYFISH_TEST_BUTTON_X = SETTINGS_CORNER_BUTTON_X;
static const int JELLYFISH_TEST_BUTTON_Y = CTHULHU_TEST_BUTTON_Y + CORNER_BUTTON_H + 4;
static const int SQUID_TEST_BUTTON_X = SETTINGS_CORNER_BUTTON_X;
static const int SQUID_TEST_BUTTON_Y = JELLYFISH_TEST_BUTTON_Y + CORNER_BUTTON_H + 4;

static const int WIFI_PANEL_X = 8 + UI_X_SHIFT;
static const int WIFI_PANEL_Y = 28;
static const int WIFI_PANEL_W = 304;
static const int WIFI_PANEL_H = 208;
static const int WIFI_CLOSE_X = WIFI_PANEL_X + WIFI_PANEL_W - 32;
static const int WIFI_CLOSE_Y = WIFI_PANEL_Y + 8;
static const int WIFI_CLOSE_W = 24;
static const int WIFI_CLOSE_H = 22;
static const int WIFI_ROW_START_Y = WIFI_PANEL_Y + 42;
static const int WIFI_ROW_GAP = 31;
static const int WIFI_ROW_H = 22;
static const int WIFI_LABEL_X = WIFI_PANEL_X + 14;
static const int WIFI_VALUE_RIGHT_X = WIFI_PANEL_X + 198;
static const int WIFI_OFF_X = WIFI_PANEL_X + 214;
static const int WIFI_ON_X = WIFI_PANEL_X + 260;
static const int WIFI_TOGGLE_W = 38;
static const int WIFI_ACTION_X = WIFI_PANEL_X + 228;
static const int WIFI_ACTION_W = 70;
static const int WIFI_LIST_ROW_COUNT = 5;
static const int WIFI_LIST_ROW_Y = WIFI_PANEL_Y + 56;
static const int WIFI_LIST_ROW_H = 23;
static const int WIFI_KEY_START_Y = WIFI_PANEL_Y + 68;
static const int WIFI_KEY_H = 18;
static const int WIFI_KEY_GAP = 3;
static const int WIFI_KEYBOARD_SPECIAL_Y = WIFI_PANEL_Y + 153;
static const int WIFI_KEYBOARD_ACTION_Y = WIFI_PANEL_Y + 179;

static const int CAPTURE_BUTTON_X = SCREEN_W - 105;
static const int CAPTURE_PANEL_X = 36 + UI_X_SHIFT;
static const int CAPTURE_PANEL_Y = 46;
static const int CAPTURE_PANEL_W = 248;
static const int CAPTURE_PANEL_H = 154;
static const int CAPTURE_CLOSE_X = CAPTURE_PANEL_X + CAPTURE_PANEL_W - 32;
static const int CAPTURE_CLOSE_Y = CAPTURE_PANEL_Y + 8;
static const int CAPTURE_CLOSE_W = 24;
static const int CAPTURE_CLOSE_H = 22;
static const int CAPTURE_ROW_START_Y = CAPTURE_PANEL_Y + 40;
static const int CAPTURE_ROW_GAP = 28;
static const int CAPTURE_ROW_H = 22;
static const int CAPTURE_LABEL_X = CAPTURE_PANEL_X + 14;
static const int CAPTURE_VALUE_RIGHT_X = CAPTURE_PANEL_X + 154;
static const int CAPTURE_ACTION_X = CAPTURE_PANEL_X + 162;
static const int CAPTURE_ACTION_W = 70;

enum SettingsTab {
  SETTINGS_TAB_TANK,
  SETTINGS_TAB_SEAWEED,
  SETTINGS_TAB_CLOCK,
  SETTINGS_TAB_BACKGROUND
};

enum AutoSkySlot {
  AUTO_SKY_SUNRISE,
  AUTO_SKY_DAY,
  AUTO_SKY_SUNSET,
  AUTO_SKY_NIGHT,
  AUTO_SKY_SLOT_COUNT,
  AUTO_SKY_SLOT_NONE = 255
};

enum ClockField {
  CLOCK_FIELD_YEAR,
  CLOCK_FIELD_MONTH,
  CLOCK_FIELD_DAY,
  CLOCK_FIELD_HOUR,
  CLOCK_FIELD_MINUTE,
  CLOCK_FIELD_COUNT
};

enum ClockDisplayStyle {
  CLOCK_STYLE_SMALL_TEXT,
  CLOCK_STYLE_ASCII,
  CLOCK_STYLE_COUNT
};

enum ClockSmallPosition {
  CLOCK_SMALL_TOP,
  CLOCK_SMALL_BOTTOM,
  CLOCK_SMALL_POSITION_COUNT
};

enum WifiPanelMode {
  WIFI_PANEL_MAIN,
  WIFI_PANEL_NETWORKS,
  WIFI_PANEL_PASSWORD
};

enum KeyboardMode {
  KEYBOARD_LOWER,
  KEYBOARD_UPPER,
  KEYBOARD_SYMBOLS
};

enum LightScheduleDimmingMode {
  LIGHT_DIM_NONE,
  LIGHT_DIM_LCD,
  LIGHT_DIM_AMBIENT,
  LIGHT_DIM_BOTH,
  LIGHT_DIM_COUNT
};

// ------------------------------ Aquarium Controls ----------------------------
static const int MIN_FISH = 6;
static const int MAX_FISH = 36;
static const int DEFAULT_FISH = 16;

static const int MIN_BUBBLES = 0;
static const int MAX_BUBBLES = 50;
static const int DEFAULT_BUBBLES = 10;

static const int DEFAULT_OCTOPUS_FREQUENCY = 12;
static const int OCTOPUS_FREQUENCY_OPTIONS[] = {1, 2, 4, 6, 12, 60};
static constexpr int OCTOPUS_FREQUENCY_OPTION_COUNT =
    sizeof(OCTOPUS_FREQUENCY_OPTIONS) / sizeof(OCTOPUS_FREQUENCY_OPTIONS[0]);
static const int DEFAULT_SEAHORSE_FREQUENCY = 2;
static const int SEAHORSE_FREQUENCY_OPTIONS[] = {1, 2, 4, 6, 12, 60};
static constexpr int SEAHORSE_FREQUENCY_OPTION_COUNT =
    sizeof(SEAHORSE_FREQUENCY_OPTIONS) / sizeof(SEAHORSE_FREQUENCY_OPTIONS[0]);
static const int DEFAULT_AUTO_FEED_FREQUENCY = 0;
static const int AUTO_FEED_FREQUENCY_OPTIONS[] = {0, 1, 2, 4, 6, 12, 60};
static constexpr int AUTO_FEED_FREQUENCY_OPTION_COUNT =
    sizeof(AUTO_FEED_FREQUENCY_OPTIONS) / sizeof(AUTO_FEED_FREQUENCY_OPTIONS[0]);
static const int DEFAULT_SNAIL_FREQUENCY = 2;
static const int DEFAULT_JELLYFISH_FREQUENCY = 1;
static const int CREATURE_FREQUENCY_OPTIONS[] = {0, 1, 2, 4, 6, 12, 60};
static constexpr int CREATURE_FREQUENCY_OPTION_COUNT =
    sizeof(CREATURE_FREQUENCY_OPTIONS) / sizeof(CREATURE_FREQUENCY_OPTIONS[0]);
static const int AUTO_FEED_SPRINKLE_COUNT = 9;
static const unsigned long AUTO_FEED_SPRINKLE_MIN_MS = 1200UL;
static const unsigned long AUTO_FEED_SPRINKLE_MAX_MS = 2800UL;
static const unsigned long AUTO_FEED_FIRST_DROP_MS = 2000UL;

static const int MIN_LCD_BACKLIGHT_BRIGHTNESS = 10;
static const int MAX_LCD_BACKLIGHT_BRIGHTNESS = 100;
static const int DEFAULT_LCD_BACKLIGHT_BRIGHTNESS = 100;
static const int LCD_BACKLIGHT_BRIGHTNESS_STEP = 5;
static const int MIN_AMBIENT_BRIGHTNESS = 5;
static const int MAX_AMBIENT_BRIGHTNESS = 100;
static const int DEFAULT_AMBIENT_BRIGHTNESS = 45;
static const int AMBIENT_BRIGHTNESS_STEP = 5;
static const bool DEFAULT_LIGHT_SCHEDULE_ENABLED = false;
static const int DEFAULT_LIGHT_SCHEDULE_START_HOUR = 8;
static const int DEFAULT_LIGHT_SCHEDULE_END_HOUR = 20;
static const int DEFAULT_LIGHT_SCHEDULE_DIM_MINUTES = 60;
static const LightScheduleDimmingMode DEFAULT_LIGHT_SCHEDULE_DIM_MODE = LIGHT_DIM_NONE;
static const int LIGHT_SCHEDULE_DIM_OPTIONS[] = {1, 5, 10, 15, 30, 60};
static constexpr int LIGHT_SCHEDULE_DIM_OPTION_COUNT =
    sizeof(LIGHT_SCHEDULE_DIM_OPTIONS) / sizeof(LIGHT_SCHEDULE_DIM_OPTIONS[0]);
static const unsigned long LIGHT_SCHEDULE_SERVICE_MS = 1000UL;
static const unsigned long LIGHT_SCHEDULE_TOUCH_WAKE_MS = 60000UL;
static const int LIGHT_SCHEDULE_UI_MIN_LCD_BRIGHTNESS = 35;

static const float MIN_SWAY = 0.25f;
static const float MAX_SWAY = 2.5f;
static const float DEFAULT_SWAY = 1.10f;

static const float MIN_SEAWEED_LENGTH = 0.80f;
static const float MAX_SEAWEED_LENGTH = 1.60f;
static const float DEFAULT_SEAWEED_LENGTH = 1.35f;

static const float MIN_SEAWEED_LENGTH_RANDOMNESS = 0.00f;
static const float MAX_SEAWEED_LENGTH_RANDOMNESS = 0.50f;
static const float DEFAULT_SEAWEED_LENGTH_RANDOMNESS = 0.35f;

static const int MAX_FLAKES = 16;
static const int MAX_FISH_POOL = 48;

static const float FISH_SWIM_WAVE_AMPLITUDE = 1.5f;
static const float FISH_SWIM_WAVE_SPEED = 5.6f;  // 30% slower than the first wave test
static const float FISH_SWIM_WAVE_SPACING = 0.85f;

static const float FISH_AVOID_RADIUS_X = 52.0f;
static const float FISH_AVOID_RADIUS_Y = 20.0f;
static const float FISH_AVOID_STRENGTH = 4.2f;
static const float FISH_CENTER_Y_OFFSET = 7.0f;

static const float OCTOPUS_EXIT_PAD = 42.0f;
static const float OCTOPUS_CENTER_Y_OFFSET = 8.0f;
static const float OCTOPUS_FISH_AVOID_RADIUS_X = 76.0f;
static const float OCTOPUS_FISH_AVOID_RADIUS_Y = 34.0f;
static const float OCTOPUS_FISH_AVOID_STRENGTH = 8.0f;
static const float OCTOPUS_FISH_CLEAR_RADIUS_X = 46.0f;
static const float OCTOPUS_FISH_CLEAR_RADIUS_Y = 22.0f;
static const int CTHULHU_SPAWN_CHANCE_PERCENT = 1;
static const unsigned long CTHULHU_FEED_PELLET_THRESHOLD = 1000UL;
static const float CTHULHU_FISH_AVOID_RADIUS_SCALE = 1.55f;
static const float CTHULHU_FISH_AVOID_STRENGTH_SCALE = 1.85f;
static const float CTHULHU_FISH_CLEAR_RADIUS_SCALE = 1.30f;

static const float SEAHORSE_EXIT_PAD = 48.0f;
static const float SEAHORSE_CENTER_X_OFFSET = 15.0f;
static const float SEAHORSE_CENTER_Y_OFFSET = 24.0f;
static const float SEAHORSE_FISH_AVOID_RADIUS_X = 58.0f;
static const float SEAHORSE_FISH_AVOID_RADIUS_Y = 38.0f;
static const float SEAHORSE_FISH_AVOID_STRENGTH = 6.0f;
static const float SEAHORSE_FISH_CLEAR_RADIUS_X = 34.0f;
static const float SEAHORSE_FISH_CLEAR_RADIUS_Y = 28.0f;
static const float SEAHORSE_SPEED_BOOST = 1.35f;
static const float VISITOR_CLEAR_RADIUS_X = 56.0f;
static const float VISITOR_CLEAR_RADIUS_Y = 38.0f;
static const float SNAIL_EXIT_PAD = 46.0f;
static const float SNAIL_CENTER_X_OFFSET = 9.0f;
static const float SNAIL_CENTER_Y_OFFSET = 7.0f;
static const float SNAIL_FISH_AVOID_RADIUS_X = 30.0f;
static const float SNAIL_FISH_AVOID_RADIUS_Y = 18.0f;
static const float SNAIL_FISH_AVOID_STRENGTH = 4.0f;
static const float SNAIL_FISH_CLEAR_RADIUS_X = 20.0f;
static const float SNAIL_FISH_CLEAR_RADIUS_Y = 12.0f;
static const unsigned long SNAIL_MIN_SPAWN_MS = 900000UL;   // 15 minutes
static const unsigned long SNAIL_MAX_SPAWN_MS = 2100000UL;  // 35 minutes
static const float JELLYFISH_CENTER_X_OFFSET = 12.0f;
static const float JELLYFISH_CENTER_Y_OFFSET = 15.0f;
static const float JELLYFISH_FISH_AVOID_RADIUS_X = 42.0f;
static const float JELLYFISH_FISH_AVOID_RADIUS_Y = 38.0f;
static const float JELLYFISH_FISH_AVOID_STRENGTH = 4.6f;
static const float JELLYFISH_FISH_CLEAR_RADIUS_X = 30.0f;
static const float JELLYFISH_FISH_CLEAR_RADIUS_Y = 26.0f;
static const float SQUID_EXIT_PAD = 34.0f;
static const float SQUID_CENTER_X_OFFSET = 20.0f;
static const float SQUID_CENTER_Y_OFFSET = 6.0f;
static const float SQUID_FISH_AVOID_RADIUS_X = 50.0f;
static const float SQUID_FISH_AVOID_RADIUS_Y = 24.0f;
static const float SQUID_FISH_AVOID_STRENGTH = 5.2f;
static const float SQUID_FISH_CLEAR_RADIUS_X = 32.0f;
static const float SQUID_FISH_CLEAR_RADIUS_Y = 16.0f;
static const bool SQUID_ENABLED = false;  // Prototype parked while the creature roster settles.

enum BackgroundStyle {
  BACKGROUND_STYLE_BLACK,
  BACKGROUND_STYLE_DITHERED,
  BACKGROUND_STYLE_SMOOTH,
  BACKGROUND_STYLE_FLOWERS,
  BACKGROUND_STYLE_COUNT
};
static const BackgroundStyle DEFAULT_BACKGROUND_STYLE = BACKGROUND_STYLE_DITHERED;
static const BackgroundStyle kBackgroundCycleStyles[] = {
    BACKGROUND_STYLE_BLACK,
    BACKGROUND_STYLE_DITHERED,
    BACKGROUND_STYLE_SMOOTH,
    BACKGROUND_STYLE_FLOWERS,
};
static const int BACKGROUND_CYCLE_STYLE_COUNT = sizeof(kBackgroundCycleStyles) / sizeof(kBackgroundCycleStyles[0]);

static const int CLOCK_MIN_YEAR = 2024;
static const int CLOCK_MAX_YEAR = 2099;
static const int DEFAULT_CLOCK_YEAR = 2026;
static const int DEFAULT_CLOCK_MONTH = 1;
static const int DEFAULT_CLOCK_DAY = 1;
static const int DEFAULT_CLOCK_HOUR = 12;
static const int DEFAULT_CLOCK_MINUTE = 0;

static constexpr const char* CLOCK_NTP_1 = "pool.ntp.org";
static constexpr const char* CLOCK_NTP_2 = "time.nist.gov";
static constexpr const char* CLOCK_NTP_3 = "time.google.com";
static const int DEFAULT_TIMEZONE_INDEX = 5;  // Central time, matching the original hard-coded default.

static const int MAX_WIFI_NETWORKS = 12;
static const int WIFI_SSID_MAX_LEN = 32;
static const int WIFI_PASS_MAX_LEN = 64;
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 18000UL;
static const unsigned long WIFI_RECONNECT_DELAY_MS = 15000UL;
static const unsigned long WIFI_SERVICE_ACTIVE_MS = 250UL;
static const unsigned long WIFI_SERVICE_UNSYNCED_MS = 1000UL;
static const unsigned long WIFI_SERVICE_IDLE_MS = 5000UL;
static const unsigned long WIFI_SERVICE_SYNCED_MS = 10000UL;
static const unsigned long NTP_RETRY_MS = 5000UL;
static const unsigned long NTP_REFRESH_MS = 3600000UL;
static const unsigned long BACKGROUND_RAINBOW_CYCLE_MS = 120000UL;
static const unsigned long BACKGROUND_RAINBOW_UPDATE_MS = 1000UL;

// ------------------------------ Objects --------------------------------------
struct Flake {
  bool active;
  float x, y;
  float vy;
  uint16_t color;
};

struct Bubble {
  bool active;
  float x, y;
  float baseX;
  float vy;
  float phase;
  float swayAmp;
  uint16_t color;
};

struct FishSpecies {
  const char* right;  // ASCII-only, faces right; mirrored at boot for vx < 0 (Font 2 safe)
  uint16_t baseColor;
};

struct Fish {
  bool active;
  int type;
  float x, y;
  float vx, vy;
  float speed;
  float phase;
  float wanderBias;
  int visualWidth;
  uint16_t displayColor;
  uint16_t renderColor;
  float depthBrightness;
};

struct Octopus {
  bool active;
  bool cthulhu;
  float x;
  float y;
  float baseY;
  float vx;
  float phase;
  float colorPhase;
  unsigned long nextSpawnMs;
};

struct Seahorse {
  bool active;
  bool facingRight;
  float x;
  float y;
  float baseY;
  float vx;
  float phase;
  float finPhase;
  unsigned long nextSpawnMs;
};

struct Snail {
  bool active;
  bool facingRight;
  float x;
  float y;
  float vx;
  float phase;
  unsigned long nextSpawnMs;
};

struct Jellyfish {
  bool active;
  float x;
  float y;
  float baseY;
  float vx;
  float phase;
  unsigned long nextSpawnMs;
};

struct Squid {
  bool active;
  bool facingRight;
  float x;
  float y;
  float baseY;
  float vx;
  float phase;
  float burstPhase;
};

struct TimezoneOption {
  const char* label;
  const char* posix;
};

struct AsciiClockGlyph {
  char c;
  const char* rows[ASCII_CLOCK_ROWS];
};

struct AsciiClockFont {
  const char* label;
  uint8_t rowCount;
  uint8_t glyphGap;
  const AsciiClockGlyph* glyphs;
  uint8_t glyphCount;
};

static const TimezoneOption timezoneOptions[] = {
    {"UTC", "UTC0"},
    {"Hawaii", "HST10"},
    {"Alaska", "AKST9AKDT,M3.2.0/2,M11.1.0/2"},
    {"Pacific", "PST8PDT,M3.2.0/2,M11.1.0/2"},
    {"Mountain", "MST7MDT,M3.2.0/2,M11.1.0/2"},
    {"Central", "CST6CDT,M3.2.0/2,M11.1.0/2"},
    {"Eastern", "EST5EDT,M3.2.0/2,M11.1.0/2"},
    {"Atlantic", "AST4ADT,M3.2.0/2,M11.1.0/2"},
    {"Newfound", "NST3:30NDT,M3.2.0/2,M11.1.0/2"},
    {"UTC-3", "UTC3"},
    {"UTC-2", "UTC2"},
    {"UTC-1", "UTC1"},
    {"UK", "GMT0BST,M3.5.0/1,M10.5.0/2"},
    {"Central EU", "CET-1CEST,M3.5.0/2,M10.5.0/3"},
    {"Eastern EU", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"UTC+3", "UTC-3"},
    {"Iran", "IRST-3:30"},
    {"Gulf", "GST-4"},
    {"UTC+5", "UTC-5"},
    {"India", "IST-5:30"},
    {"UTC+6", "UTC-6"},
    {"UTC+7", "UTC-7"},
    {"China", "CST-8"},
    {"Japan", "JST-9"},
    {"Darwin", "ACST-9:30"},
    {"Sydney", "AEST-10AEDT,M10.1.0/2,M4.1.0/3"},
    {"UTC+11", "UTC-11"},
    {"New Zealand", "NZST-12NZDT,M9.5.0/2,M4.1.0/3"},
    {"UTC+13", "UTC-13"},
    {"UTC+14", "UTC-14"},
};
static constexpr int TIMEZONE_COUNT = sizeof(timezoneOptions) / sizeof(timezoneOptions[0]);

// RGB565 theme colours (names match your list)
static constexpr uint16_t RGB565(uint8_t r, uint8_t g, uint8_t b) {
  return uint16_t((r & 0xF8) << 8 | (g & 0xFC) << 3 | (b >> 3));
}

uint16_t scaleRgb565(uint16_t color, float brightness) {
  if (brightness < 0.0f) brightness = 0.0f;
  if (brightness > 1.0f) brightness = 1.0f;
  uint16_t r = (uint16_t)(((color >> 11) & 0x1F) * brightness);
  uint16_t g = (uint16_t)(((color >> 5) & 0x3F) * brightness);
  uint16_t b = (uint16_t)((color & 0x1F) * brightness);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

uint16_t randomBubbleColor() {
  return RGB565((uint8_t)random(0, 5), (uint8_t)random(8, 24), (uint8_t)random(45, 106));
}

uint16_t randomFoodColor() {
  // Keep food orange but vary brightness/tint slightly for a natural look.
  return RGB565((uint8_t)random(220, 256), (uint8_t)random(118, 166), (uint8_t)random(0, 34));
}

int rgb565R8(uint16_t color) { return ((color >> 11) & 0x1F) * 255 / 31; }
int rgb565G8(uint16_t color) { return ((color >> 5) & 0x3F) * 255 / 63; }
int rgb565B8(uint16_t color) { return (color & 0x1F) * 255 / 31; }

uint16_t colorWheel565(uint16_t phase) {
  phase %= 1536;
  uint8_t segment = phase / 256;
  uint8_t offset = phase & 0xFF;
  uint8_t inv = 255 - offset;

  switch (segment) {
    case 0:
      return RGB565(255, offset, 0);
    case 1:
      return RGB565(inv, 255, 0);
    case 2:
      return RGB565(0, 255, offset);
    case 3:
      return RGB565(0, inv, 255);
    case 4:
      return RGB565(offset, 0, 255);
    default:
      return RGB565(255, 0, inv);
  }
}

uint16_t rainbowColorAtMs(unsigned long now) {
  uint32_t phase = ((now % BACKGROUND_RAINBOW_CYCLE_MS) * 1536UL) / BACKGROUND_RAINBOW_CYCLE_MS;
  return colorWheel565((uint16_t)phase);
}

static const uint16_t DEFAULT_ASCII_CLOCK_COLOR = RGB565(0, 20, 95);
static const uint16_t DEFAULT_SMALL_CLOCK_COLOR = TFT_WHITE;
static const uint16_t DEFAULT_BACKGROUND_GRADIENT_COLOR = RGB565(0, 8, 255);
static const uint16_t DEFAULT_BACKGROUND_PURPLE_COLOR = RGB565(108, 6, 220);
static const uint16_t DEFAULT_AUTO_SKY_SUNRISE_COLOR = RGB565(255, 128, 20);
static const uint16_t DEFAULT_AUTO_SKY_DAY_COLOR = DEFAULT_BACKGROUND_GRADIENT_COLOR;
static const uint16_t DEFAULT_AUTO_SKY_SUNSET_COLOR = RGB565(255, 96, 16);
static const uint16_t DEFAULT_AUTO_SKY_NIGHT_COLOR = DEFAULT_BACKGROUND_PURPLE_COLOR;

static const uint16_t kBackgroundColorPalette[] = {
    DEFAULT_BACKGROUND_GRADIENT_COLOR,
    DEFAULT_BACKGROUND_PURPLE_COLOR,
    RGB565(0, 180, 255),
    RGB565(0, 220, 220),
    RGB565(0, 180, 120),
    RGB565(40, 220, 80),
    RGB565(160, 255, 40),
    RGB565(255, 220, 40),
    RGB565(255, 128, 20),
    RGB565(255, 40, 40),
    RGB565(255, 120, 200),
    RGB565(220, 60, 255),
    RGB565(120, 80, 255),
    RGB565(80, 180, 255),
    RGB565(40, 80, 255),
    RGB565(156, 120, 255),
    DEFAULT_ASCII_CLOCK_COLOR,
    RGB565(0, 76, 76),
    RGB565(116, 108, 18),
    RGB565(130, 56, 0),
    RGB565(100, 0, 0),
    RGB565(58, 20, 120),
    TFT_WHITE,
};
static constexpr int BACKGROUND_COLOR_COUNT = sizeof(kBackgroundColorPalette) / sizeof(kBackgroundColorPalette[0]);

static const AsciiClockGlyph kAsciiClockStandardGlyphs[] = {
    {'0', {"  ___", " / _ \\", "| | | |", "| |_| |", " \\___/", ""}},
    {'1', {" _", "/ |", "| |", "| |", "|_|", ""}},
    {'2', {" ____", "|___ \\", "  __) |", " / __/", "|_____|", ""}},
    {'3', {" _____", "|___ /", "  |_ \\", " ___) |", "|____/", ""}},
    {'4', {" _  _", "| || |", "| || |_", "|__   _|", "   |_|", ""}},
    {'5', {" ____", "| ___|", "|___ \\", " ___) |", "|____/", ""}},
    {'6', {"  __", " / /_", "| '_ \\", "| (_) |", " \\___/", ""}},
    {'7', {" _____", "|___  |", "   / /", "  / /", " /_/", ""}},
    {'8', {"  ___", " ( _ )", " / _ \\", "| (_) |", " \\___/", ""}},
    {'9', {"  ___", " / _ \\", "| (_) |", " \\__, |", "  /_/", ""}},
    {':', {" ", " _", "(_)", " _", "(_)", " "}},
    {' ', {" ", " ", " ", " ", " ", " "}},
};

static const AsciiClockGlyph kAsciiClockBulbheadGlyphs[] = {
    {'0', {"  ___", " / _ \\", "( (_) )", " \\___/"}},
    {'1', {" __", "/  )", " )(", "(__)"}},
    {'2', {" ___", "(__ \\", " / _/", "(____)"}},
    {'3', {" ___", "(__ )", " (_ \\", "(___/"}},
    {'4', {"  __", " /. |", "(_  _)", "  (_)"}},
    {'5', {" ___", "| __)", "|__ \\", "(___/"}},
    {'6', {"  _", " / )", "/ _ \\", "\\___/"}},
    {'7', {" ___", "(__ )", " / /", "(_/"}},
    {'8', {" ___", "( _ )", "/ _ \\", "\\___/"}},
    {'9', {" ___", "/ _ \\", "\\_  /", " (_/"}},
    {':', {"", "()", "", "()"}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockDoomGlyphs[] = {
    {'0', {" _____", "|  _  |", "| |/' |", "|  /| |", "\\ |_/ /", " \\___/", "", ""}},
    {'1', {" __", "/  |", "`| |", " | |", "_| |_", "\\___/", "", ""}},
    {'2', {" _____", "/ __  \\", "`' / /'", "  / /", "./ /___", "\\_____/", "", ""}},
    {'3', {" _____", "|____ |", "    / /", "    \\ \\", ".___/ /", "\\____/", "", ""}},
    {'4', {"   ___", "  /   |", " / /| |", "/ /_| |", "\\___  |", "    |_/", "", ""}},
    {'5', {" _____", "|  ___|", "|___ \\", "    \\ \\", "/\\__/ /", "\\____/", "", ""}},
    {'6', {"  ____", " / ___|", "/ /___", "| ___ \\", "| \\_/ |", "\\_____/", "", ""}},
    {'7', {" ______", "|___  /", "   / /", "  / /", "./ /", "\\_/", "", ""}},
    {'8', {" _____", "|  _  |", " \\ V /", " / _ \\", "| |_| |", "\\_____/", "", ""}},
    {'9', {" _____", "|  _  |", "| |_| |", "\\____ |", ".___/ /", "\\____/", "", ""}},
    {':', {"", " _", "(_)", "", " _", "(_)", "", ""}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockGracefulGlyphs[] = {
    {'0', {"  __", " /  \\", "(  0 )", " \\__/"}},
    {'1', {"  __", " /  \\", "(_/ /", " (__)"}},
    {'2', {" ____", "(___ \\", " / __/", "(____)"}},
    {'3', {" ____", "( __ \\", " (__ (", "(____/"}},
    {'4', {"  ___", " / _ \\", "(__  (", "  (__/"}},
    {'5', {"  ___", " / __)", "(___ \\", "(____/"}},
    {'6', {"  ___", " / __)", "(  _ \\", " \\___/"}},
    {'7', {" ____", "(__  )", "  / /", " (_/"}},
    {'8', {" ____", "/ _  \\", ") _  (", "\\____/"}},
    {'9', {" ___", "/ _ \\", "\\__  )", "(___/"}},
    {':', {" _", "(_)", " _", "(_)"}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockOgreGlyphs[] = {
    {'0', {"  ___", " / _ \\", "| | | |", "| |_| |", " \\___/", ""}},
    {'1', {" _", "/ |", "| |", "| |", "|_|", ""}},
    {'2', {" ____", "|___ \\", "  __) |", " / __/", "|_____|", ""}},
    {'3', {" _____", "|___ /", "  |_ \\", " ___) |", "|____/", ""}},
    {'4', {" _  _", "| || |", "| || |_", "|__   _|", "   |_|", ""}},
    {'5', {" ____", "| ___|", "|___ \\", " ___) |", "|____/", ""}},
    {'6', {"  __", " / /_", "| '_ \\", "| (_) |", " \\___/", ""}},
    {'7', {" _____", "|___  |", "   / /", "  / /", " /_/", ""}},
    {'8', {"  ___", " ( _ )", " / _ \\", "| (_) |", " \\___/", ""}},
    {'9', {"  ___", " / _ \\", "| (_) |", " \\__, |", "   /_/", ""}},
    {':', {"", " _", "(_)", " _", "(_)", ""}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockSmallGlyphs[] = {
    {'0', {"  __", " /  \\", "| () |", " \\__/", ""}},
    {'1', {" _", "/ |", "| |", "|_|", ""}},
    {'2', {" ___", "|_  )", " / /", "/___|", ""}},
    {'3', {" ____", "|__ /", " |_ \\", "|___/", ""}},
    {'4', {" _ _", "| | |", "|_  _|", "  |_|", ""}},
    {'5', {" ___", "| __|", "|__ \\", "|___/", ""}},
    {'6', {"  __", " / /", "/ _ \\", "\\___/", ""}},
    {'7', {" ____", "|__  |", "  / /", " /_/", ""}},
    {'8', {" ___", "( _ )", "/ _ \\", "\\___/", ""}},
    {'9', {" ___", "/ _ \\", "\\_, /", " /_/", ""}},
    {':', {" _", "(_)", " _", "(_)", ""}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockSoftGlyphs[] = {
    {'0', {"", "  ,--.", " /    \\", "|  ()  |", " \\    /", "  `--'", ""}},
    {'1', {"", " ,--.", "/   |", "`|  |", " |  |", " `--'", ""}},
    {'2', {"", " ,---.", "'.-.  \\", " .-' .'", "/   '-.", "'-----'", ""}},
    {'3', {"", ",----.", "'.-.  |", "  .' <", "/'-'  |", "`----'", ""}},
    {'4', {"", "  ,---.", " /    |", "/  '  |", "'--|  |", "   `--'", ""}},
    {'5', {"", ",-----.", "|  .--'", "'--. `\\", ".--'  /", "`----'", ""}},
    {'6', {"", "  ,--.", " /  .'", "|  .-.", "\\   o |", " `---'", ""}},
    {'7', {"", ",-----.", "'--,  /", " .'  /", "/   /", "`--'", ""}},
    {'8', {"", " ,---.", "|  o  |", ".'   '.", "|  o  |", " `---'", ""}},
    {'9', {"", " ,---.", "| o   \\", "`..'  |", " .'  /", " `--'", ""}},
    {':', {"", "", ".--.", "'--'", ".--.", "'--'", ""}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClock3DASCIIGlyphs[] = {
    {'0', {"  ****", " *///**", "/*  */*", "/* * /*", "/**  /*", "/*   /*", "/ ****", " ////"}},
    {'1', {"  **", " ***", "//**", " /**", " /**", " /**", " ****", "////"}},
    {'2', {"  ****", " */// *", "/    /*", "   ***", "  *//", " *", "/******", "//////"}},
    {'3', {"  ****", " */// *", "/    /*", "   ***", "  /// *", " *   /*", "/ ****", " ////"}},
    {'4', {"    **", "   */*", "  * /*", " ******", "/////*", "    /*", "    /*", "    /"}},
    {'5', {" ******", "/*////", "/*****", "///// *", "     /*", " *   /*", "/ ****", " ////"}},
    {'6', {"  ****", " */// *", "/*   /", "/*****", "/*/// *", "/*   /*", "/ ****", " ////"}},
    {'7', {" ******", "//////*", "     /*", "     *", "    *", "   *", "  *", " /"}},
    {'8', {"  ****", " */// *", "/*   /*", "/ ****", " */// *", "/*   /*", "/ ****", " ////"}},
    {'9', {"  ****", " */// *", "/*   /*", "/ ****", " ///*", "   *", "  *", " /"}},
    {':', {"", "", "", "", " **", "//", " **", "//"}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockChunkyGlyphs[] = {
    {'0', {" ______", "|      |", "|  --  |", "|______|", ""}},
    {'1', {" ____", "|_   |", " _|  |_", "|______|", ""}},
    {'2', {" ______", "|__    |", "|    __|", "|______|", ""}},
    {'3', {" ______", "|__    |", "|__    |", "|______|", ""}},
    {'4', {" _____", "|  |  |", "|__    |", "   |__|", ""}},
    {'5', {" ______", "|    __|", "|__    |", "|______|", ""}},
    {'6', {" ______", "|    __|", "|  __  |", "|______|", ""}},
    {'7', {" ______", "|      |", "|_     |", "  |____|", ""}},
    {'8', {" ______", "|  __  |", "|  __  |", "|______|", ""}},
    {'9', {" ______", "|  __  |", "|__    |", "|______|", ""}},
    {':', {" __", "|__|", " __", "|__|", ""}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockCricketGlyphs[] = {
    {'0', {" _______", "|   _   |", "|.  |   |", "|.  |   |", "|:  1   |", "|::.. . |", "`-------'", ""}},
    {'1', {" _____", "| _   |", "|.|   |", "`-|.  |", "  |:  |", "  |::.|", "  `---'", ""}},
    {'2', {" _______", "|       |", "|___|   |", " /  ___/", "|:  1  \\", "|::.. . |", "`-------'", ""}},
    {'3', {" _______", "|   _   |", "|___|   |", " _(__   |", "|:  1   |", "|::.. . |", "`-------'", ""}},
    {'4', {" ___ ___", "|   Y   |", "|   |   |", "|____   |", "    |:  |", "    |::.|", "    `---'", ""}},
    {'5', {" _______", "|   _   |", "|   1___|", "|____   |", "|:  1   |", "|::.. . |", "`-------'", ""}},
    {'6', {" _______", "|   _   |", "|   1___|", "|.     \\", "|:  1   |", "|::.. . |", "`-------'", ""}},
    {'7', {" _______", "|   _   |", "|___|   |", "   /   /", "  |   |", "  |   |", "  `---'", ""}},
    {'8', {" _______", "|   _   |", "|.  |   |", "|.  _   |", "|:  1   |", "|::.. . |", "`-------'", ""}},
    {'9', {" _______", "|   _   |", "|   |   |", " \\___   |", "|:  1   |", "|::.. . |", "`-------'", ""}},
    {':', {" __", "|__|", " __", "|__|", "", "", "", ""}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockFuzzyGlyphs[] = {
    {'0', {" .--.", ": ,. :", ": :: :", ": :; :", "`.__.'", "", ""}},
    {'1', {"  ,-.", ".'  :", " `: :", "  : :", "  :_;", "", ""}},
    {'2', {".---.", "`--. :", "  ,','", ".'.'_", ":____;", "", ""}},
    {'3', {".----.", "`--  ;", " .' '", " _`,`.", "`.__.'", "", ""}},
    {'4', {"  .-.", " .'.'", ".'.'_", ":_ ` :", "  :_:", "", ""}},
    {'5', {".----.", ": .--'", "`. `.", ".-`, :", "`.__.'", "", ""}},
    {'6', {"  .-.", " .'.'", ".' '.", ": .; :", "`.__.'", "", ""}},
    {'7', {".----.", "`--  ;", " ,','", " : :", " :_:", "", ""}},
    {'8', {" .--.", ": .; :", "`.  .'", ": .; :", "`.__.'", "", ""}},
    {'9', {" .--.", ": .; :", "`._, :", "   : :", "   :_:", "", ""}},
    {':', {"", " _", ":_:", " _", ":_;", "", ""}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockGreekGlyphs[] = {
    {'0', {"", "  ___", " / _ \\", "| | | |", "| | | |", "| |_| |", " \\___/", "", ""}},
    {'1', {"", " _", "/ |", "- |", "| |", "| |", "|_|", "", ""}},
    {'2', {"", " ____", "(___ \\", "  __) )", " / __/", "| |___", "|_____)", "", ""}},
    {'3', {"", " _____", "(__  /", "  / /", " (__ \\", " ___) )", "(____/", "", ""}},
    {'4', {"", "    _", "  /  |", " / o |_", "/__   _)", "   | |", "   |_|", "", ""}},
    {'5', {"", " ____", "|  __)", "| |__", "|___ \\", " ___) )", "(____/", "", ""}},
    {'6', {"", "   __", "  / /", " / /_", "| '_ \\", "| (_) )", " \\___/", "", ""}},
    {'7', {"", " _____", "(___  )", "  _/ /", " (  _)", " / /", "/_/", "", ""}},
    {'8', {"", "  ___", " /   \\", " \\ O /", " / _ \\", "( (_) )", " \\___/", "", ""}},
    {'9', {"", "  ___", " / _ \\", "( (_) |", " \\__, |", "   / /", "  /_/", "", ""}},
    {':', {"", "", "", " _", "(_)", "", "", "", ""}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockLarry3DGlyphs[] = {
    {'0', {"   __", " /'__`\\", "/\\ \\/\\ \\", "\\ \\ \\ \\ \\", " \\ \\ \\_\\ \\", "  \\ \\____/", "   \\/___/", "", ""}},
    {'1', {"   _", " /' \\", "/\\_, \\", "\\/_/\\ \\", "   \\ \\ \\", "    \\ \\_\\", "     \\/_/", "", ""}},
    {'2', {"   ___", " /'___`\\", "/\\_\\ /\\ \\", "\\/_/// /__", "   // /_\\ \\", "  /\\______/", "  \\/_____/", "", ""}},
    {'3', {"   __", " /'__`\\", "/\\_\\L\\ \\", "\\/_/_\\_<_", "  /\\ \\L\\ \\", "  \\ \\____/", "   \\/___/", "", ""}},
    {'4', {" __ __", "/\\ \\\\ \\", "\\ \\ \\\\ \\", " \\ \\ \\\\ \\_", "  \\ \\__ ,__\\", "   \\/_/\\_\\_/", "      \\/_/", "", ""}},
    {'5', {" ______", "/\\  ___\\", "\\ \\ \\__/", " \\ \\___``\\", "  \\/\\ \\L\\ \\", "   \\ \\____/", "    \\/___/", "", ""}},
    {'6', {"  ____", " /'___\\", "/\\ \\__/", "\\ \\  _``\\", " \\ \\ \\L\\ \\", "  \\ \\____/", "   \\/___/", "", ""}},
    {'7', {" ________", "/\\_____  \\", "\\/___//'/'", "    /' /'", "  /' /'", " /\\_/", " \\//", "", ""}},
    {'8', {"   __", " /'_ `\\", "/\\ \\L\\ \\", "\\/_> _ <_", "  /\\ \\L\\ \\", "  \\ \\____/", "   \\/___/", "", ""}},
    {'9', {"   __", " /'_ `\\", "/\\ \\L\\ \\", "\\ \\___, \\", " \\/__,/\\ \\", "      \\ \\_\\", "       \\/_/", "", ""}},
    {':', {"", "", " __", "/\\_\\", "\\/_/_", "  /\\_\\", "  \\/_/", "", ""}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockLCDGlyphs[] = {
    {'0', {" ___", "|  /|", "| + |", "|/  |", " ---", ""}},
    {'1', {" _", "  |", "  +", "  |", " ---", ""}},
    {'2', {" ___", "    |", " -+-", "|", " ---", ""}},
    {'3', {" ___", "    |", " -+-", "    |", " ---", ""}},
    {'4', {"", "| |", " -+-", "  |", "", ""}},
    {'5', {" ___", "|", " -+-", "    |", " ---", ""}},
    {'6', {" ___", "|", "|-+-", "|   |", " ---", ""}},
    {'7', {" ___", "   /", "  +", " /", "", ""}},
    {'8', {" ___", "|   |", " -+-", "|   |", " ---", ""}},
    {'9', {" ___", "|   |", " -+-|", "    |", " ---", ""}},
    {':', {"", "  |", "", "  |", "", ""}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockLeanGlyphs[] = {
    {'0', {"", "     _/", "  _/  _/", " _/  _/", "_/  _/", " _/", "", ""}},
    {'1', {"", "    _/", " _/_/", "  _/", " _/", "_/", "", ""}},
    {'2', {"", "      _/_/", "   _/    _/", "      _/", "   _/", "_/_/_/_/", "", ""}},
    {'3', {"", "    _/_/_/", "         _/", "    _/_/", "       _/", "_/_/_/", "", ""}},
    {'4', {"", "  _/  _/", " _/  _/", "_/_/_/_/", "   _/", "  _/", "", ""}},
    {'5', {"", "    _/_/_/_/", "   _/", "  _/_/_/", "       _/", "_/_/_/", "", ""}},
    {'6', {"", "     _/_/_/", "  _/", " _/_/_/", "_/    _/", " _/_/", "", ""}},
    {'7', {"", "  _/_/_/_/_/", "         _/", "      _/", "   _/", "_/", "", ""}},
    {'8', {"", "     _/_/", "  _/    _/", "   _/_/", "_/    _/", " _/_/", "", ""}},
    {'9', {"", "      _/_/", "   _/    _/", "    _/_/_/", "       _/", "_/_/_/", "", ""}},
    {':', {"", "", "   _/", "", "", "_/", "", ""}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockNTGreekGlyphs[] = {
    {'0', {"", "  ___", " / _ \\", "| | | |", "| | | |", "| |_| |", " \\___/", "", ""}},
    {'1', {"", " _", "/ |", "- |", "| |", "| |", "|_|", "", ""}},
    {'2', {"", " ____", "(___ \\", "  __) )", " / __/", "| |___", "|_____)", "", ""}},
    {'3', {"", " _____", "(__  /", "  / /", " (__ \\", " ___) )", "(____/", "", ""}},
    {'4', {"", "    _", "  /  |", " / o |_", "/__   _)", "   | |", "   |_|", "", ""}},
    {'5', {"", " ____", "|  __)", "| |__", "|___ \\", " ___) )", "(____/", "", ""}},
    {'6', {"", "   __", "  / /", " / /_", "| '_ \\", "| (_) )", " \\___/", "", ""}},
    {'7', {"", " _____", "(___  )", "  _/ /", " (  _)", " / /", "/_/", "", ""}},
    {'8', {"", "  ___", " /   \\", " \\ O /", " / _ \\", "( (_) )", " \\___/", "", ""}},
    {'9', {"", "  ___", " / _ \\", "( (_) |", " \\__, |", "   / /", "  /_/", "", ""}},
    {':', {"", "", "", " _", "(_)", "", "", "", ""}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockPuffyGlyphs[] = {
    {'0', {"  __", "/' _`\\", "| ( ) |", "| | | |", "| (_) |", "`\\___/'", "", ""}},
    {'1', {"   _", " /' )", "(_, |", "  | |", "  | |", "  (_)", "", ""}},
    {'2', {"   __", " /'__`\\", "(_)  ) )", "   /' /", " /' /( )", "(_____/'", "", ""}},
    {'3', {"   ___", " /'_  )", "(_)_) |", " _(_ <", "( )_) |", "`\\____)", "", ""}},
    {'4', {" _  _", "( )( )", "| || |", "| || |_", "(__ ,__)", "   (_)", "", ""}},
    {'5', {" _____", "(  ___)", "| (__", "|___ `\\", "( )_) |", "`\\___/'", "", ""}},
    {'6', {" _____", "(  ___)", "| (__", "|  _ `\\", "| (_) |", "`\\___/'", "", ""}},
    {'7', {" _______", "(_____  )", "     /'/'", "   /'/'", " /'/'", "(_/", "", ""}},
    {'8', {"   _", " /'_`\\", "( (_) )", " > _ <'", "( (_) )", "`\\___/'", "", ""}},
    {'9', {"   __", " /'_ `\\", "( (_) |", " \\__, |", "    | |", "    (_)", "", ""}},
    {':', {"", "", " _", "(_)", " _", "(_)", "", ""}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockRammsteinGlyphs[] = {
    {'0', {"", " _____", "/     |", "|  /  |", "|_____/", "", ""}},
    {'1', {"", " _____", "|_    |", " |    |", " |____|", "", ""}},
    {'2', {"", " ______", "|____  |", "|    --|", "|______|", "", ""}},
    {'3', {"", " ______", "|___   |", "|___   |", "|______|", "", ""}},
    {'4', {"", " __   _", "|  | | |", "|  |_| |", "'----__|", "", ""}},
    {'5', {"", " ______", "|  ____|", "|___   \\", "|______/", "", ""}},
    {'6', {"", "  ____", " /   /_", "|   _  |", "|______|", "", ""}},
    {'7', {"", " ______", "|___   |", "  /   /", " |___|", "", ""}},
    {'8', {"", " _____", "<  -  >", "/  _  \\", "\\_____/", "", ""}},
    {'9', {"", " ______", "|   _  |", "|____  |", "    |__|", "", ""}},
    {':', {"", " _", "|_|", " _", "|_|", "", ""}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockStopGlyphs[] = {
    {'0', {"  ______", " / __   |", "| | //| |", "| |// | |", "|  /__| |", " \\_____/", ""}},
    {'1', {"  __", " /  |", "/_/ |", "  | |", "  | |", "  |_|", ""}},
    {'2', {" ______", "(_____ \\", "  ____) )", " /_____/", " _______", "(_______)", ""}},
    {'3', {" ________", "(_______/", "   ____", "  (___ \\", " _____) )", "(______/", ""}},
    {'4', {"   __", "  / /", " / /____", "|___   _)", "    | |", "    |_|", ""}},
    {'5', {" _______", "(_______)", " ______", "(_____ \\", " _____) )", "(______/", ""}},
    {'6', {"    __", "   / /", "  / /_", " / __ \\", "( (__) )", " \\____/", ""}},
    {'7', {" _______", "(_______)", "      _", "     / )", "    / /", "   (_/", ""}},
    {'8', {"  _____", " / ___ \\", "( (   ) )", " > > < <", "( (___) )", " \\_____/", ""}},
    {'9', {"  ____", " / __ \\", "( (__) )", " \\__  /", "   / /", "  /_/", ""}},
    {':', {"", "", " _", "(_)", " _", "(_)", ""}},
    {' ', {" "}},
};

static const AsciiClockGlyph kAsciiClockSwanGlyphs[] = {
    {'0', {"", "", " .-.", ":   :", "|   |", ":   ;", " `-'", "", ""}},
    {'1', {"", "", "  .", ".'|", "  |", "  |", "'---'", "", ""}},
    {'2', {"", "", " .-.", "(   )", "  .'", " /", "'---'", "", ""}},
    {'3', {"", "", ".--.", "    )", " --:", "    )", "`--'", "", ""}},
    {'4', {"", "", ".  .", "|  |", "'--|-", "   |", "   '", "", ""}},
    {'5', {"", "", ".---.", "|", "'--.", ".   )", " `-'", "", ""}},
    {'6', {"", "", "   ,", "  /", " /-.", "(   )", " `-'", "", ""}},
    {'7', {"", "", ".---.", "    /", "   /", "  /", " '", "", ""}},
    {'8', {"", "", " .-.", "(   )", " >-<", "(   )", " `-'", "", ""}},
    {'9', {"", "", " .-.", "(   )", " `-/", "  /", " '", "", ""}},
    {':', {"", "", "", "", "o", "", "o", "", ""}},
    {' ', {" "}},
};

static const AsciiClockFont kAsciiClockFonts[] = {
    {"Standard", 6, 1, kAsciiClockStandardGlyphs, (uint8_t)(sizeof(kAsciiClockStandardGlyphs) / sizeof(kAsciiClockStandardGlyphs[0]))},
    {"Bulbhead", 4, 1, kAsciiClockBulbheadGlyphs, (uint8_t)(sizeof(kAsciiClockBulbheadGlyphs) / sizeof(kAsciiClockBulbheadGlyphs[0]))},
    {"Doom", 8, 1, kAsciiClockDoomGlyphs, (uint8_t)(sizeof(kAsciiClockDoomGlyphs) / sizeof(kAsciiClockDoomGlyphs[0]))},
    {"Graceful", 4, 1, kAsciiClockGracefulGlyphs, (uint8_t)(sizeof(kAsciiClockGracefulGlyphs) / sizeof(kAsciiClockGracefulGlyphs[0]))},
    {"Ogre", 6, 1, kAsciiClockOgreGlyphs, (uint8_t)(sizeof(kAsciiClockOgreGlyphs) / sizeof(kAsciiClockOgreGlyphs[0]))},
    {"Small", 5, 1, kAsciiClockSmallGlyphs, (uint8_t)(sizeof(kAsciiClockSmallGlyphs) / sizeof(kAsciiClockSmallGlyphs[0]))},
    {"Soft", 7, 1, kAsciiClockSoftGlyphs, (uint8_t)(sizeof(kAsciiClockSoftGlyphs) / sizeof(kAsciiClockSoftGlyphs[0]))},
    {"3D-ASCII", 8, 1, kAsciiClock3DASCIIGlyphs, (uint8_t)(sizeof(kAsciiClock3DASCIIGlyphs) / sizeof(kAsciiClock3DASCIIGlyphs[0]))},
    {"Chunky", 5, 1, kAsciiClockChunkyGlyphs, (uint8_t)(sizeof(kAsciiClockChunkyGlyphs) / sizeof(kAsciiClockChunkyGlyphs[0]))},
    {"Cricket", 8, 1, kAsciiClockCricketGlyphs, (uint8_t)(sizeof(kAsciiClockCricketGlyphs) / sizeof(kAsciiClockCricketGlyphs[0]))},
    {"Fuzzy", 7, 1, kAsciiClockFuzzyGlyphs, (uint8_t)(sizeof(kAsciiClockFuzzyGlyphs) / sizeof(kAsciiClockFuzzyGlyphs[0]))},
    {"Greek", 9, 1, kAsciiClockGreekGlyphs, (uint8_t)(sizeof(kAsciiClockGreekGlyphs) / sizeof(kAsciiClockGreekGlyphs[0]))},
    {"Larry 3D", 9, 0, kAsciiClockLarry3DGlyphs, (uint8_t)(sizeof(kAsciiClockLarry3DGlyphs) / sizeof(kAsciiClockLarry3DGlyphs[0]))},
    {"LCD", 6, 1, kAsciiClockLCDGlyphs, (uint8_t)(sizeof(kAsciiClockLCDGlyphs) / sizeof(kAsciiClockLCDGlyphs[0]))},
    {"Lean", 8, 0, kAsciiClockLeanGlyphs, (uint8_t)(sizeof(kAsciiClockLeanGlyphs) / sizeof(kAsciiClockLeanGlyphs[0]))},
    {"NT Greek", 9, 1, kAsciiClockNTGreekGlyphs, (uint8_t)(sizeof(kAsciiClockNTGreekGlyphs) / sizeof(kAsciiClockNTGreekGlyphs[0]))},
    {"Puffy", 8, 1, kAsciiClockPuffyGlyphs, (uint8_t)(sizeof(kAsciiClockPuffyGlyphs) / sizeof(kAsciiClockPuffyGlyphs[0]))},
    {"Rammstein", 7, 1, kAsciiClockRammsteinGlyphs, (uint8_t)(sizeof(kAsciiClockRammsteinGlyphs) / sizeof(kAsciiClockRammsteinGlyphs[0]))},
    {"Stop", 7, 1, kAsciiClockStopGlyphs, (uint8_t)(sizeof(kAsciiClockStopGlyphs) / sizeof(kAsciiClockStopGlyphs[0]))},
    {"Swan", 9, 1, kAsciiClockSwanGlyphs, (uint8_t)(sizeof(kAsciiClockSwanGlyphs) / sizeof(kAsciiClockSwanGlyphs[0]))},
};
static constexpr int ASCII_CLOCK_FONT_COUNT = sizeof(kAsciiClockFonts) / sizeof(kAsciiClockFonts[0]);
static constexpr int DEFAULT_ASCII_CLOCK_FONT_INDEX = 0;
static const size_t kFishGlyphBuf = 28;

// Printable ASCII only (Font 2). Use ' instead of °, * instead of bullets, etc.
FishSpecies fishSpecies[] = {
    {/* Small Green */ "><>", RGB565(80, 200, 120)},                         // Emerald
    {/* Blue Dart */ ">)))'>", RGB565(0, 150, 255)},                         // Azure
    {/* Pink Bubble */ "oO0", TFT_PINK},                                      // Pink
    {/* Golden Emperor */ "><((( '>", RGB565(255, 184, 0)},                   // Amber
    {/* Purple Jellyfish */ "~~{o}", TFT_VIOLET},                              // Violet
    {/* Red Snapper */ "><(((o>", TFT_RED},                                   // Red
    {/* Orange Wrasse */ "><((((>`", TFT_ORANGE},
    {/* Teal Glider */ "><((( '>", RGB565(0, 180, 170)},
    {/* Royal Indigo */ "}>{{{{* >", RGB565(75, 0, 156)},                    // Indigo
    {/* Lilac Starfish */ "><((( *>", RGB565(200, 120, 255)},
    {/* Pink Tetra */ ">(')>", RGB565(255, 158, 200)},
    {/* Yellow Minnow */ ">'>", TFT_YELLOW},
};
static constexpr int GLYPH_COUNT = sizeof(fishSpecies) / sizeof(fishSpecies[0]);

static char fishMirroredLeft[GLYPH_COUNT][kFishGlyphBuf];
static uint8_t fishGlyphLenRight[GLYPH_COUNT];
static uint8_t fishGlyphLenLeft[GLYPH_COUNT];
static int16_t fishGlyphWidthRight[GLYPH_COUNT];
static int16_t fishGlyphWidthLeft[GLYPH_COUNT];
static int16_t fishGlyphOffsetRight[GLYPH_COUNT][kFishGlyphBuf];
static int16_t fishGlyphOffsetLeft[GLYPH_COUNT][kFishGlyphBuf];

static char mirrorAsciiBracket(char c) {
  switch (c) {
    case '>':
      return '<';
    case '<':
      return '>';
    case '(':
      return ')';
    case ')':
      return '(';
    case '{':
      return '}';
    case '}':
      return '{';
    case '[':
      return ']';
    case ']':
      return '[';
    default:
      return c;
  }
}

static bool buildMirroredGlyph(const char* right, char* leftOut, size_t outCap) {
  size_t n = strlen(right);
  if (n == 0 || n + 1 > outCap) return false;
  for (size_t i = 0; i < n; ++i) {
    unsigned char u = static_cast<unsigned char>(right[i]);
    if (u < 32 || u > 126) return false;
    leftOut[i] = mirrorAsciiBracket(right[n - 1 - i]);
  }
  leftOut[n] = '\0';
  return true;
}

static void initFishMirrors() {
  for (int i = 0; i < GLYPH_COUNT; ++i) {
    if (!buildMirroredGlyph(fishSpecies[i].right, fishMirroredLeft[i], kFishGlyphBuf)) {
      strncpy(fishMirroredLeft[i], fishSpecies[i].right, kFishGlyphBuf - 1);
      fishMirroredLeft[i][kFishGlyphBuf - 1] = '\0';
    }
  }
}

inline const char* fishGlyphDrawing(const Fish& f) {
  return (f.vx >= 0.0f) ? fishSpecies[f.type].right : fishMirroredLeft[f.type];
}

// Occasional non-canonical hue for variety (~1 in 5 fish)
static const uint16_t kAltFishColors[] = {
    TFT_CYAN, TFT_MAGENTA, TFT_WHITE, TFT_SKYBLUE, TFT_GOLD,
    TFT_ORANGE, TFT_GREENYELLOW, TFT_DARKGREY};
static const int kAltFishColorCount = sizeof(kAltFishColors) / sizeof(kAltFishColors[0]);

// ------------------------------ Globals --------------------------------------
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite canvas = TFT_eSprite(&tft);
TFT_eSprite clockFlipSprite = TFT_eSprite(&tft);
#if !defined(AQUARIUM_BOARD_ST7796U35) && !defined(AQUARIUM_BOARD_PANCAKE)
SPIClass touchSPI(HSPI);
XPT2046_Touchscreen touch(TOUCH_CS_PIN, TOUCH_IRQ_PIN);
#endif
#if defined(AQUARIUM_BOARD_PANCAKE)
// TFT + SD share the ESP32-C5 FSPI bus; wrap the default SPI instance for SD.
SPIClass sdSPI(SPI);
#else
SPIClass sdSPI(VSPI);
#endif
Preferences prefs;

Fish fishPool[MAX_FISH_POOL];
Flake flakes[MAX_FLAKES];
Bubble bubbles[MAX_BUBBLES];
Octopus octopus;
Seahorse seahorse;
Snail snail;
Jellyfish jellyfish;
Squid squid;

int fishTargetCount = DEFAULT_FISH;
int bubbleTargetCount = DEFAULT_BUBBLES;
int octopusFrequency = DEFAULT_OCTOPUS_FREQUENCY;
int seahorseFrequency = DEFAULT_SEAHORSE_FREQUENCY;
int autoFeedFrequency = DEFAULT_AUTO_FEED_FREQUENCY;
int snailFrequency = DEFAULT_SNAIL_FREQUENCY;
int jellyfishFrequency = DEFAULT_JELLYFISH_FREQUENCY;
unsigned long cthulhuFeedPelletCount = 0;
unsigned long nextAutoFeedMs = 0;
bool autoFeedSprinkleActive = false;
bool autoFeedSprinkleLeftToRight = true;
int autoFeedSprinkleDropped = 0;
unsigned long autoFeedSprinkleNextMs = 0;
unsigned long autoFeedSprinkleIntervalMs = 220UL;
float seaweedSwaySpeed = DEFAULT_SWAY;
float seaweedLength = DEFAULT_SEAWEED_LENGTH;
float seaweedLengthRandomness = DEFAULT_SEAWEED_LENGTH_RANDOMNESS;
bool hudVisible = false;  // HUD + corner H/S hidden until top-left toggle
bool settingsOpen = false;
bool backlightPanelOpen = false;
bool lightColorModePanelOpen = false;
bool lightSchedulePanelOpen = false;
bool eventsPanelOpen = false;
bool creaturesPanelOpen = false;
SettingsTab activeSettingsTab = SETTINGS_TAB_TANK;
BackgroundStyle backgroundStyle = DEFAULT_BACKGROUND_STYLE;
uint16_t backgroundGradientColor = DEFAULT_BACKGROUND_GRADIENT_COLOR;
bool backgroundRainbowEnabled = false;
uint16_t backgroundRainbowColor = DEFAULT_BACKGROUND_GRADIENT_COLOR;
bool autoSkyEnabled = false;
uint16_t autoSkySunriseColor = DEFAULT_AUTO_SKY_SUNRISE_COLOR;
uint16_t autoSkyDayColor = DEFAULT_AUTO_SKY_DAY_COLOR;
uint16_t autoSkySunsetColor = DEFAULT_AUTO_SKY_SUNSET_COLOR;
uint16_t autoSkyNightColor = DEFAULT_AUTO_SKY_NIGHT_COLOR;
uint16_t autoSkyGradientColor = DEFAULT_AUTO_SKY_DAY_COLOR;
unsigned long lastBackgroundRainbowUpdateMs = 0;
bool displayFlip180 = false;
int lcdBacklightBrightness = DEFAULT_LCD_BACKLIGHT_BRIGHTNESS;
bool ambientLightEnabled = false;
int ambientLightBrightness = DEFAULT_AMBIENT_BRIGHTNESS;
bool ambientLightLinkedToBackground = true;
uint16_t ambientLightColor = DEFAULT_BACKGROUND_GRADIENT_COLOR;
bool ambientLightRainbowEnabled = false;
bool lightScheduleEnabled = DEFAULT_LIGHT_SCHEDULE_ENABLED;
int lightScheduleStartHour = DEFAULT_LIGHT_SCHEDULE_START_HOUR;
int lightScheduleEndHour = DEFAULT_LIGHT_SCHEDULE_END_HOUR;
int lightScheduleDimMinutes = DEFAULT_LIGHT_SCHEDULE_DIM_MINUTES;
LightScheduleDimmingMode lightScheduleDimmingMode = DEFAULT_LIGHT_SCHEDULE_DIM_MODE;
bool lcdBacklightPwmReady = false;
bool ambientGreenPwmReady = false;
bool ambientBluePwmReady = false;
bool ambientRedPwmReady = false;

bool clockVisible = false;
bool clockUse24Hour = false;
bool clockUseInternetTime = false;
ClockDisplayStyle clockDisplayStyle = CLOCK_STYLE_SMALL_TEXT;
ClockSmallPosition clockSmallPosition = CLOCK_SMALL_TOP;
int asciiClockFontIndex = DEFAULT_ASCII_CLOCK_FONT_INDEX;
// Clock size, driven by pinch-to-zoom and persisted to NVS. Zoom scales only the
// clock, nothing else. Notch 0 is the original (smallest) size; the top notch is
// the biggest allowed (2x the original, which used to be a single size step), with
// finer notches evenly filling the range between. Scaling is fractional, so it is
// applied by nearest-neighbour blit rather than setTextSize (which is integer-only).
static const uint8_t CLOCK_ZOOM_NOTCHES = 5;      // 1.00, 1.25, 1.50, 1.75, 2.00
static const float CLOCK_ZOOM_MIN_SCALE = 1.0f;   // notch 0 = original
static const float CLOCK_ZOOM_MAX_SCALE = 2.0f;   // top notch = biggest allowed
uint8_t clockZoomNotch = 0;
float clockZoomScale() {
  return CLOCK_ZOOM_MIN_SCALE +
         (CLOCK_ZOOM_MAX_SCALE - CLOCK_ZOOM_MIN_SCALE) * clockZoomNotch / (CLOCK_ZOOM_NOTCHES - 1);
}
#if defined(AQUARIUM_BOARD_PANCAKE)
bool clockPinchActive = false;       // two fingers currently down
bool clockPinchSuppressTap = false;  // swallow taps until all fingers lift after a pinch
float clockPinchStartDist = 1.0f;    // finger spread when the pinch began
float clockPinchStartScale = 1.0f;   // clock scale when the pinch began
#endif
bool clockStylePanelOpen = false;
bool clockColorPanelOpen = false;
bool backgroundColorPanelOpen = false;
bool autoSkyPanelOpen = false;
bool ambientColorPanelOpen = false;
bool clockFlipHorizontal = false;
AutoSkySlot activeAutoSkyColorSlot = AUTO_SKY_SLOT_NONE;
uint16_t clockSmallTextColor = DEFAULT_SMALL_CLOCK_COLOR;
uint16_t clockAsciiTextColor = DEFAULT_ASCII_CLOCK_COLOR;
bool clockSmallRainbowEnabled = false;
bool clockAsciiRainbowEnabled = false;
bool clockFlipSpriteReady = false;
int timezoneIndex = DEFAULT_TIMEZONE_INDEX;
int clockYear = DEFAULT_CLOCK_YEAR;
int clockMonth = DEFAULT_CLOCK_MONTH;
int clockDay = DEFAULT_CLOCK_DAY;
int clockHour = DEFAULT_CLOCK_HOUR;
int clockMinute = DEFAULT_CLOCK_MINUTE;
unsigned long clockLastMinuteMs = 0;
ClockField activeClockField = CLOCK_FIELD_HOUR;
bool rtcPresent = false;   // DS3231 detected on the I2C bus (optional hardware)

bool wifiPanelOpen = false;
WifiPanelMode wifiPanelMode = WIFI_PANEL_MAIN;
KeyboardMode keyboardMode = KEYBOARD_LOWER;
bool wifiEnabled = false;
bool wifiRadioStarted = false;
bool wifiScanInProgress = false;
bool wifiConnecting = false;
bool wifiConnected = false;
bool wifiConnectionFailed = false;
bool wifiSavePendingCredentials = false;
bool wifiTimeConfigured = false;
bool wifiTimeSynced = false;
unsigned long lastWifiServiceMs = 0;
char wifiStatusText[40] = "Off";
char wifiSsid[WIFI_SSID_MAX_LEN + 1] = "";
char wifiPass[WIFI_PASS_MAX_LEN + 1] = "";
char pendingWifiSsid[WIFI_SSID_MAX_LEN + 1] = "";
char pendingWifiPass[WIFI_PASS_MAX_LEN + 1] = "";
char wifiPasswordBuffer[WIFI_PASS_MAX_LEN + 1] = "";
char wifiNetworkNames[MAX_WIFI_NETWORKS][WIFI_SSID_MAX_LEN + 1];
int wifiNetworkRssi[MAX_WIFI_NETWORKS];
bool wifiNetworkOpen[MAX_WIFI_NETWORKS];
uint8_t wifiNetworkBssid[MAX_WIFI_NETWORKS][6];  // BSSID of the strongest AP per SSID
uint8_t wifiNetworkChannel[MAX_WIFI_NETWORKS];   // its channel (encodes the band: >14 = 5 GHz)
int wifiNetworkCount = 0;
int wifiNetworkPage = 0;
// AP the user picked in the list — carried through the password prompt to connect.
uint8_t pendingWifiBssid[6];
uint8_t pendingWifiChannel = 0;
bool pendingWifiHasBssid = false;
// AP we actually associated with, remembered so reconnects target the same
// BSSID/band instead of re-guessing on a combined 2.4/5 GHz SSID.
char lastGoodSsid[WIFI_SSID_MAX_LEN + 1] = "";
uint8_t lastGoodBssid[6];
uint8_t lastGoodChannel = 0;
bool lastGoodValid = false;
unsigned long wifiConnectStartMs = 0;
unsigned long wifiLastReconnectMs = 0;
unsigned long wifiLastNtpAttemptMs = 0;
unsigned long wifiLastNtpSyncMs = 0;

bool bootButtonLastReading = false;
bool bootButtonStablePressed = false;
unsigned long bootButtonLastChangeMs = 0;

bool capturePanelOpen = false;
bool captureSdReady = false;
bool captureIndexReady = false;
bool captureSinglePending = false;
bool captureSequenceEnabled = false;
unsigned long captureNextImageIndex = 1;
unsigned long captureNextSequenceIndex = 1;
unsigned long captureStatusMs = 0;
unsigned long captureToastUntilMs = 0;
char captureStatusText[48] = "SD not checked";
char captureLastFile[40] = "";
char captureSequenceDir[24] = "/AQSEQ";
uint8_t bmpRowBuffer[SCREEN_W * 3];
unsigned long respawnButtonFlashUntilMs = 0;
unsigned long seahorseButtonFlashUntilMs = 0;
unsigned long octopusButtonFlashUntilMs = 0;
unsigned long snailButtonFlashUntilMs = 0;
unsigned long cthulhuButtonFlashUntilMs = 0;
unsigned long jellyfishButtonFlashUntilMs = 0;
unsigned long squidButtonFlashUntilMs = 0;

unsigned long lastMs = 0;
// Animation clock. During sequence capture it advances one video frame per saved BMP,
// so slow SD writes do not create skipped motion in the exported image sequence.
unsigned long aquariumNowMs = 0;
unsigned long fpsTimer = 0;
unsigned long frameCount = 0;
float fps = 0.0f;

unsigned long lastTouchMs = 0;
#if defined(AQUARIUM_BOARD_PANCAKE)
const unsigned long TOUCH_DEBOUNCE_MS = 45;   // FT6336 cap touch is clean — keep it snappy
#else
const unsigned long TOUCH_DEBOUNCE_MS = 160;  // resistive panels need more debounce
#endif
static const unsigned long HUD_BUTTON_FLASH_MS = 180;
bool touchWasDown = false;
bool spriteReady = false;
bool touchReady = false;
int mainCanvasActualColorDepth = 0;
int mainCanvasRenderHeight = SCREEN_H;
bool stripRenderActive = false;
int stripRenderY = 0;
uint16_t* gradientBandCache = nullptr;
BackgroundStyle gradientBandCacheStyle = BACKGROUND_STYLE_COUNT;
uint16_t gradientBandCacheColor = 0;
bool settingsDirty = false;
unsigned long settingsDirtyMs = 0;
unsigned long lastSettingsSaveMs = 0;
unsigned long lastLightScheduleServiceMs = 0;
unsigned long lightScheduleWakeUntilMs = 0;
static const unsigned long SETTINGS_SAVE_DELAY_MS = 1200UL;
static const unsigned long CLOCK_AUTOSAVE_INTERVAL_MS = 300000UL;

void beginInternetTimeSync();

void cacheGlyphMetrics(const char* txt, uint8_t& lenOut, int16_t& widthOut, int16_t offsetsOut[]) {
  char prefix[kFishGlyphBuf];
  size_t len = strlen(txt);
  if (len >= kFishGlyphBuf) len = kFishGlyphBuf - 1;

  for (size_t c = 0; c < len; ++c) {
    memcpy(prefix, txt, c);
    prefix[c] = '\0';
    offsetsOut[c] = (int16_t)tft.textWidth(prefix);
  }

  lenOut = (uint8_t)len;
  widthOut = (int16_t)tft.textWidth(txt);
}

void initFishGlyphMetrics() {
  for (int i = 0; i < GLYPH_COUNT; ++i) {
    cacheGlyphMetrics(fishSpecies[i].right, fishGlyphLenRight[i], fishGlyphWidthRight[i], fishGlyphOffsetRight[i]);
    cacheGlyphMetrics(fishMirroredLeft[i], fishGlyphLenLeft[i], fishGlyphWidthLeft[i], fishGlyphOffsetLeft[i]);
  }
}

// ------------------------------ Utility --------------------------------------
template <typename T>
T clampVal(T v, T lo, T hi) {
  return (v < lo) ? lo : ((v > hi) ? hi : v);
}

void copySafe(char* out, size_t outCap, const char* src) {
  if (outCap == 0) return;
  if (!src) src = "";
  strncpy(out, src, outCap - 1);
  out[outCap - 1] = '\0';
}

void appendCharSafe(char* out, size_t outCap, char c) {
  size_t len = strlen(out);
  if (len + 1 >= outCap) return;
  out[len] = c;
  out[len + 1] = '\0';
}

void removeLastChar(char* out) {
  size_t len = strlen(out);
  if (len > 0) out[len - 1] = '\0';
}

void formatShortText(char* out, size_t outCap, const char* src, size_t maxChars) {
  if (outCap == 0) return;
  if (!src) src = "";
  if (outCap < 4) {
    copySafe(out, outCap, src);
    return;
  }
  size_t len = strlen(src);
  if (len <= maxChars || maxChars < 4) {
    copySafe(out, outCap, src);
    return;
  }

  size_t copyLen = maxChars - 3;
  if (copyLen > outCap - 4) copyLen = outCap - 4;
  strncpy(out, src, copyLen);
  out[copyLen] = '\0';
  strncat(out, "...", outCap - strlen(out) - 1);
}

void markSettingsDirty() {
  settingsDirty = true;
  settingsDirtyMs = millis();
}

int displayRotation() {
  return displayFlip180 ? 3 : 1;
}

void applyTouchOrientation() {
#if !defined(AQUARIUM_BOARD_ST7796U35) && !defined(AQUARIUM_BOARD_PANCAKE)
  if (touchReady) touch.setRotation(displayRotation());
#endif
  // ST7796U resistive and Pancake FT6336 both derive orientation inside
  // readTouchPoint(), so there is no controller rotation register to set here.
}

void applyDisplayOrientation(bool clearScreen) {
  tft.setRotation(displayRotation());
  applyTouchOrientation();
  if (clearScreen) tft.fillScreen(BG_COLOR);
}

void setDisplayFlip180(bool flipped) {
  if (displayFlip180 == flipped) return;
  displayFlip180 = flipped;
  applyDisplayOrientation(true);
  markSettingsDirty();
}

void invalidateBackgroundGradientCache() {
  gradientBandCacheStyle = BACKGROUND_STYLE_COUNT;
}

void releaseGradientBandCache() {
  if (gradientBandCache == nullptr) return;
  free(gradientBandCache);
  gradientBandCache = nullptr;
  invalidateBackgroundGradientCache();
}

float aquariumTimeSec() {
  return aquariumNowMs * 0.001f;
}

static inline float wrappedSinf(float radians) {
  static const float TWO_PI_F = 6.28318530718f;
  // Keep long-running wave animations fast: large sine inputs get slower on ESP32 math libs.
  if (radians > TWO_PI_F || radians < -TWO_PI_F) {
    radians = fmodf(radians, TWO_PI_F);
  }
  return sinf(radians);
}

void setCaptureStatus(const char* status, bool toast = false) {
  copySafe(captureStatusText, sizeof(captureStatusText), status);
  captureStatusMs = millis();
  if (toast) captureToastUntilMs = captureStatusMs + 2400UL;
}

#if defined(AQUARIUM_BOARD_PANCAKE)
// On the Pancake the TFT and SD share one FSPI bus. There is nothing to re-mux:
// each library drives its own chip-select, and TFT_eSPI re-applies its SPI
// settings on every transaction, so we only manage the CS lines here.
static bool sdSharedBusBegun = false;

void beginCaptureSdBus() {
  pinMode(AQUARIUM_TFT_CS_PIN, OUTPUT);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(AQUARIUM_TFT_CS_PIN, HIGH);  // park the TFT while SD talks
  digitalWrite(SD_CS_PIN, HIGH);
  if (!sdSharedBusBegun) {
    sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN);
    sdSharedBusBegun = true;
  }
}

void restoreTftBus() {
  digitalWrite(SD_CS_PIN, HIGH);            // release SD; TFT_eSPI reselects itself
  digitalWrite(AQUARIUM_TFT_CS_PIN, HIGH);
}
#else
void beginCaptureSdBus() {
  pinMode(AQUARIUM_TFT_CS_PIN, OUTPUT);
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(AQUARIUM_TFT_CS_PIN, HIGH);
  digitalWrite(SD_CS_PIN, HIGH);
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN);
  sdSPI.setFrequency(SD_SPI_FREQUENCY);
}

void restoreTftBus() {
  digitalWrite(SD_CS_PIN, HIGH);
  digitalWrite(AQUARIUM_TFT_CS_PIN, HIGH);
  SPIClass& tftSpi = TFT_eSPI::getSPIinstance();
  tftSpi.begin(AQUARIUM_TFT_SCK_PIN, AQUARIUM_TFT_MISO_PIN, AQUARIUM_TFT_MOSI_PIN, -1);
  tftSpi.setFrequency(TFT_SPI_FREQUENCY);
}
#endif

void writeLE16(File& file, uint16_t value) {
  uint8_t bytes[2] = {(uint8_t)(value & 0xFF), (uint8_t)(value >> 8)};
  file.write(bytes, sizeof(bytes));
}

void writeLE32(File& file, uint32_t value) {
  uint8_t bytes[4] = {
      (uint8_t)(value & 0xFF),
      (uint8_t)((value >> 8) & 0xFF),
      (uint8_t)((value >> 16) & 0xFF),
      (uint8_t)((value >> 24) & 0xFF)};
  file.write(bytes, sizeof(bytes));
}

void writeBmpHeader(File& file) {
  const uint32_t rowSize = SCREEN_W * 3UL;
  const uint32_t pixelBytes = rowSize * SCREEN_H;
  const uint32_t fileSize = 54UL + pixelBytes;

  file.write((const uint8_t*)"BM", 2);
  writeLE32(file, fileSize);
  writeLE16(file, 0);
  writeLE16(file, 0);
  writeLE32(file, 54);
  writeLE32(file, 40);
  writeLE32(file, SCREEN_W);
  writeLE32(file, SCREEN_H);
  writeLE16(file, 1);
  writeLE16(file, 24);
  writeLE32(file, 0);
  writeLE32(file, pixelBytes);
  writeLE32(file, 2835);
  writeLE32(file, 2835);
  writeLE32(file, 0);
  writeLE32(file, 0);
}

bool writeBmpSpriteRow(File& file, TFT_eSprite& s, int localY) {
  const uint32_t rowSize = SCREEN_W * 3UL;
  for (int x = 0; x < SCREEN_W; ++x) {
    uint16_t color = s.readPixel(x, localY);
    int o = x * 3;
    bmpRowBuffer[o + 0] = (uint8_t)(((color & 0x1F) * 255) / 31);
    bmpRowBuffer[o + 1] = (uint8_t)((((color >> 5) & 0x3F) * 255) / 63);
    bmpRowBuffer[o + 2] = (uint8_t)((((color >> 11) & 0x1F) * 255) / 31);
  }
  return file.write(bmpRowBuffer, rowSize) == rowSize;
}

void capturePath(char* out, size_t outCap, bool sequence, unsigned long index) {
  if (sequence) {
    snprintf(out, outCap, "%s/SEQ_%06lu.BMP", captureSequenceDir, index);
  } else {
    snprintf(out, outCap, "%s/SCR_%06lu.BMP", CAPTURE_DIR, index);
  }
}

unsigned long findNextCaptureIndex(bool sequence, unsigned long startIndex) {
  char path[40];
  unsigned long limit = sequence ? 999999UL : 9999UL;
  unsigned long index = clampVal(startIndex, 1UL, limit);
  while (index <= limit) {
    capturePath(path, sizeof(path), sequence, index);
    if (!SD.exists(path)) return index;
    index++;
  }
  return 1UL;
}

bool ensureCaptureSdReady() {
  beginCaptureSdBus();

  bool justMounted = false;
  if (!captureSdReady) {
    SD.end();
    delay(12);
    captureSdReady = SD.begin(SD_CS_PIN, sdSPI, SD_SPI_FREQUENCY);
    justMounted = captureSdReady;
  }

  if (!captureSdReady) {
    captureSdReady = false;
    captureIndexReady = false;
    captureSequenceEnabled = false;
    capturePanelOpen = true;
    setCaptureStatus("SD begin failed");
    return false;
  }

  if (justMounted && SD.cardType() == CARD_NONE) {
    SD.end();
    captureSdReady = false;
    captureIndexReady = false;
    captureSequenceEnabled = false;
    capturePanelOpen = true;
    setCaptureStatus("No card");
    return false;
  }

  SD.mkdir(CAPTURE_DIR);
  SD.mkdir(SEQUENCE_DIR);
  if (!captureIndexReady) {
    // Avoid scanning old sequence frames here. A card with thousands of BMPs in
    // /AQSEQ can otherwise look frozen while Check probes every old filename.
    captureNextImageIndex = clampVal(captureNextImageIndex, 1UL, 9999UL);
    captureNextSequenceIndex = 1;
    captureIndexReady = true;
  }
  if (!captureSequenceEnabled) {
    unsigned long cardMb = (unsigned long)(SD.cardSize() / (1024ULL * 1024ULL));
    char status[48];
    snprintf(status, sizeof(status), "SD OK %luMB", cardMb);
    setCaptureStatus(status);
  }
  return true;
}

bool checkCaptureSdReady() {
  captureSdReady = false;
  captureIndexReady = false;
  bool ready = ensureCaptureSdReady();
  restoreTftBus();
  return ready;
}

void freeRenderBuffersForCaptureSd(bool resetCaptureState = true) {
  canvas.deleteSprite();
  spriteReady = false;
  stripRenderActive = false;
  stripRenderY = 0;
  mainCanvasActualColorDepth = 0;
  mainCanvasRenderHeight = 0;
  releaseGradientBandCache();
  SD.end();
  captureSdReady = false;
  if (resetCaptureState) {
    captureIndexReady = false;
    captureSequenceEnabled = false;
  }
  delay(1);
}

bool checkCaptureSdReadyLowMemory() {
  setCaptureStatus("Checking SD...");
  captureSequenceEnabled = false;
  freeRenderBuffersForCaptureSd();

  bool ready = ensureCaptureSdReady();
  endCaptureSdSession();
  restoreMainCanvasAfterSlowCapture();
  return ready;
}

bool beginCaptureSequenceSession() {
  beginCaptureSdBus();
  for (int attempt = 0; attempt < 8; ++attempt) {
    unsigned long sessionId = (unsigned long)esp_random();
    if (sessionId == 0) sessionId = millis();
    snprintf(captureSequenceDir, sizeof(captureSequenceDir), "%s/S%08lX", SEQUENCE_DIR, sessionId);
    if (SD.mkdir(captureSequenceDir)) {
      captureNextSequenceIndex = 1;
      restoreTftBus();
      return true;
    }
  }

  copySafe(captureSequenceDir, sizeof(captureSequenceDir), SEQUENCE_DIR);
  setCaptureStatus("Seq dir failed");
  restoreTftBus();
  return false;
}

bool beginCaptureSequenceSessionLowMemory() {
  setCaptureStatus("Starting seq...");
  freeRenderBuffersForCaptureSd();

  bool ready = ensureCaptureSdReady();
  bool started = ready && beginCaptureSequenceSession();
  endCaptureSdSession();
  restoreMainCanvasAfterSlowCapture();
  return started;
}

bool saveBmpToSd(TFT_eSprite& s, const char* path) {
  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    setCaptureStatus("File open failed");
    captureSequenceEnabled = false;
    capturePanelOpen = true;
    return false;
  }

  writeBmpHeader(file);

  for (int y = SCREEN_H - 1; y >= 0; --y) {
    if (!writeBmpSpriteRow(file, s, y)) {
      file.close();
      setCaptureStatus("Write failed");
      captureSequenceEnabled = false;
      capturePanelOpen = true;
      return false;
    }
  }

  file.close();
  return true;
}

bool saveNextCaptureFrame(TFT_eSprite& s, bool sequence, bool showToast) {
  if (!ensureCaptureSdReady()) {
    restoreTftBus();
    return false;
  }

  char path[40];
  unsigned long& index = sequence ? captureNextSequenceIndex : captureNextImageIndex;
  if (!sequence) index = findNextCaptureIndex(false, index);
  capturePath(path, sizeof(path), sequence, index);

  if (!saveBmpToSd(s, path)) {
    restoreTftBus();
    return false;
  }

  index++;
  copySafe(captureLastFile, sizeof(captureLastFile), path);
  char status[48];
  snprintf(status, sizeof(status), "Saved %s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
  setCaptureStatus(status, showToast);
  restoreTftBus();
  return true;
}

void requestSingleCapture() {
  captureSinglePending = true;
  setCaptureStatus("Saving BMP...");
}

void stopCaptureRecording(bool showToast) {
  if (!captureSequenceEnabled) return;
  captureSequenceEnabled = false;
  setCaptureStatus("Recording stopped", showToast);
}

void setCaptureSequenceEnabled(bool enabled) {
  if (!enabled) {
    stopCaptureRecording(false);
    return;
  }

  if (beginCaptureSequenceSessionLowMemory()) {
    captureSequenceEnabled = true;
    capturePanelOpen = false;
    settingsOpen = false;
    wifiPanelOpen = false;
    hudVisible = false;
    captureToastUntilMs = 0;
    setCaptureStatus("Recording frames");
  }
}

float frand(float a, float b) {
  return a + (b - a) * (float)random(0, 10000) / 9999.0f;
}

int fishVisualWidth(const Fish& f) {
  if (f.visualWidth > 0) return f.visualWidth;
  return (int)strlen(fishGlyphDrawing(f)) * 12;  // Font 2 fallback before metrics init
}

inline int activeFishLimit() {
  return clampVal(fishTargetCount, 0, MAX_FISH_POOL);
}

inline int activeBubbleLimit() {
  return clampVal(bubbleTargetCount, 0, MAX_BUBBLES);
}

inline bool fishAvoidanceEnabled() {
  return true;
}

int normalizeOctopusFrequency(int value) {
  int best = OCTOPUS_FREQUENCY_OPTIONS[0];
  int bestDiff = abs(value - best);
  for (int i = 1; i < OCTOPUS_FREQUENCY_OPTION_COUNT; ++i) {
    int diff = abs(value - OCTOPUS_FREQUENCY_OPTIONS[i]);
    if (diff < bestDiff) {
      best = OCTOPUS_FREQUENCY_OPTIONS[i];
      bestDiff = diff;
    }
  }
  return best;
}

int normalizeSeahorseFrequency(int value) {
  int best = SEAHORSE_FREQUENCY_OPTIONS[0];
  int bestDiff = abs(value - best);
  for (int i = 1; i < SEAHORSE_FREQUENCY_OPTION_COUNT; ++i) {
    int diff = abs(value - SEAHORSE_FREQUENCY_OPTIONS[i]);
    if (diff < bestDiff) {
      best = SEAHORSE_FREQUENCY_OPTIONS[i];
      bestDiff = diff;
    }
  }
  return best;
}

int normalizeAutoFeedFrequency(int value) {
  int best = AUTO_FEED_FREQUENCY_OPTIONS[0];
  int bestDiff = abs(value - best);
  for (int i = 1; i < AUTO_FEED_FREQUENCY_OPTION_COUNT; ++i) {
    int diff = abs(value - AUTO_FEED_FREQUENCY_OPTIONS[i]);
    if (diff < bestDiff) {
      best = AUTO_FEED_FREQUENCY_OPTIONS[i];
      bestDiff = diff;
    }
  }
  return best;
}

int normalizeCreatureFrequency(int value) {
  int best = CREATURE_FREQUENCY_OPTIONS[0];
  int bestDiff = abs(value - best);
  for (int i = 1; i < CREATURE_FREQUENCY_OPTION_COUNT; ++i) {
    int diff = abs(value - CREATURE_FREQUENCY_OPTIONS[i]);
    if (diff < bestDiff) {
      best = CREATURE_FREQUENCY_OPTIONS[i];
      bestDiff = diff;
    }
  }
  return best;
}

void cycleOctopusFrequency(int delta) {
  int current = normalizeOctopusFrequency(octopusFrequency);
  int index = 0;
  for (int i = 0; i < OCTOPUS_FREQUENCY_OPTION_COUNT; ++i) {
    if (OCTOPUS_FREQUENCY_OPTIONS[i] == current) {
      index = i;
      break;
    }
  }

  index += delta;
  if (index < 0) index = OCTOPUS_FREQUENCY_OPTION_COUNT - 1;
  if (index >= OCTOPUS_FREQUENCY_OPTION_COUNT) index = 0;
  octopusFrequency = OCTOPUS_FREQUENCY_OPTIONS[index];
  if (!octopus.active) octopus.nextSpawnMs = 0;
  markSettingsDirty();
}

void cycleSeahorseFrequency(int delta) {
  int current = normalizeSeahorseFrequency(seahorseFrequency);
  int index = 0;
  for (int i = 0; i < SEAHORSE_FREQUENCY_OPTION_COUNT; ++i) {
    if (SEAHORSE_FREQUENCY_OPTIONS[i] == current) {
      index = i;
      break;
    }
  }

  index += delta;
  if (index < 0) index = SEAHORSE_FREQUENCY_OPTION_COUNT - 1;
  if (index >= SEAHORSE_FREQUENCY_OPTION_COUNT) index = 0;
  seahorseFrequency = SEAHORSE_FREQUENCY_OPTIONS[index];
  if (!seahorse.active) seahorse.nextSpawnMs = 0;
  markSettingsDirty();
}

void cycleAutoFeedFrequency(int delta) {
  int current = normalizeAutoFeedFrequency(autoFeedFrequency);
  int index = 0;
  for (int i = 0; i < AUTO_FEED_FREQUENCY_OPTION_COUNT; ++i) {
    if (AUTO_FEED_FREQUENCY_OPTIONS[i] == current) {
      index = i;
      break;
    }
  }

  index += delta;
  if (index < 0) index = AUTO_FEED_FREQUENCY_OPTION_COUNT - 1;
  if (index >= AUTO_FEED_FREQUENCY_OPTION_COUNT) index = 0;
  autoFeedFrequency = AUTO_FEED_FREQUENCY_OPTIONS[index];
  if (autoFeedFrequency <= 0) {
    nextAutoFeedMs = 0;
    autoFeedSprinkleActive = false;
  } else {
    nextAutoFeedMs = aquariumNowMs + AUTO_FEED_FIRST_DROP_MS;
    autoFeedSprinkleActive = false;
  }
  markSettingsDirty();
}

void cycleSnailFrequency(int delta) {
  int current = normalizeCreatureFrequency(snailFrequency);
  int index = 0;
  for (int i = 0; i < CREATURE_FREQUENCY_OPTION_COUNT; ++i) {
    if (CREATURE_FREQUENCY_OPTIONS[i] == current) {
      index = i;
      break;
    }
  }

  index += delta;
  if (index < 0) index = CREATURE_FREQUENCY_OPTION_COUNT - 1;
  if (index >= CREATURE_FREQUENCY_OPTION_COUNT) index = 0;
  snailFrequency = CREATURE_FREQUENCY_OPTIONS[index];
  if (!snail.active) snail.nextSpawnMs = 0;
  markSettingsDirty();
}

void cycleJellyfishFrequency(int delta) {
  int current = normalizeCreatureFrequency(jellyfishFrequency);
  int index = 0;
  for (int i = 0; i < CREATURE_FREQUENCY_OPTION_COUNT; ++i) {
    if (CREATURE_FREQUENCY_OPTIONS[i] == current) {
      index = i;
      break;
    }
  }

  index += delta;
  if (index < 0) index = CREATURE_FREQUENCY_OPTION_COUNT - 1;
  if (index >= CREATURE_FREQUENCY_OPTION_COUNT) index = 0;
  jellyfishFrequency = CREATURE_FREQUENCY_OPTIONS[index];
  if (!jellyfish.active) jellyfish.nextSpawnMs = 0;
  markSettingsDirty();
}

unsigned long octopusSpawnIntervalMs() {
  int frequency = normalizeOctopusFrequency(octopusFrequency);
  return 3600000UL / (unsigned long)frequency;
}

unsigned long seahorseSpawnIntervalMs() {
  int frequency = normalizeSeahorseFrequency(seahorseFrequency);
  return 3600000UL / (unsigned long)frequency;
}

unsigned long autoFeedIntervalMs() {
  int frequency = normalizeAutoFeedFrequency(autoFeedFrequency);
  if (frequency <= 0) return 0;
  return 3600000UL / (unsigned long)frequency;
}

unsigned long snailSpawnIntervalMs() {
  int frequency = normalizeCreatureFrequency(snailFrequency);
  if (frequency <= 0) return 0;
  return 3600000UL / (unsigned long)frequency;
}

unsigned long jellyfishSpawnIntervalMs() {
  int frequency = normalizeCreatureFrequency(jellyfishFrequency);
  if (frequency <= 0) return 0;
  return 3600000UL / (unsigned long)frequency;
}

inline uint8_t fishGlyphLength(const Fish& f) {
  return (f.vx >= 0.0f) ? fishGlyphLenRight[f.type] : fishGlyphLenLeft[f.type];
}

inline const int16_t* fishGlyphOffsets(const Fish& f) {
  return (f.vx >= 0.0f) ? fishGlyphOffsetRight[f.type] : fishGlyphOffsetLeft[f.type];
}

int screenMapX(int rawX) {
  rawX = clampVal(rawX, TOUCH_RAW_MIN_X, TOUCH_RAW_MAX_X);
  return map(rawX, TOUCH_RAW_MIN_X, TOUCH_RAW_MAX_X, 0, SCREEN_W - 1);
}

int screenMapY(int rawY) {
  rawY = clampVal(rawY, TOUCH_RAW_MIN_Y, TOUCH_RAW_MAX_Y);
  return map(rawY, TOUCH_RAW_MIN_Y, TOUCH_RAW_MAX_Y, 0, SCREEN_H - 1);
}

int physicalMapX(int rawX) {
  rawX = clampVal(rawX, TOUCH_RAW_MIN_X, TOUCH_RAW_MAX_X);
  return map(rawX, TOUCH_RAW_MIN_X, TOUCH_RAW_MAX_X, 0, PHYSICAL_SCREEN_W - 1);
}

int physicalMapY(int rawY) {
  rawY = clampVal(rawY, TOUCH_RAW_MIN_Y, TOUCH_RAW_MAX_Y);
  return map(rawY, TOUCH_RAW_MIN_Y, TOUCH_RAW_MAX_Y, 0, PHYSICAL_SCREEN_H - 1);
}

int16_t touchBestTwoAverage(int16_t a, int16_t b, int16_t c) {
  int16_t ab = abs(a - b);
  int16_t ac = abs(a - c);
  int16_t bc = abs(b - c);
  if (ab <= ac && ab <= bc) return (a + b) >> 1;
  if (ac <= ab && ac <= bc) return (a + c) >> 1;
  return (b + c) >> 1;
}

bool initTouchHardware() {
#if defined(AQUARIUM_BOARD_PANCAKE)
  return ft6336_init();   // FT6336 over I2C (SDA=9, SCL=10, RST=8)
#elif defined(AQUARIUM_BOARD_ST7796U35)
  pinMode(TOUCH_CS_PIN, OUTPUT);
  digitalWrite(TOUCH_CS_PIN, HIGH);
  pinMode(TOUCH_IRQ_PIN, INPUT);
  return true;
#else
  touchSPI.begin(TOUCH_CLK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN);
  return touch.begin(touchSPI);
#endif
}

#if defined(AQUARIUM_BOARD_ST7796U35)
bool readSharedSpiTouchPoint(int& sx, int& sy) {
  SPIClass& displaySpi = TFT_eSPI::getSPIinstance();
  int16_t samples[6];
  int z = 0;

  digitalWrite(AQUARIUM_TFT_CS_PIN, HIGH);
  displaySpi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
  digitalWrite(TOUCH_CS_PIN, LOW);

  displaySpi.transfer(0xB1);                         // Z1
  int16_t z1 = displaySpi.transfer16(0xC1) >> 3;     // Z2
  z = z1 + 4095;
  int16_t z2 = displaySpi.transfer16(0x91) >> 3;     // X
  z -= z2;

  if (z >= 400) {
    displaySpi.transfer16(0x91);                     // Dummy X, first read is noisy
    samples[0] = displaySpi.transfer16(0xD1) >> 3;   // Y
    samples[1] = displaySpi.transfer16(0x91) >> 3;   // X
    samples[2] = displaySpi.transfer16(0xD1) >> 3;   // Y
    samples[3] = displaySpi.transfer16(0x91) >> 3;   // X
  } else {
    samples[0] = samples[1] = samples[2] = samples[3] = 0;
  }
  samples[4] = displaySpi.transfer16(0xD0) >> 3;     // Last Y, power down
  samples[5] = displaySpi.transfer16(0) >> 3;

  digitalWrite(TOUCH_CS_PIN, HIGH);
  displaySpi.endTransaction();

  if (z < 400) return false;

  int rawX = touchBestTwoAverage(samples[0], samples[2], samples[4]);
  int rawY = touchBestTwoAverage(samples[1], samples[3], samples[5]);
  int physicalX = PHYSICAL_SCREEN_W - 1 - physicalMapX(rawX);
  int physicalY = physicalMapY(rawY);
  sx = physicalX - DISPLAY_OFFSET_X;
  sy = physicalY - DISPLAY_OFFSET_Y;
  return (sx >= 0 && sx < SCREEN_W && sy >= 0 && sy < SCREEN_H);
}
#endif

#if defined(AQUARIUM_BOARD_PANCAKE)
// If a tap registers mirrored on real hardware, flip one/both of these.
static const bool CAP_TOUCH_INVERT_X = false;  // long (480) landscape axis
static const bool CAP_TOUCH_INVERT_Y = false;  // short (320) landscape axis

bool readCapTouchPoint(int& sx, int& sy) {
  uint16_t rx, ry;
  if (!ft6336_read_raw(&rx, &ry)) return false;  // panel-native portrait: rx 0..319, ry 0..479

  // Rotate the portrait panel into the 480x320 landscape scene (TFT rotation 1).
  // The panel's native portrait width (raw x, 0..319) equals the landscape height.
  int lx = (int)ry;                 // 0..479  -> screen X (SCREEN_W)
  int ly = (SCREEN_H - 1) - (int)rx;  // 0..319 -> screen Y (SCREEN_H)
  if (displayFlip180) {                 // rotation 3 is 180deg from rotation 1
    lx = (SCREEN_W - 1) - lx;
    ly = (SCREEN_H - 1) - ly;
  }
  if (CAP_TOUCH_INVERT_X) lx = (SCREEN_W - 1) - lx;
  if (CAP_TOUCH_INVERT_Y) ly = (SCREEN_H - 1) - ly;

  sx = clampVal(lx, 0, SCREEN_W - 1);
  sy = clampVal(ly, 0, SCREEN_H - 1);
  return true;
}
#endif

bool readTouchPoint(int& sx, int& sy) {
  if (!touchReady) return false;
#if defined(AQUARIUM_BOARD_PANCAKE)
  return readCapTouchPoint(sx, sy);
#elif defined(AQUARIUM_BOARD_ST7796U35)
  return readSharedSpiTouchPoint(sx, sy);
#else
  if (!touch.touched()) return false;
  TS_Point p = touch.getPoint();
  sx = screenMapX(p.x);
  sy = screenMapY(p.y);
  return (sx >= 0 && sx < SCREEN_W && sy >= 0 && sy < SCREEN_H);
#endif
}

bool isLeapYear(int year) {
  return ((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0);
}

int daysInMonth(int year, int month) {
  static const uint8_t daysByMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  month = clampVal(month, 1, 12);
  if (month == 2 && isLeapYear(year)) return 29;
  return daysByMonth[month - 1];
}

void normalizeClockDate() {
  clockYear = clampVal(clockYear, CLOCK_MIN_YEAR, CLOCK_MAX_YEAR);
  clockMonth = clampVal(clockMonth, 1, 12);
  clockDay = clampVal(clockDay, 1, daysInMonth(clockYear, clockMonth));
  clockHour = clampVal(clockHour, 0, 23);
  clockMinute = clampVal(clockMinute, 0, 59);
}

void incrementClockDay() {
  clockDay++;
  if (clockDay > daysInMonth(clockYear, clockMonth)) {
    clockDay = 1;
    clockMonth++;
    if (clockMonth > 12) {
      clockMonth = 1;
      if (clockYear < CLOCK_MAX_YEAR) clockYear++;
    }
  }
}

void decrementClockDay() {
  clockDay--;
  if (clockDay < 1) {
    clockMonth--;
    if (clockMonth < 1) {
      clockMonth = 12;
      if (clockYear > CLOCK_MIN_YEAR) clockYear--;
    }
    clockDay = daysInMonth(clockYear, clockMonth);
  }
}

void addClockMinute(int delta) {
  if (delta > 0) {
    clockMinute++;
    if (clockMinute >= 60) {
      clockMinute = 0;
      clockHour++;
      if (clockHour >= 24) {
        clockHour = 0;
        incrementClockDay();
      }
    }
  } else if (delta < 0) {
    clockMinute--;
    if (clockMinute < 0) {
      clockMinute = 59;
      clockHour--;
      if (clockHour < 0) {
        clockHour = 23;
        decrementClockDay();
      }
    }
  }
}

void updateClock(unsigned long now) {
  while (now - clockLastMinuteMs >= 60000UL) {
    clockLastMinuteMs += 60000UL;
    addClockMinute(1);
  }
}

void resetClockTick() {
  clockLastMinuteMs = millis();
}

void selectClockField(int delta) {
  int next = (int)activeClockField + delta;
  if (next < 0) next = CLOCK_FIELD_COUNT - 1;
  if (next >= CLOCK_FIELD_COUNT) next = 0;
  activeClockField = (ClockField)next;
}

void adjustClockField(int delta) {
  switch (activeClockField) {
    case CLOCK_FIELD_YEAR:
      clockYear = clampVal(clockYear + delta, CLOCK_MIN_YEAR, CLOCK_MAX_YEAR);
      break;
    case CLOCK_FIELD_MONTH:
      clockMonth += delta;
      if (clockMonth < 1) clockMonth = 12;
      if (clockMonth > 12) clockMonth = 1;
      break;
    case CLOCK_FIELD_DAY:
      clockDay += delta;
      if (clockDay < 1) clockDay = daysInMonth(clockYear, clockMonth);
      if (clockDay > daysInMonth(clockYear, clockMonth)) clockDay = 1;
      break;
    case CLOCK_FIELD_HOUR:
      clockHour += delta;
      if (clockHour < 0) clockHour = 23;
      if (clockHour > 23) clockHour = 0;
      break;
    case CLOCK_FIELD_MINUTE:
      clockMinute += delta;
      if (clockMinute < 0) clockMinute = 59;
      if (clockMinute > 59) clockMinute = 0;
      break;
    default:
      break;
  }
  normalizeClockDate();
  resetClockTick();
  markSettingsDirty();
  if (rtcPresent) {
    // clockYear/etc. are wall-clock in the currently selected timezone;
    // mktime() reads a struct tm as local time per the active TZ env var
    // (kept current by applyConfiguredTimezone()) and returns UTC epoch.
    struct tm localTm = {};
    localTm.tm_year = clockYear - 1900;
    localTm.tm_mon = clockMonth - 1;
    localTm.tm_mday = clockDay;
    localTm.tm_hour = clockHour;
    localTm.tm_min = clockMinute;
    localTm.tm_sec = 0;
    localTm.tm_isdst = -1;
    ds3231_write_utc(mktime(&localTm));
  }
}

const char* clockFieldName() {
  switch (activeClockField) {
    case CLOCK_FIELD_YEAR:
      return "Year";
    case CLOCK_FIELD_MONTH:
      return "Month";
    case CLOCK_FIELD_DAY:
      return "Day";
    case CLOCK_FIELD_HOUR:
      return "Hour";
    case CLOCK_FIELD_MINUTE:
      return "Minute";
    default:
      return "Clock";
  }
}

void formatClockFieldValue(char* out, size_t outCap) {
  switch (activeClockField) {
    case CLOCK_FIELD_YEAR:
      snprintf(out, outCap, "%d", clockYear);
      break;
    case CLOCK_FIELD_MONTH:
      snprintf(out, outCap, "%02d", clockMonth);
      break;
    case CLOCK_FIELD_DAY:
      snprintf(out, outCap, "%02d", clockDay);
      break;
    case CLOCK_FIELD_HOUR:
      if (clockUse24Hour) {
        snprintf(out, outCap, "%02d", clockHour);
      } else {
        int h = clockHour % 12;
        if (h == 0) h = 12;
        snprintf(out, outCap, "%d %s", h, clockHour < 12 ? "AM" : "PM");
      }
      break;
    case CLOCK_FIELD_MINUTE:
      snprintf(out, outCap, "%02d", clockMinute);
      break;
    default:
      snprintf(out, outCap, "--");
      break;
  }
}

void formatClockDisplay(char* out, size_t outCap) {
  static const char* monthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (clockUse24Hour) {
    snprintf(out, outCap, "%s %02d  %02d:%02d", monthNames[clockMonth - 1], clockDay, clockHour, clockMinute);
  } else {
    int h = clockHour % 12;
    if (h == 0) h = 12;
    snprintf(out, outCap, "%s %02d  %d:%02d %s", monthNames[clockMonth - 1], clockDay, h, clockMinute,
             clockHour < 12 ? "AM" : "PM");
  }
}

void formatClockTimeOnly(char* out, size_t outCap, bool includeMeridiem) {
  if (clockUse24Hour) {
    snprintf(out, outCap, "%02d:%02d", clockHour, clockMinute);
  } else {
    int h = clockHour % 12;
    if (h == 0) h = 12;
    if (includeMeridiem) {
      snprintf(out, outCap, "%d:%02d %s", h, clockMinute, clockHour < 12 ? "am" : "pm");
    } else {
      snprintf(out, outCap, "%d:%02d", h, clockMinute);
    }
  }
}

const AsciiClockFont& currentAsciiClockFont() {
  asciiClockFontIndex = clampVal(asciiClockFontIndex, 0, ASCII_CLOCK_FONT_COUNT - 1);
  return kAsciiClockFonts[asciiClockFontIndex];
}

void adjustAsciiClockFont(int delta) {
  asciiClockFontIndex += delta;
  while (asciiClockFontIndex < 0) asciiClockFontIndex += ASCII_CLOCK_FONT_COUNT;
  while (asciiClockFontIndex >= ASCII_CLOCK_FONT_COUNT) asciiClockFontIndex -= ASCII_CLOCK_FONT_COUNT;
  markSettingsDirty();
}

const AsciiClockGlyph& asciiClockGlyphFor(const AsciiClockFont& font, char c) {
  for (int i = 0; i < font.glyphCount; ++i) {
    if (font.glyphs[i].c == c) return font.glyphs[i];
  }
  return font.glyphs[font.glyphCount - 1];  // Space
}

int asciiClockGlyphWidth(const AsciiClockFont& font, const AsciiClockGlyph& glyph) {
  int width = 1;
  for (int row = 0; row < font.rowCount; ++row) {
    const char* glyphRow = glyph.rows[row] ? glyph.rows[row] : "";
    int rowWidth = strlen(glyphRow);
    if (rowWidth > width) width = rowWidth;
  }
  return width;
}

int asciiClockTextCols(const char* text, const AsciiClockFont& font) {
  int cols = 0;
  for (size_t i = 0; text[i] != '\0'; ++i) {
    if (i > 0) cols += font.glyphGap;
    cols += asciiClockGlyphWidth(font, asciiClockGlyphFor(font, text[i]));
  }
  return cols;
}

void appendAsciiClockGlyphRow(char* out, size_t outCap, const AsciiClockFont& font, const AsciiClockGlyph& glyph,
                              int row) {
  int width = asciiClockGlyphWidth(font, glyph);
  const char* glyphRow = glyph.rows[row] ? glyph.rows[row] : "";
  int rowLen = strlen(glyphRow);
  for (int col = 0; col < width; ++col) {
    appendCharSafe(out, outCap, col < rowLen ? glyphRow[col] : ' ');
  }
}

void trimTrailingSpaces(char* out) {
  int len = strlen(out);
  while (len > 0 && out[len - 1] == ' ') {
    out[--len] = '\0';
  }
}

char mirroredClockChar(char c) {
  switch (c) {
    case '/': return '\\';
    case '\\': return '/';
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    case '<': return '>';
    case '>': return '<';
    default: return c;
  }
}

void mirrorClockTextInPlace(char* text) {
  int len = strlen(text);
  for (int i = 0; i < len / 2; ++i) {
    char left = mirroredClockChar(text[i]);
    char right = mirroredClockChar(text[len - 1 - i]);
    text[i] = right;
    text[len - 1 - i] = left;
  }
  if (len % 2 == 1) {
    text[len / 2] = mirroredClockChar(text[len / 2]);
  }
}

uint16_t activeRainbowColor() {
  unsigned long now = aquariumNowMs != 0 ? aquariumNowMs : millis();
  return rainbowColorAtMs(now);
}

bool activeClockTextRainbowEnabled() {
  return (clockDisplayStyle == CLOCK_STYLE_ASCII) ? clockAsciiRainbowEnabled : clockSmallRainbowEnabled;
}

uint16_t storedActiveClockTextColor() {
  return (clockDisplayStyle == CLOCK_STYLE_ASCII) ? clockAsciiTextColor : clockSmallTextColor;
}

uint16_t activeClockTextColor() {
  return activeClockTextRainbowEnabled() ? activeRainbowColor() : storedActiveClockTextColor();
}

uint16_t currentAsciiClockTextColor() {
  return clockAsciiRainbowEnabled ? activeRainbowColor() : clockAsciiTextColor;
}

uint16_t currentSmallClockTextColor() {
  return clockSmallRainbowEnabled ? activeRainbowColor() : clockSmallTextColor;
}

void setActiveClockTextColor(uint16_t color) {
  if (clockDisplayStyle == CLOCK_STYLE_ASCII) {
    clockAsciiTextColor = color;
    clockAsciiRainbowEnabled = false;
  } else {
    clockSmallTextColor = color;
    clockSmallRainbowEnabled = false;
  }
  markSettingsDirty();
}

void setActiveClockTextRainbowEnabled(bool enabled) {
  if (clockDisplayStyle == CLOCK_STYLE_ASCII) {
    clockAsciiRainbowEnabled = enabled;
  } else {
    clockSmallRainbowEnabled = enabled;
  }
  markSettingsDirty();
}

const TimezoneOption& currentTimezone() {
  timezoneIndex = clampVal(timezoneIndex, 0, TIMEZONE_COUNT - 1);
  return timezoneOptions[timezoneIndex];
}

// Points the C library's local-time conversion (localtime_r/mktime) at the
// currently selected timezone. configTzTime() does this too, but only when
// Wi-Fi/NTP actually runs; this keeps manual-mode RTC round-trips correct
// even when the device is never connected.
void applyConfiguredTimezone() {
  const TimezoneOption& tz = currentTimezone();
  setenv("TZ", tz.posix, 1);
  tzset();
}

void cycleTimezone(int delta) {
  timezoneIndex += delta;
  if (timezoneIndex < 0) timezoneIndex = TIMEZONE_COUNT - 1;
  if (timezoneIndex >= TIMEZONE_COUNT) timezoneIndex = 0;
  applyConfiguredTimezone();
  wifiTimeConfigured = false;
  wifiTimeSynced = false;
  if (clockUseInternetTime && wifiConnected) {
    beginInternetTimeSync();
  }
  markSettingsDirty();
}

const char* backgroundStyleName() {
  switch (backgroundStyle) {
    case BACKGROUND_STYLE_BLACK:
      return "Black";
    case BACKGROUND_STYLE_DITHERED:
      return "Dithered";
    case BACKGROUND_STYLE_SMOOTH:
      return "Smooth";
    case BACKGROUND_STYLE_FLOWERS:
      return "Flowers";
    default:
      return "Background";
  }
}

bool backgroundUsesGradientColor() {
  return backgroundStyle == BACKGROUND_STYLE_DITHERED || backgroundStyle == BACKGROUND_STYLE_SMOOTH;
}

const char* autoSkySlotName(AutoSkySlot slot) {
  switch (slot) {
    case AUTO_SKY_SUNRISE:
      return "Sunrise";
    case AUTO_SKY_DAY:
      return "Day";
    case AUTO_SKY_SUNSET:
      return "Sunset";
    case AUTO_SKY_NIGHT:
      return "Night";
    default:
      return "Sky";
  }
}

uint16_t autoSkySlotColor(AutoSkySlot slot) {
  switch (slot) {
    case AUTO_SKY_SUNRISE:
      return autoSkySunriseColor;
    case AUTO_SKY_DAY:
      return autoSkyDayColor;
    case AUTO_SKY_SUNSET:
      return autoSkySunsetColor;
    case AUTO_SKY_NIGHT:
      return autoSkyNightColor;
    default:
      return autoSkyDayColor;
  }
}

uint16_t blendRgb565(uint16_t a, uint16_t b, int t, int span) {
  if (span <= 0) return b;
  t = clampVal(t, 0, span);
  int r = rgb565R8(a) + ((rgb565R8(b) - rgb565R8(a)) * t) / span;
  int g = rgb565G8(a) + ((rgb565G8(b) - rgb565G8(a)) * t) / span;
  int bl = rgb565B8(a) + ((rgb565B8(b) - rgb565B8(a)) * t) / span;
  return rgb565From888(r, g, bl);
}

uint16_t autoSkyColorForSecond(int secondOfDay) {
  static const int kSunriseSecond = 6 * 3600;
  static const int kDaySecond = 9 * 3600;
  static const int kSunsetSecond = 18 * 3600;
  static const int kNightSecond = 21 * 3600;
  static const int kDaySeconds = 24 * 3600;

  secondOfDay = clampVal(secondOfDay, 0, kDaySeconds - 1);
  if (secondOfDay < kSunriseSecond) {
    return blendRgb565(autoSkyNightColor, autoSkySunriseColor,
                       secondOfDay + kDaySeconds - kNightSecond,
                       kDaySeconds - kNightSecond + kSunriseSecond);
  }
  if (secondOfDay < kDaySecond) {
    return blendRgb565(autoSkySunriseColor, autoSkyDayColor,
                       secondOfDay - kSunriseSecond, kDaySecond - kSunriseSecond);
  }
  if (secondOfDay < kSunsetSecond) {
    return blendRgb565(autoSkyDayColor, autoSkySunsetColor,
                       secondOfDay - kDaySecond, kSunsetSecond - kDaySecond);
  }
  if (secondOfDay < kNightSecond) {
    return blendRgb565(autoSkySunsetColor, autoSkyNightColor,
                       secondOfDay - kSunsetSecond, kNightSecond - kSunsetSecond);
  }
  return blendRgb565(autoSkyNightColor, autoSkySunriseColor,
                     secondOfDay - kNightSecond, kDaySeconds - kNightSecond + kSunriseSecond);
}

uint16_t currentAutoSkyColor() {
  unsigned long now = millis();
  unsigned long secondsIntoMinute = (now >= clockLastMinuteMs) ? ((now - clockLastMinuteMs) / 1000UL) : 0UL;
  if (secondsIntoMinute > 59UL) secondsIntoMinute = 59UL;
  return autoSkyColorForSecond(clockHour * 3600 + clockMinute * 60 + (int)secondsIntoMinute);
}

uint16_t activeBackgroundGradientColor() {
  if (autoSkyEnabled) return autoSkyGradientColor;
  return backgroundRainbowEnabled ? backgroundRainbowColor : backgroundGradientColor;
}

int backgroundCycleIndex(BackgroundStyle style) {
  for (int i = 0; i < BACKGROUND_CYCLE_STYLE_COUNT; ++i) {
    if (kBackgroundCycleStyles[i] == style) return i;
  }
  return 1;  // Dithered
}

void cycleBackgroundStyle(int delta) {
  int next = backgroundCycleIndex(backgroundStyle) + delta;
  while (next < 0) next += BACKGROUND_CYCLE_STYLE_COUNT;
  while (next >= BACKGROUND_CYCLE_STYLE_COUNT) next -= BACKGROUND_CYCLE_STYLE_COUNT;
  backgroundStyle = kBackgroundCycleStyles[next];
  if (autoSkyEnabled && !backgroundUsesGradientColor()) {
    autoSkyEnabled = false;
    autoSkyPanelOpen = false;
    activeAutoSkyColorSlot = AUTO_SKY_SLOT_NONE;
  }
  invalidateBackgroundGradientCache();
  markSettingsDirty();
}

void setBackgroundGradientColor(uint16_t color) {
  backgroundGradientColor = color;
  backgroundRainbowEnabled = false;
  autoSkyEnabled = false;
  invalidateBackgroundGradientCache();
  applyAmbientLight();
  markSettingsDirty();
}

void setBackgroundRainbowEnabled(bool enabled) {
  if (backgroundRainbowEnabled == enabled) return;
  backgroundRainbowEnabled = enabled;
  if (enabled) {
    autoSkyEnabled = false;
    unsigned long now = aquariumNowMs != 0 ? aquariumNowMs : millis();
    backgroundRainbowColor = rainbowColorAtMs(now);
    lastBackgroundRainbowUpdateMs = now;
  }
  invalidateBackgroundGradientCache();
  applyAmbientLight();
  markSettingsDirty();
}

void setAutoSkyEnabled(bool enabled) {
  if (autoSkyEnabled == enabled) return;
  autoSkyEnabled = enabled;
  if (enabled) {
    backgroundRainbowEnabled = false;
    if (!backgroundUsesGradientColor()) backgroundStyle = BACKGROUND_STYLE_DITHERED;
    autoSkyGradientColor = currentAutoSkyColor();
  }
  invalidateBackgroundGradientCache();
  applyAmbientLight();
  markSettingsDirty();
}

void setAutoSkySlotColor(AutoSkySlot slot, uint16_t color) {
  switch (slot) {
    case AUTO_SKY_SUNRISE:
      autoSkySunriseColor = color;
      break;
    case AUTO_SKY_DAY:
      autoSkyDayColor = color;
      break;
    case AUTO_SKY_SUNSET:
      autoSkySunsetColor = color;
      break;
    case AUTO_SKY_NIGHT:
      autoSkyNightColor = color;
      break;
    default:
      return;
  }
  if (autoSkyEnabled) autoSkyGradientColor = currentAutoSkyColor();
  invalidateBackgroundGradientCache();
  applyAmbientLight();
  markSettingsDirty();
}

void serviceBackgroundRainbow(unsigned long now) {
  if (!backgroundRainbowEnabled) return;
  if (backgroundRainbowColor != 0 && now - lastBackgroundRainbowUpdateMs < BACKGROUND_RAINBOW_UPDATE_MS) return;

  uint16_t nextColor = rainbowColorAtMs(now);
  lastBackgroundRainbowUpdateMs = now;
  if (nextColor == backgroundRainbowColor) return;
  backgroundRainbowColor = nextColor;
  invalidateBackgroundGradientCache();
  applyAmbientLight();
}

void serviceAutoSky() {
  if (!autoSkyEnabled) return;
  uint16_t nextColor = currentAutoSkyColor();
  if (nextColor == autoSkyGradientColor) return;
  autoSkyGradientColor = nextColor;
  invalidateBackgroundGradientCache();
  applyAmbientLight();
}

uint16_t activeAmbientLightColor() {
  if (ambientLightLinkedToBackground) return activeBackgroundGradientColor();
  return ambientLightRainbowEnabled ? activeRainbowColor() : ambientLightColor;
}

bool lightScheduleControlsLcd() {
  return lightScheduleEnabled &&
         (lightScheduleDimmingMode == LIGHT_DIM_LCD || lightScheduleDimmingMode == LIGHT_DIM_BOTH);
}

bool lightScheduleControlsAmbient() {
  return lightScheduleEnabled &&
         (lightScheduleDimmingMode == LIGHT_DIM_AMBIENT || lightScheduleDimmingMode == LIGHT_DIM_BOTH);
}

int lightScheduleSecondOfDay(unsigned long now) {
  unsigned long secondsIntoMinute = (now >= clockLastMinuteMs) ? ((now - clockLastMinuteMs) / 1000UL) : 0UL;
  if (secondsIntoMinute > 59UL) secondsIntoMinute = 59UL;
  return clockHour * 3600 + clockMinute * 60 + (int)secondsIntoMinute;
}

int lightScheduleBaseScalePercent(unsigned long now) {
  if (!lightScheduleEnabled || lightScheduleDimmingMode == LIGHT_DIM_NONE) return 100;

  const int daySeconds = 86400;
  int nowSecond = lightScheduleSecondOfDay(now);
  int startSecond = clampVal(lightScheduleStartHour, 0, 23) * 3600;
  int endSecond = clampVal(lightScheduleEndHour, 0, 23) * 3600;
  if (startSecond == endSecond) return 100;

  int windowSeconds = endSecond - startSecond;
  if (windowSeconds <= 0) windowSeconds += daySeconds;

  int positionSeconds = nowSecond - startSecond;
  if (positionSeconds < 0) positionSeconds += daySeconds;

  int dimSeconds = clampVal(lightScheduleDimMinutes, 1, 60) * 60;
  if (positionSeconds >= windowSeconds) {
    int secondsUntilStart = startSecond - nowSecond;
    if (secondsUntilStart < 0) secondsUntilStart += daySeconds;
    if (secondsUntilStart >= dimSeconds) return 0;
    return clampVal(((dimSeconds - secondsUntilStart) * 100) / dimSeconds, 0, 100);
  }

  int scale = 100;
  int secondsUntilEnd = windowSeconds - positionSeconds;
  if (secondsUntilEnd < dimSeconds) {
    scale = min(scale, (secondsUntilEnd * 100) / dimSeconds);
  }
  return clampVal(scale, 0, 100);
}

int lightScheduleWakeScalePercent(unsigned long now) {
  if (lightScheduleWakeUntilMs == 0 || (long)(now - lightScheduleWakeUntilMs) >= 0) {
    lightScheduleWakeUntilMs = 0;
    return 0;
  }
  unsigned long remainingMs = lightScheduleWakeUntilMs - now;
  if (remainingMs > LIGHT_SCHEDULE_TOUCH_WAKE_MS) remainingMs = LIGHT_SCHEDULE_TOUCH_WAKE_MS;
  return clampVal((int)((remainingMs * 100UL) / LIGHT_SCHEDULE_TOUCH_WAKE_MS), 0, 100);
}

int lightScheduleScalePercent(unsigned long now) {
  return max(lightScheduleBaseScalePercent(now), lightScheduleWakeScalePercent(now));
}

bool lightScheduleLcdUiGuardActive() {
  return backlightPanelOpen || lightColorModePanelOpen || lightSchedulePanelOpen || ambientColorPanelOpen;
}

int scheduledLcdBrightness(unsigned long now) {
  int brightness = clampVal(lcdBacklightBrightness, MIN_LCD_BACKLIGHT_BRIGHTNESS, MAX_LCD_BACKLIGHT_BRIGHTNESS);
  if (!lightScheduleControlsLcd()) return brightness;
  int scheduledBrightness = (brightness * lightScheduleScalePercent(now)) / 100;
  if (lightScheduleLcdUiGuardActive()) {
    scheduledBrightness = max(scheduledBrightness, LIGHT_SCHEDULE_UI_MIN_LCD_BRIGHTNESS);
  }
  return clampVal(scheduledBrightness, 0, MAX_LCD_BACKLIGHT_BRIGHTNESS);
}

int scheduledAmbientBrightness(unsigned long now) {
  int brightness = clampVal(ambientLightBrightness, MIN_AMBIENT_BRIGHTNESS, MAX_AMBIENT_BRIGHTNESS);
  if (!lightScheduleControlsAmbient()) return brightness;
  return clampVal((brightness * lightScheduleScalePercent(now)) / 100, 0, MAX_AMBIENT_BRIGHTNESS);
}

void writePwmPin(int pin, bool pwmReady, int duty, bool activeLow) {
  if (pin < 0) return;
  duty = clampVal(duty, 0, 255);
  int writeDuty = activeLow ? (255 - duty) : duty;
  if (pwmReady) {
    ledcWrite(pin, writeDuty);
  } else {
    pinMode(pin, OUTPUT);
    bool on = duty > 0;
    digitalWrite(pin, activeLow ? (on ? LOW : HIGH) : (on ? HIGH : LOW));
  }
}

void applyLcdBacklight() {
  lcdBacklightBrightness = clampVal(lcdBacklightBrightness, MIN_LCD_BACKLIGHT_BRIGHTNESS, MAX_LCD_BACKLIGHT_BRIGHTNESS);
  int duty = (scheduledLcdBrightness(millis()) * 255) / 100;
  writePwmPin(TFT_BACKLIGHT_PIN, lcdBacklightPwmReady, duty, false);
}

void applyAmbientLight() {
  ambientLightBrightness = clampVal(ambientLightBrightness, MIN_AMBIENT_BRIGHTNESS, MAX_AMBIENT_BRIGHTNESS);
  uint16_t color = activeAmbientLightColor();
  int scale = ambientLightEnabled ? scheduledAmbientBrightness(millis()) : 0;
  int r = (rgb565R8(color) * scale) / 100;
  int g = (rgb565G8(color) * scale) / 100;
  int b = (rgb565B8(color) * scale) / 100;
  writePwmPin(AMBIENT_LED_RED_PIN, ambientRedPwmReady, r, AMBIENT_LED_ACTIVE_LOW);
  writePwmPin(AMBIENT_LED_GREEN_PIN, ambientGreenPwmReady, g, AMBIENT_LED_ACTIVE_LOW);
  writePwmPin(AMBIENT_LED_BLUE_PIN, ambientBluePwmReady, b, AMBIENT_LED_ACTIVE_LOW);
}

void applyLightingOutputs() {
  applyLcdBacklight();
  applyAmbientLight();
}

bool wakeLightScheduleFromTouch(unsigned long now) {
  if (!lightScheduleEnabled || lightScheduleDimmingMode == LIGHT_DIM_NONE) return false;

  int baseScale = lightScheduleBaseScalePercent(now);
  if (baseScale >= 100) return false;

  bool wakeAlreadyActive = lightScheduleWakeUntilMs != 0 && (long)(now - lightScheduleWakeUntilMs) < 0;
  lightScheduleWakeUntilMs = now + LIGHT_SCHEDULE_TOUCH_WAKE_MS;
  applyLightingOutputs();

  // If the LCD is fully off, make the first tap only wake the tank so hidden
  // controls do not fire blindly under the user's finger. Once awake, touches
  // pass through normally while extending the one-minute wake window.
  return !wakeAlreadyActive && baseScale <= 0 && lightScheduleControlsLcd();
}

bool attachLightingPin(int pin, bool activeLow) {
  if (pin < 0) return false;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, activeLow ? HIGH : LOW);
  return ledcAttach(pin, BACKLIGHT_PWM_FREQ, BACKLIGHT_PWM_BITS);
}

void initLightingHardware() {
  lcdBacklightPwmReady = attachLightingPin(TFT_BACKLIGHT_PIN, false);
  ambientRedPwmReady = attachLightingPin(AMBIENT_LED_RED_PIN, AMBIENT_LED_ACTIVE_LOW);
  ambientGreenPwmReady = attachLightingPin(AMBIENT_LED_GREEN_PIN, AMBIENT_LED_ACTIVE_LOW);
  ambientBluePwmReady = attachLightingPin(AMBIENT_LED_BLUE_PIN, AMBIENT_LED_ACTIVE_LOW);
  applyLightingOutputs();
}

void setAmbientLightColor(uint16_t color) {
  ambientLightColor = color;
  ambientLightLinkedToBackground = false;
  ambientLightRainbowEnabled = false;
  applyAmbientLight();
  markSettingsDirty();
}

void setAmbientLightRainbowEnabled(bool enabled) {
  ambientLightRainbowEnabled = enabled;
  ambientLightLinkedToBackground = false;
  applyAmbientLight();
  markSettingsDirty();
}

void setAmbientLightEnabled(bool enabled) {
  ambientLightEnabled = enabled;
  applyAmbientLight();
  markSettingsDirty();
}

void adjustLcdBacklightBrightness(int delta) {
  lcdBacklightBrightness = clampVal(lcdBacklightBrightness + delta, MIN_LCD_BACKLIGHT_BRIGHTNESS,
                                    MAX_LCD_BACKLIGHT_BRIGHTNESS);
  applyLcdBacklight();
  markSettingsDirty();
}

void adjustAmbientLightBrightness(int delta) {
  ambientLightBrightness = clampVal(ambientLightBrightness + delta, MIN_AMBIENT_BRIGHTNESS, MAX_AMBIENT_BRIGHTNESS);
  applyAmbientLight();
  markSettingsDirty();
}

void setAmbientLightLinkedToBackground(bool linked) {
  ambientLightLinkedToBackground = linked;
  applyAmbientLight();
  markSettingsDirty();
}

LightScheduleDimmingMode normalizeLightScheduleDimmingMode(int mode) {
  if (mode >= 0 && mode < LIGHT_DIM_COUNT) return (LightScheduleDimmingMode)mode;
  return DEFAULT_LIGHT_SCHEDULE_DIM_MODE;
}

const char* lightScheduleDimmingName() {
  switch (lightScheduleDimmingMode) {
    case LIGHT_DIM_LCD:
      return "LCD";
    case LIGHT_DIM_AMBIENT:
      return "Ambient";
    case LIGHT_DIM_BOTH:
      return "Both";
    case LIGHT_DIM_NONE:
    default:
      return "None";
  }
}

int normalizeLightScheduleDimMinutes(int value) {
  int best = LIGHT_SCHEDULE_DIM_OPTIONS[0];
  int bestDiff = abs(value - best);
  for (int i = 1; i < LIGHT_SCHEDULE_DIM_OPTION_COUNT; ++i) {
    int diff = abs(value - LIGHT_SCHEDULE_DIM_OPTIONS[i]);
    if (diff < bestDiff) {
      best = LIGHT_SCHEDULE_DIM_OPTIONS[i];
      bestDiff = diff;
    }
  }
  return best;
}

void sanitizeLightingSettings() {
  lcdBacklightBrightness = clampVal(lcdBacklightBrightness, MIN_LCD_BACKLIGHT_BRIGHTNESS, MAX_LCD_BACKLIGHT_BRIGHTNESS);
  ambientLightBrightness = clampVal(ambientLightBrightness, MIN_AMBIENT_BRIGHTNESS, MAX_AMBIENT_BRIGHTNESS);
  lightScheduleStartHour = clampVal(lightScheduleStartHour, 0, 23);
  lightScheduleEndHour = clampVal(lightScheduleEndHour, 0, 23);
  lightScheduleDimMinutes = normalizeLightScheduleDimMinutes(lightScheduleDimMinutes);
  lightScheduleDimmingMode = normalizeLightScheduleDimmingMode((int)lightScheduleDimmingMode);
}

void keepLightScheduleLcdAwakeIfNeeded(unsigned long now) {
  if (!lightScheduleControlsLcd()) return;
  if (lightScheduleBaseScalePercent(now) < 100) {
    lightScheduleWakeUntilMs = now + LIGHT_SCHEDULE_TOUCH_WAKE_MS;
  }
}

void setLightScheduleEnabled(bool enabled) {
  lightScheduleEnabled = enabled;
  keepLightScheduleLcdAwakeIfNeeded(millis());
  applyLightingOutputs();
  markSettingsDirty();
}

void adjustLightScheduleHour(int& hour, int delta) {
  hour += delta;
  while (hour < 0) hour += 24;
  while (hour > 23) hour -= 24;
  keepLightScheduleLcdAwakeIfNeeded(millis());
  applyLightingOutputs();
  markSettingsDirty();
}

void cycleLightScheduleDimMinutes(int delta) {
  int current = normalizeLightScheduleDimMinutes(lightScheduleDimMinutes);
  int index = 0;
  for (int i = 0; i < LIGHT_SCHEDULE_DIM_OPTION_COUNT; ++i) {
    if (LIGHT_SCHEDULE_DIM_OPTIONS[i] == current) {
      index = i;
      break;
    }
  }

  index += delta;
  if (index < 0) index = LIGHT_SCHEDULE_DIM_OPTION_COUNT - 1;
  if (index >= LIGHT_SCHEDULE_DIM_OPTION_COUNT) index = 0;
  lightScheduleDimMinutes = LIGHT_SCHEDULE_DIM_OPTIONS[index];
  keepLightScheduleLcdAwakeIfNeeded(millis());
  applyLightingOutputs();
  markSettingsDirty();
}

void cycleLightScheduleDimmingMode(int delta) {
  int mode = (int)normalizeLightScheduleDimmingMode((int)lightScheduleDimmingMode) + delta;
  while (mode < 0) mode += LIGHT_DIM_COUNT;
  while (mode >= LIGHT_DIM_COUNT) mode -= LIGHT_DIM_COUNT;
  lightScheduleDimmingMode = (LightScheduleDimmingMode)mode;
  keepLightScheduleLcdAwakeIfNeeded(millis());
  applyLightingOutputs();
  markSettingsDirty();
}

void serviceLightSchedule(unsigned long now) {
  if (now - lastLightScheduleServiceMs < LIGHT_SCHEDULE_SERVICE_MS) return;
  lastLightScheduleServiceMs = now;
  applyLightingOutputs();
}

BackgroundStyle normalizeBackgroundStyle(uint8_t savedStyle) {
  if (savedStyle < BACKGROUND_STYLE_COUNT) return (BackgroundStyle)savedStyle;
  return DEFAULT_BACKGROUND_STYLE;
}

BackgroundStyle legacyBackgroundStyle(uint8_t savedMode, uint8_t savedVersion) {
  // v1.96 and older saved colour-specific background IDs. Migrate those into
  // the new style+colour model without surprising existing installs.
  if (savedVersion < 4) {
    switch (savedMode) {
      case 0:
        return BACKGROUND_STYLE_BLACK;
      case 7:
        return BACKGROUND_STYLE_SMOOTH;
      case 9:
        return BACKGROUND_STYLE_DITHERED;
      case 10:
        return BACKGROUND_STYLE_FLOWERS;
      default:
        return BACKGROUND_STYLE_DITHERED;
    }
  }

  switch (savedMode) {
    case 0:
      return BACKGROUND_STYLE_BLACK;
    case 1:
      return BACKGROUND_STYLE_DITHERED;
    case 2:
      return BACKGROUND_STYLE_SMOOTH;
    case 3:
      return BACKGROUND_STYLE_DITHERED;
    case 4:
      return BACKGROUND_STYLE_FLOWERS;
    default:
      return BACKGROUND_STYLE_DITHERED;
  }
}

uint16_t legacyBackgroundColor(uint8_t savedMode, uint8_t savedVersion) {
  if ((savedVersion < 4 && savedMode == 9) || (savedVersion >= 4 && savedMode == 3)) {
    return DEFAULT_BACKGROUND_PURPLE_COLOR;
  }
  return DEFAULT_BACKGROUND_GRADIENT_COLOR;
}

int color5To8(int v5) { return (v5 << 3) | (v5 >> 2); }
int color6To8(int v6) { return (v6 << 2) | (v6 >> 4); }

int gradientNoiseThreshold(int x, int y) {
  uint32_t h = (uint32_t)x * 374761393UL + (uint32_t)y * 668265263UL + 0x9E3779B9UL;
  h = (h ^ (h >> 13)) * 1274126177UL;
  h ^= (h >> 16);
  return (int)(h & 0xFF);
}

int gradientBayerThreshold(int x, int y, int scale) {
  static const uint8_t kBayer8x8[64] = {
      0, 48, 12, 60, 3, 51, 15, 63,
      32, 16, 44, 28, 35, 19, 47, 31,
      8, 56, 4, 52, 11, 59, 7, 55,
      40, 24, 36, 20, 43, 27, 39, 23,
      2, 50, 14, 62, 1, 49, 13, 61,
      34, 18, 46, 30, 33, 17, 45, 29,
      10, 58, 6, 54, 9, 57, 5, 53,
      42, 26, 38, 22, 41, 25, 37, 21};
  int sx = x / max(1, scale);
  int sy = y / max(1, scale);
  return kBayer8x8[(sy & 7) * 8 + (sx & 7)] << 2;
}

enum GradientDitherPattern {
  GRADIENT_DITHER_ORDERED,
  GRADIENT_DITHER_CHECKER,
  GRADIENT_DITHER_CHECKER_LOCKED,
  GRADIENT_DITHER_NOISE,
  GRADIENT_DITHER_NONE
};

static const uint8_t kGradientStops[] = {0, 18, 42, 74, 112, 156, 204, 232, 255};
static const uint8_t kGradientBrightness[] = {255, 228, 198, 164, 126, 90, 58, 30, 0};
static constexpr int kGradientStopCount = sizeof(kGradientStops) / sizeof(kGradientStops[0]);
static uint16_t activeGradientColors[kGradientStopCount];

int gradientDitherThreshold(int x, int y, int pattern, int scale) {
  switch (pattern) {
    case GRADIENT_DITHER_ORDERED:
      return gradientBayerThreshold(x, y, scale);
    case GRADIENT_DITHER_CHECKER:
    case GRADIENT_DITHER_CHECKER_LOCKED:
      return (((x / max(1, scale)) + (y / max(1, scale))) & 1) ? 224 : 32;
    case GRADIENT_DITHER_NOISE:
      return gradientNoiseThreshold(x, y);
    default:
      return 128;
  }
}

void gradientRgbAtT(const uint16_t* colors, const uint8_t* stops, int count, int t255, int& r8, int& g8, int& b8) {
  if (count <= 0) {
    r8 = g8 = b8 = 0;
    return;
  }
  if (t255 <= stops[0]) {
    r8 = color5To8((colors[0] >> 11) & 0x1F);
    g8 = color6To8((colors[0] >> 5) & 0x3F);
    b8 = color5To8(colors[0] & 0x1F);
    return;
  }
  if (t255 >= stops[count - 1]) {
    r8 = color5To8((colors[count - 1] >> 11) & 0x1F);
    g8 = color6To8((colors[count - 1] >> 5) & 0x3F);
    b8 = color5To8(colors[count - 1] & 0x1F);
    return;
  }

  for (int i = 1; i < count; ++i) {
    if (t255 <= stops[i]) {
      int t0 = stops[i - 1];
      int t1 = stops[i];
      int seg = t1 - t0;
      int blend = (seg > 0) ? ((t255 - t0) * 255) / seg : 255;
      int inv = 255 - blend;
      int c0r = color5To8((colors[i - 1] >> 11) & 0x1F);
      int c0g = color6To8((colors[i - 1] >> 5) & 0x3F);
      int c0b = color5To8(colors[i - 1] & 0x1F);
      int c1r = color5To8((colors[i] >> 11) & 0x1F);
      int c1g = color6To8((colors[i] >> 5) & 0x3F);
      int c1b = color5To8(colors[i] & 0x1F);
      r8 = (c0r * inv + c1r * blend) / 255;
      g8 = (c0g * inv + c1g * blend) / 255;
      b8 = (c0b * inv + c1b * blend) / 255;
      return;
    }
  }
}

uint16_t rgb565From888(int r8, int g8, int b8) {
  return RGB565((uint8_t)clampVal(r8, 0, 255), (uint8_t)clampVal(g8, 0, 255), (uint8_t)clampVal(b8, 0, 255));
}

void buildGradientColorsFromTop(uint16_t topColor, uint16_t* colorsOut, int count) {
  int rTop = color5To8((topColor >> 11) & 0x1F);
  int gTop = color6To8((topColor >> 5) & 0x3F);
  int bTop = color5To8(topColor & 0x1F);
  int brightnessCount = sizeof(kGradientBrightness) / sizeof(kGradientBrightness[0]);
  for (int i = 0; i < count; ++i) {
    int brightness = (i < brightnessCount) ? kGradientBrightness[i] : 0;
    colorsOut[i] = rgb565From888((rTop * brightness) / 255, (gTop * brightness) / 255,
                                 (bTop * brightness) / 255);
  }
  if (count > 0) colorsOut[count - 1] = BG_COLOR;
}

uint16_t swap565(uint16_t color) {
  return (uint16_t)((color << 8) | (color >> 8));
}

void applyRenderViewport(TFT_eSprite& s) {
  if (stripRenderActive) {
    s.setViewport(0, -stripRenderY, SCREEN_W, SCREEN_H, true);
  } else {
    s.resetViewport();
  }
}

void clearRenderSurface(TFT_eSprite& s) {
  if (!stripRenderActive) {
    s.fillSprite(BG_COLOR);
    return;
  }

  s.resetViewport();
  s.fillSprite(BG_COLOR);
  applyRenderViewport(s);
}

void drawVerticalGradientStops(TFT_eSprite& s, const uint16_t* colors, const uint8_t* stops, int stopCount, int gradientHeight,
                               int ditherPattern, int ditherScale, int ditherAmplitude) {
  int drawH = clampVal(gradientHeight, 2, SCREEN_H);
  int yMax = drawH - 1;
  int cellSize = max(1, ditherScale);
  bool lockToDitherCells = (ditherPattern == GRADIENT_DITHER_CHECKER_LOCKED);
  for (int y = 0; y < drawH; ++y) {
    int sampleY = lockToDitherCells ? ((y / cellSize) * cellSize) : y;
    int baseT255 = (sampleY * 255) / yMax;

    for (int x = 0; x < SCREEN_W; ++x) {
      int threshold = gradientDitherThreshold(x, y, ditherPattern, ditherScale) - 128;
      int sampleT255 = clampVal(baseT255 + (threshold * ditherAmplitude) / 128, 0, 255);
      int r8, g8, b8;
      gradientRgbAtT(colors, stops, stopCount, sampleT255, r8, g8, b8);
      s.drawPixel(x, y, rgb565From888(r8, g8, b8));
    }
  }
}

void buildGradientBandCache(const uint16_t* colors, const uint8_t* stops, int stopCount, int ditherPattern, int ditherScale,
                            int ditherAmplitude) {
  if (gradientBandCache == nullptr) return;

  int cellSize = max(1, ditherScale);
  bool lockToDitherCells = (ditherPattern == GRADIENT_DITHER_CHECKER_LOCKED);
  for (int y = 0; y < BACKGROUND_GRADIENT_H; ++y) {
    int sampleY = lockToDitherCells ? ((y / cellSize) * cellSize) : y;
    int baseT255 = (sampleY * 255) / (BACKGROUND_GRADIENT_H - 1);
    for (int x = 0; x < SCREEN_W; ++x) {
      int threshold = gradientDitherThreshold(x, y, ditherPattern, ditherScale) - 128;
      int sampleT255 = clampVal(baseT255 + (threshold * ditherAmplitude) / 128, 0, 255);
      int r8, g8, b8;
      gradientRgbAtT(colors, stops, stopCount, sampleT255, r8, g8, b8);
      gradientBandCache[y * SCREEN_W + x] = swap565(rgb565From888(r8, g8, b8));
    }
  }
}

void drawTopGradientBackground(TFT_eSprite& s, uint16_t topColor, int ditherPattern, int ditherScale, int ditherAmplitude) {
  clearRenderSurface(s);
  buildGradientColorsFromTop(topColor, activeGradientColors, kGradientStopCount);
  drawVerticalGradientStops(s, activeGradientColors, kGradientStops, kGradientStopCount, BACKGROUND_GRADIENT_H, ditherPattern, ditherScale,
                            ditherAmplitude);
}

void drawCachedTopGradientBackground(TFT_eSprite& s, BackgroundStyle style, uint16_t topColor, int ditherPattern,
                                     int ditherScale, int ditherAmplitude) {
  if (gradientBandCache == nullptr) {
    drawTopGradientBackground(s, topColor, ditherPattern, ditherScale, ditherAmplitude);
    return;
  }

  if (stripRenderActive && stripRenderY >= BACKGROUND_GRADIENT_H) {
    clearRenderSurface(s);
    return;
  }

  if (gradientBandCacheStyle != style || gradientBandCacheColor != topColor) {
    buildGradientColorsFromTop(topColor, activeGradientColors, kGradientStopCount);
    buildGradientBandCache(activeGradientColors, kGradientStops, kGradientStopCount, ditherPattern, ditherScale, ditherAmplitude);
    gradientBandCacheStyle = style;
    gradientBandCacheColor = topColor;
  }
  clearRenderSurface(s);
  s.pushImage(0, 0, SCREEN_W, BACKGROUND_GRADIENT_H, gradientBandCache);
}

void allocateGradientBandCache() {
  size_t pixelCount = (size_t)SCREEN_W * (size_t)BACKGROUND_GRADIENT_H;
  gradientBandCache = (uint16_t*)malloc(pixelCount * sizeof(uint16_t));
  invalidateBackgroundGradientCache();
}

bool createMainCanvas(int colorDepth, int renderHeight) {
  canvas.setColorDepth(colorDepth);
#if defined(AQUARIUM_BOARD_PANCAKE)
  // Keep the render surface in fast internal RAM. A full 480x320 canvas would
  // otherwise land in PSRAM, where per-pixel glyph draws are very slow. Forcing
  // internal RAM makes the existing strip fallback pick the largest strip that
  // fits, which the loop below relies on for a smooth frame rate.
  canvas.setAttribute(PSRAM_ENABLE, false);
#endif
  if (canvas.createSprite(SCREEN_W, renderHeight) == nullptr) return false;

  mainCanvasActualColorDepth = colorDepth;
  mainCanvasRenderHeight = renderHeight;
  return true;
}

bool allocateMainCanvas() {
  if (createMainCanvas(MAIN_SPRITE_COLOR_DEPTH, MAIN_RENDER_SURFACE_H)) return true;

  static const int kStripHeights[] = {
      STRIP_RENDER_SURFACE_H_1,
      STRIP_RENDER_SURFACE_H_2,
      STRIP_RENDER_SURFACE_H_3,
      STRIP_RENDER_SURFACE_H_4,
      STRIP_RENDER_SURFACE_H_5,
  };
  for (int i = 0; i < (int)(sizeof(kStripHeights) / sizeof(kStripHeights[0])); ++i) {
    int h = clampVal(kStripHeights[i], 1, SCREEN_H);
    if (h >= SCREEN_H) continue;
    if (createMainCanvas(MAIN_SPRITE_COLOR_DEPTH, h)) return true;
  }

  if (createMainCanvas(8, MAIN_RENDER_SURFACE_H)) return true;

  mainCanvasActualColorDepth = 0;
  mainCanvasRenderHeight = 0;
  return false;
}

struct PixelFlowerSpec {
  int cx;
  int cy;
  int radius;
  float rotation;
  uint16_t color;
};

struct PixelFlowerPoint {
  int16_t x;
  int16_t y;
};

static const PixelFlowerSpec kDefaultPixelFlowers[] = {
    {70, 70, 58, 6.28318f, RGB565(0, 26, 76)},
    {248, 72, 58, 6.82318f, RGB565(0, 22, 66)},
    {202, 156, 28, 3.14159f, RGB565(116, 108, 18)},
};
static PixelFlowerSpec pixelFlowers[sizeof(kDefaultPixelFlowers) / sizeof(kDefaultPixelFlowers[0])];
static constexpr int kPixelFlowerCount = sizeof(pixelFlowers) / sizeof(pixelFlowers[0]);
static const int PIXEL_FLOWER_SEGMENTS = 80;
static PixelFlowerPoint pixelFlowerPoints[kPixelFlowerCount][PIXEL_FLOWER_SEGMENTS + 1];
static bool pixelFlowerGeometryDirty = true;

void savePersistentState() {
  prefs.begin("ascii-aq", false);
  prefs.putUChar("ver", 15);
  prefs.putInt("fish", fishTargetCount);
  prefs.putInt("bubbles", bubbleTargetCount);
  prefs.putInt("oct_freq", octopusFrequency);
  prefs.putInt("seah_freq", seahorseFrequency);
  prefs.putInt("auto_feed", autoFeedFrequency);
  prefs.putInt("snail_fr", snailFrequency);
  prefs.putInt("jelly_fr", jellyfishFrequency);
  prefs.putULong("cth_pel", cthulhuFeedPelletCount);
  prefs.putFloat("sway", seaweedSwaySpeed);
  prefs.putFloat("sea_len", seaweedLength);
  prefs.putFloat("sea_rand", seaweedLengthRandomness);
  prefs.putUChar("bg_style", (uint8_t)backgroundStyle);
  prefs.putUShort("bg_color", backgroundGradientColor);
  prefs.putBool("bg_rain", backgroundRainbowEnabled);
  prefs.putBool("sky_on", autoSkyEnabled);
  prefs.putUShort("sky_sr", autoSkySunriseColor);
  prefs.putUShort("sky_day", autoSkyDayColor);
  prefs.putUShort("sky_ss", autoSkySunsetColor);
  prefs.putUShort("sky_nt", autoSkyNightColor);
  prefs.putBool("disp_flip", displayFlip180);
  prefs.putInt("lcd_brite", lcdBacklightBrightness);
  prefs.putBool("amb_on", ambientLightEnabled);
  prefs.putInt("amb_brite", ambientLightBrightness);
  prefs.putBool("amb_bg", ambientLightLinkedToBackground);
  prefs.putUShort("amb_color", ambientLightColor);
  prefs.putBool("amb_rain", ambientLightRainbowEnabled);
  prefs.putBool("ls_on", lightScheduleEnabled);
  prefs.putUChar("ls_start", (uint8_t)lightScheduleStartHour);
  prefs.putUChar("ls_end", (uint8_t)lightScheduleEndHour);
  prefs.putUChar("ls_dim", (uint8_t)lightScheduleDimMinutes);
  prefs.putUChar("ls_mode", (uint8_t)lightScheduleDimmingMode);
  prefs.putBool("clock_on", clockVisible);
  prefs.putBool("clock_24h", clockUse24Hour);
  prefs.putBool("clock_net", clockUseInternetTime);
  prefs.putUChar("clk_style", (uint8_t)clockDisplayStyle);
  prefs.putUChar("clk_pos", (uint8_t)clockSmallPosition);
  prefs.putUChar("clk_font", (uint8_t)asciiClockFontIndex);
  prefs.putUChar("clk_zoom", clockZoomNotch);
  prefs.putBool("clk_flip", clockFlipHorizontal);
  prefs.putUShort("clk_s_col", clockSmallTextColor);
  prefs.putUShort("clk_a_col", clockAsciiTextColor);
  prefs.putBool("clk_s_rn", clockSmallRainbowEnabled);
  prefs.putBool("clk_a_rn", clockAsciiRainbowEnabled);
  prefs.putInt("tz_idx", timezoneIndex);
  prefs.putInt("clk_year", clockYear);
  prefs.putInt("clk_month", clockMonth);
  prefs.putInt("clk_day", clockDay);
  prefs.putInt("clk_hour", clockHour);
  prefs.putInt("clk_min", clockMinute);
  prefs.putBool("wifi_on", wifiEnabled);
  prefs.putString("wifi_ssid", wifiSsid);
  prefs.putString("wifi_pass", wifiPass);
  prefs.putBytes("flowers", pixelFlowers, sizeof(pixelFlowers));
  prefs.end();
  settingsDirty = false;
  lastSettingsSaveMs = millis();
}

void loadPersistentState() {
  memcpy(pixelFlowers, kDefaultPixelFlowers, sizeof(pixelFlowers));
  wifiSsid[0] = '\0';
  wifiPass[0] = '\0';

  prefs.begin("ascii-aq", true);
  uint8_t version = prefs.getUChar("ver", 0);
  bool migratedSpawnDefaults = false;
  if (version != 0) {
    fishTargetCount = prefs.getInt("fish", DEFAULT_FISH);
    bubbleTargetCount = prefs.getInt("bubbles", DEFAULT_BUBBLES);
    octopusFrequency = prefs.getInt("oct_freq", DEFAULT_OCTOPUS_FREQUENCY);
    seahorseFrequency = prefs.getInt("seah_freq", DEFAULT_SEAHORSE_FREQUENCY);
    autoFeedFrequency = prefs.getInt("auto_feed", DEFAULT_AUTO_FEED_FREQUENCY);
    snailFrequency = prefs.getInt("snail_fr", DEFAULT_SNAIL_FREQUENCY);
    jellyfishFrequency = prefs.getInt("jelly_fr", DEFAULT_JELLYFISH_FREQUENCY);
    cthulhuFeedPelletCount = prefs.getULong("cth_pel", 0UL);
    if (version < 2 && seahorseFrequency == 2) {
      seahorseFrequency = DEFAULT_SEAHORSE_FREQUENCY;
    }
    if (version < 8) {
      if (octopusFrequency == 1) {
        octopusFrequency = DEFAULT_OCTOPUS_FREQUENCY;
        migratedSpawnDefaults = true;
      }
      if (seahorseFrequency == 1) {
        seahorseFrequency = DEFAULT_SEAHORSE_FREQUENCY;
        migratedSpawnDefaults = true;
      }
    }
    seaweedSwaySpeed = prefs.getFloat("sway", DEFAULT_SWAY);
    seaweedLength = prefs.getFloat("sea_len", DEFAULT_SEAWEED_LENGTH);
    seaweedLengthRandomness = prefs.getFloat("sea_rand", DEFAULT_SEAWEED_LENGTH_RANDOMNESS);
    if (version >= 5) {
      backgroundStyle = normalizeBackgroundStyle(prefs.getUChar("bg_style", (uint8_t)DEFAULT_BACKGROUND_STYLE));
      backgroundGradientColor = prefs.getUShort("bg_color", DEFAULT_BACKGROUND_GRADIENT_COLOR);
      backgroundRainbowEnabled = prefs.getBool("bg_rain", false);
      autoSkyEnabled = prefs.getBool("sky_on", false);
      autoSkySunriseColor = prefs.getUShort("sky_sr", DEFAULT_AUTO_SKY_SUNRISE_COLOR);
      autoSkyDayColor = prefs.getUShort("sky_day", DEFAULT_AUTO_SKY_DAY_COLOR);
      autoSkySunsetColor = prefs.getUShort("sky_ss", DEFAULT_AUTO_SKY_SUNSET_COLOR);
      autoSkyNightColor = prefs.getUShort("sky_nt", DEFAULT_AUTO_SKY_NIGHT_COLOR);
    } else {
      uint8_t savedMode = prefs.getUChar("bg_mode", 1);
      backgroundStyle = legacyBackgroundStyle(savedMode, version);
      backgroundGradientColor = legacyBackgroundColor(savedMode, version);
      backgroundRainbowEnabled = false;
      autoSkyEnabled = false;
    }
    displayFlip180 = prefs.getBool("disp_flip", false);
    lcdBacklightBrightness = prefs.getInt("lcd_brite", DEFAULT_LCD_BACKLIGHT_BRIGHTNESS);
    ambientLightEnabled = prefs.getBool("amb_on", false);
    ambientLightBrightness = prefs.getInt("amb_brite", DEFAULT_AMBIENT_BRIGHTNESS);
    ambientLightLinkedToBackground = prefs.getBool("amb_bg", true);
    ambientLightColor = prefs.getUShort("amb_color", DEFAULT_BACKGROUND_GRADIENT_COLOR);
    ambientLightRainbowEnabled = prefs.getBool("amb_rain", false);
    lightScheduleEnabled = prefs.getBool("ls_on", DEFAULT_LIGHT_SCHEDULE_ENABLED);
    lightScheduleStartHour = prefs.getUChar("ls_start", DEFAULT_LIGHT_SCHEDULE_START_HOUR);
    lightScheduleEndHour = prefs.getUChar("ls_end", DEFAULT_LIGHT_SCHEDULE_END_HOUR);
    lightScheduleDimMinutes = prefs.getUChar("ls_dim", DEFAULT_LIGHT_SCHEDULE_DIM_MINUTES);
    lightScheduleDimmingMode = (LightScheduleDimmingMode)prefs.getUChar("ls_mode", DEFAULT_LIGHT_SCHEDULE_DIM_MODE);
    clockVisible = prefs.getBool("clock_on", false);
    clockUse24Hour = prefs.getBool("clock_24h", false);
    clockUseInternetTime = prefs.getBool("clock_net", false);
    clockDisplayStyle = (ClockDisplayStyle)prefs.getUChar("clk_style", (uint8_t)CLOCK_STYLE_SMALL_TEXT);
    clockSmallPosition = (ClockSmallPosition)prefs.getUChar("clk_pos", (uint8_t)CLOCK_SMALL_TOP);
    asciiClockFontIndex = prefs.getUChar("clk_font", DEFAULT_ASCII_CLOCK_FONT_INDEX);
    clockZoomNotch = (uint8_t)clampVal((int)prefs.getUChar("clk_zoom", 0), 0, (int)CLOCK_ZOOM_NOTCHES - 1);
    clockFlipHorizontal = prefs.getBool("clk_flip", false);
    clockSmallTextColor = prefs.getUShort("clk_s_col", DEFAULT_SMALL_CLOCK_COLOR);
    clockAsciiTextColor = prefs.getUShort("clk_a_col", DEFAULT_ASCII_CLOCK_COLOR);
    clockSmallRainbowEnabled = prefs.getBool("clk_s_rn", false);
    clockAsciiRainbowEnabled = prefs.getBool("clk_a_rn", false);
    timezoneIndex = prefs.getInt("tz_idx", DEFAULT_TIMEZONE_INDEX);
    clockYear = prefs.getInt("clk_year", DEFAULT_CLOCK_YEAR);
    clockMonth = prefs.getInt("clk_month", DEFAULT_CLOCK_MONTH);
    clockDay = prefs.getInt("clk_day", DEFAULT_CLOCK_DAY);
    clockHour = prefs.getInt("clk_hour", DEFAULT_CLOCK_HOUR);
    clockMinute = prefs.getInt("clk_min", DEFAULT_CLOCK_MINUTE);
    wifiEnabled = prefs.getBool("wifi_on", false);
    prefs.getString("wifi_ssid", wifiSsid, sizeof(wifiSsid));
    prefs.getString("wifi_pass", wifiPass, sizeof(wifiPass));
    size_t flowerBytes = prefs.getBytesLength("flowers");
    if (flowerBytes == sizeof(pixelFlowers)) {
      prefs.getBytes("flowers", pixelFlowers, sizeof(pixelFlowers));
    }
  }
  prefs.end();

  fishTargetCount = clampVal(fishTargetCount, MIN_FISH, MAX_FISH);
  bubbleTargetCount = clampVal(bubbleTargetCount, MIN_BUBBLES, MAX_BUBBLES);
  octopusFrequency = normalizeOctopusFrequency(octopusFrequency);
  seahorseFrequency = normalizeSeahorseFrequency(seahorseFrequency);
  autoFeedFrequency = normalizeAutoFeedFrequency(autoFeedFrequency);
  snailFrequency = normalizeCreatureFrequency(snailFrequency);
  jellyfishFrequency = normalizeCreatureFrequency(jellyfishFrequency);
  if (cthulhuFeedPelletCount > CTHULHU_FEED_PELLET_THRESHOLD) {
    cthulhuFeedPelletCount = CTHULHU_FEED_PELLET_THRESHOLD;
  }
  if (autoFeedFrequency <= 0) {
    nextAutoFeedMs = 0;
    autoFeedSprinkleActive = false;
  }
  seaweedSwaySpeed = clampVal(seaweedSwaySpeed, MIN_SWAY, MAX_SWAY);
  seaweedLength = clampVal(seaweedLength, MIN_SEAWEED_LENGTH, MAX_SEAWEED_LENGTH);
  seaweedLengthRandomness = clampVal(seaweedLengthRandomness, MIN_SEAWEED_LENGTH_RANDOMNESS, MAX_SEAWEED_LENGTH_RANDOMNESS);
  backgroundStyle = normalizeBackgroundStyle((uint8_t)backgroundStyle);
  if (autoSkyEnabled) {
    backgroundRainbowEnabled = false;
    if (!backgroundUsesGradientColor()) backgroundStyle = BACKGROUND_STYLE_DITHERED;
  }
  backgroundRainbowColor = backgroundRainbowEnabled ? rainbowColorAtMs(millis()) : backgroundGradientColor;
  autoSkyGradientColor = currentAutoSkyColor();
  sanitizeLightingSettings();
  if ((int)clockDisplayStyle < 0 || clockDisplayStyle >= CLOCK_STYLE_COUNT) clockDisplayStyle = CLOCK_STYLE_SMALL_TEXT;
  if ((int)clockSmallPosition < 0 || clockSmallPosition >= CLOCK_SMALL_POSITION_COUNT) clockSmallPosition = CLOCK_SMALL_TOP;
  asciiClockFontIndex = clampVal(asciiClockFontIndex, 0, ASCII_CLOCK_FONT_COUNT - 1);
  timezoneIndex = clampVal(timezoneIndex, 0, TIMEZONE_COUNT - 1);
  if (!wifiEnabled) clockUseInternetTime = false;
  normalizeClockDate();
  invalidateBackgroundGradientCache();
  pixelFlowerGeometryDirty = true;
  settingsDirty = false;
  if (migratedSpawnDefaults) markSettingsDirty();
}

void serviceSettingsPersistence(unsigned long now) {
  if (settingsDirty && now - settingsDirtyMs >= SETTINGS_SAVE_DELAY_MS) {
    savePersistentState();
    return;
  }
  if (now - lastSettingsSaveMs >= CLOCK_AUTOSAVE_INTERVAL_MS) {
    savePersistentState();
  }
}

void setWifiStatus(const char* text) {
  copySafe(wifiStatusText, sizeof(wifiStatusText), text);
}

const char* internetTimeStatus() {
  if (!clockUseInternetTime) return "Manual";
  if (!wifiEnabled) return "WiFi Off";
  if (!wifiConnected) return wifiConnecting ? "Connecting" : "Waiting WiFi";
  return wifiTimeSynced ? "Synced" : "Syncing";
}

#if defined(AQUARIUM_BOARD_PANCAKE)
// Log association/DHCP events so a failing connect tells us WHY. The disconnect
// "reason" code is the key diagnostic:
//   2/15/205 = auth/handshake (wrong password or PMF/WPA3 mismatch)
//   201      = AP not found (channel/band/regulatory: AP not usable on this band)
//   203/204  = assoc/handshake fail
//   200      = beacon timeout (out of range)
void onAquariumWifiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.printf("[WiFi] assoc OK, channel %u\n", (unsigned)WiFi.channel());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[WiFi] disconnected, reason=%d\n",
                    (int)info.wifi_sta_disconnected.reason);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[WiFi] got IP %s\n", WiFi.localIP().toString().c_str());
      break;
    default:
      break;
  }
}
#endif

void ensureWifiRadioStarted() {
  if (wifiRadioStarted) return;
#if defined(AQUARIUM_BOARD_PANCAKE)
  WiFi.onEvent(onAquariumWifiEvent);
#endif
  WiFi.mode(WIFI_STA);
#if defined(CONFIG_IDF_TARGET_ESP32C5)
  // Enable both bands (2.4 + 5 GHz) — reliability comes from BSSID/channel
  // targeting in startWifiConnectTo(), not from disabling a band.
  esp_err_t bandErr = esp_wifi_set_band_mode(AQUARIUM_WIFI_BAND_MODE);
  if (bandErr != ESP_OK) {
    Serial.printf("[WiFi] esp_wifi_set_band_mode failed: %d\n", (int)bandErr);
  }
  // Set a regulatory domain so 5 GHz (and 2.4 GHz ch 12/13) channels are usable
  // for association. ieee80211d=true makes the STA follow the AP's advertised
  // country, so this works across regions. Marauder sets country explicitly too.
  esp_err_t ctryErr = esp_wifi_set_country_code(AQUARIUM_WIFI_COUNTRY, true);
  if (ctryErr != ESP_OK) {
    Serial.printf("[WiFi] esp_wifi_set_country_code failed: %d\n", (int)ctryErr);
  }
#endif
  WiFi.setSleep(false);
  wifiRadioStarted = true;
}

void clearWifiScanResults() {
  wifiNetworkCount = 0;
  wifiNetworkPage = 0;
  for (int i = 0; i < MAX_WIFI_NETWORKS; ++i) {
    wifiNetworkNames[i][0] = '\0';
    wifiNetworkRssi[i] = 0;
    wifiNetworkOpen[i] = false;
    wifiNetworkChannel[i] = 0;
    memset(wifiNetworkBssid[i], 0, 6);
  }
}

int wifiFindListed(const char* ssid) {
  for (int i = 0; i < wifiNetworkCount; ++i) {
    if (strncmp(wifiNetworkNames[i], ssid, WIFI_SSID_MAX_LEN) == 0) return i;
  }
  return -1;
}

void startWifiScan() {
  if (!wifiEnabled) return;
  ensureWifiRadioStarted();
  if (wifiConnecting) {
    wifiConnecting = false;
    wifiSavePendingCredentials = false;
    WiFi.disconnect(false);
  }
  clearWifiScanResults();
  WiFi.scanDelete();
  WiFi.scanNetworks(true, true);
  wifiScanInProgress = true;
  lastWifiServiceMs = 0;
  setWifiStatus("Scanning...");
}

void finishWifiScanIfReady() {
  if (!wifiScanInProgress) return;
  int scanResult = WiFi.scanComplete();
  if (scanResult == WIFI_SCAN_RUNNING) return;

  wifiScanInProgress = false;
  clearWifiScanResults();
  if (scanResult < 0) {
    setWifiStatus("Scan failed");
    WiFi.scanDelete();
    return;
  }

  // A combined 2.4/5 GHz SSID shows up as one scan entry per band. Keep a single
  // list row per name but remember the strongest AP's BSSID/channel, so tapping
  // the name connects to whichever band has the better signal.
  for (int i = 0; i < scanResult; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    int rssi = WiFi.RSSI(i);
    int channel = WiFi.channel(i);
    bool isOpen = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
    const uint8_t* bssid = WiFi.BSSID(i);

    int existing = wifiFindListed(ssid.c_str());
    if (existing >= 0) {
      if (rssi > wifiNetworkRssi[existing]) {   // stronger AP for this SSID → prefer it
        wifiNetworkRssi[existing] = rssi;
        wifiNetworkChannel[existing] = (uint8_t)channel;
        wifiNetworkOpen[existing] = isOpen;
        if (bssid) memcpy(wifiNetworkBssid[existing], bssid, 6);
      }
      continue;
    }
    if (wifiNetworkCount >= MAX_WIFI_NETWORKS) continue;

    copySafe(wifiNetworkNames[wifiNetworkCount], sizeof(wifiNetworkNames[wifiNetworkCount]), ssid.c_str());
    wifiNetworkRssi[wifiNetworkCount] = rssi;
    wifiNetworkChannel[wifiNetworkCount] = (uint8_t)channel;
    wifiNetworkOpen[wifiNetworkCount] = isOpen;
    if (bssid) memcpy(wifiNetworkBssid[wifiNetworkCount], bssid, 6);
    wifiNetworkCount++;
  }

  WiFi.scanDelete();
  setWifiStatus(wifiNetworkCount > 0 ? "Select network" : "No networks found");
}

void beginInternetTimeSync() {
  if (!wifiConnected) return;
  const TimezoneOption& tz = currentTimezone();
  configTzTime(tz.posix, CLOCK_NTP_1, CLOCK_NTP_2, CLOCK_NTP_3);
  wifiTimeConfigured = true;
  wifiLastNtpAttemptMs = 0;
  wifiTimeSynced = false;
}

bool syncClockFromSystemTime(bool markDirty) {
  struct tm timeInfo;
  if (!getLocalTime(&timeInfo, 10)) return false;

  clockYear = timeInfo.tm_year + 1900;
  clockMonth = timeInfo.tm_mon + 1;
  clockDay = timeInfo.tm_mday;
  clockHour = timeInfo.tm_hour;
  clockMinute = timeInfo.tm_min;
  normalizeClockDate();
  unsigned long now = millis();
  unsigned long secondOffsetMs = (unsigned long)clampVal(timeInfo.tm_sec, 0, 59) * 1000UL;
  clockLastMinuteMs = (now > secondOffsetMs) ? (now - secondOffsetMs) : now;
  wifiTimeSynced = true;
  wifiLastNtpSyncMs = now;
  if (rtcPresent) {
    // System clock's epoch is UTC regardless of the TZ env var (TZ only
    // affects getLocalTime()'s formatting above), so this is already the
    // right value to hand to the chip.
    ds3231_write_utc(time(nullptr));
  }
  if (markDirty) markSettingsDirty();
  return true;
}

// Connect, optionally targeting a specific AP. Passing channel>0 + bssid locks
// association to that exact access point (used for scan-selected networks and
// remembered reconnects); otherwise falls back to a plain SSID connect.
void startWifiConnectTo(const char* ssid, const char* pass, bool savePendingCredentials,
                        int channel, const uint8_t* bssid) {
  if (!wifiEnabled || !ssid || ssid[0] == '\0') return;
  ensureWifiRadioStarted();
  copySafe(pendingWifiSsid, sizeof(pendingWifiSsid), ssid);
  copySafe(pendingWifiPass, sizeof(pendingWifiPass), pass ? pass : "");
  wifiSavePendingCredentials = savePendingCredentials;
  wifiConnecting = true;
  wifiConnected = false;
  wifiConnectionFailed = false;
  wifiTimeSynced = false;
  wifiConnectStartMs = millis();
  wifiLastReconnectMs = wifiConnectStartMs;

  // ESP32-C5: associate with a plain WiFi.begin() and let the supplicant pick the
  // band — esp_wifi_set_band_mode(AUTO) is set in ensureWifiRadioStarted(). Pinning
  // the BSSID/channel from a scan proved unreliable on the C5: a stale or cross-band
  // target makes association fail. The scan now only feeds the network picker, not
  // the connect. (This mirrors the FlipperHTTP ESP32-C5 approach used in FlipSocial.)
  (void)channel;
  (void)bssid;

  WiFi.disconnect(false);
#if defined(AQUARIUM_BOARD_PANCAKE)
  Serial.printf("[WiFi] connecting '%s' (pw %d chars)\n",
                pendingWifiSsid, (int)strlen(pendingWifiPass));
#endif
  WiFi.begin(pendingWifiSsid, pendingWifiPass);
  lastWifiServiceMs = 0;
  setWifiStatus("Connecting...");
}

// Reconnect / saved-network entry point: no explicit AP, so it reuses the
// remembered BSSID for this SSID (if any) inside startWifiConnectTo.
void startWifiConnect(const char* ssid, const char* pass, bool savePendingCredentials) {
  startWifiConnectTo(ssid, pass, savePendingCredentials, 0, nullptr);
}

void setWifiEnabled(bool enabled) {
  if (wifiEnabled == enabled && enabled) {
    if (wifiSsid[0] != '\0' && !wifiConnected && !wifiConnecting) {
      startWifiConnect(wifiSsid, wifiPass, false);
    } else if (wifiSsid[0] == '\0' && !wifiScanInProgress) {
      startWifiScan();
    }
    return;
  }

  wifiEnabled = enabled;
  if (!wifiEnabled) {
    wifiScanInProgress = false;
    wifiConnecting = false;
    wifiConnected = false;
    wifiConnectionFailed = false;
    wifiSavePendingCredentials = false;
    wifiTimeConfigured = false;
    wifiTimeSynced = false;
    clockUseInternetTime = false;
    pendingWifiSsid[0] = '\0';
    pendingWifiPass[0] = '\0';
    clearWifiScanResults();
    WiFi.scanDelete();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiRadioStarted = false;
    lastWifiServiceMs = 0;
    setWifiStatus("Off");
    markSettingsDirty();
    return;
  }

  ensureWifiRadioStarted();
  setWifiStatus("Starting...");
  markSettingsDirty();
  if (wifiSsid[0] != '\0') {
    startWifiConnect(wifiSsid, wifiPass, false);
  } else {
    startWifiScan();
  }
}

void serviceInternetTime(unsigned long now) {
  if (!clockUseInternetTime || !wifiConnected) return;
  if (!wifiTimeConfigured) beginInternetTimeSync();

  bool needsRetry = !wifiTimeSynced && (wifiLastNtpAttemptMs == 0 || now - wifiLastNtpAttemptMs >= NTP_RETRY_MS);
  bool needsRefresh = wifiTimeSynced && (now - wifiLastNtpSyncMs >= NTP_REFRESH_MS);
  if (!needsRetry && !needsRefresh) return;

  wifiLastNtpAttemptMs = now;
  if (syncClockFromSystemTime(true)) {
    setWifiStatus("Connected");
  }
}

unsigned long wifiServiceIntervalMs() {
  if (wifiPanelOpen || wifiScanInProgress || wifiConnecting) return WIFI_SERVICE_ACTIVE_MS;
  if (wifiConnected && wifiTimeSynced) return WIFI_SERVICE_SYNCED_MS;
  if (wifiConnected) return WIFI_SERVICE_UNSYNCED_MS;
  return WIFI_SERVICE_IDLE_MS;
}

void serviceWifi(unsigned long now) {
  if (!wifiEnabled) {
    lastWifiServiceMs = 0;
    return;
  }

  unsigned long interval = wifiServiceIntervalMs();
  if (lastWifiServiceMs != 0 && now - lastWifiServiceMs < interval) return;
  lastWifiServiceMs = now;

  ensureWifiRadioStarted();
  finishWifiScanIfReady();

  bool connectedNow = (WiFi.status() == WL_CONNECTED);
  if (connectedNow) {
    if (!wifiConnected) {
      wifiConnected = true;
      wifiConnecting = false;
      wifiConnectionFailed = false;
      // Remember the AP we actually associated with so later reconnects target
      // the same BSSID/band instead of re-guessing on a combined 2.4/5 GHz SSID.
      if (const uint8_t* b = WiFi.BSSID()) {
        memcpy(lastGoodBssid, b, 6);
        lastGoodChannel = (uint8_t)WiFi.channel();
        copySafe(lastGoodSsid, sizeof(lastGoodSsid), pendingWifiSsid);
        lastGoodValid = true;
      }
      if (wifiSavePendingCredentials) {
        copySafe(wifiSsid, sizeof(wifiSsid), pendingWifiSsid);
        copySafe(wifiPass, sizeof(wifiPass), pendingWifiPass);
        wifiSavePendingCredentials = false;
      }
      clockUseInternetTime = true;
      beginInternetTimeSync();
      setWifiStatus("Connected");
      savePersistentState();
    }
    serviceInternetTime(now);
    return;
  }

  if (wifiConnected) {
    wifiConnected = false;
    wifiTimeConfigured = false;
    wifiTimeSynced = false;
    setWifiStatus("Disconnected");
  }

  if (wifiConnecting) {
    if (now - wifiConnectStartMs >= WIFI_CONNECT_TIMEOUT_MS) {
      wifiConnecting = false;
      wifiConnectionFailed = true;
      wifiSavePendingCredentials = false;
      // The remembered AP may have roamed/changed band; drop it so the next
      // reconnect attempt falls back to a plain scan-and-connect.
      lastGoodValid = false;
      WiFi.disconnect(false);
      setWifiStatus("Connect failed");
    }
    return;
  }

  if (wifiScanInProgress) return;   // let an in-flight scan finish before (re)connecting

  if (wifiSsid[0] != '\0') {
    if (wifiLastReconnectMs == 0 || now - wifiLastReconnectMs >= WIFI_RECONNECT_DELAY_MS) {
      int idx = wifiFindListed(wifiSsid);
      if (idx >= 0) {
        // Found our saved network in the scan — target its strongest AP/band.
        startWifiConnectTo(wifiSsid, wifiPass, false, wifiNetworkChannel[idx], wifiNetworkBssid[idx]);
      } else if (lastGoodValid || wifiNetworkCount > 0) {
        // Reuse the remembered AP, or (scanned but not seen) fall back to plain connect.
        startWifiConnect(wifiSsid, wifiPass, false);
      } else {
        // Cold boot with no scan data yet: discover the AP's band/BSSID first.
        startWifiScan();
      }
    }
  } else if (wifiNetworkCount == 0) {
    startWifiScan();
  }
}

uint16_t randomFlowerColor(int index) {
  if (index == kPixelFlowerCount - 1) {
    return RGB565((uint8_t)random(92, 146), (uint8_t)random(84, 126), (uint8_t)random(8, 28));
  }
  return RGB565((uint8_t)random(0, 10), (uint8_t)random(18, 38), (uint8_t)random(46, 96));
}

void randomizeFlowers() {
  pixelFlowers[0].cx = random(42, 98);
  pixelFlowers[0].cy = random(50, 86);
  pixelFlowers[0].radius = random(50, 63);
  pixelFlowers[0].rotation = frand(0.0f, 6.28318f);
  pixelFlowers[0].color = randomFlowerColor(0);

  pixelFlowers[1].cx = random(214, 278);
  pixelFlowers[1].cy = random(48, 90);
  pixelFlowers[1].radius = random(50, 63);
  pixelFlowers[1].rotation = frand(0.0f, 6.28318f);
  pixelFlowers[1].color = randomFlowerColor(1);

  pixelFlowers[2].cx = random(162, 248);
  pixelFlowers[2].cy = random(124, SEA_LEVEL_Y - 18);
  pixelFlowers[2].radius = random(20, 34);
  pixelFlowers[2].rotation = frand(0.0f, 6.28318f);
  pixelFlowers[2].color = randomFlowerColor(2);
  pixelFlowerGeometryDirty = true;
  markSettingsDirty();
}

void drawThickLine(TFT_eSprite& s, int x0, int y0, int x1, int y1, uint16_t color) {
  s.drawLine(x0, y0, x1, y1, color);
  s.drawLine(x0 + 1, y0, x1 + 1, y1, color);
  s.drawLine(x0, y0 + 1, x1, y1 + 1, color);
  s.drawLine(x0 - 1, y0, x1 - 1, y1, color);
  s.drawLine(x0, y0 - 1, x1, y1 - 1, color);
}

void drawActionRow(TFT_eSprite& s, int rowY, const char* label, const char* actionLabel) {
  const int centerY = rowY + SETTINGS_BUTTON_H / 2;
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(label, SETTINGS_PANEL_X + 12, centerY);
  drawButton(s, SETTINGS_ACTION_X, rowY, SETTINGS_ACTION_W, SETTINGS_BUTTON_H, actionLabel, TFT_CYAN, TFT_DARKGREEN);
}

void rebuildPixelFlowerGeometry() {
  if (!pixelFlowerGeometryDirty) return;

  for (int flowerIndex = 0; flowerIndex < kPixelFlowerCount; ++flowerIndex) {
    const PixelFlowerSpec& flower = pixelFlowers[flowerIndex];
    for (int i = 0; i <= PIXEL_FLOWER_SEGMENTS; ++i) {
      float theta = flower.rotation + (6.28318f * i) / PIXEL_FLOWER_SEGMENTS;
      float petal = 0.5f + 0.5f * sinf(theta * 5.0f);
      float r = flower.radius * (0.58f + 0.42f * petal);
      pixelFlowerPoints[flowerIndex][i].x = (int16_t)(flower.cx + (int)(cosf(theta) * r));
      pixelFlowerPoints[flowerIndex][i].y = (int16_t)(flower.cy + (int)(sinf(theta) * r));
    }
  }

  pixelFlowerGeometryDirty = false;
}

void drawPixelFlower(TFT_eSprite& s, int flowerIndex, uint16_t color) {
  for (int i = 1; i <= PIXEL_FLOWER_SEGMENTS; ++i) {
    const PixelFlowerPoint& prev = pixelFlowerPoints[flowerIndex][i - 1];
    const PixelFlowerPoint& point = pixelFlowerPoints[flowerIndex][i];
    drawThickLine(s, prev.x, prev.y, point.x, point.y, color);
  }
}

void drawFlowerBackground(TFT_eSprite& s) {
  clearRenderSurface(s);
  rebuildPixelFlowerGeometry();
  for (int i = 0; i < kPixelFlowerCount; ++i) {
    drawPixelFlower(s, i, pixelFlowers[i].color);
  }
}

void drawBackground(TFT_eSprite& s, float tSec) {
  (void)tSec;
  switch (backgroundStyle) {
    case BACKGROUND_STYLE_BLACK:
      clearRenderSurface(s);
      break;
    case BACKGROUND_STYLE_DITHERED:
      drawCachedTopGradientBackground(s, BACKGROUND_STYLE_DITHERED, activeBackgroundGradientColor(),
                                      GRADIENT_DITHER_ORDERED, 4, 28);
      break;
    case BACKGROUND_STYLE_SMOOTH:
      drawCachedTopGradientBackground(s, BACKGROUND_STYLE_SMOOTH, activeBackgroundGradientColor(),
                                      GRADIENT_DITHER_NONE, 1, 0);
      break;
    case BACKGROUND_STYLE_FLOWERS:
      drawFlowerBackground(s);
      break;
    default:
      clearRenderSurface(s);
      break;
  }
}

// ------------------------------ Aquarium Logic -------------------------------
void noteFeedPelletSpawned() {
  bool crossedThreshold = false;
  if (cthulhuFeedPelletCount < CTHULHU_FEED_PELLET_THRESHOLD) {
    cthulhuFeedPelletCount++;
    crossedThreshold = (cthulhuFeedPelletCount >= CTHULHU_FEED_PELLET_THRESHOLD);
  }

  if (crossedThreshold) {
    markSettingsDirty();
  }

  if (cthulhuFeedPelletCount >= CTHULHU_FEED_PELLET_THRESHOLD && !octopus.active) {
    unsigned long soon = aquariumNowMs + (unsigned long)random(6000, 18000);
    if (octopus.nextSpawnMs == 0 || (long)(soon - octopus.nextSpawnMs) < 0) {
      octopus.nextSpawnMs = soon;
    }
  }
}

void spawnFlake(float x, float y) {
  for (int i = 0; i < MAX_FLAKES; i++) {
    if (!flakes[i].active) {
      flakes[i].active = true;
      flakes[i].x = x;
      flakes[i].y = y;
      flakes[i].vy = frand(22.0f, 48.0f);
      flakes[i].color = randomFoodColor();
      noteFeedPelletSpawned();
      return;
    }
  }
}

float randomFishDepthBrightness() {
  int roll = random(100);
  if (roll < 28) return frand(0.48f, 0.64f);
  if (roll < 70) return frand(0.66f, 0.84f);
  return frand(0.88f, 1.0f);
}

void refreshFishRenderColor(Fish& f) {
  f.renderColor = scaleRgb565(f.displayColor, f.depthBrightness);
}

void refreshFishDepth(Fish& f) {
  f.depthBrightness = randomFishDepthBrightness();
  refreshFishRenderColor(f);
}

void activateFish(Fish& f, bool activeNow) {
  f.active = activeNow;
  if (!activeNow) return;
  f.type = random(0, GLYPH_COUNT);
  int rightWidth = fishGlyphWidthRight[f.type];
  int leftWidth = fishGlyphWidthLeft[f.type];
  f.visualWidth = (rightWidth > leftWidth) ? rightWidth : leftWidth;
  if (f.visualWidth <= 0) f.visualWidth = (int)strlen(fishSpecies[f.type].right) * 12;
  f.displayColor = fishSpecies[f.type].baseColor;
  if (random(100) < 20) {
    f.displayColor = kAltFishColors[random(0, kAltFishColorCount)];
  }
  refreshFishDepth(f);
  f.x = frand(-42, SCREEN_W + 12);  // allow natural side entry
  f.y = frand(20, SEA_LEVEL_Y - 10);
  f.vx = frand(-1.0f, 1.0f);
  f.vy = frand(-0.5f, 0.5f);
  f.speed = frand(14.0f, 30.0f);
  f.phase = frand(0.0f, 6.28318f);
  f.wanderBias = frand(0.4f, 1.3f);
}

void applyFishPopulation() {
  fishTargetCount = clampVal(fishTargetCount, MIN_FISH, MAX_FISH);
  for (int i = 0; i < MAX_FISH_POOL; i++) {
    bool shouldBeActive = (i < fishTargetCount);
    if (shouldBeActive && !fishPool[i].active) activateFish(fishPool[i], true);
    if (!shouldBeActive && fishPool[i].active) fishPool[i].active = false;
  }
}

bool fishSpawnClear(int fishIndex, float x, float y, float minGapX, float minGapY) {
  Fish& f = fishPool[fishIndex];
  float centerX = x + f.visualWidth * 0.5f;
  float centerY = y + FISH_CENTER_Y_OFFSET;
  int fishCount = activeFishLimit();
  for (int i = 0; i < fishCount; ++i) {
    if (i == fishIndex || !fishPool[i].active) continue;
    Fish& other = fishPool[i];
    float otherCenterX = other.x + other.visualWidth * 0.5f;
    float otherCenterY = other.y + FISH_CENTER_Y_OFFSET;
    if (fabsf(otherCenterX - centerX) < minGapX && fabsf(otherCenterY - centerY) < minGapY) return false;
  }
  return true;
}

void spreadInitialFishLayout() {
  int fishCount = activeFishLimit();
  float minGapX = FISH_AVOID_RADIUS_X * 0.92f;
  float minGapY = FISH_AVOID_RADIUS_Y * 1.05f;
  for (int i = 0; i < fishCount; ++i) {
    Fish& f = fishPool[i];
    if (!f.active) continue;

    float bestX = f.x;
    float bestY = f.y;
    bool placed = false;
    for (int attempt = 0; attempt < 80; ++attempt) {
      float candidateX = frand(10.0f, SCREEN_W - f.visualWidth - 10.0f);
      float candidateY = frand(18.0f, SEA_LEVEL_Y - 18.0f);
      bestX = candidateX;
      bestY = candidateY;
      if (fishSpawnClear(i, candidateX, candidateY, minGapX, minGapY)) {
        placed = true;
        break;
      }
    }

    f.x = bestX;
    f.y = bestY;
    if (!placed) {
      f.y = clampVal(f.y + frand(-8.0f, 8.0f), 18.0f, (float)SEA_LEVEL_Y - 18.0f);
    }
    f.vx = (random(100) < 50) ? -1.0f : 1.0f;
    f.vy = frand(-0.22f, 0.22f);
  }
}

void respawnFishPopulation() {
  fishTargetCount = clampVal(fishTargetCount, MIN_FISH, MAX_FISH);
  int fishCount = activeFishLimit();
  for (int i = 0; i < MAX_FISH_POOL; ++i) {
    activateFish(fishPool[i], i < fishCount);
  }
  spreadInitialFishLayout();
}

void resetBubble(Bubble& b, bool spreadOut) {
  b.active = true;
  b.baseX = frand(8.0f, SCREEN_W - 8.0f);
  b.x = b.baseX;
  b.y = spreadOut ? frand(4.0f, SCREEN_H + 48.0f) : frand(SCREEN_H - 4.0f, SCREEN_H + 48.0f);
  b.vy = frand(12.0f, 28.0f);
  b.phase = frand(0.0f, 6.28318f);
  b.swayAmp = frand(2.0f, 7.0f);
  b.color = randomBubbleColor();
}

void applyBubblePopulation(bool spreadNew = false) {
  bubbleTargetCount = clampVal(bubbleTargetCount, MIN_BUBBLES, MAX_BUBBLES);
  for (int i = 0; i < MAX_BUBBLES; i++) {
    bool shouldBeActive = (i < bubbleTargetCount);
    if (shouldBeActive && !bubbles[i].active) resetBubble(bubbles[i], spreadNew);
    if (!shouldBeActive && bubbles[i].active) bubbles[i].active = false;
  }
}

void updateFlakes(float dt) {
  float t = aquariumTimeSec();
  for (int i = 0; i < MAX_FLAKES; i++) {
    if (!flakes[i].active) continue;
    flakes[i].y += flakes[i].vy * dt;
    flakes[i].x += sinf(t * 1.2f + i) * 8.0f * dt;
    if (flakes[i].y > SEA_LEVEL_Y) flakes[i].active = false;
  }
}

void updateBubbles(float dt) {
  float t = aquariumTimeSec();
  int bubbleCount = activeBubbleLimit();
  for (int i = 0; i < bubbleCount; i++) {
    if (!bubbles[i].active) continue;
    bubbles[i].y -= bubbles[i].vy * dt;
    bubbles[i].x = bubbles[i].baseX + sinf(t * 1.8f + bubbles[i].phase) * bubbles[i].swayAmp;
    if (bubbles[i].y < -10.0f) resetBubble(bubbles[i], false);
  }
}

int closestFlakeForFish(const Fish& f, float maxDist) {
  int best = -1;
  float bestD2 = maxDist * maxDist;
  for (int i = 0; i < MAX_FLAKES; i++) {
    if (!flakes[i].active) continue;
    float dx = flakes[i].x - f.x;
    float dy = flakes[i].y - f.y;
    float d2 = dx * dx + dy * dy;
    if (d2 < bestD2) {
      bestD2 = d2;
      best = i;
    }
  }
  return best;
}

void steerFishAwayFromOctopus(Fish& f, float fishCenterX, float fishCenterY, float dt) {
  if (!octopus.active) return;

  float dx = fishCenterX - octopus.x;
  float dy = fishCenterY - (octopus.y + OCTOPUS_CENTER_Y_OFFSET);
  float radiusX = octopus.cthulhu ? OCTOPUS_FISH_AVOID_RADIUS_X * CTHULHU_FISH_AVOID_RADIUS_SCALE
                                  : OCTOPUS_FISH_AVOID_RADIUS_X;
  float radiusY = octopus.cthulhu ? OCTOPUS_FISH_AVOID_RADIUS_Y * CTHULHU_FISH_AVOID_RADIUS_SCALE
                                  : OCTOPUS_FISH_AVOID_RADIUS_Y;
  float strength = octopus.cthulhu ? OCTOPUS_FISH_AVOID_STRENGTH * CTHULHU_FISH_AVOID_STRENGTH_SCALE
                                   : OCTOPUS_FISH_AVOID_STRENGTH;
  float sx = dx / radiusX;
  float sy = dy / radiusY;
  float scaledD2 = sx * sx + sy * sy;
  if (scaledD2 <= 0.0001f || scaledD2 >= 1.0f) return;

  float dist = sqrtf(dx * dx + dy * dy) + 0.0001f;
  float push = 1.0f - scaledD2;
  push *= push;
  f.vx += (dx / dist) * push * strength * dt;
  f.vy += (dy / dist) * push * strength * dt;
}

void steerFishAwayFromSeahorse(Fish& f, float fishCenterX, float fishCenterY, float dt) {
  if (!seahorse.active) return;

  float horseCenterX = seahorse.x + SEAHORSE_CENTER_X_OFFSET;
  float horseCenterY = seahorse.y + SEAHORSE_CENTER_Y_OFFSET;
  float dx = fishCenterX - horseCenterX;
  float dy = fishCenterY - horseCenterY;
  float sx = dx / SEAHORSE_FISH_AVOID_RADIUS_X;
  float sy = dy / SEAHORSE_FISH_AVOID_RADIUS_Y;
  float scaledD2 = sx * sx + sy * sy;
  if (scaledD2 <= 0.0001f || scaledD2 >= 1.0f) return;

  float dist = sqrtf(dx * dx + dy * dy) + 0.0001f;
  float push = 1.0f - scaledD2;
  push *= push;
  f.vx += (dx / dist) * push * SEAHORSE_FISH_AVOID_STRENGTH * dt;
  f.vy += (dy / dist) * push * SEAHORSE_FISH_AVOID_STRENGTH * dt;
}

void steerFishAwayFromSnail(Fish& f, float fishCenterX, float fishCenterY, float dt) {
  if (!snail.active) return;

  float snailCenterX = snail.x + SNAIL_CENTER_X_OFFSET;
  float snailCenterY = snail.y + SNAIL_CENTER_Y_OFFSET;
  float dx = fishCenterX - snailCenterX;
  float dy = fishCenterY - snailCenterY;
  float sx = dx / SNAIL_FISH_AVOID_RADIUS_X;
  float sy = dy / SNAIL_FISH_AVOID_RADIUS_Y;
  float scaledD2 = sx * sx + sy * sy;
  if (scaledD2 <= 0.0001f || scaledD2 >= 1.0f) return;

  float dist = sqrtf(dx * dx + dy * dy) + 0.0001f;
  float push = 1.0f - scaledD2;
  push *= push;
  f.vx += (dx / dist) * push * SNAIL_FISH_AVOID_STRENGTH * dt;
  f.vy += (dy / dist) * push * SNAIL_FISH_AVOID_STRENGTH * dt;
}

void steerFishAwayFromJellyfish(Fish& f, float fishCenterX, float fishCenterY, float dt) {
  if (!jellyfish.active) return;

  float jellyCenterX = jellyfish.x + JELLYFISH_CENTER_X_OFFSET;
  float jellyCenterY = jellyfish.y + JELLYFISH_CENTER_Y_OFFSET;
  float dx = fishCenterX - jellyCenterX;
  float dy = fishCenterY - jellyCenterY;
  float sx = dx / JELLYFISH_FISH_AVOID_RADIUS_X;
  float sy = dy / JELLYFISH_FISH_AVOID_RADIUS_Y;
  float scaledD2 = sx * sx + sy * sy;
  if (scaledD2 <= 0.0001f || scaledD2 >= 1.0f) return;

  float dist = sqrtf(dx * dx + dy * dy) + 0.0001f;
  float push = 1.0f - scaledD2;
  push *= push;
  f.vx += (dx / dist) * push * JELLYFISH_FISH_AVOID_STRENGTH * dt;
  f.vy += (dy / dist) * push * JELLYFISH_FISH_AVOID_STRENGTH * dt;
}

void steerFishAwayFromSquid(Fish& f, float fishCenterX, float fishCenterY, float dt) {
  if (!squid.active) return;

  float squidCenterX = squid.x + SQUID_CENTER_X_OFFSET;
  float squidCenterY = squid.y + SQUID_CENTER_Y_OFFSET;
  float dx = fishCenterX - squidCenterX;
  float dy = fishCenterY - squidCenterY;
  float sx = dx / SQUID_FISH_AVOID_RADIUS_X;
  float sy = dy / SQUID_FISH_AVOID_RADIUS_Y;
  float scaledD2 = sx * sx + sy * sy;
  if (scaledD2 <= 0.0001f || scaledD2 >= 1.0f) return;

  float dist = sqrtf(dx * dx + dy * dy) + 0.0001f;
  float push = 1.0f - scaledD2;
  push *= push;
  f.vx += (dx / dist) * push * SQUID_FISH_AVOID_STRENGTH * dt;
  f.vy += (dy / dist) * push * SQUID_FISH_AVOID_STRENGTH * dt;
}

void keepFishOutsideOctopus(Fish& f) {
  if (!octopus.active) return;

  float fishCenterX = f.x + f.visualWidth * 0.5f;
  float fishCenterY = f.y + FISH_CENTER_Y_OFFSET;
  float octoCenterY = octopus.y + OCTOPUS_CENTER_Y_OFFSET;
  float dx = fishCenterX - octopus.x;
  float dy = fishCenterY - octoCenterY;
  float radiusX = octopus.cthulhu ? OCTOPUS_FISH_CLEAR_RADIUS_X * CTHULHU_FISH_CLEAR_RADIUS_SCALE
                                  : OCTOPUS_FISH_CLEAR_RADIUS_X;
  float radiusY = octopus.cthulhu ? OCTOPUS_FISH_CLEAR_RADIUS_Y * CTHULHU_FISH_CLEAR_RADIUS_SCALE
                                  : OCTOPUS_FISH_CLEAR_RADIUS_Y;
  float sx = dx / radiusX;
  float sy = dy / radiusY;
  float scaledD2 = sx * sx + sy * sy;
  if (scaledD2 >= 1.0f) return;

  if (scaledD2 <= 0.0001f) {
    dx = (f.vx >= 0.0f) ? 1.0f : -1.0f;
    dy = (f.vy >= 0.0f) ? 0.35f : -0.35f;
    scaledD2 = (dx / radiusX) * (dx / radiusX) + (dy / radiusY) * (dy / radiusY);
  }

  float scale = 1.0f / sqrtf(scaledD2);
  float targetCenterX = octopus.x + dx * scale;
  float targetCenterY = octoCenterY + dy * scale;
  f.x += (targetCenterX - fishCenterX) * 0.55f;
  f.y += (targetCenterY - fishCenterY) * 0.55f;
}

void keepFishOutsideSeahorse(Fish& f) {
  if (!seahorse.active) return;

  float fishCenterX = f.x + f.visualWidth * 0.5f;
  float fishCenterY = f.y + FISH_CENTER_Y_OFFSET;
  float horseCenterX = seahorse.x + SEAHORSE_CENTER_X_OFFSET;
  float horseCenterY = seahorse.y + SEAHORSE_CENTER_Y_OFFSET;
  float dx = fishCenterX - horseCenterX;
  float dy = fishCenterY - horseCenterY;
  float sx = dx / SEAHORSE_FISH_CLEAR_RADIUS_X;
  float sy = dy / SEAHORSE_FISH_CLEAR_RADIUS_Y;
  float scaledD2 = sx * sx + sy * sy;
  if (scaledD2 >= 1.0f) return;

  if (scaledD2 <= 0.0001f) {
    dx = (f.vx >= 0.0f) ? 1.0f : -1.0f;
    dy = (f.vy >= 0.0f) ? 0.35f : -0.35f;
    scaledD2 = (dx / SEAHORSE_FISH_CLEAR_RADIUS_X) * (dx / SEAHORSE_FISH_CLEAR_RADIUS_X) +
               (dy / SEAHORSE_FISH_CLEAR_RADIUS_Y) * (dy / SEAHORSE_FISH_CLEAR_RADIUS_Y);
  }

  float scale = 1.0f / sqrtf(scaledD2);
  float targetCenterX = horseCenterX + dx * scale;
  float targetCenterY = horseCenterY + dy * scale;
  f.x += (targetCenterX - fishCenterX) * 0.45f;
  f.y += (targetCenterY - fishCenterY) * 0.45f;
}

void keepFishOutsideSnail(Fish& f) {
  if (!snail.active) return;

  float fishCenterX = f.x + f.visualWidth * 0.5f;
  float fishCenterY = f.y + FISH_CENTER_Y_OFFSET;
  float snailCenterX = snail.x + SNAIL_CENTER_X_OFFSET;
  float snailCenterY = snail.y + SNAIL_CENTER_Y_OFFSET;
  float dx = fishCenterX - snailCenterX;
  float dy = fishCenterY - snailCenterY;
  float sx = dx / SNAIL_FISH_CLEAR_RADIUS_X;
  float sy = dy / SNAIL_FISH_CLEAR_RADIUS_Y;
  float scaledD2 = sx * sx + sy * sy;
  if (scaledD2 >= 1.0f) return;

  if (scaledD2 <= 0.0001f) {
    dx = (f.vx >= 0.0f) ? 1.0f : -1.0f;
    dy = -0.45f;
    scaledD2 = (dx / SNAIL_FISH_CLEAR_RADIUS_X) * (dx / SNAIL_FISH_CLEAR_RADIUS_X) +
               (dy / SNAIL_FISH_CLEAR_RADIUS_Y) * (dy / SNAIL_FISH_CLEAR_RADIUS_Y);
  }

  float scale = 1.0f / sqrtf(scaledD2);
  float targetCenterX = snailCenterX + dx * scale;
  float targetCenterY = snailCenterY + dy * scale;
  f.x += (targetCenterX - fishCenterX) * 0.35f;
  f.y += (targetCenterY - fishCenterY) * 0.55f;
}

void keepFishOutsideJellyfish(Fish& f) {
  if (!jellyfish.active) return;

  float fishCenterX = f.x + f.visualWidth * 0.5f;
  float fishCenterY = f.y + FISH_CENTER_Y_OFFSET;
  float jellyCenterX = jellyfish.x + JELLYFISH_CENTER_X_OFFSET;
  float jellyCenterY = jellyfish.y + JELLYFISH_CENTER_Y_OFFSET;
  float dx = fishCenterX - jellyCenterX;
  float dy = fishCenterY - jellyCenterY;
  float sx = dx / JELLYFISH_FISH_CLEAR_RADIUS_X;
  float sy = dy / JELLYFISH_FISH_CLEAR_RADIUS_Y;
  float scaledD2 = sx * sx + sy * sy;
  if (scaledD2 >= 1.0f) return;

  if (scaledD2 <= 0.0001f) {
    dx = (f.vx >= 0.0f) ? 1.0f : -1.0f;
    dy = (f.vy >= 0.0f) ? 0.55f : -0.55f;
    scaledD2 = (dx / JELLYFISH_FISH_CLEAR_RADIUS_X) * (dx / JELLYFISH_FISH_CLEAR_RADIUS_X) +
               (dy / JELLYFISH_FISH_CLEAR_RADIUS_Y) * (dy / JELLYFISH_FISH_CLEAR_RADIUS_Y);
  }

  float scale = 1.0f / sqrtf(scaledD2);
  float targetCenterX = jellyCenterX + dx * scale;
  float targetCenterY = jellyCenterY + dy * scale;
  f.x += (targetCenterX - fishCenterX) * 0.38f;
  f.y += (targetCenterY - fishCenterY) * 0.38f;
}

void keepFishOutsideSquid(Fish& f) {
  if (!squid.active) return;

  float fishCenterX = f.x + f.visualWidth * 0.5f;
  float fishCenterY = f.y + FISH_CENTER_Y_OFFSET;
  float squidCenterX = squid.x + SQUID_CENTER_X_OFFSET;
  float squidCenterY = squid.y + SQUID_CENTER_Y_OFFSET;
  float dx = fishCenterX - squidCenterX;
  float dy = fishCenterY - squidCenterY;
  float sx = dx / SQUID_FISH_CLEAR_RADIUS_X;
  float sy = dy / SQUID_FISH_CLEAR_RADIUS_Y;
  float scaledD2 = sx * sx + sy * sy;
  if (scaledD2 >= 1.0f) return;

  if (scaledD2 <= 0.0001f) {
    dx = (f.vx >= 0.0f) ? 1.0f : -1.0f;
    dy = (f.vy >= 0.0f) ? 0.4f : -0.4f;
    scaledD2 = (dx / SQUID_FISH_CLEAR_RADIUS_X) * (dx / SQUID_FISH_CLEAR_RADIUS_X) +
               (dy / SQUID_FISH_CLEAR_RADIUS_Y) * (dy / SQUID_FISH_CLEAR_RADIUS_Y);
  }

  float scale = 1.0f / sqrtf(scaledD2);
  float targetCenterX = squidCenterX + dx * scale;
  float targetCenterY = squidCenterY + dy * scale;
  f.x += (targetCenterX - fishCenterX) * 0.40f;
  f.y += (targetCenterY - fishCenterY) * 0.35f;
}

void updateFish(float dt) {
  const float t = aquariumTimeSec();
  int fishCount = activeFishLimit();
  float centerX[MAX_FISH_POOL];
  float centerY[MAX_FISH_POOL];
  for (int i = 0; i < fishCount; ++i) {
    Fish& f = fishPool[i];
    if (!f.active) continue;
    centerX[i] = f.x + f.visualWidth * 0.5f;
    centerY[i] = f.y + FISH_CENTER_Y_OFFSET;
  }

  for (int i = 0; i < fishCount; i++) {
    Fish& f = fishPool[i];
    if (!f.active) continue;

    // Wander behavior
    float wanderX = cosf(f.phase + t * 0.9f) * 0.45f * f.wanderBias;
    float wanderY = sinf(f.phase * 1.7f + t * 0.7f) * 0.22f;
    f.vx += wanderX * dt;
    f.vy += wanderY * dt;

    // Schooling with same type (alignment + cohesion)
    float avgVX = 0, avgVY = 0, cx = 0, cy = 0;
    int nearCount = 0;
    float repelX = 0.0f;
    float repelY = 0.0f;
    int repelCount = 0;
    float fCenterX = centerX[i];
    float fCenterY = centerY[i];
    if (fishAvoidanceEnabled()) {
      for (int j = 0; j < fishCount; j++) {
        if (i == j || !fishPool[j].active) continue;
        Fish& n = fishPool[j];

        float dx = n.x - f.x;
        float dy = n.y - f.y;
        if (n.type == f.type) {
          float d2 = dx * dx + dy * dy;
          if (d2 < 3600.0f) {  // within ~60px
            avgVX += n.vx;
            avgVY += n.vy;
            cx += n.x;
            cy += n.y;
            nearCount++;
          }
        }

        float sdx = centerX[j] - fCenterX;
        float sdy = centerY[j] - fCenterY;
        if (sdx > SCREEN_W * 0.5f) sdx -= SCREEN_W;
        if (sdx < -SCREEN_W * 0.5f) sdx += SCREEN_W;

        float sx = sdx / FISH_AVOID_RADIUS_X;
        float sy = sdy / FISH_AVOID_RADIUS_Y;
        float scaledD2 = sx * sx + sy * sy;
        if (scaledD2 > 0.0001f && scaledD2 < 1.0f) {
          float dist = sqrtf(sdx * sdx + sdy * sdy) + 0.0001f;
          float push = 1.0f - scaledD2;
          push *= push;
          repelX -= (sdx / dist) * push;
          repelY -= (sdy / dist) * push;
          repelCount++;
        }
      }
    }
    if (nearCount > 0) {
      avgVX /= nearCount;
      avgVY /= nearCount;
      cx /= nearCount;
      cy /= nearCount;
      f.vx += (avgVX - f.vx) * 0.45f * dt;
      f.vy += (avgVY - f.vy) * 0.25f * dt;
      f.vx += (cx - f.x) * 0.0018f;
      f.vy += (cy - f.y) * 0.0012f;
    }

    // Feed-seeking behavior
    int fi = closestFlakeForFish(f, 140.0f);
    if (fi >= 0) {
      float dx = flakes[fi].x - f.x;
      float dy = flakes[fi].y - f.y;
      float d = sqrtf(dx * dx + dy * dy) + 0.0001f;
      f.vx += (dx / d) * 0.95f * dt;
      f.vy += (dy / d) * 0.95f * dt;
      if (d < 8.0f) flakes[fi].active = false;  // "eat"
    }

    if (fishAvoidanceEnabled()) {
      steerFishAwayFromOctopus(f, fCenterX, fCenterY, dt);
      steerFishAwayFromSeahorse(f, fCenterX, fCenterY, dt);
      steerFishAwayFromSnail(f, fCenterX, fCenterY, dt);
      steerFishAwayFromJellyfish(f, fCenterX, fCenterY, dt);
      if (SQUID_ENABLED) steerFishAwayFromSquid(f, fCenterX, fCenterY, dt);
    }

    // Gentle steering separation, not hard collision.
    if (repelCount > 0) {
      f.vx += (repelX / repelCount) * FISH_AVOID_STRENGTH * dt;
      f.vy += (repelY / repelCount) * FISH_AVOID_STRENGTH * dt;
    }

    // Vertical edge avoidance only (horizontal uses wraparound)
    if (f.y < 18) f.vy += 0.8f * dt;
    if (f.y > SEA_LEVEL_Y - 8) f.vy -= 0.8f * dt;

    // Normalize velocity and apply speed
    float mag = sqrtf(f.vx * f.vx + f.vy * f.vy);
    if (mag < 0.0001f) {
      f.vx = 1.0f;
      f.vy = 0.0f;
      mag = 1.0f;
    }
    f.vx /= mag;
    f.vy /= mag;

    float fishSpeed = f.speed + sinf(t * 3.2f + f.phase) * 4.0f;
    f.x += f.vx * fishSpeed * dt;
    f.y += f.vy * fishSpeed * dt;
    if (fishAvoidanceEnabled()) {
      keepFishOutsideOctopus(f);
      keepFishOutsideSeahorse(f);
      keepFishOutsideSnail(f);
      keepFishOutsideJellyfish(f);
      if (SQUID_ENABLED) keepFishOutsideSquid(f);
    }

    // Horizontal wrap keeps fish flowing off-screen and re-entering smoothly
    int w = fishVisualWidth(f);
    float wrapPad = (float)w + 10.0f;
    if (f.x > SCREEN_W + wrapPad) {
      f.x = -wrapPad;
      refreshFishDepth(f);
    }
    if (f.x < -wrapPad) {
      f.x = SCREEN_W + wrapPad;
      refreshFishDepth(f);
    }
    f.y = clampVal(f.y, 14.0f, (float)SEA_LEVEL_Y - 6.0f);
  }
}

bool timeReached(unsigned long now, unsigned long target) {
  return (long)(now - target) >= 0;
}

void startAutoFeedSprinkle(unsigned long now) {
  autoFeedSprinkleActive = true;
  autoFeedSprinkleLeftToRight = (random(100) < 50);
  autoFeedSprinkleDropped = 0;
  autoFeedSprinkleNextMs = now;

  unsigned long duration = random(AUTO_FEED_SPRINKLE_MIN_MS, AUTO_FEED_SPRINKLE_MAX_MS + 1);
  autoFeedSprinkleIntervalMs = duration / (AUTO_FEED_SPRINKLE_COUNT - 1);
  if (autoFeedSprinkleIntervalMs < 120UL) autoFeedSprinkleIntervalMs = 120UL;
}

void dropNextAutoFeedFlake() {
  float progress = (float)autoFeedSprinkleDropped / (float)(AUTO_FEED_SPRINKLE_COUNT - 1);
  if (!autoFeedSprinkleLeftToRight) progress = 1.0f - progress;

  float x = 16.0f + progress * ((float)SCREEN_W - 32.0f) + frand(-5.0f, 5.0f);
  float y = frand(8.0f, 22.0f);
  spawnFlake(clampVal(x, 10.0f, (float)SCREEN_W - 10.0f), y);
}

void serviceAutoFeedSprinkle(unsigned long now) {
  if (!autoFeedSprinkleActive || !timeReached(now, autoFeedSprinkleNextMs)) return;

  dropNextAutoFeedFlake();
  autoFeedSprinkleDropped++;
  if (autoFeedSprinkleDropped >= AUTO_FEED_SPRINKLE_COUNT) {
    autoFeedSprinkleActive = false;
    return;
  }

  autoFeedSprinkleNextMs += autoFeedSprinkleIntervalMs;
  if (timeReached(now, autoFeedSprinkleNextMs)) {
    autoFeedSprinkleNextMs = now + autoFeedSprinkleIntervalMs;
  }
}

void serviceAutoFeed(unsigned long now) {
  int frequency = normalizeAutoFeedFrequency(autoFeedFrequency);
  if (frequency <= 0) {
    nextAutoFeedMs = 0;
    autoFeedSprinkleActive = false;
    return;
  }

  serviceAutoFeedSprinkle(now);

  if (nextAutoFeedMs == 0) {
    nextAutoFeedMs = now + autoFeedIntervalMs();
    return;
  }

  if (!timeReached(now, nextAutoFeedMs)) return;
  startAutoFeedSprinkle(now);
  serviceAutoFeedSprinkle(now);

  unsigned long interval = autoFeedIntervalMs();
  nextAutoFeedMs += interval;
  if (timeReached(now, nextAutoFeedMs)) nextAutoFeedMs = now + interval;
}

void scheduleOctopusSpawn(unsigned long now) {
  octopus.nextSpawnMs = now + octopusSpawnIntervalMs();
}

bool shouldSpawnCthulhu() {
  if (cthulhuFeedPelletCount >= CTHULHU_FEED_PELLET_THRESHOLD) {
    cthulhuFeedPelletCount = 0;
    markSettingsDirty();
    return true;
  }

  if (random(100) < CTHULHU_SPAWN_CHANCE_PERCENT) {
    cthulhuFeedPelletCount = 0;
    markSettingsDirty();
    return true;
  }

  return false;
}

void spawnOctopus(unsigned long now) {
  bool fromLeft = (random(100) < 50);
  octopus.active = true;
  octopus.cthulhu = shouldSpawnCthulhu();
  octopus.vx = fromLeft ? frand(4.5f, 8.0f) : -frand(4.5f, 8.0f);
  octopus.x = fromLeft ? -OCTOPUS_EXIT_PAD : (SCREEN_W + OCTOPUS_EXIT_PAD);
  octopus.baseY = frand(36.0f, (float)SEA_LEVEL_Y - 48.0f);
  octopus.y = octopus.baseY;
  octopus.phase = frand(0.0f, 6.28318f);
  octopus.colorPhase = frand(0.0f, 6.28318f);
  scheduleOctopusSpawn(now);
}

void spawnOctopusAtCenter(unsigned long now) {
  octopus.active = true;
  octopus.cthulhu = false;
  octopus.x = SCREEN_W * 0.5f;
  octopus.baseY = SEA_LEVEL_Y * 0.55f;
  octopus.y = octopus.baseY;
  octopus.vx = (random(100) < 50) ? -frand(3.8f, 6.5f) : frand(3.8f, 6.5f);
  octopus.phase = frand(0.0f, 6.28318f);
  octopus.colorPhase = frand(0.0f, 6.28318f);
  scheduleOctopusSpawn(now);
}

void spawnCthulhuAtCenter(unsigned long now) {
  octopus.active = true;
  octopus.cthulhu = true;
  octopus.x = SCREEN_W * 0.5f;
  octopus.baseY = SEA_LEVEL_Y * 0.52f;
  octopus.y = octopus.baseY;
  octopus.vx = (random(100) < 50) ? -frand(3.4f, 5.8f) : frand(3.4f, 5.8f);
  octopus.phase = frand(0.0f, 6.28318f);
  octopus.colorPhase = frand(0.0f, 6.28318f);
  scheduleOctopusSpawn(now);
}

void updateOctopus(unsigned long now, float dt) {
  if (!octopus.active) {
    if (octopus.nextSpawnMs == 0) {
      scheduleOctopusSpawn(now);
    } else if (timeReached(now, octopus.nextSpawnMs)) {
      spawnOctopus(now);
    }
    return;
  }

  float t = now * 0.001f;
  octopus.x += octopus.vx * dt;
  octopus.y = octopus.baseY + sinf(t * 0.45f + octopus.phase) * 6.0f;
  if ((octopus.vx > 0.0f && octopus.x > SCREEN_W + OCTOPUS_EXIT_PAD) ||
      (octopus.vx < 0.0f && octopus.x < -OCTOPUS_EXIT_PAD)) {
    octopus.active = false;
  }
}

void scheduleSeahorseSpawn(unsigned long now) {
  seahorse.nextSpawnMs = now + seahorseSpawnIntervalMs();
}

void spawnSeahorse(unsigned long now) {
  bool fromLeft = (random(100) < 50);
  seahorse.active = true;
  seahorse.facingRight = fromLeft;
  seahorse.vx = fromLeft ? frand(1.6f, 2.9f) * SEAHORSE_SPEED_BOOST
                          : -frand(1.6f, 2.9f) * SEAHORSE_SPEED_BOOST;
  seahorse.x = fromLeft ? -SEAHORSE_EXIT_PAD : (SCREEN_W + SEAHORSE_EXIT_PAD);
  seahorse.baseY = frand(34.0f, (float)SEA_LEVEL_Y - 56.0f);
  seahorse.y = seahorse.baseY;
  seahorse.phase = frand(0.0f, 6.28318f);
  seahorse.finPhase = frand(0.0f, 6.28318f);
  scheduleSeahorseSpawn(now);
}

void spawnSeahorseAtCenter(unsigned long now) {
  seahorse.active = true;
  seahorse.facingRight = (random(100) < 50);
  seahorse.x = SCREEN_W * 0.5f - 16.0f;
  seahorse.baseY = SEA_LEVEL_Y * 0.46f;
  seahorse.y = seahorse.baseY;
  seahorse.vx = seahorse.facingRight ? frand(1.4f, 2.4f) * SEAHORSE_SPEED_BOOST
                                      : -frand(1.4f, 2.4f) * SEAHORSE_SPEED_BOOST;
  seahorse.phase = frand(0.0f, 6.28318f);
  seahorse.finPhase = frand(0.0f, 6.28318f);
  scheduleSeahorseSpawn(now);
}

void updateSeahorse(unsigned long now, float dt) {
  if (!seahorse.active) {
    if (seahorse.nextSpawnMs == 0) {
      scheduleSeahorseSpawn(now);
    } else if (timeReached(now, seahorse.nextSpawnMs)) {
      spawnSeahorse(now);
    }
    return;
  }

  float t = now * 0.001f;
  float pulse = 1.0f + sinf(t * 0.55f + seahorse.phase) * 0.18f;
  seahorse.x += seahorse.vx * pulse * dt;
  seahorse.y = seahorse.baseY + sinf(t * 0.82f + seahorse.phase) * 4.5f +
               sinf(t * 2.15f + seahorse.phase * 1.7f) * 0.9f;

  if ((seahorse.vx > 0.0f && seahorse.x > SCREEN_W + SEAHORSE_EXIT_PAD) ||
      (seahorse.vx < 0.0f && seahorse.x < -SEAHORSE_EXIT_PAD)) {
    seahorse.active = false;
  }
}

void scheduleSnailSpawn(unsigned long now) {
  unsigned long interval = snailSpawnIntervalMs();
  snail.nextSpawnMs = (interval > 0) ? now + interval : 0;
}

void spawnSnail(unsigned long now) {
  bool fromLeft = (random(100) < 50);
  snail.active = true;
  snail.facingRight = fromLeft;
  snail.vx = fromLeft ? frand(2.2f, 3.7f) : -frand(2.2f, 3.7f);
  snail.x = fromLeft ? -SNAIL_EXIT_PAD : (SCREEN_W + SNAIL_EXIT_PAD);
  snail.y = SCREEN_H - 15.0f;
  snail.phase = frand(0.0f, 6.28318f);
  scheduleSnailSpawn(now);
}

void spawnSnailAtCenter(unsigned long now) {
  snail.active = true;
  snail.facingRight = (random(100) < 50);
  snail.x = SCREEN_W * 0.5f - SNAIL_CENTER_X_OFFSET;
  snail.y = SCREEN_H - 15.0f;
  snail.vx = snail.facingRight ? frand(2.0f, 3.4f) : -frand(2.0f, 3.4f);
  snail.phase = frand(0.0f, 6.28318f);
  scheduleSnailSpawn(now);
}

void updateSnail(unsigned long now, float dt) {
  if (!snail.active) {
    if (normalizeCreatureFrequency(snailFrequency) <= 0) {
      snail.nextSpawnMs = 0;
      return;
    }
    if (snail.nextSpawnMs == 0) {
      scheduleSnailSpawn(now);
    } else if (timeReached(now, snail.nextSpawnMs)) {
      spawnSnail(now);
    }
    return;
  }

  float t = now * 0.001f;
  float scoot = 1.0f + sinf(t * 2.2f + snail.phase) * 0.18f;
  snail.x += snail.vx * scoot * dt;
  snail.y = SCREEN_H - 15.0f;

  if ((snail.vx > 0.0f && snail.x > SCREEN_W + SNAIL_EXIT_PAD) ||
      (snail.vx < 0.0f && snail.x < -SNAIL_EXIT_PAD)) {
    snail.active = false;
  }
}

void scheduleJellyfishSpawn(unsigned long now) {
  unsigned long interval = jellyfishSpawnIntervalMs();
  jellyfish.nextSpawnMs = (interval > 0) ? now + interval : 0;
}

void spawnJellyfish(unsigned long now) {
  bool fromLeft = (random(100) < 50);
  jellyfish.active = true;
  jellyfish.x = fromLeft ? -34.0f : (SCREEN_W + 34.0f);
  jellyfish.baseY = SEA_LEVEL_Y * 0.48f;
  jellyfish.y = jellyfish.baseY;
  jellyfish.vx = fromLeft ? frand(2.2f, 4.0f) : -frand(2.2f, 4.0f);
  jellyfish.phase = frand(0.0f, 6.28318f);
  scheduleJellyfishSpawn(now);
}

void spawnJellyfishAtCenter(unsigned long now) {
  jellyfish.active = true;
  jellyfish.x = SCREEN_W * 0.5f - JELLYFISH_CENTER_X_OFFSET;
  jellyfish.baseY = SEA_LEVEL_Y * 0.48f;
  jellyfish.y = jellyfish.baseY;
  jellyfish.vx = (random(100) < 50) ? -frand(2.2f, 4.0f) : frand(2.2f, 4.0f);
  jellyfish.phase = frand(0.0f, 6.28318f);
  scheduleJellyfishSpawn(now);
}

void spawnDebugJellyfish(unsigned long now) {
  spawnJellyfishAtCenter(now);
}

void updateJellyfish(unsigned long now, float dt) {
  if (!jellyfish.active) {
    if (normalizeCreatureFrequency(jellyfishFrequency) <= 0) {
      jellyfish.nextSpawnMs = 0;
      return;
    }
    if (jellyfish.nextSpawnMs == 0) {
      scheduleJellyfishSpawn(now);
    } else if (timeReached(now, jellyfish.nextSpawnMs)) {
      spawnJellyfish(now);
    }
    return;
  }

  float t = now * 0.001f;
  float pulseWave = sinf(t * 1.55f + jellyfish.phase);
  float pulse = pulseWave > 0.0f ? pulseWave * pulseWave : 0.0f;
  jellyfish.x += (jellyfish.vx + sinf(t * 0.38f + jellyfish.phase) * 2.0f) * dt;
  jellyfish.baseY -= pulse * 3.2f * dt;
  jellyfish.baseY += sinf(t * 0.22f + jellyfish.phase) * 0.42f * dt;
  jellyfish.y = jellyfish.baseY + sinf(t * 0.72f + jellyfish.phase) * 5.0f - pulse * 2.5f;
  jellyfish.y = clampVal(jellyfish.y, 24.0f, (float)SEA_LEVEL_Y - 34.0f);

  if (jellyfish.x < -34.0f || jellyfish.x > SCREEN_W + 34.0f || jellyfish.baseY < 10.0f) {
    jellyfish.active = false;
  }
}

void spawnSquidAtCenter(unsigned long now) {
  bool fromLeft = (random(100) < 50);
  squid.active = true;
  squid.facingRight = fromLeft;
  squid.x = SCREEN_W * 0.5f - SQUID_CENTER_X_OFFSET;
  squid.baseY = SEA_LEVEL_Y * 0.42f;
  squid.y = squid.baseY;
  squid.vx = fromLeft ? 1.0f : -1.0f;
  squid.phase = frand(0.0f, 6.28318f);
  squid.burstPhase = frand(0.0f, 6.28318f);
}

void updateSquid(unsigned long now, float dt) {
  if (!squid.active) return;

  float t = now * 0.001f;
  float burstWave = sinf(t * 2.0f + squid.burstPhase);
  float burst = burstWave > 0.45f ? (burstWave - 0.45f) / 0.55f : 0.0f;
  burst *= burst * burst;
  float speed = 5.0f + burst * 44.0f;
  squid.x += squid.vx * speed * dt;
  squid.y = squid.baseY + sinf(t * 1.25f + squid.phase) * 3.5f + burst * sinf(t * 9.0f + squid.phase) * 1.5f;

  if ((squid.vx > 0.0f && squid.x > SCREEN_W + SQUID_EXIT_PAD) ||
      (squid.vx < 0.0f && squid.x < -SQUID_EXIT_PAD)) {
    squid.active = false;
  }
}

void keepVisitorsSeparated() {
  if (!octopus.active || !seahorse.active) return;

  float octoCenterX = octopus.x;
  float octoCenterY = octopus.y + OCTOPUS_CENTER_Y_OFFSET;
  float horseCenterX = seahorse.x + SEAHORSE_CENTER_X_OFFSET;
  float horseCenterY = seahorse.y + SEAHORSE_CENTER_Y_OFFSET;
  float dx = horseCenterX - octoCenterX;
  float dy = horseCenterY - octoCenterY;
  float sx = dx / VISITOR_CLEAR_RADIUS_X;
  float sy = dy / VISITOR_CLEAR_RADIUS_Y;
  float scaledD2 = sx * sx + sy * sy;
  if (scaledD2 >= 1.0f) return;

  if (scaledD2 <= 0.0001f) {
    dx = (seahorse.vx >= octopus.vx) ? 1.0f : -1.0f;
    dy = 0.35f;
    scaledD2 = (dx / VISITOR_CLEAR_RADIUS_X) * (dx / VISITOR_CLEAR_RADIUS_X) +
               (dy / VISITOR_CLEAR_RADIUS_Y) * (dy / VISITOR_CLEAR_RADIUS_Y);
  }

  float scale = 1.0f / sqrtf(scaledD2);
  float targetHorseCenterX = octoCenterX + dx * scale;
  float targetHorseCenterY = octoCenterY + dy * scale;
  float pushX = (targetHorseCenterX - horseCenterX) * 0.18f;
  float pushY = (targetHorseCenterY - horseCenterY) * 0.22f;

  seahorse.x += pushX;
  seahorse.baseY = clampVal(seahorse.baseY + pushY, 24.0f, (float)SEA_LEVEL_Y - 54.0f);
  seahorse.y += pushY;
  octopus.x -= pushX * 0.55f;
  octopus.baseY = clampVal(octopus.baseY - pushY * 0.55f, 28.0f, (float)SEA_LEVEL_Y - 44.0f);
  octopus.y -= pushY * 0.55f;
}

// ------------------------------ Drawing --------------------------------------
void seaweedPointAt(float u, float bx, int y0, float bladeHeight, float sway, float tSec, int bladeIndex, float& x, float& y) {
  u = clampVal(u, 0.0f, 1.0f);
  float bodyWave = wrappedSinf(tSec * (1.05f + bladeIndex * 0.025f) * seaweedSwaySpeed - u * 5.1f + bladeIndex * 0.72f);
  float ripple = wrappedSinf(tSec * 0.72f * seaweedSwaySpeed + u * 9.0f + bladeIndex * 1.31f);
  float bend = sway * u * (0.20f + u * 0.80f);
  float travel = bodyWave * (1.5f + bladeHeight * 0.055f) * u * u;
  float detail = ripple * 1.2f * u;
  x = bx + bend + travel + detail;
  y = y0 - bladeHeight * u;
}

void drawSeaweedBranches(TFT_eSprite& s, int bladeIndex, float bladeHeight, float sway, float tSec,
                         float bx, int y0) {
  int branchCount = clampVal((int)(bladeHeight / 14.0f), 2, 5);
  for (int b = 0; b < branchCount; ++b) {
    float u = 0.30f + b * 0.14f + ((bladeIndex + b) % 3) * 0.018f;
    if (u > 0.88f) u = 0.88f;

    float px, py;
    seaweedPointAt(u, bx, y0, bladeHeight, sway, tSec, bladeIndex, px, py);

    float side = ((bladeIndex + b) & 1) ? 1.0f : -1.0f;
    float branchLen = 5.5f + ((bladeIndex * 3 + b * 5) % 5);
    float branchWiggle = wrappedSinf(tSec * (1.1f + bladeIndex * 0.03f) * seaweedSwaySpeed + bladeIndex + b * 1.7f) * 1.2f;
    int ex = (int)(px + side * (branchLen * 0.58f + fabsf(sway) * 0.05f) + branchWiggle);
    int ey = (int)(py - branchLen * 0.78f);
    uint16_t color = (b & 1) ? TFT_DARKGREEN : TFT_GREEN;
    s.drawLine((int)px, (int)py, ex, ey, color);
  }
}

void drawSeaweed(TFT_eSprite& s, float tSec) {
  static const int roots = 12;
  static bool cached = false;
  static float baseX[roots];
  static float amp[roots];
  static float heightNoise[roots];

  if (!cached) {
    for (int i = 0; i < roots; ++i) {
      baseX[i] = 10 + i * (SCREEN_W - 20.0f) / (roots - 1);
      amp[i] = 5.0f + (i % 4) * 2.0f;
      heightNoise[i] = sinf(i * 2.173f + 0.61f);
    }
    cached = true;
  }

  for (int i = 0; i < roots; i++) {
    float bx = baseX[i];
    float sway = wrappedSinf(tSec * (0.8f + 0.09f * i) * seaweedSwaySpeed + i * 0.7f) * amp[i];
    float heightVariation = 1.0f + seaweedLengthRandomness * heightNoise[i];
    float bladeHeight = clampVal(32.0f * seaweedLength * heightVariation, 18.0f, 72.0f);
    int y0 = SCREEN_H - 2;

    float prevX, prevY;
    prevX = bx;
    prevY = y0;
    const int segments = 7;
    for (int seg = 1; seg <= segments; ++seg) {
      float u = (float)seg / segments;
      float x, y;
      seaweedPointAt(u, bx, y0, bladeHeight, sway, tSec, i, x, y);
      uint16_t color = (u < 0.38f) ? TFT_DARKGREEN : ((u < 0.76f) ? TFT_GREEN : TFT_GREENYELLOW);
      s.drawLine((int)prevX, (int)prevY, (int)x, (int)y, color);
      if (u < 0.78f) {
        s.drawLine((int)prevX + 1, (int)prevY, (int)x + 1, (int)y, TFT_DARKGREEN);
      }
      prevX = x;
      prevY = y;
    }
    drawSeaweedBranches(s, i, bladeHeight, sway, tSec, bx, y0);
  }
}

void drawFlakes(TFT_eSprite& s) {
  s.setTextSize(1);
  s.setTextDatum(MC_DATUM);
  for (int i = 0; i < MAX_FLAKES; i++) {
    if (!flakes[i].active) continue;
    s.setTextColor(flakes[i].color);
    s.drawString("*", (int)flakes[i].x, (int)flakes[i].y);
  }
}

void drawBubbles(TFT_eSprite& s) {
  s.setTextSize(1);
  s.setTextDatum(MC_DATUM);
  int bubbleCount = activeBubbleLimit();
  for (int i = 0; i < bubbleCount; i++) {
    if (!bubbles[i].active) continue;
    s.setTextColor(bubbles[i].color);
    s.drawString("o", (int)bubbles[i].x, (int)bubbles[i].y);
  }
}

void drawFish(TFT_eSprite& s) {
  s.setTextSize(1);
  s.setTextDatum(TL_DATUM);
  const float t = aquariumTimeSec();
  const float waveBase = t * FISH_SWIM_WAVE_SPEED;
  static const float waveStepSin = sinf(FISH_SWIM_WAVE_SPACING);
  static const float waveStepCos = cosf(FISH_SWIM_WAVE_SPACING);
  int fishCount = activeFishLimit();
  for (int i = 0; i < fishCount; i++) {
    Fish& f = fishPool[i];
    if (!f.active) continue;
    const char* txt = fishGlyphDrawing(f);
    const int16_t* glyphOffsets = fishGlyphOffsets(f);
    uint8_t len = fishGlyphLength(f);
    float waveAngle = waveBase + f.phase;
    float wave = sinf(waveAngle);
    float waveCos = cosf(waveAngle);
    s.setTextColor(f.renderColor);
    for (uint8_t c = 0; c < len; ++c) {
      if (txt[c] != ' ') {
        float yOffset = wave * FISH_SWIM_WAVE_AMPLITUDE;
        int charX = (int)f.x + glyphOffsets[c];
        int charY = (int)f.y + (int)(yOffset + ((yOffset >= 0.0f) ? 0.5f : -0.5f));
        s.drawChar((uint16_t)txt[c], charX, charY);
      }

      float nextWave = wave * waveStepCos + waveCos * waveStepSin;
      waveCos = waveCos * waveStepCos - wave * waveStepSin;
      wave = nextWave;
    }
  }
}

uint16_t octopusColor(float tSec) {
  int r = 205 + (int)(42.0f * sinf(tSec * 0.18f + octopus.colorPhase));
  int g = 78 + (int)(38.0f * sinf(tSec * 0.13f + octopus.colorPhase + 2.1f));
  int b = 178 + (int)(58.0f * sinf(tSec * 0.16f + octopus.colorPhase + 4.2f));
  return rgb565From888(r, g, b);
}

uint16_t cthulhuColor(float tSec) {
  int r = 8 + (int)(5.0f * sinf(tSec * 0.10f + octopus.colorPhase));
  int g = 82 + (int)(24.0f * sinf(tSec * 0.13f + octopus.colorPhase + 1.7f));
  int b = 34 + (int)(10.0f * sinf(tSec * 0.09f + octopus.colorPhase + 3.1f));
  return rgb565From888(r, g, b);
}

void drawOctopusGlyph(TFT_eSprite& s, const char* glyph, int x, int y) {
  if (!glyph || glyph[0] == '\0') return;
  s.drawChar((uint16_t)glyph[0], x, y);
}

void drawCthulhu(TFT_eSprite& s, float t, int cx, int cy) {
  s.setTextColor(cthulhuColor(t));
  s.setTextDatum(MC_DATUM);
  int tentacleFrame = ((int)(t * 3.0f + octopus.phase) & 1);
  int headBob = (int)(sinf(t * 1.1f + octopus.phase) * 1.4f);
  int tentacleBob = (int)(sinf(t * 1.8f + octopus.phase) * 1.6f);
  const char* tentacles = tentacleFrame ? "((()))" : "(()())";
  s.drawString("(;,,;)", cx, cy + headBob);
  s.drawString(tentacles, cx, cy + 15 + tentacleBob);
  s.setTextDatum(TL_DATUM);
}

void drawOctopus(TFT_eSprite& s) {
  if (!octopus.active) return;
  float t = aquariumTimeSec();
  int cx = (int)octopus.x;
  int cy = (int)octopus.y;

  s.setTextSize(1);
  s.setTextDatum(TL_DATUM);

  if (octopus.cthulhu) {
    drawCthulhu(s, t, cx, cy);
    return;
  }

  s.setTextColor(octopusColor(t));

  float topWave = sinf(t * 1.25f + octopus.phase) * 1.4f;
  drawOctopusGlyph(s, "(", cx - 13, cy + (int)topWave);
  drawOctopusGlyph(s, ".", cx - 3, cy - 5);
  drawOctopusGlyph(s, ".", cx + 7, cy - 5);
  drawOctopusGlyph(s, ")", cx + 16, cy - (int)topWave);

  static const char* tentacleGlyphs[] = {"(", "(", "(", ")", ")", ")"};
  static const int tentacleX[] = {-24, -16, -8, 2, 10, 18};
  for (int i = 0; i < 6; ++i) {
    float wave = sinf(t * 1.75f + octopus.phase + i * 0.72f);
    int x = cx + tentacleX[i] + (int)(wave * 1.4f);
    int y = cy + 13 + (int)(wave * 2.2f);
    drawOctopusGlyph(s, tentacleGlyphs[i], x, y);
  }
}

uint16_t seahorseColor(float tSec) {
  int r = 238 + (int)(12.0f * sinf(tSec * 0.11f + seahorse.phase));
  int g = 142 + (int)(18.0f * sinf(tSec * 0.16f + seahorse.phase + 1.4f));
  int b = 48 + (int)(12.0f * sinf(tSec * 0.13f + seahorse.phase + 2.8f));
  return rgb565From888(r, g, b);
}

char mirrorSeahorseGlyph(char glyph) {
  switch (glyph) {
    case '/': return '\\';
    case '\\': return '/';
    case '[': return ']';
    case ']': return '[';
    case '(': return ')';
    case ')': return '(';
    case '<': return '>';
    case '>': return '<';
    default: return glyph;
  }
}

void drawSeahorseGlyph(TFT_eSprite& s, char glyph, int x, int y) {
  s.drawChar((uint16_t)glyph, x, y);
}

void drawSeahorse(TFT_eSprite& s) {
  if (!seahorse.active) return;
  static const char* seahorseLeftRows[] = {
      "  ^^  ",
      " / o) ",
      "[__-/ ",
      "  /|  ",
      " / |  ",
      " \\ |  ",
      "  ( ) ",
      "  \\_/ ",
  };
  static const int SEAHORSE_ART_ROWS = sizeof(seahorseLeftRows) / sizeof(seahorseLeftRows[0]);
  static const int SEAHORSE_ART_COLS = 6;
  static const int SEAHORSE_CELL_W = 5;
  static const int SEAHORSE_ROW_H = 6;

  float t = aquariumTimeSec();
  int x = (int)seahorse.x;
  int y = (int)seahorse.y;
  int sway = (int)(sinf(t * 1.15f + seahorse.phase) * 1.2f);
  int finFlutter = (int)(sinf(t * 10.0f + seahorse.finPhase) * 1.2f);
  const char* finGlyph = (sinf(t * 12.0f + seahorse.finPhase) > 0.0f) ? "~" : "-";

  s.setTextSize(1);
  s.setTextFont(1);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(seahorseColor(t));

  for (int row = 0; row < SEAHORSE_ART_ROWS; ++row) {
    const char* line = seahorseLeftRows[row];
    int len = strlen(line);
    int rowSway = (row >= 1 && row <= 3) ? sway : 0;
    for (int col = 0; col < SEAHORSE_ART_COLS; ++col) {
      char glyph = (col < len) ? line[col] : ' ';
      if (glyph == ' ') continue;
      int drawCol = col;
      if (seahorse.facingRight) {
        drawCol = SEAHORSE_ART_COLS - 1 - col;
        glyph = mirrorSeahorseGlyph(glyph);
      }
      drawSeahorseGlyph(s, glyph, x + drawCol * SEAHORSE_CELL_W + rowSway, y + row * SEAHORSE_ROW_H);
    }
  }

  s.setTextColor(rgb565From888(255, 188, 82));
  int finX = seahorse.facingRight ? x + 5 + finFlutter : x + 20 + finFlutter;
  s.drawString(finGlyph, finX, y + 24);
  s.setTextFont(2);
}

uint16_t snailColor(float tSec) {
  int r = 228 + (int)(14.0f * sinf(tSec * 0.16f + snail.phase));
  int g = 116 + (int)(16.0f * sinf(tSec * 0.13f + snail.phase + 1.3f));
  int b = 34 + (int)(8.0f * sinf(tSec * 0.11f + snail.phase + 2.6f));
  return rgb565From888(r, g, b);
}

void drawSnail(TFT_eSprite& s) {
  if (!snail.active) return;

  float t = aquariumTimeSec();
  bool stalkFrame = sinf(t * 1.6f + snail.phase) > 0.0f;
  int x = (int)snail.x;
  int y = (int)snail.y;

#if 0
  // Previous multi-line snail kept for quick revival if we want to revisit it.
  static const char* snailRightRows[2][3] = {
      {" __ |/", "(@l)o", "`~~~~'"},
      {" __ /|", "(@l)o", "`~--~'"},
  };
  static const char* snailLeftRows[2][3] = {
      {"\\| __", "o(l@)", "'~~~~`"},
      {"|\\ __", "o(l@)", "'~--~`"},
  };
#endif

  s.setTextSize(1);
  s.setTextFont(2);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(snailColor(t));

  if (snail.facingRight) {
    s.drawString("@", x, y);
    s.drawString(stalkFrame ? "/" : "|", x + 11, y);
  } else {
    s.drawString(stalkFrame ? "\\" : "|", x, y);
    s.drawString("@", x + 7, y);
  }
}

uint16_t jellyfishColor(float tSec) {
  int r = 92 + (int)(20.0f * sinf(tSec * 0.12f + jellyfish.phase));
  int g = 188 + (int)(24.0f * sinf(tSec * 0.16f + jellyfish.phase + 1.2f));
  int b = 236 + (int)(16.0f * sinf(tSec * 0.10f + jellyfish.phase + 2.5f));
  return rgb565From888(r, g, b);
}

void drawJellyfish(TFT_eSprite& s) {
  if (!jellyfish.active) return;

  static const char* jellyRows[2][4] = {
      {" .-. ", "(   )", " \\|/ ", " /~\\ "},
      {" _-_ ", "(   )", "  |  ", " ~ ~ "},
  };

  float t = aquariumTimeSec();
  int frame = ((int)(t * 1.05f + jellyfish.phase) & 1);
  int x = (int)jellyfish.x;
  int y = (int)jellyfish.y;
  int tentacleSway = (int)(sinf(t * 1.45f + jellyfish.phase) * 1.4f);

  s.setTextSize(1);
  s.setTextFont(1);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(jellyfishColor(t));

  const char* const* rows = jellyRows[frame];
  s.drawString(rows[0], x, y);
  s.drawString(rows[1], x, y + 7);
  s.drawString(rows[2], x + tentacleSway, y + 15);
  s.drawString(rows[3], x - tentacleSway, y + 23);
  s.setTextFont(2);
}

uint16_t squidColor(float tSec) {
  int r = 116 + (int)(22.0f * sinf(tSec * 0.20f + squid.phase));
  int g = 186 + (int)(24.0f * sinf(tSec * 0.16f + squid.phase + 2.1f));
  int b = 228 + (int)(20.0f * sinf(tSec * 0.18f + squid.phase + 4.2f));
  return rgb565From888(r, g, b);
}

void drawSquid(TFT_eSprite& s) {
  if (!squid.active) return;

  float t = aquariumTimeSec();
  int x = (int)squid.x;
  int y = (int)squid.y;
  float burstWave = sinf(t * 2.0f + squid.burstPhase);
  bool burstFrame = burstWave > 0.35f;
  bool tentacleFrame = sinf(t * 9.0f + squid.phase) > 0.0f;
  const char* glyph = nullptr;
  if (squid.facingRight) {
    glyph = tentacleFrame ? "(((:>" : "{{{:>";
  } else {
    glyph = tentacleFrame ? "<:)))" : "<:}}}";
  }

  s.setTextSize(1);
  s.setTextFont(2);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(squidColor(t));
  s.drawString(glyph, x, y);

  if (burstFrame) {
    s.setTextColor(rgb565From888(74, 134, 190));
    if (squid.facingRight) {
      s.drawString("..", x - 12, y + 2);
    } else {
      s.drawString("..", x + 40, y + 2);
    }
  }
}

void drawAsciiClockBackground(TFT_eSprite& s) {
  if (!clockVisible || clockDisplayStyle != CLOCK_STYLE_ASCII) return;

  char timeText[16];
  formatClockTimeOnly(timeText, sizeof(timeText), false);
  const AsciiClockFont& font = currentAsciiClockFont();
  int artCols = asciiClockTextCols(timeText, font);
  // The ASCII art clock is already large; scale it by the nearest integer of the
  // zoom factor (1x or 2x) rather than fractionally.
  int asciiSize = clampVal((int)lroundf(clockZoomScale()), 1, 2);
  int charW = ASCII_CLOCK_CHAR_W * asciiSize;   // pinch-to-zoom scales only the clock
  int rowH = ASCII_CLOCK_ROW_H * asciiSize;
  int artPixelW = artCols * charW;
  int x = (SCREEN_W - artPixelW) / 2;
  if (x < 0) x = 0;
  int y = ASCII_CLOCK_Y;

  s.setTextFont(1);
  s.setTextSize(asciiSize);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(currentAsciiClockTextColor());

  for (int row = 0; row < font.rowCount; ++row) {
    char rowBuf[96] = "";
    for (size_t i = 0; timeText[i] != '\0'; ++i) {
      if (i > 0) {
        for (int gap = 0; gap < font.glyphGap; ++gap) appendCharSafe(rowBuf, sizeof(rowBuf), ' ');
      }
      appendAsciiClockGlyphRow(rowBuf, sizeof(rowBuf), font, asciiClockGlyphFor(font, timeText[i]), row);
    }
    if (clockFlipHorizontal) {
      mirrorClockTextInPlace(rowBuf);
    }
    trimTrailingSpaces(rowBuf);
    if (rowBuf[0] != '\0') s.drawString(rowBuf, x, y + row * rowH);
  }

  s.setTextFont(2);
  s.setTextSize(1);
}

// Base (unscaled) height of the small clock text, used to place the bottom clock.
static const int SMALL_CLOCK_BASE_H = 16;   // font 2 at size 1

// Draws the small clock centred at `yTop`, scaled by float `scale` (1.0 = original),
// mirrored when clockFlipHorizontal is set. Renders once into the reusable flip
// sprite at 1x, then nearest-neighbour blits it to the canvas — this gives smooth
// fractional zoom notches that setTextSize (integer only) can't. Returns false if
// the scratch sprite isn't available (caller falls back to plain drawString).
bool drawSmallClockScaled(TFT_eSprite& s, const char* line, int yTop, float scale) {
  if (!clockFlipSpriteReady) return false;

  clockFlipSprite.setTextFont(2);
  clockFlipSprite.setTextSize(1);
  clockFlipSprite.setTextDatum(TL_DATUM);
  clockFlipSprite.fillSprite(CLOCK_FLIP_TRANSPARENT);
  uint16_t transparentKey = clockFlipSprite.readPixel(0, CLOCK_FLIP_SPRITE_H - 1);
  clockFlipSprite.setTextColor(currentSmallClockTextColor(), CLOCK_FLIP_TRANSPARENT);
  clockFlipSprite.drawString(line, 0, 0);

  int textW = clockFlipSprite.textWidth(line);
  if (textW <= 0) return true;
  int srcW = clampVal(textW, 1, CLOCK_FLIP_SPRITE_W);
  int srcH = clampVal(SMALL_CLOCK_BASE_H, 1, CLOCK_FLIP_SPRITE_H);

  int outW = (int)(srcW * scale + 0.5f);
  int outH = (int)(srcH * scale + 0.5f);
  if (outW < 1) outW = 1;
  if (outH < 1) outH = 1;
  int destX = (SCREEN_W - outW) / 2;

  for (int dy = 0; dy < outH; ++dy) {
    int py = yTop + dy;
    if (py < 0 || py >= SCREEN_H) continue;
    int sy = (int)(dy / scale);
    if (sy >= srcH) sy = srcH - 1;
    for (int dx = 0; dx < outW; ++dx) {
      int px = destX + dx;
      if (px < 0 || px >= SCREEN_W) continue;
      int sx = (int)(dx / scale);
      if (sx >= srcW) sx = srcW - 1;
      int readX = clockFlipHorizontal ? (srcW - 1 - sx) : sx;
      uint16_t color = clockFlipSprite.readPixel(readX, sy);
      if (color != transparentKey) s.drawPixel(px, py, color);
    }
  }
  return true;
}

void drawClock(TFT_eSprite& s) {
  if (!clockVisible || clockDisplayStyle != CLOCK_STYLE_SMALL_TEXT) return;
  char line[32];
  formatClockDisplay(line, sizeof(line));
  float scale = clockZoomScale();
  int scaledH = (int)(SMALL_CLOCK_BASE_H * scale + 0.5f);
  int y = (clockSmallPosition == CLOCK_SMALL_TOP) ? 4 : (SCREEN_H - scaledH - 2);

  if (drawSmallClockScaled(s, line, y, scale)) return;   // fractional (preferred) path

  // Fallback: scratch sprite unavailable — nearest integer size via setTextSize.
  s.setTextSize(clampVal((int)lroundf(scale), 1, 2));
  s.setTextDatum(TC_DATUM);
  s.setTextColor(currentSmallClockTextColor());
  if (clockFlipHorizontal) {
    char fallback[32];
    copySafe(fallback, sizeof(fallback), line);
    mirrorClockTextInPlace(fallback);
    s.drawString(fallback, SCREEN_W / 2, y);
  } else {
    s.drawString(line, SCREEN_W / 2, y);
  }
  s.setTextSize(1);
}

bool hudButtonFlashActive(unsigned long untilMs) {
  return millis() < untilMs;
}

void drawHud(TFT_eSprite& s) {
  if (!hudVisible) return;
  constexpr int kHudTextYStart = 32;   // Below corners so long labels avoid H overlap
  constexpr int kHudTextLineDY = 16;   // Font 2-ish second line spacing
  constexpr int kHudTextX = 8;
  constexpr bool kShowHudHardwareStats = false;  // Re-enable when diagnosing sprite memory or PSRAM.

  s.setTextSize(1);
  s.setTextColor(TFT_WHITE, BG_COLOR);
  s.setTextDatum(TL_DATUM);

  // Draw hidden HUD chrome first.
  drawButton(s, DEBUG_BUTTON_X, CORNER_BUTTON_Y, CORNER_BUTTON_W, CORNER_BUTTON_H, "D", TFT_WHITE, TFT_DARKGREEN);
  drawButton(s, BACKLIGHT_BUTTON_X, CORNER_BUTTON_Y, CORNER_BUTTON_W, CORNER_BUTTON_H, "B",
             backlightPanelOpen ? TFT_NAVY : TFT_WHITE, backlightPanelOpen ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, CAPTURE_BUTTON_X, CORNER_BUTTON_Y, CORNER_BUTTON_W, CORNER_BUTTON_H, "C",
             capturePanelOpen ? TFT_NAVY : TFT_WHITE, capturePanelOpen ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, WIFI_BUTTON_X, CORNER_BUTTON_Y, CORNER_BUTTON_W, CORNER_BUTTON_H, "W",
             wifiPanelOpen ? TFT_NAVY : TFT_WHITE, wifiPanelOpen ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, SETTINGS_CORNER_BUTTON_X, CORNER_BUTTON_Y, CORNER_BUTTON_W, CORNER_BUTTON_H, "S",
             settingsOpen ? TFT_NAVY : TFT_WHITE, settingsOpen ? TFT_CYAN : TFT_DARKGREEN);
  bool respawnFlash = hudButtonFlashActive(respawnButtonFlashUntilMs);
  bool seahorseFlash = hudButtonFlashActive(seahorseButtonFlashUntilMs);
  bool octopusFlash = hudButtonFlashActive(octopusButtonFlashUntilMs);
  bool snailFlash = hudButtonFlashActive(snailButtonFlashUntilMs);
  bool cthulhuFlash = hudButtonFlashActive(cthulhuButtonFlashUntilMs);
  bool jellyfishFlash = hudButtonFlashActive(jellyfishButtonFlashUntilMs);
  drawButton(s, SETTINGS_CORNER_BUTTON_X, RESPAWN_BUTTON_Y, CORNER_BUTTON_W, CORNER_BUTTON_H, "R",
             respawnFlash ? TFT_NAVY : TFT_WHITE, respawnFlash ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, SEAHORSE_TEST_BUTTON_X, SEAHORSE_TEST_BUTTON_Y, CORNER_BUTTON_W, CORNER_BUTTON_H, "H",
             seahorseFlash ? TFT_NAVY : TFT_WHITE, seahorseFlash ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, OCTOPUS_TEST_BUTTON_X, OCTOPUS_TEST_BUTTON_Y, CORNER_BUTTON_W, CORNER_BUTTON_H, "O",
             octopusFlash ? TFT_NAVY : TFT_WHITE, octopusFlash ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, SNAIL_TEST_BUTTON_X, SNAIL_TEST_BUTTON_Y, CORNER_BUTTON_W, CORNER_BUTTON_H, "N",
             snailFlash ? TFT_NAVY : TFT_WHITE, snailFlash ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, CTHULHU_TEST_BUTTON_X, CTHULHU_TEST_BUTTON_Y, CORNER_BUTTON_W, CORNER_BUTTON_H, "C",
             cthulhuFlash ? TFT_NAVY : TFT_WHITE, cthulhuFlash ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, JELLYFISH_TEST_BUTTON_X, JELLYFISH_TEST_BUTTON_Y, CORNER_BUTTON_W, CORNER_BUTTON_H, "J",
             jellyfishFlash ? TFT_NAVY : TFT_WHITE, jellyfishFlash ? TFT_CYAN : TFT_DARKGREEN);
  if (SQUID_ENABLED) {
    bool squidFlash = hudButtonFlashActive(squidButtonFlashUntilMs);
    drawButton(s, SQUID_TEST_BUTTON_X, SQUID_TEST_BUTTON_Y, CORNER_BUTTON_W, CORNER_BUTTON_H, "Q",
               squidFlash ? TFT_NAVY : TFT_WHITE, squidFlash ? TFT_CYAN : TFT_DARKGREEN);
  }
  s.setTextDatum(TL_DATUM);
  s.setTextColor(TFT_WHITE, BG_COLOR);

  char title[48];
  snprintf(title, sizeof(title), "ASCII Aquarium %s", kSketchVersionLabel);
  s.drawString(title, kHudTextX, kHudTextYStart);

  char line[64];
  int visibleFish = activeFishLimit();
  if (visibleFish == fishTargetCount) {
    snprintf(line, sizeof(line), "Fish:%d  FPS:%2.1f", fishTargetCount, fps);
  } else {
    snprintf(line, sizeof(line), "Fish:%d/%d  FPS:%2.1f", visibleFish, fishTargetCount, fps);
  }
  s.drawString(line, kHudTextX, kHudTextYStart + kHudTextLineDY);

  if (kShowHudHardwareStats) {
    snprintf(line, sizeof(line), "Canvas:%db %dx%d", mainCanvasActualColorDepth, SCREEN_W, mainCanvasRenderHeight);
    s.drawString(line, kHudTextX, kHudTextYStart + kHudTextLineDY * 2);

    snprintf(line, sizeof(line), "Heap:%lu/%luK", (unsigned long)(ESP.getFreeHeap() / 1024UL),
             (unsigned long)(ESP.getHeapSize() / 1024UL));
    s.drawString(line, kHudTextX, kHudTextYStart + kHudTextLineDY * 3);

    uint32_t psramSize = ESP.getPsramSize();
    if (psramSize > 0) {
      snprintf(line, sizeof(line), "PSRAM:%lu/%luK", (unsigned long)(ESP.getFreePsram() / 1024UL),
               (unsigned long)(psramSize / 1024UL));
    } else {
      snprintf(line, sizeof(line), "PSRAM: none");
    }
    s.drawString(line, kHudTextX, kHudTextYStart + kHudTextLineDY * 4);
  }
}

void drawButton(TFT_eSprite& s, int x, int y, int w, int h, const char* label, uint16_t fg, uint16_t bg) {
  s.fillRoundRect(x, y, w, h, 4, bg);
  s.drawRoundRect(x, y, w, h, 4, fg);
  s.setTextColor(fg, bg);
  s.setTextDatum(MC_DATUM);
  s.drawString(label, x + w / 2, y + h / 2);
}

void drawSettingRow(TFT_eSprite& s, int rowY, const char* label, const char* value) {
  const int controlY = rowY;
  const int centerY = controlY + SETTINGS_BUTTON_H / 2;
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(label, SETTINGS_PANEL_X + 12, centerY);

  s.setTextDatum(MR_DATUM);
  s.setTextColor(TFT_GREENYELLOW, TFT_NAVY);
  s.drawString(value, SETTINGS_VALUE_RIGHT_X, centerY);

  drawButton(s, SETTINGS_MINUS_X, controlY, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H, "-", TFT_WHITE, TFT_DARKGREEN);
  drawButton(s, SETTINGS_PLUS_X, controlY, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H, "+", TFT_WHITE, TFT_DARKGREEN);
}

void formatHourlyFrequency(char* buf, size_t len, int frequency) {
  if (frequency <= 0) {
    copySafe(buf, len, "Off");
    return;
  }
  snprintf(buf, len, "%d/hr", frequency);
}

void drawEventFrequencyRow(TFT_eSprite& s, int rowY, const char* label, int frequency) {
  char value[12];
  formatHourlyFrequency(value, sizeof(value), frequency);
  const int centerY = rowY + EVENTS_BUTTON_H / 2;

  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(label, EVENTS_LABEL_X, centerY);

  s.setTextDatum(MR_DATUM);
  s.setTextColor(TFT_GREENYELLOW, TFT_NAVY);
  s.drawString(value, EVENTS_VALUE_RIGHT_X, centerY);

  drawButton(s, EVENTS_MINUS_X, rowY, EVENTS_BUTTON_W, EVENTS_BUTTON_H, "-", TFT_WHITE, TFT_DARKGREEN);
  drawButton(s, EVENTS_PLUS_X, rowY, EVENTS_BUTTON_W, EVENTS_BUTTON_H, "+", TFT_WHITE, TFT_DARKGREEN);
}

void drawEventActionRow(TFT_eSprite& s, int rowY, const char* label, const char* actionLabel) {
  const int centerY = rowY + EVENTS_BUTTON_H / 2;
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(label, EVENTS_LABEL_X, centerY);
  drawButton(s, EVENTS_ACTION_X, rowY, EVENTS_ACTION_W, EVENTS_BUTTON_H, actionLabel, TFT_CYAN, TFT_DARKGREEN);
}

void drawCreatureFrequencyRow(TFT_eSprite& s, int rowY, const char* label, int frequency) {
  char value[12];
  formatHourlyFrequency(value, sizeof(value), frequency);
  const int centerY = rowY + CREATURES_BUTTON_H / 2;

  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(label, CREATURES_LABEL_X, centerY);

  s.setTextDatum(MR_DATUM);
  s.setTextColor(TFT_GREENYELLOW, TFT_NAVY);
  s.drawString(value, CREATURES_VALUE_RIGHT_X, centerY);

  drawButton(s, CREATURES_MINUS_X, rowY, CREATURES_BUTTON_W, CREATURES_BUTTON_H, "-", TFT_WHITE, TFT_DARKGREEN);
  drawButton(s, CREATURES_PLUS_X, rowY, CREATURES_BUTTON_W, CREATURES_BUTTON_H, "+", TFT_WHITE, TFT_DARKGREEN);
}

void drawSettingToggleRow(TFT_eSprite& s, int rowY, const char* label, const char* leftLabel, const char* rightLabel,
                          bool leftActive, bool rightActive) {
  const int controlY = rowY;
  const int centerY = controlY + SETTINGS_BUTTON_H / 2;
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(label, SETTINGS_PANEL_X + 12, centerY);

  drawButton(s, SETTINGS_MINUS_X, controlY, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H, leftLabel,
             leftActive ? TFT_NAVY : TFT_WHITE, leftActive ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, SETTINGS_PLUS_X, controlY, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H, rightLabel,
             rightActive ? TFT_NAVY : TFT_WHITE, rightActive ? TFT_CYAN : TFT_DARKGREEN);
}

void drawSettingStatusRow(TFT_eSprite& s, int rowY, const char* label, const char* value) {
  const int centerY = rowY + SETTINGS_BUTTON_H / 2;
  char shortValue[18];
  formatShortText(shortValue, sizeof(shortValue), value, 14);
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(label, SETTINGS_PANEL_X + 12, centerY);
  s.setTextDatum(MR_DATUM);
  s.setTextColor(TFT_GREENYELLOW, TFT_NAVY);
  s.drawString(shortValue, SETTINGS_PANEL_X + SETTINGS_PANEL_W - 16, centerY);
}

void drawWifiPanelBase(TFT_eSprite& s, const char* title) {
  s.fillRoundRect(WIFI_PANEL_X, WIFI_PANEL_Y, WIFI_PANEL_W, WIFI_PANEL_H, 8, TFT_NAVY);
  s.drawRoundRect(WIFI_PANEL_X, WIFI_PANEL_Y, WIFI_PANEL_W, WIFI_PANEL_H, 8, TFT_CYAN);
  s.setTextSize(1);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(title, WIFI_PANEL_X + 10, WIFI_PANEL_Y + 10);
  drawButton(s, WIFI_CLOSE_X, WIFI_CLOSE_Y, WIFI_CLOSE_W, WIFI_CLOSE_H, "X", TFT_WHITE, TFT_RED);
}

void drawWifiToggleRow(TFT_eSprite& s, int rowY, const char* label, bool offActive, bool onActive) {
  int centerY = rowY + WIFI_ROW_H / 2;
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(label, WIFI_LABEL_X, centerY);
  drawButton(s, WIFI_OFF_X, rowY, WIFI_TOGGLE_W, WIFI_ROW_H, "OFF",
             offActive ? TFT_NAVY : TFT_WHITE, offActive ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, WIFI_ON_X, rowY, WIFI_TOGGLE_W, WIFI_ROW_H, "ON",
             onActive ? TFT_NAVY : TFT_WHITE, onActive ? TFT_CYAN : TFT_DARKGREEN);
}

void drawWifiStatusRow(TFT_eSprite& s, int rowY, const char* label, const char* value) {
  char shortValue[24];
  int centerY = rowY + WIFI_ROW_H / 2;
  formatShortText(shortValue, sizeof(shortValue), value, 19);
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(label, WIFI_LABEL_X, centerY);
  s.setTextDatum(MR_DATUM);
  s.setTextColor(TFT_GREENYELLOW, TFT_NAVY);
  s.drawString(shortValue, WIFI_PANEL_X + WIFI_PANEL_W - 16, centerY);
}

void drawWifiActionRow(TFT_eSprite& s, int rowY, const char* label, const char* value, const char* actionLabel) {
  char shortValue[18];
  int centerY = rowY + WIFI_ROW_H / 2;
  formatShortText(shortValue, sizeof(shortValue), value, 13);
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(label, WIFI_LABEL_X, centerY);
  s.setTextDatum(MR_DATUM);
  s.setTextColor(TFT_GREENYELLOW, TFT_NAVY);
  s.drawString(shortValue, WIFI_VALUE_RIGHT_X, centerY);
  drawButton(s, WIFI_ACTION_X, rowY, WIFI_ACTION_W, WIFI_ROW_H, actionLabel, TFT_CYAN, TFT_DARKGREEN);
}

void drawWifiStepRow(TFT_eSprite& s, int rowY, const char* label, const char* value) {
  char shortValue[20];
  int centerY = rowY + WIFI_ROW_H / 2;
  formatShortText(shortValue, sizeof(shortValue), value, 15);
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(label, WIFI_LABEL_X, centerY);
  s.setTextDatum(MR_DATUM);
  s.setTextColor(TFT_GREENYELLOW, TFT_NAVY);
  s.drawString(shortValue, WIFI_VALUE_RIGHT_X, centerY);
  drawButton(s, WIFI_OFF_X, rowY, WIFI_TOGGLE_W, WIFI_ROW_H, "-", TFT_WHITE, TFT_DARKGREEN);
  drawButton(s, WIFI_ON_X, rowY, WIFI_TOGGLE_W, WIFI_ROW_H, "+", TFT_WHITE, TFT_DARKGREEN);
}

const char* keyboardRowText(int row) {
  static const char* lowerRows[] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm"};
  static const char* upperRows[] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
  static const char* symbolRows[] = {"!@#$%^&*()", "-_=+[]{}", ";:'\",.<>", "/\\?|~`"};
  if (row < 0 || row >= 4) return "";
  if (keyboardMode == KEYBOARD_UPPER) return upperRows[row];
  if (keyboardMode == KEYBOARD_SYMBOLS) return symbolRows[row];
  return lowerRows[row];
}

void keyboardKeyBounds(int row, int keyIndex, int& x, int& y, int& w, int& h) {
  const char* keys = keyboardRowText(row);
  int keyCount = strlen(keys);
  int totalGap = (keyCount - 1) * WIFI_KEY_GAP;
  w = (WIFI_PANEL_W - 22 - totalGap) / keyCount;
  h = WIFI_KEY_H;
  int totalW = w * keyCount + totalGap;
  x = WIFI_PANEL_X + (WIFI_PANEL_W - totalW) / 2 + keyIndex * (w + WIFI_KEY_GAP);
  y = WIFI_KEY_START_Y + row * (WIFI_KEY_H + WIFI_KEY_GAP);
}

void drawKeyboardKeys(TFT_eSprite& s) {
  for (int row = 0; row < 4; ++row) {
    const char* keys = keyboardRowText(row);
    int keyCount = strlen(keys);
    for (int i = 0; i < keyCount; ++i) {
      int x, y, w, h;
      keyboardKeyBounds(row, i, x, y, w, h);
      char label[2] = {keys[i], '\0'};
      drawButton(s, x, y, w, h, label, TFT_WHITE, TFT_DARKGREEN);
    }
  }

  drawButton(s, WIFI_PANEL_X + 18, WIFI_KEYBOARD_SPECIAL_Y, 56, 22, keyboardMode == KEYBOARD_UPPER ? "lower" : "SHIFT",
             keyboardMode == KEYBOARD_UPPER ? TFT_NAVY : TFT_WHITE, keyboardMode == KEYBOARD_UPPER ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, WIFI_PANEL_X + 82, WIFI_KEYBOARD_SPECIAL_Y, 48, 22, keyboardMode == KEYBOARD_SYMBOLS ? "ABC" : "SYM",
             keyboardMode == KEYBOARD_SYMBOLS ? TFT_NAVY : TFT_WHITE, keyboardMode == KEYBOARD_SYMBOLS ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, WIFI_PANEL_X + 138, WIFI_KEYBOARD_SPECIAL_Y, 86, 22, "Space", TFT_WHITE, TFT_DARKGREEN);
  drawButton(s, WIFI_PANEL_X + 232, WIFI_KEYBOARD_SPECIAL_Y, 56, 22, "Del", TFT_WHITE, TFT_DARKGREEN);
}

void drawWifiMainPanel(TFT_eSprite& s) {
  char ssidLabel[24];
  const char* shownSsid = wifiConnecting ? pendingWifiSsid : wifiSsid;
  formatShortText(ssidLabel, sizeof(ssidLabel), shownSsid[0] ? shownSsid : "(none)", 18);

  drawWifiPanelBase(s, "WiFi");
  drawWifiToggleRow(s, WIFI_ROW_START_Y, "WiFi", !wifiEnabled, wifiEnabled);
  drawWifiActionRow(s, WIFI_ROW_START_Y + WIFI_ROW_GAP, "Network", ssidLabel,
                    wifiScanInProgress ? "Wait" : "List");
  drawWifiStatusRow(s, WIFI_ROW_START_Y + WIFI_ROW_GAP * 2, "Status", wifiStatusText);
  drawWifiStatusRow(s, WIFI_ROW_START_Y + WIFI_ROW_GAP * 3, "Time", internetTimeStatus());
  drawWifiStepRow(s, WIFI_ROW_START_Y + WIFI_ROW_GAP * 4, "Timezone", currentTimezone().label);
}

void drawWifiNetworksPanel(TFT_eSprite& s) {
  drawWifiPanelBase(s, "WiFi Networks");
  if (wifiScanInProgress) {
    s.setTextDatum(MC_DATUM);
    s.setTextColor(TFT_GREENYELLOW, TFT_NAVY);
    s.drawString("Scanning...", SCREEN_W / 2, WIFI_PANEL_Y + 92);
  } else if (wifiNetworkCount == 0) {
    s.setTextDatum(MC_DATUM);
    s.setTextColor(TFT_WHITE, TFT_NAVY);
    s.drawString("No networks found", SCREEN_W / 2, WIFI_PANEL_Y + 84);
  } else {
    int start = wifiNetworkPage * WIFI_LIST_ROW_COUNT;
    int end = start + WIFI_LIST_ROW_COUNT;
    if (end > wifiNetworkCount) end = wifiNetworkCount;
    for (int i = start; i < end; ++i) {
      int row = i - start;
      int y = WIFI_LIST_ROW_Y + row * WIFI_LIST_ROW_H;
      char ssidShort[24];
      char rssiText[10];
      formatShortText(ssidShort, sizeof(ssidShort), wifiNetworkNames[i], wifiNetworkOpen[i] ? 15 : 18);
      snprintf(rssiText, sizeof(rssiText), "%ddB", wifiNetworkRssi[i]);
      s.fillRoundRect(WIFI_PANEL_X + 12, y, WIFI_PANEL_W - 24, WIFI_LIST_ROW_H - 2, 4, TFT_DARKGREEN);
      s.drawRoundRect(WIFI_PANEL_X + 12, y, WIFI_PANEL_W - 24, WIFI_LIST_ROW_H - 2, 4, TFT_CYAN);
      s.setTextDatum(ML_DATUM);
      s.setTextColor(TFT_WHITE, TFT_DARKGREEN);
      s.drawString(ssidShort, WIFI_PANEL_X + 20, y + (WIFI_LIST_ROW_H - 2) / 2);
      if (wifiNetworkOpen[i]) {
        s.setTextColor(TFT_GREENYELLOW, TFT_DARKGREEN);
        s.drawString("open", WIFI_PANEL_X + 166, y + (WIFI_LIST_ROW_H - 2) / 2);
      }
      s.setTextDatum(MR_DATUM);
      s.setTextColor(TFT_CYAN, TFT_DARKGREEN);
      s.drawString(rssiText, WIFI_PANEL_X + WIFI_PANEL_W - 22, y + (WIFI_LIST_ROW_H - 2) / 2);
    }
  }

  int bottomY = WIFI_PANEL_Y + WIFI_PANEL_H - 30;
  drawButton(s, WIFI_PANEL_X + 18, bottomY, 58, 22, "Back", TFT_CYAN, TFT_DARKGREEN);
  drawButton(s, WIFI_PANEL_X + 84, bottomY, 58, 22, "Scan", TFT_CYAN, TFT_DARKGREEN);
  drawButton(s, WIFI_PANEL_X + 184, bottomY, 54, 22, "Prev", TFT_CYAN, TFT_DARKGREEN);
  drawButton(s, WIFI_PANEL_X + 246, bottomY, 54, 22, "Next", TFT_CYAN, TFT_DARKGREEN);
}

void drawWifiPasswordPanel(TFT_eSprite& s) {
  char title[42];
  char ssidShort[22];
  char passShort[34];
  formatShortText(ssidShort, sizeof(ssidShort), pendingWifiSsid, 15);
  formatShortText(passShort, sizeof(passShort), wifiPasswordBuffer, 29);
  snprintf(title, sizeof(title), "WiFi Password: %s", ssidShort);

  drawWifiPanelBase(s, title);
  s.setTextDatum(TL_DATUM);
  s.fillRoundRect(WIFI_PANEL_X + 12, WIFI_PANEL_Y + 35, WIFI_PANEL_W - 24, 22, 4, TFT_BLACK);
  s.drawRoundRect(WIFI_PANEL_X + 12, WIFI_PANEL_Y + 35, WIFI_PANEL_W - 24, 22, 4, TFT_CYAN);
  s.setTextColor(TFT_GREENYELLOW, TFT_BLACK);
  s.drawString(passShort[0] ? passShort : "password", WIFI_PANEL_X + 18, WIFI_PANEL_Y + 39);

  drawKeyboardKeys(s);
  drawButton(s, WIFI_PANEL_X + 18, WIFI_KEYBOARD_ACTION_Y, 76, 22, "Cancel", TFT_CYAN, TFT_DARKGREEN);
  drawButton(s, WIFI_PANEL_X + 226, WIFI_KEYBOARD_ACTION_Y, 76, 22, "Join", TFT_CYAN, TFT_DARKGREEN);
}

void drawWifiPanel(TFT_eSprite& s) {
  if (!wifiPanelOpen) return;
  if (wifiPanelMode == WIFI_PANEL_NETWORKS) {
    drawWifiNetworksPanel(s);
  } else if (wifiPanelMode == WIFI_PANEL_PASSWORD) {
    drawWifiPasswordPanel(s);
  } else {
    drawWifiMainPanel(s);
  }
}

void drawCaptureValueRow(TFT_eSprite& s, int rowY, const char* label, const char* value, const char* actionLabel) {
  char valueShort[22];
  formatShortText(valueShort, sizeof(valueShort), value, actionLabel && actionLabel[0] ? 15 : 20);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(label, CAPTURE_LABEL_X, rowY + 4);
  s.setTextDatum(TR_DATUM);
  s.setTextColor(TFT_GREENYELLOW, TFT_NAVY);
  s.drawString(valueShort, CAPTURE_VALUE_RIGHT_X, rowY + 4);
  s.setTextDatum(TL_DATUM);
  if (actionLabel && actionLabel[0]) {
    drawButton(s, CAPTURE_ACTION_X, rowY, CAPTURE_ACTION_W, CAPTURE_ROW_H, actionLabel, TFT_CYAN, TFT_DARKGREEN);
  }
}

void drawCapturePanel(TFT_eSprite& s) {
  if (!capturePanelOpen) return;
  s.fillRoundRect(CAPTURE_PANEL_X, CAPTURE_PANEL_Y, CAPTURE_PANEL_W, CAPTURE_PANEL_H, 8, TFT_NAVY);
  s.drawRoundRect(CAPTURE_PANEL_X, CAPTURE_PANEL_Y, CAPTURE_PANEL_W, CAPTURE_PANEL_H, 8, TFT_CYAN);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Capture", CAPTURE_PANEL_X + 10, CAPTURE_PANEL_Y + 10);
  drawButton(s, CAPTURE_CLOSE_X, CAPTURE_CLOSE_Y, CAPTURE_CLOSE_W, CAPTURE_CLOSE_H, "X", TFT_WHITE, TFT_RED);

  char fpsLabel[12];
  snprintf(fpsLabel, sizeof(fpsLabel), "%lu", captureNextSequenceIndex);
  drawCaptureValueRow(s, CAPTURE_ROW_START_Y, "SD", captureStatusText, "Check");
  drawCaptureValueRow(s, CAPTURE_ROW_START_Y + CAPTURE_ROW_GAP, "Frames", "Every frame", "Save");
  drawCaptureValueRow(s, CAPTURE_ROW_START_Y + CAPTURE_ROW_GAP * 2, "Next", fpsLabel, "");
  drawCaptureValueRow(s, CAPTURE_ROW_START_Y + CAPTURE_ROW_GAP * 3, "Last", captureLastFile[0] ? captureLastFile : "None", "");
}

void drawCaptureToast(TFT_eSprite& s) {
  unsigned long now = millis();
  if (capturePanelOpen || now >= captureToastUntilMs) return;
  const int w = 190;
  const int h = 38;
  const int x = (SCREEN_W - w) / 2;
  const int y = 32;
  s.fillRoundRect(x, y, w, h, 8, TFT_NAVY);
  s.drawRoundRect(x, y, w, h, 8, TFT_CYAN);
  s.setTextDatum(MC_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(captureStatusText, x + w / 2, y + h / 2);
  s.setTextDatum(TL_DATUM);
}

void drawSettingsTabs(TFT_eSprite& s) {
  bool tankActive = (activeSettingsTab == SETTINGS_TAB_TANK);
  bool seaweedActive = (activeSettingsTab == SETTINGS_TAB_SEAWEED);
  bool clockActive = (activeSettingsTab == SETTINGS_TAB_CLOCK);
  bool backgroundActive = (activeSettingsTab == SETTINGS_TAB_BACKGROUND);
  drawButton(s, SETTINGS_TANK_TAB_X, SETTINGS_TAB_Y, SETTINGS_TANK_TAB_W, SETTINGS_TAB_H, "Tank",
             tankActive ? TFT_NAVY : TFT_CYAN, tankActive ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, SETTINGS_SEAWEED_TAB_X, SETTINGS_TAB_Y, SETTINGS_SEAWEED_TAB_W, SETTINGS_TAB_H, "Seaweed",
             seaweedActive ? TFT_NAVY : TFT_CYAN, seaweedActive ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, SETTINGS_CLOCK_TAB_X, SETTINGS_TAB_Y, SETTINGS_CLOCK_TAB_W, SETTINGS_TAB_H, "Clock",
             clockActive ? TFT_NAVY : TFT_CYAN, clockActive ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, SETTINGS_BACKGROUND_TAB_X, SETTINGS_TAB_Y, SETTINGS_BACKGROUND_TAB_W, SETTINGS_TAB_H, "Backgrd",
             backgroundActive ? TFT_NAVY : TFT_CYAN, backgroundActive ? TFT_CYAN : TFT_DARKGREEN);
}

void drawClockEnableStyleRow(TFT_eSprite& s) {
  const int centerY = CLOCK_ROW_1_Y + SETTINGS_BUTTON_H / 2;
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Clock", SETTINGS_PANEL_X + 12, centerY);

  drawButton(s, CLOCK_STYLE_BUTTON_X, CLOCK_ROW_1_Y, CLOCK_STYLE_BUTTON_W, SETTINGS_BUTTON_H, "Style",
             clockStylePanelOpen ? TFT_NAVY : TFT_CYAN, clockStylePanelOpen ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, SETTINGS_MINUS_X, CLOCK_ROW_1_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H, "OFF",
             !clockVisible ? TFT_NAVY : TFT_WHITE, !clockVisible ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, SETTINGS_PLUS_X, CLOCK_ROW_1_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H, "ON",
             clockVisible ? TFT_NAVY : TFT_WHITE, clockVisible ? TFT_CYAN : TFT_DARKGREEN);
}

void drawClockSettings(TFT_eSprite& s) {
  char buf[24];
  drawClockEnableStyleRow(s);
  drawSettingToggleRow(s, CLOCK_ROW_2_Y, "Time", "Man", "Net", !clockUseInternetTime, clockUseInternetTime);
  drawSettingToggleRow(s, CLOCK_ROW_3_Y, "Format", "12h", "24h", !clockUse24Hour, clockUse24Hour);

  if (clockUseInternetTime) {
    drawSettingStatusRow(s, CLOCK_ROW_4_Y, "Internet", internetTimeStatus());
    drawSettingRow(s, CLOCK_ROW_5_Y, "Timezone", currentTimezone().label);
    return;
  }

  formatClockFieldValue(buf, sizeof(buf));
  drawSettingRow(s, CLOCK_ROW_4_Y, clockFieldName(), buf);
  drawSettingRow(s, CLOCK_ROW_5_Y, "Timezone", currentTimezone().label);
  drawButton(s, CLOCK_FIELD_PREV_X, CLOCK_FIELD_NAV_Y, CLOCK_FIELD_NAV_W, CLOCK_FIELD_NAV_H, "Prev", TFT_CYAN, TFT_DARKGREEN);
  drawButton(s, CLOCK_FIELD_NEXT_X, CLOCK_FIELD_NAV_Y, CLOCK_FIELD_NAV_W, CLOCK_FIELD_NAV_H, "Next", TFT_CYAN, TFT_DARKGREEN);
}

void drawRainbowSwatch(TFT_eSprite& s, int x, int y, int w, int h, int radius);

void drawClockStyleChoiceRow(TFT_eSprite& s, int rowY, const char* label, const char* leftLabel, const char* rightLabel,
                             bool leftActive, bool rightActive) {
  const int centerY = rowY + SETTINGS_BUTTON_H / 2;
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(label, CLOCK_STYLE_LABEL_X, centerY);
  drawButton(s, CLOCK_STYLE_LEFT_X, rowY, CLOCK_STYLE_CHOICE_W, SETTINGS_BUTTON_H, leftLabel,
             leftActive ? TFT_NAVY : TFT_WHITE, leftActive ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, CLOCK_STYLE_RIGHT_X, rowY, CLOCK_STYLE_CHOICE_W, SETTINGS_BUTTON_H, rightLabel,
             rightActive ? TFT_NAVY : TFT_WHITE, rightActive ? TFT_CYAN : TFT_DARKGREEN);
}

void drawClockStyleColorRow(TFT_eSprite& s) {
  const int centerY = CLOCK_STYLE_ROW_4_Y + SETTINGS_BUTTON_H / 2;
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Colour", CLOCK_STYLE_LABEL_X, centerY);

  uint16_t color = activeClockTextColor();
  if (activeClockTextRainbowEnabled()) {
    drawRainbowSwatch(s, CLOCK_STYLE_SWATCH_X, CLOCK_STYLE_ROW_4_Y + 3, CLOCK_STYLE_SWATCH_W,
                      SETTINGS_BUTTON_H - 6, 3);
  } else {
    s.fillRoundRect(CLOCK_STYLE_SWATCH_X, CLOCK_STYLE_ROW_4_Y + 3, CLOCK_STYLE_SWATCH_W, SETTINGS_BUTTON_H - 6, 3,
                    color);
  }
  s.drawRoundRect(CLOCK_STYLE_SWATCH_X, CLOCK_STYLE_ROW_4_Y + 3, CLOCK_STYLE_SWATCH_W, SETTINGS_BUTTON_H - 6, 3,
                  (!activeClockTextRainbowEnabled() && color == TFT_WHITE) ? TFT_DARKGREY : TFT_WHITE);
  drawButton(s, CLOCK_STYLE_COLOR_BUTTON_X, CLOCK_STYLE_ROW_4_Y, CLOCK_STYLE_COLOR_BUTTON_W, SETTINGS_BUTTON_H,
             "Pick", clockColorPanelOpen ? TFT_NAVY : TFT_CYAN, clockColorPanelOpen ? TFT_CYAN : TFT_DARKGREEN);
}

void drawClockStyleFontRow(TFT_eSprite& s) {
  const int centerY = CLOCK_STYLE_ROW_2_Y + SETTINGS_BUTTON_H / 2;
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Font", CLOCK_STYLE_LABEL_X, centerY);

  s.setTextDatum(MR_DATUM);
  s.setTextColor(TFT_GREENYELLOW, TFT_NAVY);
  s.drawString(currentAsciiClockFont().label, CLOCK_STYLE_FONT_VALUE_RIGHT_X, centerY);
  drawButton(s, CLOCK_STYLE_FONT_MINUS_X, CLOCK_STYLE_ROW_2_Y, CLOCK_STYLE_FONT_BUTTON_W, SETTINGS_BUTTON_H, "-",
             TFT_WHITE, TFT_DARKGREEN);
  drawButton(s, CLOCK_STYLE_FONT_PLUS_X, CLOCK_STYLE_ROW_2_Y, CLOCK_STYLE_FONT_BUTTON_W, SETTINGS_BUTTON_H, "+",
             TFT_WHITE, TFT_DARKGREEN);
}

void drawClockColorPanel(TFT_eSprite& s) {
  if (!clockColorPanelOpen) return;

  s.fillRoundRect(CLOCK_COLOR_PANEL_X, CLOCK_COLOR_PANEL_Y, CLOCK_COLOR_PANEL_W, CLOCK_COLOR_PANEL_H, 8, TFT_NAVY);
  s.drawRoundRect(CLOCK_COLOR_PANEL_X, CLOCK_COLOR_PANEL_Y, CLOCK_COLOR_PANEL_W, CLOCK_COLOR_PANEL_H, 8, TFT_CYAN);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Clock Colour", CLOCK_COLOR_PANEL_X + 12, CLOCK_COLOR_PANEL_Y + 12);

  uint16_t selected = storedActiveClockTextColor();
  const int totalSwatches = BACKGROUND_COLOR_COUNT + 1;
  for (int i = 0; i < totalSwatches; ++i) {
    int col = i % CLOCK_COLOR_SWATCH_COLS;
    int row = i / CLOCK_COLOR_SWATCH_COLS;
    int x = CLOCK_COLOR_GRID_X + col * (CLOCK_COLOR_SWATCH_W + CLOCK_COLOR_SWATCH_GAP_X);
    int y = CLOCK_COLOR_GRID_Y + row * (CLOCK_COLOR_SWATCH_H + CLOCK_COLOR_SWATCH_GAP_Y);
    bool isRainbow = (i == 0);
    uint16_t swatchColor = isRainbow ? TFT_BLACK : kBackgroundColorPalette[i - 1];
    bool isSelected = isRainbow ? activeClockTextRainbowEnabled()
                                : (!activeClockTextRainbowEnabled() && swatchColor == selected);

    if (isSelected) {
      s.drawRoundRect(x - 3, y - 3, CLOCK_COLOR_SWATCH_W + 6, CLOCK_COLOR_SWATCH_H + 6, 5, TFT_WHITE);
    }
    if (isRainbow) {
      drawRainbowSwatch(s, x, y, CLOCK_COLOR_SWATCH_W, CLOCK_COLOR_SWATCH_H, 4);
    } else {
      s.fillRoundRect(x, y, CLOCK_COLOR_SWATCH_W, CLOCK_COLOR_SWATCH_H, 4, swatchColor);
    }
    s.drawRoundRect(x, y, CLOCK_COLOR_SWATCH_W, CLOCK_COLOR_SWATCH_H, 4, isSelected ? TFT_YELLOW : TFT_CYAN);
    if (!isRainbow && swatchColor == TFT_WHITE) {
      s.drawRoundRect(x + 1, y + 1, CLOCK_COLOR_SWATCH_W - 2, CLOCK_COLOR_SWATCH_H - 2, 3, TFT_DARKGREY);
    }
  }

  drawButton(s, CLOCK_COLOR_CLOSE_X, CLOCK_COLOR_CLOSE_Y, CLOCK_COLOR_CLOSE_W, CLOCK_COLOR_CLOSE_H, "X", TFT_WHITE, TFT_RED);
}

void drawRainbowSwatch(TFT_eSprite& s, int x, int y, int w, int h, int radius) {
  s.fillRoundRect(x, y, w, h, radius, TFT_BLACK);
  for (int i = 1; i < w - 1; ++i) {
    uint16_t color = colorWheel565((uint16_t)((i * 1536) / max(1, w - 2)));
    s.drawFastVLine(x + i, y + 1, h - 2, color);
  }
}

void drawBackgroundColorRow(TFT_eSprite& s, int rowY) {
  const int centerY = rowY + SETTINGS_BUTTON_H / 2;
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Colour", SETTINGS_PANEL_X + 12, centerY);

  if (backgroundRainbowEnabled) {
    drawRainbowSwatch(s, BACKGROUND_COLOR_SWATCH_X, rowY + 3, BACKGROUND_COLOR_SWATCH_W, SETTINGS_BUTTON_H - 6, 3);
  } else {
    s.fillRoundRect(BACKGROUND_COLOR_SWATCH_X, rowY + 3, BACKGROUND_COLOR_SWATCH_W, SETTINGS_BUTTON_H - 6, 3,
                    backgroundGradientColor);
  }
  s.drawRoundRect(BACKGROUND_COLOR_SWATCH_X, rowY + 3, BACKGROUND_COLOR_SWATCH_W, SETTINGS_BUTTON_H - 6, 3,
                  (!backgroundRainbowEnabled && backgroundGradientColor == TFT_WHITE) ? TFT_DARKGREY : TFT_WHITE);
  drawButton(s, BACKGROUND_COLOR_BUTTON_X, rowY, BACKGROUND_COLOR_BUTTON_W, SETTINGS_BUTTON_H, "Pick",
             backgroundColorPanelOpen ? TFT_NAVY : TFT_CYAN, backgroundColorPanelOpen ? TFT_CYAN : TFT_DARKGREEN);
}

bool backgroundColorPickerEditingAutoSky() {
  return activeAutoSkyColorSlot != AUTO_SKY_SLOT_NONE;
}

void drawBackgroundAutoSkyRow(TFT_eSprite& s, int rowY) {
  const int centerY = rowY + SETTINGS_BUTTON_H / 2;
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Auto Sky", SETTINGS_PANEL_X + 12, centerY);

  if (autoSkyEnabled) {
    drawButton(s, AUTO_SKY_PICK_X, rowY, AUTO_SKY_PICK_W, SETTINGS_BUTTON_H, "Pick",
               autoSkyPanelOpen ? TFT_NAVY : TFT_CYAN, autoSkyPanelOpen ? TFT_CYAN : TFT_DARKGREEN);
  }
  drawButton(s, SETTINGS_MINUS_X, rowY, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H, "Off",
             !autoSkyEnabled ? TFT_NAVY : TFT_WHITE, !autoSkyEnabled ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, SETTINGS_PLUS_X, rowY, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H, "On",
             autoSkyEnabled ? TFT_NAVY : TFT_WHITE, autoSkyEnabled ? TFT_CYAN : TFT_DARKGREEN);
}

void drawBackgroundColorPanel(TFT_eSprite& s) {
  if (!backgroundColorPanelOpen) return;

  s.fillRoundRect(BACKGROUND_COLOR_PANEL_X, BACKGROUND_COLOR_PANEL_Y, BACKGROUND_COLOR_PANEL_W, BACKGROUND_COLOR_PANEL_H,
                  8, TFT_NAVY);
  s.drawRoundRect(BACKGROUND_COLOR_PANEL_X, BACKGROUND_COLOR_PANEL_Y, BACKGROUND_COLOR_PANEL_W, BACKGROUND_COLOR_PANEL_H,
                  8, TFT_CYAN);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  char title[28];
  if (backgroundColorPickerEditingAutoSky()) {
    snprintf(title, sizeof(title), "%s Colour", autoSkySlotName(activeAutoSkyColorSlot));
  } else {
    snprintf(title, sizeof(title), "Background Colour");
  }
  s.drawString(title, BACKGROUND_COLOR_PANEL_X + 12, BACKGROUND_COLOR_PANEL_Y + 12);

  bool includeRainbow = !backgroundColorPickerEditingAutoSky();
  uint16_t selectedColor = backgroundColorPickerEditingAutoSky() ? autoSkySlotColor(activeAutoSkyColorSlot)
                                                                 : backgroundGradientColor;
  const int totalSwatches = BACKGROUND_COLOR_COUNT + (includeRainbow ? 1 : 0);
  for (int i = 0; i < totalSwatches; ++i) {
    int col = i % BACKGROUND_COLOR_SWATCH_COLS;
    int row = i / BACKGROUND_COLOR_SWATCH_COLS;
    int x = BACKGROUND_COLOR_GRID_X + col * (BACKGROUND_COLOR_SWATCH_GRID_W + BACKGROUND_COLOR_SWATCH_GAP_X);
    int y = BACKGROUND_COLOR_GRID_Y + row * (BACKGROUND_COLOR_SWATCH_GRID_H + BACKGROUND_COLOR_SWATCH_GAP_Y);
    bool isRainbow = includeRainbow && (i == 0);
    int paletteIndex = includeRainbow ? i - 1 : i;
    uint16_t swatchColor = isRainbow ? TFT_BLACK : kBackgroundColorPalette[paletteIndex];
    bool isSelected = isRainbow ? backgroundRainbowEnabled
                                : (!backgroundRainbowEnabled && swatchColor == selectedColor);

    if (isSelected) {
      s.drawRoundRect(x - 3, y - 3, BACKGROUND_COLOR_SWATCH_GRID_W + 6, BACKGROUND_COLOR_SWATCH_GRID_H + 6, 5,
                      TFT_WHITE);
    }
    if (isRainbow) {
      drawRainbowSwatch(s, x, y, BACKGROUND_COLOR_SWATCH_GRID_W, BACKGROUND_COLOR_SWATCH_GRID_H, 4);
    } else {
      s.fillRoundRect(x, y, BACKGROUND_COLOR_SWATCH_GRID_W, BACKGROUND_COLOR_SWATCH_GRID_H, 4, swatchColor);
    }
    s.drawRoundRect(x, y, BACKGROUND_COLOR_SWATCH_GRID_W, BACKGROUND_COLOR_SWATCH_GRID_H, 4,
                    isSelected ? TFT_YELLOW : TFT_CYAN);
    if (!isRainbow && swatchColor == TFT_WHITE) {
      s.drawRoundRect(x + 1, y + 1, BACKGROUND_COLOR_SWATCH_GRID_W - 2, BACKGROUND_COLOR_SWATCH_GRID_H - 2, 3,
                      TFT_DARKGREY);
    }
  }

  drawButton(s, BACKGROUND_COLOR_CLOSE_X, BACKGROUND_COLOR_CLOSE_Y, BACKGROUND_COLOR_CLOSE_W, BACKGROUND_COLOR_CLOSE_H,
             "X", TFT_WHITE, TFT_RED);
}

void drawAutoSkyPanel(TFT_eSprite& s) {
  if (!autoSkyPanelOpen) return;

  s.fillRoundRect(AUTO_SKY_PANEL_X, AUTO_SKY_PANEL_Y, AUTO_SKY_PANEL_W, AUTO_SKY_PANEL_H, 8, TFT_NAVY);
  s.drawRoundRect(AUTO_SKY_PANEL_X, AUTO_SKY_PANEL_Y, AUTO_SKY_PANEL_W, AUTO_SKY_PANEL_H, 8, TFT_CYAN);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Pick the colours for time of day", AUTO_SKY_PANEL_X + 10, AUTO_SKY_PANEL_Y + 12);

  drawButton(s, AUTO_SKY_BUTTON_LEFT_X, AUTO_SKY_BUTTON_TOP_Y, AUTO_SKY_BUTTON_W, AUTO_SKY_BUTTON_H,
             "Sunrise", TFT_CYAN, TFT_DARKGREEN);
  drawButton(s, AUTO_SKY_BUTTON_RIGHT_X, AUTO_SKY_BUTTON_TOP_Y, AUTO_SKY_BUTTON_W, AUTO_SKY_BUTTON_H,
             "Day", TFT_CYAN, TFT_DARKGREEN);
  drawButton(s, AUTO_SKY_BUTTON_LEFT_X, AUTO_SKY_BUTTON_BOTTOM_Y, AUTO_SKY_BUTTON_W, AUTO_SKY_BUTTON_H,
             "Sunset", TFT_CYAN, TFT_DARKGREEN);
  drawButton(s, AUTO_SKY_BUTTON_RIGHT_X, AUTO_SKY_BUTTON_BOTTOM_Y, AUTO_SKY_BUTTON_W, AUTO_SKY_BUTTON_H,
             "Night", TFT_CYAN, TFT_DARKGREEN);
  drawButton(s, AUTO_SKY_CLOSE_X, AUTO_SKY_CLOSE_Y, AUTO_SKY_CLOSE_W, AUTO_SKY_CLOSE_H, "X", TFT_WHITE, TFT_RED);
}

void drawBacklightValueRow(TFT_eSprite& s, int rowY, const char* label, const char* value) {
  int centerY = rowY + BACKLIGHT_BUTTON_H / 2;
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(label, BACKLIGHT_LABEL_X, centerY);

  s.setTextDatum(MR_DATUM);
  s.setTextColor(TFT_GREENYELLOW, TFT_NAVY);
  s.drawString(value, BACKLIGHT_VALUE_RIGHT_X, centerY);
  drawButton(s, BACKLIGHT_MINUS_X, rowY, BACKLIGHT_BUTTON_W, BACKLIGHT_BUTTON_H, "-", TFT_WHITE, TFT_DARKGREEN);
  drawButton(s, BACKLIGHT_PLUS_X, rowY, BACKLIGHT_BUTTON_W, BACKLIGHT_BUTTON_H, "+", TFT_WHITE, TFT_DARKGREEN);
}

void drawBacklightToggleRow(TFT_eSprite& s, int rowY, const char* label, const char* leftLabel, const char* rightLabel,
                            bool leftActive, bool rightActive) {
  int centerY = rowY + BACKLIGHT_BUTTON_H / 2;
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(label, BACKLIGHT_LABEL_X, centerY);
  drawButton(s, BACKLIGHT_MINUS_X, rowY, BACKLIGHT_BUTTON_W, BACKLIGHT_BUTTON_H, leftLabel,
             leftActive ? TFT_NAVY : TFT_WHITE, leftActive ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, BACKLIGHT_PLUS_X, rowY, BACKLIGHT_BUTTON_W, BACKLIGHT_BUTTON_H, rightLabel,
             rightActive ? TFT_NAVY : TFT_WHITE, rightActive ? TFT_CYAN : TFT_DARKGREEN);
}

void formatScheduleHour(char* out, size_t outCap, int hour) {
  hour = clampVal(hour, 0, 23);
  int displayHour = hour % 12;
  if (displayHour == 0) displayHour = 12;
  snprintf(out, outCap, "%d%s", displayHour, hour < 12 ? "am" : "pm");
}

void formatScheduleDimTime(char* out, size_t outCap) {
  if (lightScheduleDimMinutes >= 60) {
    copySafe(out, outCap, "1h");
  } else {
    snprintf(out, outCap, "%dm", lightScheduleDimMinutes);
  }
}

void drawLightScheduleValueRow(TFT_eSprite& s, int rowY, const char* label, const char* value) {
  int centerY = rowY + LIGHT_SCHEDULE_BUTTON_H / 2;
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString(label, LIGHT_SCHEDULE_LABEL_X, centerY);

  s.setTextDatum(MR_DATUM);
  s.setTextColor(TFT_GREENYELLOW, TFT_NAVY);
  s.drawString(value, LIGHT_SCHEDULE_VALUE_RIGHT_X, centerY);

  drawButton(s, LIGHT_SCHEDULE_MINUS_X, rowY, LIGHT_SCHEDULE_BUTTON_W, LIGHT_SCHEDULE_BUTTON_H, "-", TFT_WHITE,
             TFT_DARKGREEN);
  drawButton(s, LIGHT_SCHEDULE_PLUS_X, rowY, LIGHT_SCHEDULE_BUTTON_W, LIGHT_SCHEDULE_BUTTON_H, "+", TFT_WHITE,
             TFT_DARKGREEN);
}

void drawLightScheduleToggleRow(TFT_eSprite& s, int rowY) {
  int centerY = rowY + LIGHT_SCHEDULE_BUTTON_H / 2;
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Schedule", LIGHT_SCHEDULE_LABEL_X, centerY);
  drawButton(s, LIGHT_SCHEDULE_MINUS_X, rowY, LIGHT_SCHEDULE_BUTTON_W, LIGHT_SCHEDULE_BUTTON_H, "OFF",
             !lightScheduleEnabled ? TFT_NAVY : TFT_WHITE, !lightScheduleEnabled ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, LIGHT_SCHEDULE_PLUS_X, rowY, LIGHT_SCHEDULE_BUTTON_W, LIGHT_SCHEDULE_BUTTON_H, "ON",
             lightScheduleEnabled ? TFT_NAVY : TFT_WHITE, lightScheduleEnabled ? TFT_CYAN : TFT_DARKGREEN);
}

void drawBacklightColorRow(TFT_eSprite& s, int rowY) {
  int centerY = rowY + BACKLIGHT_BUTTON_H / 2;
  uint16_t color = activeAmbientLightColor();
  bool showRainbow = (ambientLightLinkedToBackground && backgroundRainbowEnabled) ||
                     (!ambientLightLinkedToBackground && ambientLightRainbowEnabled);
  s.setTextDatum(ML_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Colour", BACKLIGHT_LABEL_X, centerY);
  if (showRainbow) {
    drawRainbowSwatch(s, BACKLIGHT_COLOR_SWATCH_X, rowY + 3, BACKLIGHT_COLOR_SWATCH_W, BACKLIGHT_BUTTON_H - 6, 3);
  } else {
    s.fillRoundRect(BACKLIGHT_COLOR_SWATCH_X, rowY + 3, BACKLIGHT_COLOR_SWATCH_W, BACKLIGHT_BUTTON_H - 6, 3, color);
  }
  s.drawRoundRect(BACKLIGHT_COLOR_SWATCH_X, rowY + 3, BACKLIGHT_COLOR_SWATCH_W, BACKLIGHT_BUTTON_H - 6, 3,
                  (!showRainbow && color == TFT_WHITE) ? TFT_DARKGREY : TFT_WHITE);
  if (ambientLightLinkedToBackground) {
    s.setTextDatum(MR_DATUM);
    s.setTextColor(TFT_GREENYELLOW, TFT_NAVY);
    s.drawString("BG Linked", BACKLIGHT_PANEL_X + BACKLIGHT_PANEL_W - 12, centerY);
  } else {
    drawButton(s, BACKLIGHT_ACTION_X, rowY, BACKLIGHT_ACTION_W, BACKLIGHT_BUTTON_H, "Pick",
               ambientColorPanelOpen ? TFT_NAVY : TFT_CYAN, ambientColorPanelOpen ? TFT_CYAN : TFT_DARKGREEN);
  }
}

void drawBacklightPanel(TFT_eSprite& s) {
  if (!backlightPanelOpen) return;

  s.fillRoundRect(BACKLIGHT_PANEL_X, BACKLIGHT_PANEL_Y, BACKLIGHT_PANEL_W, BACKLIGHT_PANEL_H, 8, TFT_NAVY);
  s.drawRoundRect(BACKLIGHT_PANEL_X, BACKLIGHT_PANEL_Y, BACKLIGHT_PANEL_W, BACKLIGHT_PANEL_H, 8, TFT_CYAN);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Backlight", BACKLIGHT_PANEL_X + 10, BACKLIGHT_PANEL_Y + 10);

  char value[16];
  snprintf(value, sizeof(value), "%d%%", lcdBacklightBrightness);
  drawBacklightValueRow(s, BACKLIGHT_ROW_START_Y, "LCD Bright", value);
  drawBacklightToggleRow(s, BACKLIGHT_ROW_START_Y + BACKLIGHT_ROW_GAP, "Ambient", "OFF", "ON",
                         !ambientLightEnabled, ambientLightEnabled);
  snprintf(value, sizeof(value), "%d%%", ambientLightBrightness);
  drawBacklightValueRow(s, BACKLIGHT_ROW_START_Y + BACKLIGHT_ROW_GAP * 2, "Amb Bright", value);
  drawButton(s, BACKLIGHT_COLOUR_MODE_X, BACKLIGHT_BOTTOM_BUTTON_Y, BACKLIGHT_BOTTOM_BUTTON_W, BACKLIGHT_BUTTON_H,
             "Colour Mode", lightColorModePanelOpen ? TFT_NAVY : TFT_CYAN,
             lightColorModePanelOpen ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, BACKLIGHT_SCHEDULE_X, BACKLIGHT_BOTTOM_BUTTON_Y, BACKLIGHT_BOTTOM_BUTTON_W, BACKLIGHT_BUTTON_H,
             "Schedule", lightSchedulePanelOpen ? TFT_NAVY : TFT_CYAN,
             lightSchedulePanelOpen ? TFT_CYAN : TFT_DARKGREEN);
  drawButton(s, BACKLIGHT_CLOSE_X, BACKLIGHT_CLOSE_Y, BACKLIGHT_CLOSE_W, BACKLIGHT_CLOSE_H, "X", TFT_WHITE, TFT_RED);
}

void drawLightColorModePanel(TFT_eSprite& s) {
  if (!lightColorModePanelOpen) return;

  s.fillRoundRect(LIGHT_COLOR_PANEL_X, LIGHT_COLOR_PANEL_Y, LIGHT_COLOR_PANEL_W, LIGHT_COLOR_PANEL_H, 8, TFT_NAVY);
  s.drawRoundRect(LIGHT_COLOR_PANEL_X, LIGHT_COLOR_PANEL_Y, LIGHT_COLOR_PANEL_W, LIGHT_COLOR_PANEL_H, 8, TFT_CYAN);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Colour Mode", LIGHT_COLOR_PANEL_X + 10, LIGHT_COLOR_PANEL_Y + 10);

  drawBacklightToggleRow(s, LIGHT_COLOR_ROW_START_Y, "Mode", "BG", "Pick",
                         ambientLightLinkedToBackground, !ambientLightLinkedToBackground);
  drawBacklightColorRow(s, LIGHT_COLOR_ROW_START_Y + LIGHT_COLOR_ROW_GAP);
  drawButton(s, LIGHT_COLOR_CLOSE_X, LIGHT_COLOR_CLOSE_Y, LIGHT_COLOR_CLOSE_W, LIGHT_COLOR_CLOSE_H, "X",
             TFT_WHITE, TFT_RED);
}

void drawLightSchedulePanel(TFT_eSprite& s) {
  if (!lightSchedulePanelOpen) return;

  s.fillRoundRect(LIGHT_SCHEDULE_PANEL_X, LIGHT_SCHEDULE_PANEL_Y, LIGHT_SCHEDULE_PANEL_W, LIGHT_SCHEDULE_PANEL_H, 8,
                  TFT_NAVY);
  s.drawRoundRect(LIGHT_SCHEDULE_PANEL_X, LIGHT_SCHEDULE_PANEL_Y, LIGHT_SCHEDULE_PANEL_W, LIGHT_SCHEDULE_PANEL_H, 8,
                  TFT_CYAN);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Light Schedule", LIGHT_SCHEDULE_PANEL_X + 10, LIGHT_SCHEDULE_PANEL_Y + 10);

  drawLightScheduleToggleRow(s, LIGHT_SCHEDULE_ROW_START_Y);
  if (lightScheduleEnabled) {
    char value[16];
    formatScheduleHour(value, sizeof(value), lightScheduleStartHour);
    drawLightScheduleValueRow(s, LIGHT_SCHEDULE_ROW_START_Y + LIGHT_SCHEDULE_ROW_GAP, "Start", value);
    formatScheduleHour(value, sizeof(value), lightScheduleEndHour);
    drawLightScheduleValueRow(s, LIGHT_SCHEDULE_ROW_START_Y + LIGHT_SCHEDULE_ROW_GAP * 2, "End", value);
    formatScheduleDimTime(value, sizeof(value));
    drawLightScheduleValueRow(s, LIGHT_SCHEDULE_ROW_START_Y + LIGHT_SCHEDULE_ROW_GAP * 3, "Dim Time", value);
    drawLightScheduleValueRow(s, LIGHT_SCHEDULE_ROW_START_Y + LIGHT_SCHEDULE_ROW_GAP * 4, "Dimming",
                              lightScheduleDimmingName());
  }

  drawButton(s, LIGHT_SCHEDULE_CLOSE_X, LIGHT_SCHEDULE_CLOSE_Y, LIGHT_SCHEDULE_CLOSE_W, LIGHT_SCHEDULE_CLOSE_H, "X",
             TFT_WHITE, TFT_RED);
}

void drawAmbientColorPanel(TFT_eSprite& s) {
  if (!ambientColorPanelOpen) return;

  s.fillRoundRect(AMBIENT_COLOR_PANEL_X, AMBIENT_COLOR_PANEL_Y, AMBIENT_COLOR_PANEL_W, AMBIENT_COLOR_PANEL_H, 8,
                  TFT_NAVY);
  s.drawRoundRect(AMBIENT_COLOR_PANEL_X, AMBIENT_COLOR_PANEL_Y, AMBIENT_COLOR_PANEL_W, AMBIENT_COLOR_PANEL_H, 8,
                  TFT_CYAN);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Ambient Colour", AMBIENT_COLOR_PANEL_X + 12, AMBIENT_COLOR_PANEL_Y + 12);

  const int totalSwatches = BACKGROUND_COLOR_COUNT + 1;
  for (int i = 0; i < totalSwatches; ++i) {
    int col = i % AMBIENT_COLOR_SWATCH_COLS;
    int row = i / AMBIENT_COLOR_SWATCH_COLS;
    int x = AMBIENT_COLOR_GRID_X + col * (AMBIENT_COLOR_SWATCH_W + AMBIENT_COLOR_SWATCH_GAP_X);
    int y = AMBIENT_COLOR_GRID_Y + row * (AMBIENT_COLOR_SWATCH_H + AMBIENT_COLOR_SWATCH_GAP_Y);
    bool isRainbow = (i == 0);
    uint16_t swatchColor = isRainbow ? TFT_BLACK : kBackgroundColorPalette[i - 1];
    bool isSelected = isRainbow ? ambientLightRainbowEnabled
                                : (!ambientLightRainbowEnabled && swatchColor == ambientLightColor);

    if (isSelected) {
      s.drawRoundRect(x - 3, y - 3, AMBIENT_COLOR_SWATCH_W + 6, AMBIENT_COLOR_SWATCH_H + 6, 5, TFT_WHITE);
    }
    if (isRainbow) {
      drawRainbowSwatch(s, x, y, AMBIENT_COLOR_SWATCH_W, AMBIENT_COLOR_SWATCH_H, 4);
    } else {
      s.fillRoundRect(x, y, AMBIENT_COLOR_SWATCH_W, AMBIENT_COLOR_SWATCH_H, 4, swatchColor);
    }
    s.drawRoundRect(x, y, AMBIENT_COLOR_SWATCH_W, AMBIENT_COLOR_SWATCH_H, 4, isSelected ? TFT_YELLOW : TFT_CYAN);
    if (!isRainbow && swatchColor == TFT_WHITE) {
      s.drawRoundRect(x + 1, y + 1, AMBIENT_COLOR_SWATCH_W - 2, AMBIENT_COLOR_SWATCH_H - 2, 3, TFT_DARKGREY);
    }
  }

  drawButton(s, AMBIENT_COLOR_CLOSE_X, AMBIENT_COLOR_CLOSE_Y, AMBIENT_COLOR_CLOSE_W, AMBIENT_COLOR_CLOSE_H, "X",
             TFT_WHITE, TFT_RED);
}

void drawClockStylePanel(TFT_eSprite& s) {
  if (!clockStylePanelOpen) return;

  s.fillRoundRect(CLOCK_STYLE_PANEL_X, CLOCK_STYLE_PANEL_Y, CLOCK_STYLE_PANEL_W, CLOCK_STYLE_PANEL_H, 8, TFT_NAVY);
  s.drawRoundRect(CLOCK_STYLE_PANEL_X, CLOCK_STYLE_PANEL_Y, CLOCK_STYLE_PANEL_W, CLOCK_STYLE_PANEL_H, 8, TFT_CYAN);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Clock Style", CLOCK_STYLE_PANEL_X + 12, CLOCK_STYLE_PANEL_Y + 12);

  drawClockStyleChoiceRow(s, CLOCK_STYLE_ROW_1_Y, "Style", "Small", "ASCII",
                          clockDisplayStyle == CLOCK_STYLE_SMALL_TEXT, clockDisplayStyle == CLOCK_STYLE_ASCII);

  if (clockDisplayStyle == CLOCK_STYLE_SMALL_TEXT) {
    drawClockStyleChoiceRow(s, CLOCK_STYLE_ROW_2_Y, "Position", "Top", "Bottom",
                            clockSmallPosition == CLOCK_SMALL_TOP, clockSmallPosition == CLOCK_SMALL_BOTTOM);
  } else {
    drawClockStyleFontRow(s);
  }

  drawClockStyleChoiceRow(s, CLOCK_STYLE_ROW_3_Y, "Flip Clock", "Off", "On",
                          !clockFlipHorizontal, clockFlipHorizontal);
  drawClockStyleColorRow(s);
  drawButton(s, CLOCK_STYLE_CLOSE_X, CLOCK_STYLE_CLOSE_Y, CLOCK_STYLE_CLOSE_W, CLOCK_STYLE_CLOSE_H, "X", TFT_WHITE, TFT_RED);
}

void drawSettingsPanel(TFT_eSprite& s) {
  if (!settingsOpen) return;
  int px = SETTINGS_PANEL_X, py = SETTINGS_PANEL_Y, pw = SETTINGS_PANEL_W, ph = SETTINGS_PANEL_H;
  s.fillRoundRect(px, py, pw, ph, 8, TFT_NAVY);
  s.drawRoundRect(px, py, pw, ph, 8, TFT_CYAN);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Settings", px + 10, py + 10);

  char buf[24];
  if (activeSettingsTab == SETTINGS_TAB_TANK) {
    snprintf(buf, sizeof(buf), "%d", fishTargetCount);
    drawSettingRow(s, SETTINGS_ROW_START_Y, "Fish Population", buf);

    snprintf(buf, sizeof(buf), "%d", bubbleTargetCount);
    drawSettingRow(s, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP, "Bubble Amount", buf);

    drawActionRow(s, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP * 2, "Timed Events", "Events");
    drawSettingToggleRow(s, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP * 3, "Flip Display", "Off", "On",
                         !displayFlip180, displayFlip180);
  } else if (activeSettingsTab == SETTINGS_TAB_SEAWEED) {
    snprintf(buf, sizeof(buf), "%.2f", seaweedSwaySpeed);
    drawSettingRow(s, SETTINGS_ROW_START_Y, "Sway", buf);

    snprintf(buf, sizeof(buf), "%.2f", seaweedLength);
    drawSettingRow(s, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP, "Length", buf);

    snprintf(buf, sizeof(buf), "%.2f", seaweedLengthRandomness);
    drawSettingRow(s, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP * 2, "Length Rand", buf);
  } else if (activeSettingsTab == SETTINGS_TAB_CLOCK) {
    drawClockSettings(s);
  } else {
    drawSettingRow(s, SETTINGS_ROW_START_Y, "Style", backgroundStyleName());
    if (backgroundUsesGradientColor() && !autoSkyEnabled) {
      drawBackgroundColorRow(s, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP);
    } else if (backgroundStyle == BACKGROUND_STYLE_FLOWERS && !autoSkyEnabled) {
      drawActionRow(s, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP, "Flowers", "Randomize");
    }
    drawBackgroundAutoSkyRow(s, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP * 2);
  }

  drawButton(s, SETTINGS_CLOSE_X, SETTINGS_CLOSE_Y, SETTINGS_CLOSE_W, SETTINGS_CLOSE_H, "X", TFT_WHITE, TFT_RED);
  drawSettingsTabs(s);
}

void drawEventsPanel(TFT_eSprite& s) {
  if (!eventsPanelOpen) return;

  s.fillRoundRect(EVENTS_PANEL_X, EVENTS_PANEL_Y, EVENTS_PANEL_W, EVENTS_PANEL_H, 8, TFT_NAVY);
  s.drawRoundRect(EVENTS_PANEL_X, EVENTS_PANEL_Y, EVENTS_PANEL_W, EVENTS_PANEL_H, 8, TFT_CYAN);
  s.setTextSize(1);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Timed Events", EVENTS_PANEL_X + 12, EVENTS_PANEL_Y + 12);

  drawEventFrequencyRow(s, EVENTS_ROW_START_Y, "Auto Feed", autoFeedFrequency);
  drawEventActionRow(s, EVENTS_ROW_START_Y + EVENTS_ROW_GAP, "Creatures", "Open");

  drawButton(s, EVENTS_CLOSE_X, EVENTS_CLOSE_Y, EVENTS_CLOSE_W, EVENTS_CLOSE_H, "X", TFT_WHITE, TFT_RED);
}

void drawCreaturesPanel(TFT_eSprite& s) {
  if (!creaturesPanelOpen) return;

  s.fillRoundRect(CREATURES_PANEL_X, CREATURES_PANEL_Y, CREATURES_PANEL_W, CREATURES_PANEL_H, 8, TFT_NAVY);
  s.drawRoundRect(CREATURES_PANEL_X, CREATURES_PANEL_Y, CREATURES_PANEL_W, CREATURES_PANEL_H, 8, TFT_CYAN);
  s.setTextSize(1);
  s.setTextDatum(TL_DATUM);
  s.setTextColor(TFT_WHITE, TFT_NAVY);
  s.drawString("Creature Events", CREATURES_PANEL_X + 12, CREATURES_PANEL_Y + 12);

  drawCreatureFrequencyRow(s, CREATURES_ROW_START_Y, "Octopus", octopusFrequency);
  drawCreatureFrequencyRow(s, CREATURES_ROW_START_Y + CREATURES_ROW_GAP, "Seahorse", seahorseFrequency);
  drawCreatureFrequencyRow(s, CREATURES_ROW_START_Y + CREATURES_ROW_GAP * 2, "Snail", snailFrequency);
  drawCreatureFrequencyRow(s, CREATURES_ROW_START_Y + CREATURES_ROW_GAP * 3, "Jellyfish", jellyfishFrequency);

  drawButton(s, CREATURES_CLOSE_X, CREATURES_CLOSE_Y, CREATURES_CLOSE_W, CREATURES_CLOSE_H, "X", TFT_WHITE, TFT_RED);
}

void serviceCaptureAfterDraw(TFT_eSprite& s) {
  if (!captureSequenceEnabled) return;
  saveNextCaptureFrame(s, true, false);
}

void drawSceneLayers(TFT_eSprite& s, bool captureEnabledForSurface, bool drawBackgroundLayer = true,
                     bool drawCaptureUi = true) {
  float tSec = aquariumTimeSec();
  if (drawBackgroundLayer) {
    drawBackground(s, tSec);
  } else {
    clearRenderSurface(s);
  }
  drawAsciiClockBackground(s);
  drawSnail(s);
  drawSeaweed(s, tSec);
  drawBubbles(s);
  drawFlakes(s);
  drawFish(s);
  drawJellyfish(s);
  if (SQUID_ENABLED) drawSquid(s);
  drawOctopus(s);
  drawSeahorse(s);
  drawClock(s);
  drawHud(s);
  drawSettingsPanel(s);
  drawEventsPanel(s);
  drawCreaturesPanel(s);
  drawClockStylePanel(s);
  drawClockColorPanel(s);
  drawAutoSkyPanel(s);
  drawBackgroundColorPanel(s);
  drawBacklightPanel(s);
  drawLightColorModePanel(s);
  drawLightSchedulePanel(s);
  drawAmbientColorPanel(s);
  drawWifiPanel(s);
  if (captureEnabledForSurface) serviceCaptureAfterDraw(s);
  if (drawCaptureUi) {
    drawCapturePanel(s);
    drawCaptureToast(s);
  }
}

void endCaptureSdSession() {
  SD.end();
  captureSdReady = false;
  restoreTftBus();
}

void restoreMainCanvasAfterSlowCapture() {
  canvas.deleteSprite();
  stripRenderActive = false;
  stripRenderY = 0;
  mainCanvasActualColorDepth = 0;
  mainCanvasRenderHeight = SCREEN_H;
  spriteReady = allocateMainCanvas();
  if (spriteReady && gradientBandCache == nullptr) {
    allocateGradientBandCache();
  } else if (!spriteReady) {
    setCaptureStatus("Sprite restore failed", true);
  }
}

bool saveCaptureFrameSlowBmp(bool sequence, bool showToast) {
  setCaptureStatus(sequence ? "Saving seq..." : "Saving BMP...", showToast);

  freeRenderBuffersForCaptureSd(!sequence);

  if (!ensureCaptureSdReady()) {
    endCaptureSdSession();
    restoreMainCanvasAfterSlowCapture();
    return false;
  }

  char path[48];
  unsigned long& index = sequence ? captureNextSequenceIndex : captureNextImageIndex;
  if (!sequence) index = findNextCaptureIndex(false, index);
  capturePath(path, sizeof(path), sequence, index);

  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    setCaptureStatus("File open failed", showToast);
    if (sequence) captureSequenceEnabled = false;
    capturePanelOpen = true;
    endCaptureSdSession();
    restoreMainCanvasAfterSlowCapture();
    return false;
  }

  int captureStripH = clampVal(CAPTURE_RENDER_STRIP_H, 1, SCREEN_H);
  canvas.setColorDepth(16);
  if (canvas.createSprite(SCREEN_W, captureStripH) == nullptr) {
    file.close();
    setCaptureStatus("Capture sprite failed", showToast);
    endCaptureSdSession();
    restoreMainCanvasAfterSlowCapture();
    return false;
  }

  writeBmpHeader(file);

  bool ok = true;
  for (int stripBottom = SCREEN_H; stripBottom > 0 && ok; stripBottom -= captureStripH) {
    int stripY = max(0, stripBottom - captureStripH);
    int stripH = stripBottom - stripY;
    stripRenderActive = true;
    stripRenderY = stripY;
    applyRenderViewport(canvas);
    drawSceneLayers(canvas, false, true, false);
    stripRenderActive = false;
    canvas.resetViewport();

    for (int localY = stripH - 1; localY >= 0; --localY) {
      if (!writeBmpSpriteRow(file, canvas, localY)) {
        ok = false;
        break;
      }
    }
    yield();
  }

  file.close();
  endCaptureSdSession();

  if (ok) {
    index++;
    copySafe(captureLastFile, sizeof(captureLastFile), path);
    char status[48];
    snprintf(status, sizeof(status), "Saved %s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    setCaptureStatus(status, showToast);
  } else {
    setCaptureStatus("Write failed", showToast);
    if (sequence) captureSequenceEnabled = false;
    capturePanelOpen = true;
  }

  restoreMainCanvasAfterSlowCapture();
  return ok;
}

bool saveSingleCaptureSlowBmp(bool showToast) {
  return saveCaptureFrameSlowBmp(false, showToast);
}

void renderFrame() {
  if (!spriteReady) return;

  if (mainCanvasActualColorDepth == MAIN_SPRITE_COLOR_DEPTH && mainCanvasRenderHeight > 0 &&
      mainCanvasRenderHeight < SCREEN_H) {
    for (int y = 0; y < SCREEN_H; y += mainCanvasRenderHeight) {
      int stripH = min(mainCanvasRenderHeight, SCREEN_H - y);
      stripRenderActive = true;
      stripRenderY = y;
      applyRenderViewport(canvas);
      drawSceneLayers(canvas, false);
      stripRenderActive = false;
      canvas.resetViewport();
      canvas.pushSprite(DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y + y, 0, 0, SCREEN_W, stripH);
    }
    if (captureSequenceEnabled) {
      saveCaptureFrameSlowBmp(true, false);
    }
    if (captureSinglePending) {
      captureSinglePending = false;
      saveSingleCaptureSlowBmp(true);
    }
    return;
  }

  applyRenderViewport(canvas);
  drawSceneLayers(canvas, false);
  canvas.pushSprite(DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y);
  if (captureSequenceEnabled) {
    saveCaptureFrameSlowBmp(true, false);
  }
  if (captureSinglePending) {
    captureSinglePending = false;
    saveSingleCaptureSlowBmp(true);
  }
}

// ------------------------------ Input Handling -------------------------------
bool inside(int x, int y, int rx, int ry, int rw, int rh) {
  return (x >= rx && x < rx + rw && y >= ry && y < ry + rh);
}

void handleWifiPasswordTouch(int x, int y) {
  for (int row = 0; row < 4; ++row) {
    const char* keys = keyboardRowText(row);
    int keyCount = strlen(keys);
    for (int i = 0; i < keyCount; ++i) {
      int keyX, keyY, keyW, keyH;
      keyboardKeyBounds(row, i, keyX, keyY, keyW, keyH);
      if (inside(x, y, keyX, keyY, keyW, keyH)) {
        appendCharSafe(wifiPasswordBuffer, sizeof(wifiPasswordBuffer), keys[i]);
        return;
      }
    }
  }

  if (inside(x, y, WIFI_PANEL_X + 18, WIFI_KEYBOARD_SPECIAL_Y, 56, 22)) {
    keyboardMode = (keyboardMode == KEYBOARD_UPPER) ? KEYBOARD_LOWER : KEYBOARD_UPPER;
    return;
  }
  if (inside(x, y, WIFI_PANEL_X + 82, WIFI_KEYBOARD_SPECIAL_Y, 48, 22)) {
    keyboardMode = (keyboardMode == KEYBOARD_SYMBOLS) ? KEYBOARD_LOWER : KEYBOARD_SYMBOLS;
    return;
  }
  if (inside(x, y, WIFI_PANEL_X + 138, WIFI_KEYBOARD_SPECIAL_Y, 86, 22)) {
    appendCharSafe(wifiPasswordBuffer, sizeof(wifiPasswordBuffer), ' ');
    return;
  }
  if (inside(x, y, WIFI_PANEL_X + 232, WIFI_KEYBOARD_SPECIAL_Y, 56, 22)) {
    removeLastChar(wifiPasswordBuffer);
    return;
  }
  if (inside(x, y, WIFI_PANEL_X + 18, WIFI_KEYBOARD_ACTION_Y, 76, 22)) {
    wifiPanelMode = WIFI_PANEL_NETWORKS;
    wifiPasswordBuffer[0] = '\0';
    return;
  }
  if (inside(x, y, WIFI_PANEL_X + 226, WIFI_KEYBOARD_ACTION_Y, 76, 22)) {
    startWifiConnectTo(pendingWifiSsid, wifiPasswordBuffer, true,
                       pendingWifiChannel, pendingWifiHasBssid ? pendingWifiBssid : nullptr);
    wifiPanelMode = WIFI_PANEL_MAIN;
    return;
  }
}

void handleWifiNetworksTouch(int x, int y) {
  int bottomY = WIFI_PANEL_Y + WIFI_PANEL_H - 30;
  if (inside(x, y, WIFI_PANEL_X + 18, bottomY, 58, 22)) {
    wifiPanelMode = WIFI_PANEL_MAIN;
    return;
  }
  if (inside(x, y, WIFI_PANEL_X + 84, bottomY, 58, 22)) {
    if (!wifiEnabled) {
      setWifiEnabled(true);
    } else {
      startWifiScan();
    }
    return;
  }
  if (inside(x, y, WIFI_PANEL_X + 184, bottomY, 54, 22)) {
    if (wifiNetworkPage > 0) wifiNetworkPage--;
    return;
  }
  if (inside(x, y, WIFI_PANEL_X + 246, bottomY, 54, 22)) {
    int maxPage = (wifiNetworkCount <= 0) ? 0 : (wifiNetworkCount - 1) / WIFI_LIST_ROW_COUNT;
    if (wifiNetworkPage < maxPage) wifiNetworkPage++;
    return;
  }

  if (wifiScanInProgress) return;
  if (x < WIFI_PANEL_X + 12 || x >= WIFI_PANEL_X + WIFI_PANEL_W - 12) return;
  if (y < WIFI_LIST_ROW_Y || y >= WIFI_LIST_ROW_Y + WIFI_LIST_ROW_COUNT * WIFI_LIST_ROW_H) return;

  int row = (y - WIFI_LIST_ROW_Y) / WIFI_LIST_ROW_H;
  int index = wifiNetworkPage * WIFI_LIST_ROW_COUNT + row;
  if (index < 0 || index >= wifiNetworkCount) return;

  copySafe(pendingWifiSsid, sizeof(pendingWifiSsid), wifiNetworkNames[index]);
  // Capture the selected AP's BSSID/channel so we connect to that exact access
  // point (and the correct band) after the password prompt.
  memcpy(pendingWifiBssid, wifiNetworkBssid[index], 6);
  pendingWifiChannel = wifiNetworkChannel[index];
  pendingWifiHasBssid = true;
  wifiPasswordBuffer[0] = '\0';
  keyboardMode = KEYBOARD_LOWER;
  if (wifiNetworkOpen[index]) {
    startWifiConnectTo(pendingWifiSsid, "", true, pendingWifiChannel, pendingWifiBssid);
    wifiPanelMode = WIFI_PANEL_MAIN;
  } else {
    wifiPanelMode = WIFI_PANEL_PASSWORD;
  }
}

void handleWifiMainTouch(int x, int y) {
  if (inside(x, y, WIFI_OFF_X, WIFI_ROW_START_Y, WIFI_TOGGLE_W, WIFI_ROW_H)) {
    setWifiEnabled(false);
    return;
  }
  if (inside(x, y, WIFI_ON_X, WIFI_ROW_START_Y, WIFI_TOGGLE_W, WIFI_ROW_H)) {
    setWifiEnabled(true);
    return;
  }

  int networkY = WIFI_ROW_START_Y + WIFI_ROW_GAP;
  if (inside(x, y, WIFI_ACTION_X, networkY, WIFI_ACTION_W, WIFI_ROW_H) ||
      inside(x, y, WIFI_LABEL_X, networkY, WIFI_PANEL_W - 28, WIFI_ROW_H)) {
    if (!wifiEnabled) setWifiEnabled(true);
    if (!wifiScanInProgress && wifiNetworkCount == 0) startWifiScan();
    wifiPanelMode = WIFI_PANEL_NETWORKS;
    return;
  }

  int timezoneY = WIFI_ROW_START_Y + WIFI_ROW_GAP * 4;
  if (inside(x, y, WIFI_OFF_X, timezoneY, WIFI_TOGGLE_W, WIFI_ROW_H)) {
    cycleTimezone(-1);
    return;
  }
  if (inside(x, y, WIFI_ON_X, timezoneY, WIFI_TOGGLE_W, WIFI_ROW_H)) {
    cycleTimezone(1);
    return;
  }
}

void handleWifiPanelTouch(int x, int y) {
  if (inside(x, y, WIFI_CLOSE_X, WIFI_CLOSE_Y, WIFI_CLOSE_W, WIFI_CLOSE_H)) {
    wifiPanelOpen = false;
    wifiPanelMode = WIFI_PANEL_MAIN;
    return;
  }

  if (wifiPanelMode == WIFI_PANEL_PASSWORD) {
    handleWifiPasswordTouch(x, y);
  } else if (wifiPanelMode == WIFI_PANEL_NETWORKS) {
    handleWifiNetworksTouch(x, y);
  } else {
    handleWifiMainTouch(x, y);
  }
}

void handleCapturePanelTouch(int x, int y) {
  if (inside(x, y, CAPTURE_CLOSE_X, CAPTURE_CLOSE_Y, CAPTURE_CLOSE_W, CAPTURE_CLOSE_H)) {
    capturePanelOpen = false;
    return;
  }

  if (inside(x, y, CAPTURE_ACTION_X, CAPTURE_ROW_START_Y, CAPTURE_ACTION_W, CAPTURE_ROW_H)) {
    checkCaptureSdReadyLowMemory();
    return;
  }

  int saveY = CAPTURE_ROW_START_Y + CAPTURE_ROW_GAP;
  if (inside(x, y, CAPTURE_ACTION_X, saveY, CAPTURE_ACTION_W, CAPTURE_ROW_H)) {
    setCaptureSequenceEnabled(true);
    return;
  }
}

void handleClockColorPanelTouch(int x, int y) {
  if (inside(x, y, CLOCK_COLOR_CLOSE_X, CLOCK_COLOR_CLOSE_Y, CLOCK_COLOR_CLOSE_W, CLOCK_COLOR_CLOSE_H)) {
    clockColorPanelOpen = false;
    return;
  }

  const int totalSwatches = BACKGROUND_COLOR_COUNT + 1;
  for (int i = 0; i < totalSwatches; ++i) {
    int col = i % CLOCK_COLOR_SWATCH_COLS;
    int row = i / CLOCK_COLOR_SWATCH_COLS;
    int swatchX = CLOCK_COLOR_GRID_X + col * (CLOCK_COLOR_SWATCH_W + CLOCK_COLOR_SWATCH_GAP_X);
    int swatchY = CLOCK_COLOR_GRID_Y + row * (CLOCK_COLOR_SWATCH_H + CLOCK_COLOR_SWATCH_GAP_Y);
    if (inside(x, y, swatchX - 3, swatchY - 3, CLOCK_COLOR_SWATCH_W + 6, CLOCK_COLOR_SWATCH_H + 6)) {
      if (i == 0) {
        setActiveClockTextRainbowEnabled(true);
      } else {
        setActiveClockTextColor(kBackgroundColorPalette[i - 1]);
      }
      clockColorPanelOpen = false;
      return;
    }
  }
}

void handleBackgroundColorPanelTouch(int x, int y) {
  if (inside(x, y, BACKGROUND_COLOR_CLOSE_X, BACKGROUND_COLOR_CLOSE_Y, BACKGROUND_COLOR_CLOSE_W,
             BACKGROUND_COLOR_CLOSE_H)) {
    backgroundColorPanelOpen = false;
    activeAutoSkyColorSlot = AUTO_SKY_SLOT_NONE;
    return;
  }

  bool editingAutoSky = backgroundColorPickerEditingAutoSky();
  bool includeRainbow = !editingAutoSky;
  const int totalSwatches = BACKGROUND_COLOR_COUNT + (includeRainbow ? 1 : 0);
  for (int i = 0; i < totalSwatches; ++i) {
    int col = i % BACKGROUND_COLOR_SWATCH_COLS;
    int row = i / BACKGROUND_COLOR_SWATCH_COLS;
    int swatchX = BACKGROUND_COLOR_GRID_X + col * (BACKGROUND_COLOR_SWATCH_GRID_W + BACKGROUND_COLOR_SWATCH_GAP_X);
    int swatchY = BACKGROUND_COLOR_GRID_Y + row * (BACKGROUND_COLOR_SWATCH_GRID_H + BACKGROUND_COLOR_SWATCH_GAP_Y);
    if (inside(x, y, swatchX - 3, swatchY - 3, BACKGROUND_COLOR_SWATCH_GRID_W + 6,
               BACKGROUND_COLOR_SWATCH_GRID_H + 6)) {
      if (editingAutoSky) {
        setAutoSkySlotColor(activeAutoSkyColorSlot, kBackgroundColorPalette[i]);
      } else if (i == 0) {
        setBackgroundRainbowEnabled(true);
      } else {
        setBackgroundGradientColor(kBackgroundColorPalette[i - 1]);
      }
      backgroundColorPanelOpen = false;
      activeAutoSkyColorSlot = AUTO_SKY_SLOT_NONE;
      return;
    }
  }
}

void openAutoSkyColourPicker(AutoSkySlot slot) {
  activeAutoSkyColorSlot = slot;
  backgroundColorPanelOpen = true;
}

void handleAutoSkyPanelTouch(int x, int y) {
  if (inside(x, y, AUTO_SKY_CLOSE_X, AUTO_SKY_CLOSE_Y, AUTO_SKY_CLOSE_W, AUTO_SKY_CLOSE_H)) {
    autoSkyPanelOpen = false;
    return;
  }

  if (inside(x, y, AUTO_SKY_BUTTON_LEFT_X, AUTO_SKY_BUTTON_TOP_Y, AUTO_SKY_BUTTON_W, AUTO_SKY_BUTTON_H)) {
    openAutoSkyColourPicker(AUTO_SKY_SUNRISE);
    return;
  }
  if (inside(x, y, AUTO_SKY_BUTTON_RIGHT_X, AUTO_SKY_BUTTON_TOP_Y, AUTO_SKY_BUTTON_W, AUTO_SKY_BUTTON_H)) {
    openAutoSkyColourPicker(AUTO_SKY_DAY);
    return;
  }
  if (inside(x, y, AUTO_SKY_BUTTON_LEFT_X, AUTO_SKY_BUTTON_BOTTOM_Y, AUTO_SKY_BUTTON_W, AUTO_SKY_BUTTON_H)) {
    openAutoSkyColourPicker(AUTO_SKY_SUNSET);
    return;
  }
  if (inside(x, y, AUTO_SKY_BUTTON_RIGHT_X, AUTO_SKY_BUTTON_BOTTOM_Y, AUTO_SKY_BUTTON_W, AUTO_SKY_BUTTON_H)) {
    openAutoSkyColourPicker(AUTO_SKY_NIGHT);
    return;
  }
}

void handleAmbientColorPanelTouch(int x, int y) {
  if (inside(x, y, AMBIENT_COLOR_CLOSE_X, AMBIENT_COLOR_CLOSE_Y, AMBIENT_COLOR_CLOSE_W, AMBIENT_COLOR_CLOSE_H)) {
    ambientColorPanelOpen = false;
    return;
  }

  const int totalSwatches = BACKGROUND_COLOR_COUNT + 1;
  for (int i = 0; i < totalSwatches; ++i) {
    int col = i % AMBIENT_COLOR_SWATCH_COLS;
    int row = i / AMBIENT_COLOR_SWATCH_COLS;
    int swatchX = AMBIENT_COLOR_GRID_X + col * (AMBIENT_COLOR_SWATCH_W + AMBIENT_COLOR_SWATCH_GAP_X);
    int swatchY = AMBIENT_COLOR_GRID_Y + row * (AMBIENT_COLOR_SWATCH_H + AMBIENT_COLOR_SWATCH_GAP_Y);
    if (inside(x, y, swatchX - 3, swatchY - 3, AMBIENT_COLOR_SWATCH_W + 6, AMBIENT_COLOR_SWATCH_H + 6)) {
      if (i == 0) {
        setAmbientLightRainbowEnabled(true);
      } else {
        setAmbientLightColor(kBackgroundColorPalette[i - 1]);
      }
      ambientColorPanelOpen = false;
      return;
    }
  }
}

void handleBacklightPanelTouch(int x, int y) {
  if (inside(x, y, BACKLIGHT_CLOSE_X, BACKLIGHT_CLOSE_Y, BACKLIGHT_CLOSE_W, BACKLIGHT_CLOSE_H)) {
    backlightPanelOpen = false;
    ambientColorPanelOpen = false;
    lightColorModePanelOpen = false;
    lightSchedulePanelOpen = false;
    return;
  }

  if (inside(x, y, BACKLIGHT_MINUS_X, BACKLIGHT_ROW_START_Y, BACKLIGHT_BUTTON_W, BACKLIGHT_BUTTON_H)) {
    adjustLcdBacklightBrightness(-LCD_BACKLIGHT_BRIGHTNESS_STEP);
    return;
  }
  if (inside(x, y, BACKLIGHT_PLUS_X, BACKLIGHT_ROW_START_Y, BACKLIGHT_BUTTON_W, BACKLIGHT_BUTTON_H)) {
    adjustLcdBacklightBrightness(LCD_BACKLIGHT_BRIGHTNESS_STEP);
    return;
  }

  int ambientRowY = BACKLIGHT_ROW_START_Y + BACKLIGHT_ROW_GAP;
  if (inside(x, y, BACKLIGHT_MINUS_X, ambientRowY, BACKLIGHT_BUTTON_W, BACKLIGHT_BUTTON_H)) {
    setAmbientLightEnabled(false);
    return;
  }
  if (inside(x, y, BACKLIGHT_PLUS_X, ambientRowY, BACKLIGHT_BUTTON_W, BACKLIGHT_BUTTON_H)) {
    setAmbientLightEnabled(true);
    return;
  }

  int ambientBrightY = BACKLIGHT_ROW_START_Y + BACKLIGHT_ROW_GAP * 2;
  if (inside(x, y, BACKLIGHT_MINUS_X, ambientBrightY, BACKLIGHT_BUTTON_W, BACKLIGHT_BUTTON_H)) {
    adjustAmbientLightBrightness(-AMBIENT_BRIGHTNESS_STEP);
    return;
  }
  if (inside(x, y, BACKLIGHT_PLUS_X, ambientBrightY, BACKLIGHT_BUTTON_W, BACKLIGHT_BUTTON_H)) {
    adjustAmbientLightBrightness(AMBIENT_BRIGHTNESS_STEP);
    return;
  }

  if (inside(x, y, BACKLIGHT_COLOUR_MODE_X, BACKLIGHT_BOTTOM_BUTTON_Y, BACKLIGHT_BOTTOM_BUTTON_W,
             BACKLIGHT_BUTTON_H)) {
    lightColorModePanelOpen = true;
    lightSchedulePanelOpen = false;
    ambientColorPanelOpen = false;
    return;
  }

  if (inside(x, y, BACKLIGHT_SCHEDULE_X, BACKLIGHT_BOTTOM_BUTTON_Y, BACKLIGHT_BOTTOM_BUTTON_W,
             BACKLIGHT_BUTTON_H)) {
    lightSchedulePanelOpen = true;
    lightColorModePanelOpen = false;
    ambientColorPanelOpen = false;
    return;
  }
}

void handleLightColorModePanelTouch(int x, int y) {
  if (inside(x, y, LIGHT_COLOR_CLOSE_X, LIGHT_COLOR_CLOSE_Y, LIGHT_COLOR_CLOSE_W, LIGHT_COLOR_CLOSE_H)) {
    lightColorModePanelOpen = false;
    ambientColorPanelOpen = false;
    return;
  }

  if (inside(x, y, BACKLIGHT_MINUS_X, LIGHT_COLOR_ROW_START_Y, BACKLIGHT_BUTTON_W, BACKLIGHT_BUTTON_H)) {
    setAmbientLightLinkedToBackground(true);
    ambientColorPanelOpen = false;
    return;
  }
  if (inside(x, y, BACKLIGHT_PLUS_X, LIGHT_COLOR_ROW_START_Y, BACKLIGHT_BUTTON_W, BACKLIGHT_BUTTON_H)) {
    setAmbientLightLinkedToBackground(false);
    return;
  }

  int colorRowY = LIGHT_COLOR_ROW_START_Y + LIGHT_COLOR_ROW_GAP;
  if (!ambientLightLinkedToBackground &&
      (inside(x, y, BACKLIGHT_COLOR_SWATCH_X, colorRowY, BACKLIGHT_COLOR_SWATCH_W, BACKLIGHT_BUTTON_H) ||
       inside(x, y, BACKLIGHT_ACTION_X, colorRowY, BACKLIGHT_ACTION_W, BACKLIGHT_BUTTON_H))) {
    ambientColorPanelOpen = true;
    return;
  }
}

void handleLightSchedulePanelTouch(int x, int y) {
  if (inside(x, y, LIGHT_SCHEDULE_CLOSE_X, LIGHT_SCHEDULE_CLOSE_Y, LIGHT_SCHEDULE_CLOSE_W,
             LIGHT_SCHEDULE_CLOSE_H)) {
    lightSchedulePanelOpen = false;
    keepLightScheduleLcdAwakeIfNeeded(millis());
    applyLightingOutputs();
    return;
  }

  if (inside(x, y, LIGHT_SCHEDULE_MINUS_X, LIGHT_SCHEDULE_ROW_START_Y, LIGHT_SCHEDULE_BUTTON_W,
             LIGHT_SCHEDULE_BUTTON_H)) {
    setLightScheduleEnabled(false);
    return;
  }
  if (inside(x, y, LIGHT_SCHEDULE_PLUS_X, LIGHT_SCHEDULE_ROW_START_Y, LIGHT_SCHEDULE_BUTTON_W,
             LIGHT_SCHEDULE_BUTTON_H)) {
    setLightScheduleEnabled(true);
    return;
  }

  if (!lightScheduleEnabled) return;

  int startY = LIGHT_SCHEDULE_ROW_START_Y + LIGHT_SCHEDULE_ROW_GAP;
  if (inside(x, y, LIGHT_SCHEDULE_MINUS_X, startY, LIGHT_SCHEDULE_BUTTON_W, LIGHT_SCHEDULE_BUTTON_H)) {
    adjustLightScheduleHour(lightScheduleStartHour, -1);
    return;
  }
  if (inside(x, y, LIGHT_SCHEDULE_PLUS_X, startY, LIGHT_SCHEDULE_BUTTON_W, LIGHT_SCHEDULE_BUTTON_H)) {
    adjustLightScheduleHour(lightScheduleStartHour, 1);
    return;
  }

  int endY = LIGHT_SCHEDULE_ROW_START_Y + LIGHT_SCHEDULE_ROW_GAP * 2;
  if (inside(x, y, LIGHT_SCHEDULE_MINUS_X, endY, LIGHT_SCHEDULE_BUTTON_W, LIGHT_SCHEDULE_BUTTON_H)) {
    adjustLightScheduleHour(lightScheduleEndHour, -1);
    return;
  }
  if (inside(x, y, LIGHT_SCHEDULE_PLUS_X, endY, LIGHT_SCHEDULE_BUTTON_W, LIGHT_SCHEDULE_BUTTON_H)) {
    adjustLightScheduleHour(lightScheduleEndHour, 1);
    return;
  }

  int dimY = LIGHT_SCHEDULE_ROW_START_Y + LIGHT_SCHEDULE_ROW_GAP * 3;
  if (inside(x, y, LIGHT_SCHEDULE_MINUS_X, dimY, LIGHT_SCHEDULE_BUTTON_W, LIGHT_SCHEDULE_BUTTON_H)) {
    cycleLightScheduleDimMinutes(-1);
    return;
  }
  if (inside(x, y, LIGHT_SCHEDULE_PLUS_X, dimY, LIGHT_SCHEDULE_BUTTON_W, LIGHT_SCHEDULE_BUTTON_H)) {
    cycleLightScheduleDimMinutes(1);
    return;
  }

  int modeY = LIGHT_SCHEDULE_ROW_START_Y + LIGHT_SCHEDULE_ROW_GAP * 4;
  if (inside(x, y, LIGHT_SCHEDULE_MINUS_X, modeY, LIGHT_SCHEDULE_BUTTON_W, LIGHT_SCHEDULE_BUTTON_H)) {
    cycleLightScheduleDimmingMode(-1);
    return;
  }
  if (inside(x, y, LIGHT_SCHEDULE_PLUS_X, modeY, LIGHT_SCHEDULE_BUTTON_W, LIGHT_SCHEDULE_BUTTON_H)) {
    cycleLightScheduleDimmingMode(1);
    return;
  }
}

void handleClockStylePanelTouch(int x, int y) {
  if (inside(x, y, CLOCK_STYLE_CLOSE_X, CLOCK_STYLE_CLOSE_Y, CLOCK_STYLE_CLOSE_W, CLOCK_STYLE_CLOSE_H)) {
    clockStylePanelOpen = false;
    clockColorPanelOpen = false;
    return;
  }

  if (inside(x, y, CLOCK_STYLE_LEFT_X, CLOCK_STYLE_ROW_1_Y, CLOCK_STYLE_CHOICE_W, SETTINGS_BUTTON_H)) {
    clockDisplayStyle = CLOCK_STYLE_SMALL_TEXT;
    clockColorPanelOpen = false;
    markSettingsDirty();
    return;
  }
  if (inside(x, y, CLOCK_STYLE_RIGHT_X, CLOCK_STYLE_ROW_1_Y, CLOCK_STYLE_CHOICE_W, SETTINGS_BUTTON_H)) {
    clockDisplayStyle = CLOCK_STYLE_ASCII;
    clockColorPanelOpen = false;
    markSettingsDirty();
    return;
  }

  if (inside(x, y, CLOCK_STYLE_SWATCH_X, CLOCK_STYLE_ROW_4_Y, CLOCK_STYLE_SWATCH_W, SETTINGS_BUTTON_H) ||
      inside(x, y, CLOCK_STYLE_COLOR_BUTTON_X, CLOCK_STYLE_ROW_4_Y, CLOCK_STYLE_COLOR_BUTTON_W, SETTINGS_BUTTON_H)) {
    clockColorPanelOpen = true;
    return;
  }

  if (inside(x, y, CLOCK_STYLE_LEFT_X, CLOCK_STYLE_ROW_3_Y, CLOCK_STYLE_CHOICE_W, SETTINGS_BUTTON_H)) {
    clockFlipHorizontal = false;
    markSettingsDirty();
    return;
  }
  if (inside(x, y, CLOCK_STYLE_RIGHT_X, CLOCK_STYLE_ROW_3_Y, CLOCK_STYLE_CHOICE_W, SETTINGS_BUTTON_H)) {
    clockFlipHorizontal = true;
    markSettingsDirty();
    return;
  }

  if (clockDisplayStyle == CLOCK_STYLE_SMALL_TEXT) {
    if (inside(x, y, CLOCK_STYLE_LEFT_X, CLOCK_STYLE_ROW_2_Y, CLOCK_STYLE_CHOICE_W, SETTINGS_BUTTON_H)) {
      clockSmallPosition = CLOCK_SMALL_TOP;
      markSettingsDirty();
      return;
    }
    if (inside(x, y, CLOCK_STYLE_RIGHT_X, CLOCK_STYLE_ROW_2_Y, CLOCK_STYLE_CHOICE_W, SETTINGS_BUTTON_H)) {
      clockSmallPosition = CLOCK_SMALL_BOTTOM;
      markSettingsDirty();
      return;
    }
  } else {
    if (inside(x, y, CLOCK_STYLE_FONT_MINUS_X, CLOCK_STYLE_ROW_2_Y, CLOCK_STYLE_FONT_BUTTON_W, SETTINGS_BUTTON_H)) {
      adjustAsciiClockFont(-1);
      return;
    }
    if (inside(x, y, CLOCK_STYLE_FONT_PLUS_X, CLOCK_STYLE_ROW_2_Y, CLOCK_STYLE_FONT_BUTTON_W, SETTINGS_BUTTON_H)) {
      adjustAsciiClockFont(1);
      return;
    }
  }
}

void serviceBootButton(unsigned long now) {
  if (BOOT_BUTTON_PIN < 0) return;   // no BOOT button on this board (e.g. Pancake)
  bool reading = (digitalRead(BOOT_BUTTON_PIN) == LOW);
  if (reading != bootButtonLastReading) {
    bootButtonLastReading = reading;
    bootButtonLastChangeMs = now;
  }

  if (now - bootButtonLastChangeMs < BOOT_BUTTON_DEBOUNCE_MS) return;
  if (reading == bootButtonStablePressed) return;

  bootButtonStablePressed = reading;
  if (bootButtonStablePressed) {
    requestSingleCapture();
  }
}

void handleEventsPanelTouch(int x, int y) {
  if (inside(x, y, EVENTS_CLOSE_X, EVENTS_CLOSE_Y, EVENTS_CLOSE_W, EVENTS_CLOSE_H)) {
    eventsPanelOpen = false;
    creaturesPanelOpen = false;
    return;
  }

  if (inside(x, y, EVENTS_MINUS_X, EVENTS_ROW_START_Y, EVENTS_BUTTON_W, EVENTS_BUTTON_H)) {
    cycleAutoFeedFrequency(-1);
    return;
  }
  if (inside(x, y, EVENTS_PLUS_X, EVENTS_ROW_START_Y, EVENTS_BUTTON_W, EVENTS_BUTTON_H)) {
    cycleAutoFeedFrequency(1);
    return;
  }

  if (inside(x, y, EVENTS_ACTION_X, EVENTS_ROW_START_Y + EVENTS_ROW_GAP, EVENTS_ACTION_W, EVENTS_BUTTON_H)) {
    creaturesPanelOpen = true;
    return;
  }
}

void handleCreaturesPanelTouch(int x, int y) {
  if (inside(x, y, CREATURES_CLOSE_X, CREATURES_CLOSE_Y, CREATURES_CLOSE_W, CREATURES_CLOSE_H)) {
    creaturesPanelOpen = false;
    return;
  }

  if (inside(x, y, CREATURES_MINUS_X, CREATURES_ROW_START_Y, CREATURES_BUTTON_W, CREATURES_BUTTON_H)) {
    cycleOctopusFrequency(-1);
    return;
  }
  if (inside(x, y, CREATURES_PLUS_X, CREATURES_ROW_START_Y, CREATURES_BUTTON_W, CREATURES_BUTTON_H)) {
    cycleOctopusFrequency(1);
    return;
  }

  if (inside(x, y, CREATURES_MINUS_X, CREATURES_ROW_START_Y + CREATURES_ROW_GAP, CREATURES_BUTTON_W, CREATURES_BUTTON_H)) {
    cycleSeahorseFrequency(-1);
    return;
  }
  if (inside(x, y, CREATURES_PLUS_X, CREATURES_ROW_START_Y + CREATURES_ROW_GAP, CREATURES_BUTTON_W, CREATURES_BUTTON_H)) {
    cycleSeahorseFrequency(1);
    return;
  }

  if (inside(x, y, CREATURES_MINUS_X, CREATURES_ROW_START_Y + CREATURES_ROW_GAP * 2, CREATURES_BUTTON_W, CREATURES_BUTTON_H)) {
    cycleSnailFrequency(-1);
    return;
  }
  if (inside(x, y, CREATURES_PLUS_X, CREATURES_ROW_START_Y + CREATURES_ROW_GAP * 2, CREATURES_BUTTON_W, CREATURES_BUTTON_H)) {
    cycleSnailFrequency(1);
    return;
  }

  if (inside(x, y, CREATURES_MINUS_X, CREATURES_ROW_START_Y + CREATURES_ROW_GAP * 3, CREATURES_BUTTON_W, CREATURES_BUTTON_H)) {
    cycleJellyfishFrequency(-1);
    return;
  }
  if (inside(x, y, CREATURES_PLUS_X, CREATURES_ROW_START_Y + CREATURES_ROW_GAP * 3, CREATURES_BUTTON_W, CREATURES_BUTTON_H)) {
    cycleJellyfishFrequency(1);
    return;
  }
}

#if defined(AQUARIUM_BOARD_PANCAKE)
// Two-finger pinch scales only the clock. Returns true when the gesture consumes
// this frame's touch (so the normal tap/feed logic is skipped). The new size is
// saved to NVS when the pinch ends.
bool handleClockPinch() {
  uint16_t rx1, ry1, rx2, ry2;
  uint8_t count = ft6336_read_points(&rx1, &ry1, &rx2, &ry2);

  if (count >= 2) {
    if (clockVisible) {
      float dx = (float)rx1 - (float)rx2;
      float dy = (float)ry1 - (float)ry2;
      float dist = sqrtf(dx * dx + dy * dy);
      if (!clockPinchActive) {
        clockPinchActive = true;
        clockPinchStartDist = (dist > 1.0f) ? dist : 1.0f;
        clockPinchStartScale = clockZoomScale();
      } else {
        // Target scale tracks the finger spread, then snaps to the nearest notch.
        float target = clockPinchStartScale * (dist / clockPinchStartDist);
        target = clampVal(target, CLOCK_ZOOM_MIN_SCALE, CLOCK_ZOOM_MAX_SCALE);
        float frac = (target - CLOCK_ZOOM_MIN_SCALE) / (CLOCK_ZOOM_MAX_SCALE - CLOCK_ZOOM_MIN_SCALE);
        int notch = (int)lroundf(frac * (CLOCK_ZOOM_NOTCHES - 1));
        clockZoomNotch = (uint8_t)clampVal(notch, 0, (int)CLOCK_ZOOM_NOTCHES - 1);
      }
    }
    clockPinchSuppressTap = true;
    touchWasDown = true;   // never let a pinch register as a tap/feed
    return true;
  }

  // Fewer than two fingers now.
  if (clockPinchActive) {
    clockPinchActive = false;
    markSettingsDirty();   // persist the new zoom (deferred NVS write)
  }
  if (clockPinchSuppressTap) {
    if (count == 0) {      // all fingers lifted — resume normal handling next frame
      clockPinchSuppressTap = false;
      touchWasDown = false;
      return false;
    }
    touchWasDown = true;   // a finger lingers after the pinch — keep swallowing taps
    return true;
  }
  return false;
}
#endif

void processTouch() {
#if defined(AQUARIUM_BOARD_PANCAKE)
  if (handleClockPinch()) return;
#endif
  int x, y;
  if (!readTouchPoint(x, y)) {
    touchWasDown = false;
    return;
  }

  if (touchWasDown) return;
  touchWasDown = true;
  if (millis() - lastTouchMs < TOUCH_DEBOUNCE_MS) return;
  lastTouchMs = millis();
  if (wakeLightScheduleFromTouch(lastTouchMs)) return;

  // Top-left: HUD toggle. Once the HUD is visible, keep this limited to the D
  // button so adjacent debug buttons have their own touch zones.
  bool debugToggleHit = hudVisible
                            ? inside(x, y, DEBUG_BUTTON_X, CORNER_BUTTON_Y, CORNER_BUTTON_W, CORNER_BUTTON_H)
                            : inside(x, y, 0, 0, 42, 26);
  if (debugToggleHit) {
    stopCaptureRecording(true);
    hudVisible = !hudVisible;
    if (!hudVisible) {
      wifiPanelOpen = false;
      capturePanelOpen = false;
      settingsOpen = false;
      backlightPanelOpen = false;
      clockStylePanelOpen = false;
      clockColorPanelOpen = false;
      backgroundColorPanelOpen = false;
      autoSkyPanelOpen = false;
      ambientColorPanelOpen = false;
      lightColorModePanelOpen = false;
      lightSchedulePanelOpen = false;
      eventsPanelOpen = false;
      creaturesPanelOpen = false;
    }
    return;
  }

  if (hudVisible && inside(x, y, BACKLIGHT_BUTTON_X, CORNER_BUTTON_Y, CORNER_BUTTON_W, CORNER_BUTTON_H)) {
    backlightPanelOpen = !backlightPanelOpen;
    if (backlightPanelOpen) {
      settingsOpen = false;
      wifiPanelOpen = false;
      capturePanelOpen = false;
      clockStylePanelOpen = false;
      clockColorPanelOpen = false;
      backgroundColorPanelOpen = false;
      autoSkyPanelOpen = false;
      ambientColorPanelOpen = false;
      lightColorModePanelOpen = false;
      lightSchedulePanelOpen = false;
      eventsPanelOpen = false;
      creaturesPanelOpen = false;
    } else {
      ambientColorPanelOpen = false;
      lightColorModePanelOpen = false;
      lightSchedulePanelOpen = false;
    }
    return;
  }

  // Hidden capture menu appears with the HUD controls.
  if (hudVisible && inside(x, y, CAPTURE_BUTTON_X, 0, CORNER_BUTTON_W, 26)) {
    capturePanelOpen = !capturePanelOpen;
    if (capturePanelOpen) {
      settingsOpen = false;
      wifiPanelOpen = false;
      backlightPanelOpen = false;
      clockStylePanelOpen = false;
      clockColorPanelOpen = false;
      backgroundColorPanelOpen = false;
      autoSkyPanelOpen = false;
      ambientColorPanelOpen = false;
      lightColorModePanelOpen = false;
      lightSchedulePanelOpen = false;
      eventsPanelOpen = false;
      creaturesPanelOpen = false;
    }
    return;
  }

  // Hidden WiFi menu appears with the HUD controls.
  if (hudVisible && inside(x, y, WIFI_BUTTON_X, 0, CORNER_BUTTON_W, 26)) {
    wifiPanelOpen = !wifiPanelOpen;
    wifiPanelMode = WIFI_PANEL_MAIN;
    if (wifiPanelOpen) {
      settingsOpen = false;
      capturePanelOpen = false;
      backlightPanelOpen = false;
      clockStylePanelOpen = false;
      clockColorPanelOpen = false;
      backgroundColorPanelOpen = false;
      autoSkyPanelOpen = false;
      ambientColorPanelOpen = false;
      lightColorModePanelOpen = false;
      lightSchedulePanelOpen = false;
      eventsPanelOpen = false;
      creaturesPanelOpen = false;
      autoSkyPanelOpen = false;
    }
    return;
  }

  // Top-right: settings toggle
  if (hudVisible && inside(x, y, SCREEN_W - 42, 0, 42, 26)) {
    settingsOpen = !settingsOpen;
    if (settingsOpen) {
      wifiPanelOpen = false;
      capturePanelOpen = false;
      backlightPanelOpen = false;
      ambientColorPanelOpen = false;
      lightColorModePanelOpen = false;
      lightSchedulePanelOpen = false;
      eventsPanelOpen = false;
      creaturesPanelOpen = false;
    } else {
      clockStylePanelOpen = false;
      clockColorPanelOpen = false;
      backgroundColorPanelOpen = false;
      autoSkyPanelOpen = false;
      lightColorModePanelOpen = false;
      lightSchedulePanelOpen = false;
      eventsPanelOpen = false;
      creaturesPanelOpen = false;
    }
    return;
  }

  // Hidden respawn button under Settings: randomize active fish positions/types.
  if (hudVisible && inside(x, y, SETTINGS_CORNER_BUTTON_X, RESPAWN_BUTTON_Y, CORNER_BUTTON_W, CORNER_BUTTON_H)) {
    respawnButtonFlashUntilMs = millis() + HUD_BUTTON_FLASH_MS;
    respawnFishPopulation();
    return;
  }

  // Hidden creature test spawns for final tuning.
  if (hudVisible && inside(x, y, SEAHORSE_TEST_BUTTON_X, SEAHORSE_TEST_BUTTON_Y,
                           CORNER_BUTTON_W, CORNER_BUTTON_H)) {
    seahorseButtonFlashUntilMs = millis() + HUD_BUTTON_FLASH_MS;
    spawnSeahorseAtCenter(aquariumNowMs);
    return;
  }
  if (hudVisible && inside(x, y, OCTOPUS_TEST_BUTTON_X, OCTOPUS_TEST_BUTTON_Y,
                           CORNER_BUTTON_W, CORNER_BUTTON_H)) {
    octopusButtonFlashUntilMs = millis() + HUD_BUTTON_FLASH_MS;
    spawnOctopusAtCenter(aquariumNowMs);
    return;
  }
  if (hudVisible && inside(x, y, SNAIL_TEST_BUTTON_X, SNAIL_TEST_BUTTON_Y,
                           CORNER_BUTTON_W, CORNER_BUTTON_H)) {
    snailButtonFlashUntilMs = millis() + HUD_BUTTON_FLASH_MS;
    spawnSnailAtCenter(aquariumNowMs);
    return;
  }
  if (hudVisible && inside(x, y, CTHULHU_TEST_BUTTON_X, CTHULHU_TEST_BUTTON_Y,
                           CORNER_BUTTON_W, CORNER_BUTTON_H)) {
    cthulhuButtonFlashUntilMs = millis() + HUD_BUTTON_FLASH_MS;
    spawnCthulhuAtCenter(aquariumNowMs);
    return;
  }
  if (hudVisible && inside(x, y, JELLYFISH_TEST_BUTTON_X, JELLYFISH_TEST_BUTTON_Y,
                           CORNER_BUTTON_W, CORNER_BUTTON_H)) {
    jellyfishButtonFlashUntilMs = millis() + HUD_BUTTON_FLASH_MS;
    spawnDebugJellyfish(aquariumNowMs);
    return;
  }
  if (SQUID_ENABLED && hudVisible && inside(x, y, SQUID_TEST_BUTTON_X, SQUID_TEST_BUTTON_Y,
                           CORNER_BUTTON_W, CORNER_BUTTON_H)) {
    squidButtonFlashUntilMs = millis() + HUD_BUTTON_FLASH_MS;
    spawnSquidAtCenter(aquariumNowMs);
    return;
  }

  if (capturePanelOpen) {
    handleCapturePanelTouch(x, y);
    return;
  }

  if (wifiPanelOpen) {
    handleWifiPanelTouch(x, y);
    return;
  }

  if (clockColorPanelOpen) {
    handleClockColorPanelTouch(x, y);
    return;
  }

  if (backgroundColorPanelOpen) {
    handleBackgroundColorPanelTouch(x, y);
    return;
  }

  if (autoSkyPanelOpen) {
    handleAutoSkyPanelTouch(x, y);
    return;
  }

  if (ambientColorPanelOpen) {
    handleAmbientColorPanelTouch(x, y);
    return;
  }

  if (lightColorModePanelOpen) {
    handleLightColorModePanelTouch(x, y);
    return;
  }

  if (lightSchedulePanelOpen) {
    handleLightSchedulePanelTouch(x, y);
    return;
  }

  if (clockStylePanelOpen) {
    handleClockStylePanelTouch(x, y);
    return;
  }

  if (backlightPanelOpen) {
    handleBacklightPanelTouch(x, y);
    return;
  }

  if (creaturesPanelOpen) {
    handleCreaturesPanelTouch(x, y);
    return;
  }

  if (eventsPanelOpen) {
    handleEventsPanelTouch(x, y);
    return;
  }

  if (settingsOpen) {
    if (inside(x, y, SETTINGS_CLOSE_X, SETTINGS_CLOSE_Y, SETTINGS_CLOSE_W, SETTINGS_CLOSE_H)) {
      settingsOpen = false;
      clockStylePanelOpen = false;
      clockColorPanelOpen = false;
      backgroundColorPanelOpen = false;
      autoSkyPanelOpen = false;
      eventsPanelOpen = false;
      creaturesPanelOpen = false;
      return;
    }

    if (inside(x, y, SETTINGS_TANK_TAB_X, SETTINGS_TAB_Y, SETTINGS_TANK_TAB_W, SETTINGS_TAB_H)) {
      activeSettingsTab = SETTINGS_TAB_TANK;
      clockStylePanelOpen = false;
      clockColorPanelOpen = false;
      backgroundColorPanelOpen = false;
      autoSkyPanelOpen = false;
      eventsPanelOpen = false;
      creaturesPanelOpen = false;
      return;
    }
    if (inside(x, y, SETTINGS_SEAWEED_TAB_X, SETTINGS_TAB_Y, SETTINGS_SEAWEED_TAB_W, SETTINGS_TAB_H)) {
      activeSettingsTab = SETTINGS_TAB_SEAWEED;
      clockStylePanelOpen = false;
      clockColorPanelOpen = false;
      backgroundColorPanelOpen = false;
      autoSkyPanelOpen = false;
      eventsPanelOpen = false;
      creaturesPanelOpen = false;
      return;
    }
    if (inside(x, y, SETTINGS_CLOCK_TAB_X, SETTINGS_TAB_Y, SETTINGS_CLOCK_TAB_W, SETTINGS_TAB_H)) {
      activeSettingsTab = SETTINGS_TAB_CLOCK;
      backgroundColorPanelOpen = false;
      autoSkyPanelOpen = false;
      eventsPanelOpen = false;
      creaturesPanelOpen = false;
      return;
    }
    if (inside(x, y, SETTINGS_BACKGROUND_TAB_X, SETTINGS_TAB_Y, SETTINGS_BACKGROUND_TAB_W, SETTINGS_TAB_H)) {
      activeSettingsTab = SETTINGS_TAB_BACKGROUND;
      clockStylePanelOpen = false;
      clockColorPanelOpen = false;
      backgroundColorPanelOpen = false;
      autoSkyPanelOpen = false;
      eventsPanelOpen = false;
      creaturesPanelOpen = false;
      return;
    }

    if (activeSettingsTab == SETTINGS_TAB_TANK) {
      // Fish pop -/+
      if (inside(x, y, SETTINGS_MINUS_X, SETTINGS_ROW_START_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        fishTargetCount -= 1;
        applyFishPopulation();
        markSettingsDirty();
        return;
      }
      if (inside(x, y, SETTINGS_PLUS_X, SETTINGS_ROW_START_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        fishTargetCount += 1;
        applyFishPopulation();
        markSettingsDirty();
        return;
      }

      // Bubble amount -/+
      if (inside(x, y, SETTINGS_MINUS_X, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        bubbleTargetCount -= 1;
        applyBubblePopulation();
        markSettingsDirty();
        return;
      }
      if (inside(x, y, SETTINGS_PLUS_X, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        bubbleTargetCount += 1;
        applyBubblePopulation();
        markSettingsDirty();
        return;
      }

      if (inside(x, y, SETTINGS_ACTION_X, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP * 2, SETTINGS_ACTION_W,
                 SETTINGS_BUTTON_H)) {
        eventsPanelOpen = true;
        return;
      }
      if (inside(x, y, SETTINGS_MINUS_X, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP * 3, SETTINGS_BUTTON_W,
                 SETTINGS_BUTTON_H)) {
        setDisplayFlip180(false);
        return;
      }
      if (inside(x, y, SETTINGS_PLUS_X, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP * 3, SETTINGS_BUTTON_W,
                 SETTINGS_BUTTON_H)) {
        setDisplayFlip180(true);
        return;
      }
    } else if (activeSettingsTab == SETTINGS_TAB_SEAWEED) {
      // Seaweed sway -/+
      if (inside(x, y, SETTINGS_MINUS_X, SETTINGS_ROW_START_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        seaweedSwaySpeed = clampVal(seaweedSwaySpeed - 0.05f, MIN_SWAY, MAX_SWAY);
        markSettingsDirty();
        return;
      }
      if (inside(x, y, SETTINGS_PLUS_X, SETTINGS_ROW_START_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        seaweedSwaySpeed = clampVal(seaweedSwaySpeed + 0.05f, MIN_SWAY, MAX_SWAY);
        markSettingsDirty();
        return;
      }

      // Seaweed length -/+
      if (inside(x, y, SETTINGS_MINUS_X, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        seaweedLength = clampVal(seaweedLength - 0.05f, MIN_SEAWEED_LENGTH, MAX_SEAWEED_LENGTH);
        markSettingsDirty();
        return;
      }
      if (inside(x, y, SETTINGS_PLUS_X, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        seaweedLength = clampVal(seaweedLength + 0.05f, MIN_SEAWEED_LENGTH, MAX_SEAWEED_LENGTH);
        markSettingsDirty();
        return;
      }

      // Seaweed length randomness -/+
      if (inside(x, y, SETTINGS_MINUS_X, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP * 2, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        seaweedLengthRandomness = clampVal(seaweedLengthRandomness - 0.05f, MIN_SEAWEED_LENGTH_RANDOMNESS, MAX_SEAWEED_LENGTH_RANDOMNESS);
        markSettingsDirty();
        return;
      }
      if (inside(x, y, SETTINGS_PLUS_X, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP * 2, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        seaweedLengthRandomness = clampVal(seaweedLengthRandomness + 0.05f, MIN_SEAWEED_LENGTH_RANDOMNESS, MAX_SEAWEED_LENGTH_RANDOMNESS);
        markSettingsDirty();
        return;
      }
    } else if (activeSettingsTab == SETTINGS_TAB_CLOCK) {
      if (inside(x, y, CLOCK_STYLE_BUTTON_X, CLOCK_ROW_1_Y, CLOCK_STYLE_BUTTON_W, SETTINGS_BUTTON_H)) {
        clockStylePanelOpen = true;
        clockColorPanelOpen = false;
        backgroundColorPanelOpen = false;
        autoSkyPanelOpen = false;
        return;
      }

      if (!clockUseInternetTime && inside(x, y, CLOCK_FIELD_PREV_X, CLOCK_FIELD_NAV_Y, CLOCK_FIELD_NAV_W, CLOCK_FIELD_NAV_H)) {
        selectClockField(-1);
        return;
      }
      if (!clockUseInternetTime && inside(x, y, CLOCK_FIELD_NEXT_X, CLOCK_FIELD_NAV_Y, CLOCK_FIELD_NAV_W, CLOCK_FIELD_NAV_H)) {
        selectClockField(1);
        return;
      }

      // Clock visibility buttons
      if (inside(x, y, SETTINGS_MINUS_X, CLOCK_ROW_1_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        clockVisible = false;
        resetClockTick();
        markSettingsDirty();
        return;
      }
      if (inside(x, y, SETTINGS_PLUS_X, CLOCK_ROW_1_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        clockVisible = true;
        resetClockTick();
        markSettingsDirty();
        return;
      }

      // Time source buttons
      if (inside(x, y, SETTINGS_MINUS_X, CLOCK_ROW_2_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        clockUseInternetTime = false;
        resetClockTick();
        markSettingsDirty();
        return;
      }
      if (inside(x, y, SETTINGS_PLUS_X, CLOCK_ROW_2_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        clockUseInternetTime = true;
        if (!wifiEnabled) {
          setWifiEnabled(true);
        } else if (wifiConnected) {
          beginInternetTimeSync();
        } else if (!wifiConnecting && wifiSsid[0] != '\0') {
          startWifiConnect(wifiSsid, wifiPass, false);
        } else if (!wifiScanInProgress) {
          startWifiScan();
        }
        markSettingsDirty();
        return;
      }

      // Format buttons
      if (inside(x, y, SETTINGS_MINUS_X, CLOCK_ROW_3_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        clockUse24Hour = false;
        markSettingsDirty();
        return;
      }
      if (inside(x, y, SETTINGS_PLUS_X, CLOCK_ROW_3_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        clockUse24Hour = true;
        markSettingsDirty();
        return;
      }

      // Timezone -/+
      if (inside(x, y, SETTINGS_MINUS_X, CLOCK_ROW_5_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        cycleTimezone(-1);
        return;
      }
      if (inside(x, y, SETTINGS_PLUS_X, CLOCK_ROW_5_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        cycleTimezone(1);
        return;
      }

      // Selected clock field -/+
      if (!clockUseInternetTime && inside(x, y, SETTINGS_MINUS_X, CLOCK_ROW_4_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        adjustClockField(-1);
        return;
      }
      if (!clockUseInternetTime && inside(x, y, SETTINGS_PLUS_X, CLOCK_ROW_4_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        adjustClockField(1);
        return;
      }
    } else {
      // Background style -/+
      if (inside(x, y, SETTINGS_MINUS_X, SETTINGS_ROW_START_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        cycleBackgroundStyle(-1);
        return;
      }
      if (inside(x, y, SETTINGS_PLUS_X, SETTINGS_ROW_START_Y, SETTINGS_BUTTON_W, SETTINGS_BUTTON_H)) {
        cycleBackgroundStyle(1);
        return;
      }
      if (backgroundUsesGradientColor() && !autoSkyEnabled &&
          (inside(x, y, BACKGROUND_COLOR_SWATCH_X, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP, BACKGROUND_COLOR_SWATCH_W,
                  SETTINGS_BUTTON_H) ||
           inside(x, y, BACKGROUND_COLOR_BUTTON_X, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP, BACKGROUND_COLOR_BUTTON_W,
                  SETTINGS_BUTTON_H))) {
        activeAutoSkyColorSlot = AUTO_SKY_SLOT_NONE;
        backgroundColorPanelOpen = true;
        return;
      }
      if (backgroundStyle == BACKGROUND_STYLE_FLOWERS && !autoSkyEnabled &&
          inside(x, y, SETTINGS_ACTION_X, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP, SETTINGS_ACTION_W, SETTINGS_BUTTON_H)) {
        randomizeFlowers();
        return;
      }
      if (autoSkyEnabled && inside(x, y, AUTO_SKY_PICK_X, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP * 2,
                                   AUTO_SKY_PICK_W, SETTINGS_BUTTON_H)) {
        autoSkyPanelOpen = true;
        return;
      }
      if (inside(x, y, SETTINGS_MINUS_X, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP * 2, SETTINGS_BUTTON_W,
                 SETTINGS_BUTTON_H)) {
        setAutoSkyEnabled(false);
        autoSkyPanelOpen = false;
        activeAutoSkyColorSlot = AUTO_SKY_SLOT_NONE;
        return;
      }
      if (inside(x, y, SETTINGS_PLUS_X, SETTINGS_ROW_START_Y + SETTINGS_ROW_GAP * 2, SETTINGS_BUTTON_W,
                 SETTINGS_BUTTON_H)) {
        setAutoSkyEnabled(true);
        return;
      }
    }
    return;
  }

  // Feed interaction: tap anywhere else to spawn falling flake
  spawnFlake((float)x, (float)y);
}

// ------------------------------ Setup / Loop ---------------------------------
void setup() {
  Serial.begin(115200);
  randomSeed((uint32_t)esp_random());

  // Allocate the biggest RAM block before NVS/WiFi/TFT setup can fragment heap.
  spriteReady = allocateMainCanvas();

  loadPersistentState();
  if (wifiEnabled) {
    setWifiStatus(wifiSsid[0] ? "Starting..." : "Ready to scan");
  }

  pinMode(TFT_BACKLIGHT_PIN, OUTPUT);
  digitalWrite(TFT_BACKLIGHT_PIN, HIGH);
  tft.init();
  applyDisplayOrientation(false);
  tft.fillScreen(BG_COLOR);
  tft.setTextWrap(false);
  tft.setTextFont(2);
  tft.setTextColor(TFT_GREEN, BG_COLOR);
  tft.setCursor(10, 10);
  tft.println("ASCII Aquarium booting...");
  initLightingHardware();

  if (BOOT_BUTTON_PIN >= 0) {
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    bootButtonLastReading = (digitalRead(BOOT_BUTTON_PIN) == LOW);
    bootButtonStablePressed = bootButtonLastReading;
  }
  bootButtonLastChangeMs = millis();
  pinMode(SD_CS_PIN, OUTPUT);
  digitalWrite(SD_CS_PIN, HIGH);

  if (spriteReady) {
    canvas.setTextFont(2);
    allocateGradientBandCache();
    clockFlipSprite.setColorDepth(16);
    clockFlipSpriteReady = (clockFlipSprite.createSprite(CLOCK_FLIP_SPRITE_W, CLOCK_FLIP_SPRITE_H) != nullptr);
    if (clockFlipSpriteReady) clockFlipSprite.setTextFont(2);
    tft.setCursor(10, 28);
    tft.setTextColor(TFT_CYAN, BG_COLOR);
    tft.println("Sprite buffer: OK");
    tft.setCursor(10, 46);
    tft.printf("Render: %dx%d\n", SCREEN_W, SCREEN_H);
    tft.setCursor(10, 64);
    tft.printf("Canvas: %db %dx%d\n", mainCanvasActualColorDepth, SCREEN_W, mainCanvasRenderHeight);
    tft.setCursor(10, 82);
    tft.printf("Heap: %lu/%luK\n", (unsigned long)(ESP.getFreeHeap() / 1024UL),
               (unsigned long)(ESP.getHeapSize() / 1024UL));
    tft.setCursor(10, 100);
    if (ESP.getPsramSize() > 0) {
      tft.printf("PSRAM: %lu/%luK\n", (unsigned long)(ESP.getFreePsram() / 1024UL),
                 (unsigned long)(ESP.getPsramSize() / 1024UL));
    } else {
      tft.println("PSRAM: none");
    }
  } else {
    tft.setCursor(10, 28);
    tft.setTextColor(TFT_RED, BG_COLOR);
    tft.println("Sprite alloc failed");
    tft.setCursor(10, 46);
    tft.println("Not enough sprite RAM");
  }

  // CYD uses a dedicated touch bus; ST7796U shares the TFT bus and must not remux pins here.
  touchReady = initTouchHardware();
  if (touchReady) {
    applyTouchOrientation();
    tft.setCursor(10, 118);
    tft.setTextColor(TFT_GREENYELLOW, BG_COLOR);
    tft.println("Touch: OK");
  } else {
    tft.setCursor(10, 118);
    tft.setTextColor(TFT_ORANGE, BG_COLOR);
    tft.println("Touch not detected");
  }

  for (int i = 0; i < MAX_FLAKES; i++) flakes[i].active = false;
  for (int i = 0; i < MAX_BUBBLES; i++) bubbles[i].active = false;
  for (int i = 0; i < MAX_FISH_POOL; i++) fishPool[i].active = false;
  octopus.active = false;
  octopus.cthulhu = false;
  octopus.nextSpawnMs = 0;
  seahorse.active = false;
  seahorse.nextSpawnMs = 0;
  snail.active = false;
  snail.nextSpawnMs = 0;
  jellyfish.active = false;
  jellyfish.nextSpawnMs = 0;
  squid.active = false;
  initFishMirrors();
  initFishGlyphMetrics();
  applyFishPopulation();
  spreadInitialFishLayout();
  applyBubblePopulation(true);

  delay(350);
  tft.fillScreen(BG_COLOR);
  lastMs = millis();
  aquariumNowMs = lastMs;
  clockLastMinuteMs = lastMs;
  lastSettingsSaveMs = lastMs;
  fpsTimer = lastMs;
  frameCount = 0;

#if defined(AQUARIUM_BOARD_PANCAKE)
  // Optional DS3231 on the shared touch I2C bus (SDA=9/SCL=10, addr 0x68).
  // The chip stores UTC (matching M5PORKCHOP/Launcher on this same
  // hardware — see ds3231_rtc.h), so convert to local using the persisted
  // timezone before seeding the on-screen clock fields. If present and its
  // battery held time through the outage, this beats the NVS-saved manual
  // time (whatever was set before the last power-off) and covers boots with
  // no Wi-Fi. Net mode (NTP), if enabled, still overrides this once it
  // connects, and writes the corrected UTC back to the RTC (see
  // syncClockFromSystemTime).
  applyConfiguredTimezone();
  rtcPresent = ds3231_init();
  if (rtcPresent && !ds3231_lost_power()) {
    time_t utcNow;
    if (ds3231_read_utc(&utcNow)) {
      struct tm localTm;
      localtime_r(&utcNow, &localTm);
      clockYear = localTm.tm_year + 1900;
      clockMonth = localTm.tm_mon + 1;
      clockDay = localTm.tm_mday;
      clockHour = localTm.tm_hour;
      clockMinute = localTm.tm_min;
      normalizeClockDate();
      unsigned long secondOffsetMs = (unsigned long)clampVal(localTm.tm_sec, 0, 59) * 1000UL;
      clockLastMinuteMs = (lastMs > secondOffsetMs) ? (lastMs - secondOffsetMs) : lastMs;
      markSettingsDirty();
      Serial.printf("[RTC] Loaded %04d-%02d-%02d %02d:%02d:%02d local (UTC epoch %lu)\n",
                    clockYear, clockMonth, clockDay, clockHour, clockMinute, localTm.tm_sec,
                    (unsigned long)utcNow);
    }
  } else if (rtcPresent) {
    Serial.println("[RTC] Lost power (dead/missing battery) — keeping saved time until manual or Net sync");
  }
#endif
}

void loop() {
  unsigned long now = millis();
  unsigned long elapsedMs = now - lastMs;
  lastMs = now;

  // Keep services on real millis(), but decouple visible motion from SD write stalls.
  unsigned long aquariumStepMs = captureSequenceEnabled ? CAPTURE_RECORD_FRAME_MS : clampVal(elapsedMs, 1UL, 50UL);
  aquariumNowMs += aquariumStepMs;
  float dt = aquariumStepMs * 0.001f;

  updateClock(now);
  serviceBootButton(now);
  processTouch();
  serviceLightSchedule(now);
  serviceAutoSky();
  serviceBackgroundRainbow(aquariumNowMs);
  serviceWifi(now);
  serviceSettingsPersistence(now);
  serviceAutoFeed(aquariumNowMs);
  updateFlakes(dt);
  updateBubbles(dt);
  updateFish(dt);
  updateOctopus(aquariumNowMs, dt);
  updateSeahorse(aquariumNowMs, dt);
  updateSnail(aquariumNowMs, dt);
  updateJellyfish(aquariumNowMs, dt);
  if (SQUID_ENABLED) updateSquid(aquariumNowMs, dt);
  if (fishAvoidanceEnabled()) keepVisitorsSeparated();
  renderFrame();

  frameCount++;
  if (now - fpsTimer >= 500) {
    fps = (frameCount * 1000.0f) / (now - fpsTimer + 1);
    frameCount = 0;
    fpsTimer = now;
  }
}
