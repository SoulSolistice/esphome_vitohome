# VS2 transaction harness — fixture folded from live vitohome capture

Host-side regression gate for the part of the VS2 engine upstream never unit-tested:
the request/ACK/response/fragment-reassembly choreography. It composes with the
existing 74-check `decode.h` tests — this harness covers wire→payload; those cover
payload→value.

## Status: built and run against the real VS2 engine — 8/8 pass

The fixture was **not** asserted on faith. It was compiled against the actual
VitoWiFi VS2 sources on the host and executed:

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
- `build_and_run.sh` — host compile + run. Pass the path to the vendored VitoWiFi
  `src` as `$1` (default assumes `components/vitohome/vitowifi/src`).

## Three things the build surfaced (all already in the cleanup plan)

1. **Namespace collides with class name.** `VitoWiFi::VitoWiFi<VS2>` must be fully
   qualified — `using namespace VitoWiFi;` is ambiguous. (vitohome's component
   already does this correctly; the test follows suit.)
2. **`PacketVS2` is non-copyable** (deleted copy-assign — owns a `malloc` buffer), so
   the callback extracts the payload rather than copying. The §1c `std::array` swap
   would make it trivially copyable again — a free side benefit.

   The harness also caught a real null-deref hazard: for a write-ack, `data()` returns
   `nullptr` by design but `dataLength()` still returns the echoed length, so a naive
   `data()[0..dataLength())` segfaults. The correct consumer guard is `if (data())`
   before reading payload. vitohome's hub already does the right thing (hardware
   doesn't crash); this vector now guards that contract permanently.
3. **The §1b `<iostream>` wart is live.** The engine's verbose `state N --> M`
   logging is upstream `Logging.h`'s PC branch writing to `std::cout`
   unconditionally; `build_and_run.sh` filters it. After the §1b fix it is silent.

## CI integration

Add as a host gate beside the decode tests (it is the first host test that compiles
the actual VS2 engine):

```yaml
- name: VS2 transaction harness
  working-directory: tests/native
  run: ./build_and_run.sh ../../components/vitohome/vitowifi/src
```

The script exits non-zero on any vector failure. **Note:** the build currently links
`LinuxSerialInterface.cpp` because the unmodified upstream `VS2.cpp` defines the
`VS2(const char*)` constructor under `__linux__`. After cleanup §1a removes the
platform-specific constructors, drop that file from the link line — the harness then
compiles only `VS2` / `ParserVS2` / `PacketVS2` / `Constants` / `Datapoint*`.

## Extending

Add a vector by appending one `TransactionVector` to `fixture_vectors.h`: the request
frame, the device chunks (`{0x06}` then the response, split at the same boundaries the
capture shows), and the expected payload. Pull new frames straight from a `uart_debug`
capture — `>>>` is the request, `<<<` chunks are the device bytes in order.
