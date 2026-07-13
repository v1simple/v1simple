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

- **ArduinoJson 7.4.3** — MIT — https://github.com/bblanchon/ArduinoJson
  — the exact installed license text is bundled at
  `licenses/ArduinoJson-LICENSE.txt`.
- **NimBLE-Arduino 2.5.0** — Apache-2.0 —
  https://github.com/h2zero/NimBLE-Arduino — the exact installed license and
  attribution notice are bundled at `licenses/NimBLE-Arduino-LICENSE.txt` and
  `licenses/NimBLE-Arduino-NOTICE.txt`.
- **GFX Library for Arduino** — BSD License — https://github.com/moononournation/Arduino_GFX
  — the upstream license text is bundled at
  `licenses/Arduino-GFX-LICENSE.txt`.
- **OpenFontRender** — primarily the FreeType Project License (FTL), with some
  separately identified non-FreeType files available under MIT —
  https://github.com/takkaO/OpenFontRender. The upstream wrapper notice is
  bundled at `licenses/OpenFontRender-LICENSE.txt`, and the complete FTL text
  is bundled at `licenses/FreeType-FTL.txt`. The project builds it with a local
  modification: the first-character bearing adjustment in `drawHString`
  (which read the stale FT_Face glyph slot under FTC caching) is removed for
  deterministic, standard typographic placement. The change is applied at
  build time by `scripts/patch_openfontrender.py`; see that file for rationale.
- **FreeType** (used by OpenFontRender) — FreeType Project License (FTL).
  This software is based in part on the work of the FreeType Team
  (https://www.freetype.org). Portions of this software are copyright the
  FreeType Project authors. All rights reserved. See
  `licenses/FreeType-FTL.txt`.
- **pioarduino / platform-espressif32** — ESP32 Arduino core, ESP-IDF
  precompiled libraries, and compiler runtimes. These framework, SDK,
  toolchain, and transitively linked components require a separate
  component-level license inventory; the direct-library texts above do not
  claim to complete that broader review.

---

## Web UI bundle (built into the LittleFS image)

The maintenance web interface shipped on the device's LittleFS filesystem is a
static build that embeds the following runtime code and generated CSS (all
fetched at build time via npm, not vendored):

- **Svelte 5.55.9 runtime** — MIT — https://github.com/sveltejs/svelte
  — the exact installed license text is bundled at
  `licenses/Svelte-LICENSE.md`.
- **SvelteKit 2.61.1 runtime** — MIT — https://github.com/sveltejs/kit
  — the exact installed license text is bundled at
  `licenses/SvelteKit-LICENSE.txt`.
- **daisyUI 5.5.20** (component CSS compiled into the bundle) — MIT —
  https://github.com/saadeghi/daisyui — the exact installed license text is
  bundled at `licenses/daisyUI-LICENSE.txt`.
- **Tailwind CSS 4.3.0** (utility CSS compiled into the bundle) — MIT —
  https://github.com/tailwindlabs/tailwindcss — the exact installed license
  text is bundled at `licenses/Tailwind-CSS-LICENSE.txt`.

Build-only tooling (Vite, Vitest, svelte-check, etc.) is not distributed in the
firmware image and is therefore not listed here.

The direct web-runtime texts above do not claim to cover transitive modules
that a framework compiler may emit into generated chunks. Those modules remain
part of the broader component-level inventory described above.

---

## Voice audio

The bundled voice clips — `tools/freq_audio/mulaw/*.mul` and
`include/warning_audio.h` — are synthesized with the **Piper** neural TTS
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
