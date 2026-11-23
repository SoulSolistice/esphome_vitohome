# P300 transaction harness — fixture folded from live vitohome capture

Host-side regression gate for the part of the engine upstream never unit-tested:
the request/ACK/response/fragment-reassembly choreography. It composes with the
existing 400-check `decode.h` tests — this harness covers wire→payload; those cover
payload→value.

## Status: built and run against the in-tree optolink engine — 8/8 pass

The fixture was **not** asserted on faith. It is compiled against the actual
vendored Optolink engine (`components/vitohome/optolink/`, P300 path) on the host
and executed:

```
  Outside Temp 0x5525    READ  addr=0x5525 frag=0  wire=ok   resp=ok
  Boiler Temp 0x0810     READ  addr=0x0810 frag=0  wire=ok   resp=ok
  Part No 0x08E0         READ  addr=0x08E0 frag=0  wire=ok   resp=ok
  Ctrl Serial 0xF000     READ  addr=0xF000 frag=0  wire=ok   resp=ok
  Betriebsart raw 0x2301 READ  addr=0x2301 frag=1  wire=ok   resp=ok
  Betriebsart set 0x2323 READ  addr=0x2323 frag=1  wire=ok   resp=ok
  Write 0x2323 <= 0x01   WRITE addr=0x2323 frag=0  wire=ok   resp=ok
  Write 0x2323 <= 0x03   WRITE addr=0x2323 frag=0  wire=ok   resp=ok

8 vectors, 0 failure(s)
```

Each READ asserts (1) the exact bytes the engine puts on the wire (`0x41` … request …
checksum, then the response-ACK `0x06`) and (2) the payload the parser surfaces. Each
WRITE asserts the write request frame (`41 06 00 02 <addr> 01 <data> <cs>`), the
response-ACK, and that the bare write-acknowledgement (`41 05 01 02 <addr> 01 <cs>`,
no payload) completes. WRITE vectors come from the Betriebsart switch capture and are
hardware-confirmed.

## Where the vectors come from

`fixture_vectors.h` is golden-master data lifted from `viessman-optolink-logs_6_.txt`
— the `uart_debug` capture from the **running vitohome firmware** (poll @ 09:00:43).
Every request/response checksum and payload was reverified against the capture both
programmatically and by this harness. Coverage spans lengths 1 / 2 / 7 / 16, numeric
and ASCII, contiguous and fragmented.

The two fragmented vectors are the important ones. On hardware, every `0x23xx` read
arrives split — `41 06` (start + length) on one UART read, the payload on the next —
exactly like the older Brenner-Modulation case. `fixture_vectors.h` preserves those
chunk boundaries, and the harness feeds them as separate reads so the byte-at-a-time
parser is exercised across the boundary it crosses on every poll. Both pass.

## Files

- `fixture_vectors.h` — the committed vectors (the fixture). Single source of truth;
  supersedes the placeholder Appendix A in the cleanup plan.
- `fake_optolink.h` — duck-typed Optolink stand-in: the test feeds device→ESP bytes,
  the fake captures ESP→device writes. Chunk boundaries across `feed()` calls model
  UART fragmentation.
- `tidy_headers.cpp` — not a test: the translation unit through which
  `scripts/clang_tidy.sh` analyses the hub headers that build without ESPHome.
- `test_p300_transaction.cpp` — data-driven runner: handshake into IDLE, then per
  vector replay request + chunked response and assert wire + payload.
- `proof_hub_requests.cpp` — the hub's requests (`read_request()`,
  `write_request()`) on its engine (`SelectedEngine`), once per protocol.
  On P300 and GWG the adapter and the old engine called as the hub called it
  run side by side on one clock: same refusals, same bytes, same results,
  the getters the hub used to read included, and each case's named outcome.
  On KW each request equals the one `kw_calls.h` sends.
- `proof_kw_timing.cpp` — the KW link's own timing on a frozen clock: its
  two pauses (20 ms after the ENQ, 25 ms after an answer), 0 as the same
  pass, their bounds, the ENQ hold, the fast loop while a write waits, and
  the retry after silence with its limits. The other KW proofs run on
  `kw_calls::kNoPause`, the same link without the pauses.
- `proof_kw_chain.cpp`, `proof_kw_gfa.cpp`, `proof_kw_write.cpp`,
  `proof_kw_frame.cpp` — the KW exchange: chaining and its two windows, the
  GFA read and its pause, the write acknowledgement, open frames.
- `proof_codec_equality.cpp` — the codecs (`optolink_codec_*.h`) against
  the packet classes they replaced, kept in `reference/`: every TYPE byte or
  function code, every length, boundary addresses, with and without payload,
  both GWG EOT positions; the P300 parser byte by byte on generated streams.
- `build_and_run.sh` — host compile + run. Optional `$1` is the component root
  containing `optolink/` (default `../../components/vitohome`); it compiles the
  P300 harnesses with `constants` and the header-only P300 exchange.
- `fuzz_parser_vs2.cpp` — fuzz target for `P300Parser::parse()`, in two builds
  from one source: a deterministic corpus replay (g++, run by CI) and a
  libFuzzer target (clang, run by hand). Carries the oracles.
- `fuzz_corpus_vs2.h` — its seed corpus, as byte arrays with a stated reason
  each. Seeds the fuzzer and doubles as the permanent regression corpus CI
  replays; see "Fuzzing" below.
- `fuzz_decode.cpp` — the same two builds over the buffer-writing helpers in
  `decode.h`: `decode_schaltzeiten_day`, `decode_ascii`, `decode_utf16`,
  `format_raw_dump`, `decode_datetime_bcd`, `encode_scaled`, `encode_raw_le`
  and `encode_schaltzeiten_day`, plus round-trip oracles on the bit-field
  read-modify-write merge (`merge_bit_field` / `extract_bit_field`: the bits
  outside the mask are unchanged, the field reads back as written). The
  `kDSeed_bitfield_*` seeds are what make those oracles fire on a leaky merge;
  negative-controlled with `old | shifted` in place of the masked merge.
- `test_decode.cpp` also covers the bit-field helpers **exhaustively**: all 256
  masks against a brute-force contiguity reference, and for each of the 36
  contiguous masks every old byte and every field value up to one past the
  mask's capacity. Pinned with the issue #21 bytes (`0xA8`, `0x91`) decoding to
  the export's factory defaults.
- `proof_gwg_eeprom_pairs.cpp` case (8) pins `GWGEngine::lastReadHadUnservedEeprom()`
  (THIRD_PARTY.md #25): set inside the callback exactly when an `FF FF` pair was
  part of the read, reset by the next read, never set for PHYSICAL.
- `fuzz_corpus_decode.h` — its seed corpus. The first two bytes of every seed
  steer the output-buffer capacity and the declared field width; the rest is
  the payload.
- `fuzz_field.cpp` — the same two builds over the value half of `decode.h`:
  `field_slice`, `extract_field`, the scaled decoders and `encode_scaled`, the
  BCD, DateTimeBCD and switch-point codecs, `utf16_label_bytes` and
  `setpoint_byte`. Its seeds are in `fuzz_corpus_field.h`; see "The `field`
  target" below.
- `fuzz_gwg_session.cpp` — the same two builds over whole GWG sessions: up
  to six reads and writes in any access mode against a scripted device. Its
  seeds are in `fuzz_corpus_gwg_session.h`; see "The session targets" below.
- `fuzz_kw_session.cpp` — the same over KW sessions, on a KW link
  (the KW exchange) and over the P300 engine's KW detour. Seeds in
  `fuzz_corpus_kw_session.h`.
- `fuzz_diff_engines.cpp` — the engine harnesses against the reference
  engines (`reference/`), compared pass by pass; `diff_hooks.h` is what each
  harness reports. Boundary seeds in `fuzz_corpus_diff.h`; see "The
  differential target" below.
- `sim/` — device simulators: a Vitotronic in KW and P300
  (`sim_vitotronic.h`) and a GWG controller (`sim_gwg.h`) on a shared port
  (`sim_port.h`). `proof_sim.cpp` runs each engine against its simulator,
  quirks included; `proof_sim_traces.cpp` replays real frame logs
  (`sim/trace_*.h`, from `sim/import_frames.py`) against the simulators;
  `fuzz_sim_session.cpp` fuzzes operations and quirks against them. See
  "Device simulators" below.
- `fuzz.sh` — campaign driver (clang/libFuzzer), one target or all of them
  in turn. Not a gate, not run by CI.
- `build_and_run_wsl.sh` — Windows-only convenience wrapper. Mirrors the two
  directories the suite reads onto ext4 inside WSL and then calls
  `build_and_run.sh` there, because building straight off `/mnt/<drive>` pays a
  9p round-trip per header open: 68 s in place against 39.5 s staged, for the
  same run on the same machine. Not a gate, not called by CI, and a no-op
  detour on a Linux checkout — run `build_and_run.sh` directly there. See the
  script header for the invocation.

## Fuzzing

`P300Parser::parse()` (`optolink_codec_p300.h`) is a byte-at-a-time state
machine fed straight off the optical link, where garbled RX is routine rather
than exceptional, and it is the one place in this tree with a history of
memory-safety defects — the zero-payload out-of-bounds write (THIRD_PARTY.md
#12) and, in the packet class it filled then, the RESPONSE payload guards
(#16). Both were found by someone writing
down the single input that trips them. Fuzzing covers the inputs nobody thought
of.

**Two builds, one source.** `fuzz_parser_vs2.cpp` compiles either way, so the
oracles CI enforces and the oracles a campaign enforces cannot drift apart:

| build | driver | who runs it |
|---|---|---|
| replay | walks `fuzz_corpus_vs2.h`, plus any files named as arguments | CI, via `build_and_run.sh`; sub-second, g++ |
| libFuzzer | `-fsanitize=fuzzer`, coverage-guided search | a person, via `fuzz.sh`; needs clang |

CI runs only the replay. An unbounded search does not belong on a push, and a
campaign that finds nothing is not worth a CI minute — but every input a
campaign *does* find becomes a seed, and from then on CI executes it forever.
That promotion step is the point of the split.

**The oracles.** Memory safety comes free from ASan/UBSan and is most of the
value. On top of it the harness asserts four properties, because a fuzzer with
no oracle finds only crashes while the defects this project fears are
silent-wrong-value ones (design_notes.md §1a):

1. `COMPLETE` is only ever returned on a checksum the packet agrees with.
2. For a frame the engine would deliver (`packetType() == RESPONSE`) with
   non-null `data()`, the advertised payload lies inside the packet buffer.
3. ... and `dataLength()` equals `length() - 6`, the structural invariant that
   makes (2) true.
4. **Liveness:** no byte sequence can permanently wedge the parser. After the
   input, the harness drains any partial state and feeds a probe frame, which
   must complete *and be that frame*.

(2) and (3) are scoped to RESPONSE deliberately, and the scope is the
interesting part — see the comment in `check_complete_frame()`. The parser
skips payload-length validation for the two frame shapes carrying no inline
payload, so a `READ`+`REQUEST` frame can complete claiming `dataLength() == 255`
with only six bytes written and a non-null `data()`. Nothing can reach that
today, because the P300 exchange delivers only RESPONSE frames. That
guard was added for correctness (#9); it turns out to also be what keeps a
consumer inside the packet buffer, which is recorded nowhere else.

**Both oracle classes were negative-controlled**, because an oracle that never
fires is indistinguishable from one that cannot:

- reverting the zero-payload fix in the parser makes the corpus trap
  under ASan (`heap-use-after-free`, exit 1);
- removing the `CHECKSUM` step's reset-to-`STARTBYTE` wedges the parser, and
  oracle (4) reports it against the `bad_checksum` seed.

The second control is why the probe frame is `0xBEEF`/`0xDEAD` rather than a
capture frame: the first version reused the 0x5525 response, and a wedged
parser sitting in `CHECKSUM` completed it on a coincidental checksum match —
the oracle passed and the wedge went unseen. A probe that cannot be
impersonated by stale state, plus an identity check on what completed, is the
difference between that oracle working and merely appearing to.

**Coverage.** A gcov build replayed over a campaign corpus reached 100% of
the parser's 42 branches (95.2% taken at least once) and 90.4% of lines, when
it was `parser_vs2.cpp`; the state machine moved unchanged into the codec
(`proof_codec_equality.cpp`). libFuzzer's edge count plateaus at 80, so the
search saturates this parser quickly. The uncovered remainder is `reset()`,
which is engine-driven and covered by `proof_p300_guards.cpp`.

### The `decode` target

The second target covers the other half of the path — payload to text — and one
thing the parser target does not touch at all: `encode_schaltzeiten_day()`
parses an arbitrary string a **person** typed into a Home Assistant field, with
raw `const char *` walking in `parse_hhmm_`. Everything else in this codebase
is fed by the device.

Two techniques do the work:

- **Every output buffer is heap-allocated at exactly the capacity passed in**,
  so a one-past-the-end write lands in an ASan redzone instead of in slack the
  test happened to reserve. The capacity comes from the fuzz input (1..256,
  spanning every buffer a real caller passes — `char out[64]`, `char buf[80]`,
  `char buf[160]`, `char buffer[208]`), so the truncation branches are explored
  rather than assumed away. Buffers are pre-filled with a non-zero canary so
  the NUL-termination assertions cannot pass vacuously against zeroed heap.
- **A round-trip oracle on the Schaltzeiten program**, which `decode.h` states
  as a contract ("Canonical string (round-trippable — decode and encode share
  it)"). Bytes out of the encoder must decode to a canonical string that
  re-encodes to the *same* bytes. Only that direction: device bytes → string →
  bytes is deliberately **not** a contract, because a malformed OFF byte
  renders through the same formula so the raw state stays visible, and that
  rendering is intentionally not re-encodable.

Both were negative-controlled. Loosening `decode_utf16`'s three-byte bound
check trips ASan on the seed corpus alone; swapping the ON/OFF writes in
`encode_schaltzeiten_day` — a plausible copy-paste slip — trips the round-trip
oracle naming the seeds. Note a subtlety worth keeping: a *duplicated* write
(`tmp[2*pair+1] = onb`) would **not** trip it, because that bug is idempotent
over its own output. The oracle catches reordering, not every possible error.

**Coverage.** Replayed over a campaign corpus, the target reaches 100% of
`decode.h`'s 308 branches (86.0% taken at least once) and 94.2% of lines. The
uncovered remainder is null-pointer guards and `snprintf`-failure paths that
cannot be reached from a caller passing real buffers; they are left uncovered
rather than padded with calls that cannot happen in production.

Getting there exposed a defect in the harness rather than the code, which is
worth recording. The capacity range was first capped at 96 — generous against
the longest canonical Schaltzeiten string (47 + NUL) — and that silently made
`format_raw_dump`'s `" ...(N bytes)"` elision unreachable: 48 bytes of hex need
7 + 144 characters before the elision is even considered, so the buffer was
always full first. gcov found it; no amount of fuzzing would have, because the
input space never contained the case. **A harness whose parameter range is
narrower than the caller's is not testing the caller.**

### The `field` target

`fuzz_decode.cpp` covers how a payload becomes text; `fuzz_field.cpp` covers
how it becomes a number, which is what nearly every entity reads. The input
picks a response length (up to 71, past `MAX_PAYLOAD` on purpose), a field
offset, width, mask, sign and scale, and the response is copied into a buffer
of exactly that length, so a read one past it is an ASan report.

Most oracles are round trips, because the decoders and encoders are written as
inverses: a field merged into the block with `merge_block_field()` reads back
as written, a value `decode_scaled()` delivered encodes back to the same
bytes, and the BCD, DateTimeBCD and switch-point codecs refuse the same inputs
on both sides. The rest are bounds: `field_slice()` accepts exactly the fields
inside the response, `utf16_label_bytes()` never exceeds the field, and
`setpoint_byte()` stays inside `[min, max]`, NaN included.

### The session targets

`fuzz_raw_engines.cpp` drives one telegram against a fuzzed controller. The
session targets drive up to six operations back to back, the way the hub sends
them, so the state an engine carries from one telegram to the next is under
test too. The input picks the operations and then scripts the device: a byte
with bit 7 set lets time pass in 10 ms steps, any other byte feeds that many
plus one bytes. The clock is frozen and moves only on the script's say.

`fuzz_gwg_session.cpp` covers GWG: the burst window and its fallback, EEPROM
pair runs, the ENQ-to-send delay and the response deadline, each set from the
input. Its oracles check that every accepted operation completes exactly once
with its own address and length, that refusals follow the documented rules,
that a request sent on an ENQ waited at least the delay, and that a late-answer
report names an address of the session, states the configured deadline and
never covers a telegram that got exactly its answer. An idle engine must not
write to the line.

`fuzz_kw_session.cpp` covers KW, on a KW link and over the P300 engine's
detour: reads, writes, GFA reads, generic telegrams and, on P300, RPCs mixed
with KW telegrams, so the chain window and the P300 -> KW -> P300 switches are
crossed in every order. Besides completion and the documented limits it checks
the GFA pause: a GFA read chained onto an answer goes out no earlier than
`GFA_GAP_MS` after it, unless a byte arrived in between that the engine takes
as its sync. Its first finding was a real defect: the P300 engine accepted a
read longer than `MAX_PAYLOAD`, whose answer the parser cannot hold.

### The differential target

`fuzz_diff_engines.cpp` is the acceptance check of the engine redesign: the
same bytes at the same moments. `reference/` holds a frozen copy of the
engines as they stood before it, under the namespace `optolink_ref`.
`fuzz_raw_engines.cpp`, `fuzz_gwg_session.cpp` and `fuzz_kw_session.cpp` are
each compiled twice with `-DVITOHOME_DIFF_SIDE` (`diff_hooks.h`), once against
the live engines and once against the reference, and the target runs one input
through both on the same clock. After every `loop()` it compares the bytes
written, and it compares every callback with its address and data.

The input's first byte picks the harness and, with bit 7 set, makes the
script's time steps 1 ms instead of 10 ms; the rest is that harness's input.
The replay build walks the three harness corpora twice, in 10 ms and in 1 ms
steps, and the seeds in `fuzz_corpus_diff.h`, which sit on a timing boundary.
A divergence names the pass and both sides' bytes:

```
kw session: record 10 (after 9 pass(es)): reference: pass, wrote 0 byte(s); live: pass, wrote 4 byte(s) 6B 40 50 01
```

The seeds sample time coarsely: after the script, the harnesses move in
100 ms steps. A change of a few ms shows only where a seed puts a step on
that boundary (`diff_kw_gfa_gap_edge` catches `GFA_GAP_MS` 25 → 26) or where a
campaign finds one. `fuzz.sh diff_engines` runs one, seeded with all five
corpora.

`fuzz_sim_session.cpp` is the fourth harness: there both sides face a device
simulator built from the same input.

On KW the two sides are different code. The live side runs the new KW stack
(`optolink_kw.h`: the KW codec, `EnqLink`, `Exchange`) on `VS1Engine`'s timing
(`kw_calls::kVs1Timing`: one pass later, no retry after silence), the
reference side `VS1Engine`; the new timing has its own proofs
(`proof_kw_timing.cpp`) and the harnesses' own builds run on it; `kw_calls.h` gives a harness one set of KW calls for both, and
maps the stack's `Result` back onto the old engines' two callbacks, so the
checks and the compared events stay one set. The boundary seeds put a step on
each KW timing constant the stack carries that a harness can reach: the GFA
pause, the answer deadline, the EOT interval and the watchdog. The ENQ hold
and the chain window are out of reach, because the harnesses submit the next
operation the moment the last one completes.

### Device simulators

A simulator is a controller behind the port: it sends its ENQs, parses each
telegram and answers from its memory, on the host test clock. A test states
what the device is, not which bytes arrive when. `SimVitotronic` is KW and,
on 16 00 00, P300 (the KW detour drops back to KW with an EOT); `SimGwg`
has the access modes, EEPROM pairs and the burst window. Quirks (`Quirks` in
`sim_port.h`) are the field findings as switches: answer delay and byte gap,
the ENQ window, refused chains, extra or missing answer bytes, the P300 ACK
swallowed, dead phases, one telegram in N ignored.

The defaults come from frame logs: the VScotHO1_72's P300 logs and timed
frames, and #35's GWG logs. Each default names its measurement in the
config struct.

`proof_sim_traces.cpp` checks the simulators against those logs. It replays
a trace as the engine: each `>>>` line goes to the simulator, which must send
the logged `<<<` bytes before it. The simulator's memory is seeded from each
logged answer, and a request the log shows unanswered is ignored, so the
check is the protocol: ACKs, framing, lengths, checksums, EEPROM pairs, the
ENQs in between. Times are not compared; the frame log merges bytes within
30 ms. The P300 trace found the error frame's DL byte (the code's length,
not the request's), its 125 ms pause after `41 LL`, and the `06` the device
gives an EOT while that frame is unacknowledged.

To add a trace:

```
python sim/import_frames.py LOG NAME --source "what the log is" [--start 04] [--lines N] > sim/trace_NAME.h
```

It keeps bytes and times only. Add a replay for it in `proof_sim_traces.cpp`.

`fuzz_sim_session.cpp` drives up to six operations against a simulator whose
quirks come from the input. Without quirks every operation must succeed and
every read return what the device's memory holds, a check the scripted
harnesses cannot make.

**Running a campaign.**

```
bash fuzz.sh                         # list the targets
bash fuzz.sh parser_vs2              # 5 minutes, one worker per core
bash fuzz.sh decode 3600 6           # an hour, 6 workers
bash fuzz.sh all 3600                # every target for an hour each
KEEP_GOING=1 bash fuzz.sh all 14400  # overnight: findings do not stop it
```

By default each worker stops at its first finding. `KEEP_GOING=1` switches to
libFuzzer's fork mode, which writes crashes, timeouts and out-of-memory reports
to the findings and keeps searching, so a long run is not spent by one shallow
defect. An input that runs longer than 10 s counts as a finding: every script
is bounded, so it is a loop that does not end.

Corpus, findings and one log per run live in `$FUZZ_DIR/<target>/`, by default
`~/.cache/vitohome-fuzz`, so the next campaign starts from the corpus the last
one reached; WSL clears `/tmp` on restart.

A finding is written to `$FUZZ_DIR/<target>/findings/`. Replay it against the
CI build first to confirm it is not an artefact of the fuzzer's own
instrumentation (`./fuzz_decode <crash-file>` — the replay binaries take files
as arguments), minimise it (`-minimize_crash=1`), then add the reduced bytes to
the target's corpus header with a note on what it broke, and delete the file.
A crash file left behind is a finding that will be rediscovered from scratch
next year.

## Sanitizers are not per-target

Every binary either script builds carries `$SAN` —
`-fsanitize=address,undefined -fno-sanitize-recover=all` — defined once in
`build_and_run.sh` and exported to the chained `build_and_run_protocols.sh`.
It was previously opt-in, which left every engine proof (the VS1/GWG sync,
burst and access-mode work) running with no memory checker over exactly the
byte-consuming code where garbled RX is routine; see design_notes.md §10.
`-fno-sanitize-recover=all` is what makes a UBSan hit fail the run instead of
printing into a green one.

`SAN= bash build_and_run.sh` builds unsanitized. That is a debugging
convenience — it does not count as passing the gate.

## Three things the original build surfaced (now resolved in-tree)

1. **Namespace once collided with the class name.** Upstream `VitoWiFi::VitoWiFi<VS2>`
   had to be fully qualified. After de-branding the P300 engine is
   `esphome::vitohome::optolink::VS2Engine`, so the collision is gone; the
   harness just aliases the namespace.
2. **The P300 packet was non-copyable** (deleted copy-assign — owned a `malloc`
   buffer); its successor, `P300Frame`, holds a `std::array`. The callback still
   extracts the payload there, because `data()` points into engine-owned storage valid
   only for the duration of the callback.

   The enduring finding is a real null-deref hazard: for a write-ack, `data()` returns
   `nullptr` by design but `dataLength()` still returns the echoed length, so a naive
   `data()[0..dataLength())` segfaults. The correct consumer guard is `if (data())`
   before reading payload. The two WRITE vectors guard that contract permanently.
3. **The `<iostream>` log wart is fixed.** Upstream `Logging.h`'s PC branch wrote
   `state N --> M` to `std::cout` unconditionally. The vendored `logging.h` (§1b) gates
   all engine logging on `VITOHOME_DEBUG_OPTOLINK && ESP_PLATFORM`, so the host build is
   silent — no stdout filtering needed.

## CI integration

Add as a host gate beside the decode tests (it is the host test that compiles the
actual P300 engine, not just `decode.h`):

```yaml
- name: P300 transaction harness
  working-directory: tests/native
  run: bash build_and_run.sh
```

The script exits non-zero on any vector failure. It compiles only the P300 path —
`constants` and the header-only P300 exchange and codecs — since the GWG
translation units are not referenced by these vectors.

## Extending

Add a vector by appending one `TransactionVector` to `fixture_vectors.h`: the request
frame, the device chunks (`{0x06}` then the response, split at the same boundaries the
capture shows), and the expected payload. Pull new frames straight from a `uart_debug`
capture — `>>>` is the request, `<<<` chunks are the device bytes in order.
