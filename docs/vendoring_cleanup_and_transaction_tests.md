# VitoWiFi vendoring — cleanup & transaction-test plan

Status: **mixed.** The transaction-test harness (§3) is **built and passing — 8/8
against the real VS2 engine** (`tests/native/`). The vendoring and structural cleanups
(§1–§2) remain the proposed, unexecuted roadmap, gated on the layout decision in §0.
Target: bringing VitoWiFi (`bertmelis/VitoWiFi @ edc059a7`) in-tree as a self-contained,
vitohome-owned wire engine, with the quality cleanups that pay off and the automated
regression gate the upstream library never had — now in place.

All file paths, edits, constants, byte frames, and checksums below were verified
against the upstream source at `edc059a7` and against real `uart_debug` captures from
the running vitohome firmware: `viessman-optolink-logs_6_.txt` (reads) and
`viessman-optolink-logs_7_.txt` (Betriebsart writes). The harness vectors were further
verified by compiling and running against the actual engine. Nothing here is reasoned
from memory.

---

## 0. Scope, sequencing, and the one open gate

**What this covers**

1. Vendor the library *whole* (recap of the prior decision).
2. Three protocol-agnostic structural cleanups that are low-risk and high-readability.
3. A host-side transaction-test harness — the part of the engine upstream never
   unit-tested — **now built and passing** on golden-master vectors taken from the
   live vitohome captures (`tests/native/`, §3).

**Sequencing.** Everything in §2 is protocol-agnostic and benefits all three engines,
so do it once across the tree. The verification work in §3 is **VS2-first**, because
VS2 is the only protocol you can hardware-confirm. KW/GWG wiring stays a separate
stage (§5); the cleanups still apply to their files when that stage lands.

**The one open gate (must be settled before finalizing layout).** It is still
unconfirmed how ESPHome compiles *nested* third-party `.cpp` inside an external
component across both build systems (PlatformIO for `arduino`, CMake for `esp-idf`).
Everything in this document is **layout-independent at the source level** — the edits
are the same whether the vendored tree is flat or nested. Settle flat-vs-nested with a
real `esphome compile` of both `tests/test.esp32-idf.yaml` and
`tests/test.esp32-arduino.yaml` before committing to a directory shape. The safe
default if the compile is ambiguous is to flatten all vendored `.cpp` into the
component root, since that is the mechanism already building `vito_*.cpp`.

**License.** vitohome is GPLv3; VitoWiFi is MIT (Copyright Bert Melis, 2023).
MIT-into-GPL is permitted (MIT is GPL-compatible); the combined work is GPLv3.
Preserve the MIT header on every vendored file and add a provenance note
(`THIRD_PARTY.md` or a `NOTICE`) recording the upstream repo and the exact commit
`edc059a7`.

---

## 1. Step 0 — vendor whole (recap)

Copy all of upstream `src/` at `edc059a7` into the component, MIT headers intact.
Do **not** trim VS1/GWG out at this stage: taking the library intact is lower
per-file risk than a surgical extract, the unused engines are link-stripped under
`-ffunction-sections --gc-sections` (so they cost ~nothing in the binary while
unreferenced), and — importantly — vendoring whole also brings the upstream
**native unit tests** (`test/test_PacketVS2`, `test_ParserVS2`, `test_Datapoint`,
`test_PacketVS1`), which become your regression gate for the buffer swap in §1c.

---

## 2. Step 1 — structural cleanups (protocol-agnostic, low-risk)

### 1a. Remove the unused serial adapters and platform-specific constructors

vitohome supplies its own `ESPHomeUARTInterface` and constructs the engine through
the duck-typed template path on **both** frameworks:

```cpp
VitoWiFi::VitoWiFi<VitoWiFi::VS2>(&this->iface_)
//  -> template<class IFACE> VitoWiFi(IFACE*)
//  -> template<class C>     VS2(C*)   (wraps C in GenericInterface<C>)
```

The `HardwareSerial` / `SoftwareSerial` / Linux constructors are therefore dead
weight even on the `arduino` target. Remove them.

**Delete (6 files):**

```
Interface/HardwareSerialInterface.cpp
Interface/HardwareSerialInterface.h
Interface/SoftwareSerialInterface.cpp
Interface/SoftwareSerialInterface.h
Interface/LinuxSerialInterface.cpp
Interface/LinuxSerialInterface.h
```

Keep `Interface/SerialInterface.h` and `Interface/GenericInterface.h`.

**Edit `VS2/VS2.h` (and the matching blocks in `VS1/VS1.h`, `GWG/GWG.h`):**
remove the conditional adapter includes, keep only the generic include —

```cpp
// remove:
#if defined(ARDUINO_ARCH_ESP8266) || defined(ARDUINO_ARCH_ESP32)
#include "../Interface/HardwareSerialInterface.h"
#if defined(ARDUINO_ARCH_ESP8266)
#include "../Interface/SoftwareSerialInterface.h"
#endif
#elif defined(__linux__)
#include "../Interface/LinuxSerialInterface.h"
#endif
// keep:
#include "../Interface/GenericInterface.h"
```

— and remove the platform-gated constructor **declarations**:

```cpp
// remove from the class body:
#if defined(ARDUINO_ARCH_ESP8266) || defined(ARDUINO_ARCH_ESP32)
explicit VS2(HardwareSerial* interface);
#if defined(ARDUINO_ARCH_ESP8266)
explicit VS2(SoftwareSerial* interface);
#endif
#endif
#if defined(__linux__)
explicit VS2(const char* interface);
#endif
// keep the template<class C> VS2(C*) constructor
```

**Edit `VS2/VS2.cpp` (and `VS1/VS1.cpp`, `GWG/GWG.cpp`):** delete the corresponding
platform-gated constructor **definitions** (the `#if defined(ARDUINO_ARCH_*)` and
`#if defined(__linux__)` blocks at the top of each `.cpp`). Keep only the template
constructor.

**Why this is safe on both frameworks.** Under pure `esp-idf`, `ARDUINO_ARCH_ESP32`
is undefined, so those blocks were already no-ops. Under `arduino`, they *were*
active but vitohome never called them. Removing them changes nothing vitohome uses.

**Gate:** `esphome compile` green on **both** `test.esp32-idf.yaml` and
`test.esp32-arduino.yaml`.

---

### 1b. Fix `Logging.h` (the `<iostream>`-under-IDF wart)

Upstream `Logging.h` routes to `esp32-hal-log.h` only under `ARDUINO_ARCH_ESP32`;
under pure ESP-IDF it falls to the PC branch and pulls in `<iostream>`/`<iomanip>`.
Replace the whole file with a no-op default that, when explicitly enabled, routes to
ESP-IDF's logger (available under both frameworks). This drops the heavy include and
unifies behavior.

**Replacement `Logging.h`:**

```cpp
/* Upstream MIT header retained above this block. */
#pragma once

// vitohome: VitoWiFi's internal logging. Off by default (matching upstream's
// release behavior). When DEBUG_VITOWIFI is defined, route to the ESP-IDF logger,
// which is present under both the esp-idf and arduino-on-ESP32 frameworks. The
// upstream <iostream> PC-branch is removed so pure ESP-IDF builds do not drag in
// iostream/iomanip.
#if defined(DEBUG_VITOWIFI) && defined(ESP_PLATFORM)
  #include "esp_log.h"
  #define vw_log_i(...) ESP_LOGI("VitoWiFi", __VA_ARGS__)
  #define vw_log_e(...) ESP_LOGE("VitoWiFi", __VA_ARGS__)
  #define vw_log_w(...) ESP_LOGW("VitoWiFi", __VA_ARGS__)
#else
  #define vw_log_i(...) do {} while (0)
  #define vw_log_e(...) do {} while (0)
  #define vw_log_w(...) do {} while (0)
#endif
```

`Helpers.h` needs **no** change — its `vw_millis()` already resolves correctly via
`ESP_PLATFORM` to `xTaskGetTickCount()` on both frameworks.

**Gate:** IDF compile no longer references `<iostream>`; confirm a small binary-size
delta and that vitohome's own `ESP_LOGx` output is unaffected (it is the only
logging in practice, since `DEBUG_VITOWIFI` is off).

---

### 1c. Swap the packet `malloc` for a fixed `std::array`

`PacketVS2` manages its buffer with `malloc`/`free`/`realloc` and a manual
`_allocatedLength`. The VS2 length byte is a `uint8_t`, so the largest valid frame
is **256 bytes** (`length` + the bytes it counts). A fixed
`std::array<uint8_t, 256>` covers every legal frame and never reallocates.

> Honest correction to the earlier "~5-line" estimate: it is **~4 edit sites**
> (~15 lines), not 5. The accessor methods (`operator[]`, `data()`, `checksum()`,
> `reset()`) are untouched because `std::array::operator[]` matches the existing
> raw-pointer indexing.

**Edits to `VS2/PacketVS2.{h,cpp}`:**

- Header: `#include <array>`; replace the `uint8_t* _buffer;` member and the
  `_allocatedLength` member with `std::array<uint8_t, 256> _buffer;`. Add a named
  constant, e.g. `static constexpr std::size_t kMaxFrame = 256;`.
- Constructor: delete the `malloc` block (keep `reset();`).
- Destructor: delete the `free(_buffer);` body.
- `createPacket(...)`: replace the realloc-growth block with a bounds check —
  ```cpp
  std::size_t needed = (fc == FunctionCode::WRITE) ? len + 6 : 6;
  if (needed > _buffer.size()) return false;   // was: realloc grow
  ```
- `setLength(uint8_t length)`: replace the realloc-growth block with —
  ```cpp
  if (static_cast<std::size_t>(length) + 1 > _buffer.size()) return false;
  _buffer[0] = length;
  return true;
  ```

**Tradeoff (stated plainly):** this costs ~256 bytes of static RAM per packet
instance (VS2 holds two: the parser's packet and `_currentPacket`), in exchange for
zero heap use and deterministic, fragmentation-free allocation. On the ESP32-C3 that
RAM is negligible. If you can bound the maximum datapoint length lower than the
protocol max, `kMaxFrame` can be reduced; 256 is the safe protocol-complete value.

**Regression gate:** run the vendored upstream `test/test_PacketVS2` (this is the
concrete reason §1 vendors the tests too). The buffer swap must leave every packet
assertion passing.

> VS1/GWG carry the same `malloc` pattern in `PacketVS1`/`PacketGWG` **and** an
> engine-level `_responseBuffer` malloc in `VS1.cpp`/`GWG.cpp`. Apply the identical
> treatment when those protocols are wired (§5). VS2 itself has no `_responseBuffer`
> — it reads through the parser's packet — so the VS2 scope here is `PacketVS2` only.

---

### 1d. Name the magic-number timeouts

The VS2 state machine hardcodes its timing inline. Lift to named `constexpr`. This is
pure readability — the values are unchanged — but it also **surfaces a real protocol
difference** that is currently invisible: GWG's request timeout is `3000`, while
VS2/VS1 use `4000`. Keep the constants **per-engine**; do not unify them.

VS2 literals (`VS2/VS2.cpp`):

| Line | Current literal | Meaning | Proposed constant |
|------|-----------------|---------|-------------------|
| ~188 | `> 4000UL` | per-request response watchdog | `REQUEST_TIMEOUT_MS = 4000` |
| ~232 | `> 3000`   | RESET-ACK retry window | `HANDSHAKE_RETRY_MS = 3000` |
| ~257 | `> 3000`   | INIT-ACK timeout | `HANDSHAKE_RETRY_MS = 3000` |
| ~267 | `> 3000UL` | idle re-INIT keepalive | `KEEPALIVE_INTERVAL_MS = 3000` |

VS1: `REQUEST_TIMEOUT_MS = 4000`, reset/keepalive `= 3000`.
GWG: `REQUEST_TIMEOUT_MS = 3000` (← the difference worth a comment).

Place each engine's constants as `static constexpr uint32_t` members of its class (or
in a per-engine constants block). **Gate:** byte-identical behavior; confirm the
constants equal the old literals, then compile.

---

## 3. Step 2 — the transaction-test harness (BUILT — 8/8 passing)

This section is **done**. The harness lives in `tests/native/` and was compiled
against the actual VitoWiFi VS2 sources on the host and executed:

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
WRITE asserts the write request (`41 06 00 02 <addr> 01 <data> <cs>`), the response-ACK,
and that the bare write-acknowledgement (`41 05 01 02 <addr> 01 <cs>`, no payload)
completes.

**Why it was the best investment.** Upstream has native unit tests for `Packet`,
`Parser`, and `Datapoint` — the static pieces — but **none for the state machines**.
The handshake, keepalive, ACK ordering, timeout, and fragment-reassembly behavior is
validated only by compile-only example builds and field use. This harness is the
regression gate the engine never had, host-side, no hardware — exactly what makes the
engine *safe to refactor later* (e.g. the §1a–§1d cleanups) and what de-risks the
KW/GWG work (the seam you cannot hardware-test).

**Coverage split (composes with what you already have).** The harness locks down the
**transaction/handshake/fragmentation/write** layer using bare `VitoWiFi<VS2>`; the
existing 74-check `decode.h` tests lock down the **value** layer (including the
float32-precision case). Together they cover wire→decode→value end-to-end without the
ESPHome framework in a host test.

### Files (in `tests/native/`)

- `fixture_vectors.h` — the committed golden-master vectors (Appendix A), pulled from
  the live `_6_`/`_7_` captures and checksum-verified. Single source of truth.
- `fake_optolink.h` — duck-typed Optolink stand-in (below).
- `test_vs2_transaction.cpp` — data-driven runner: handshake into IDLE, then per
  vector replay request + chunked response and assert wire + payload.
- `build_and_run.sh` — host compile + run; takes the vendored VitoWiFi `src` path.

### The fake interface

Duck-typed to the contract `ESPHomeUARTInterface` satisfies. Trivial by design — the
test orchestrates the script. `feed()` takes one chunk; chunk boundaries across calls
model UART fragmentation.

```cpp
// tests/native/fake_optolink.h
class FakeOptolink {
 public:
  bool begin() { return true; }
  void end() {}
  std::size_t write(const uint8_t* d, uint8_t n) {
    written_.insert(written_.end(), d, d + n); return n;
  }
  std::size_t available() const { return inbound_.size(); }
  uint8_t read() {
    if (inbound_.empty()) return 0;
    uint8_t b = inbound_.front(); inbound_.pop_front(); return b;
  }
  void feed(const std::vector<uint8_t>& bytes) {        // device -> ESP, one chunk
    inbound_.insert(inbound_.end(), bytes.begin(), bytes.end());
  }
  const std::vector<uint8_t>& written() const { return written_; }
  void clear_written() { written_.clear(); }
 private:
  std::deque<uint8_t> inbound_;
  std::vector<uint8_t> written_;
};
```

### The runner (read + write, one path)

The handshake is modeled with the known protocol bytes (`EOT 04` → `ENQ 05`,
`SYNC 16 00 00` → `ACK 06`); then each vector's request is emitted and its device
chunks fed back, one chunk per pump so fragmentation is exercised.

```cpp
static bool run_vector(const TransactionVector& tv) {
  FakeOptolink io;
  VitoWiFi::VitoWiFi<VitoWiFi::VS2> vito(&io);     // must be fully qualified (see findings)
  std::vector<uint8_t> got_payload; bool got_resp = false;
  vito.onResponse([&](const VitoWiFi::PacketVS2& r, const VitoWiFi::Datapoint&) {
    if (r.data()) got_payload.assign(r.data(), r.data() + r.dataLength());  // guard: null on write-ack
    got_resp = true;
  });
  vito.begin();
  auto pump = [&](int n){ for (int i = 0; i < n; ++i) vito.loop(); };
  handshake(io, pump);                              // EOT/ENQ/SYNC/ACK -> IDLE, clear_written()

  VitoWiFi::Datapoint dp(tv.name, tv.address, tv.read_len, VitoWiFi::noconv);
  if (tv.kind == Kind::WRITE)
    vito.write(dp, tv.write_data.data(), (uint8_t)tv.write_data.size());
  else
    vito.read(dp);
  pump(8);                                          // engine emits request -> SEND_ACK
  for (const auto& chunk : tv.device_chunks) { io.feed(chunk); pump(6); }

  std::vector<uint8_t> expect = tv.request; expect.push_back(0x06);   // request + response-ACK
  bool wire_ok = (io.written() == expect);
  bool payload_ok = got_resp && (tv.kind == Kind::WRITE || got_payload == tv.payload);
  return wire_ok && payload_ok;
}
```

### Fragmentation — reproducible, not a one-off

Both `0x23xx` reads arrive split on hardware (`41 06` on one UART read, the payload on
the next), exactly like the older Brenner-Modulation case. The fixture preserves those
chunk boundaries, so the byte-at-a-time parser is driven across the boundary it crosses
on **every** poll. Both pass.

### Three findings the build surfaced (each folds back here)

1. **Namespace collides with the class name.** `VitoWiFi::VitoWiFi<VS2>` must be fully
   qualified — `using namespace VitoWiFi;` is ambiguous. vitohome's component already
   does this; the harness follows suit. (No code change needed; just a note.)
2. **`PacketVS2` is non-copyable** (deleted copy-assign — owns a `malloc` buffer), so
   the callback extracts the payload rather than copying. The **§1c `std::array` swap
   would make it trivially copyable again** — a free side benefit; consider restoring
   the copy operators when you do §1c.
3. **Write-ack `data()` is `nullptr` by design, but `dataLength()` still returns the
   echoed length.** A naive `data()[0..dataLength())` segfaults (ASan caught it
   instantly). The correct consumer guard is `if (data())` before reading payload —
   which the harness now uses, and which vitohome's hub already does (hardware never
   crashes). This is the exact non-discriminated-read class the architecture avoids,
   now machine-checked.

### Where the bytes come from

`>>>` is ESP→device, `<<<` is device→ESP in the `uart_debug` captures. A ~10-line
extractor (filter `>>>`/`<<<`, strip timestamp/tag, split on `:`, parse hex) turns log
lines into vectors; the extracted set is committed as `fixture_vectors.h` so the test
does not depend on the raw log. To add a vector: append one `TransactionVector` (the
request frame, the device chunks `{0x06}` then the response split at the same
boundaries the capture shows, and the expected payload). Appendix A is the current set.

---

## 4. Verification gate sequence

Fold into the project's existing gate model (host C++ → Python → lint → config →
compile → run). The new harness slots in beside the decode tests and becomes a
permanent CI gate:

```
host: g++ -Werror decode.h tests            (existing, 74 checks)
host: VS2 transaction harness                (DONE — §3, 8/8 passing; build_and_run.sh)
host: vendored upstream test/test_*          (NEW — gates the §1c buffer swap)
ruff check / ruff format                      (existing)
clang-format --Werror                         (existing)
pytest tests/unit                             (existing)
esphome config  (both yamls)                  (existing)
esphome compile (esp-idf AND arduino)         (existing — settles §0 layout gate)
esphome run     (VS2 hardware)                (existing — definitive)
```

Each §1 cleanup also carries its own narrow gate (stated inline). Do the cleanups as
separate commits so "did this change behavior?" stays answerable in isolation.

---

## 5. Explicitly deferred / do-not-do

- **Do not reimplement the state machine or parser from logic.** They are clean,
  battle-proven, and have no automated oracle other than the harness you are now
  adding. The openv spec gives the protocol shape, not the empirically-tuned timing.
  Copy them; the §3 harness is what makes them safe to refactor later if ever needed.
- **KW/GWG wiring is Stage 3.** Bring the engines along now (vendored whole, cleanups
  applied to their files), but the hub plumbing, codegen, and — critically —
  hardware verification are a separate stage gated on a KW unit or a community tester.
  Ship any KW/GWG path labeled "implemented, pending hardware verification."
- **Layout (flat vs nested) is undecided.** Settle it with the dual-framework compile
  in §0 before committing a directory shape.

---

## Appendix A — committed fixture vectors (`tests/native/fixture_vectors.h`)

Golden-master vectors from the **running vitohome firmware**: READs from
`viessman-optolink-logs_6_.txt` (@ 09:00:43), WRITEs from
`viessman-optolink-logs_7_.txt` (Betriebsart switches @ 09:49:33+). `>>>` = ESP→device,
`<<<` = device→ESP. Every request/response checksum and payload was recomputed,
matches the captures, and passes against the real engine (§3). Bytes in hex.

| # | Datapoint | Kind | Addr | Len | Request (ESP→dev) | Response (dev→ESP) | Payload / value |
|---|-----------|------|------|-----|-------------------|--------------------|-----------------|
| R1 | Outside Temp | READ | 0x5525 | 2 | `41 05 00 01 55 25 02 82` | `41 07 01 01 55 25 02 02 01 88` | `02 01` = 258 ×0.1 = **25.8 °C** |
| R2 | Boiler Temp | READ | 0x0810 | 2 | `41 05 00 01 08 10 02 20` | `41 07 01 01 08 10 02 DC 00 FF` | `DC 00` = 220 = **22.0 °C** |
| R3 | Part No | READ | 0x08E0 | 7 | `41 05 00 01 08 E0 07 F5` | `41 0C 01 01 08 E0 07 35 34 36 34 37 39 33 73` | ASCII **"5464793"** |
| R4 | Ctrl Serial | READ | 0xF000 | 16 | `41 05 00 01 F0 00 10 06` | `41 15 01 01 F0 00 10 …59` | ASCII **"7539778404101109"** |
| R5 | Betriebsart raw | READ | 0x2301 | 1 | `41 05 00 01 23 01 01 2B` | `41 06` ∥ `01 01 23 01 01 05 32` | `0x05` — **fragmented** |
| R6 | Betriebsart set | READ | 0x2323 | 1 | `41 05 00 01 23 23 01 4D` | `41 06` ∥ `01 01 23 23 01 00 4F` | `0x00` — **fragmented** |
| W1 | Write Betriebsart | WRITE | 0x2323 | 1 | `41 06 00 02 23 23 01 01 50` | `41 05 01 02 23 23 01 4F` | write `0x01` → bare ack |
| W2 | Write Betriebsart | WRITE | 0x2323 | 1 | `41 06 00 02 23 23 01 03 52` | `41 05 01 02 23 23 01 4F` | write `0x03` → bare ack |

Checksum rule (verified, e.g. request 0x08A7 → `0xB9`): sum the length byte through the
last counted byte, mod 256. ESP acknowledges every device response with `06`; the
device acknowledges every request with `06` (consumed in `SEND_ACK`).

R5/R6 are the canonical robustness vectors — both `0x23xx` reads fragment on hardware
(`41 06` on one read, the rest on the next), so they exercise parser reassembly across
the boundary it crosses every poll. W1/W2 cover the write path and pin the write-ack
contract (`data()==nullptr`, see §3 finding 3). The **float32-precision** case (the
burner-hours counter > 2²⁴) is a *decode* concern and lives in the 74-check `decode.h`
tests, not in this transaction fixture.
