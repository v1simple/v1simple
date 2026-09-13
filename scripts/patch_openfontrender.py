"""Apply the v1simple OpenFontRender compatibility patches (pre-build).

Upstream OpenFontRender (pinned commit a9acf54) adjusts the first character
of every drawn/measured line by ``aface->glyph->metrics.horiBearingX`` — the
FT_Face glyph SLOT. FTC image-cache lookups do not populate that slot, so the
value belongs to whatever glyph the most recent cache MISS happened to load,
making first-character placement depend on render history. Measured on
hardware: the Segment7 '1' bogey counter drifted 22 px between bench phases
(runs 8a599c91 / 1533471d / 8f3a47bb), and OFR text measurements drifted the
same way.

The patch REMOVES the first-character adjustment at both sites (measure and
render) rather than correcting it. This project's layout math assumes
standard typographic placement — ink lands at pen + the glyph's own bearing —
which is also what fixed-cell 7-segment rendering requires: the narrow '1'
lives in the right of its digit cell, so "10.525" reads tight like the real
instrument instead of "1 0.525". The visually-correct renders before this
patch were exactly the cases where the stale bearing happened to be ~0,
i.e. no adjustment; the patch makes that the only behavior.

The pinned library also embeds FreeType 2.4.12.  Current production compiler
diagnostics expose three old-source issues: one helper compiled while all of
its callers are disabled, an intentional compact-span fallthrough chain with
no annotations, and FreeType's public/internal debug-hook typedef mismatch.
The warning cleanup preserves behavior, stays at those exact source sites,
and is guarded by whole-file fingerprints.  It does not weaken diagnostics
for project code or any other dependency code.

Fail-closed: if vendored source matches neither the expected upstream source
nor the exact patched source, the build stops — do not build with an unknown
OFR.
"""

Import("env")  # noqa: F821  (SCons construction environment)

import hashlib
from pathlib import Path

MARKER = "v1simple-ofr-bearing-patch-v2"
LEGACY_MARKER = "v1simple-ofr-bearing-patch]"  # v1: substituted bearing instead of removing

MEASURE_UPSTREAM = """\t\t// Adjust for horizontal bearing if it's the first character in the line
\t\tif (isFirstChar) {
\t\t\tFT_Face aface;
\t\t\terror = FTC_Manager_LookupFace(_ftc_manager, &_face_id, &aface);
\t\t\tif (error) return 0;
\t\t\tint32_t horiBearingX = aface->glyph->metrics.horiBearingX >> 6;
\t\t\tcurrent_x -= horiBearingX; // Adjust starting position
\t\t\tisFirstChar = false;
\t\t}"""

MEASURE_PATCHED = """\t\t// [v1simple-ofr-bearing-patch-v2] Upstream subtracted a "first character
\t\t// bearing" here, read from the FT_Face glyph SLOT (aface->glyph),
\t\t// which FTC image-cache lookups do not populate — the value belonged
\t\t// to whatever glyph the last cache MISS left behind, so first-char
\t\t// placement and measurement depended on render history. This
\t\t// project's layout math expects standard typographic placement
\t\t// (ink = pen + glyph bearing), so the adjustment is removed rather
\t\t// than corrected. See scripts/patch_openfontrender.py.
\t\tif (isFirstChar) {
\t\t\tisFirstChar = false;
\t\t}"""

RENDER_UPSTREAM = """\t\t\t\t// Adjust for horizontal bearing if it's the first character in the line
\t\t\t\tif (isFirstCharInLine) {
\t\t\t\t\tFT_Face aface;
\t\t\t\t\tFTC_Manager_LookupFace(_ftc_manager, &_face_id, &aface);
\t\t\t\t\tint32_t horiBearingX = aface->glyph->metrics.horiBearingX >> 6;
\t\t\t\t\tcurrentX -= horiBearingX; // Adjust starting position
\t\t\t\t\tisFirstCharInLine = false;
\t\t\t\t}"""

RENDER_PATCHED = """\t\t\t\t// [v1simple-ofr-bearing-patch-v2] See the measure-pass note: upstream's
\t\t\t\t// first-char adjustment read the stale face slot and fought this
\t\t\t\t// project's fixed-cell layout math; removed rather than corrected.
\t\t\t\tif (isFirstCharInLine) {
\t\t\t\t\tisFirstCharInLine = false;
\t\t\t\t}"""


CMAP_UPSTREAM_SHA256 = "ba857d5f126eea6c00a53856b0b7f4aa8bd6dbcfc842c6eb5ce3ef7cf7a62f6a"
CMAP_PATCHED_SHA256 = "992f6349cac92ba60cf17e015c431d76e6ca4ff0f3f37f0450a2a53b1276a9db"
CMAP_UPSTREAM = """  FT_CALLBACK_DEF( FT_Error )
  tt_cmap_init( TT_CMap   cmap,
                FT_Byte*  table )
  {
    cmap->data = table;
    return FT_Err_Ok;
  }"""
CMAP_PATCHED = """  /* v1simple-ofr-warning-cleanup-v1: this generic initializer is used
   * only by the optional formats named below.  The pinned build enables
   * format 4, which has its own initializer.
   */
#if defined( TT_CONFIG_CMAP_FORMAT_0 )  || \\
    defined( TT_CONFIG_CMAP_FORMAT_2 )  || \\
    defined( TT_CONFIG_CMAP_FORMAT_6 )  || \\
    defined( TT_CONFIG_CMAP_FORMAT_8 )  || \\
    defined( TT_CONFIG_CMAP_FORMAT_10 )
  FT_CALLBACK_DEF( FT_Error )
  tt_cmap_init( TT_CMap   cmap,
                FT_Byte*  table )
  {
    cmap->data = table;
    return FT_Err_Ok;
  }
#endif"""

GRAYS_UPSTREAM_SHA256 = "fc6c48868e7c97d8a1f666ae39c2a1f52be91373b04ea6a9d24ec9e8f8e16a93"
GRAYS_PATCHED_SHA256 = "c84e6a4fd9270b72c23137de129bdbd5251808394ada066fbe86678fb689a2b5"
GRAYS_UPSTREAM = """          switch ( spans->len )
          {
          case 7: *q++ = (unsigned char)coverage;
          case 6: *q++ = (unsigned char)coverage;
          case 5: *q++ = (unsigned char)coverage;
          case 4: *q++ = (unsigned char)coverage;
          case 3: *q++ = (unsigned char)coverage;
          case 2: *q++ = (unsigned char)coverage;
          case 1: *q   = (unsigned char)coverage;"""
GRAYS_PATCHED = """          /* v1simple-ofr-warning-cleanup-v1: the compact span writer
           * deliberately fills each remaining byte through this fallthrough
           * chain.  Make that control flow explicit to current compilers.
           */
          switch ( spans->len )
          {
          case 7: *q++ = (unsigned char)coverage;
                  /* fall through */
          case 6: *q++ = (unsigned char)coverage;
                  /* fall through */
          case 5: *q++ = (unsigned char)coverage;
                  /* fall through */
          case 4: *q++ = (unsigned char)coverage;
                  /* fall through */
          case 3: *q++ = (unsigned char)coverage;
                  /* fall through */
          case 2: *q++ = (unsigned char)coverage;
                  /* fall through */
          case 1: *q   = (unsigned char)coverage;"""

TTOBJS_UPSTREAM_SHA256 = "aba4f90928a4a4dd6bc7d85e1d614f197955ba86eb6756ac11082d2fae31d44f"
TTOBJS_PATCHED_SHA256 = "18a6953b25b3a91876db4a44491be0a7b89468ce3d2e18f01bae3caa9341df44"
TTOBJS_UPSTREAM = """      face->interpreter = (TT_Interpreter)
                            library->debug_hooks[FT_DEBUG_HOOK_TRUETYPE];"""
TTOBJS_PATCHED = """      /* v1simple-ofr-warning-cleanup-v1: FreeType 2.4.12 exposes
       * debug hooks as void callbacks while this internal interpreter slot
       * returns FT_Error.  Keep the upstream ABI bridge, but scope its
       * compiler diagnostic to this exact cast.
       */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored \"-Wcast-function-type\"
      face->interpreter = (TT_Interpreter)
                            library->debug_hooks[FT_DEBUG_HOOK_TRUETYPE];
#pragma GCC diagnostic pop"""


# The v1 patch (substituted the processed glyph's true bearing instead of
# removing the adjustment). Machines that built between v1 and v2 carry
# these blocks in .pio/libdeps; upgrade them in place.
V1_MEASURE = """\t\t// Adjust for horizontal bearing if it's the first character in the line
\t\t// [v1simple-ofr-bearing-patch] Upstream read horiBearingX from the
\t\t// face's glyph SLOT (aface->glyph), which FTC image-cache lookups do
\t\t// not populate; the value belonged to whatever glyph the last cache
\t\t// MISS left behind, so first-char placement depended on render
\t\t// history. Use the true bearing of the glyph being measured: its
\t\t// CBox xMin, computed above before the current_x offset is applied.
\t\t// See scripts/patch_openfontrender.py.
\t\tif (isFirstChar) {
\t\t\tcurrent_x -= glyph_bbox.xMin; // Adjust starting position
\t\t\tisFirstChar = false;
\t\t}"""

V1_RENDER = """\t\t\t\t// Adjust for horizontal bearing if it's the first character in the line
\t\t\t\t// [v1simple-ofr-bearing-patch] See the measure-pass note: the face
\t\t\t\t// slot is stale under FTC. The bitmap glyph converted above carries
\t\t\t\t// the true bearing in ->left.
\t\t\t\tif (isFirstCharInLine) {
\t\t\t\t\tcurrentX -= ((FT_BitmapGlyph)aglyph)->left; // Adjust starting position
\t\t\t\t\tisFirstCharInLine = false;
\t\t\t\t}"""


def _fail(message: str) -> None:
    print(f"[patch_openfontrender] ERROR: {message}")
    env.Exit(1)


def open_font_render_root() -> Path:
    return (
        Path(env.subst("$PROJECT_LIBDEPS_DIR"))
        / env["PIOENV"]
        / "OpenFontRender"
        / "src"
    )


def apply_bearing_patch() -> None:
    source = open_font_render_root() / "OpenFontRender.cpp"
    if not source.exists():
        _fail(
            f"{source} not found. If dependencies have not been installed yet, "
            "run `pio pkg install` (or `pio run` once) and build again — the "
            "firmware must not be built with an unpatched OpenFontRender."
        )

    text = source.read_text(encoding="utf-8")
    if MARKER in text:
        print("[patch_openfontrender] already applied (v2)")
        return

    if V1_MEASURE in text and V1_RENDER in text:
        text = text.replace(V1_MEASURE, MEASURE_PATCHED, 1)
        text = text.replace(V1_RENDER, RENDER_PATCHED, 1)
        source.write_text(text, encoding="utf-8")
        print("[patch_openfontrender] upgraded v1 -> v2 (adjustment removed)")
        return

    if MEASURE_UPSTREAM not in text or RENDER_UPSTREAM not in text:
        _fail(
            "vendored OpenFontRender does not match the expected upstream "
            "blocks and carries no patch marker. The pinned commit changed; "
            "re-evaluate the patch before building (see this script's header)."
        )

    text = text.replace(MEASURE_UPSTREAM, MEASURE_PATCHED, 1)
    text = text.replace(RENDER_UPSTREAM, RENDER_PATCHED, 1)
    source.write_text(text, encoding="utf-8")
    print("[patch_openfontrender] applied first-char bearing patch")


def apply_warning_patch(relative_path: str, upstream_sha256: str, patched_sha256: str,
                        upstream_block: str, patched_block: str) -> None:
    source = open_font_render_root() / relative_path
    if not source.is_file():
        _fail(f"missing pinned warning source: {source}")

    text = source.read_text(encoding="utf-8")
    actual = hashlib.sha256(text.encode("utf-8")).hexdigest()
    if actual == patched_sha256:
        if text.count(patched_block) != 1:
            _fail(f"existing warning patch is incomplete: {relative_path}")
        print(f"[patch_openfontrender] warning cleanup already applied: {relative_path}")
        return

    if actual != upstream_sha256:
        _fail(
            f"pinned warning source identity mismatch for {relative_path}: "
            f"expected pristine or patched source, got {actual}"
        )
    if text.count(upstream_block) != 1 or text.count(patched_block) != 0:
        _fail(f"pinned warning source does not contain the unique expected block: {relative_path}")

    text = text.replace(upstream_block, patched_block, 1)
    generated = hashlib.sha256(text.encode("utf-8")).hexdigest()
    if generated != patched_sha256:
        _fail(
            f"generated warning source identity mismatch for {relative_path}: "
            f"expected {patched_sha256}, got {generated}"
        )
    source.write_text(text, encoding="utf-8")
    print(f"[patch_openfontrender] applied warning cleanup: {relative_path}")


def apply_warning_patches() -> None:
    apply_warning_patch(
        "sfnt/ttcmap.c", CMAP_UPSTREAM_SHA256, CMAP_PATCHED_SHA256,
        CMAP_UPSTREAM, CMAP_PATCHED,
    )
    apply_warning_patch(
        "smooth/ftgrays.c", GRAYS_UPSTREAM_SHA256, GRAYS_PATCHED_SHA256,
        GRAYS_UPSTREAM, GRAYS_PATCHED,
    )
    apply_warning_patch(
        "truetype/ttobjs.c", TTOBJS_UPSTREAM_SHA256, TTOBJS_PATCHED_SHA256,
        TTOBJS_UPSTREAM, TTOBJS_PATCHED,
    )


apply_bearing_patch()
apply_warning_patches()
