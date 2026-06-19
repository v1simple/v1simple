# V1 Protocol References

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
  before being compressed to this display's six-bar scale.
- The canonical library uses one-based alert-row indices. This firmware accepts
  both zero-based and one-based row numbering so captures from older firmware
  and third-party emulators remain parseable.

## User settings bytes

Code anchors:

- `src/v1_profiles.h`
- `src/settings.h`
- `src/ble_commands.cpp`

Stable invariants:

- V1 Gen2 user settings are represented as six bytes.
- Settings profile code should treat those bytes as protocol data and avoid
  inventing alternate serialized forms.

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
