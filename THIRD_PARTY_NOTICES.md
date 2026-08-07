# Third-Party Notices

This is a source-tree inventory, not a complete legal conclusion about every
component in a built artifact. Retained license texts are in `licenses/`.

V1Simple is independent of Valentine Research, Inc. “Valentine One” and “V1”
are used only to identify interoperable hardware. Other product names belong
to their respective owners.

## Upstream project

V1Simple derives from
[kennygarreau/v1g2-t4s3](https://github.com/kennygarreau/v1g2-t4s3):

```text
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

## Bundled assets

- `include/FreeSansBold24pt7b.h` was generated from GNU FreeFont's
  FreeSans-Bold and is identified as GPLv3 with the font exception by the
  [GNU FreeFont project](https://www.gnu.org/software/freefont/). See
  `licenses/GNU-FreeFont-COPYING.txt` and
  `licenses/GNU-FreeFont-README.txt`.
- `include/Segment7Font.h` contains V1SevenX, a renamed and modified Segment7
  font by Cedric Knight (2014), under the SIL Open Font License 1.1. See
  `licenses/OFL-1.1.txt`.
- `tools/build_voice_clips.py` identifies the bundled voice clips as generated
  with Piper's `en_US-libritts_r-medium` voice from LibriTTS-R, CC BY 4.0.
  Attribution: Y. Koizumi et al., “LibriTTS-R: A Restored Multi-Speaker
  Text-to-Speech Corpus,” 2023. See `licenses/CC-BY-4.0.txt`; source:
  [OpenSLR 141](https://openslr.org/141/).

## Direct firmware dependencies

Versions below come from `platformio.ini`:

- ArduinoJson 7.4.3 — MIT — `licenses/ArduinoJson-LICENSE.txt`
- NimBLE-Arduino 2.5.0 — Apache-2.0 —
  `licenses/NimBLE-Arduino-LICENSE.txt` and
  `licenses/NimBLE-Arduino-NOTICE.txt`
- GFX Library for Arduino 1.6.5 — BSD —
  `licenses/Arduino-GFX-LICENSE.txt`
- OpenFontRender commit `a9acf5498ed058a034acaa1becbad790627b9ec1` —
  FreeType Project License, with separately marked MIT files —
  `licenses/OpenFontRender-LICENSE.txt` and `licenses/FreeType-FTL.txt`.
  This software is based in part on the work of the FreeType Team.
  `scripts/patch_openfontrender.py` applies a local source change at build time.

The pinned ESP32 platform also supplies the Arduino core, ESP-IDF, toolchain,
and transitive components. This file does not establish their complete
component-level inventory.

## Maintenance web bundle

The lockfile resolves these direct build inputs:

- Svelte 5.55.9 — MIT — `licenses/Svelte-LICENSE.md`
- SvelteKit 2.61.1 — MIT — `licenses/SvelteKit-LICENSE.txt`
- daisyUI 5.5.20 — MIT — `licenses/daisyUI-LICENSE.txt`
- Tailwind CSS 4.3.0 — MIT — `licenses/Tailwind-CSS-LICENSE.txt`

Those entries do not establish the complete contents of generated web chunks
or enumerate build-only packages.
