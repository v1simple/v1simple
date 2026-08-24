#pragma once

#include <cstddef>
#include <cstdint>

// Runtime bond persistence is best-effort and must never perform SD I/O on the
// caller. enqueueCurrentBleBondBackupSnapshot() captures the small NimBLE bond
// table in memory and hands it to a dedicated Core-0 writer through a bounded,
// latest-value-wins queue.
int enqueueCurrentBleBondBackupSnapshot();

// Fresh-flash migration waits for its queued Core-0 write before deleting
// NimBLE's NVS bonds. The caller performs no SD I/O and the wait is bounded.
int backupCurrentBleBondsViaCore0AtBoot();

// Drains/stops the Core-0 writer during graceful power-off. A blocked SD
// subsystem is bounded by timeoutMs from the caller's perspective.
void shutdownBleBondBackupWriter(uint32_t timeoutMs);

// Reopens writer admission when the hardware shutdown tail aborts. Existing
// queue/task state is preserved; later work reuses or lazily respawns it.
void resumeBleBondBackupWriterAfterAbortedShutdown();

#ifdef UNIT_TEST
void resetBleBondBackupWriterForTest();
bool runBleBondBackupWriterOnceForTest();
size_t bleBondBackupQueueDepthForTest();
size_t bleBondBackupSnapshotSizeForTest();
#endif
