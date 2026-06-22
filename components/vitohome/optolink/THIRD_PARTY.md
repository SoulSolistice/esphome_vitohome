# Third-party software: Optolink protocol engine

The `optolink/` subtree is a vendored and modified copy of the
**VitoWiFi** library by Bert Melis.

| | |
|---|---|
| Upstream project | https://github.com/bertmelis/VitoWiFi |
| Exact commit vendored | `edc059a7c3df3de0a5de089ebc1bdbfc19ca6faa` |
| Upstream license | MIT (Copyright (c) 2023 Bert Melis) |
| License text | see [`LICENSE.optolink`](./LICENSE.optolink) |

Only the engine sources from the upstream `src/` tree were vendored. The
upstream `test/` tree was imported separately into `tests/native/upstream/`
as a regression gate (see that directory).

This component as a whole is licensed under the GPLv3. Combining MIT-licensed
sources into a GPLv3 work is permitted; the combined work is distributed under
the GPLv3. The per-file MIT notices are retained at the top of every vendored
file, each annotated with a single line noting it was modified as part of
vitohome and pointing back to this file.

## Modifications applied during vendoring

These are intentional divergences from upstream `edc059a7`:

1. **Namespace & class rename (de-branding).** `namespace VitoWiFi` ->
   `esphome::vitohome::optolink`; `namespace VitoWiFiInternals` ->
   `esphome::vitohome::optolink::internals`. The umbrella template class
   `VitoWiFi::VitoWiFi<PROTOCOLVERSION>` (whose name collided with its
   namespace) is renamed to `OptolinkEngine<PROTOCOLVERSION>`, with the
   concrete per-protocol engines named `VS2Engine` / `VS1Engine` /
   `GWGEngine`. The logging/helper macros were renamed off the old brand
   (`vw_*` -> `optolink_*`). No `VitoWiFi`/`vitowifi` token remains in code,
   paths, includes or build files; the only remaining mentions are this
   attribution prose and the per-file MIT headers.

2. **Platform serial adapters removed.** `HardwareSerialInterface`,
   `SoftwareSerialInterface` and `LinuxSerialInterface` (and the
   platform-gated constructors that used them in `VS2`/`VS1`/`GWG`) were
   deleted. The component only ever constructs an engine through the
   duck-typed `template<class C>` constructor with its own
   `ESPHomeUARTInterface`. `SerialInterface.h` and `GenericInterface.h` are
   kept.

3. **Logging reworked.** The upstream `Logging.h` fell back to a PC branch
   that pulled in `<iostream>`/`<iomanip>` under pure ESP-IDF and wrote
   verbose state transitions to `std::cout` on host. It is replaced with
   no-op-by-default macros that, when `VITOHOME_DEBUG_OPTOLINK` is defined,
   route to the ESP-IDF logger (`esp_log.h`, available under both the
   esp-idf and arduino frameworks via `ESP_PLATFORM`). Host builds are silent
   by default.

4. **Packet buffers modernized.** `PacketVS2`, `PacketVS1` and `PacketGWG`
   replaced their `malloc`/`free`/`realloc` + `_allocatedLength` buffers with
   fixed `std::array`s (VS2: `kMaxFrame=256`, the exact protocol-complete
   bound; VS1/GWG: `kMaxFrame=260`). The engine-level `_responseBuffer`
   `malloc` + `_expandResponseBuffer()` + `_allocatedLength` in `VS1`/`GWG`
   were likewise replaced by fixed `std::array<uint8_t, 256>`. Frames that
   would exceed the fixed bound now fail soft (return `false`) instead of
   growing. With the raw buffer gone, the previously-deleted copy operations
   on the packet classes are restored.

5. **VS2 write-payload guard.** Because a VS2 write stores its P300 length
   byte as `0x05 + len`, `PacketVS2::createPacket` now rejects write payloads
   longer than 250 bytes.

6. **Named timeouts.** The inline magic-number timeouts were lifted to named
   `static constexpr` members, per engine (values byte-identical to
   upstream): VS2 `REQUEST_TIMEOUT_MS=4000`, `HANDSHAKE_RETRY_MS=3000`,
   `KEEPALIVE_INTERVAL_MS=3000`; VS1 `REQUEST_TIMEOUT_MS=4000`,
   `ENQ_RESET_INTERVAL_MS=3000`, `SYNC_WINDOW_MS=50`; GWG
   `REQUEST_TIMEOUT_MS=3000` (deliberately distinct from VS2/VS1).

7. **GWG one-shot bugfix (behavioral divergence).** Upstream
   `GWG::_tryOnResponse()` did not clear `_currentDatapoint` after a
   successful response, unlike `VS1`/`VS2`. Because `read()`/`write()`
   early-return while `_currentDatapoint` is truthy, upstream GWG refused
   every request after the first success - i.e. it was effectively one-shot.
   The vendored `GWG::_tryOnResponse()` now clears `_currentDatapoint` after
   invoking the callback, matching VS1/VS2. This is the only intentional
   change to on-wire/runtime behavior; everything else preserves upstream
   protocol behavior.
