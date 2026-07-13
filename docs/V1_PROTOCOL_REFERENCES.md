# V1 Protocol References

> Mirror fidelity is the default; deliberate departures from mirroring (mute
> debounce, ALP laser compose, driver-elected filters) are governed by
> `docs/VALENTINE_PHILOSOPHY.md` and annotated in code as `Valentine's Law`
> notes. When fidelity and the Law conflict, the Law wins — visibly, in an
> annotated block, never silently.

This tracked note is the repo citation target for Valentine One Gen2 protocol
facts used by source comments, tests, and API docs.

The official Valentine Research ESP Specification PDFs are not committed here.
Maintainers may compare these notes against private or local official copies,
but production comments should cite this file by anchor first so a fresh clone
has a reproducible evidence chain.

Do not paste specification excerpts into this file. Keep the content to original
summaries, code-facing invariants, and the official version/section names needed
to re-check the fact against an official copy.

## Packet framing

Code anchors:

- `include/config.h`
- `src/packet_parser.cpp`
- `src/ble_commands.cpp`

Stable invariants:

- ESP packets are framed by `0xAA` at the start and `0xAB` at the end.
- Packet ID is stored at byte offset 3.
- The declared payload length is at byte offset 4.
- Parser payload handling starts at byte offset 5.
- The checksum byte is inside the declared payload-length region for display
  packets; the parser treats short/chunked packets defensively instead of
  enforcing checksum validity.

## Packet IDs and volume responses

Code anchors:

- `include/config.h`
- `src/ble_commands.cpp`
- `src/packet_parser.cpp`

Stable invariants:

- `reqVersion` is packet ID `0x01`; `respVersion` is packet ID `0x02`.
- `reqAllVolume` is packet ID `0x3C`; `respAllVolume` is packet ID `0x3D`.
- The all-volume response payload carries main, muted, saved-main, and
  saved-muted volume bytes.

## Version response

Code anchors:

- `src/packet_parser.cpp`
- `test/test_packet_parser/test_packet_parser.cpp`

Stable invariants:

- The version response payload is seven ASCII bytes.
- The first byte identifies the device family.
- The remaining bytes encode a dotted version string shaped like `v4.1028`.
- Incoming version packets must use `respVersion` (`0x02`); `reqVersion`
  (`0x01`) is only the outbound request ID.

## `infDisplayData`

Code anchors:

- `src/packet_parser.cpp`
- `src/display.h`
- `src/display_update.cpp`
- `test/test_packet_parser/test_packet_parser.cpp`
- `test/test_display_rendering_arrow/test_display_rendering_arrow.cpp`

Stable invariants:

- The display data payload's first eight bytes are:
  1. bogey-counter image 1
  2. bogey-counter image 2
  3. signal-bar LED bitmap
  4. band/arrow/mute image 1
  5. band/arrow steady image 2
  6. aux/status byte 0
  7. aux/status byte 1
  8. aux/status byte 2
- The bogey counter is one physical character. Image 2 is the companion
  blink/steady mask for image 1, not a second digit.
- For bogey, band, and arrow indicators, bits present in image 1 but absent
  from image 2 are the blinking bits.
- The signal-bar LED bitmap is contiguous-from-LSB. V1 Gen2 hardware exposes
  six visible source bars (`0x01` ... `0x3F`), while this project paints an
  eight-slot meter. Parser output is therefore the local display strength:
  `0x3F` is full deflection (8). Wider or non-contiguous/overflow bitmaps also
  mean full deflection, matching VR's `InfDisplayData.getNumberOfLEDS()`
  overflow sentinel and avoiding understated live threats.
- The mute indicator is sourced from image 1 bit 4; volume values alone do not
  imply muted display state.
- Aux/status byte 0 bit 0 is the V1 soft-mute flag.
- Aux/status byte 0 bit 2 gates whether band/arrow display data is meaningful.
- Aux/status byte 0 bit 3 reports V1 main-display state. This firmware does
  not use that bit as a renderer blanking gate because doing so would hide
  alert rendering during dark-mode transitions.
- Aux/status byte 2 carries main and muted volume nibbles when enough payload
  bytes are present to prove the byte is not the checksum.
- The renderer uses a 96 ms shared blink phase to match the V1 display cadence.

## Alert rows

Code anchors:

- `src/packet_parser_alerts.cpp`
- `test/test_packet_parser/test_packet_parser.cpp`

Stable invariants:

- The alert-row band value is carried in the low five bits of the band/arrow
  byte.
- Known band values are Laser `0x01`, Ka `0x02`, K `0x04`, X `0x08`, and
  Ku `0x10`.
- Direction is carried in the high bits of the same byte: front `0x20`, side
  `0x40`, rear `0x80`.
- Per-alert strengths are converted through the Valentine bargraph thresholds
  before being compressed to the six-bar secondary-card scale. (The main
  meter does not use these; it expands the display-data LED bitmap onto the
  local eight-slot meter.)
- The canonical library uses one-based alert-row indices. This firmware accepts
  both zero-based and one-based row numbering so captures from older firmware
  and third-party emulators remain parseable.
- Row byte offsets and aux0 bit semantics are pinned by the fenced
  `alert-row-layout` and `alert-aux0-bits` tables below (conformance-tested).
  `JUNK` gates on V1 firmware ≥ 4.1032 and `PHOTO_TYPE` on ≥ 4.1037.

## User settings bytes

Code anchors:

- `src/v1_profiles.h`
- `src/settings.h`
- `src/ble_commands.cpp`

Stable invariants:

- V1 Gen2 user settings are represented as six bytes.
- Settings profile code should treat those bytes as protocol data and avoid
  inventing alternate serialized forms.
- The per-bit meaning and polarity of bytes 0-3 are pinned by the fenced
  `user-bytes-bit-map` table below (conformance-tested against the real
  `V1UserSettings` accessors). This table is a Valentine's Law surface: the
  ALP laser handoff and auto-push mute-to-zero rewrite flip individual bits
  from it — see docs/VALENTINE_PHILOSOPHY.md.

## BLE GATT proxy surface

Code anchors:

- `include/config.h`
- `src/ble_proxy.cpp`
- `docs/API.md`

Stable invariants:

- The proxy mirrors the Valentine service UUID and characteristic UUIDs so
  companion apps see a V1-compatible attribute table.
- The short notify characteristic carries display packets.
- The long notify characteristic carries longer alert/response payloads.
- Writable characteristics forward companion-app writes to the upstream V1.

## Machine-readable spec tables

The fenced blocks below are the **single source of truth** for protocol decode
tables. `scripts/check_protocol_spec_tables.py` parses them, regenerates
`test/fixtures/protocol_spec_tables.h`, and fails CI if the committed fixture
header drifts from this document. `test_protocol_spec_conformance` then feeds
these tables through the real parse path, so the firmware's decode behavior is
tested against *documented protocol facts*, not against the implementation's
own beliefs. To change a table: edit it here, run
`python3 scripts/check_protocol_spec_tables.py --write`, and let the
conformance test arbitrate.

Provenance: values transcribed from VR's ESPLibrary2.0 semantics
(`InfDisplayData.getNumberOfLEDS()`, per-band bargraph thresholds), then
normalized for this project's eight-slot display. Live V1 Gen2 hardware shows
six visible source bars; this display renders source full-scale as all eight
local bars.

### Signal-bar LED bitmap → local display bars

Contiguous-from-LSB bitmap; `overflow` = any other byte (VR full-scale
sentinel). Values are the 0..8 bar count painted by this display, not the
physical source LED count.

<!-- spec-table: led-bitmap-bars -->
```text
0x00 -> 0
0x01 -> 1
0x03 -> 3
0x07 -> 4
0x0F -> 5
0x1F -> 7
0x3F -> 8
0x7F -> 8
0xFF -> 8
overflow -> 8
```

### Alert-row band values (low 5 bits of band/arrow byte)

<!-- spec-table: alert-band-values -->
```text
0x01 -> LASER
0x02 -> KA
0x04 -> K
0x08 -> X
0x10 -> KU
```

### Alert-row direction bits (high bits of band/arrow byte)

<!-- spec-table: alert-direction-bits -->
```text
0x20 -> FRONT
0x40 -> SIDE
0x80 -> REAR
```

### Per-band strength thresholds (raw RSSI → VR bargraph bars)

Each line is the **minimum raw value** for that VR bar count; raw `0x00` is
0 bars; any nonzero raw below the 2-bar threshold is 1 bar. Laser is always
8 bars regardless of raw value.

<!-- spec-table: strength-thresholds-ka -->
```text
0xBA -> 8
0xB3 -> 7
0xAC -> 6
0xA5 -> 5
0x9E -> 4
0x97 -> 3
0x90 -> 2
0x01 -> 1
```

<!-- spec-table: strength-thresholds-x -->
```text
0xD0 -> 8
0xC5 -> 7
0xBD -> 6
0xB4 -> 5
0xAA -> 4
0xA0 -> 3
0x96 -> 2
0x01 -> 1
```

<!-- spec-table: strength-thresholds-k -->
```text
0xC2 -> 8
0xB8 -> 7
0xAE -> 6
0xA4 -> 5
0x9A -> 4
0x90 -> 3
0x88 -> 2
0x01 -> 1
```

### VR bargraph → six-bar card compression

`cardBars = (vrBars * 6 + 4) / 8` (round half up). The main meter does NOT
use this table; it expands the display-data LED bitmap onto the local
eight-slot meter.

<!-- spec-table: vr-to-card-bars -->
```text
0 -> 0
1 -> 1
2 -> 2
3 -> 2
4 -> 3
5 -> 4
6 -> 5
7 -> 5
8 -> 6
```

### Alert-row byte layout (7-byte row payload after the index/count byte pair)

Byte offsets within one alert row as parsed by `parseAlertData`. Offset 0 is
the packed index/count byte (upper nibble = row index, lower nibble = total
rows in this table).

<!-- spec-table: alert-row-layout -->
```text
0 -> INDEX_COUNT
1 -> FREQ_MSB
2 -> FREQ_LSB
3 -> FRONT_RSSI
4 -> REAR_RSSI
5 -> BAND_ARROW
6 -> AUX0
```

### Alert-row aux0 bits

`JUNK` requires V1 firmware ≥ 4.1032; `PHOTO_TYPE` (low nibble, enum) requires
≥ 4.1037. Older firmware leaves those bits meaningless and the parser must
gate on the reported version.

<!-- spec-table: alert-aux0-bits -->
```text
0x80 -> PRIORITY
0x40 -> JUNK
0x0F -> PHOTO_TYPE
```

### User settings bytes — bit map

The six user bytes the firmware reads (`respUserBytes`) and writes
(`reqWriteUserBytes`). Factory default is all bytes `0xFF`. Polarity:
`direct` = bit set means feature ON; `inverted` = bit **clear** means feature
ON; `field` = multi-bit field (mask marks the field, value semantics in
`src/v1_profiles.h`). Bytes 4-5 carry no decoded fields in this firmware and
are passed through unchanged.

**Valentine's Law note:** `v1_profile_push_policy` flips `byte0/0x08 LASER`
on push when ALP owns laser alerting, and auto-push rewrites
`byte0/0x10 MUTE_TO_MUTE_VOLUME` from the slot toggle. A wrong row here means
a profile push could silently disable a detection band — the cardinal sin.
Any change to this table must be arbitrated by the conformance suite.

<!-- spec-table: user-bytes-bit-map -->
```text
byte0 0x01 direct -> X_BAND
byte0 0x02 direct -> K_BAND
byte0 0x04 direct -> KA_BAND
byte0 0x08 direct -> LASER
byte0 0x10 inverted -> MUTE_TO_MUTE_VOLUME
byte0 0x20 direct -> BOGEY_LOCK_LOUD
byte0 0x40 inverted -> MUTE_X_K_REAR
byte0 0x80 inverted -> KU_BAND
byte1 0x01 inverted -> EURO_MODE
byte1 0x02 direct -> K_VERIFIER
byte1 0x04 direct -> LASER_REAR
byte1 0x08 inverted -> CUSTOM_FREQS
byte1 0x10 inverted -> KA_ALWAYS_PRIORITY
byte1 0x20 direct -> FAST_LASER_DETECT
byte1 0xC0 field -> KA_SENSITIVITY
byte2 0x01 direct -> STARTUP_SEQUENCE
byte2 0x02 direct -> RESTING_DISPLAY
byte2 0x04 inverted -> BSM_PLUS
byte2 0x18 field -> AUTO_MUTE
byte2 0x60 field -> K_SENSITIVITY
byte2 0x80 inverted -> MRCT
byte3 0x03 field -> X_SENSITIVITY
byte3 0x04 inverted -> DRIVE_SAFE_3D
byte3 0x08 inverted -> DRIVE_SAFE_3D_HD
byte3 0x10 inverted -> REDFLEX_HALO
byte3 0x20 inverted -> REDFLEX_NK7
byte3 0x40 inverted -> EKIN
byte3 0x80 inverted -> PHOTO_VERIFIER
```
