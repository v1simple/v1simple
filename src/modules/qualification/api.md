# Serial Bench Module API

Serial-controlled SD bench runner for unattended hardware measurements without
live Wi-Fi metrics.

## File: `qualification_serial_module.{h,cpp}`

### Purpose

Runs bounded core/display bench windows in normal runtime while Wi-Fi
remains off. Metrics are written only to the SD-backed perf CSV. After the
window ends, the module pauses further CSV snapshot emission, closes the current
CSV file, and exports that CSV over USB serial on request.

### Serial commands

All commands are newline-terminated ASCII. Responses are newline-delimited and
prefixed so the host can ignore unrelated boot/runtime serial logs.

- `QSTART core <seconds>` — start a core soak window.
- `QSTART display <seconds>` — start a display-preview soak window using the
  internal display preview driver.
- `QSTATUS` — report state, suite, elapsed time, and CSV path.
- `QGETCSV [path]` — export the last run CSV, or an explicit absolute `/perf/...`
  path, as hex chunks.
- `QABORT` — stop the active qualification run and pause CSV capture.

### Response prefixes

- `QRESP {json}` — direct command response.
- `QEVENT {json}` — asynchronous state transition, such as finalizing/done.
- `QERR {json}` — command or state error.
- `QFILE {json}` — CSV export header with path and size.
- `QCHUNK <seq> <hex>` — one CSV data chunk.
- `QEND {json}` — export completion with byte count, chunk count, and CRC32.

### Safety constraints

- No NVS plan/state is written.
- No live metric polling is exposed or required.
- During the measured window, host tooling should remain quiet; the module only
  uses serial for start/end/export control.
- CSV export is chunked from the main loop using non-blocking SD try-locks.
