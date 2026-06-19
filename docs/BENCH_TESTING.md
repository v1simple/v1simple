# Bench Testing

`./bench.sh` is the maintained bench hardware evidence command.

It exists to answer two bench questions only:

1. Is the production-image core runtime healthy while metrics are recorded to SD?
2. Is the production-image display pipeline healthy while metrics are recorded to SD?

It intentionally does **not** qualify OBD/proxy physical behavior, compare
against certified/moving baselines, or emit release-qualification verdicts.
It can compare against one locally promoted baseline per suite for variance
visibility; that baseline is not a certification gate. Broader qualification
concerns made previous bench runs ambiguous and were removed from this path.

Live collection builds and uploads the production `waveshare-349` firmware
through `build.sh` before the first window unless `--no-upload` is passed. That
keeps bench evidence tied to the same firmware environment that release ships.

## Commands

```bash
./bench.sh                         # core + display, live SD/serial collection
./bench.sh --core                  # core only
./bench.sh --display               # display only
./bench.sh --duration-seconds 600  # longer windows
./bench.sh --no-upload             # use firmware already on the board
./bench.sh --from-csv perf.csv --display
./bench.sh --promote-baseline      # save this PASS as the local baseline
./bench.sh --no-baseline           # skip local baseline comparison
```

Exit codes:

| result | exit | meaning |
|---|---:|---|
| `PASS` | 0 | collection succeeded; no hard/advisory failures |
| `WARN` | 1 | collection succeeded; advisory warning present |
| `FAIL` | 2 | collection succeeded; hard metric/SLO failure present |
| `COLLECTION_FAILED` | 3 | serial/CSV/import/scoring artifact missing or invalid |

## Artifacts

Runs are written to:

```text
.artifacts/bench/<board-id>/runs/<timestamp>_<git-sha>/
```

Important files:

| path | purpose |
|---|---|
| `bench.log` | full run log |
| `bench_result.json` | machine-readable result |
| `bench_summary.txt` | human-readable result |
| `core/` | core CSV, import, scoring, diagnostics |
| `display/` | display CSV, import, scoring, diagnostics |

## Result language

A useful bench failure should look like this:

```text
bench result: FAIL
collection: PASS
core: PASS (61 rows, 300.0s)
display: FAIL (61 rows, 300.0s)

failed:
  display.queue_drops_delta current=1 state=fail messages=value 1 above max 0

artifacts:
  /path/to/.artifacts/bench/release/runs/<run-id>
```

There is no `NO_BASELINE`, `qualification passed`, `physical coverage missing`,
`proxy_rows`, or `obd_rows` in bench output.

## Relationship to other hardware tests

Device-unit tests still live under `scripts/run_device_tests.sh` and validate
boot, heap, PSRAM, RTOS, NVS, battery, and radio/coexistence behavior on real
hardware.

Bench testing is separate from those device tests. Bench testing runs the
production image and measures runtime/display behavior through SD perf CSV
exports.
