# Contributing to v1simple

Thanks for your interest! v1simple is an independent, community-built display
and BLE proxy for the Valentine 1 Gen 2. It is **not affiliated with Valentine
Research** — see `THIRD_PARTY_NOTICES.md`.

## Ground rules

- The public repo is a single squashed snapshot; there is no upstream
  development history to rebase onto.
- By contributing, you agree your contributions are licensed under the
  project's MIT License (see `LICENSE`).
- Be respectful and assume good faith in reviews.

## Development setup

**Firmware** (requires [PlatformIO](https://platformio.org/)):

```sh
pio run -e waveshare-349     # build the default firmware
pio test -e native          # native (host) unit tests
```

**Formatter** (pinned; the CI gate checks the version):

```sh
pip install "clang-format==22.1.8"
```

**Web UI** (`interface/`):

```sh
cd interface
npm ci
npm run test:coverage       # vitest + coverage
npm run build               # production build
npm run deploy              # regenerate data/ web + audio assets
```

Voice clips are generated with a redistributable TTS — see
`tools/build_voice_clips.py`. Do **not** commit audio rendered from proprietary
system voices (e.g. Apple "Samantha").

## The CI gate

`scripts/ci-test.sh` is the authoritative check, and the same script runs in
GitHub Actions. It covers native tests, frontend lint/tests/build, asset and
manifest guardrails, the firmware build, and memory-headroom checks. Please make
sure it passes locally before opening a pull request.

## C++ formatting: the clang-format ratchet

Firmware style is defined by `.clang-format` and enforced by
`scripts/check_clang_format.py`, which runs as a blocking step in
`scripts/ci-test.sh`.

**The formatter is pinned to `clang-format==22.1.8`** (install it with
`pip install "clang-format==22.1.8"`; the PyPI wheel works on Linux CI and macOS
dev machines alike). The pin is load-bearing: different clang-format releases
format the same input differently, so an unpinned formatter would make the gate
pass on one machine and fail on another. The checker probes
`clang-format --version` and fails loudly on a mismatch.

**Enforcement is a ratchet, not a repo-wide sweep.** When the gate landed, 277
of 294 non-generated firmware files drifted from clang-format output. A big-bang
reformat was rejected: it would rewrite nearly every line, invalidate the tracked
mutation-test anchors wholesale, and produce an unreviewable diff. So only the
files listed in `test/contracts/format_clean_manifest.txt` are gated, and that
list grows one file at a time:

```sh
python3 scripts/check_clang_format.py    # gate; also lists files ready to add
clang-format -i src/some_file.cpp        # format a file you are already touching
```

To migrate a file: format it, build and run the tests (formatting is not
*supposed* to change behavior, but macro-heavy code can surprise you — prove it),
then add its path to `test/contracts/format_clean_manifest.txt`, keeping the list
sorted. Commit the reformat plus its manifest entry on their own, separate from
behavior changes, so reviewers can skim the noise. A clean run of the checker
prints every unratcheted file that is already clean, so free wins are easy to
find.

**Entries are only ever added.** Removing a path is a regression, not a fix: if a
listed file drifts, reformat it. The checker also fails when a manifest entry
does not exist, so a rename must carry its entry along instead of silently
dropping out of the ratchet.

Generated/vendored data headers (`include/v1simple_logo.h`,
`include/warning_audio.h`, `include/Segment7Font.h`, `include/FreeSans*.h`) are
excluded in `.clang-format-ignore` and must never be added to the manifest.

`SortIncludes` is deliberately **off**: include order is load-bearing in this
Arduino/ESP-IDF codebase. Do not flip it as a drive-by — see the comment in
`.clang-format`.

## Pull requests

- Keep changes focused and include tests for new behavior.
- Follow the existing style (`.clang-format`, `.editorconfig`), and see the
  clang-format ratchet above before reformatting anything.
- Update `docs/` and `CHANGELOG.md` when you change behavior or APIs.
- Note in the PR whether you verified on real hardware.
