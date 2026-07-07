#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace esphome::vitohome {

// Pure, framework-free decode/encode helpers, kept separate so the logic can
// be unit-tested on the host without the optolink engine or ESPHome headers
// (tests/native/test_decode.cpp).
//
// Why this file exists (Stage 2): the optolink engine's converters decode through a
// non-discriminated union and do their math in float32, which silently loses
// precision for 4-byte counters (uint32 -> float drops bits above 2^24; a
// burner-seconds counter of ~212,000,000 is already past that). vitohome
// therefore decodes the raw payload itself: integer extraction in uint64/
// int64, scaling in double, and only the *final* value is narrowed to the
// float32 that ESPHome's sensor state requires. After scaling, the values are
// small (hours, degrees, percent), so the final narrowing is harmless.

// ---------------------------------------------------------------------------
// bit/byte extraction (Stage 1, unchanged)
// ---------------------------------------------------------------------------

// Returns false (and leaves *out untouched) if byte_offset is out of range.
inline bool decode_masked_bit(const uint8_t* data, std::size_t data_len, uint8_t byte_offset, uint8_t bit_mask,
                              bool* out) {
  if (data == nullptr || data_len <= byte_offset) return false;
  *out = (data[byte_offset] & bit_mask) != 0;
  return true;
}

// ---------------------------------------------------------------------------
// numeric decode/encode (Stage 2)
// ---------------------------------------------------------------------------

// Optolink payloads are little-endian (verified against the optolink engine
// Converter.cpp at the pinned SHA: data[1] << 8 | data[0], etc.).
inline uint64_t read_le(const uint8_t* data, uint8_t len) {
  uint64_t v = 0;
  for (uint8_t i = 0; i < len && i < 8; i++) {
    v |= static_cast<uint64_t>(data[i]) << (8 * i);
  }
  return v;
}

// Big-endian assembly for Vitosoft's "RotateBytes" conversion (the same bytes
// read most-significant-first): data[0] << 8*(len-1) | ... | data[len-1].
// Used by e.g. GWG_Codierstecker_Kennziffer (0x1040) and VSKO_Scot_NEC_* on
// VScotHO1_72. Sign extension afterwards is byte-order-agnostic (it operates on
// the assembled len-byte integer), so sign_extend_le() is reused as-is.
inline uint64_t read_be(const uint8_t* data, uint8_t len) {
  uint64_t v = 0;
  for (uint8_t i = 0; i < len && i < 8; i++) {
    v = (v << 8) | static_cast<uint64_t>(data[i]);
  }
  return v;
}

// Two's-complement sign extension of a little-endian raw value of `len`
// bytes. len in [1,8]; len >= 8 returns the value unchanged.
inline int64_t sign_extend_le(uint64_t raw, uint8_t len) {
  if (len == 0 || len >= 8) return static_cast<int64_t>(raw);
  const uint64_t sign_bit = 1ULL << (8 * len - 1);
  if (raw & sign_bit) {
    return static_cast<int64_t>(raw | ~((sign_bit << 1) - 1));
  }
  return static_cast<int64_t>(raw);
}

// Decode `len` bytes starting at data[0] as a (signed or unsigned) integer
// and scale it in double precision. Returns false if the payload is shorter
// than `len` or len is outside [1,8].
inline bool decode_scaled(const uint8_t* data, std::size_t data_len, uint8_t len, bool is_signed, double scale,
                          double* out) {
  if (data == nullptr || len == 0 || len > 8 || data_len < len) return false;
  const uint64_t raw = read_le(data, len);
  const double v = is_signed ? static_cast<double>(sign_extend_le(raw, len)) : static_cast<double>(raw);
  *out = v * scale;
  return true;
}

// Big-endian counterpart of decode_scaled for Vitosoft "RotateBytes" datapoints.
// Identical contract; only the byte assembly differs (read_be).
inline bool decode_scaled_be(const uint8_t* data, std::size_t data_len, uint8_t len, bool is_signed, double scale,
                             double* out) {
  if (data == nullptr || len == 0 || len > 8 || data_len < len) return false;
  const uint64_t raw = read_be(data, len);
  const double v = is_signed ? static_cast<double>(sign_extend_le(raw, len)) : static_cast<double>(raw);
  *out = v * scale;
  return true;
}

// Encode `value` into `len` little-endian bytes after dividing out `scale`
// (the inverse of decode_scaled), rounding to the nearest raw step. Returns
// false if the raw value does not fit the (signed or unsigned) range of
// `len` bytes, or on a non-finite input — the caller must treat that as a
// hard error and not transmit. len in [1,4] (Optolink writes).
inline bool encode_scaled(double value, double scale, bool is_signed, uint8_t len, uint8_t* buf) {
  if (buf == nullptr || len == 0 || len > 4 || scale == 0.0 || !std::isfinite(value)) return false;
  const double raw_d = value / scale;
  if (!std::isfinite(raw_d)) return false;
  const int64_t raw = static_cast<int64_t>(std::llround(raw_d));
  if (is_signed) {
    const int64_t lo = -(1LL << (8 * len - 1));
    const int64_t hi = (1LL << (8 * len - 1)) - 1;
    if (raw < lo || raw > hi) return false;
  } else {
    // 1ULL is >= 64 bits on any conforming compiler, so the shift is
    // well-defined for len == 4 too ((1ULL << 32) - 1 == 0xFFFFFFFF); no
    // special case needed.
    if (raw < 0 || static_cast<uint64_t>(raw) > ((1ULL << (8 * len)) - 1)) return false;
  }
  const uint64_t u = static_cast<uint64_t>(raw);  // two's complement bit pattern
  for (uint8_t i = 0; i < len; i++) {
    buf[i] = static_cast<uint8_t>((u >> (8 * i)) & 0xFF);
  }
  return true;
}

// ---------------------------------------------------------------------------
// BCD / DateTimeBCD (Stage 2 — error history, full date)
// ---------------------------------------------------------------------------

// Decode one packed-BCD byte (0x25 -> 25). Returns false on a non-BCD nibble
// (e.g. 0xFF, the fill value of empty error-history slots).
inline bool bcd_to_int(uint8_t b, uint8_t* out) {
  const uint8_t hi = (b >> 4) & 0x0F;
  const uint8_t lo = b & 0x0F;
  if (hi > 9 || lo > 9) return false;
  *out = static_cast<uint8_t>(hi * 10 + lo);
  return true;
}

struct BcdDateTime {
  uint16_t year;
  uint8_t month, day, hour, minute, second;
};

// Viessmann DateTimeBCD, 8 bytes starting at data[offset]:
//   [0]=year-hi BCD (e.g. 0x20)  [1]=year-lo BCD (e.g. 0x26)
//   [2]=month BCD  [3]=day BCD  [4]=weekday (sunday=0..saturday=6, IGNORED)
//   [5]=hour BCD  [6]=minute BCD  [7]=second BCD
// Layout source: InsideViessmannVitosoft, Viessmann2MQTT.py
// DateTimeFromBCD() — i.e. the reverse-engineering repo's own decoder, NOT
// the [code,day,month,...] guess in the old vitoconnect config (see
// docs/design_notes.md SS7 for that correction).
// Returns false on any non-BCD byte or an out-of-range field (empty
// error-history slots are 0xFF-filled and fail the BCD check).
inline bool decode_datetime_bcd(const uint8_t* data, std::size_t data_len, std::size_t offset, BcdDateTime* out) {
  if (data == nullptr || out == nullptr || data_len < offset + 8) return false;
  uint8_t yh, yl, mo, da, ho, mi, se;
  if (!bcd_to_int(data[offset + 0], &yh) || !bcd_to_int(data[offset + 1], &yl) || !bcd_to_int(data[offset + 2], &mo) ||
      !bcd_to_int(data[offset + 3], &da) || !bcd_to_int(data[offset + 5], &ho) || !bcd_to_int(data[offset + 6], &mi) ||
      !bcd_to_int(data[offset + 7], &se)) {
    return false;
  }
  if (mo < 1 || mo > 12 || da < 1 || da > 31 || ho > 23 || mi > 59 || se > 59) return false;
  out->year = static_cast<uint16_t>(yh * 100 + yl);
  out->month = mo;
  out->day = da;
  out->hour = ho;
  out->minute = mi;
  out->second = se;
  // Plausibility: heating controllers did not exist before ~1990 and a
  // BCD year caps at 9999; an all-zero slot decodes as year 0 -> reject.
  return out->year >= 1990;
}

// ---------------------------------------------------------------------------
// BCD encode (system-time set)
// ---------------------------------------------------------------------------

// Inverse of bcd_to_int: pack a decimal 0..99 into one packed-BCD byte
// (25 -> 0x25). A single BCD byte cannot hold values above 99, so the caller
// reduces wider fields upstream (the year is split into a century byte and a
// year-of-century byte). Returns false on an out-of-range input.
inline bool int_to_bcd(uint8_t dec, uint8_t* out) {
  if (out == nullptr || dec > 99) return false;
  *out = static_cast<uint8_t>(((dec / 10) << 4) | (dec % 10));
  return true;
}

// Days since 1970-01-01 for a civil (proleptic Gregorian) date -- Howard
// Hinnant's days_from_civil. Used only to compare two wall-clock times (the
// device clock vs the HA time source) for the drift check: both sides are
// computed identically, so the absolute epoch and timezone are irrelevant.
inline int64_t civil_days(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// Seconds-since-epoch for a decoded device datetime, treated as a plain
// wall-clock value (no timezone math) -- for the drift comparison only.
inline int64_t civil_seconds(const BcdDateTime& dt) {
  return civil_days(dt.year, dt.month, dt.day) * 86400 + static_cast<int64_t>(dt.hour) * 3600 +
         static_cast<int64_t>(dt.minute) * 60 + dt.second;
}

// ESPHome ESPTime::day_of_week is sunday=1..saturday=7; the Vitotronic stores
// the weekday byte as sunday=0..saturday=6 -- the strftime %w convention that
// vcontrold writes. Hardware-confirmed on 0x20CB: a read of 0x088E on a
// Wednesday returns weekday byte 0x03. Map ESPTime -> device.
inline uint8_t device_weekday_from_esptime(uint8_t dow_sun1) { return static_cast<uint8_t>((dow_sun1 + 6) % 7); }

// Encode a datetime into the 8-byte Viessmann DateTimeBCD wire layout (the
// inverse of decode_datetime_bcd):
//   [0]=century BCD  [1]=year-of-century BCD  [2]=month  [3]=day
//   [4]=weekday (sunday=0..saturday=6)  [5]=hour  [6]=minute  [7]=second
// The device validates the weekday against the date, so it must match the
// sunday=0 convention (see device_weekday_from_esptime). Builds into a local
// and only commits on success, so a bad field leaves buf8 unchanged. Returns
// false on any out-of-range field.
inline bool encode_datetime_bcd(uint16_t year, uint8_t month, uint8_t day, uint8_t weekday, uint8_t hour,
                                uint8_t minute, uint8_t second, uint8_t* buf8) {
  if (buf8 == nullptr) return false;
  if (year > 9999 || month < 1 || month > 12 || day < 1 || day > 31 || weekday > 6 || hour > 23 || minute > 59 ||
      second > 59) {
    return false;
  }
  uint8_t tmp[8];
  if (!int_to_bcd(static_cast<uint8_t>(year / 100), &tmp[0]) ||
      !int_to_bcd(static_cast<uint8_t>(year % 100), &tmp[1]) || !int_to_bcd(month, &tmp[2]) ||
      !int_to_bcd(day, &tmp[3]) || !int_to_bcd(weekday, &tmp[4]) || !int_to_bcd(hour, &tmp[5]) ||
      !int_to_bcd(minute, &tmp[6]) || !int_to_bcd(second, &tmp[7])) {
    return false;
  }
  std::memcpy(buf8, tmp, 8);
  return true;
}

// ---------------------------------------------------------------------------
// Schaltzeiten / cycle-time (PhaseType) -- per-day 8-byte switching program
// ---------------------------------------------------------------------------
//
// A Viessmann switching-time program is addressed per weekday as an 8-byte
// block: four ON/OFF switch-point pairs, each pair two bytes (even = ON,
// odd = OFF). Each byte encodes a time of day in 10-minute resolution:
//   hour   = byte >> 3            (5 bits, 0..31)
//   minute = (byte & 0x07) * 10   (3 bits -> 0,10,20,30,40,50)
// An ON byte with hour >= 24 (0xC0..0xFF; the device fills 0xFF) marks the
// pair disabled. The three-bit minute field is the reason the device only
// stores 10-minute steps -- there is physically no room for a finer value, so
// the encode path truncates to the next-lower 10 minutes (06:07 -> 06:00).
//
// Format cross-checked against two independent references: vcontrold
// getCycleTime/setCycleTime (src/unit.c) and InsideViessmannVitosoft
// Viessmann2MQTT.py PhaseDay. vcontrold addresses each weekday separately
// (xml/300/vito.xml: getTimerM1Mo 0x2000 len 8 ... So 0x2030), the per-day
// model vitohome follows so a read covers exactly the bytes a write sets
// (read-back alignment).
//
// Canonical string (round-trippable -- decode and encode share it):
//   "06:00-22:00 08:30-12:00"  -- up to four space-separated ON-OFF pairs.
// A disabled pair is "--"; trailing disabled pairs are omitted; an interior
// disabled pair keeps its slot as "--" so positions are preserved. An
// all-disabled day is the empty string.

// True if a switch-point byte encodes a real time (hour 0..23), false if it
// is the disabled sentinel (hour >= 24, which includes the 0xFF fill).
inline bool timebyte_active(uint8_t b) { return static_cast<uint8_t>(b >> 3) < 24; }

// Decode one switch-point byte to hour/minute. Returns false for a disabled
// byte (hour >= 24); *h/*m are left untouched in that case.
inline bool timebyte_to_hhmm(uint8_t b, uint8_t* h, uint8_t* m) {
  if (h == nullptr || m == nullptr) return false;
  const uint8_t hour = static_cast<uint8_t>(b >> 3);
  if (hour >= 24) return false;
  *h = hour;
  *m = static_cast<uint8_t>((b & 0x07) * 10);
  return true;
}

// Encode hour/minute to one switch-point byte, truncating the minute to the
// next-lower 10-minute step. Returns false if the hour is not 0..23 (24+
// collides with the disabled sentinel) or the minute is not 0..59, so a bad
// value is rejected rather than silently disabling the slot.
inline bool hhmm_to_timebyte(uint8_t h, uint8_t m, uint8_t* b) {
  if (b == nullptr || h > 23 || m > 59) return false;
  *b = static_cast<uint8_t>((h << 3) | (m / 10));  // m/10 in 0..5 fits 3 bits
  return true;
}

// Decode an 8-byte per-day program into the canonical string. Returns the
// character count written (excluding the NUL), or -1 on bad args / a payload
// shorter than 8 bytes. out_cap should be >= 48 (worst case
// "HH:MM-HH:MM" x4 + 3 spaces = 47, + NUL).
inline int decode_schaltzeiten_day(const uint8_t* data, std::size_t data_len, char* out, std::size_t out_cap) {
  if (out != nullptr && out_cap > 0) out[0] = '\0';
  if (data == nullptr || out == nullptr || out_cap == 0 || data_len < 8) return -1;
  // Trailing disabled pairs are trimmed, so find the last active ON byte.
  int last_active = -1;
  for (int p = 0; p < 4; p++) {
    if (timebyte_active(data[2 * p])) last_active = p;
  }
  std::size_t off = 0;
  for (int p = 0; p <= last_active; p++) {
    if (off != 0 && off < out_cap - 1) out[off++] = ' ';
    const uint8_t on = data[2 * p];
    int w;
    if (!timebyte_active(on)) {
      w = std::snprintf(out + off, out_cap - off, "--");
    } else {
      uint8_t oh = 0, om = 0;
      timebyte_to_hhmm(on, &oh, &om);
      // A malformed OFF (disabled while ON is active) still renders via the
      // same formula so the raw state is visible rather than hidden.
      const uint8_t offb = data[2 * p + 1];
      const uint8_t fh = static_cast<uint8_t>(offb >> 3);
      const uint8_t fm = static_cast<uint8_t>((offb & 0x07) * 10);
      w = std::snprintf(out + off, out_cap - off, "%02u:%02u-%02u:%02u", oh, om, fh, fm);
    }
    if (w < 0) break;
    off += static_cast<std::size_t>(w);
    if (off >= out_cap) {
      off = out_cap - 1;  // truncated; snprintf left it NUL-terminated
      break;
    }
  }
  out[off] = '\0';
  return static_cast<int>(off);
}

// Parse "HH:MM" (1-2 digit hour, 1-2 digit minute) in [*pp, end) into a
// switch-point byte (truncating the minute). Advances *pp past the field.
// Returns false on a malformed or out-of-range field.
inline bool parse_hhmm_(const char** pp, const char* end, uint8_t* b) {
  const char* p = *pp;
  int field[2] = {0, 0};
  for (int f = 0; f < 2; f++) {
    if (p >= end || *p < '0' || *p > '9') return false;
    int v = 0, n = 0;
    while (p < end && *p >= '0' && *p <= '9' && n < 2) {
      v = v * 10 + (*p - '0');
      p++;
      n++;
    }
    field[f] = v;
    if (f == 0) {
      if (p >= end || *p != ':') return false;
      p++;
    }
  }
  if (field[0] > 23 || field[1] > 59) return false;
  if (!hhmm_to_timebyte(static_cast<uint8_t>(field[0]), static_cast<uint8_t>(field[1]), b)) return false;
  *pp = p;
  return true;
}

// Encode the canonical string into an 8-byte per-day program (caller provides
// buf8). Empty / blank input clears the day (all 0xFF). Returns false on any
// malformed token or more than four pairs; on failure buf8 is left unchanged
// (the parse builds into a local and only commits on success, so a rejected
// string never leaves a torn write) and the caller must not transmit -- same
// contract as encode_scaled.
inline bool encode_schaltzeiten_day(const char* str, uint8_t* buf8) {
  if (buf8 == nullptr) return false;
  uint8_t tmp[8];
  for (int i = 0; i < 8; i++) tmp[i] = 0xFF;
  if (str != nullptr) {
    const char* p = str;
    const char* const end = str + std::strlen(str);
    int pair = 0;
    while (p < end) {
      while (p < end && *p == ' ') p++;  // skip spaces between tokens
      if (p >= end) break;
      if (pair >= 4) return false;  // too many pairs
      const char* tok_end = p;
      while (tok_end < end && *tok_end != ' ') tok_end++;
      bool has_colon = false;
      for (const char* q = p; q < tok_end; q++) {
        if (*q == ':') {
          has_colon = true;
          break;
        }
      }
      const std::size_t tok_len = static_cast<std::size_t>(tok_end - p);
      if (!has_colon) {  // disabled pair: "-" or "--", leaves 0xFF/0xFF
        if (!(tok_len == 1 && p[0] == '-') && !(tok_len == 2 && p[0] == '-' && p[1] == '-')) return false;
      } else {  // "HH:MM-HH:MM"
        const char* cur = p;
        uint8_t onb = 0, offb = 0;
        if (!parse_hhmm_(&cur, tok_end, &onb)) return false;
        if (cur >= tok_end || *cur != '-') return false;
        cur++;
        if (!parse_hhmm_(&cur, tok_end, &offb)) return false;
        if (cur != tok_end) return false;  // trailing garbage in the token
        tmp[2 * pair] = onb;
        tmp[2 * pair + 1] = offb;
      }
      p = tok_end;
      pair++;
    }
  }
  std::memcpy(buf8, tmp, 8);
  return true;
}

// ---------------------------------------------------------------------------
// ASCII byte string (HexByte2AsciiByte) -- device part / serial numbers
// ---------------------------------------------------------------------------

// Decode `len` bytes starting at data[0] as an ASCII string. The Vitosoft
// "HexByte2AsciiByte" conversion is a byte-array-as-string: each raw byte IS
// an ASCII character code (the Sachnummer/Herstellnummer datapoints carry
// SDKDataType=ByteArray / Parameter=String, and the numeric ConversionFactor/
// Offset on those rows are vestigial and ignored). A NUL (0x00) terminates the
// string; trailing spaces are trimmed; any non-printable byte (outside
// 0x20..0x7E) becomes '?' so a bad read yields a safe string, never control
// characters. Writes the result plus a NUL into `out` (out_cap must be >=
// len+1). Returns the character count written (excluding the terminator), or
// -1 on bad arguments / short payload.
inline int decode_ascii(const uint8_t* data, std::size_t data_len, uint8_t len, char* out, std::size_t out_cap) {
  if (data == nullptr || out == nullptr || out_cap == 0) return -1;
  if (len == 0 || data_len < len || out_cap < static_cast<std::size_t>(len) + 1) return -1;
  std::size_t n = 0;
  for (uint8_t i = 0; i < len; i++) {
    const uint8_t c = data[i];
    if (c == 0x00) break;  // NUL terminates the string
    out[n++] = (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?';
  }
  while (n > 0 && out[n - 1] == ' ') n--;  // trim trailing space padding
  out[n] = '\0';
  return static_cast<int>(n);
}

// Decode `len` bytes of UTF-16LE (Vitosoft "HexByte2UTF16Byte") to UTF-8.
// Used by the editable heating-circuit labels Beschriftung_HK1..3 (0x7360..,
// 40 bytes = 20 code units). `len` must be even. Each code unit is data[2i] |
// data[2i+1]<<8; 0x0000 terminates and 0xFFFF (empty-slot fill) is skipped.
// BMP only -- a surrogate (0xD800..0xDFFF) is emitted as '?'. Trailing spaces
// are trimmed. out_cap must allow up to 3 UTF-8 bytes per unit + NUL. Returns
// the byte count written (excluding the NUL), or -1 on bad args.
inline int decode_utf16(const uint8_t* data, std::size_t data_len, uint8_t len, char* out, std::size_t out_cap) {
  if (data == nullptr || out == nullptr || out_cap == 0) return -1;
  if (len == 0 || (len & 1) != 0 || data_len < len) return -1;
  std::size_t n = 0;
  for (uint8_t i = 0; i + 1 < len; i += 2) {
    const uint16_t cu = static_cast<uint16_t>(data[i] | (data[i + 1] << 8));
    if (cu == 0x0000) break;     // NUL terminates
    if (cu == 0xFFFF) continue;  // empty-slot fill
    uint32_t cp = cu;
    if (cu >= 0xD800 && cu <= 0xDFFF) cp = '?';  // lone surrogate -> placeholder
    if (cp < 0x80) {
      if (n + 1 >= out_cap) break;
      out[n++] = static_cast<char>(cp);
    } else if (cp < 0x800) {
      if (n + 2 >= out_cap) break;
      out[n++] = static_cast<char>(0xC0 | (cp >> 6));
      out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      if (n + 3 >= out_cap) break;
      out[n++] = static_cast<char>(0xE0 | (cp >> 12));
      out[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  while (n > 0 && out[n - 1] == ' ') n--;  // trim trailing space padding
  out[n] = '\0';
  return static_cast<int>(n);
}

// ---------------------------------------------------------------------------
// scan-console raw response formatting (debug)
// ---------------------------------------------------------------------------

// Format a raw Optolink response for the live scan console: the request
// address, the payload as space-separated hex, and -- for widths 1..8 -- the
// little-endian unsigned and signed integer interpretations computed in
// 64-bit (so a 4-byte value is exact, unlike the float32 a sensor publishes).
// Example: "0x0800: AA BB  u=48042 i=-17494".
//
// Pure / framework-free so it is host-tested in tests/native/test_decode.cpp.
// Never writes past out[cap-1]; always NUL-terminates when cap > 0. Returns
// the number of characters written (excluding the NUL), or 0 on bad args.
inline int format_raw_dump(uint16_t address, const uint8_t* data, uint8_t len, char* out, std::size_t cap) {
  if (out == nullptr || cap == 0) return 0;
  int off = std::snprintf(out, cap, "0x%04X:", static_cast<unsigned>(address));
  if (off < 0) {
    out[0] = '\0';
    return 0;
  }
  // Cap the hex run so a long/garbled response can't blow the line; a P300
  // read is a few bytes (error_history is 9), so 32 is generous.
  const uint8_t max_hex = (len < 32) ? len : 32;
  for (uint8_t i = 0; i < max_hex && static_cast<std::size_t>(off) < cap; i++) {
    const int w = std::snprintf(out + off, cap - static_cast<std::size_t>(off), " %02X",
                                static_cast<unsigned>(data == nullptr ? 0 : data[i]));
    if (w < 0) return 0;
    off += w;
  }
  if (len > max_hex && static_cast<std::size_t>(off) < cap) {
    const int w =
        std::snprintf(out + off, cap - static_cast<std::size_t>(off), " ...(%u bytes)", static_cast<unsigned>(len));
    if (w < 0) return 0;
    off += w;
  }
  if (data != nullptr && len >= 1 && len <= 8 && static_cast<std::size_t>(off) < cap) {
    const uint64_t u = read_le(data, len);
    const int64_t s = sign_extend_le(u, len);
    const int w = std::snprintf(out + off, cap - static_cast<std::size_t>(off), "  u=%llu i=%lld",
                                static_cast<unsigned long long>(u), static_cast<long long>(s));
    if (w < 0) return 0;
    off += w;
  }
  if (static_cast<std::size_t>(off) >= cap) off = static_cast<int>(cap) - 1;  // truncated; snprintf NUL-terminated
  return off;
}

}  // namespace esphome::vitohome
