#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace esphome {
namespace vitohome {

// Pure, framework-free decode/encode helpers, kept separate so the logic can
// be unit-tested on the host without VitoWiFi or ESPHome headers
// (tests/native/test_decode.cpp).
//
// Why this file exists (Stage 2): VitoWiFi's converters decode through a
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
inline bool decode_masked_bit(const uint8_t *data, std::size_t data_len, uint8_t byte_offset, uint8_t bit_mask,
                              bool *out) {
  if (data == nullptr || data_len <= byte_offset) return false;
  *out = (data[byte_offset] & bit_mask) != 0;
  return true;
}

// ---------------------------------------------------------------------------
// numeric decode/encode (Stage 2)
// ---------------------------------------------------------------------------

// Optolink payloads are little-endian (verified against VitoWiFi
// Converter.cpp at the pinned SHA: data[1] << 8 | data[0], etc.).
inline uint64_t read_le(const uint8_t *data, uint8_t len) {
  uint64_t v = 0;
  for (uint8_t i = 0; i < len && i < 8; i++) {
    v |= static_cast<uint64_t>(data[i]) << (8 * i);
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
inline bool decode_scaled(const uint8_t *data, std::size_t data_len, uint8_t len, bool is_signed, double scale,
                          double *out) {
  if (data == nullptr || len == 0 || len > 8 || data_len < len) return false;
  const uint64_t raw = read_le(data, len);
  const double v = is_signed ? static_cast<double>(sign_extend_le(raw, len)) : static_cast<double>(raw);
  *out = v * scale;
  return true;
}

// Encode `value` into `len` little-endian bytes after dividing out `scale`
// (the inverse of decode_scaled), rounding to the nearest raw step. Returns
// false if the raw value does not fit the (signed or unsigned) range of
// `len` bytes, or on a non-finite input — the caller must treat that as a
// hard error and not transmit. len in [1,4] (Optolink writes).
inline bool encode_scaled(double value, double scale, bool is_signed, uint8_t len, uint8_t *buf) {
  if (buf == nullptr || len == 0 || len > 4 || scale == 0.0 || !std::isfinite(value)) return false;
  const double raw_d = value / scale;
  if (!std::isfinite(raw_d)) return false;
  const int64_t raw = static_cast<int64_t>(std::llround(raw_d));
  if (is_signed) {
    const int64_t lo = -(1LL << (8 * len - 1));
    const int64_t hi = (1LL << (8 * len - 1)) - 1;
    if (raw < lo || raw > hi) return false;
  } else {
    if (raw < 0 || static_cast<uint64_t>(raw) > ((len == 4) ? 0xFFFFFFFFULL : ((1ULL << (8 * len)) - 1))) return false;
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
inline bool bcd_to_int(uint8_t b, uint8_t *out) {
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
//   [2]=month BCD  [3]=day BCD  [4]=weekday (0=Monday, IGNORED)
//   [5]=hour BCD  [6]=minute BCD  [7]=second BCD
// Layout source: InsideViessmannVitosoft, Viessmann2MQTT.py
// DateTimeFromBCD() — i.e. the reverse-engineering repo's own decoder, NOT
// the [code,day,month,...] guess in the old vitoconnect config (see
// docs/stage2_design.md for that correction).
// Returns false on any non-BCD byte or an out-of-range field (empty
// error-history slots are 0xFF-filled and fail the BCD check).
inline bool decode_datetime_bcd(const uint8_t *data, std::size_t data_len, std::size_t offset, BcdDateTime *out) {
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
inline int decode_ascii(const uint8_t *data, std::size_t data_len, uint8_t len, char *out, std::size_t out_cap) {
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

}  // namespace vitohome
}  // namespace esphome
