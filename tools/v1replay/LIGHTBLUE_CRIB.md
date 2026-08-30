# LightBlue manual test — V1G-REPLAY

Exercise the BLE and display path by hand. `v1replay crib` prints the five draft
vectors below. `verify/verify_protocol.py` separately compiles the normal Swift
packet producer against the firmware parser; it does not verify these literal
crib bytes.

## Virtual device

Name: **V1G-REPLAY** (any name starting `V1G`, `V1C`, or `V1-` also satisfies
v1simple's name check — but the service UUID alone is enough, so the name is
cosmetic.)

Service `92A0AFF4-9E05-11E2-AA59-F23C91AEC05E`

| Ending | Properties | Purpose | Required by v1simple? |
|---|---|---|---|
| B2CE | Read, Notify | complete V1 packets up to 20 bytes, including display, alert rows, and short replies | **yes** — connection fails without it |
| B4E0 | Read, Notify | partial transport for packets over 20 bytes | optional; not used by these vectors |
| B6D4 | Write Without Response | commands from v1simple | **yes** (or BAD4) |
| B8D2 | Write Without Response | long commands | optional |
| BCE0 | Read, Notify | compatibility stub | no — companion apps only |
| BAD4 | Write, Write Without Response | alternate commands | fallback for B6D4 |

Full UUIDs are `92A0` + the ending + `-9E05-11E2-AA59-F23C91AEC05E`.

Do not add a `0x2902` descriptor by hand. LightBlue and CoreBluetooth create the
CCCD for notify characteristics automatically.

## Packets — draft framing

These match the hand-written draft: no checksum byte, `dest`/`src` = `DA E4`.
The firmware parser checks framing but does not validate destination, source,
or checksum, so these parse as written. Normal replay output uses checksums and
the protocol headers described below.

**Alert on B2CE** — synthetic Ka 34.700 GHz, front, priority, 1 bar:

```
AA DA E4 43 07 11 87 8C 80 00 22 80 AB
```

**Display on B2CE** — one bar:

```
AA DA E4 31 08 06 06 01 22 22 04 00 00 AB
```

**Display on B2CE** — six bars:

```
AA DA E4 31 08 06 06 3F 22 22 04 00 00 AB
```

**Version reply on B2CE** (after a `reqVersion` write):

```
respVersion    AA DA E4 02 07 76 34 2E 31 30 33 38 AB
```

**All-volume reply on B2CE** (after a `reqAllVolume` write):

```
respAllVolume  AA DA E4 3D 04 04 00 04 00 AB
```

The repeated current/saved values are this emulator fixture's configured state,
not a universal device default.

## Packets — fixture-compatible checksummed form

These examples retain the `DA E4` fixture-compatibility header while adding a
checksum byte and the V4.1028+ full eight-byte display payload so auxData2
carries current main/muted volume in its high/low nibbles; saved values are not
carried.
This is the same 9-byte payload region
`test_protocol_spec_conformance.cpp` builds — a 15-byte display packet is
already the house style, the 14-byte form above is the outlier.
These explicit fixture packets retain Aux1 `00`; normal v4.1038 playback carries
the current mode in Aux1 instead.

```
1 bar          AA DA E4 31 09 06 06 01 22 22 0C 00 40 3F AB
3 bars         AA DA E4 31 09 06 06 07 22 22 0C 00 40 45 AB
6 bars         AA DA E4 31 09 06 06 3F 22 22 0C 00 40 7D AB
idle           AA DA E4 31 09 38 38 00 00 00 0C 00 40 5E AB
alert 1 bar    AA DA E4 43 08 11 87 8C 80 00 22 80 F9 AB
alert 6 bars   AA DA E4 43 08 11 87 8C AF 00 22 80 28 AB
alert cleared  AA DA E4 43 08 00 00 00 00 00 00 00 B3 AB
```

Short version reply on B2CE:

```
respVersion    AA DA E4 02 08 76 34 2E 31 30 33 38 16 AB
```

Short all-volume reply on B2CE:

```
respAllVolume  AA DA E4 3D 05 04 00 04 00 B2 AB
```

`--header draft` selects the listed compatibility header. Playback defaults to
`D8 EA` for generated display/alert information and `D6 EA` for targeted
replies; `--blink-bogey --no-checksum --header draft` reproduces the draft
packets byte-for-byte. `--no-checksum` is outbound-only; manual commands into
the emulator remain checksummed.

## Bogey blink stimulus

Normal replay uses matching bogey image planes (`06 06`). `--blink-bogey`
selects the draft's differing planes (`06 00`) and exercises the firmware's
blink-refresh path.

## Reading the display packet

```
AA  D8  EA  31  09  06 06 3F 22 22 0C 0C 40  8D  AB
│   │   │   │   │   │  │  │  │  │  │  │  │   │   └ end
│   │   │   │   │   │  │  │  │  │  │  │  │   └ checksum (byte sum)
│   │   │   │   │   │  │  │  │  │  │  │  └ auxData2: V4.1028+ current only, main high / muted low
│   │   │   │   │   │  │  │  │  │  │  └ auxData1: 0C advanced-logic mode
│   │   │   │   │   │  │  │  │  │  └ auxData0: 04 system status + 08 display on
│   │   │   │   │   │  │  │  │  └ image2 — steady bits
│   │   │   │   │   │  │  │  └ image1 — 02 Ka + 20 front
│   │   │   │   │   │  │  └ LED bar bitmap — 3F = six bars
│   │   │   │   │   │  └ bogey image2
│   │   │   │   │   └ bogey image1 — 06 = '1'
│   │   │   │   └ payload length, counting the checksum
│   │   │   └ packet id — 31 infDisplayData
│   │   └ origin — E0 + 0A (V1)
│   └ destination — D8 broadcast information
└ start
```

The parser decodes `01`, `03`, `07`, `0F`, `1F`, `3F`, `7F`, and `FF` as
strengths 1 through 8. The main display has six cells, so strengths 7 and 8 are
clamped to a full six-cell meter.

**If bands and arrows do not appear**, check auxData0 bit 2. Clear it and the
parser blanks `activeBands` and `arrows` — the alert disappears with no other
symptom.

## Suggested sequence

1. Create the device, connect from v1simple, confirm it reaches CONNECTED and
   subscribes to B2CE.
2. Watch for writes on B6D4: `reqStartAlertData` (`0x41`), then `reqVersion`
   (`0x01`) and `reqAllVolume` (`0x3C`). Reply to the last two.
3. Notify the one-bar display packet. One bar should appear.
4. Notify the six-bar packet. The meter should jump to six.
5. Alternate 1 and 6 by hand a few times, then quickly, and watch for lag or a
   stuck meter.
6. Notify the alert packet on B2CE and confirm the alert card shows the
   synthetic Ka frequency and front direction.

Then use `.build/v1replay demo` for a generated multi-strength sequence.
