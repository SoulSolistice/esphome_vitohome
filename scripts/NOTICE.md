# Credits and Third-Party Notices

vitohome is licensed under the **GNU General Public License v3.0** (see
[`LICENSE`](../LICENSE)). It builds on the work of several projects, credited here.

## Optolink protocol engine — VitoWiFi (vendored)

The wire/transport engine under
[`components/vitohome/optolink/`](../components/vitohome/optolink/) is a vendored
and modified copy of **VitoWiFi** by Bert Melis, used as the P300/VS2 transport
layer.

- Upstream: <https://github.com/bertmelis/VitoWiFi>
- Vendored at commit `edc059a7c3df3de0a5de089ebc1bdbfc19ca6faa`
- License: MIT — see
  [`components/vitohome/optolink/LICENSE.optolink`](../components/vitohome/optolink/LICENSE.optolink)

VitoWiFi is MIT-licensed; combining it into this GPLv3 work is permitted, and
the combined work is distributed under the GPLv3. The exact modifications made
during vendoring — the de-branding rename, the buffer changes, and the
behavioural fixes — together with the retained per-file MIT notices, are
documented in
[`components/vitohome/optolink/THIRD_PARTY.md`](../components/vitohome/optolink/THIRD_PARTY.md).

## Datapoint structure — InsideViessmannVitosoft

Per-device datapoint definitions are not discoverable over the Optolink bus;
they live in Viessmann's Vitosoft data. The structure of that data — how
datapoints, events, value types, identification and conversions relate — is
documented by
**[InsideViessmannVitosoft](https://github.com/sarnau/InsideViessmannVitosoft)**
by Markus Fritze (sarnau). That project's reverse engineering of the Vitosoft
XML files and the Optolink protocol is the groundbreaking work that makes this
component's catalog generation possible. The BCD date/time decoding in
`decode.h` follows the layout from its `Viessmann2MQTT.py`.

## Protocol cross-reference — openv / vcontrold

P300 protocol constants and error-code mappings were cross-checked against the
**[openv / vcontrold](https://github.com/openv/vcontrold)** project as an
independent reference.

## Switching-time format cross-reference — optolink-splitter

The per-day switching-time (Schaltzeiten) byte format used by the
`text`/`text_sensor` codecs in `decode.h` (hour in the high 5 bits, the
10-minute step in the low 3 bits, `0xFF` = unused) was cross-checked against
**[philippoo66 / optolink-splitter](https://github.com/philippoo66/optolink-splitter)**
(`utils.py`, `byte_to_hhmm` / `schedvdens`) so a program written by this
component is byte-identical to what that project reads. optolink-splitter is
GPL-3.0; its continuation of the older ViessData tooling (originally MIT,
DI Zimmermann Stephan, 2008/2009) is GPLv3-compatible.

## Fault-code text — Viessmann service documentation

The Vitotronic display-Stoerungscode maps in `scripts/fault_codes.py` are sourced
from Viessmann Serviceanleitungen and cross-checked against the openv map:
`VITOTRONIC_VD300_B3HA` from the Vitodens 300-W (type B3HA) manual — the
authoritative, default set, since VScotHO1_72 ("Projekt Neptun") is the
Vitotronic 200 controller in that boiler — and `VITOTRONIC_VD200` from the
Vitodens 200 (WB2A) manual. These are factual code-to-text labels; fault-code
semantics are device-variant-specific and the generator marks the map as a
default to verify on the unit.

The official Viessmann fault-code reference for Gas-Wandgeraete der Serie 200/300
with Vitotronic control (https://www.viessmann.de/de/wissen/wartung-und-reparatur/fehlercodes.html)
was used to cross-validate the VD300-W set and to correct two codes (0x91/0x99)
that were garbled in the PDF extraction. It is a general orientation list (it
disclaims completeness and notes newer models may differ), so the device-specific
B3HA Serviceanleitung remains the primary source.

## Trademarks

"Viessmann", "Vitosoft" and "Optolink" are trademarks of Viessmann. This is an
independent, unofficial project, and is not affiliated with, endorsed by, or
supported by Viessmann.
