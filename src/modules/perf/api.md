# perf Module API

Serial logging alias and the serial-debug gate. This module has no class, no runtime — it's a single header of preprocessor definitions used throughout the codebase.

The directory name `perf/` is historical; the contents are now narrower than the name suggests. Performance metrics themselves live outside this directory, in `src/perf_metrics.{h,cpp}` (counters, atomics and hot-path recording) plus the sections split out of it: `src/perf_snapshot_types.h` (snapshot payload types, re-included by `perf_metrics.h`), `src/perf_snapshot.cpp` (snapshot capture/populate), `src/perf_names.cpp` (reason/state name tables) and `src/perf_report.cpp` (periodic stack/CSV report). `src/perf_metrics_internal.h` carries the state shared between those translation units and is not a public header.

## File: `debug_macros.h`

**Header:** `src/modules/perf/debug_macros.h`.

### Aliases

#### `SerialLog`
Alias for `Serial` — for consistent logging when the underlying stream might change.
**Source:** `debug_macros.h:10`.

### Compile-gated debug macros

The `DEBUG_SERIAL` build flag gates a separate set of macros that *do* produce serial output when enabled.

#### `DEBUG_SERIAL`
Compile-time switch. Default: `0` (disabled in production builds). Enable with `-D DEBUG_SERIAL=1` in `platformio.ini build_flags`.
**Source:** `debug_macros.h`.

#### Gated macros (active only when `DEBUG_SERIAL=1`)

- `DBG_PRINT(...)` → `Serial.print(__VA_ARGS__)` or `((void)0)`.
- `DBG_PRINTLN(...)` → `Serial.println(__VA_ARGS__)` or `((void)0)`.
- `DBG_PRINTF(...)` → `Serial.printf(__VA_ARGS__)` or `((void)0)`.

**Source:** `debug_macros.h`.

## Notes for maintainers

`DEBUG_SERIAL=1` produces a lot of output. It is for bench debugging only, not for production firmware. Don't enable it in default `platformio.ini`.
