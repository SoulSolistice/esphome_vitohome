#pragma once
#include <cstddef>
#include <cstdint>

// Seed corpus for the differential target (fuzz_diff_engines.cpp), in its
// input layout: the harness byte, then that harness's input. The three
// harness corpora are replayed as well; these are the seeds that sit on a
// timing boundary in 1 ms steps, where a 1 ms change shows.

// KW: kw_read_then_gfa with the GFA read due at exactly GFA_GAP_MS (25 ms)
inline constexpr uint8_t kDSeed_kw_gfa_gap_edge[] = {
    0x80, 0x00, 0x01, 0x00, 0x76, 0x50, 0x00, 0x00, 0x02, 0x40,
    0x50, 0x00, 0x00, 0x00, 0x05, 0x00, 0x24, 0x99, 0x00, 0x0F,
};

// KW: the answer's byte after exactly ANSWER_TIMEOUT_MS (1000 ms) of silence: still the answer
inline constexpr uint8_t kDSeed_kw_answer_deadline_edge[] = {
    0x80, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00, 0x05,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x00, 0x42,
};

// KW: no ENQ for exactly 4000 ms, then 1 ms more: the EOT interval and the watchdog end together
inline constexpr uint8_t kDSeed_kw_eot_watchdog_edge[] = {
    0x80, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xBF, 0x81,
};

// P300: two GFA reads on the reset's ENQ, the second due at exactly GFA_GAP_MS (25 ms)
inline constexpr uint8_t kDSeed_p300_gfa_gap_edge[] = {
    0x80, 0x01, 0x01, 0x02, 0x40, 0x50, 0x00, 0x00, 0x02, 0x40, 0x06, 0x00, 0x00,
    0x00, 0x05, 0x00, 0x24, 0x99, 0x00, 0x0F, 0xFF, 0xFF, 0x00, 0x05, 0x00, 0x06,
};

struct DiffSeed {
  const char *name;
  const uint8_t *bytes;
  std::size_t len;
};

inline constexpr DiffSeed kDiffSeeds[] = {
    {"diff_kw_gfa_gap_edge", kDSeed_kw_gfa_gap_edge, sizeof(kDSeed_kw_gfa_gap_edge)},
    {"diff_kw_answer_deadline_edge", kDSeed_kw_answer_deadline_edge, sizeof(kDSeed_kw_answer_deadline_edge)},
    {"diff_kw_eot_watchdog_edge", kDSeed_kw_eot_watchdog_edge, sizeof(kDSeed_kw_eot_watchdog_edge)},
    {"diff_p300_gfa_gap_edge", kDSeed_p300_gfa_gap_edge, sizeof(kDSeed_p300_gfa_gap_edge)},
};

inline constexpr std::size_t kDiffSeedCount = sizeof(kDiffSeeds) / sizeof(kDiffSeeds[0]);
