#pragma once

// What a fuzz harness reports when it is built as one side of the
// differential target (fuzz_diff_engines.cpp). Built with
// -DVITOHOME_DIFF_SIDE=<entry name>, a harness:
//   - runs its engines from the namespace `optolink` it aliases: the live
//     ones, or with -DVITOHOME_DIFF_REF the reference copy (reference/);
//   - exports its fuzz entry under that name and has no main();
//   - reports every engine pass, callback and failed oracle through the
//     calls below, which fuzz_diff_engines.cpp defines.
// Without VITOHOME_DIFF_SIDE the macros evaluate their arguments and discard
// them, so what a harness builds only for them still counts as used.

#include <cstddef>
#include <cstdint>

enum class DiffEvent : uint8_t { RESPONSE, ERROR, TIMING, FALLBACK, LATE };

// After each loop(): every byte the engine wrote so far on this run.
void diff_note_pass(const uint8_t *written, std::size_t total);
// A callback: its kind, address and data (nullptr: none, `length` still kept).
void diff_note_event(DiffEvent kind, uint16_t address, const uint8_t *data, std::size_t length);
// An oracle that failed on this side.
void diff_note_oracle(const char *what);

// The ms one script step lets pass: 10, or 1 for a finer replay.
extern uint32_t diff_step_ms;

#ifdef VITOHOME_DIFF_SIDE
#define DIFF_STEP_MS diff_step_ms
#define DIFF_PASS(written, total) diff_note_pass((written), (total))
#define DIFF_EVENT(kind, address, data, length) diff_note_event((kind), (address), (data), (length))
#else
#define DIFF_STEP_MS 10
#define DIFF_PASS(written, total) ((void) (written), (void) (total))
#define DIFF_EVENT(kind, address, data, length) ((void) (kind), (void) (address), (void) (data), (void) (length))
#endif
