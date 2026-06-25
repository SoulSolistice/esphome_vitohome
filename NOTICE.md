# Credits and Third-Party Notices

vitohome is licensed under the **GNU General Public License v3.0** (see
[`LICENSE`](LICENSE)). It builds on the work of several projects, credited here.

## Optolink protocol engine — VitoWiFi (vendored)

The wire/transport engine under
[`components/vitohome/optolink/`](components/vitohome/optolink/) is a vendored
and modified copy of **VitoWiFi** by Bert Melis, used as the P300/VS2 transport
layer.

- Upstream: <https://github.com/bertmelis/VitoWiFi>
- Vendored at commit `edc059a7c3df3de0a5de089ebc1bdbfc19ca6faa`
- License: MIT — see
  [`components/vitohome/optolink/LICENSE.optolink`](components/vitohome/optolink/LICENSE.optolink)

VitoWiFi is MIT-licensed; combining it into this GPLv3 work is permitted, and
the combined work is distributed under the GPLv3. The exact modifications made
during vendoring — the de-branding rename, the buffer changes, and the
behavioural fixes — together with the retained per-file MIT notices, are
documented in
[`components/vitohome/optolink/THIRD_PARTY.md`](components/vitohome/optolink/THIRD_PARTY.md).

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

## Trademarks

"Viessmann", "Vitosoft" and "Optolink" are trademarks of Viessmann. This is an
independent, unofficial project, and is not affiliated with, endorsed by, or
supported by Viessmann.
