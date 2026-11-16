#pragma once
#include <cstddef>
#include <cstdint>

// Seed corpus for the field fuzz target (fuzz_field.cpp), in that file's
// input layout: [0] response length, [1] byte_offset, [2] width, [3] mask,
// [4] signed + scale, then the response. Same contract as
// fuzz_corpus_vs2.h: seeds for a campaign, the regression corpus for CI.

// a 2-byte signed temperature, 23.1 degC
inline constexpr uint8_t kFSeed_temp_div10[] = {
    0x02, 0x00, 0x02, 0x00, 0x81, 0xE7, 0x00,
};

// the same, -10.0 degC
inline constexpr uint8_t kFSeed_temp_negative[] = {
    0x02, 0x00, 0x02, 0x00, 0x81, 0x9C, 0xFF,
};

// an enum in the high nibble of byte 1 of a 4-byte block
inline constexpr uint8_t kFSeed_enum_high_nibble[] = {
    0x04, 0x01, 0x01, 0xF0, 0x00, 0x00, 0xA5, 0x00, 0x00,
};

// a mask with a gap, refused
inline constexpr uint8_t kFSeed_gapped_mask[] = {
    0x04, 0x01, 0x01, 0x0A, 0x00, 0x00, 0xFF, 0x00, 0x00,
};

// byte_offset 1, width 2, a 2-byte answer
inline constexpr uint8_t kFSeed_field_past_response[] = {
    0x02, 0x01, 0x02, 0x00, 0x00, 0x11, 0x22,
};

// byte_offset -1 reads from the start
inline constexpr uint8_t kFSeed_negative_offset[] = {
    0x03, 0xFF, 0x02, 0x00, 0x00, 0x11, 0x22, 0x33,
};

// 4 bytes unsigned, all set, scale 0.001
inline constexpr uint8_t kFSeed_u32_milli[] = {
    0x04, 0x00, 0x04, 0x00, 0x07, 0xFF, 0xFF, 0xFF, 0xFF,
};

// 4 bytes signed, the most negative
inline constexpr uint8_t kFSeed_s32_min[] = {
    0x04, 0x00, 0x04, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80,
};

// an 8-byte counter, both byte orders
inline constexpr uint8_t kFSeed_u64_counter[] = {
    0x08, 0x00, 0x08, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x88,
};

// DateTimeBCD 2026-11-14, weekday 5, 13:45:30
inline constexpr uint8_t kFSeed_datetime_bcd[] = {
    0x08, 0x00, 0x08, 0x00, 0x00, 0x20, 0x26, 0x11, 0x14, 0x05, 0x13, 0x45, 0x30,
};

// an empty error-history slot, 0xFF filled
inline constexpr uint8_t kFSeed_datetime_empty_slot[] = {
    0x08, 0x00, 0x08, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// a counted UTF-16 label at offset 2
inline constexpr uint8_t kFSeed_utf16_label[] = {
    0x0A, 0x02, 0x08, 0x00, 0x00, 0x00, 0x03, 0x41, 0x00, 0x42, 0x00, 0x43, 0x00, 0x00, 0x00,
};

// a count that does not fit the field
inline constexpr uint8_t kFSeed_utf16_count_too_big[] = {
    0x0A, 0x02, 0x08, 0x00, 0x00, 0x00, 0x09, 0x41, 0x00, 0x42, 0x00, 0x43, 0x00, 0x00, 0x00,
};

// end of day, minute code 6, the 0xFF fill
inline constexpr uint8_t kFSeed_switch_points[] = {
    0x04, 0x00, 0x01, 0x00, 0x00, 0xC0, 0x2E, 0xFF, 0x30,
};

// a NaN climate target
inline constexpr uint8_t kFSeed_setpoint_nan[] = {
    0x01, 0x05, 0x14, 0x01, 0x18, 0x00,
};

// a +inf climate target
inline constexpr uint8_t kFSeed_setpoint_inf[] = {
    0x01, 0x05, 0x14, 0x01, 0x10, 0x00,
};

// no bytes at all
inline constexpr uint8_t kFSeed_empty_response[] = {
    0x00, 0x00, 0x01, 0x01, 0x00,
};

// a 71-byte block, field at its end
inline constexpr uint8_t kFSeed_long_block[] = {
    0x47, 0x43, 0x04, 0x00, 0x01, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D,
    0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
    0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33,
    0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
};

// the RMW merge of a flag into byte 2
inline constexpr uint8_t kFSeed_masked_write_back[] = {
    0x04, 0x02, 0x01, 0x08, 0x00, 0x10, 0x20, 0x30, 0x40, 0x01,
};

// Coverage seeds: units from a 4 h campaign corpus that reach lines the
// seeds above miss, picked greedily by the lines they add. Each names the
// functions it reaches.

// campaign coverage: harness `const bool wd_ok = bcd_to_int(resp[doff + 4], &wd) && wd <= 7;`, harness
// `encode_datetime_bcd(dt.year, dt.month, dt.day, wd_ok ? wd : 0, dt.h...`, harness `if (i != 4 || wd_ok)`, harness
// `resp[i] = (5 + i < size) ? data[5 + i] : 0;`
inline constexpr uint8_t kFSeed_cov_harness[] = {
    0x08, 0x00, 0x08, 0x00, 0x00, 0x20, 0x26, 0x11, 0x14, 0x3B,
};

// campaign coverage: harness `if (size < 5)`
inline constexpr uint8_t kFSeed_cov_harness_2[] = {
    0x08,
};

// campaign coverage: merge_bit_field
inline constexpr uint8_t kFSeed_cov_merge_bit_field[] = {
    0x08, 0x00, 0x02, 0x06, 0x21,
};

// campaign coverage: harness input decoding
inline constexpr uint8_t kFSeed_cov_harness_3[] = {
    0x21, 0x0D, 0x5B, 0x80, 0x00,
};

// campaign coverage: decode_datetime_bcd
inline constexpr uint8_t kFSeed_cov_decode_datetime_bcd[] = {
    0x00, 0x02, 0x00, 0x38, 0x40,
};

// campaign coverage: harness `const uint8_t len = static_cast<uint8_t>(width == 0 ? 1 : (width > ...`
inline constexpr uint8_t kFSeed_cov_harness_4[] = {
    0x28, 0xFC, 0xD1, 0x9B, 0xFF,
};

struct FieldSeed {
  const char *name;
  const uint8_t *bytes;
  std::size_t len;
};

inline constexpr FieldSeed kFieldSeeds[] = {
    {"temp_div10", kFSeed_temp_div10, sizeof(kFSeed_temp_div10)},
    {"temp_negative", kFSeed_temp_negative, sizeof(kFSeed_temp_negative)},
    {"enum_high_nibble", kFSeed_enum_high_nibble, sizeof(kFSeed_enum_high_nibble)},
    {"gapped_mask", kFSeed_gapped_mask, sizeof(kFSeed_gapped_mask)},
    {"field_past_response", kFSeed_field_past_response, sizeof(kFSeed_field_past_response)},
    {"negative_offset", kFSeed_negative_offset, sizeof(kFSeed_negative_offset)},
    {"u32_milli", kFSeed_u32_milli, sizeof(kFSeed_u32_milli)},
    {"s32_min", kFSeed_s32_min, sizeof(kFSeed_s32_min)},
    {"u64_counter", kFSeed_u64_counter, sizeof(kFSeed_u64_counter)},
    {"datetime_bcd", kFSeed_datetime_bcd, sizeof(kFSeed_datetime_bcd)},
    {"datetime_empty_slot", kFSeed_datetime_empty_slot, sizeof(kFSeed_datetime_empty_slot)},
    {"utf16_label", kFSeed_utf16_label, sizeof(kFSeed_utf16_label)},
    {"utf16_count_too_big", kFSeed_utf16_count_too_big, sizeof(kFSeed_utf16_count_too_big)},
    {"switch_points", kFSeed_switch_points, sizeof(kFSeed_switch_points)},
    {"setpoint_nan", kFSeed_setpoint_nan, sizeof(kFSeed_setpoint_nan)},
    {"setpoint_inf", kFSeed_setpoint_inf, sizeof(kFSeed_setpoint_inf)},
    {"empty_response", kFSeed_empty_response, sizeof(kFSeed_empty_response)},
    {"long_block", kFSeed_long_block, sizeof(kFSeed_long_block)},
    {"masked_write_back", kFSeed_masked_write_back, sizeof(kFSeed_masked_write_back)},
    {"cov_harness", kFSeed_cov_harness, sizeof(kFSeed_cov_harness)},
    {"cov_harness_2", kFSeed_cov_harness_2, sizeof(kFSeed_cov_harness_2)},
    {"cov_merge_bit_field", kFSeed_cov_merge_bit_field, sizeof(kFSeed_cov_merge_bit_field)},
    {"cov_harness_3", kFSeed_cov_harness_3, sizeof(kFSeed_cov_harness_3)},
    {"cov_decode_datetime_bcd", kFSeed_cov_decode_datetime_bcd, sizeof(kFSeed_cov_decode_datetime_bcd)},
    {"cov_harness_4", kFSeed_cov_harness_4, sizeof(kFSeed_cov_harness_4)},
};

inline constexpr std::size_t kFieldSeedCount = sizeof(kFieldSeeds) / sizeof(kFieldSeeds[0]);
