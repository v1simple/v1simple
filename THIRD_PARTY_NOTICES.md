# Third-Party Notices

This file collects attribution and license notices for material that this
project incorporates or derives from. It is required reading for redistribution.

---

## Trademarks

This project is independent and is **not affiliated with, endorsed by, or
sponsored by Valentine Research, Inc.** "Valentine One," "V1," and related
marks and logos are trademarks of Valentine Research, Inc. They are used in this
project only nominatively — to describe interoperability with that hardware.
"AL Priority" and any OBD/GPS hardware names are trademarks of their respective
owners and are likewise used only to describe interoperability.

---

## Upstream source — v1g2-t4s3 (MIT)

This project is derived from **kennygarreau/v1g2-t4s3**
(https://github.com/kennygarreau/v1g2-t4s3). Its license and copyright notice
are reproduced below as required by the MIT License.

```
MIT License

Copyright (c) 2024 Kenny Garreau

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## Bundled fonts

- **FreeSansBold** (`include/FreeSansBold24pt7b.h`) — generated from GNU FreeFont
  (FreeSans-Bold.ttf) using Adafruit's `fontconvert`. GNU FreeFont is licensed
  under the GNU GPL v3 with the font-embedding exception, which permits
  embedding the font in a document/product without that product becoming subject
  to the GPL. Source: https://www.gnu.org/software/freefont/
- **V1SevenX** (`include/Segment7Font.h`) — modified from **Segment7** by
  **Cedric Knight (2014)**,
  licensed under the **SIL Open Font License v1.1** (free for personal and
  commercial use with attribution). Source:
  https://fontlibrary.org/en/font/segment7 . The embedded copy is **modified**
  (custom glyphs: laser bars for `#`, a small "L" for `&`).
  **OFL compliance:** the modified font's internal names and C++ symbol have
  been renamed to `V1SevenX`/`V1SevenXFont`, which do not contain the original's
  Reserved Font Name. The OFL text is bundled at `licenses/OFL-1.1.txt`.

---

## Runtime libraries (fetched at build time via PlatformIO, not vendored)

- **ArduinoJson** — MIT — https://github.com/bblanchon/ArduinoJson
- **NimBLE-Arduino** — Apache-2.0 — https://github.com/h2zero/NimBLE-Arduino
- **GFX Library for Arduino** — see upstream — https://github.com/moononournation/Arduino_GFX
- **OpenFontRender** — MIT (bundles FreeType) — https://github.com/takkaO/OpenFontRender
- **pioarduino / platform-espressif32** — ESP32 Arduino core and toolchain

---

## Voice audio

The bundled voice clips — `tools/freq_audio/mulaw/*.mul`, `include/alert_audio.h`,
and `include/warning_audio.h` — are synthesized with the **Piper** neural TTS
tool (https://github.com/rhasspy/piper) using the **en_US-libritts_r-medium**
voice, which is built from the **LibriTTS-R** speech corpus. Piper is used only
as a build-time generator (like a compiler); it does not impose its license on
the generated audio.

The clips are therefore governed by the corpus/voice license:

* **LibriTTS-R — CC BY 4.0** (https://openslr.org/141/). Attribution: Y. Koizumi
  et al., "LibriTTS-R: A Restored Multi-Speaker Text-to-Speech Corpus," 2023.

The clips are freely redistributable under CC BY 4.0 with the attribution above.
Regenerate them with `tools/build_voice_clips.py` (see that file for setup). The
project no longer ships audio rendered from proprietary system voices such as
Apple "Samantha," which cannot be redistributed.
