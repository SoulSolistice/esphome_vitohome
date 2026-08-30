#pragma once
#include <cstddef>
#include <cstdint>

// Seed corpus for the decode.h fuzz target (fuzz_decode.cpp).
//
// Same contract as fuzz_corpus_vs2.h: these seed the coverage-guided search
// AND, because the replay runner walks the same list, they are the permanent
// regression corpus CI executes on every push. Anything a campaign finds gets
// minimised and added here. They live in a header rather than as binary files
// so they stay reviewable in a diff and cannot be rewritten by the
// text-typed pre-commit hooks -- see fuzz_corpus_vs2.h for the full argument.
//
// INPUT LAYOUT. fuzz_decode.cpp reads the first two bytes as steering and the
// rest as the payload:
//
//   [0] capacity selector  -> output buffer capacity, 1 + b  (1..256)
//   [1] width selector     -> the `len` / `byte_length` a YAML datapoint would
//                             declare, b % 64; also picks signedness (bit 7)
//                             and width for the encode_scaled targets
//   [2..] payload          -> the device bytes, or the text a person typed
//
// The capacity is steered rather than fixed because the truncation branches --
// where the hand-rolled offset arithmetic actually bites -- only run when the
// buffer is too small, and a test that always passes a generous buffer never
// reaches them. The width is steered independently of the real payload length
// because on a device it comes from YAML, so a width that disagrees with the
// bytes present is a misconfiguration the code must reject, not a state it may
// assume away.

// the 0x7360 32-byte label block exactly as hardware returned it (design_notes 8g)
inline constexpr uint8_t kDSeed_hw_label_block_32[] = {
    0x50, 0x20, 0x00, 0x0B, 0x48, 0x00, 0x65, 0x00, 0x69, 0x00, 0x7A, 0x00, 0x6B, 0x00, 0x72, 0x00, 0x65,
    0x00, 0x69, 0x00, 0x73, 0x00, 0x20, 0x00, 0x31, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// the byte-oriented fill that made decode_utf16 publish "Heizkreis 1y": 13 x 0xFF is an ODD run, so code unit 17 is
// 0x00FF
inline constexpr uint8_t kDSeed_hw_label_odd_fill[] = {
    0x50, 0x28, 0x48, 0x00, 0x65, 0x00, 0x69, 0x00, 0x7A, 0x00, 0x6B, 0x00, 0x72, 0x00,
    0x65, 0x00, 0x69, 0x00, 0x73, 0x00, 0x20, 0x00, 0x31, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// 0x08E0 part number, the HexByte2AsciiByte path, straight off the capture
inline constexpr uint8_t kDSeed_hw_ascii_partno[] = {
    0x3C, 0x07, 0x35, 0x34, 0x36, 0x34, 0x37, 0x39, 0x33,
};

// ASCII with trailing spaces then NUL fill: exercises both the terminator and the space trim
inline constexpr uint8_t kDSeed_ascii_with_padding[] = {
    0x3C, 0x0C, 0x41, 0x42, 0x31, 0x32, 0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// an ordinary two-pair day, 06:00-22:00, trailing pairs disabled
inline constexpr uint8_t kDSeed_schaltzeiten_two_pairs[] = {
    0x3C, 0x08, 0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// all four switch-point pairs in use -- the longest canonical string, 47 chars
inline constexpr uint8_t kDSeed_schaltzeiten_full_day[] = {
    0x3C, 0x08, 0x2B, 0x40, 0x58, 0x6B, 0x80, 0x90, 0xA0, 0xB3,
};

// an interior disabled pair, which must keep its slot as "--" so positions survive the round trip
inline constexpr uint8_t kDSeed_schaltzeiten_middle_disabled[] = {
    0x3C, 0x08, 0x30, 0x40, 0xFF, 0xFF, 0x88, 0xA8, 0xFF, 0xFF,
};

// an all-disabled day: the empty canonical string
inline constexpr uint8_t kDSeed_schaltzeiten_all_ff[] = {
    0x3C, 0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// active ON with a disabled OFF -- rendered through the same formula so the raw state stays visible, and deliberately
// NOT re-encodable
inline constexpr uint8_t kDSeed_schaltzeiten_malformed_off[] = {
    0x3C, 0x08, 0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// an error-history slot: code byte then the 8-byte packed-BCD timestamp
inline constexpr uint8_t kDSeed_bcd_datetime_slot[] = {
    0x3C, 0x09, 0x0A, 0x20, 0x26, 0x08, 0x30, 0x00, 0x13, 0x45, 0x00,
};

// an empty error-history slot: every nibble non-BCD, must be rejected cleanly
inline constexpr uint8_t kDSeed_bcd_all_ff[] = {
    0x3C, 0x09, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// year 1989 -- one below the implausible-year floor -- with every other field at its maximum
inline constexpr uint8_t kDSeed_bcd_boundary_fields[] = {
    0x3C, 0x09, 0x01, 0x19, 0x89, 0x12, 0x31, 0x06, 0x23, 0x59, 0x59,
};

// a canonical Schaltzeiten string as a HUMAN would type it: the encode path's happy case
inline constexpr uint8_t kDSeed_text_program_canonical[] = {
    0x3C, 0x08, 0x30, 0x36, 0x3A, 0x30, 0x30, 0x2D, 0x32, 0x32, 0x3A, 0x30, 0x30,
    0x20, 0x30, 0x38, 0x3A, 0x33, 0x30, 0x2D, 0x31, 0x32, 0x3A, 0x30, 0x30,
};

// leading disabled pair written as "--"
inline constexpr uint8_t kDSeed_text_program_dashes[] = {
    0x3C, 0x08, 0x2D, 0x2D, 0x20, 0x30, 0x38, 0x3A, 0x33, 0x30, 0x2D, 0x31, 0x32, 0x3A, 0x30, 0x30,
};

// five pairs where the format allows four: must be rejected without touching the caller's buffer
inline constexpr uint8_t kDSeed_text_program_five_pairs[] = {
    0x3C, 0x08, 0x30, 0x31, 0x3A, 0x30, 0x30, 0x2D, 0x30, 0x32, 0x3A, 0x30, 0x30, 0x20, 0x30, 0x33,
    0x3A, 0x30, 0x30, 0x2D, 0x30, 0x34, 0x3A, 0x30, 0x30, 0x20, 0x30, 0x35, 0x3A, 0x30, 0x30, 0x2D,
    0x30, 0x36, 0x3A, 0x30, 0x30, 0x20, 0x30, 0x37, 0x3A, 0x30, 0x30, 0x2D, 0x30, 0x38, 0x3A, 0x30,
    0x30, 0x20, 0x30, 0x39, 0x3A, 0x30, 0x30, 0x2D, 0x31, 0x30, 0x3A, 0x30, 0x30,
};

// hour and minute past their limits: parse_hhmm_ must reject rather than truncate into a valid byte
inline constexpr uint8_t kDSeed_text_program_out_of_range[] = {
    0x3C, 0x08, 0x39, 0x39, 0x3A, 0x39, 0x39, 0x2D, 0x30, 0x30, 0x3A, 0x30, 0x30,
};

// trailing garbage inside a token
inline constexpr uint8_t kDSeed_text_program_trailing_junk[] = {
    0x3C, 0x08, 0x30, 0x36, 0x3A, 0x30, 0x30, 0x2D, 0x32, 0x32, 0x3A, 0x30, 0x30, 0x78,
};

// a plausible-looking string with no separator
inline constexpr uint8_t kDSeed_text_program_no_colon[] = {
    0x3C, 0x08, 0x30, 0x36, 0x30, 0x30, 0x2D, 0x32, 0x32, 0x30, 0x30,
};

// cap of 1 byte against a payload that wants 11: the truncation clamp with no room at all
inline constexpr uint8_t kDSeed_tiny_cap_real_payload[] = {
    0x00, 0x08, 0x30, 0xB0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

// cap of 2 -- room for exactly one character and the NUL
inline constexpr uint8_t kDSeed_cap_two[] = {
    0x01, 0x28, 0x48, 0x00, 0x65, 0x00, 0x69, 0x00, 0x7A, 0x00, 0x6B, 0x00,
    0x72, 0x00, 0x65, 0x00, 0x69, 0x00, 0x73, 0x00, 0x20, 0x00, 0x31, 0x00,
};

// an odd UTF-16 length, which must be rejected rather than read a half code unit
inline constexpr uint8_t kDSeed_utf16_odd_len[] = {
    0x3C, 0x07, 0x48, 0x00, 0x65, 0x00, 0x69, 0x00, 0x7A, 0x00, 0x6B, 0x00,
    0x72, 0x00, 0x65, 0x00, 0x69, 0x00, 0x73, 0x00, 0x20, 0x00, 0x31, 0x00,
};

// a declared width far longer than the bytes actually present: the misconfigured-YAML case
inline constexpr uint8_t kDSeed_len_exceeds_payload[] = {
    0x3C,
    0x3F,
    0x41,
    0x42,
};

// no payload at all
inline constexpr uint8_t kDSeed_empty_payload[] = {
    0x3C,
    0x00,
};

// an all-zero block: NUL terminators everywhere and a zero-year BCD slot
inline constexpr uint8_t kDSeed_all_zero[] = {
    0x3C, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// value 1e300 over scale 0.1 for encode_scaled: the llround domain guard, signed, width 4
inline constexpr uint8_t kDSeed_doubles_extreme[] = {
    0x3C, 0x83, 0x9C, 0x75, 0x00, 0x88, 0x3C, 0xE4, 0x37, 0x7E, 0x9A, 0x99, 0x99, 0x99, 0x99, 0x99, 0xB9, 0x3F,
};

// a non-finite value and a zero scale -- both hard rejections
inline constexpr uint8_t kDSeed_doubles_nan[] = {
    0x3C, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// an ordinary setpoint: 21.5 at scale 0.1, the idempotence path
inline constexpr uint8_t kDSeed_doubles_ordinary[] = {
    0x3C, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x35, 0x40, 0x9A, 0x99, 0x99, 0x99, 0x99, 0x99, 0xB9, 0x3F,
};
// a response longer than RAW_DUMP_MAX_HEX (48) with room to print it: the
// scan console's elision branch, which nothing else reaches -- design_notes 8c
// raised that cap to 48 precisely so a real block read is NOT elided, so the
// branch that elides is worth exercising rather than assuming.
inline constexpr uint8_t kDSeed_raw_dump_elided[] = {
    0x5F, 0x08, 0x03, 0x0A, 0x11, 0x18, 0x1F, 0x26, 0x2D, 0x34, 0x3B, 0x42, 0x49, 0x50, 0x57, 0x5E,
    0x65, 0x6C, 0x73, 0x7A, 0x81, 0x88, 0x8F, 0x96, 0x9D, 0xA4, 0xAB, 0xB2, 0xB9, 0xC0, 0xC7, 0xCE,
    0xD5, 0xDC, 0xE3, 0xEA, 0xF1, 0xF8, 0xFF, 0x06, 0x0D, 0x14, 0x1B, 0x22, 0x29, 0x30, 0x37, 0x3E,
    0x45, 0x4C, 0x53, 0x5A, 0x61, 0x68, 0x6F, 0x76, 0x7D, 0x84, 0x8B, 0x92, 0x99, 0xA0,
};

struct DecodeSeed {
  const char *name;
  const uint8_t *bytes;
  std::size_t len;
};

inline constexpr DecodeSeed kDecodeSeeds[] = {
    {"hw_label_block_32", kDSeed_hw_label_block_32, sizeof(kDSeed_hw_label_block_32)},
    {"hw_label_odd_fill", kDSeed_hw_label_odd_fill, sizeof(kDSeed_hw_label_odd_fill)},
    {"hw_ascii_partno", kDSeed_hw_ascii_partno, sizeof(kDSeed_hw_ascii_partno)},
    {"ascii_with_padding", kDSeed_ascii_with_padding, sizeof(kDSeed_ascii_with_padding)},
    {"schaltzeiten_two_pairs", kDSeed_schaltzeiten_two_pairs, sizeof(kDSeed_schaltzeiten_two_pairs)},
    {"schaltzeiten_full_day", kDSeed_schaltzeiten_full_day, sizeof(kDSeed_schaltzeiten_full_day)},
    {"schaltzeiten_middle_disabled", kDSeed_schaltzeiten_middle_disabled, sizeof(kDSeed_schaltzeiten_middle_disabled)},
    {"schaltzeiten_all_ff", kDSeed_schaltzeiten_all_ff, sizeof(kDSeed_schaltzeiten_all_ff)},
    {"schaltzeiten_malformed_off", kDSeed_schaltzeiten_malformed_off, sizeof(kDSeed_schaltzeiten_malformed_off)},
    {"bcd_datetime_slot", kDSeed_bcd_datetime_slot, sizeof(kDSeed_bcd_datetime_slot)},
    {"bcd_all_ff", kDSeed_bcd_all_ff, sizeof(kDSeed_bcd_all_ff)},
    {"bcd_boundary_fields", kDSeed_bcd_boundary_fields, sizeof(kDSeed_bcd_boundary_fields)},
    {"text_program_canonical", kDSeed_text_program_canonical, sizeof(kDSeed_text_program_canonical)},
    {"text_program_dashes", kDSeed_text_program_dashes, sizeof(kDSeed_text_program_dashes)},
    {"text_program_five_pairs", kDSeed_text_program_five_pairs, sizeof(kDSeed_text_program_five_pairs)},
    {"text_program_out_of_range", kDSeed_text_program_out_of_range, sizeof(kDSeed_text_program_out_of_range)},
    {"text_program_trailing_junk", kDSeed_text_program_trailing_junk, sizeof(kDSeed_text_program_trailing_junk)},
    {"text_program_no_colon", kDSeed_text_program_no_colon, sizeof(kDSeed_text_program_no_colon)},
    {"tiny_cap_real_payload", kDSeed_tiny_cap_real_payload, sizeof(kDSeed_tiny_cap_real_payload)},
    {"cap_two", kDSeed_cap_two, sizeof(kDSeed_cap_two)},
    {"utf16_odd_len", kDSeed_utf16_odd_len, sizeof(kDSeed_utf16_odd_len)},
    {"len_exceeds_payload", kDSeed_len_exceeds_payload, sizeof(kDSeed_len_exceeds_payload)},
    {"empty_payload", kDSeed_empty_payload, sizeof(kDSeed_empty_payload)},
    {"all_zero", kDSeed_all_zero, sizeof(kDSeed_all_zero)},
    {"doubles_extreme", kDSeed_doubles_extreme, sizeof(kDSeed_doubles_extreme)},
    {"doubles_nan", kDSeed_doubles_nan, sizeof(kDSeed_doubles_nan)},
    {"doubles_ordinary", kDSeed_doubles_ordinary, sizeof(kDSeed_doubles_ordinary)},
    {"raw_dump_elided", kDSeed_raw_dump_elided, sizeof(kDSeed_raw_dump_elided)},
};

inline constexpr std::size_t kDecodeSeedCount = sizeof(kDecodeSeeds) / sizeof(kDecodeSeeds[0]);
