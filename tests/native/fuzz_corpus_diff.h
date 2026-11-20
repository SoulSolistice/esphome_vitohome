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

struct DiffSeed {
  const char *name;
  const uint8_t *bytes;
  std::size_t len;
};

inline constexpr DiffSeed kDiffSeeds[] = {
    {"diff_kw_gfa_gap_edge", kDSeed_kw_gfa_gap_edge, sizeof(kDSeed_kw_gfa_gap_edge)},
};

inline constexpr std::size_t kDiffSeedCount = sizeof(kDiffSeeds) / sizeof(kDiffSeeds[0]);
