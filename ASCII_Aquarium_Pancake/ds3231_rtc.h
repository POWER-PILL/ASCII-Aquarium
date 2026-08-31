// ds3231_rtc.h — self-contained DS3231 RTC driver for the Marauder Pancake
// (ESP32-C5). Shares the FT6336 touch I2C bus (SDA=9, SCL=10) — no extra
// pins needed. Address 0x68 does not collide with the FT6336 (0x38).
//
// Wiring (Compact DS3231 w/ battery):
//   +  VCC  -> 3V3 (not 5V)
//   D  SDA  -> GPIO 9
//   C  SCL  -> GPIO 10
//   NC      -> leave empty
//   -  GND  -> GND
//
// Storage convention: the chip holds UTC — the same convention used by the
// M5PORKCHOP firmware's rtc_ds3231.cpp on this same hardware. The Pancake
// Launcher does not read or write this chip at all (checked: no RTC driver
// on the pancake board target), so it's a non-issue for it either way. The
// Pancake's Wi-Fi/manual "timezone" setting only affects how UTC is
// displayed, never how it's stored, so the module reads the same on
// whichever firmware boots next. Callers convert to/from local time
// themselves (localtime_r / mktime) around ds3231_read_utc/ds3231_write_utc.
#pragma once
#ifndef ds3231_rtc_h
#define ds3231_rtc_h

#include <Arduino.h>
#include <Wire.h>
#include <time.h>

#ifndef RTC_SDA
#define RTC_SDA 9
#endif
#ifndef RTC_SCL
#define RTC_SCL 10
#endif

#define DS3231_ADDR        0x68
#define DS3231_REG_SECONDS 0x00
#define DS3231_REG_STATUS  0x0F
#define DS3231_STATUS_OSF  0x80   // Oscillator Stop Flag — set after power/battery loss

static uint8_t _ds3231_bcd2dec(uint8_t v) { return (uint8_t)(((v >> 4) * 10) + (v & 0x0F)); }
static uint8_t _ds3231_dec2bcd(uint8_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

// Days since 1970-01-01 for a civil UTC Y/M/D (Howard Hinnant's algorithm) —
// same routine M5PORKCHOP's rtc_ds3231.cpp uses, so both drivers agree on
// the exact same epoch math for whatever they read/write to this chip.
static long _ds3231_days_from_civil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  long era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153u * (m + (m > 2 ? -3u : 9u)) + 2u) / 5u + d - 1u;
  unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097L + (long)doe - 719468L;
}

static bool _ds3231_read(uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom((int)DS3231_ADDR, (int)len);
  for (uint8_t i = 0; i < len; i++)
    buf[i] = Wire.available() ? Wire.read() : 0;
  return true;
}

static bool _ds3231_write(uint8_t reg, const uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(DS3231_ADDR);
  Wire.write(reg);
  for (uint8_t i = 0; i < len; i++) Wire.write(buf[i]);
  return Wire.endTransmission() == 0;
}

// Returns true if a DS3231 responded on the I2C bus. Safe to call even if
// Wire.begin() was already run (e.g. by the FT6336 touch init) — same bus,
// same pins/speed.
static bool ds3231_init() {
  Wire.begin(RTC_SDA, RTC_SCL, 400000U);
  Wire.beginTransmission(DS3231_ADDR);
  bool present = (Wire.endTransmission() == 0);
  Serial.printf("[RTC] DS3231 %s\n", present ? "found" : "not detected");
  return present;
}

// True when the oscillator has stopped since the flag was last cleared —
// dead/missing coin cell, or first-ever power-up. Time read back while
// this is set should not be trusted.
static bool ds3231_lost_power() {
  uint8_t status = 0;
  if (!_ds3231_read(DS3231_REG_STATUS, &status, 1)) return true;
  return (status & DS3231_STATUS_OSF) != 0;
}

static void ds3231_clear_lost_power_flag() {
  uint8_t status = 0;
  if (!_ds3231_read(DS3231_REG_STATUS, &status, 1)) return;
  status &= (uint8_t)~DS3231_STATUS_OSF;
  _ds3231_write(DS3231_REG_STATUS, &status, 1);
}

// Reads the chip's stored UTC time as an epoch. Module is always run in
// 24-hour mode. Returns false on I2C failure or an out-of-range reading
// (unset/corrupt chip).
static bool ds3231_read_utc(time_t *utcOut) {
  uint8_t d[7];
  if (!_ds3231_read(DS3231_REG_SECONDS, d, 7)) return false;

  uint8_t sec   = _ds3231_bcd2dec(d[0] & 0x7F);
  uint8_t min   = _ds3231_bcd2dec(d[1] & 0x7F);
  uint8_t hour;
  if (d[2] & 0x40) {                                // 12-hour mode (set by some other tool)
    hour = _ds3231_bcd2dec(d[2] & 0x1F) % 12;
    if (d[2] & 0x20) hour += 12;                    // PM bit
  } else {                                          // 24-hour mode (how we write it)
    hour = _ds3231_bcd2dec(d[2] & 0x3F);
  }
  uint8_t day   = _ds3231_bcd2dec(d[4] & 0x3F);
  uint8_t month = _ds3231_bcd2dec(d[5] & 0x1F);      // ignore century bit
  int     year  = 2000 + _ds3231_bcd2dec(d[6]);

  if (month < 1 || month > 12 || day < 1 || day > 31 ||
      hour > 23 || min > 59 || sec > 59 || year < 2024) {
    return false;                                    // unset / corrupt reading
  }

  long days = _ds3231_days_from_civil(year, (unsigned)month, (unsigned)day);
  *utcOut = (time_t)days * 86400L + hour * 3600L + min * 60L + sec;
  return true;
}

// Writes a UTC epoch into the chip and clears the lost-power flag so future
// reads are trusted again.
static bool ds3231_write_utc(time_t utc) {
  struct tm tmv;
  gmtime_r(&utc, &tmv);

  uint8_t d[7];
  d[0] = _ds3231_dec2bcd((uint8_t)tmv.tm_sec);
  d[1] = _ds3231_dec2bcd((uint8_t)tmv.tm_min);
  d[2] = _ds3231_dec2bcd((uint8_t)tmv.tm_hour);          // 24h mode, bit6=0
  d[3] = (uint8_t)(tmv.tm_wday + 1);                     // DS3231 day-of-week 1..7
  d[4] = _ds3231_dec2bcd((uint8_t)tmv.tm_mday);
  d[5] = _ds3231_dec2bcd((uint8_t)(tmv.tm_mon + 1));     // century bit7=0 (20xx)
  d[6] = _ds3231_dec2bcd((uint8_t)((tmv.tm_year + 1900) - 2000));
  if (!_ds3231_write(DS3231_REG_SECONDS, d, 7)) return false;
  ds3231_clear_lost_power_flag();
  return true;
}

#endif // ds3231_rtc_h
