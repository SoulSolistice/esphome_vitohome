#pragma once
#include <cstddef>
#include <cstdint>

// Seed corpus for the generic-telegram fuzz target (fuzz_raw_engines.cpp):
// one operation each, with a controller script in that file's input layout.
// Generated from a list of (name, transaction); bytes for fuzz.sh's export.

// request(05, 1000, 2): handshake, the answer by its frame
inline constexpr uint8_t kRawEngineSeed_p300_eeprom_read[] = {
    0x00, 0x05, 0x10, 0x00, 0x02, 0x00, 0x00, 0x05, 0x00, 0x06, 0x0A,
    0x06, 0x41, 0x07, 0x01, 0x05, 0x10, 0x00, 0x02, 0xAA, 0xBB, 0x84,
};

// request(09, 4050, 1) refused with error 05
inline constexpr uint8_t kRawEngineSeed_p300_refused_05[] = {
    0x00, 0x09, 0x40, 0x50, 0x01, 0x00, 0x00, 0x05, 0x00, 0x06, 0x09,
    0x06, 0x41, 0x06, 0x03, 0x09, 0x40, 0x50, 0x01, 0x05, 0xA8,
};

// an EEPROM read out of range: a two-byte error code
inline constexpr uint8_t kRawEngineSeed_p300_error_31_01[] = {
    0x00, 0x05, 0xFF, 0x05, 0x10, 0x00, 0x00, 0x05, 0x00, 0x06, 0x0A,
    0x06, 0x41, 0x07, 0x03, 0x05, 0xFF, 0x05, 0x10, 0x31, 0x01, 0x55,
};

// request(02, 2306, 25): the write ack
inline constexpr uint8_t kRawEngineSeed_p300_write_payload[] = {
    0x00, 0x02, 0x23, 0x06, 0x01, 0x01, 0x25, 0x00, 0x05, 0x00, 0x06,
    0x08, 0x06, 0x41, 0x05, 0x01, 0x02, 0x23, 0x06, 0x01, 0x32,
};

// request(07): an RPC answer longer than its DataLength
inline constexpr uint8_t kRawEngineSeed_p300_call_long_answer[] = {
    0x00, 0x07, 0xA8, 0x01, 0x01, 0x01, 0x00, 0x00, 0x05, 0x00, 0x06, 0x10, 0x06, 0x41, 0x0D,
    0x01, 0x07, 0xA8, 0x01, 0x01, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0xDB,
};

// request with no controller at all: the timeout
inline constexpr uint8_t kRawEngineSeed_p300_silent[] = {
    0x00, 0x05, 0x10, 0x00, 0x02, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
};

// kwTelegram(7B, 0800, 2): EOT, ACK + ENQ, two bytes back, back to P300
inline constexpr uint8_t kRawEngineSeed_kw_detour_read[] = {
    0x01, 0x7B, 0x08, 0x00, 0x02, 0x00, 0x00, 0x05, 0x00, 0x06, 0x01,
    0x06, 0x05, 0x01, 0x12, 0x34, 0x9E, 0x00, 0x05, 0x00, 0x06,
};

// kwTelegram(F4, 2306, 0A 0B): one ack byte
inline constexpr uint8_t kRawEngineSeed_kw_detour_write[] = {
    0x01, 0xF4, 0x23, 0x06, 0x02, 0x01, 0x0A, 0x0B, 0x00, 0x05, 0x00, 0x06, 0x00, 0x05, 0x00, 0x00,
};

// the controller never offers its ENQ
inline constexpr uint8_t kRawEngineSeed_kw_detour_no_enq[] = {
    0x01, 0x6B, 0x40, 0x50, 0x01, 0x00, 0x00, 0x05, 0x00, 0x06, 0xFF, 0xFF, 0xFF,
};

// garbage where the ENQ should be, then the ENQ
inline constexpr uint8_t kRawEngineSeed_kw_detour_garbage[] = {
    0x01, 0x6B, 0x40, 0x50, 0x01, 0x00, 0x00, 0x05, 0x00, 0x06, 0x03, 0x41, 0x06, 0x99, 0x15, 0x00, 0x05, 0x00, 0x24,
};

// VS1 telegram(7B, 0800, 2)
inline constexpr uint8_t kRawEngineSeed_kw_telegram[] = {
    0x02, 0x7B, 0x08, 0x00, 0x02, 0x00, 0x00, 0x05, 0x01, 0x12, 0x34,
};

// VS1 telegram(68, 4050, 01): the ack
inline constexpr uint8_t kRawEngineSeed_kw_telegram_payload[] = {
    0x02, 0x68, 0x40, 0x50, 0x01, 0x01, 0x01, 0x00, 0x05, 0x00, 0x00,
};

// VS1: the answer one byte short
inline constexpr uint8_t kRawEngineSeed_kw_telegram_short[] = {
    0x02, 0x7B, 0x08, 0x00, 0x02, 0x00, 0x00, 0x05, 0x00, 0x12, 0xFF, 0xFF, 0xFF,
};

// GWG telegram(31, 05, 1)
inline constexpr uint8_t kRawEngineSeed_gwg_unnamed[] = {
    0x03, 0x31, 0x00, 0x05, 0x01, 0x00, 0x00, 0x05, 0x00, 0x7F,
};

// GWG telegram(AE, 05, 2): two complement pairs
inline constexpr uint8_t kRawEngineSeed_gwg_eeprom[] = {
    0x03, 0xAE, 0x00, 0x05, 0x02, 0x00, 0x00, 0x05, 0x01, 0xF0, 0x0F, 0x00, 0x05, 0x01, 0xEE, 0x11,
};

// GWG EEPROM: a pair that does not sum to FF
inline constexpr uint8_t kRawEngineSeed_gwg_eeprom_bad_pair[] = {
    0x03, 0xAE, 0x00, 0x05, 0x01, 0x00, 0x00, 0x05, 0x01, 0xF0, 0x01,
};

// GWG telegram(99, 10, 42): the ack
inline constexpr uint8_t kRawEngineSeed_gwg_payload[] = {
    0x03, 0x99, 0x00, 0x10, 0x01, 0x01, 0x42, 0x00, 0x05, 0x00, 0x00,
};

// GWG: an address above FF is refused
inline constexpr uint8_t kRawEngineSeed_gwg_high_address[] = {
    0x03, 0x31, 0x01, 0x00, 0x01, 0x00, 0x00, 0x05,
};

struct RawEngineSeed {
  const char *name;
  const uint8_t *bytes;
  std::size_t len;
};

inline constexpr RawEngineSeed kRawEngineSeeds[] = {
    {"p300_eeprom_read", kRawEngineSeed_p300_eeprom_read, sizeof(kRawEngineSeed_p300_eeprom_read)},
    {"p300_refused_05", kRawEngineSeed_p300_refused_05, sizeof(kRawEngineSeed_p300_refused_05)},
    {"p300_error_31_01", kRawEngineSeed_p300_error_31_01, sizeof(kRawEngineSeed_p300_error_31_01)},
    {"p300_write_payload", kRawEngineSeed_p300_write_payload, sizeof(kRawEngineSeed_p300_write_payload)},
    {"p300_call_long_answer", kRawEngineSeed_p300_call_long_answer, sizeof(kRawEngineSeed_p300_call_long_answer)},
    {"p300_silent", kRawEngineSeed_p300_silent, sizeof(kRawEngineSeed_p300_silent)},
    {"kw_detour_read", kRawEngineSeed_kw_detour_read, sizeof(kRawEngineSeed_kw_detour_read)},
    {"kw_detour_write", kRawEngineSeed_kw_detour_write, sizeof(kRawEngineSeed_kw_detour_write)},
    {"kw_detour_no_enq", kRawEngineSeed_kw_detour_no_enq, sizeof(kRawEngineSeed_kw_detour_no_enq)},
    {"kw_detour_garbage", kRawEngineSeed_kw_detour_garbage, sizeof(kRawEngineSeed_kw_detour_garbage)},
    {"kw_telegram", kRawEngineSeed_kw_telegram, sizeof(kRawEngineSeed_kw_telegram)},
    {"kw_telegram_payload", kRawEngineSeed_kw_telegram_payload, sizeof(kRawEngineSeed_kw_telegram_payload)},
    {"kw_telegram_short", kRawEngineSeed_kw_telegram_short, sizeof(kRawEngineSeed_kw_telegram_short)},
    {"gwg_unnamed", kRawEngineSeed_gwg_unnamed, sizeof(kRawEngineSeed_gwg_unnamed)},
    {"gwg_eeprom", kRawEngineSeed_gwg_eeprom, sizeof(kRawEngineSeed_gwg_eeprom)},
    {"gwg_eeprom_bad_pair", kRawEngineSeed_gwg_eeprom_bad_pair, sizeof(kRawEngineSeed_gwg_eeprom_bad_pair)},
    {"gwg_payload", kRawEngineSeed_gwg_payload, sizeof(kRawEngineSeed_gwg_payload)},
    {"gwg_high_address", kRawEngineSeed_gwg_high_address, sizeof(kRawEngineSeed_gwg_high_address)},
};

inline constexpr std::size_t kRawEngineSeedCount = sizeof(kRawEngineSeeds) / sizeof(kRawEngineSeeds[0]);
