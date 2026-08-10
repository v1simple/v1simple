# SD storage policy and 32 KB benchmark

V1Simple uses FAT32 for removable SD storage. Normal firmware boots retain the
newest 20 generated CSV files in each of `/perf`, `/alp`, and `/encounters`.
Maintenance boots do not prune evidence, and files that do not exactly match a
firmware-generated CSV name are never removed.

Each enabled logger warms its file at boot: directory creation, CSV create,
and header (plus the perf session marker) are written during setup, before BLE
connects.
These first writes carry the FAT-allocation cost — measured at 10–25 ms each on
a worn card — and paying them in setup keeps them off the shared SD path while
an alert is in flight. Warm-up failure is never fatal; every logger falls back
to its previous lazy create-on-first-write behavior.

## 32 KB allocation-unit experiment

Formatting is deliberately not a firmware feature. It destroys existing card
contents, so complete the experiment only after exporting and independently
checking a backup. Large cards may default to exFAT; confirm both **FAT32** and
an allocation unit of **32768 bytes** before returning a candidate card to the
device.

Use the same firmware revision, physical card, restored file set, power source,
and bench setup for both layouts:

1. Back up the card and verify that the copy contains every required file.
2. Format the card as the current FAT32 layout, restore the same file set, and
   run three core windows. Upload only for the first run:

   ```sh
   ./bench.sh --core --duration-seconds 300 --no-baseline
   ./bench.sh --core --duration-seconds 300 --no-baseline --no-upload
   ./bench.sh --core --duration-seconds 300 --no-baseline --no-upload
   ```

3. Back up anything newly required, format that same card as FAT32 with a
   32 KB allocation unit, and restore the identical starting file set.
4. Run the same three commands, using `--no-upload` for every run because the
   firmware revision must remain unchanged.
5. Compare `sd_start_max_peak_us`, `sd_runtime_max_peak_us`, `flushMax_us`, SD
   write-latency buckets, dropped perf rows, and the overall verdict across all
   three trials.

Adopt 32 KB only if it produces no new correctness or evidence failures and
improves typical runtime SD latency without making the worst trial materially
worse. A short clean-card comparison is directional evidence, not proof of
long-term aging behavior; continue watching the same metrics as the retained
20-file sets turn over in normal use.
