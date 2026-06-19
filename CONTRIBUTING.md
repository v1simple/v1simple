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

## Pull requests

- Keep changes focused and include tests for new behavior.
- Follow the existing style (`.clang-format`, `.editorconfig`).
- Update `docs/` and `CHANGELOG.md` when you change behavior or APIs.
- Note in the PR whether you verified on real hardware.
