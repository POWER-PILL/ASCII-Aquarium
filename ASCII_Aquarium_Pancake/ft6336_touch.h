// ft6336_touch.h — self-contained FT6336U capacitive touch driver for the
// Marauder Pancake (ESP32-C5). Ported from the Marauder Bible firmware.
//
// Wiring (shared I2C bus): SDA=9, SCL=10, RST=8, INT unused. Panel is native
// portrait 320x480, so raw coordinates are x:0..319, y:0..479. The sketch
// rotates these into the 480x320 landscape scene in readCapTouchPoint().
#pragma once
#ifndef ft6336_touch_h
#define ft6336_touch_h

#include <Arduino.h>
#include <Wire.h>

// ── Pancake FT6336 pin map (adjust here only if your board differs) ──────────
#ifndef CTP_SDA
#define CTP_SDA 9
#endif
#ifndef CTP_SCL
#define CTP_SCL 10
#endif
#ifndef CTP_RST
#define CTP_RST 8
#endif

#define FT6336_ADDR      0x38
#define FT6336_TD_STATUS 0x02
#define FT6336_T1_XH     0x03

static bool _ft6336_read(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom((int)FT6336_ADDR, (int)len);
  for (uint8_t i = 0; i < len; i++)
    buf[i] = Wire.available() ? Wire.read() : 0;
  return true;
}

// Returns true if the controller responded on the I2C bus.
static bool ft6336_init() {
  pinMode(CTP_RST, OUTPUT);
  digitalWrite(CTP_RST, LOW);
  delay(10);
  digitalWrite(CTP_RST, HIGH);
  delay(300);
  Wire.begin(CTP_SDA, CTP_SCL, 400000U);

  uint8_t chipId = 0;
  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(0xA3);                 // CHIPER register
  bool present = (Wire.endTransmission(false) == 0);
  if (present) {
    Wire.requestFrom((int)FT6336_ADDR, 1);
    if (Wire.available()) chipId = Wire.read();
  }
  Serial.printf("[Touch] FT6336U ID: 0x%02X%s\n",
                chipId, chipId == 0x64 ? " (OK)" : " (unexpected)");

  // Raise the touch threshold a little to reduce phantom touches through a case.
  // Register 0x80 = IDTHRESHOLD (default 22); higher = less sensitive.
  Wire.beginTransmission(FT6336_ADDR);
  Wire.write(0x80);
  Wire.write(40);
  Wire.endTransmission();

  return present;
}

// Reads one raw touch point in panel-native portrait space (x:0..319, y:0..479).
// Returns 1 when a finger is present, 0 otherwise.
static uint8_t ft6336_read_raw(uint16_t *raw_x, uint16_t *raw_y) {
  uint8_t data[7];
  if (!_ft6336_read(FT6336_TD_STATUS, data, 7)) return 0;
  if ((data[0] & 0x0F) == 0) return 0;
  *raw_x = ((uint16_t)(data[1] & 0x0F) << 8) | data[2];
  *raw_y = ((uint16_t)(data[3] & 0x0F) << 8) | data[4];
  return 1;
}

// Reads up to two raw touch points (panel-native portrait). Returns the number
// of active points (0, 1 or 2). Point 2 is only written when the return is >= 2.
// The FT6336 lays the points out contiguously from TD_STATUS:
//   [0]=count [1..4]=P1 XH,XL,YH,YL [5..6]=weight/misc [7..10]=P2 XH,XL,YH,YL
static uint8_t ft6336_read_points(uint16_t *x1, uint16_t *y1,
                                  uint16_t *x2, uint16_t *y2) {
  uint8_t data[13];
  if (!_ft6336_read(FT6336_TD_STATUS, data, 13)) return 0;
  uint8_t count = data[0] & 0x0F;
  if (count == 0) return 0;
  *x1 = ((uint16_t)(data[1] & 0x0F) << 8) | data[2];
  *y1 = ((uint16_t)(data[3] & 0x0F) << 8) | data[4];
  if (count >= 2) {
    *x2 = ((uint16_t)(data[7] & 0x0F) << 8) | data[8];
    *y2 = ((uint16_t)(data[9] & 0x0F) << 8) | data[10];
  }
  return count;
}

#endif // ft6336_touch_h
