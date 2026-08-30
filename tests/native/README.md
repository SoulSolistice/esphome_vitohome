# VS2 transaction harness — fixture folded from live vitohome capture

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
- `test_vs2_transaction.cpp` — data-driven runner: handshake into IDLE, then per
  vector replay request + chunked response and assert wire + payload.
- `proof_packet_vs2_response.cpp` — ASan/UBSan regression for
  `PacketVS2::createPacket`'s RESPONSE payload guards (null-data, `len > 250`,
  buffer sizing): pre-fix, a RESPONSE with len 251–255 wrote past the packet
  array and a null data pointer was dereferenced (see THIRD_PARTY.md #16).
- `build_and_run.sh` — host compile + run. Optional `$1` is the component root
  containing `optolink/` (default `../../components/vitohome`); it compiles the
  P300 translation units (`constants`, `datapoint/*`, `protocol/vs2/*`).
- `fuzz_parser_vs2.cpp` — fuzz target for `ParserVS2::parse()`, in two builds
  from one source: a deterministic corpus replay (g++, run by CI) and a
  libFuzzer target (clang, run by hand). Carries the oracles.
- `fuzz_corpus_vs2.h` — its seed corpus, as byte arrays with a stated reason
  each. Seeds the fuzzer and doubles as the permanent regression corpus CI
  replays; see "Fuzzing" below.
- `fuzz_decode.cpp` — the same two builds over the buffer-writing helpers in
  `decode.h`: `decode_schaltzeiten_day`, `decode_ascii`, `decode_utf16`,
  `format_raw_dump`, `decode_datetime_bcd`, `encode_scaled`, `encode_raw_le`
  and `encode_schaltzeiten_day`.
- `fuzz_corpus_decode.h` — its seed corpus. The first two bytes of every seed
  steer the output-buffer capacity and the declared field width; the rest is
  the payload.
- `fuzz.sh` — campaign driver (clang/libFuzzer), one target per run. Not a
  gate, not run by CI.
- `build_and_run_wsl.sh` — Windows-only convenience wrapper. Mirrors the two
  directories the suite reads onto ext4 inside WSL and then calls
  `build_and_run.sh` there, because building straight off `/mnt/<drive>` pays a
  9p round-trip per header open: 68 s in place against 39.5 s staged, for the
  same run on the same machine. Not a gate, not called by CI, and a no-op
  detour on a Linux checkout — run `build_and_run.sh` directly there. See the
  script header for the invocation.

## Fuzzing

`ParserVS2::parse()` is a byte-at-a-time state machine fed straight off the
optical link, where garbled RX is routine rather than exceptional, and it is
the one place in this tree with a history of memory-safety defects — the
zero-payload out-of-bounds write (THIRD_PARTY.md #12) and, in the packet it
fills, the RESPONSE payload guards (#16). Both were found by someone writing
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
today, because `VS2Engine::_receive()` forwards only RESPONSE frames. That
guard was added for correctness (#9); it turns out to also be what keeps a
consumer inside the packet buffer, which is recorded nowhere else.

**Both oracle classes were negative-controlled**, because an oracle that never
fires is indistinguishable from one that cannot:

- reverting the zero-payload fix in `parser_vs2.cpp` makes the corpus trap
  under ASan (`heap-use-after-free`, exit 1);
- removing the `CHECKSUM` step's reset-to-`STARTBYTE` wedges the parser, and
  oracle (4) reports it against the `bad_checksum` seed.

The second control is why the probe frame is `0xBEEF`/`0xDEAD` rather than a
capture frame: the first version reused the 0x5525 response, and a wedged
parser sitting in `CHECKSUM` completed it on a coincidental checksum match —
the oracle passed and the wedge went unseen. A probe that cannot be
impersonated by stale state, plus an identity check on what completed, is the
difference between that oracle working and merely appearing to.

**Coverage.** A gcov build replayed over a campaign corpus reaches 100% of
`parser_vs2.cpp`'s 42 branches (95.2% taken at least once) and 90.4% of lines;
libFuzzer's edge count plateaus at 80, so the search saturates this parser
quickly. The uncovered remainder is `ParserVS2::reset()`, which is
engine-driven and covered by `proof_vs2_guards.cpp`, and the
`!_packet.setLength(b)` branch, which is unreachable by construction — a
`uint8_t` length plus one can never exceed the 256-byte `kMaxFrame`.

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

**Running a campaign.**

```
bash fuzz.sh                  # list the targets
bash fuzz.sh parser_vs2       # 5 minutes, one worker per core
bash fuzz.sh decode 3600 6    # an hour, 6 workers
```

A finding is written to `$FUZZ_DIR/<target>/findings/`. Replay it against the
CI build first to confirm it is not an artefact of the fuzzer's own
instrumentation (`./fuzz_decode <crash-file>` — the replay binaries take files
as arguments), minimise it (`-minimize_crash=1`), then add the reduced bytes to
the target's corpus header with a note on what it broke. A crash file left in
`/tmp` is a finding that will be rediscovered from scratch next year.

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
   had to be fully qualified. After de-branding the engine is
   `esphome::vitohome::optolink::OptolinkEngine<P300>`, so the collision is gone; the
   harness just aliases the namespace.
2. **`PacketVS2` was non-copyable** (deleted copy-assign — owned a `malloc` buffer).
   The §1c `std::array` swap restored copyability. The callback still extracts the
   payload there, because `data()` points into engine-owned storage valid only for the
   duration of the callback.

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
- name: VS2 transaction harness
  working-directory: tests/native
  run: bash build_and_run.sh
```

The script exits non-zero on any vector failure. It compiles only the P300 path —
`constants` / `datapoint/*` / `protocol/vs2/{vs2,parser_vs2,packet_vs2}` — since the
VS1/GWG translation units are deferred protocols these vectors do not reference.

## Extending

Add a vector by appending one `TransactionVector` to `fixture_vectors.h`: the request
frame, the device chunks (`{0x06}` then the response, split at the same boundaries the
capture shows), and the expected payload. Pull new frames straight from a `uart_debug`
capture — `>>>` is the request, `<<<` chunks are the device bytes in order.
