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

## C++ formatting

Firmware style is defined by `.clang-format` and enforced by
`scripts/check_clang_format.py`, which runs as a blocking step in
`scripts/ci-test.sh`. Every non-generated C/C++ file under `src/` and `include/`
must be byte-identical to clang-format output — 301 files, no opt-outs.

```sh
pip install "clang-format==22.1.8"       # once
python3 scripts/check_clang_format.py    # the gate
clang-format -i src/some_file.cpp        # fix a file
```

**The formatter is pinned to `clang-format==22.1.8`** (the PyPI wheel works on
Linux CI and macOS dev machines alike). The pin is load-bearing: different
clang-format releases format the same input differently, so an unpinned formatter
would make the gate pass on one machine and fail on another. The checker probes
`clang-format --version` and fails loudly on a mismatch. Bumping the pin means
bumping `.clang-format`, `.github/workflows/ci.yml` and this file in the same
commit, and reformatting the tree in that commit.

Generated/vendored data headers (`include/v1simple_logo.h`,
`include/warning_audio.h`, `include/Segment7Font.h`, `include/FreeSans*.h`) are
excluded in `.clang-format-ignore`. clang-format honors that file natively, so
your editor skips exactly what the gate skips — there is no second exclusion list
to drift out of sync.

`SortIncludes` is deliberately **off**: include order is load-bearing in this
Arduino/ESP-IDF codebase (`ble_connection.cpp` includes `ble_client.h` first on
purpose). Do not flip it as a drive-by — see the comment in `.clang-format`.

### git blame and the reformat commits

The config had existed for a long time but had never been applied: 277 of 294
files drifted from it. Applying it rewrote nearly every line without changing a
single instruction, so `git blame` needs to be told to skip those commits:

```sh
git config blame.ignoreRevsFile .git-blame-ignore-revs
```

The reformat landed in four reviewable slices (leaf modules → wifi/system →
`include/` + `src/` root → hot paths) rather than one big-bang commit, because a
repo-wide sweep would have invalidated the tracked mutation anchors wholesale and
produced an unreviewable diff. Each slice was verified byte-exactly equal to
clang-format(previous content), so no hand edit rode along inside a reformat.

**If you reformat, keep it in its own commit**, separate from behavior changes,
and add the hash to `.git-blame-ignore-revs`. Two things break when source text
moves, and both fail loudly if you forget them:

- `test/mutations/critical_mutations.json` anchors mutations on exact source
  text. Run `python3 scripts/mutation_test.py --critical --validate-only`; if an
  anchor no longer resolves, re-anchor it to the new text with the *same*
  semantic mutation, then run it for real and confirm it is still KILLED.
- Several suites and checkers assert against production source text, and
  `src/modules/*/api.md` cites source line numbers. A source-text assertion that
  stops matching anything passes vacuously — so prove it still fails on a real
  violation, don't just chase a green exit code.

## Pull requests

- Keep changes focused and include tests for new behavior.
- Follow the existing style (`.clang-format`, `.editorconfig`), and see the
  C++ formatting section above before reformatting anything.
- Update `docs/` and `CHANGELOG.md` when you change behavior or APIs.
- Note in the PR whether you verified on real hardware.
