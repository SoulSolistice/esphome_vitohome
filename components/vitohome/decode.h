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
// Why this file exists: the optolink engine's converters decode through a
// non-discriminated union and do their math in float32, which silently loses
// precision for 4-byte counters (uint32 -> float drops bits above 2^24; a
// burner-seconds counter of ~212,000,000 is already past that). vitohome
// therefore decodes the raw payload itself: integer extraction in uint64/
// int64, scaling in double, and only the *final* value is narrowed to the
// float32 that ESPHome's sensor state requires. After scaling, the values are
// small (hours, degrees, percent), so the final narrowing is harmless.

// ---------------------------------------------------------------------------
// bit/byte extraction
// ---------------------------------------------------------------------------

// Returns false (and leaves *out untouched) if byte_offset is out of range.
inline bool decode_masked_bit(const uint8_t *data, std::size_t data_len, uint8_t byte_offset, uint8_t bit_mask,
                              bool *out) {
  if (data == nullptr || data_len <= byte_offset)
    return false;
  *out = (data[byte_offset] & bit_mask) != 0;
  return true;
}

// ---------------------------------------------------------------------------
// bit fields (`bit_mask` on sensor / number / select / switch / enum)
// ---------------------------------------------------------------------------
//
// A bit field is a run of adjacent bits inside one byte, named by its MASK:
// 0xF0 is the high nibble, 0x08 a single flag. A mask carries no bit-numbering
// convention, which is the point -- Vitosoft numbers BitPosition MSB-first,
// vcontrold numbers <bit> LSB-first (docs/design_notes.md SS8e), and the
// translation happens once, in scripts/gen_catalog.py.
//
// The FIELD value is the masked bits shifted down: field = (byte & mask) >>
// shift, where shift is the mask's lowest set bit. Option values, on/off
// values and the converter scale all apply to the field, never to the byte.

// True for a non-zero mask whose set bits are adjacent (0x0C, 0xF0, 0xFF; not
// 0x0A, not 0). The Python schema rejects anything else at config time; this
// is the runtime twin of that check.
inline bool bit_mask_contiguous(uint8_t mask) {
  if (mask == 0)
    return false;
  // Adding the lowest set bit carries through a contiguous run and clears it:
  // 0x0C + 0x04 = 0x10, and 0x10 & 0x0C == 0. A gap leaves a set bit behind
  // (0x0A + 0x02 = 0x0C, 0x0C & 0x0A == 0x08). Done in unsigned so 0xF0+0x10
  // = 0x100 is not truncated before the AND.
  const unsigned m = mask;
  return ((m + (m & (0U - m))) & m) == 0;
}

// Index of the mask's lowest set bit: the right-shift that turns the masked
// byte into the field value. Caller guarantees mask != 0.
inline uint8_t bit_mask_shift(uint8_t mask) {
  uint8_t shift = 0;
  while (shift < 7 && ((mask >> shift) & 0x01) == 0)
    shift++;
  return shift;
}

// Largest field value the mask can hold (0x0F for 0xF0, 1 for 0x08).
inline uint8_t bit_field_max(uint8_t mask) {
  return mask == 0 ? 0 : static_cast<uint8_t>(mask >> bit_mask_shift(mask));
}

// field = (byte & mask) >> shift. Returns false (leaving *field untouched) for
// a zero or non-contiguous mask.
inline bool extract_bit_field(uint8_t byte, uint8_t mask, uint8_t *field) {
  if (field == nullptr || !bit_mask_contiguous(mask))
    return false;
  *field = static_cast<uint8_t>((byte & mask) >> bit_mask_shift(mask));
  return true;
}

// The read-modify-write merge: replace only the masked bits of `old` with
// `field`, keeping every other bit exactly as read. Returns false -- and the
// caller must NOT transmit -- for a zero or non-contiguous mask, or a field
// value that does not fit the mask. That last check is the encode_scaled
// contract carried over: an over-wide value is refused, never truncated into
// the field (truncation would write a different, plausible-looking value).
inline bool merge_bit_field(uint8_t old, uint8_t mask, uint32_t field, uint8_t *out) {
  if (out == nullptr || !bit_mask_contiguous(mask) || field > bit_field_max(mask))
    return false;
  const uint8_t shifted = static_cast<uint8_t>(field << bit_mask_shift(mask));
  *out = static_cast<uint8_t>((old & static_cast<uint8_t>(~mask)) | (shifted & mask));
  return true;
}

// The whole-block read-modify-write merge. The device writes its WHOLE datapoint
// (hardware-confirmed: a 1-byte write into a 2-byte block stored the telegram's
// checksum in byte 1, docs/design_notes.md SS5b), so a field smaller than its block
// is written by sending the whole block back with only the field replaced.
//
// out[0..block_len) = old with `width` bytes of `field` (little-endian, as the
// entity staged them) at `offset`. With a non-zero `mask` the field is one byte
// (width 1) and only its masked bits change -- merge_bit_field() on that byte,
// with field[0] as the FIELD value. Every other byte is copied from `old`
// unchanged. Returns false -- and the caller must NOT transmit -- for a field
// outside the block, width 0 or > 4, a mask with width != 1, a bad mask, or a
// masked field value that does not fit. `out` may alias `old`.
inline bool merge_block_field(const uint8_t *old, uint8_t block_len, uint8_t offset, uint8_t width, uint8_t mask,
                              const uint8_t *field, uint8_t *out) {
  if (old == nullptr || field == nullptr || out == nullptr || block_len == 0 || width == 0 || width > 4 ||
      static_cast<unsigned>(offset) + width > block_len)
    return false;
  if (mask != 0 && width != 1)
    return false;
  uint8_t merged_field = 0;
  if (mask != 0 && !merge_bit_field(old[offset], mask, field[0], &merged_field))
    return false;
  if (out != old)
    std::memcpy(out, old, block_len);
  if (mask != 0) {
    out[offset] = merged_field;
  } else {
    std::memcpy(out + offset, field, width);
  }
  return true;
}

// ---------------------------------------------------------------------------
// numeric decode/encode
// ---------------------------------------------------------------------------

// Optolink payloads are little-endian (verified against the optolink engine
// Converter.cpp at the pinned SHA: data[1] << 8 | data[0], etc.).
inline uint64_t read_le(const uint8_t *data, uint8_t len) {
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
inline uint64_t read_be(const uint8_t *data, uint8_t len) {
  uint64_t v = 0;
  for (uint8_t i = 0; i < len && i < 8; i++) {
    v = (v << 8) | static_cast<uint64_t>(data[i]);
  }
  return v;
}

// Two's-complement sign extension of a little-endian raw value of `len`
// bytes. len in [1,8]; len >= 8 returns the value unchanged.
inline int64_t sign_extend_le(uint64_t raw, uint8_t len) {
  if (len == 0 || len >= 8)
    return static_cast<int64_t>(raw);
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
  if (data == nullptr || len == 0 || len > 8 || data_len < len)
    return false;
  const uint64_t raw = read_le(data, len);
  const double v = is_signed ? static_cast<double>(sign_extend_le(raw, len)) : static_cast<double>(raw);
  *out = v * scale;
  return true;
}

// Big-endian counterpart of decode_scaled for Vitosoft "RotateBytes" datapoints.
// Identical contract; only the byte assembly differs (read_be).
inline bool decode_scaled_be(const uint8_t *data, std::size_t data_len, uint8_t len, bool is_signed, double scale,
                             double *out) {
  if (data == nullptr || len == 0 || len > 8 || data_len < len)
    return false;
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
inline bool encode_scaled(double value, double scale, bool is_signed, uint8_t len, uint8_t *buf) {
  if (buf == nullptr || len == 0 || len > 4 || scale == 0.0 || !std::isfinite(value))
    return false;
  const double raw_d = value / scale;
  if (!std::isfinite(raw_d))
    return false;
  // Guard the llround argument itself. When the rounded value does not fit
  // long long, C17 (7.12.9.7) leaves std::llround's numeric result
  // UNSPECIFIED (a domain error may also occur). glibc happens to saturate to
  // LLONG_MAX/LLONG_MIN -- which the per-width range checks below reject --
  // but that is an implementation detail, not a guarantee: an unspecified
  // value is permitted to land INSIDE the accepted range, turning an absurd
  // value/scale pair (e.g. 1e30 / 0.1) into a transmitted garbage raw. This
  // check makes the rejection deterministic. Any magnitude at or beyond 2^32
  // is already out of range for every len <= 4 (the widest legal raw is
  // 2^32 - 1, unsigned len 4), so it moves no accept/reject boundary.
  if (raw_d >= 4294967296.0 || raw_d <= -4294967296.0)
    return false;
  const int64_t raw = static_cast<int64_t>(std::llround(raw_d));
  if (is_signed) {
    const int64_t lo = -(1LL << (8 * len - 1));
    const int64_t hi = (1LL << (8 * len - 1)) - 1;
    if (raw < lo || raw > hi)
      return false;
  } else {
    // 1ULL is >= 64 bits on any conforming compiler, so the shift is
    // well-defined for len == 4 too ((1ULL << 32) - 1 == 0xFFFFFFFF); no
    // special case needed.
    if (raw < 0 || static_cast<uint64_t>(raw) > ((1ULL << (8 * len)) - 1))
      return false;
  }
  const uint64_t u = static_cast<uint64_t>(raw);  // two's complement bit pattern
  for (uint8_t i = 0; i < len; i++) {
    buf[i] = static_cast<uint8_t>((u >> (8 * i)) & 0xFF);
  }
  return true;
}

// Write an already-raw (unscaled) value as `len` little-endian bytes -- the
// enum/boolean write path, where the wire value IS the configured raw option and
// no scaling applies. `encode_scaled` is the numeric counterpart.
//
// `len` is CLAMPED to `cap` (the caller's staging buffer size) and the effective
// width is returned, so an over-long write-datapoint length can never make the
// caller memcpy past its buffer. This mirrors the read path's
// `read_le(p, len > 4 ? 4 : len)` clamp. Returns 0 for bad arguments, which the
// caller must treat as "do not transmit".
inline uint8_t encode_raw_le(uint32_t value, uint8_t len, uint8_t *buf, uint8_t cap) {
  if (buf == nullptr || len == 0 || cap == 0)
    return 0;
  const uint8_t width = len > cap ? cap : len;
  for (uint8_t i = 0; i < width; i++) {
    buf[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
  }
  return width;
}

// ---------------------------------------------------------------------------
// BCD / DateTimeBCD (error history, full date)
// ---------------------------------------------------------------------------

// Decode one packed-BCD byte (0x25 -> 25). Returns false on a non-BCD nibble
// (e.g. 0xFF, the fill value of empty error-history slots).
inline bool bcd_to_int(uint8_t b, uint8_t *out) {
  const uint8_t hi = (b >> 4) & 0x0F;
  const uint8_t lo = b & 0x0F;
  if (hi > 9 || lo > 9)
    return false;
  *out = static_cast<uint8_t>(hi * 10 + lo);
  return true;
}

struct BcdDateTime {
  uint16_t year;
  uint8_t month, day, hour, minute, second;
};

// Viessmann DateTimeBCD, 8 bytes starting at data[offset]:
//   [0]=year-hi BCD (e.g. 0x20)  [1]=year-lo BCD (e.g. 0x26)
//   [2]=month BCD  [3]=day BCD  [4]=weekday (IGNORED here; its convention
//   differs by clock, see clock_weekday_base)
//   [5]=hour BCD  [6]=minute BCD  [7]=second BCD
// Layout source: InsideViessmannVitosoft, Viessmann2MQTT.py
// DateTimeFromBCD() — i.e. the reverse-engineering repo's own decoder, NOT
// the [code,day,month,...] guess in the old vitoconnect config (see
// docs/design_notes.md SS7 for that correction).
// Returns false on any non-BCD byte or an out-of-range field (empty
// error-history slots are 0xFF-filled and fail the BCD check).
inline bool decode_datetime_bcd(const uint8_t *data, std::size_t data_len, std::size_t offset, BcdDateTime *out) {
  if (data == nullptr || out == nullptr || data_len < offset + 8)
    return false;
  uint8_t yh, yl, mo, da, ho, mi, se;
  if (!bcd_to_int(data[offset + 0], &yh) || !bcd_to_int(data[offset + 1], &yl) || !bcd_to_int(data[offset + 2], &mo) ||
      !bcd_to_int(data[offset + 3], &da) || !bcd_to_int(data[offset + 5], &ho) || !bcd_to_int(data[offset + 6], &mi) ||
      !bcd_to_int(data[offset + 7], &se)) {
    return false;
  }
  if (mo < 1 || mo > 12 || da < 1 || da > 31 || ho > 23 || mi > 59 || se > 59)
    return false;
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
inline bool int_to_bcd(uint8_t dec, uint8_t *out) {
  if (out == nullptr || dec > 99)
    return false;
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
inline int64_t civil_seconds(const BcdDateTime &dt) {
  return civil_days(dt.year, dt.month, dt.day) * 86400 + static_cast<int64_t>(dt.hour) * 3600 +
         static_cast<int64_t>(dt.minute) * 60 + dt.second;
}

// Where a clock's weekday byte starts counting. SUNDAY_0 (sunday=0..saturday=6)
// is the strftime %w convention vcontrold writes, hardware-confirmed on 0x20CB:
// a read of 0x088E on a Wednesday returns weekday byte 0x03. A clock may count
// from Monday instead; the clock itself tells which (clock_weekday_base).
enum class WeekdayBase : uint8_t { SUNDAY_0, MONDAY_0 };

// ESPHome ESPTime::day_of_week is sunday=1..saturday=7. Map ESPTime -> the
// device's weekday byte in its convention.
inline uint8_t device_weekday_from_esptime(uint8_t dow_sun1, WeekdayBase base = WeekdayBase::SUNDAY_0) {
  return static_cast<uint8_t>((dow_sun1 + (base == WeekdayBase::MONDAY_0 ? 5 : 6)) % 7);
}

// The convention of a DateTimeBCD block's weekday byte ([4]), judged against
// the block's own date: counted from Sunday and from Monday, one date's
// weekday differs by exactly one, so at most one convention matches the byte.
// Returns false, leaving *out alone, when the block does not decode or the
// byte matches neither (a clock never set, a non-BCD byte).
inline bool clock_weekday_base(const uint8_t *data, std::size_t data_len, std::size_t offset, WeekdayBase *out) {
  BcdDateTime dt{};
  uint8_t weekday;
  if (out == nullptr || !decode_datetime_bcd(data, data_len, offset, &dt) || !bcd_to_int(data[offset + 4], &weekday))
    return false;
  // 1970-01-01, civil day 0, was a Thursday: 4 counted from Sunday. The decode
  // rejects years before 1990, so the day count is positive.
  const auto from_sunday = static_cast<uint8_t>((civil_days(dt.year, dt.month, dt.day) + 4) % 7);
  if (weekday == from_sunday) {
    *out = WeekdayBase::SUNDAY_0;
    return true;
  }
  if (weekday == (from_sunday + 6) % 7) {
    *out = WeekdayBase::MONDAY_0;
    return true;
  }
  return false;
}

// Encode a datetime into the 8-byte Viessmann DateTimeBCD wire layout (the
// inverse of decode_datetime_bcd):
//   [0]=century BCD  [1]=year-of-century BCD  [2]=month  [3]=day
//   [4]=weekday (0..6, in the device's WeekdayBase)  [5]=hour  [6]=minute
//   [7]=second
// The device validates the weekday against the date, so it must be counted the
// way the device counts (device_weekday_from_esptime, clock_weekday_base).
// Builds into a local and only commits on success, so a bad field leaves buf8
// unchanged. Returns false on any out-of-range field.
inline bool encode_datetime_bcd(uint16_t year, uint8_t month, uint8_t day, uint8_t weekday, uint8_t hour,
                                uint8_t minute, uint8_t second, uint8_t *buf8) {
  if (buf8 == nullptr)
    return false;
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
// pair disabled. An OFF byte of exactly 0xC0 is not a sentinel but the
// end-of-day switch point 24:00 ("00:00-24:00" = on all day); a Vitotronic
// 200 KO1B accepts it and reads it back unchanged (PR #25), so 24:00 is valid
// as an OFF time and only there. The three-bit minute field is the reason the
// device only stores 10-minute steps -- there is physically no room for a
// finer value, so the encode path truncates to the next-lower 10 minutes
// (06:07 -> 06:00).
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
// all-disabled day is the empty string. Within a pair, OFF must not precede
// ON (equal times are allowed: "00:00-00:00" is off all day); the encoder
// rejects a reversed pair rather than writing it.

// The OFF byte for 24:00 (hour 24, minute 0): end of day. Never valid as ON,
// where any hour >= 24 is the disabled sentinel.
inline constexpr uint8_t TIMEBYTE_END_OF_DAY = 0xC0;

// True if an ON switch-point byte encodes a real time (hour 0..23), false if
// it is the disabled sentinel (hour >= 24, which includes the 0xFF fill). Not
// meaningful for OFF bytes, where TIMEBYTE_END_OF_DAY is a real time.
inline bool timebyte_active(uint8_t b) { return static_cast<uint8_t>(b >> 3) < 24; }

// Decode one ON switch-point byte to hour/minute. Returns false for a
// disabled byte (hour >= 24, including 24:00); *h/*m are left untouched in
// that case. decode_schaltzeiten_day renders OFF bytes directly, so 24:00
// still decodes there.
inline bool timebyte_to_hhmm(uint8_t b, uint8_t *h, uint8_t *m) {
  if (h == nullptr || m == nullptr)
    return false;
  const uint8_t hour = static_cast<uint8_t>(b >> 3);
  if (hour >= 24)
    return false;
  *h = hour;
  *m = static_cast<uint8_t>((b & 0x07) * 10);
  return true;
}

// Encode hour/minute to one switch-point byte, truncating minutes to the
// next-lower 10-minute step. Accepts 00:00..23:59 and exactly 24:00
// (TIMEBYTE_END_OF_DAY); the day-program parser permits 24:00 only as an OFF
// time. Returns false (leaving *b untouched) for any other hour >= 24, a
// minute outside 0..59, or a null b.
inline bool hhmm_to_timebyte(uint8_t h, uint8_t m, uint8_t *b) {
  if (b == nullptr || h > 24 || m > 59 || (h == 24 && m != 0))
    return false;
  *b = static_cast<uint8_t>((h << 3) | (m / 10));  // m/10 in 0..5 fits 3 bits
  return true;
}

// Decode an 8-byte per-day program into the canonical string. Returns the
// character count written (excluding the NUL), or -1 on bad args / a payload
// shorter than 8 bytes. out_cap should be >= 48 (worst case
// "HH:MM-HH:MM" x4 + 3 spaces = 47, + NUL).
inline int decode_schaltzeiten_day(const uint8_t *data, std::size_t data_len, char *out, std::size_t out_cap) {
  if (out != nullptr && out_cap > 0)
    out[0] = '\0';
  if (data == nullptr || out == nullptr || out_cap == 0 || data_len < 8)
    return -1;
  // Trailing disabled pairs are trimmed, so find the last active ON byte.
  int last_active = -1;
  for (int p = 0; p < 4; p++) {
    if (timebyte_active(data[2 * p]))
      last_active = p;
  }
  std::size_t off = 0;
  for (int p = 0; p <= last_active; p++) {
    if (off != 0 && off < out_cap - 1)
      out[off++] = ' ';
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
    if (w < 0)
      break;
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
inline bool parse_hhmm_(const char **pp, const char *end, uint8_t *b) {
  const char *p = *pp;
  int field[2] = {0, 0};
  for (int f = 0; f < 2; f++) {
    if (p >= end || *p < '0' || *p > '9')
      return false;
    int v = 0, n = 0;
    while (p < end && *p >= '0' && *p <= '9' && n < 2) {
      v = v * 10 + (*p - '0');
      p++;
      n++;
    }
    field[f] = v;
    if (f == 0) {
      if (p >= end || *p != ':')
        return false;
      p++;
    }
  }
  if (!hhmm_to_timebyte(static_cast<uint8_t>(field[0]), static_cast<uint8_t>(field[1]), b))
    return false;
  *pp = p;
  return true;
}

// Encode the canonical string into an 8-byte per-day program (caller provides
// buf8). Empty / blank input clears the day (all 0xFF). Returns false on any
// malformed token, more than four pairs, 24:00 as an ON time, or a reversed
// pair (OFF before ON, compared after truncation to the 10-minute grid). On
// failure buf8 is left unchanged (the parse builds into a local and only
// commits on success, so a rejected string never leaves a torn write) and the
// caller must not transmit -- same contract as encode_scaled.
inline bool encode_schaltzeiten_day(const char *str, uint8_t *buf8) {
  if (buf8 == nullptr)
    return false;
  uint8_t tmp[8];
  for (int i = 0; i < 8; i++)
    tmp[i] = 0xFF;
  if (str != nullptr) {
    const char *p = str;
    const char *const end = str + std::strlen(str);
    int pair = 0;
    while (p < end) {
      while (p < end && *p == ' ')
        p++;  // skip spaces between tokens
      if (p >= end)
        break;
      if (pair >= 4)
        return false;  // too many pairs
      const char *tok_end = p;
      while (tok_end < end && *tok_end != ' ')
        tok_end++;
      bool has_colon = false;
      for (const char *q = p; q < tok_end; q++) {
        if (*q == ':') {
          has_colon = true;
          break;
        }
      }
      const std::size_t tok_len = static_cast<std::size_t>(tok_end - p);
      if (!has_colon) {  // disabled pair: "-" or "--", leaves 0xFF/0xFF
        if (!(tok_len == 1 && p[0] == '-') && !(tok_len == 2 && p[0] == '-' && p[1] == '-'))
          return false;
      } else {  // "HH:MM-HH:MM"
        const char *cur = p;
        uint8_t onb = 0, offb = 0;
        if (!parse_hhmm_(&cur, tok_end, &onb))
          return false;
        if (cur >= tok_end || *cur != '-')
          return false;
        cur++;
        if (!parse_hhmm_(&cur, tok_end, &offb))
          return false;
        if (cur != tok_end)
          return false;  // trailing garbage in the token
        // 24:00 is only valid as an end time.
        if (onb == TIMEBYTE_END_OF_DAY)
          return false;
        // Reject reversed intervals without changing the existing program.
        // Equal start and end times remain valid. The byte layout is
        // monotonic in time, so comparing bytes compares times.
        if (offb < onb)
          return false;

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
inline int decode_ascii(const uint8_t *data, std::size_t data_len, uint8_t len, char *out, std::size_t out_cap) {
  if (data == nullptr || out == nullptr || out_cap == 0)
    return -1;
  if (len == 0 || data_len < len || out_cap < static_cast<std::size_t>(len) + 1)
    return -1;
  std::size_t n = 0;
  for (uint8_t i = 0; i < len; i++) {
    const uint8_t c = data[i];
    if (c == 0x00)
      break;  // NUL terminates the string
    out[n++] = (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?';
  }
  while (n > 0 && out[n - 1] == ' ')
    n--;  // trim trailing space padding
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
inline int decode_utf16(const uint8_t *data, std::size_t data_len, uint8_t len, char *out, std::size_t out_cap) {
  if (data == nullptr || out == nullptr || out_cap == 0)
    return -1;
  if (len == 0 || (len & 1) != 0 || data_len < len)
    return -1;
  std::size_t n = 0;
  for (uint8_t i = 0; i + 1 < len; i += 2) {
    const uint16_t cu = static_cast<uint16_t>(data[i] | (data[i + 1] << 8));
    if (cu == 0x0000)
      break;  // NUL terminates
    // 0xFFFF TERMINATES. U+FFFF is a Unicode noncharacter and can never appear
    // in a label, so a fill slot means the string is over. Skipping it instead
    // (the original behaviour) walked into the padding: the full 42-byte read of
    // Beschriftung_HK1~0x7360 on 2026-07-10 returned an ODD run of thirteen 0xFF
    // bytes followed by five 0x00, so code unit 17 is 0x00FF -- and "Heizkreis 1"
    // published as "Heizkreis 1ÿ". The fill is byte-oriented, not code-unit
    // aligned; do not assume it is.
    if (cu == 0xFFFF)
      break;
    uint32_t cp = cu;
    if (cu >= 0xD800 && cu <= 0xDFFF)
      cp = '?';  // lone surrogate -> placeholder
    if (cp < 0x80) {
      if (n + 1 >= out_cap)
        break;
      out[n++] = static_cast<char>(cp);
    } else if (cp < 0x800) {
      if (n + 2 >= out_cap)
        break;
      out[n++] = static_cast<char>(0xC0 | (cp >> 6));
      out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      if (n + 3 >= out_cap)
        break;
      out[n++] = static_cast<char>(0xE0 | (cp >> 12));
      out[n++] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out[n++] = static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
  while (n > 0 && out[n - 1] == ' ')
    n--;  // trim trailing space padding
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
// Widest hex run the dump prints before eliding. Matches
// VitoHomeComponent::RAW_READ_MAX so a raw read is never elided.
inline constexpr uint8_t RAW_DUMP_MAX_HEX = 48;

// Pure / framework-free so it is host-tested in tests/native/test_decode.cpp.
// Never writes past out[cap-1]; always NUL-terminates when cap > 0. Returns
// the number of characters written (excluding the NUL), or 0 on bad args.
inline int format_raw_dump(uint16_t address, const uint8_t *data, uint8_t len, char *out, std::size_t cap) {
  if (out == nullptr || cap == 0)
    return 0;
  int off = std::snprintf(out, cap, "0x%04X:", static_cast<unsigned>(address));
  if (off < 0) {
    out[0] = '\0';
    return 0;
  }
  // Cap the hex run so a long/garbled response can't blow the line. Must be at
  // least VitoHomeComponent::RAW_READ_MAX (48) or the scan console elides the
  // very bytes the user asked for -- which it did for the 42-byte read of
  // 0x7360, hiding the fill layout behind " ...(42 bytes)".
  const uint8_t max_hex = (len < RAW_DUMP_MAX_HEX) ? len : RAW_DUMP_MAX_HEX;
  for (uint8_t i = 0; i < max_hex && static_cast<std::size_t>(off) < cap; i++) {
    const int w = std::snprintf(out + off, cap - static_cast<std::size_t>(off), " %02X",
                                static_cast<unsigned>(data == nullptr ? 0 : data[i]));
    if (w < 0)
      return 0;
    off += w;
  }
  if (len > max_hex && static_cast<std::size_t>(off) < cap) {
    const int w =
        std::snprintf(out + off, cap - static_cast<std::size_t>(off), " ...(%u bytes)", static_cast<unsigned>(len));
    if (w < 0)
      return 0;
    off += w;
  }
  if (data != nullptr && len >= 1 && len <= 8 && static_cast<std::size_t>(off) < cap) {
    const uint64_t u = read_le(data, len);
    const int64_t s = sign_extend_le(u, len);
    const int w = std::snprintf(out + off, cap - static_cast<std::size_t>(off), "  u=%llu i=%lld",
                                static_cast<unsigned long long>(u), static_cast<long long>(s));
    if (w < 0)
      return 0;
    off += w;
  }
  if (static_cast<std::size_t>(off) >= cap)
    off = static_cast<int>(cap) - 1;  // truncated; snprintf NUL-terminated
  return off;
}

}  // namespace esphome::vitohome
