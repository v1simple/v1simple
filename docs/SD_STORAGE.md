# SD storage policy

V1Simple uses FAT32 for removable SD storage. Normal firmware boots retain the
newest 20 generated CSV files in each of `/perf`, `/alp`, and `/encounters`.
Maintenance boots do not prune evidence, and files that do not exactly match a
firmware-generated CSV name are never removed.

`sd_max_peak_us`, `sd_start_max_peak_us`, and `sd_runtime_max_peak_us` are
required diagnostic telemetry. They have no pass/fail upper bound because the
recorded corpus has not established a raw-duration value that causes a receive
or display failure. The verdict instead follows direct consequences: CSV
transfer/parse integrity, explicit perf/event/packet drop counters, parser
failures, receive/display continuity, reboots, and camera capture integrity.
Keep the emitted latency lines as diagnostic evidence; do not treat a raw peak
by itself as a product failure.

Each enabled logger warms its file at boot: directory creation, CSV create,
and header (plus the perf session marker) are written during setup, before BLE
connects.
These first writes carry the FAT-allocation cost — measured at 10–25 ms each on
a worn card — and paying them in setup keeps them off the shared SD path while
an alert is in flight. Warm-up failure is never fatal; every logger falls back
to its previous lazy create-on-first-write behavior.
