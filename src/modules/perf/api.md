# perf Module API

Serial logging alias and the serial-debug gate. This module has no class, no runtime — it's a single header of preprocessor definitions used throughout the codebase.

The directory name `perf/` is historical; the contents are now narrower than the name suggests. Performance metrics themselves live in `src/perf_metrics.{h,cpp}` outside this directory.

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
