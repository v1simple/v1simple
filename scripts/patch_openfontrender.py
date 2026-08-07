"""Apply the v1simple OpenFontRender first-char bearing patch (pre-build).

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

Fail-closed: if the vendored source matches neither the upstream block nor
the patched marker, the build stops — do not build with an unknown OFR.
"""

Import("env")  # noqa: F821  (SCons construction environment)

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


def apply_patch() -> None:
    source = (
        Path(env.subst("$PROJECT_LIBDEPS_DIR"))
        / env["PIOENV"]
        / "OpenFontRender"
        / "src"
        / "OpenFontRender.cpp"
    )
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


apply_patch()
