# vitohome — Design Notes & Engineering Knowledge

The durable *why* behind the component, and the decode/protocol/validation traps
that shaped it. This is the design rationale and the hard-won lessons — not a
user manual or an API table. For the things other docs already own, this file
points rather than repeats:

- **Authoring/decoding a datapoint, the converter table, two-address `select`,
  config-time validation rules** → [`converters.md`](converters.md).
- **Quick start, YAML shape, how it works at a glance** → [`README.md`](../README.md).
- **Where the engine's behaviour differs from VitoWiFi's, and licensing** →
  [`THIRD_PARTY.md`](../components/vitohome/THIRD_PARTY.md) and
  [`NOTICE.md`](../NOTICE.md).
- **Each protocol: wire format, state flow, timing, quirks** →
  [`p300_protocol.md`](p300_protocol.md), [`kw_protocol.md`](kw_protocol.md),
  [`gwg_protocol.md`](gwg_protocol.md).
- **Catalog generation tooling** → [`scripts/README.md`](../scripts/README.md).
- **What GWG units actually did on the wire, per access mode, and the open
  questions** → [`gwg_protocol.md`](gwg_protocol.md).
- **The host test harnesses** → [`tests/native/README.md`](../tests/native/README.md).

---

## 0. Source-of-truth discipline (read this first)

Three rules govern every factual claim in this codebase, and they are the reason
the design is what it is:

- **The wire engine is the project's own code.** It began as VitoWiFi at
  `edc059a7`; every engine file has since been rewritten from the protocol and
  the field evidence. A frozen copy of the engines before the rewrite
  (`tests/native/reference/`) holds the new ones to the same bytes at the same
  moments (§3), so each change on the wire is a deliberate one, listed in
  `THIRD_PARTY.md`.
- **The reverse-engineering write-ups are dated.** InsideViessmannVitosoft's
  docs are a map, not the territory — e.g. they describe per-language
  `Textresource_de.xml`/`Textresource_en.xml`; the 2026 definition export ships
  only a slim multi-culture `Textresource.xml`, while the full per-language files
  live in the Vitosoft installation (§8i), and `DPDefinitions.xml` now
  wraps its tables in an `ImportExportDataHolder` with an extra
  `DocumentServerDataSet` the docs don't mention (§8). Datapoint facts are
  verified against a *current* export, never recalled from the docs.
- **Claims are labelled by evidence class.** A statement about VitoWiFi is
  *verified against its source at `edc059a7`*, not from memory. Findings are
  distinguished as **hardware-confirmed** (replayed against real captures /
  observed on the unit) versus **model-derived** (correct by construction but not
  yet seen on this physical heater). The two are never silently merged.

---

## 1. Architecture — the engine moves bytes; decode in the component

The engine (`components/vitohome/optolink_*.h`) is **only** the P300/KW/GWG
wire layer: framing, sequencing, ACK/NACK, checksums, retries. Every
`Datapoint` carries `optolink::noconv`, and a request carries raw bytes
(`Request`). **All value decode and encode happens in the component**, in
`decode.h`: read the raw little-endian bytes into a `uint64_t`, scale in
`double`, and narrow to `float` only at the ESPHome state boundary
(`publish_state`). The Python layer turns YAML into the three things the runtime
needs per entity — a `Datapoint(noconv)`, a decode scale, and a signedness flag.

```
YAML ──Python codegen──▶ Datapoint(noconv) + scale + signed
                              │
   bus bytes ◀── optolink engine (frame/CRC/seq) ──▶ raw payload
                              │  (byte-mover callback → ResponseView)
                       decode.h (double) ──▶ float32 state
```

This buys a single, host-testable decode/encode path with no hardware in the
loop. It was chosen against two hazards of VitoWiFi's converter layer, which
the engine no longer has:

### 1a. A tagless value union

VitoWiFi returned decoded values through a **non-discriminated** union
(`VariantValue`). Nothing recorded which member was written, so reading the
wrong one returned whatever bit pattern was there: silent garbage, not an
error. Calling `operator float()` on a `noconv` value did exactly that, which
is why the decode path is converter-aware rather than "just cast to float".

### 1b. float32 loses bits on 4-byte counters

VitoWiFi did converter arithmetic in `float`. A 32-bit counter can exceed
float32's exact-integer ceiling of `2²⁴ = 16,777,216`, after which not every
integer is representable. Burner-hours is the concrete, **hardware-confirmed**
case: a raw seconds value of **212,197,680 s** (`0x0CA5E130`, little-endian
`30 E1 A5 0C`) read at `0x08A7`. The float32 path rounds the raw integer *before*
scaling; the in-component path reads it bit-exact into `uint64_t`, scales in
`double` to **58943.8 h**, and narrows last. The host test asserts both halves:
the bit-exact integer read and the correct scaled hours. (The resulting float32
*state* of ≈59 000.0 is fine — the bug being fixed is the loss that happened
while the value was still a large raw integer.) The same double-then-narrow
discipline is what makes sub-zero temperatures decode correctly under the signed
converters; see [`converters.md`](converters.md) for the preset table and the
signedness rules.

---

## 2. The protocol layer (P300 / KW / GWG)

| Protocol | `protocol:` value | Status | Reference |
|---|---|---|---|
| P300 / VS2 | `P300` (default) | **Hardware-confirmed** on a VScotHO1 (`0x20CB`) | [p300_protocol.md](p300_protocol.md) |
| KW / VS1 | `KW` | **Hardware-confirmed** on a VScotHO1 | [kw_protocol.md](kw_protocol.md) |
| GWG | `GWG` | **Reads and EEPROM writes hardware-confirmed** on three `0x2053` units (#13, #21, #35) | [gwg_protocol.md](gwg_protocol.md) |

Each protocol document has the same layout: wire format, the engine's state
flow, a timing table (what the device allows, what the engine uses, what is
settable), the quirks found in the field, the engine's files and proofs, and
the open questions. This section holds what the protocols share.

- **8E2 at 4800 baud is mandatory, give or take a drifting clock.** `esphome
  config` rejects any data-bit/stop-bit/parity mismatch on the referenced
  `uart:` (final validation in `__init__.py`) rather than letting it surface as
  silent bus errors. The baud rate may sit anywhere in 4320-5280 (4800 ±10 %),
  with a config warning off 4800: some old WB2 controllers run their clock
  off-nominal and are reported to answer only at 4600 or 5200 (#35). Their ENQ still
  decodes at 4800, because after its first four single-bit pulses `0x05` holds
  the line low for six bit times, so drifting sample points still land on the
  right level; a request or answer with mixed bits does not survive the same
  drift. This is a config-time check, not a runtime read-back of the bus:
  `usb_uart` channels did not report their parity through
  `UARTComponent::get_parity()` before esphome/esphome#20161, so a runtime
  check failed correctly wired `usb_uart` heads.
- **Protocol is selected at compile time.** Codegen emits exactly one
  `VITOHOME_PROTOCOL_*` flag from the `protocol:` option (no flag → P300, the
  default). `protocol_select.h` resolves it to `SelectedProtocol` (the
  protocol's tag) and `SelectedEngine<Port>` (`P300Exchange`, `KwExchange` or
  `GwgExchange`).
- **The hub is protocol-agnostic by construction.** Every operation becomes
  one `Request` (`read_request()` / `write_request()` in `address_space.h`
  for an entity's space) for `SelectedEngine`. One `Result` comes back per
  request; the hub wraps
  an answer in a `ResponseView { data, data_length, address, unserved_eeprom }`
  (`response_view.h`), so the entities never see the engine. On P300 the `address`
  field is the one **echoed in the response frame itself** (the device echoes
  it on read responses and write acks alike — hardware-confirmed by the
  transaction-harness fixtures), so the hub's response-address match is a live
  wire-level check there; on KW/GWG no address travels in the response, the
  engine echoes the request's, and the check degenerates to a tautology. The
  uniform engine API is what makes adding/maintaining a second protocol cheap,
  and it is exercised host-side without hardware by
  `tests/native/engine_compile_proof.cpp` (the engine layer has no
  `Datapoint` dependency).
- **Link is verified at runtime.** After `begin()`, the hub watches for the first
  successful exchange within a window; it logs the established link, or, if
  the configured protocol never establishes one, an error — so a wrong
  `protocol:` surfaces at start-up, not as silent dead air. It also publishes
  the link offline and raises the component's error status, and it keeps
  polling: a boiler that is off at ESP boot looks the same as a wrong
  `protocol:`, and must not need a reboot of the ESP. The first answer clears
  the error, and identification is
  repeated on that link-up if it had ended as "unknown". While the link is
  offline, per-datapoint timeouts are logged at VERBOSE, not ERROR.
- **Link health counts silence, not failed operations.** NACK and an error
  frame are answers. A TIMEOUT counts towards OFFLINE, except under GWG, where
  only a request that never went out for want of an ENQ counts
  (`Result::never_sent`, [gwg_protocol.md](gwg_protocol.md#quirks)), and an
  open frame's, whose silence is the expected answer to most candidates.
- **All-`0xFF` warning (GWG).** GWG answers an address it does not serve in the
  requested mode with `0xFF`; after 8 answers in a row that are `0xFF` in every
  byte the hub warns once to check the `access:` mode.

### Access spaces: one model for `access:`

GWG modes, `rpc` and `gfa` are three answers to one question -- *which
telegram reads this address?* -- and the same address means different things
in different spaces (`0x01` in GWG's eeprom and port, `0x4050` in the
Vitotronic and in the burner control). The hub models that once:

- `AddressSpace` (`address_space.h`): `OWN` (the protocol's own read and
  write), `GFA`, `RPC`, and GWG's modes. Per protocol, constexpr:
  `space_available()`, `space_writable()`, `space_has_switch_cost()`.
- One field per entity (`VitoEntityBase::space()`) and per raw-lane
  operation (`RawOp::space`). The codegen setters (`set_access()` for a GWG
  mode, `set_rpc_prefix()`, `set_gfa()`) set it; the raw-lane API keeps its
  GWGAccessMode argument for lambdas, which maps to `OWN` off GWG.
- `access:` is accepted on `sensor`, `binary_sensor`, `text_sensor`,
  `number`, `select`, `switch` and `text` (GWG modes), and `rpc` / `gfa` on
  the three read-only platforms. `climate` and `event` do not take it.
- `send_read_()` / `send_write_()` turn a space into a `Request`
  (`read_request()` / `write_request()`, `address_space.h`), the only place on
  the dispatch path that knows protocols. Every lane goes through them: reads,
  writes, read-modify-writes, identification (`GWG_VIRTUAL` on GWG), the raw
  console. A space a protocol has no telegram for is refused there.
- The sibling refresh after a write compares spaces (same address, other
  space = other register) on every protocol, not only GWG.
- Address-space affinity: after a read in a space with a switch cost (over
  P300: GFA), the next queued read in that space goes first.
- Config: `ACCESS_SPACES` (`__init__.py`) holds, per `access:` value, the
  protocols that have it and whether it is written;
  `_validate_access_spaces()` checks every entity against it. GWG's own rules
  (a writable entity must state its mode, the physical-default warning) stay
  GWG-only. `tests/unit/test_access_spaces.py` keeps the table and
  `space_name()` in step; `tests/native/proof_address_space.cpp` pins the
  per-protocol rows.

### The generic raw operation (`queue_raw_op()`)

For reverse engineering, the raw lane sends any telegram of any layer, typed
as one line in the scan console ("Scan · Raw op") or built from a lambda.
The second field says how much of the protocol applies:

```
<layer> r <code> <address> <length>     inside the protocol, no payload
<layer> w <code> <address> <bytes...>   inside the protocol, with payload
<layer> f <bytes...> [/ <length>]       outside the protocol: an open frame
```

- `r` and `w` are telegrams of the layer's protocol. The engine builds them
  from code, address and length or payload (framing, length byte, checksum)
  and checks the answer as that protocol's answer. The code picks the
  function, so `r` and `w` only say whether a payload is carried.
- `f` is not bound by any protocol. The bytes go out exactly as given,
  nothing is added, and the answer is neither parsed nor checked. Only the
  link's timing applies: when the frame may be sent and how long its answer
  is awaited. Its layer must be the link's protocol.

Layers are `p300`, `kw` and `gwg`; codes and addresses are hex, lengths
decimal.

```
p300 r 05 1000 16      P300 function code 05 (EEPROM_READ), 16 bytes at 0x1000
p300 w 02 2306 25      a P300 telegram with payload
kw r 7B 0800 2         a KW telegram -- native on KW, the detour on P300
gwg r 31 05 1          any GWG TYPE byte
gwg f 01 43 [03 00 43 0A 01 01 01 01] 04
                       an open GWG frame, a CRC-16/KERMIT appended at ]
kw f 01 F7 00 F8 02 / 2
                       an open KW frame: a read written out, two bytes back
p300 f 41 {05 00 01 00 F8 02}
                       an open P300 frame, its checksum appended at }
```

`raw_op.h` holds the parser and the safety class, framework-free, so the
native suite pins both and fuzzes the parser. The P300 code is the 5-bit
function code; the engine adds message id 0, so `0x41` / `0x43` style
"function codes" (a message id over Virtual_READ / Physical_READ) cannot be
mistaken for new functions. The result line names the operation, the time
from dispatch, and on a P300 error frame the controller's code (`01` address
not served, `05` function not implemented).

Two config gates, both off by default:

| | standard | `raw_access: unrestricted` | + `unsafe_raw_writes: true` |
|---|---|---|---|
| `queue_raw_op()` | refused | READ, PROBE, WRITE | also UNSAFE |
| `queue_raw_frame()`, `<layer> f` | refused | refused | the link's protocol |

- READ: P300 01 / 03 / 05, KW F7 / 6B / 7B, GWG's read TYPE bytes, without
  payload.
- WRITE: what the raw lane already writes -- P300 02 and the 07 call, KW
  F4, GWG's write TYPE bytes -- with payload.
- PROBE: a code with no name here, without payload. The controller refuses
  one it lacks (P300 `05`); each is logged.
- UNSAFE: everything that may change the controller -- P300 04 / 06
  (physical / EEPROM write), KW 68 / 78 (GFA_WRITE / PROZESS_WRITE), a GWG
  EEPROM write of two bytes (the cell's pair as given, unchecked), a
  payload on a read code or on an unnamed one.

An open frame (`queue_raw_frame()`, `<layer> f <bytes...> [/ <n>]`) is for
framings nobody has settled, the GWG KM-bus read first
([`gwg_protocol.md`](gwg_protocol.md#kmbus_eeprom)). Its layer must be the
link's protocol, and the bytes go out exactly as given:

| | when | after the answer |
|---|---|---|
| GWG | on the device's next ENQ, never in a burst; the `01` is the caller's | the next request waits for an ENQ |
| KW | on the device's next ENQ, never chained; the engine sends no `01` of its own | the next request waits for an ENQ |
| P300 | inside the session, the `41` and the checksum the caller's | RESET: EOT, ENQ, SYNC |

With `/ n` the answer is `n` bytes, and fewer is a TIMEOUT whose bytes are
not delivered (`log_frames` shows them on the line); without it the engine
collects what arrives until the line is quiet (GWG its response deadline, KW 1000 ms, P300
500 ms, a choice since no P300 answer time is measured; at most 64 bytes, the
dump shows 48). On P300 the answer is raw: the ACK, then whatever frame the
controller sends, unparsed and unacknowledged. A session-start variant needs
the KW state a P300 controller drops to after an EOT, so it goes through
`kw f` on a KW link, where the engine sends EOTs while it waits for an ENQ.

The result line reads `<layer> frame` at address `0x0000`. Nothing checks
what the bytes do, so a frame is always UNSAFE. `[ ... ]` appends the bytes'
CRC-16/KERMIT, low byte first, the KM-bus telegram check; `{ ... }` appends
their sum modulo 256, the P300 checksum over the bytes after the `41`. At
most 32 bytes, the check bytes included.

#### Timed frames

A frame with `~ms`, `@now` or `/ p300` is timed. It tests a timing
hypothesis without new firmware, as #35's A/B/C test needed two engine
setters for:

```
gwg f 01 AE 16 01 04 / 2          A: everything on the ENQ (an untimed frame)
gwg f ~30 01 AE 16 01 04 / 2      B: 30 ms after the ENQ, then the frame
gwg f 01 ~30 AE 16 01 04 / 2      C: the 01 on the ENQ, the rest 30 ms later
kw f @now 01 F7 00 F8 02 / 2      KW without waiting for an ENQ
p300 f 41 {05 00 01 00 F8 02} / p300
                                  the answer as ACK and frame, checksum checked
```

- `~ms` (1..255) waits before the next byte, counted from the previous write.
  A leading one counts from the ENQ (KW, GWG) or from the start (P300,
  `@now`). At most four per frame; never two in a row, last, or inside
  `[ ]` / `{ }`.
- `@now` (KW, GWG) sends at once instead of on the device's next ENQ.
- `/ p300` (P300) reads the answer as the device's ACK (or NACK) and one
  frame, and checks its checksum. Without it the answer is raw, as for an
  untimed frame.

A timed frame does not go to the engine. The script runner (`raw_script.h`,
framework-free, `proof_raw_script.cpp`) takes the port from an idle engine,
which the hub stops looping until the script ends:

1. On KW and GWG it waits up to 4 s for the ENQ, and other bytes meanwhile
   are counted. A frame with no leading wait goes out in the pass that read
   the ENQ, as the engines answer one.
2. It writes the frame in segments at their times. The hub keeps the main
   loop awake meanwhile, so a wait is not stretched by a loop interval.
   Bytes that arrive before the last write are not the answer; they are
   counted and shown.
3. It collects the answer: `n` bytes, or until the line is quiet. The silence
   allowed is the engine's: GWG its response deadline, KW 1000 ms, P300
   500 ms. The silence check runs before the drain, as in the engines. The
   whole script ends after 8 s, inside the hub's 10 s watchdog.
4. The hub restarts the engine (`begin()`), which resyncs: P300 from its
   RESET, KW and GWG on the next ENQ, and GWG without a burst window left
   over from before. KW counts its EOT timer from the restart, so no EOT
   follows a script.

The result line has the spec, when each segment went out and when the answer
came:

```
gwg f 01 ~30 AE 16 01 04 / 2: ENQ after 812 ms, sent 01 +0 ms, AE 16 01 04 +31 ms; answer +38 ms: E9 16
```

On KW and GWG a lone 05 on an unfinished answer is the device's next ENQ,
shown as `no answer, ENQ +78 ms`. The times are read from the clock in the
loop pass that wrote or read the bytes, so they are as exact as the loop, not
the wire. A timed frame has the
open frame's gates and, like it, does not count toward link health.
`vitohome.raw_op` queues any spec from an automation, without a lambda:

```yaml
on_press:
  - vitohome.raw_op:
      spec: "gwg f 01 ~30 AE 16 01 04 / 2"
```

Every result is logged and published to the `scan_result` sensor. Home
Assistant gets them through the API, which sends at most one state per entity
per `batch_delay` (100 ms by default): results closer together arrive as the
last one. The log and an `on_value` automation on the device see each.

What timed frames measured on the reference unit is in the timing tables of
[kw_protocol.md](kw_protocol.md#timing) and
[p300_protocol.md](p300_protocol.md#timing). A timed frame is timed from an
ENQ, so on KW a frame that follows another takes about 3 s: the device's next
ENQ comes 2.97 s after its answer.

`unrestricted` needs `raw_queue_size`, and `unsafe_raw_writes` needs
`unrestricted` (config errors). The layer is checked when the line arrives:
`p300` on a P300 hub, `kw` on KW and P300, `gwg` on GWG. Entities stay on the
checked `access:` model; nothing generic reaches a catalog.

In the engine a raw operation is an ordinary `Request`: a protocol's
telegram with any code (`Lane::P300`, `KW`, `GWG`), or an open frame
(`Lane::FRAME`). The entities' reads and writes are the same requests with
fixed codes. A KW request on a P300 link takes the KW detour. A telegram
records its length and whether a payload rides when it is built, which is what
lets a code without a name go out whole.

Fuzzing: `fuzz_raw_spec.cpp` (the line, round-tripped through its canonical
form, frames included), `fuzz_gwg_session.cpp` and `fuzz_kw_session.cpp`
(frames among a session's reads and writes), `fuzz_raw_engines.cpp` (one
operation or frame against a fuzzed controller, every engine: exactly one
completion, the promised answer shape, the engine idle afterwards) and `fuzz_parser_vs2.cpp` (each input also parsed as the
answer to a raw request for its code). `tests/native/fuzz.sh raw_spec` /
`raw_engines` / `parser_vs2` run the open-ended search.

A new access function (the export also has `PROZESS_READ`, the `KBUS_*`
tunnels, `Virtual_MBUS`, `EEPROM_READ`/`Physical_READ` outside GWG, ...) is
then: an enumerator and its rows in `address_space.h`, its telegram in
`send_read_()` (and `send_write_()` if written) and in the engine, a row in
`ACCESS_SPACES` and its setter in `emit_access()`, and on the catalog side
`_access_for()`. Nothing else in the hub dispatches on it.

---

## 3. The engine: codec, link, exchange, port

The engine is four layers, all header-only files at the component's top level:

| Layer | Knows | Does not know | Files |
|---|---|---|---|
| Codec | the bytes of one telegram and its answer | time, the UART, other telegrams | `optolink_codec_kw.h`, `_p300.h`, `_gwg.h` |
| Link | who may speak when: ENQ, ACK, sync, chaining, deadlines | what the bytes mean | `optolink_link_enq.h` (`EnqLink`) |
| Exchange | one request in flight, its steps, its watchdog, its result | byte layout | `optolink_exchange.h` + `optolink_kw.h`, `optolink_p300.h`, `optolink_gwg.h` |
| Port | `available()`, `read()`, `write()`, the frame log, the recorder hook | protocols | `optolink_port.h` (`PortRef`), `vito_uart_interface.h` |

`optolink_types.h` holds what they share, `optolink_clock.h` the clock (an
injectable one on the host, §10). The KW exchange is `Exchange` over the KW
codec and `EnqLink`. P300 and GWG keep their own state machines in their
exchanges: P300 has a session and a nested KW detour, and GWG drops an ENQ read
while idle, rides its burst window from an ENQ as well as from an answer,
drains the line at the deadline and retries only burst-sent reads, each of
which `EnqLink` would need a switch for. The state flows are drawn in the
protocol documents.

**One request, one result.** The hub submits a `Request` (a protocol's
telegram by code, address, length and payload, or an open frame) and gets one
`Result`: status, the answer, a P300 error frame's code, `never_sent` (GWG: no
ENQ came), `unserved_eeprom` (a GWG `FF FF` cell), `retries`, and the timing of
an ENQ-paced telegram. The result's pointers are valid inside the callback
only. A submit from inside it is refused, since the request is still busy.

**No heap.** The exchange is a hub member, the port a pointer with one static
function table per port type, and every buffer is sized from `MAX_PAYLOAD`
(64): the hub reads at most 48 bytes and writes at most 32. A request longer
than a buffer is refused; an answer longer than it fails.

**Timing points and windows.** Every quirk found in the field so far was about
the time between two bytes, so the links have one mechanism for it rather than
one setting per case. A timing point is a moment a link writes, held back by a
settable number of ms (`TimingPoint`, `set_link_timing()`, YAML
`link_timing:`); a window is how long it waits for something (`LinkWindow`,
`set_link_window()`, GWG's answer deadline so far). The rules:

1. The default is the pacing measured on the reference unit: 0 on P300 and
   GWG, 20 and 25 ms on KW.
2. Non-blocking: the link keeps its state with a "write due at" stamp, read
   with a fresh clock, and the main loop runs fast while a write waits.
3. Bytes that arrive during a wait are not an answer; they are dropped.
4. A value other than the default is logged as a warning at boot and on every
   change, so a forgotten setting shows in every log.
5. The bounds (100 ms after an ENQ, 400 ms after an answer) stay inside the
   device's limits and, together, inside the request watchdog. The Python
   bounds and the C++ clamps are one set (`test_limits_agree.py`).

**The trace.** Each exchange reports its state changes and the facts no state
shows (bytes drained, a retry, a NACK, a refused EOT, ...) to a `LinkSink`,
null by default; the recorder takes them (§9b).

**Proving "the same bytes at the same moments".** The engine began as VitoWiFi
at `edc059a7` and was rewritten in stages, each held to the bytes and passes of
the one before:

- `tests/native/reference/` is a frozen copy of the engines before the rewrite,
  compiled only in the native tests.
- The differential target (`fuzz_diff_engines.cpp`) runs each engine harness
  twice, against the live engines and against the reference copy, on one
  scripted device and one clock, and compares every pass's writes and every
  result. It replays its seeds in 10 ms and in 1 ms steps and has boundary
  seeds for the timing constants, since a change of a few ms would otherwise
  pass. A deliberate change to the wire runs with its parameter off on the
  live side (`kw_calls::kVs1Timing`, `P300Exchange<Port, 0, 0>`), and has its
  own proof.
- `proof_codec_equality.cpp` compares the codecs with the packet classes they
  replaced, 7.4 million cases.
- Once the reference outputs are recorded as fixtures, the copy goes.

Of VitoWiFi only the `Datapoint` class remains (`optolink_datapoint.h`, MIT
notice kept); attribution and licensing are in [`NOTICE.md`](../NOTICE.md).
Two lessons from the vendored years generalise:

- **ESPHome's component file copier does not recurse into subdirectories.**
  Every engine file sits at the component's top level, so the component
  registers no library on any toolchain.
- **`ref: main` without `refresh: 0s` throttles re-pulls to one day.** A
  same-day rebuild after pushing a fix compiles a cached, stale clone. Use
  `refresh: 0s` (or a pinned ref) during active iteration.

---

## 4. Config-time validation is load-bearing, not belt-and-braces

The engine has no converter layer, and a runtime `assert()` would be compiled
out under `NDEBUG` in every ESPHome release build anyway. The converter/length
cross-checks in the Python layer are therefore the real guard: they turn a
silent-wrong-data bug into an `esphome config` error before anything reaches
the device. The actual length sets and the encodable-range rule
live in [`converters.md`](converters.md); the principle to carry is that those
checks are **load-bearing** and must stay in lockstep with `decode.h`. The
config-time encodable-range check in `number.py` mirrors
`decode.h::encode_scaled` *exactly* (round to nearest raw step, then range-check
for byte width and sign), and the C++ `encode_scaled` itself range-checks
unconditionally before transmitting: belt **and** braces, on purpose.

**When adding a converter:** update the length sets in `converters.md`,
`decode.h` and the Python validators together, and add a
`tests/unit/test_validators.py` case.

---

## 5. The write path

`number`, `select` and `switch` stage a raw payload (`encode_scaled`, or a
little-endian enum/boolean value) into the entity's buffer and call
`request_write(this)` on the hub.
The hub keeps separate read and write queues (fixed-capacity ring buffers — see
§5a) and **writes preempt reads**, so a
user setpoint change doesn't wait behind a full poll cycle. On the device ACK,
with `read_back: true` (default) a read of the same address is pushed to the
*front* of the read queue, so Home Assistant reflects the device's own view
rather than an optimistic guess; with `read_back: false` the entity publishes
optimistically on ACK.

The state model here is subtle and was the source of a real bug:

- **`write_queued_` and `write_in_flight_` are distinct states**, and conflating
  them silently loses writes. `write_queued_` means "sitting in the hub's write
  queue, awaiting dispatch"; `write_in_flight_` means "dispatched to the engine,
  awaiting ACK/error." An entity whose write is already in flight must still be
  able to **re-enqueue** a newer value (`write_queued_ = true`) even though the
  in-flight transaction can't be recalled. Every completion path — dispatch,
  write-ACK, mismatch-drop, error, watchdog — clears `write_in_flight_`
  independently of `write_queued_`. One flag for both drops the second write
  whenever `control()` fires during an in-flight transaction.
- **`pending_value_` can desync Home Assistant.** Publishing `pending_value_`
  while a write is in flight can show a state the device hasn't accepted; it must
  be managed together with `write_in_flight_`/`read_back`, not published blindly.
- **Mode controls are two-address.** Some controls (e.g. operating mode) read
  their live value at one register and accept commands at another — `address:` is
  the write/command address, optional `state_address:` is the read/state address;
  polling, read-back and response-matching use the state address. The user-facing
  shape is documented in [`converters.md`](converters.md). One **hardware
  caveat**: whether a mode write is reflected on read-back is governed by the
  device (program-switch position, register read/write asymmetry), not by the
  component — the write→ACK→read-back pipeline is correct at the software level
  but the unit may legitimately report an unchanged value. Confirm mode behaviour
  on the actual heater rather than assuming the software ACK means the mode
  changed.
- **`select` uses the index-only `control(size_t)` override** (the string
  variant forwards to it; `Select::state` is gone). Options are an **ordered `{raw_value: label}`
  map**, so the YAML author controls order; raw values are validated to fit the
  byte width and labels to be unique.
- **The raw lane goes ahead of the write lane.** Its only tenant is the
  interactive scan console, whose operations should jump a queued write so a
  range sweep feels immediate. Clock sync is not a raw operation but an entity
  on the read/write lanes (§5a), so a background sync never stalls a setpoint
  write. Writes still preempt reads, so the clock's own write is not starved.

### 5a. The three lanes are sized-at-setup ring buffers, not deques

`read_queue_`, `write_queue_` and `raw_queue_` are `RingBuffer<T>`
(`components/vitohome/ring_buffer.h`), not `std::deque`. A deque mutated on every
tick — which all three are — allocates and frees as it grows and drains: heap
churn *after* `setup()`, the one thing the memory-discipline rules forbid on the
C3. The ring makes **exactly one allocation per lane, in `reserve()` at
`setup()`, and never again**; every push/pop is O(1) with no allocation, so
run-loop traffic can't fragment the heap. It is double-ended because the write-ACK
read-back (above) prepends to the front of the read lane, so plain FIFO wouldn't
do.

**Why each lane is sized to its real need at `setup()` rather than a compile-time
cap.** `register_entity()` registers *every* defined entity, and the scheduler
polls all of them — `disabled_by_default` is a Home Assistant hint the device
ignores (§9). So `entities_.size()` is the *total defined* count, and the
generated catalogs under `example/` are meant to be `packages:`-included whole,
ranging into the thousands of datapoints. A fixed compile-time ceiling would
hard-fail a legitimate full-catalog config **at boot while `esphome config`
passes** — a config-valid-but-runtime-dead split. So the read/write lanes are
`reserve()`d to `entities_.size()` (their true ceiling: the `read_queued_` /
`write_queued_` flags admit an entity to a lane at most once, so it can never
fill), and the raw lane to `raw_queue_capacity_` — the YAML `raw_queue_size`,
defaulting to `RAW_QUEUE_DEFAULT`. One boot-time allocation each; no arbitrary
ceiling; no wasted headroom.

The raw lane is the interactive scan console and nothing else, and it defaults
to **0**: a debug tool should not tax every config, and on an ESP8266 (~40 KiB
heap) a sweep-sized lane is a quarter of the budget. Scanning is opt-in — size
`raw_queue_size` to the largest burst intended (1–2 for one-off
`queue_raw_read()` presses; `example/vitohome-scanner-raw.yaml` uses 256,
~13 KiB, for its RANGE SWEEP). An enqueue against an unallocated lane is
rejected with a warning naming the option, so the failure is loud.

That depth **cannot be derived from the config**: the lane is driven through
`queue_raw_read()`/`queue_raw_write()` from lambdas, and the shipped sweep's
count is a Home Assistant action parameter chosen at runtime, so no
`FINAL_VALIDATE_SCHEMA` inspection could infer it. Hence an explicit knob rather
than an auto-size.

**`RawOp` has no default member initializers.** `RingBuffer<T>` static_asserts
`is_trivially_default_constructible`, and one initializer makes the implicit
default constructor non-trivial; the native proofs would not see it, since they
never instantiate `RingBuffer<RawOp>`. Every construction site writes
`RawOp operation{};`, which value-initializes the enums to their 0 members.

**Device-clock synchronization does not use this lane.** It is `VitoClock`
(`vito_clock.h`), a hub-owned `VitoEntityBase` on the ordinary read/write
lanes. The fit is exact rather than
forced: `write_buf_` is 8 bytes and the clock datapoint (0x088E) is 8 bytes of
BCD; `handle_response()` is the drift compare; and `wants_read_back()` — already
true by default — makes the verify step the existing write-ACK read-back rather
than bespoke code. `VitoEntityBase` inherits from nothing (the concrete types
multiply-inherit their Home Assistant surface separately), so a pure subclass is
a lane participant with no HA presence, which is exactly what clock sync is.

So a sweep filling the raw lane cannot starve a clock write, and
`raw_queue_size: 0` does not disable time sync. The entity lanes are
`reserve()`d to `entities_.size()` and the `read_queued_`/`write_queued_` flags
admit an entity at most once, so a slot for the clock always exists.

The lane *mechanics* are pinned by
`tests/native/proof_ring_buffer.cpp::test_clock_chain_on_entity_lanes`; the
*count* cannot be, since it is derived inside `VitoHomeComponent::setup()`. It
is sampled after every `register_entity()` call, the clock's included, and
`lanes_sized_` latches then, so a late registration is rejected loudly. A count
taken one line too early reserves the lanes one short, and the clock's
priority read then costs one due entity its slot on every boot
(`read queue rejected 1 due entities`).

**The clock address is a device property, not a constant** — source-confirmed
against the Vitosoft `DPDefinitions.xml` link tables (399 `ecnDatapointType`
tokens, 104,339 `ecnDataPointTypeEventTypeLink` rows). Three schemes exist:

| Scheme | Address | Tokens |
|---|---|---|
| `NRF_Uhrzeit` | 0x088E, 8-byte `DateTimeBCD` | Ecotronic, VBC550P, VBC550S |
| `WPR_Uhrzeit` | **0x08E0**, 8-byte `DateTimeBCD` | V200WO1A, VBC700_AW, VBC700_BW_WW, VBC702_AW, VBC702_S, CU401B_A/G/S (heat pumps) |
| `GWG_Uhrzeit_*` | 0x0074/0x0075/0x0076, three 1-byte registers | GWG_VBES_00/03/21/35/36 |

Hence `time_sync: clock_address:`, defaulting to 0x088E. WPR is the case that
makes it necessary: 0x08E0 and 0x088E are both valid 16-bit addresses, so
nothing would reject anything — the component would read 8 bytes from a
datapoint that isn't the clock, compute nonsense drift, and then **blind-write a
BCD timestamp to it**. Only the address is configurable: both `DateTimeBCD`
variants are 8 bytes, and 8 is exactly `write_buf_`.

**The weekday byte's convention is not assumed either.** Three are known:
`sunday = 0` (strftime %w, what vcontrold writes), `monday = 0`, and
`monday = 1 .. sunday = 7` (ISO; optolink writes it to `0x088E`
on a Vitodens 200-W WB2A). On `0x20CB` the Wednesday read of `0x088E` (`0x03`)
fits `sunday = 0` and ISO alike. What separates them is a Sunday, and the
unit's own fault records do: every slot of both archives on the 2026-10-26 log
is stamped Sunday 2026-03-29 with weekday byte `0x07`
(`38:20:26:03:29:07:10:50:04` at `0x7507`). So `0x20CB` most likely counts ISO
-- likely, not confirmed: those are the controller's record timestamps, not a
Sunday read of `0x088E` itself. Nothing in the export says how `0x08E0` counts.
A write in the wrong convention moves the clock a day, every weekly program
with it. The sync reads the clock before it writes anyway, and that read
answers the question: the weekday byte, judged against the clock's own date
(`clock_weekday_base`, decode.h). `sunday = 0` and `monday = 0` differ on every
day, so the byte picks one; ISO equals `sunday = 0` from Monday to Saturday --
the same byte read, the same byte written, so there is nothing to decide -- and
differs only on Sunday (`7` against `0`), where the byte read decides. A byte
that fits none (a clock never set) or an unreadable clock falls back to
`sunday = 0` with a warning.

GWG is not served by that option, because its clock is a different *shape*, not
a different address — so `_final_validate` rejects `time_id` under `protocol:
GWG` outright. (The default 0x088E is over GWG's 8-bit space anyway, and
the GWG codec (`gwg_encode()`) rejects it — loudly, once per sync interval, hours
apart, which is exactly the kind of quiet breakage a config-time error is for.)

**The XML is not a complete authority here, and the option is shaped around
that.** Only **16 of the 399** tokens list any clock datapoint at all — and
`VScotHO1_72`, the reference Vitodens 300-W, is *not* among them, despite 0x088E
being hardware-confirmed on it. So the XML is authoritative that 0x08E0 exists
and differs; it is *not* authoritative that 0x088E is correct for the other 383.
That asymmetry is why this is a user-set option with a known-good default rather
than a lookup keyed on the device ident: such a lookup would answer "unknown"
for the overwhelming majority of real devices, including the one this component
was developed against. `gen_catalog.py` skips `DateTimeBCD` entirely, so the
clock is never a catalog datapoint either way.

An ordinary polled entity would queue behind every pending poll (~150 s on a
full catalog: harmless for drift, which is measured against the live time
source at response time, but slow for `sync_on_boot`). So `VitoClock` opts out
of the poll rotation (`wants_polling() == false`) and pushes to the **head** of
the read lane via `request_priority_read()` from its own schedule. Because the
read lane cannot fill, a second sync is not rejected by a full queue, so the
chain guards itself with an explicit `Phase`
(`IDLE`/`READING`/`WRITING`/`VERIFYING`).

**The Phase guard is a recovery obligation.** `tick()` refuses to start while
`phase_ != IDLE`, and nothing re-queues the clock, so *every* path that ends a
clock transaction without a `handle_response()` must notify `handle_error()` /
`handle_write_error()`, whose job is resetting the phase. The hub's
response-address-mismatch drop in `on_response_()` notifies exactly as
`on_error_()` does; without it one crossed response during `READING` or
`VERIFYING` wedges `phase_` and time sync dies silently until reboot. The rule
for any future completion path: clearing the bookkeeping flags is not enough,
the entity must hear about it.

**The API is deliberately not the deque subset.** `reserve()` is **one-shot**
(a second call is rejected, `reserve(0)` permanently initializes an unused
lane; a *failed* allocation leaves it uninitialized so `setup()` can
`mark_failed()`), and a reference-returning `front()`/`pop_front()` pair is not
exposed — a reference could escape the lock. Instead `try_front()` and
`try_pop_front()` copy under one lock.

The dispatch hand-off is **`try_front()` then `drop_front()`**: the hub looks
at the head of a lane, offers it to the protocol engine, and removes it once
the engine has accepted it; an item the engine is too busy for stays at the
head. No lock is held between the two calls, so whatever the hand-off triggers
(an error handler, a publish, an automation queueing the next item) can use
the same ring. Peek-then-drop is a race only with a second consumer, and there
is none: one loop removes from a lane or changes its head (`push_front`,
`move_to_front`, `remove`), and a producer on another task can only append,
which never changes the head of a non-empty lane.

A hand-off under the lock would hold a non-recursive mutex across engine
calls, and a FAILED raw result published from inside it deadlocks when an
automation queues the next raw operation.
`tests/unit/test_queue_lock_discipline.py` checks that no locked hand-off
returns and that the one locked predicate (`move_first_to_front_if`) stays
free of calls leading out of the hub.

Every mutating call is `[[nodiscard]]`; a rejected push **must roll back its
companion bookkeeping** (`read_queued_`/`write_queued_`) rather than leave an
entity marked queued but absent — the scheduler, `request_write()` and the
read-back path all follow that set-flag-then-push-then-roll-back pattern.

**A priority read for an entity that is already queued moves it.** The
`read_queued_` flag allows one copy per entity, so `move_to_front()` moves the
queued copy to the head, keeping the order of the rest (O(n) under the lock, n
bounded by the entity count). Otherwise a write ACKed during a poll cycle that
already holds the writer gets its read-back after the whole cycle (2.3 s on
VScotHO1_72). An entity flagged but not in the lane has its read in flight;
there is nothing to move.

**Synchronization scope, stated precisely.** Each ring serializes its own
operations through `esphome::Mutex` (FreeRTOS-backed on ESP32; an inline no-op
on the single-threaded ESP8266/RP2040; the host proofs build with
`-DVITOHOME_NATIVE_TEST`, which selects a no-op stand-in in the header since
ESPHome headers aren't on the host include path). This makes a *single queue
operation* safe against a producer on another FreeRTOS task — which is why
`queue_raw_read()`/`queue_raw_write()` are safe to call from one: `enqueue_raw_`
is a single self-contained `push_back`. It does **not** make the hub's compound
entity-lane invariants (flag *and* queue updated together) multi-task-safe;
entity control paths must stay on the ESPHome loop. Not ISR-safe by design.
Semantics are pinned in `tests/native/proof_ring_buffer.cpp` under ASan/UBSan
(one-shot reserve, FIFO, wraparound, `push_front`, full-rejection,
the `try_front`/`drop_front` hand-off and its outcomes, re-entry from inside
it, `remove`, POD round-trip, the hub dispatch pattern,
`move_to_front` including across the wrap and the read-back-behind-a-poll
scenario, negative-controlled).

---

### 5b. Bit fields: read-modify-write

A bit field is a run of bits inside one byte that other datapoints share. GWG
EEPROM `0x0005` is eight independent coding flags (K10..K17); `0x001E` is two
4-bit switching points. Across the 2026 export there are 238 such fields, none
spanning two bytes. Before `bit_mask`, every *writable* one was emitted as a
whole-byte `number` / `select` / `switch`: choosing "1 ohne Nachlauf" on
`GWG_K13Speichernachlauf` wrote `0x01` and cleared the other seven flags, and
on the read side, as soon as any sibling flag was set, the byte matched no
option.

`bit_mask` (converters.md, *Bit fields*) fixes both sides.

**Read.** The entity extracts `field = (byte & mask) >> shift` before any other
decoding (`decode.h::extract_bit_field`).

**Write.** The entity stages the *field* value. The hub turns the write into two
transactions: an `RMW_READ` of the byte at the write address, then a `WRITE` of
that byte with only the masked bits replaced (`decode.h::merge_bit_field`).

**Why in the hub, not the entity.** Atomicity across *sibling* entities is the
point. The eight flags of `0x0005` are eight entities; if each did its own read
then write (the `VitoClock` pattern), two quick changes could interleave as
A-read, B-read, A-write, B-write, and B's write would restore A's old bit. The
hub owns the single in-flight slot, so it is the one place that can guarantee
nothing touches the bus between a read and its merged write: an armed
`rmw_pending_` is dispatched before identification, the raw lane and both
entity lanes. That is the one exception to "the raw lane is uniformly
higher-priority than `write_queue_`" (§5): a scan-console write landing between
the read and the merged write would be lost.

**Fail closed.** Every `RMW_READ` completion path -- error, address mismatch,
watchdog, an empty payload -- ends in `rmw_abort_()`: `write_in_flight_` is
cleared, the entity gets `handle_write_error()`, and *nothing is written*. A
blind fallback write is exactly the corruption this exists to prevent.

**GWG "not in use".** An EEPROM `FF FF` pair is delivered as `0xFF`,
indistinguishable from "all eight flags set". Merging into it and writing it
back would turn an unserved address into a served one, so the result flags it
(`Result::unserved_eeprom`) and the hub refuses. Ordinary reads are
unchanged.

**Read-back check.** After the ACK the hub reads the byte back anyway
(`read_back`). The bits outside the mask must equal what was written; a
difference is logged as a warning, never silently accepted. It means either the
device changed a sibling field inside the read/write window (the panel, another
master), or it did not take the write as sent.

**Coalescing** needs nothing new. A value changed while an RMW is in flight sets
`write_queued_` as before. The merge reads the entity's staging buffer at merge
time, so the newest value goes out; the queued item then runs one redundant
RMW. The `write_queued_` / `write_in_flight_` split (§5) is untouched.

**Which shapes may be written.** The read and the write must be the same
register: no `state_address` (validated). In a 1-byte datapoint -- every GWG
bit field -- that register is the byte. A bit field in a byte of a wider block
(the seven `Neptun_*` outputs on VScotHO1_72, bit `0x01` of 2-byte blocks) is
written in block mode, a read-modify-write of the whole block (below), because a
write narrower than the block corrupts the rest of it.

**Evidence for the bit order on multi-bit fields.** §8 proved MSB-first on
single bits. For 4-bit fields the export's factory defaults (`ALZ`) give an
independent check: issue #21's unit read `0x0001 = 0xA8` and `0x0002 = 0x91`,
and under the MSB-first masks all four nibbles decode to their `ALZ` (K06 = 85,
K05 = 75, K08 = 9, K07 = 8); LSB-first swaps every pair and matches none.
`0x0016 = 0x28` decodes to its `ALZ` 0 K under patch 0001's inferred encoding.
This assumes the unit is at factory settings -- five matching fields make
coincidence unlikely, but it is a cross-check, not a write test.

**Status: hardware-confirmed on P300, single-bit masks, idle bus.** The merge is
exhaustively host-tested (`test_decode.cpp`: all 36 contiguous masks × 256
bytes × every field value) and fuzz-oracled with a negative control. On
VScotHO1_72, 2026-10-14, with the frame log on:

| test | wire | result |
|---|---|---|
| fail-closed, `0x7362` | read → error frame (`…:03:01:73:62:01:01:E1`) | no write telegram sent |
| sequence, `0x2330`, mask `0x01` | read `00` → write `01` → ACK → read-back `01` | read and write adjacent, nothing between |
| preservation, `0x2306` (room setpoint), mask `0x01` | read `14` → write `15` → read-back `15`; then `15` → `14` | bits `0x14` kept, twice each way |
| write during a poll burst, `0x7951` | toggled mid-cycle: read → write → ACK, then the queued polls resume | read and write adjacent, ahead of the polls |
| 1-byte write at a 2-byte block base, `0x7951` / `0x7953` | see below | **destructive** -- byte 1 overwritten |

Still open: multi-bit masks, the read-back warning path, and bit fields on
GWG (a plain EEPROM write is confirmed there). The `refresh_all()` button only
marks entities due: they are queued on the next hub tick, so pressing it does
not by itself produce a busy bus.

**A write narrower than its block corrupts the rest of the block.** The
`Neptun_*` outputs are bit `0x01` of byte 0 of 2-byte blocks (byte 1 is the
matching `*_Drehzahl`). On `0x7953` byte 1 read `00` twice before any write
(scan console and poll). Then:

| our 1-byte write | its checksum byte | block afterwards |
|---|---|---|
| `>>> 06:41:06:00:02:79:53:01:01:D6` | `D6` | `01 D6` |
| `>>> 06:41:06:00:02:79:53:01:00:D5` | `D5` | `00 D5` |

Byte 1 became the checksum byte of our own write telegram, every time; `0x7951`
showed the same with `D3` / `D4`. The device writes its whole datapoint and
takes whatever follows the payload on the wire. The ACK was normal and the
1-byte read-back confirmed byte 0, so the read-back check (which only reads the
field's own byte) could not see it -- the block length lives only in the export.
A full 2-byte write (`>>> 41:07:00:02:79:53:02:00:00:D7`) restored `00 00`, and
the byte then stayed `00`, which also rules out the device deriving byte 1 from
byte 0. Consequence: every write sends the whole datapoint. A field smaller
than its block is written as a whole-block read-modify-write (below).

**A side effect seen on the same run.** Writing `Sparbetrieb` (`0x2331`) ON
while party mode was on cleared `0x2330`: the next read-modify-write on it read
`0x00`, not the `0x01` written 5 s earlier. That is device logic (party and
economy are mutually exclusive), not a merge problem -- the read-modify-write
read the device's real state, which is exactly its job -- but HA showed the
party switch ON for those seconds, because nothing re-reads an entity whose
register another entity's write changed. The entities on the *written block*
are re-read (*Sibling refresh* below); a device-side link between two
different registers like this one stays out of reach.

#### Whole-block read-modify-write (block mode)

The bit-field mechanism, generalised from one byte to one block. A writable
`number` / `select` / `switch` with `byte_offset` and no `state_address` is in
block mode: `address` is the block base, `length` the block, the field
`byte_length` bytes at `byte_offset` (optionally with a `bit_mask` inside a
1-byte field). The entity stages only the field
(`VitoEntityBase::staged_width()`); the hub reads `length` bytes at the base
(`RMW_READ`), replaces the field (`decode.h::merge_block_field`, which defers
to `merge_bit_field` for a mask), and writes the whole block back. Everything
above carries over unchanged: nothing is dispatched between the read and the
write, every failed or short read ends in `rmw_abort_()` with nothing written,
the GWG `FF FF` refusal, coalescing. The read-back compares the **whole block**
with what was written -- field bytes for the value, every other byte for "did
not change" -- so a sibling change inside the window is visible in the log.

`byte_offset: 0` is load-bearing: it is what selects block mode for a field at
the start of a wider block (the `Neptun_*` outputs). Without it, `length: 2`
would mean a 2-byte field.

**Cap.** `MAX_RMW_BLOCK` = 16 bytes (`vitohome.h`, mirrored in `__init__.py`
and `gen_catalog.py`): the hub's merge buffer is fixed-size, sized at compile
time. 16 covers every writable block in the 2026 export apart from the 42-byte
label strings (which have no write path); 155 writable sub-block fields, all
P300.

**Live data.** Writing back a block that also holds a value the *device*
maintains (a status byte, a counter) would restore that value from the read --
stale by the length of one write telegram, but stale. The generator therefore
uses block mode only where **every** datapoint in the block is writable
(`gen_catalog.py::_read_only_blocks`, `_block_write_blocker`); a block with any
read-only datapoint (12 of the 155 fields) stays read-only with a `NOTE`. The
component itself does not know what else lives in a block and accepts any block
mode config.

**Sibling refresh.** After the write is ACKed, the hub queues a priority read
for every other polled entity whose read address is the same block base (and,
under GWG, the same `access:`), so the `*_Drehzahl` byte and the `Neptun_*`
switch on one block both show the post-write state without waiting for their
next poll (`VitoHomeComponent::refresh_siblings_`). The writing entity gets its
usual read-back.

**Read coalescing.** Entities that read one datapoint and differ only in what
they take from it (coding flags of one byte, fields of one block, a sensor and
a text_sensor on one register) share one telegram: in the generated catalogs
14,453 of 82,650 entities (17%) repeat another entity's read. When a poll read
completes, the hub takes every other entity that is *waiting in the read lane
for the same telegram* out of the lane and hands it the same outcome
(`share_read_response_` / `share_read_error_`).

- "The same telegram" is `VitoEntityBase::reads_same_datapoint()`: access
  space, address, length, and for `access: rpc` the parameter. Byte offset, bit
  mask, converter and write address play no part. Proof:
  `tests/native/proof_read_coalesce.cpp`, per protocol.
- Errors are shared as well as answers. That is where it pays most: a
  datapoint that does not answer costs its timeout once per cycle, not once per
  entity (on GWG a deadline plus an ENQ wait each).
- Only entities already waiting are served. Nothing is delivered to an entity
  that did not ask, so its own `update_interval` still decides when it reads.
- Freshness: any read that completes after a write's ACK shows the state after
  that write, so a shared answer is as good as the entity's own, with one
  exception: an entity whose own write is queued behind the read in flight is
  handed the pre-write value. Its write's ACK re-reads it (read-back, or
  sibling refresh), so the stale value stands for one transaction.
- `VitoClock` neither shares nor is skipped: it does not poll, and it runs its
  own state machine on its reads.
- The sibling refresh above needs one telegram for the whole group: the
  writer's read-back is shared with the siblings queued behind it.

Not verified on hardware. The hub has no host harness; the two building blocks
(`RingBuffer::remove()`, `reads_same_datapoint()`) are proven on the host and
the hub code is compile-checked.

**Interior shape, deprecated.** `byte_offset` with `state_address` (block read
at the base, field written to its own interior address) validates with a
warning at `esphome config`: the interior address is not a declared write
address (§8g), and the write is narrower than the datapoint. The generator does
not emit it.

**Status: hardware-confirmed on P300.** On the host,
`test_decode.cpp::test_block_field` is exhaustive over blocks of 1..6 bytes,
every in-bounds offset and width, and all 36 contiguous masks (916k checks),
plus the Neptun `00 05` cases; the fuzz target has a block-merge oracle,
negative-controlled (a merge that copies the field to the block start instead
of `offset` trips the `kDSeed_block_*` seeds). On VScotHO1_72, 2026-10-15, on
the `0x7951` block (`Neptun_Interne_Pumpe`, bit `0x01` of byte 0, and
`Neptun_Interne_Pumpe_Drehzahl`, byte 1), with the frame log on:

| test | read → merged | wire | read-back |
|---|---|---|---|
| switch ON (bit field, block mode) | `00 00` → `01 00` | `41:07:00:02:79:51:02:01:00:D6` | `01 00` |
| Drehzahl 7 with the output ON (interior field) | `01 00` → `01 07` | `41:07:00:02:79:51:02:01:07:DD` | `01 07` |
| Drehzahl 8 after a raw `00 05` | `00 05` → `00 08` | `41:07:00:02:79:51:02:00:08:DD` | `00 08` |
| Drehzahl 100 after a raw `00 D3` | `00 D3` → `00 64` | `41:07:00:02:79:51:02:00:64:39` | `00 64` |

Every write is two bytes at the block base -- never `0x7952` -- and the byte
outside the field went back as read: byte 1 stayed `00` through the switch
write, where a 1-byte write leaves the checksum (`D4`) there, and byte 0 stayed
`01` through the Drehzahl write. Each read-back compared the whole block; each
sibling refresh re-read the other two entities on `0x7951` within 0.4 s, and
the switch showed OFF right after a Drehzahl write that followed a raw write
clearing its bit. The same session (log 24) also ran a hand-written
`length: 1` `bit_mask` switch at `0x7951` and reproduced the narrow-write
corruption a third time (`00 05` → `01 D4` → `00 D3`), with a 1-byte read-back
"confirming" it: the component cannot know a block it was not told about, so
that shape stays the user's to avoid. Not run: the switch toggled with a
non-zero byte 1 (`00 05` → `01 05`); it goes through the same merge that the
Drehzahl-with-output-ON row proves.

### 5c. No state before the device has answered

Every entity publishes only from a device response -- a read, or the write ACK
with `read_back: false` -- so Home Assistant shows "unknown" until then, never a
default. Audited 2026-10-14 across all platforms: sensor, number, binary_sensor,
select, switch, text and text_sensor publish only from a response; sensor and
number go to NaN only after three consecutive read errors; the fault `event`
records a silent baseline on its first read; the connectivity sensor is
edge-published once the link is up; `device_id` publishes after identification.

`climate`'s state is a pair read from two registers, so it publishes nothing
until both channels have been read once (`VitoClimate::ready_()`). Published
after the setpoint read alone, it showed the placeholder mode `OFF` and the
real mode 120 ms later: a "Mode: OFF" blip in HA history on every reboot,
which an automation on "climate turned off" sees. A first mode read that matches no
preset still counts as read, so a device in an unmapped Betriebsart does not
hide the whole climate; the log says the coarse mode is unconfirmed.

**Unknown after failed reads.** Three failed reads in a row
(`VitoEntityBase::UNKNOWN_AFTER_READ_FAILURES`, an error or an undecodable
answer) make an entity report "unknown": sensor and number publish NaN,
binary_sensor calls `invalidate_state()`. A good read clears the count; a failed
write never counts. select, switch, text, text_sensor and climate keep their
last value, because ESPHome has no public way to report those types as
unknown; the hub's connectivity sensor shows a lost link for all of them.

**The climate's placeholder mode** is HEAT where a preset derives HEAT (or the
climate has no mode register), else OFF. It is what stays published when the
first mode read matches no preset: a circuit in an unmapped Betriebsart
(holiday program, external switching) is mostly still heating. HEAT is offered
to Home Assistant only where a preset can write it.

Commands follow the same rule. `control()` only queues: the read-back
publishes the device's view, or with `read_back: false` the acknowledged write
does; a failed write is logged and changes nothing. Publishing on queueing
would show a state before `ready_()` at boot, and keep it after a failed write
until the next poll. `tests/unit/test_climate_publish_contract.py` keeps
`control()` free of state changes.

Not a component issue, recorded because it looked like one: `esphome logs`
prints `Current Temperature: 0.00°C` and `Fan Mode: ON` for this climate. The
device sends neither -- it advertises no current temperature and no fan modes,
so the API leaves those protobuf fields at their defaults (0, the first enum
value) -- and aioesphomeapi's state formatter prints them anyway. Home Assistant
reads the traits and ignores both.

---

## 6. Device identification — `0x20CB` is only the *family*

The hub runs a small **fail-soft** state machine after `begin()` and before
regular polling: one length-4 read at `0x00F8`, falling back to four length-1
reads at `0xF8/F9/FA/FB` if the block read comes up short. It is **on by
default for every protocol** (`identify_device: false` opts out), and
hardware-confirmed on each: `0x20CB` over P300 and KW (`HW=0x03 SW=0x51`; the
fallback path is what makes it work on KW's byte-oriented protocol), and GWG
(issue #21, `0x2053 HW=0x01 SW=0x7A`, read in the virtual access mode).

On P300 and KW a last step reads **`0x7650`, one byte: the burner-control
chip** (Vitosoft's `GFA_Kennung`), which selects the GFA fault-text companion
(§8i). It is read on every unit, because the hub cannot know beforehand which
ones have a burner control; a unit without one answers with an error frame
(P300) or `0xFF` fill (KW), and those, like `0x00`, leave the chip unknown and
are logged at DEBUG only. In the whole 2026 export `0x7650` is `GFA_Kennung`
and nothing else. GWG addresses one byte, so the step does not exist there.
Hardware, VScotHO1_72 (P300): `0x7650` = `0x24` (NEC).

Then, on P300 and KW too, **`0x00F0`, two bytes: F0** (below). Read on every
unit the same way and for the same reason; the hub does not interpret it. A
unit without it answers with an error frame (P300) or `FF FF` fill (KW), which
leaves it unknown; `00 00` is a value (the generic variant of a family F0
subdivides). It is logged raw in register order (`F0=0x1E00`: `0x1E` at
`0x00F0`, `0x00` at `0x00F1`, which is F0 30) and linked as `&f0=1E00`.

Each field that can't be read is left unknown; the result (`group`,
`controller`, `hw`, `sw`, the chip and F0) is logged at INFO --
`Device identification: 0x20CB (VScotHO1) HW=0x03 SW=0x51 GFA=0x24 (NEC)` --
and pushed to any `device_id` text sensors. A small built-in table names the families seen on the
wire (`0x20CB` → VScotHO1, plus `0x2098`/`0x2094`/`0x2053`/`0x2054`); everything else is
reported as raw hex.

Mapping the tuple to a catalog file is done off-device. The hub logs a link to
the catalog finder (https://soulsolistice.github.io/esphome_vitohome/) with the bytes it
read as a query string (`?id=20CB&hw=03&sw=51&gfa=24`); the page looks them up in
`example/catalogs/resolve.json`, which `gen_catalog.py --export-all` writes by
enumerating `Catalog.resolve_pool()` (language-independent: one table serves
`example/catalogs/de/` and `…/en/`, and the finder picks the folder). The firmware carries the URL and nothing
of the table, and never contacts the site itself.

**F0 decides between variants in two families only.** Vitosoft keys a few
datapoint types on one more value, "F0" -- read at `0x00F0` by the datapoint
its own catalog names `HO2B_Erweiterte_Geraetekennung~0x00F0` ("Erweiterte
Gerätekennung (F0)", Virtual_READ, 2 bytes, `Parameter` Int, NoConversion):
one signed 16-bit little-endian integer. Vitosoft's identification
datapoints (`sysDeviceIdent*`) read only `0xF8..0xFF`; F0 is a datapoint of
the devices that need it. In the 2026 export that is the software-index range
`0xC8..0xFF` of two families:

| ident | variants (F0 range, decimal) |
|---|---|
| `0x20CB` | `VScotHO1_200` (0), `_01` (1..9), `_10` (10), `_11` (11..19), `_20` (20..29), `_30` (30..39) |
| `0x20C9` | `VScotHC1_200` (0..9), `_10` (10..19), `_30` (30..39) |

**The ranges are decimal.** Vitosoft keeps `F0` / `F0Till` as `Int32` table
extensions (the SQL database's `ecnTableExtensionValue`, `InternalDataType` 3,
like `BlockLength`), while every hex value -- `Identification`,
`IdentificationExtension`, an event's `Address` -- is a string. `30..39` is
30..39 (`0x1E..0x27`), not `0x30..0x39`. Two more signs: read in decimal the
ranges tile 0..39 without a gap, read in hex they skip `0x0A..0x0F`,
`0x1A..0x1F` and `0x2A..0x2F`; and a token's suffix is decimal throughout --
`VScotHO1_72` is software index `0x48`, `VPlusHC1_130` is `0x82`, and the
`_30` of `VScotHO1_200_30` is F0 30. Up to schema 1 of `resolve.json` the
generator read the column as hex, so a unit with F0 30 (`F0=0x1E00`) matched no
variant and one with `0x10` got `_10` instead of `_11`.

(`0x20CA` has a single F0-keyed type, and `0x2621` is OpenTherm, which is not
driven.) The variants are not interchangeable: `VScotHO1_200_01` declares 74
addresses the generic `VScotHO1_200` lacks, and `_10` .. `_30` lack 30..61 of
the generic's.

**Evaluated in the finder, not on the device.** The hub reads the two bytes at
`0x00F0` and passes them on raw (above); what they select is decided where the
catalog knowledge is, by the same rule in two places that the parity test
(`tests/unit/test_resolve_index.py`) holds together: `f0_value()` /
`resolve_device()` in `scripts/vitocatalog/identify.py` and `f0Value()` /
`lookup()` in `web/lookup.js`.

- Four hex digits are the two raw bytes the hub reads, read as Vitosoft reads
  them: `0x00F0` low, `0x00F1` high, signed. `1E00` is 30; the `FF FF` of a
  unit without F0 is -1 and inside no range.
- Up to three decimal digits are the value as Vitosoft writes it, as a person
  types it from a token or a catalog (`--f0 30`, or the finder's F0 field).
- The value is compared with the decimal ranges; the answer names it
  ("F0 match (F0 30)"). Without F0, or when nothing matches, the lowest-F0
  variant is shown and the others listed ("F0 decides: generic variant
  shown").

Every other family, the reference `VScotHO1_72` (SW `0x51`) included, is
decided by `0xF8..0xFB` alone, and F0 is ignored there.

The load-bearing fact for anyone **adding new datapoints**:

- **A unit is identified by a tuple, not the 2-byte ID.** `0x20CB` = group `0x20`
  / controller `0xCB` = the *family* `VScotHO1`. The software distinction
  (`VScotHO1` vs `VScotHO1_72`, "ab Softwareindex 72 / Projekt Neptun") is the
  **software index at `0xFB`** — it is *not* encoded in `0x20CB` and cannot be
  recovered by decoding `0x20CB` differently.
- **`VScotHO1` is a Vitosoft internal token, not a marketing name.** The product
  name is not in the token; it is a table extension of the type
  (`ProductName` / `ShortName`, §8l), and for about half the types only. On the
  reference unit `VScotHO1_72` ("Projekt Neptun") is the
  **Vitotronic 200 controller in a Vitodens 300-W (type B3HA)** — corroborated by
  the `Neptun_Durchfluss_*` / `Neptun_Volumenstromgrenzwert_*` flow-sensor
  datapoints in its catalog, a Vitodens-300-W feature, and by Vitosoft's own
  `ProductName`: "Vitodens 3xx-W/F mit Vitotronic 200 (Typ HO1C), ...". Its fault texts now come
  from Vitosoft itself (§8i); the hand-curated Vitodens 300-W B3HA set
  (`fault_codes.VITOTRONIC_VD300_B3HA`) is the fallback for exports without
  them.
- **Authoring a new datapoint from the Vitosoft export → match your unit's
  `0xFB`.** Address and conversion can differ between `VScotHO1` and
  `VScotHO1_72`; picking the wrong variant row is a **silent-wrong-decode trap**.
  (Existing datapoints pinned to bytes this physical unit returned are immune —
  the wire is ground truth and the software index is fixed at runtime.)
- **Any future auto-identify feature must match the full tuple** (`0xF8`–`0xFB`,
  plus `0x00F0` for the two families F0 subdivides, above), never `0x20CB` alone, or it loads the wrong datapoint set
  for any family with software-index variants.

---

## 7. Error-history layout

The `esphome_vitoconnect` config reads 3 bytes at `0x7507` as
`[code, day, month]`, a guess that is **wrong**. The layout comes from
InsideViessmannVitosoft's own decoder
(`Viessmann2MQTT.py`, `DateTimeFromBCD`): a slot is **9 bytes** — `[0]` = error
code, followed by an 8-byte packed-BCD timestamp:

| byte | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 |
|------|---|---|---|---|---|---|---|---|---|
| field | code | year-hi | year-lo | month | day | weekday* | hour | minute | second |

\* weekday is counted as the controller's clock counts it -- `0x07` for a Sunday
in the `0x20CB` records, i.e. `monday = 1 .. sunday = 7` (see the clock sync,
section 5) -- and is ignored on decode.

`decode.h::decode_datetime_bcd()` validates that every nibble is BCD (so
`0xFF`-filled empty slots fail cleanly), range-checks the fields, and rejects
implausible years (`< 1990`) so an all-zero slot doesn't decode as year 0. The
`error_history` text sensor emits `"<label> (0x<code>) @ YYYY-MM-DD HH:MM:SS"`,
falling back to code-only when the timestamp is empty/invalid.

**Error history is catalog-driven, and a unit can carry TWO DIFFERENT
archives, not two addresses for one** (`_is_error_history` in
`scripts/vitocatalog/special.py`). Hardware-confirmed on VScotHO1_72:

- `ecnsysEventType~Error` at **`0x7507`** is the Vitotronic **system** fault
  history — BlockLength 90 / BlockFactor 10, i.e. ten 9-byte slots at
  `base + i*stride`, slot 1 newest ("Letzter Fehler", enabled; slots 2..10
  disabled). This is the log the Bedienteil shows and vcontrold's
  `getError0..9` read, and the one the `--error-code-set` map applies to.
- `FehlerHisFA01..20` at **`0x7590..0x763B`** is the **Feuerungsautomat (GFA
  burner control unit)** history — a different subsystem with a different code
  space (`GFA_Kennung` @ `0x7650`; K38 is `KonfiFehlerByteGFA`). Emitted as
  "GFA Fehler NN", all disabled by default, with **no** Vitotronic code map
  attached. Its own texts are keyed by the GFA chip, which the unit reports
  at `0x7650` (`--gfa-chip`, §8i; VScotHO1_72 reads `0x24`).

`gen_catalog.py` emits both when the export carries both, all under
`entity_category: diagnostic`; a genuinely distinct **second system archive**
on the same unit (e.g. the Vitotwin's `0x7000` SW02 archive) gets a
name/seed tag so the two do not collide. The `codes:` map is Vitosoft's own
text set for the device by default (`--error-code-set auto`, §8i); the
hand-curated maps in `scripts/fault_codes.py` -- `openv` (41 codes), `vd200`
(59), `vd300` (94, Vitodens 300-W B3HA) and `union` (105, all merged,
most-specific manual wins), with openv-vs-VD200 disagreements kept in
`fault_codes.CONFLICTS`, not overwritten (see `NOTICE.md`) -- are selectable
explicitly and are the fallback for exports without fault texts. It maps
the display-Stoerungscode space (byte[0]) of the **system** archive only — not
the GFA slots above, the GFA byte (`0x5738`), the LON alarm record, or the
self-describing sensor-status enums.

**Heat pumps.** The WPR and WPR3 fault buffers are remote procedure arrays, not
address ranges: thirty 8-byte (WPR) or 9-byte (WPR3) records at `0xA801`, each
one call with the record index as the parameter. The same slot fan-out emits
them, as raw hex ([p300_protocol.md](p300_protocol.md#remote-procedure-calls-access-rpc)).
What the export says about the record, and what it does not:

- **Not this layout.** `MappingType`, whose value list `DPDefinitions.xml`
  carries (`NoMap=0; PhaseCT=1; HydroCT=2; ErrorHistory=3;
  WPRErrorHistory=4; ...`), is 3 for the system archive above (and the BHKW's
  10-byte one) and 4 for both heat-pump buffers -- WPR3's 9 bytes included.
  Vitosoft decodes them with a mapping of its own.
- **Code and time.** Its fault-buffer view has three columns for every buffer
  (`viessmann.errorbuffer.header.code/.message/.time`: Fehlercode, Meldung,
  Zeitpunkt), so a record holds at least a code and a timestamp.
- **Texts exist.** The heat pumps have their own fault texts, one-byte codes
  (`viessmann.errorcode.CU401B_S.*` 158, `V200WO1A.*` 110, `VBC700_AW.*` 218).
  They are not attached yet: the slots are raw, and the generic Vitotronic set
  is not used for them (`ErrorType` 3/6, §8i).
- **The byte positions are not in any file.** One captured record with the
  controller's own reading of it settles them (§11); the decoder then belongs
  in `error_history` as a second layout chosen by `MappingType`, not in a new
  converter.

> **Hardware status.** The 9-byte slot read of the system archive is
> **hardware-confirmed** on the reference unit (slot 1 @ `0x7507`, newest
> first). Slot reads of the GFA archive at `0x7590..` are **model-derived** —
> the layout comes from the export and the KW/P300 read shape is the same
> proven 9-byte transaction, but nobody has confirmed a GFA slot on this unit
> yet. If a device NAKs a slot read, drop `length:` to 1 for a code-only
> sensor.

---

## 8. The Vitosoft data export (2026, empirically verified)

Per-device datapoint definitions are **not discoverable over the bus** — they
live in Viessmann's Vitosoft data, surfaced by the InsideViessmannVitosoft
scripts and consumed by `scripts/gen_catalog.py` (see
[`scripts/README.md`](../scripts/README.md) for the workflow). The 16-bit
Optolink address is the hex **after the `~`** in an event address string
(`Outside_Temp~0x0800` → `0x0800`); the token before is just the internal name.

Concrete shape of a full 2026 export, recorded in case it shifts again:

- **`DPDefinitions.xml`**: ~203 MB, UTF-8 with BOM. One `ImportExportDataHolder`
  containing one `ECNDataSet` (~589k rows across 20 tables) **plus** a
  `DocumentServerDataSet` (mobile-client / error-code extensions). Parses in ~27 s
  at ~1.6 GB peak via a DOM; prefer a streaming parse on a low-RAM target.
- **`Textresource.xml`**: UTF-16 LE, consolidated multi-culture (15 cultures,
  `de` = CultureId 1) — but **UI strings only** (~350 per culture). In the
  definition export, `Textresource_de.xml` is a byte-identical copy, not a
  German subset. The *full* per-language files are a different thing (§8i).
- The installer ships **stale** XML; it is regenerated from the embedded SQL
  database on first launch of Vitosoft. Verify against a freshly-generated export.

### The operational catch: display names are not in the definition export

(Superseded in practice by §8i: the names are in the Vitosoft installation's
full `Textresource_<culture>.xml` files. What follows still holds for the
definition export on its own, and is what the generator falls back to.)

Every event, value and group references its display name via an
`@@viessmann.*.name.*` label, but the strings those labels resolve to are
**absent from the 2026 export** — Textresource carries none of them, and there is
no localization table anywhere in DPDefinitions (only `ecnCulture`, the language
list). They were most likely moved to Viessmann's cloud/DocumentServer at
runtime; only ~125 system/RPC events carry real localized names. What every
datapoint *does* still carry is the full technical spec: a stable technical
identifier (`Outside_Temp`, …; ~10,781 of 11,582 are clean ASCII tokens) plus
address, length, conversion, FCRead, access mode, the value/enum structure,
units and borders.

**Naming strategy — drive the two ESPHome fields independently:**

- **`id:` ← technical identifier.** Deterministic, ASCII, stable across firmware;
  the join key back to the Viessmann data. Always carried, even when a friendly
  name exists.
- **`name:` ← friendly name.** Use a real translation where available; otherwise
  derive from the id by turning `_` into spaces. The derivation is deliberately
  light (cleans snake_case, leaves camelCase / coding-prefix compounds intact),
  so a real localization source (the openv wiki, or a recovered cloud one) is
  still preferable where clean labels matter. The catalog generator always prints
  the technical id alongside the friendly name, so the id is never lost.

**Enum *option* labels are a separate, mostly-present source.** The above is about
entity *names*. The readable text for individual enum **values** lives in
`ecnEventValueType.Description` (a pre-resolved string needing no Textresource
lookup — e.g. `0x00` → "OK", `0x02` → "Unterbrechung"), which the generator now
reads. ~87% of this device's enum option values carry a Description; the rest are
genuinely blank in the export and fall back to the resolved token stem then hex.
So an enum/`select`'s `options:` are human-readable even though the entity name
beside them may be a derived snake_case label. (The width an enum option reads is
derived from its values, not the block length, so a 1-byte status field inside a
larger block reads 1 byte at its `BytePosition`.)

---

### Bit numbering in the export is MSB-first

`BitPosition` counts from the **most significant** bit: index 0 is `0x80`, index 7
is `0x01`. The generator emitted `1 << (bit_pos % 8)` — mirrored for *every*
bit-field it has ever produced.

Hardware proof (VScotHO1_72 / `0x20CB`, 2026-07-09 logs). Address `0x55DD` carries
exactly two datapoints: `GWG_Flamme1` (BitPosition 2) and `GWG_Brenner_2`
(BitPosition 5). The byte reads `0x01` with the burner off (3 samples, modulation
0 %) and `0x21` with it firing (5 samples, modulation 11–40 %, flue gas rising).
Bit `0x04` is never set in either state. LSB-first would therefore mean "no flame
while the boiler burns", and would light a second burner stage this modulating
unit does not have. MSB-first puts `GWG_Flamme1` at `0x20`, which tracks the burn
exactly.

**Independently confirmed on a second register** (2026-07-10 log). `0xA152` is a
2-byte relay bitfield with sixteen documented `BitPosition`s. Read little-endian
(`>>> F7:A1:52:02` → `<<< 04:00` → `4`), the four observed states decode
MSB-first into a coherent picture and LSB-first into nonsense:

| raw | MSB-first | corroborated by |
|---|---|---|
| `0x0004` | UV_Warmwasser | `Umschaltventil` = "Richtung Warmwasser", HKP off |
| `0x0024` | + Interne_Pumpe | `0x7660[1]` = 10 %, `0x7663[1]` = 0 % (pump overrun) |
| `0x20B0` | Zubringer + Interne + UV_Heizen + HKP1 | `Umschaltventil` = "Richtung Heizen", burner off |
| `0x20B2` | + Brenner | `0xA305[1]` = `0x01`, modulation 11–15 % |

Under LSB-first, `0xB2` raises `Sammelstoerung` and leaves `Brenner` clear while
the boiler demonstrably fires.

Consequence: `HK_Frostgefahr_aktivA1M1` (BitPosition 135, i.e. byte 16 of the
22-byte block at `0x2500`) is mask `0x01`, not `0x80`. Reaching it also required
`binary_sensor` to accept a **block read at the base plus `byte_offset`** — the
same aligned-read pattern `sensor`/`text_sensor` already used, because a single-byte
read at the interior address `0x2510` is unaligned and NAKs on P300 (and returned a
constant `0xFF` on KW).

Every bitmask this generator emits from Vitosoft data is a *derivation*, not an
observation. Confirm each against hardware before trusting it.

---

### 8b. Two ESPHome traps the template `water_heater` sets

Both source-confirmed against ESPHome tag `2026.6.5`, both hardware-confirmed in
the 2026-07-10 log.

**`visual:` is required, not decoration.** `WaterHeaterTraits` initialises
`min_temperature_` and `max_temperature_` to `0.0f`, `TemplateWaterHeater::traits()`
never sets them, and `WaterHeaterCall::validate_()` clamps the requested target
into `[min, max]`. Omit the `visual:` block and *every* set from the Home
Assistant card arrives as `0.00 °C`. The vitohome `number` entity's own
`min_value` caught it — `[W][number]: 'Bedien WW Solltemperatur': 0.000000 < min
10.000000` — so nothing reached the boiler, but the slider was inert.

**`optimistic: true` is required too, for a different reason.**
`TemplateWaterHeater::control()` stores the commanded target only
`if (this->optimistic_)`, and `set_trigger_` is a `Trigger<>` with no arguments —
the call object never reaches the automation. With `optimistic: false` the
`set_action` reads the *previous* target and the usual `!= number.state` guard
suppresses the write entirely. The two bugs compound: fixing only the second one
turns a silent no-op into a rejected `number.set(0)`.

**`isnan()` is the wrong "no state yet" test.** `esphome::number::Number` and
`esphome::sensor::Sensor` both declare a bare `float state;` with **no
initialiser**. Before the first successful poll it reads `0.0`, not `NAN`. A
`current_temperature`/`target_temperature` lambda guarded with `std::isnan()`
therefore publishes a spurious `0.00 °C` at boot. Use `has_state()`.

---

### 8c. P300 read length

Moved: [p300_protocol.md](p300_protocol.md#read-length).

### 8d. GWG addresses a single byte

Moved: [gwg_protocol.md](gwg_protocol.md#wire-format).

---

### 8e. vcontrold numbers bits LSB-first; Vitosoft numbers them MSB-first

Two conventions coexist in the ecosystem, and mixing them is almost certainly
how `1 << (bit_pos % 8)` got into the generator in the first place.

vcontrold's `<bit>N</bit>` is **LSB-first by construction**. Its `Bitstatus`
unit is defined as `icalc get="(B0 & (0x01 << BP)) >> BP"` — mask `0x01 << BP`.
Those `<bit>` numbers are hand-curated in vcontrold's own `vito.xml`.

Vitosoft's `BitPosition` is **MSB-first** (index 0 = `0x80`), hardware-proven
twice: see §8 above.

They are different numbers for different data sources. **Never copy a `<bit>`
value from vcontrold into a vitohome `bit_mask` and vice versa.** vcontrold's
`getBrennerStatus` for device 2053 uses `<bit>1</bit>`, which under its own
convention is mask `0x02` — under Vitosoft's it would be `0x40`.

---

### 8f. The export contradicts itself on 5 bit datapoints

`BitPosition` is normally the absolute bit index across the block, so
`byte = BitPosition // 8`. Where the export also gives a non-zero
`BytePosition`, the two agree for 146 of 151 informative single-bit rows. The
five that disagree:

| datapoint | BitPosition | implies byte | declares BytePosition | FCRead |
|---|---|---|---|---|
| `nviConsumerDmd_Attribute1_CFDM~0xA385` | 24 | 3 | 2 | `Virtual_READ` |
| `nvoConsumerDmd_Attribute1_LFDM~0xA346` | 24 | 3 | 2 | `Virtual_READ` |
| `OT ID0 LowByte Bit 10` | 1 | 0 | 1 | `OT_Physical_Read` |
| `OT ID0 LowByte Bit 11` | 2 | 0 | 1 | `OT_Physical_Read` |
| `OT ID0 LowByte Bit 12` | 3 | 0 | 1 | `OT_Physical_Read` |

The three `OT` rows are byte-relative and are filtered out as unreachable
anyway. The two `nvo`/`nvi` rows are `Virtual_READ` and **were reaching
catalogs**, with `byte_offset: 3` derived from `BitPosition`. We cannot tell
which field is right, so the generator now emits a comment instead of an
entity that might silently read the wrong byte. 54 catalogs lost 2 entities each.

---

### 8g. Never fabricate an address from `BytePosition`

`Beschriftung_HK1~0x7360` is `BlockLength 42`, `BytePosition 2`, `ByteLength 40`.
The generator's string path computed `address = 0x7360 + 2 = 0x7362` and read 40
bytes. **`0x7362` is not a datapoint.** It is byte 2 of the block — an unaligned
interior read, the exact failure mode this component reads blocks at their base
to avoid.

Hardware, 2026-07-10, P300, via the raw scan console:

```
>>> 41:05:00:01:73:62:02:DD          (read 0x7362, TWO bytes)
<<< 06:41:06:03:01:73:62:01:01:E1    (MessageIdentifier 0x03 = Error)
```

Byte-identical to the error the 40-byte read produced. **The width was never the
problem.** On KW the same address returns `0xFF` fill, which `decode_utf16()`
turns into an empty string — so the entity published `""` and looked like an
unnamed heating circuit rather than a bug. That silence is why it survived so
long, and it is pinned in `tests/native/proof_string_offset.cpp`.

Blast radius: 72 entities across 28 catalogs, every one a `Beschriftung*`
(the only seven string datapoints in the export with `BytePosition > 0`).

Fixed on both sides:

* `text_sensor` types `ascii` and `utf16` accept `byte_offset` +
  `byte_length`, the same aligned-block shape `sensor`, `binary_sensor` and
  `enum` already used. Without `byte_offset` the schema is unchanged.
* the generator (`scripts/vitocatalog/`) emits `address: <block base>`, `length: <block>`,
  `byte_offset`, `byte_length` — and a **comment instead of an entity** when the
  block read would exceed `MAX_TEXT_BLOCK_LENGTH`. It performs no address
  arithmetic anywhere.

**Confirmed the same day.** The raw scan console read the block base:

```
>>> 41:05:00:01:73:60:16:EF
<<< 06:41:1B:01:01:73:60:16:00:0B:48:00:65:00:69:00:7A:00:6B:00:72:00:65:00:69:00:73:00:20:00:DF
```

22 bytes, `MessageIdentifier 0x01`, checksum verified. Bytes 2..21 decode to
`"Heizkreis "`. The base answers; only `base + BytePosition` is rejected. Those
exact bytes are now an assertion in `proof_string_offset.cpp`.

A 32-byte read of the same block, minutes later, resolved the leading bytes:

```
<<< 06:41:25:01:01:73:60:20:00:0B:48:00:...:20:00:31:00:FF:FF:FF:FF:FF:FF:FF:FF:1C
```

Byte 1 = `0x0B` = **11**, and the field decodes to `"Heizkreis 1"` — exactly 11
characters, followed by four `0xFFFF` fill units. Byte 1 is the label's character
count. **Hardware-confirmed**, no longer inferred. Bytes 0..1 are declared by no
datapoint in the export; the block layout is:

| bytes | meaning |
|---|---|
| 0 | `0x00` |
| 1 | character count |
| 2..41 | UTF-16LE label, 20 code units, unused slots `0xFFFF` |

The general rule, now enforced in every platform, in the generator, and by a
sweep test (`test_no_emitted_address_is_ever_block_base_plus_byte_position`):
***`Address` is the block base. `BytePosition` is an offset into the response,
never into the address.*** The generator contains no `addr + byte_position`
arithmetic at all.

#### The same bug on the write side

The invariant had a second violation, larger than the first. A writable field at
`BytePosition > 0` was emitted as `address: <base + BytePosition>` (the write
target) plus `state_address: <base>` (the aligned read) — **402 entities across
124 catalogs**, including `0x7661`, the address these very notes record as NAKing
on P300.

Checked against the export: of the 360 writable datapoints with
`BytePosition > 0`, `base + BytePosition` is a declared address for only **144**,
and where it *is* declared it generally belongs to an unrelated datapoint of
another device family. There is no evidence that any of them is the field's write
register.

A read of a non-existent address is a NAK. A **write** to a wrong-but-existing
address changes something. So those entities are now **demoted to read-only**:
the block-base read survives (it was always correct), the invented write target
is gone, and each carries `# NOTE: field at BytePosition N has no declared write
address -> demoted to read-only`. `COMMAND_STATE_ADDR`, which supplies real
declared state addresses, is unaffected — and no datapoint in the export combines
it with a non-zero `BytePosition`.

#### Narrow writes at the block base

The same principle has a third case, found on hardware 2026-10-14 (§5b): a
write at the block base that is NARROWER than the block. The device writes its
whole datapoint and fills the bytes after our payload from the telegram -- the
checksum, at least. 60 writable entities across P300 catalogs had that shape
(Vitotwin setpoints, 2 bytes of a 3-byte block; `UhrzeitJH`, 1 byte of the
8-byte clock block; selects on byte 0 of 2-byte blocks). Every writable field
with `ByteLength < BlockLength` was demoted to read-only with a `NOTE`. The
weekday Schaltzeiten are the documented exception: vcontrold writes 8 bytes per
day into the 56-byte program, and that path never reaches this rule.

#### Both cases now: block mode

The demotions of the two write-side cases above -- a field at
`BytePosition > 0`, and a field narrower than its block at the base -- are
replaced by block mode (§5b): address = the block base, `length` = the block,
`byte_offset` = the field, written as a whole-block read-modify-write. The
invariant holds unchanged: no address is ever computed from `BytePosition`, and
a write always covers the whole datapoint. A field stays read-only, with the
reason in its `NOTE`, where block mode is not safe or not possible: the block
also holds a read-only datapoint (live data), the block exceeds
`MAX_RMW_BLOCK`, the field is wider than 4 bytes, or it is a
`COMMAND_STATE_ADDR` command register. `--no-partial-writes` (older spelling
`--no-bitfield-writes`) restores the read-only behaviour for every field
smaller than its datapoint.

#### The padding is byte-oriented

The full 42-byte read also settled the block layout, and exposed a decode bug:

| bytes | meaning |
|---|---|
| 0 | `0x00` |
| 1 | character count (`0x0B` = 11) |
| 2..41 | UTF-16LE label, 20 code units |

The fill after the label is **thirteen** `0xFF` bytes, then five `0x00` — an odd
run, not code-unit aligned. Code unit 17 is therefore `0x00FF`, and
`decode_utf16()`, which *skipped* `0xFFFF` fill, walked straight into it and
published `"Heizkreis 1ÿ"`. It now **terminates** on `0xFFFF`: U+FFFF is a Unicode
noncharacter and cannot appear in a label, so a fill slot means the string is
over. Regression-pinned in `test_decode.cpp` and `proof_string_offset.cpp`.

Beware the address space is **overloaded across device families**. On heat-pump
(`WPR`) tokens, `0x7360`, `0x7361` and `0x7362` are three separate one-byte
datapoints; on boiler tokens `0x7360` is a 42-byte label block. The three label
blocks are contiguous — `0x7360`, `0x738A`, `0x73B4`, spaced exactly
`0x2A` = 42 apart — which is itself a check on the declared `BlockLength`.

One row in the export is worth knowing about: `WPR_S_Endzeit_Nachtbetrieb~0x7162`
carries `<Address>0x7362</Address>`. Its ID token and its `Address` field
disagree. The generator trusts `Address`, and that datapoint (1 byte,
`BytePosition 0`) is unrelated to the label block.

---

### 8h. ParameterType is a second signal for signedness, never the only one

Every row of the export declares a value type in `Parameter`: `Byte`, `SByte`,
`Int`, `SInt`, `Int4`, `SInt4`, the `*HighByteFirst` variants, `Array`,
`String`... The generator ignored it, and decided signedness from the converter's
default (`div2`/`div10` signed) and a negative lower border.

That is wrong for **400 reachable entities across 162 catalogs**: declared
unsigned, decoded signed. It bites above half the range, which for a 1-byte
`Div2` temperature is 63.5 degC -- `GWG_Kesseltemperatur` (0x0070, `Int`,
0..127.5) at 70 degC is raw `0x8C`, published as -58 degC. A writable sibling
fared worse: `GWG_Trinkwasser_SolltemperaturBEM` (`Byte`, `Div2`, 10..90) has an
upper border of raw 180, which does not fit a SIGNED byte, so the number fell
back to the 0/0/1 placeholders.

`ParameterType` alone is not the answer either. It contradicts the borders on 27
rows, among them the classic outdoor sensor `ADC_IstTemperaturwert_ATSV333MW1
~0x0800` -- declared `Int`, bordered -60..60. A type-only rule publishes 6553.5 degC there at -0.1 degC.

So the rule needs both signals to agree: `signed: false` is emitted on a
`div2`/`div10` datapoint only when the type is `Byte`/`Int`/`Int4` **and** the
lower border is present and >= 0 (`_declared_unsigned()`). That fixes 188
distinct datapoints. The remaining 212 keep the signed default deliberately:
210 have no borders at all (mostly raw ADC temperature channels, which can go
negative) and 2 have a negative border. The `*HighByteFirst` types are left
alone: they also reverse the byte order, which no reachable `div2`/`div10`
datapoint needs.

### 8i. Vitosoft texts: languages, units, fault texts

The display names §8 calls absent exist -- not in the definition export, but in
the Vitosoft installation: `Textresource_<culture>.xml`, one language per file,
18 languages (`cs da de en es fr hr hu it lt nl no pl ro ru sk sv tr`; culture
ids 1..18 as every file header declares them), ~30 MB and 95,625 labels each
(Norwegian 95,596). The copies in the installer's `data\OFFLINE\…\` folder are
byte-identical to an installed one's (2025-07-17). They carry entity names,
option labels, unit labels and 35,640 `viessmann.errorcode.*` fault texts.
The generator layers them per label (`_load_text_layers`, `scripts/vitocatalog/loader.py`): slim
`Textresource.xml` < `Textresource_de.xml` < `Textresource_<culture>.xml`; an
empty translation never hides German (English leaves many option and unit
labels empty).

Four things the texts do NOT get to decide:

- **Units.** Unit labels are translated (`kWh` -> "кВтч" in Russian), empty
  (`ecnUnit.sech` in English), or not symbols at all (`ecnUnit.dBm` is "-dBm" in
  German, "dBm" in English, "#NAME?" in Russian). `scripts/units.py` maps all
  60 unit tokens of the export to one HA/SI symbol and device class, so a
  catalog's units do not depend on its language. "-dBm" is a real signal: the
  RF level datapoints are unsigned `Byte`s, so the stored value is the
  magnitude, emitted as `dBm` with `multiply: -1` (UNVERIFIED: no RF
  participant on the reference unit).
- **Entity types.** `_boolean_pair` (switch vs select) knows German and English
  on/off words. It always decides on the German labels (`_enum_bool_pair`), so
  a French catalog has the same switches as a German one.
- **German option labels.** `ecnEventValueType.Description` stays first for
  German: it is per heating circuit ("1 M2 setzt Anforderung an Int.Pumpe")
  where the UI label is shared ("1: HK setzt ..."), and has no hyphenation
  artefacts ("Betriebsarten-umschaltung"). The German catalog is unchanged by
  this; other languages show their translation.
- **Ids.** `id:` is the technical name in every language.

A key-spelling trap: DPDefinitions names an option
`@@viessmann.eventvaluetype.name.X~0`, the text files key it
`viessmann.eventvaluetype.X~0` (no `.name.`); `_lookup_label` tries both.
Event names carry `.name.` in both places.

**Names must be unique per platform** in ESPHome, and Vitosoft UI names are not
(45 repeats on VScotHO1_72: one "Pegel" per RF participant, the "(23)..(28)"
coding names per circuit). `_make_names_unique` qualifies every member of a
colliding group with its `id:`, comparing names the way ESPHome does
(lower-case, non-`[a-z0-9_-]` to `_`). `/` in a name becomes U+2044, as
ESPHome itself substitutes (an error from ESPHome 2027.7.0).

**Fault texts are per device.** 88 distinct sets across 206 device types; a set
belongs to the Vitosoft token (`viessmann.errorcode.VScotHO1_72.<XX>`, hex code
byte), so each generated catalog carries its own device's texts on its system
fault history (the heat pumps' RPC fault records carry none yet, §7). Coverage over
the 166 catalogs with a system history: 144 have their own set; the other 22
are Vitobloc CHP units and `WILO`. The generic set
(`viessmann.errorcode.<XX>`, 72 codes) is the Vitotronic boiler meaning and is
used only where `ecnDataPointType.ErrorType` is 1 (Default) -- 131 of the 132
such devices have their own set anyway; a BHKW (`ErrorType` 4) gets no map
rather than boiler codes. Against the hand-curated `vd300` map on VScotHO1_72:
Vitosoft has all 94 codes plus 16, and 48 texts differ, several in meaning
(`0x49`/`0x4D` return-temperature sensor vs coding, `0x93`/`0x9B` solar
collector vs storage sensor, `0x1D`).

**GFA texts are per chip.** `viessmann.errorcode.fa.<chip>.<XX>`, 9 chips
(`20`-`26`, `41`, `42`; 8 distinct sets, `41` = `42`). All 53 devices with a
burner control read the chip from `GFA_Kennung` at `0x7650`, one byte, and the
key is that byte in hex -- Vitosoft's own chip table says so
(`Kennung_FA_Chip~36` = "24h - NEC"; the table: `20` GFA, `21` SCOT, `22`
Dover, `23` CES, `24` NEC, `25` ET, `26` Scot eSTB, `27` Vi (no texts), `41`
ÖFA, `42` ÖFA (Vi) -- ÖFA being the oil burner control, which fits the
Ölvorwärmer/BV2 texts of `41`/`42`). Hardware, VScotHO1_72, 2026-10-15:
`0x7650` = `0x24` (`>>> 41:05:00:01:76:50:01:CD`, `<<< ...:76:50:01:24:F3`),
NEC. Nothing in the export ties a device to a chip: `GFA_Kennung` carries no
value list on any of the 53 devices, and the chip table is used only by the
burner-control-side `VSKO_Scot_CES_P80~0x4050` (`GFA_READ`). The sets overlap
heavily -- 60 of the 72 codes they share read the same everywhere -- but 7
codes genuinely contradict between chips (`0x09` LAS vs low water pressure,
`0x12` dLP sensor vs water-pressure sensor, `0x05`, `0x10`/`0x11`, `0x1D`), so
no merged set is safe. The chip is a property of the installed unit, so the
texts cannot live in a catalog. They come as **companion packages**: one small
file per chip and language, `example/catalogs/<lang>/gfa/chip_<XX>.yaml`, which
extends the catalog's twenty GFA slots by id:

```yaml
text_sensor:
  - id: !extend gfa_fehler_01
    codes: &fault_codes
      0x00: "0: Kein Fehler"
      ...
  - id: !extend gfa_fehler_02
    codes: *fault_codes
```

All 63 GFA catalogs of the 2026 export carry exactly `gfa_fehler_01..20`, so
one companion per chip fits every one of them (9 files, 45 KB per language; a
whole per-chip copy of each catalog would have been ~500 files and ~110 MB per
language). The hub logs the chip at boot (`GFA=0x24`, §6) and puts it into
the finder link. `esphome config` resolves every companion against VScotHO1_72 in
both languages and against five catalogs of other families (Vitoplus,
Vitovalor, VSorp, Vitodens, and a `V300…` unit that has the GFA slots but no
`GFA_Kennung`): twenty entities, codes merged, no warning. The finder offers
the companion when a GFA chip is entered or arrives as `&gfa=` in the link;
`--gfa-chip` still bakes one chip into a single generated catalog. The YAML
anchor keeps the file small, but after the merge each entity holds its own
copy of the map -- see *Shared option tables* below. The GFA texts keep Vitosoft's decimal
prefix ("10: Flammenausfall ..."); three of them carry a wrong one in the
source (`20.8C` "8c: ...", `24.8A` and `26.8A` "139: Fehler 8Ah"), passed
through unchanged.

**Shared option tables.** `emit_option_table` writes each `codes:`/`options:`
map as a `static const VitoOption[]` (guideline A1). A catalog repeats one map
on many entities -- the ten system fault-history slots, and with a companion
the twenty GFA slots -- and every copy cost 8 bytes per row of `.rodata` (the
label strings themselves were already merged by the compiler). Identical
tables are now emitted once and shared, keyed by their rows in `CORE.data`
(guideline D1). VScotHO1_72 + the chip-24 companion, ESP-IDF build: flash
400,303 -> 379,087 bytes (-21.2 KB), 28 distinct tables left. The same build
surfaced a C++ trap in the texts: GFA code `0xFF` reads "Fehlermeldung
vorhanden (unbekannt/neu??)", and `??)` is a trigraph -- GCC warned once per
table. `cpp_string_literal` now escapes every `?`.

The YAML had the same repetition: ten 110-line copies of the device's map per
catalog. The generator now writes a repeated `codes:` block as an alias of its
first occurrence (`codes: &letzter_fehler_codes` ... `codes:
*letzter_fehler_codes`), decided in output order after sorting so an alias
never precedes its anchor (`_share_codes_blocks`). VScotHO1_72 with
`--gfa-chip 24`: 379 KB -> 263 KB, the committed set 33 MB -> 28 MB per
language; `esphome config` resolves all 30 slots to their full maps (110
system codes, 72 GFA).

### 8j. A device's datapoints come from two tables, not one

Vitosoft attaches datapoints to a device in two places:

1. `ecnDataPointTypeEventTypeLink` -- the direct device -> datapoint table;
2. the device's **menu tree** -- `ecnEventTypeGroup` rows carrying the
   device's `DataPointTypeId`, with datapoints hung on them by
   `ecnEventTypeEventTypeGroupLink`. Vitosoft shows a device through these
   menus.

In the 2026 export **150 devices carry datapoints only in their menus: 2,854 in all,
2,685 Optolink-reachable, 2,448 at an address the device has no other
datapoint for** (the other 400 are alternative names for an address it has,
which the field de-duplication folds). Examples: `V200CO1x` lacked its whole
`(A0)..(B5)` heating-circuit coding block (`0x27A0..`, 161 datapoints);
`CU401B_S` its compressor run hours (`0x0580`) and annual performance factors
(JAZ, `0x1680..0x1682`); VScotHO1_72 one (`nviBoilerCmd_BLR_value`, `0xA300`).
`Catalog.events_for()` takes both (`--no-menu-datapoints` reads the direct
table alone); the menus add 2,404 entities to 130 of the 201 catalogs
(standard profile).

The menus are strictly per device, not shared templates: each of the 18,018
groups carries exactly one `DataPointTypeId`, and its path names that device --
its own token (11,901), `ecnsysEventTypeGroup~<token>~...` (5,891, the system
menus such as the GFA error history), or a variant of the token (226, e.g.
`V050HK3W_9` under `V050HK3W`). A datapoint DEFINITION may sit in several
devices' menus (6,691 do), just as it may be linked to several devices
directly.

**What it does not explain -- issue #19.** The Vitocal 222-S (`CU401B_S`)
report lists three kinds of addresses "not in the catalog":

- `0xB404..0xB424` and `0x130B` are linked to `CU401B_S` directly in the 2026
  export and are in its catalog; the report used older (2017) data.
- `0x0710` exists only as an unrelated KBUS datapoint.
- the refrigerant block `0x0E03..0x0E74` (suction, superheat, evaporating and
  condensing, hot-gas, inverter IPM temperatures) exists **nowhere**: not in
  any XML of the export, not in the menu tree, not in the 825 MB SQL database,
  in neither UTF-8 nor UTF-16. Vitosoft does not know these addresses; they
  come from the reporter's own scan and belong in `example/user_curated/`.

The other XML files of a Vitosoft installation (`Checklist.xml`,
`ErrorTree.xml`, `DocumentIndex.xml`, `SparepartManagement.xml`,
`deviceSchema(ta).xml`, `coding1Multi.xml`, ...) are development fixtures from
2009 -- a checklist named "Blubber", "Knoten 1" placeholders, 0.00 EUR spare
parts -- and carry no datapoint knowledge.

### 8k. `standard` leaves out LON inputs

`standard` leaves out the datapoints with a conversion the generator cannot
decode (`DateBCD`, `DateTimeBCD`, `IPAddress`; `full` turns them into
comments) and **LON network-variable inputs**
(`nvi[A-Z]*`, 3,606 datapoints: 3,565 linked directly, 41 via the menus).
The controller mirrors its LON objects into memory, so they answer over
Optolink -- VScotHO1_72 reads `0xA300` (`nviBoilerCmd_BLR_value`) as `00 FF` --
but an input is written by a LON master; without a LON module it never holds
data, and `0xFF` is LON's "invalid" marker. Outputs (`nvo*`, e.g. `nvoAlarm`
at `0xA132`, which carries real state) and configuration (`nci*`) stay. 1,389
of the inputs are writable, and writing one over Optolink may stand in for a
LON master, so `full` keeps them; the catalog header says how many were left
out. `minimal` leaves them out too.

The same profiles leave out the **LON-bound remote procedure readings**
(`access: rpc`, [p300_protocol.md](p300_protocol.md#remote-procedure-calls-access-rpc)): the participant list at
`0xA010` on 105 device tokens, the Vitocom LON identity, `vlon*` and LON inputs
read by procedure. Without a LON module they are still answered, but hold
only the unit's own defaults and empty slots (VScotHO1_72, 2026-10-21: own
number `01 01`, entry `01` empty), which carries no information. `full` keeps
them. The unit's own readings (boiler sequence, M-Bus module state) stay in
`standard`.

### 8l. Table extensions: what Vitosoft keeps outside its columns

Vitosoft adds fields to its tables without new columns: `ecnTableExtension`
names a field (table, field name, data type) and `ecnTableExtensionValue` holds
one value per row (`RefId` = the field, `PkValue` = `"<row Id>;24"`), as Base64
of a .NET BinaryFormatter stream -- a string, or a boxed `Int32` / `Double` /
`Boolean` (`loader._decode_ext_value`). `DPDefinitions.xml` carries all
248,587 of them, the same rows as the SQL Server database Vitosoft installs, so
the generator needs no database. `ecnEventType.xml` and `ecnDataPointType.xml`
are flattened copies of some of these fields (Address, FCRead, BlockLength,
Identification, F0, ...), and miss the rest. What the catalog uses from the
extensions (`_WANTED_EXTENSIONS`):

| Field | Use |
|---|---|
| `ecnEventType.FunctionValue` | the parameter of a procedure without `PrefixWrite` ([p300_protocol.md](p300_protocol.md#remote-procedure-calls-access-rpc)) |
| `ecnEventType.Option` | `Schnellcheck` -> the `quickcheck` tag |
| `ecnEventTypeGroup.LogicalGroupMapping` | a menu's circuit -> `circuit=` tag |
| `ecnDatapointType.ShortName` / `ProductName` | the catalog header, `index.csv`, `resolve.json`, the finder |

**Metadata tags.** `Catalog.tags_for` writes one `# vito:` line above every
entity Vitosoft hangs in one of the device's menus, which the editor filters
on: `circuit=` from `LogicalGroupMapping` (Vitosoft sets it on the menus right
below a section, e.g. `VScotHO1_72~35_Information~20_Heizkreis_A1` = 1, and
leaves their sub-menus unset, so a menu inherits its nearest parent's; an
explicit 0 means none), and a flag for each of the device's own system menus
(`ecnsysEventTypeGroup~<token>~<kind>`) that carries one: `Overview` (Vitosoft's
overview screen), `Trending` (its trend recorder) and `CodeAccessLevelTD`
(shown at the service access level only, readings and writables alike: 174 + 49
on VScotHO1_72). On VScotHO1_72, 514 of the 699 `standard` entities get a tag.
Not used yet: `CodeLevelArea` on menus (TD on 1,492 of them; the device's own
TD list is taken instead),
`VIAAddress` (one name per meaning across all devices, e.g.
`konf_raumsolltemp_rw`, which could wire climate entities per circuit), and the
display conditions (`ecnDisplayCondition*`, 5,737 rules that show or hide menus
by coding values such as `K00` Anlagenschema). Which way a condition type and
operator act is not settled, and the rules are partly shared across device
families -- on VScotHO1_72 they reference an Ecotronic solar address -- so they
would be hints at best.

### 8m. Weekly programs by MappingType

Vitosoft names the reader it uses for a block in `MappingType` (an
`ecnEventType` table extension, flattened into `ecnEventType.xml`). Its weekly
programs come in three shapes, all 7 days:

| MappingType | Events | Block | Addressing | Catalog |
|---|---|---|---|---|
| 1 PhaseCT | 25 (boiler `Schaltzeiten_*`, `Vitotwin_*_Zeitprogramm_*`, Ecotronic, OpenTherm) | 56 bytes, 7 records of 8 | per byte: day n at base + n*8 | `text`, decoded and writable (converters.md, *Schaltzeiten*) |
| 2 HydroCT, 10 HydroPhaseCTLF | 5 + 1 (`WPR_Tagesprogramm_*` at 0x9000..0x901C, `HO2B_Lueftung_Schaltzeiten`) | 168 bytes, 7 records of 24 | per record: day n at base + n | read-only `type: raw` per day |
| 5..8 HydroPhaseCT(WW/ZP/PSP) | 10 (`NKU_Tagesprogramm_*` 0x9200..0x9318, `WPR3_*` 0x9350..0x93F8) | 168 bytes, 56 records of 3 | per record: day n at base + n*8 | read-only `type: raw` per day |

The heat-pump programs are addressed per **record**, not per byte: the export's
next program starts BlockFactor addresses on (HydroCT `0x9000`, `0x9007`,
`0x900E`, ...; HydroPhaseCT `0x9200`, `0x9238`, `0x9270`, ...), 7 or 56 where
a byte-addressed 168-byte block would need 168. So a day is BlockFactor / 7
records from base + day * (BlockFactor / 7), 24 bytes either way, one read
(`_hydro_schedule_shape`). The export does not describe what a record holds,
so these days are emitted as raw hex, disabled and never written; whether a
unit answers a 24-byte read at a record address is unconfirmed. PhaseCT now
decides the boiler path by itself, so programs not named `Schaltzeit` are
decoded too (Vitotwin: 5 programs); without a MappingType (older exports) the
token rule still applies. V200WO1A and its siblings link both the HydroCT and
the HydroPhaseCT programs; which one a unit uses is not in the export, so both
are emitted.

---

## 9. Per-entity polling

The hub has a base `update_interval` (the poll tick). Each entity may set its own
`update_interval`, honoured at **hub-tick granularity** — so an entity interval
shorter than the hub interval silently degrades to the hub interval. Rather than
let that surprise anyone, `setup()` logs a warning naming each such entity.
(There is intentionally no `final_validate` cross-check between entity and hub
intervals: the hub id isn't reliably resolvable from a platform's
`final_validate` without brittle global lookups, so the runtime warning is the
chosen tradeoff.) Suggested tiers: live measurements ~60 s, monotonic counters
~600 s, writable coding values ~3600 s, error history ~300 s.

**Transient-error tolerance.** `sensor` and `number` publish NAN (HA
"unavailable") only after **three consecutive failed reads**; a single CRC
glitch or timeout does not blank an hourly-polled entity until its next
poll, and a successful publish resets the streak. Failed **writes** never
blank state at all — the device value did not change — which is why the hub
dispatches read errors to `handle_error()` and write errors to
`handle_write_error()` (default: keep state) separately.

**No `update_interval` means *every hub tick*.** `poll_interval_ms_` defaults to
`0`, which the scheduler reads as "due on every cycle". That is the right default
for a sensor and a trap for an immutable identity string: `example/` had four of
them (`Sachnummer`, two `HWHerstellNr*`, `Beschriftung_HK1` — 79 bytes) going out
once a minute forever. `disabled_by_default: true` does not help; it is a Home
Assistant hint and the device polls regardless. Always pin an interval on
anything that cannot change.

**The next-due time must be anchored on the schedule, not on `now`.** The
scheduler advances `next_due_ms_` from the *previous scheduled* time
(`components/vitohome/poll_schedule.h`), and treats an entity due within half a
hub tick as due now. Both parts are load-bearing. `now` is `millis()` sampled
*inside* `update()`, a few milliseconds after the interval anchor that invoked it,
and that offset is not constant. Re-anchoring on it (`next_due = now + interval`)
made any entity whose interval *equals* the hub tick fire or not fire depending on
whether this tick's jitter happened to exceed the previous tick's — a coin flip.
Two 2026-07-09 hardware logs, **from the same firmware binary**, disagreed: one
dropped the entire 60 s tier on its second tick, the other never did. A host
simulation with realistic non-monotonic jitter drops 5 of 12 ticks when
re-anchored on `now` and 0 of 12 anchored on the schedule
(`tests/native/proof_scheduler.cpp`, which also pins the drift-free
progression, the long-stall re-anchor, and the `millis()` wrap).

**Poll backoff.** An entity whose reads time out while the link answers others
is polled less often: after `POLL_BACKOFF_AFTER` (3) timeouts in a row at 2x
its interval, then 4x, 8x and at most 16x (`poll_backoff_interval()` in
`poll_schedule.h`). A timeout counts towards the streak only if some other read
was answered since the previous one, so a dead link does not back everything
off. The first answer restores the interval. Every-cycle (0) and
`never`-polled entities are untouched.

---

### 9a. Frame logging belongs in the component, not in `uart: debug:`

ESPHome's UART debugger has no notion of an Optolink telegram, so it has to be
told where a frame ends. The `after: delimiter: [0x06]` recipe that circulates
for Optolink is the **P300 ACK byte**. On KW, `0x06` is an ordinary data byte,
so the debugger splits telegrams mid-frame — visible in the 2026-07-09 logs as
`>>> 01:F4:23:06` followed by `>>> 01:24` for a single write to `0x2306`.

The port (`vito_uart_interface.h`) knows the boundaries and needs no delimiter.
`log_frames: true` on the hub sets `-DVITOHOME_LOG_FRAMES`, and
`vito_uart_interface.h` then logs one line per telegram under the
`vitohome.frames` tag. **Both directions are buffered**, and each is flushed on
the first of: traffic in the opposite direction, an inter-byte gap over 30 ms, or
a full buffer. A write hands its bytes to the UART before it logs anything, so
no log line delays a telegram (logged ahead of the bytes, the line cost 3-6 ms
between ENQ and send on #35).

Buffering TX is needed because one telegram is several writes: the P300
exchange writes the `41`, the body and the checksum one write each, and an
ENQ-paced KW telegram is the `01` and the telegram. The 30 ms flush gap is 12
byte times at 4800 8E2 (1 start + 8 data + 1 parity + 2 stop = 2.5 ms): it
joins the pieces of a telegram, written in one pass, and stays far below KW's
~2.2 s idle ENQ. Whether our `06` and the next telegram share a line depends
on loop timing (`>>> 06:41:05:00:01:76:50:01:CD`): read a leading `06` as the
ACK of the previous answer. For exact gaps use the recorder (§9b).

It is a build flag rather than a runtime setter so a production firmware carries
no buffers and no per-byte branch; `frame_tick()` degrades to an empty inline.

One consequence worth knowing: a frame is logged when it *closes*, so the `<<<`
line lands after the decoded value it produced. The contents are right; only the
interleaving reads oddly.

---

### 9b. The recorder

The frame log merges bytes within 30 ms, has to be on before the failure, and
goes through the logger, which changes the timing it shows. `recorder:` on the
hub sets `-DVITOHOME_RECORDER` and keeps the last bytes and results in a ring
of 8-byte entries (`vito_recorder.h`):

- **Bytes:** from the port (`vito_uart_interface.h`), stamped with `micros()`.
  A write is stamped before it is handed to the UART. A read is stamped when
  the engine takes it from the UART buffer, which may be later than it
  arrived. Each entry also carries the hub's loop pass.
- **Results:** every request's status and address, and a timed frame's
  (`raw_op.h` `~ms`).
- **The link's trace:** each exchange reports to a `LinkSink`
  (`optolink_types.h`, a function and a context, off while unset). It reports
  every change of its state, which the exchange names (`state_name()`), and
  the facts no state shows: bytes drained, a retry, late bytes, bursting off,
  the device's NACK, a refused EOT, the keepalive, KW's idle EOT and the
  watchdog. The differential target runs the live side with a counting sink,
  so the trace changes no byte; `proof_link_trace.cpp` pins what each
  exchange reports.
- **Freeze:** on the first timeout, error or retry (`freeze_on:`), the ring
  takes `after:` more entries, then keeps what it holds. Later failures are
  only counted. With `dump_on_freeze: true` the hub dumps it as soon as it
  is frozen, so an unattended run logs every failure in turn.
- **Dump:** two lines per loop pass under `vitohome.recorder`. One line holds
  the bytes of one direction in one pass, prefixed by `+ms.µs` from the
  oldest entry; `~ <state>` and `* <fact> [<arg>]` lines stand between them
  in the order they happened. A dump of an unfrozen ring freezes it first. The dump re-arms
  the ring.
- **Import:** `tests/native/sim/import_frames.py` reads the first dump of a
  log into a trace for the simulator tests.

ESPHome's `uart/event` takes the bytes off the line itself, and
`add_debug_callback()` sees them at the same moment as the port, one
`std::function` call per byte. No hook gives the time a byte arrived.

---

## 10. Testing and the gate model

The gates are **sequential and non-substituting** — passing an earlier gate does
not vouch for a later one:

```
host C++ : decode/encode tests           tests/native/test_decode.cpp   (916,696 checks)
host C++ : P300 transaction harness      tests/native/test_p300_transaction.cpp (8/8)
host C++ : engine proofs, per protocol   tests/native/proof_{kw,p300,gwg}_*.cpp, proof_hub_requests.cpp
host C++ : differential target           tests/native/fuzz_diff_engines.cpp (live engines vs the reference copy)
host C++ : simulators, field traces      tests/native/proof_sim*.cpp
host C++ : recorder, link trace          tests/native/proof_recorder.cpp, proof_link_trace.cpp
host C++ : fuzz corpora (replay)         tests/native/fuzz_*.cpp  (libFuzzer via fuzz.sh)
python   : validators + catalog generator tests/unit/  (pytest)
lint     : ruff check / ruff format, clang-tidy
format   : clang-format  (pinned v22.1.8)
config   : esphome config   (every tests/test.*.yaml)
compile  : esphome compile  (esp-idf AND arduino, PlatformIO and the native ESP-IDF toolchain, ESP8266)
run      : esphome run       (real heater — the definitive gate)
```

**Every host binary is built with ASan + UBSan.** The engines consume bytes off
a noisy optical link, where garbled RX is the normal case, and that is where
both memory-safety defects this project has found lived (the zero-payload
out-of-bounds write in the P300 parser, and VitoWiFi's RESPONSE payload
guards); both would have been caught by instrumented proofs without anyone
hand-writing the input that trips them. The flag set lives in one exported
`$SAN` variable in `build_and_run.sh` (inherited by the chained protocol
script, so there is no second copy to drift) and on the `test_decode` line in
`ci.yml`.

`-fno-sanitize-recover=all` is not decoration either: UBSan's default is to
print a runtime error and **continue**, leaving the process exit code at 0, so
without it an undefined-behaviour hit is reported *into a green build* — the
same failure mode the `-Werror` note in `build_and_run.sh` describes for
warnings. ASan aborts on its own; UBSan has to be told to.

**No proof sleeps.** Every engine is a `millis()`-deadline state machine, and a
proof that sleeps races what it measures: the tightest window, a GWG answer
deadline of 250 ms, is two adjacent statements in a test but not a safe bet on
a loaded host. `optolink_clock.h`'s host branch is a freezable clock
(`optolink_test_clock_freeze` / `_advance` / `_release`), and every proof
advances it by exact amounts, so deadlines are asserted as equalities. The
suite passed with 400 ms of real sleep injected into every advance,
which showed that it depends on the injected clock and not on the wall clock.
The real-time branch reads `steady_clock`, since a stepped `system_clock`
would break the differences every reading is used for.

Two coverage layers compose to give wire→decode→value end-to-end without the
ESPHome framework:

- **`test_decode.cpp` (916,696 checks, most of them exhaustive sweeps)** locks down the *value* layer — the
  precision fix, BCD/datetime, sign-extend, encode round-trips, `raw_fits`
  boundaries.
- **The P300 transaction harness (8/8)** locks down the *transaction* layer:
  request/ACK/response and **fragment reassembly** (both `0x23xx` reads arrive
  split on hardware). Its fixtures are golden-master vectors lifted from live
  captures; the write vectors are hardware-confirmed Betriebsart switches.
  Detail and the vector table are in
  [`tests/native/README.md`](../tests/native/README.md).

### 10a. What the host harness deliberately cannot reach

The native proofs build **only the engine**, the header-only
`optolink_*.h` files (see `tests/native/build_and_run_protocols.sh`). They never compile `vitohome.cpp`,
`vito_clock.cpp` or any entity, because those include `vitohome.h`, which pulls
in the whole ESPHome component surface (`Component`, `uart`, `binary_sensor`,
`text_sensor`, the platform bases). Testing them on the host means standing up
stubs for all of it — a separate piece of test infrastructure, not an extra
proof file.

The consequence is a real, named boundary rather than an accident. The hub layer
is covered by:

- the compile gate (every `components/vitohome/` translation unit, under
  each protocol build),
- the Python validation suite, for everything decidable at config time, and
- `esphome run` on the real heater.

but **not** behaviourally on the host. The two invariants this leaves unproven
are worth naming, because both are load-bearing and both are documented
elsewhere in this file as obligations rather than as tested facts:

1. **`VitoClock`'s phase recovery** (§5a) — every completion path must return
   `phase_` to `IDLE`, since nothing re-queues the clock. Currently guaranteed
   by the structure of `handle_read_()` (drop to `IDLE` first, re-arm to
   `WRITING` only on the success path) and stated as an editing contract at the
   top of that function.
2. **The hub's in-flight state** — who has the bus is one value,
   `in_flight_op_` (`OpType`: an entity's read, write or read-before-write,
   an identification read, a raw-console operation, or nothing), plus the
   entity for the first three. `set_in_flight_()` and `take_in_flight_()` are
   the only writers, so the pair cannot disagree;
   `tests/unit/test_queue_lock_discipline.py` fails if the hub's source
   assigns either field directly.

3. **Read-modify-write atomicity** (§5b, bit fields and whole blocks) --
   between an `RMW_READ` response and the dispatch of its merged write,
   `dispatch_next_()` dispatches nothing else (`rmw_pending_` is checked
   first), and every `RMW_READ` completion path either arms `rmw_pending_` or
   ends in `rmw_abort_()`. The merged block (`rmw_buf_`) is written whole,
   never a prefix of it. The merge itself is host-tested (`merge_bit_field`,
   `merge_block_field`); its use by the hub is not.
   Guaranteed by the structure of `dispatch_next_()` and stated as an editing
   contract next to `rmw_pending_` in `vitohome.h`.

4. **The RPC dispatch** ([p300_protocol.md](p300_protocol.md#remote-procedure-calls-access-rpc)):
   an `access: rpc` entity goes out as a P300 request with function code `07`
   on the read lane, an action button as the same request on the write lane
   (which is also what lets it queue with no write payload,
   `request_write()`), and `refresh_siblings_()` skips RPC entities. The
   engine side is host-proven (`proof_p300_rpc.cpp`); the hub's choice of
   telegram is covered only by the compile gate.

`proof_hub_requests.cpp` covers the requests the hub builds for each space
and protocol, against the reference engines' calls. If hub-level host testing
is ever taken on, these four are the first things to point it at.

Two limits worth re-stating because they are recurring footguns:

- **`esphome config` ≠ `esphome compile`.** Config validates schemas and runs the
  config-time validators but does **not** run `to_code`/codegen; a generated
  method-name mismatch only shows up under a full compile.
- **No host test substitutes for `esphome run` on *address* correctness.** The
  catalog and reference YAML get the *shape* right; only the wire confirms that a
  given address answers on a given firmware.

---

### 10b. Fuzzing, and why it is split across two builds

`P300Parser::parse()` is the one function here that is a byte-at-a-time state
machine driven directly by untrusted input, and the input is untrusted in the
ordinary sense rather than the adversarial one: a noisy optical head produces
garbled framing as normal operation. It is also the only code in the tree with
a track record — the zero-payload out-of-bounds write
(`proof_vs2_zero_payload.cpp`) and VitoWiFi's RESPONSE payload guards next door
in the packet. Both were found by
someone hand-writing the single input that trips them, which is exactly the
method that does not scale to sequences nobody imagined.

`tests/native/fuzz_parser_vs2.cpp` is one source compiled two ways: a
deterministic replay over the committed seed corpus (g++, in `build_and_run.sh`,
sub-second) and a libFuzzer target (clang, via `fuzz.sh`, human-initiated).
**CI runs only the replay.** An open-ended search on every push buys nothing
reproducible, but every input a campaign finds is promoted into
`fuzz_corpus_vs2.h` and executed forever after. The promotion step is what
turns a campaign into a gate; without it a finding is a file in `/tmp`.

The corpus lives in a header rather than as binary files, for two reasons that
are both about this repository specifically: a committed blob is unreviewable
in a diff, and the pre-commit hooks (`trailing-whitespace`, `end-of-file-fixer`,
`mixed-line-ending`) are text-typed, so a blob that happened to be all-printable
would be rewritten in place and quietly stop being the byte sequence it was
added for. Bytes plus a stated reason, in the `fixture_vectors.h` idiom.

**Oracles matter more than crashes here.** Memory safety is free once the
sanitizers are on, and it is most of the value — but §1a's whole argument is
that this project's characteristic defect is a silent wrong value, not a
segfault. So the harness also asserts that `COMPLETE` implies a verified
checksum; that a frame the engine would actually deliver keeps its advertised
payload inside the packet buffer; that the delivered length is exactly the
payload the frame carries (`dataLength()` for every frame but an RPC answer,
which is delivered by its frame length); and that **no byte sequence can permanently wedge the parser**,
checked by draining and then requiring a probe frame to complete *and be that
frame*.

Two things that came out of building it are worth keeping:

- **The RESPONSE guard is load-bearing for memory safety, not just semantics.**
  The parser skips payload-length validation for the two frame shapes that
  carry no inline payload (`READ`+`REQUEST`, `WRITE`+`RESPONSE`), so a
  `READ`+`REQUEST` frame can complete with `dataLength() == 255` while six
  bytes were written — and `data()` is non-null for it, since only `fc ==
  WRITE` returns null. A consumer reading `data()[0..dataLength())` would run
  past the buffer. Nothing reaches it because the exchange delivers only
  RESPONSE frames (an error frame is DEVICE_ERROR), and an oracle pins that
  coupling instead of leaving it to two unrelated decisions.
- **An RPC answer is the one delivered frame whose DataLength is not trusted**:
  the parser takes its size from the frame length, and the
  oracle checks the length the engine delivers, not `dataLength()`. Two seeds pin
  it: `rpc_resp_param_len` (DataLength repeats the parameter's length) and
  `rpc_resp_len255` (a 250-byte answer, length byte `0xFF`). The second caught
  a real out-of-bounds write when it was added: a payload index computed from
  the length byte wraps to 0 at `0xFF` and lands the first byte 250 bytes
  before the buffer. The index comes from the payload length, and
  `proof_p300_rpc.cpp` pins the same frame through the engine.
- **What the coverage says.** A gcov build replayed over a campaign corpus
  reaches **100% of the parser's 42 branches** (measured before the RPC
  answer branch existed; the two RPC seeds cover it, no campaign has been run
  since) (95.2% taken at least
  once) and 90.4% of its lines, and libFuzzer's edge count plateaus at 80 —
  the parser is small enough that the search saturates it, so a long campaign
  is not buying much beyond a short one. Two things account for the uncovered
  remainder, and neither is a gap in the corpus. `P300Parser::reset()` is
  engine-driven rather than byte-driven and is covered by
  `proof_p300_guards.cpp`. The length-overflow branch fires on every length
  byte past the buffer sized from `MAX_PAYLOAD` (§3), and a campaign should
  reach it.
- **An oracle that never fires is indistinguishable from one that cannot**, so
  both classes were negative-controlled: reverting the zero-payload fix makes
  the corpus trap under ASan, and removing the CHECKSUM step's reset wedges the
  parser and trips the liveness oracle. The second control initially *passed*
  against a wedged parser — the probe reused the 0x5525 capture frame, and a
  parser stuck in `CHECKSUM` completed it on a coincidental match with the
  stale buffer's checksum. The probe is now an address and payload that appear
  in no seed and no real datapoint, and what completed is checked for identity.
  Any future liveness probe needs the same property.

---

### 10c. The second fuzz target, and two lessons from building it

`fuzz_decode.cpp` covers the other half of the path — payload to text — and one
input class the parser target cannot reach: `encode_schaltzeiten_day()` parses
a string a **person** typed into Home Assistant, with raw `const char *`
walking. Everything else here is fed by the device.

**Exact-size heap buffers are the technique that matters.** Every output buffer
is allocated at precisely the capacity handed to the function, so a
one-past-the-end write lands in an ASan redzone rather than in slack a test
happened to reserve, and the capacity itself comes out of the fuzz input so the
truncation branches actually run. That is what turns "ASan is on" into "ASan
can see this": a fixed 4 KiB scratch buffer would have made every off-by-one in
`decode_ascii` / `decode_utf16` / `format_raw_dump` invisible.

**The round-trip oracle is the one that catches a silent wrong value.**
`decode.h` states the Schaltzeiten string as round-trippable, so bytes out of
the encoder must decode to a canonical string that re-encodes to the same
bytes. A negative control confirms it fires: swapping the ON/OFF writes in
`encode_schaltzeiten_day` trips it on the seed corpus. But note the limit,
because it bounds what this oracle proves — a *duplicated* write
(`tmp[2*pair+1] = onb`) does **not** trip it, since that bug is idempotent over
its own output. Round-trip oracles catch reordering and loss, not every error.
Only the encode→decode→encode direction is asserted: device bytes → string →
bytes is deliberately not a contract, because a byte that is no time renders
as `!XX` so the raw state stays visible, and the encoder rejects that. The same
holds for a reversed pair read from a device: it decodes faithfully, but the
encoder rejects it. The swap negative control above still fires under that
rule: a swapped pair decodes as reversed, so it also trips "canonical string
failed to re-encode", and on the `00:00-24:00` seed the swap moves `0xC0` into
the ON slot, where it reads as disabled and the byte compare fails.

**A harness whose parameter range is narrower than the caller's is not testing
the caller.** An output-capacity range capped at 96, comfortably more than the
longest canonical Schaltzeiten string (47 + NUL), silently makes one branch
unreachable: `format_raw_dump`'s `" ...(N bytes)"`
elision is only considered once the hex run has been written, and 48 bytes of
hex need 7 + 144 characters, so with a 96-byte ceiling the buffer was always
full first and no input could get there. The real caller passes
`char buffer[208]`. gcov found it; no amount of fuzzing would have, because the
input space never contained the case. The range is now 1..256, spanning every
buffer a real caller passes, and covers 100% of `decode.h`'s 308 branches
(94.2% of lines). The remaining uncovered lines are
null-pointer guards and `snprintf`-failure paths unreachable from a caller with
real buffers; they are left uncovered rather than padded with calls that cannot
happen in production.

---

## 11. Open items / forward work

Protocol questions, each with its test, are in the open-question tables of
[p300_protocol.md](p300_protocol.md#open-questions),
[kw_protocol.md](kw_protocol.md#open-questions) and
[gwg_protocol.md](gwg_protocol.md#open-questions). What is left here is the
rest.

**Waiting for a unit**

- **F0 (§6).** The hub reads `0x00F0` raw; the reference unit does not serve it
  (error frame, code `01`), and identification finishes without it, the
  fail-soft path. Open: a capture from a `VScotHO1_200*` / `VScotHC1_200*` unit,
  and what the other families answer there.
- **The burner-control chip at boot (§6).** `0x7650` equals P80 over both
  protocols on the reference unit (`0x24`, NEC), and reportedly on a VDensHO1
  (`0x20`). Open: whether the 10 devices with a GFA history but no declared
  `GFA_Kennung` (`V100KC4B`, `V200…`, `V300…`) answer there at all.
- **RF level sign (§8i).** `ecnUnit.dBm` values are emitted negated, on the
  strength of the German "-dBm" label and the unsigned `Byte` type. Unverified:
  the reference unit has no RF participant.
- **Bit-field writes (§5b).** Confirmed on P300 with single-bit masks. Open: a
  multi-bit mask, the read-back warning path, and on GWG two flags of `0x0005`
  toggled back to back and an `FF FF` cell refused.
- **Whole-block read-modify-write (§5b).** Confirmed on P300. Open: the bit
  field with a non-zero neighbouring byte, and a block wider than 2 bytes. The
  generator emits no block-mode write for GWG.
- **Heat-pump fault records (§7).** The byte layout is in no file; one captured
  record with the controller's own reading of it settles it.

**Engine and recorder**

- **The reference copy goes** once its outputs are recorded as fixtures
  (`tests/native/reference/`, §3); until then it is what the differential
  target compares against.
- **Recorder:** keeping a frozen ring across a restart (RTC memory: 8 KB on
  ESP32, 512 bytes on ESP8266), and a `freeze_on: resync` trigger.
- **Sharing the UART** with `uart_mux`, `serial_proxy` or `tcp_uart`: the
  engine owns the port, so another user needs a way to pause and resume it.
- **A NACK retry on P300.** The telegram after a NACK now waits out the
  device's last `15`; whether a NACKed telegram should also go once more is
  open until the cause of the first-after-re-SYNC NACKs is known.

**Generator and config**

- **`Convert4BytesToFloat`** (IEEE-754) is not a converter; such datapoints
  are commented hints. No committed catalog carries one.
- **`HexByte2DecimalByte`** has no decoder: "Anlage/TN" (two decimal bytes)
  reads as one 16-bit number (`01 01` is 257).
- **The vestigial `Converter` argument** of every generated
  `Datapoint(...)` line (`noconv`) could go from the codegen and
  `optolink_datapoint.h`; it changes every generated line but no behaviour.
- **Detecting the operating unit and modules** (BES or BEM, a mixer) to pick a
  catalog or hide entities at run time. It needs a reliable signal first: the
  hardware index does not separate BES and BEM on the units seen (§6,
  [gwg_protocol.md](gwg_protocol.md#identification)). ESPHome fixes the set of
  entities at compile time, so detection can only suggest a catalog or stop
  polling entities, not add new ones.

**Build**

- **Two independent ESP32 settings.** `esp32.framework.type` (`arduino` /
  `esp-idf`) is the SDK the code is compiled against; `esp32.toolchain`
  (`platformio` / `esp-idf`) the build system. Since ESPHome 2026.7 the default
  toolchain is `esp-idf`. `tests/test.esp32-idf.yaml` and
  `tests/test.esp32-arduino.yaml` pin `platformio`, so both toolchains stay
  covered: `tests/test.esp32-idf-native.yaml` builds the native one in its own
  CI job (`compile-idf-native`, cached on the SDK under
  `~/.cache/esphome/idf`). No library is registered on either, since every
  engine file sits at the component's top level (§3). Neither toolchain has run
  on hardware from this project's CI; the reference unit runs firmware built
  with the native one.
