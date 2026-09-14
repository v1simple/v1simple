#pragma once

// Bench negative control.
//
// A green bench run only carries information if a red one is reachable. These
// builds corrupt exactly one rendered field so scripts/bench encounter-check
// must report a difference for that field and no other. Proving the reader can
// fail is what makes NO_DIFFERENCES_OBSERVED evidence rather than silence.
//
//   1 = main_bars          (src/display_bands.cpp)
//   2 = main_arrows        (src/display_arrow.cpp)
//   3 = primary_frequency  (src/display_frequency.cpp)
//
// Never set in a shipping build. scripts/build_production_artifacts.sh builds
// only -e waveshare-349, which does not define this. scripts/bench/run_window.py
// selects the isolated fault environment, retains that environment's exact
// image, and binds this value independently of the source commit identity.

#ifndef BENCH_FAULT_INJECT
#define BENCH_FAULT_INJECT 0
#endif

#if BENCH_FAULT_INJECT < 0 || BENCH_FAULT_INJECT > 3
#error "BENCH_FAULT_INJECT must be 0 (off), 1 (main_bars), 2 (main_arrows), or 3 (primary_frequency)"
#endif
