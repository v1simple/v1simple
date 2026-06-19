#!/usr/bin/env python3
"""Parse soak metrics JSONL into key=value summary fields."""

import argparse
import json
import sys
from datetime import datetime

from metric_derivation import clamp_window_bounds, percentile, window_peak  # type: ignore
from metric_schema import DISPLAY_COUNTER_DELTA_MAPPINGS, DISPLAY_SAMPLE_FIELD_MAPPINGS  # type: ignore


def update_min(cur, val):
    if val is None:
        return cur
    if cur is None or val < cur:
        return val
    return cur


def update_max(cur, val):
    if val is None:
        return cur
    if cur is None or val > cur:
        return val
    return cur


def num(v):
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, (int, float)):
        return v
    return None


def emit(key, val):
    if val is None:
        print(f"{key}=")
    else:
        print(f"{key}={val}")


def parse_ts_epoch(ts):
    if not isinstance(ts, str) or not ts:
        return None
    try:
        if ts.endswith("Z"):
            return datetime.fromisoformat(ts.replace("Z", "+00:00")).timestamp()
        return datetime.fromisoformat(ts).timestamp()
    except ValueError:
        return None


def to_int_ms(delta_seconds):
    if delta_seconds is None:
        return None
    return int(round(delta_seconds * 1000.0))


def compute_window_stats(records, start_idx, end_idx):
    start_idx, end_idx = clamp_window_bounds(len(records), start_idx, end_idx)
    window = records[start_idx:end_idx]
    wifi_vals = [rec["wifi"] for rec in window if rec["wifi"] is not None]
    loop_vals = [rec["loop"] for rec in window if rec["loop"] is not None]
    disp_vals = [rec["disp"] for rec in window if rec["disp"] is not None]
    return {
        "samples": len(window),
        "wifi_peak": max(wifi_vals) if wifi_vals else None,
        "wifi_p95": percentile(wifi_vals, 95.0) if wifi_vals else None,
        "loop_peak": max(loop_vals) if loop_vals else None,
        "loop_p95": percentile(loop_vals, 95.0) if loop_vals else None,
        "disp_peak": max(disp_vals) if disp_vals else None,
        "disp_p95": percentile(disp_vals, 95.0) if disp_vals else None,
    }

def update_counter_windows(data, first_map, last_map, mappings):
    for data_key, _output_key in mappings:
        val = num(data.get(data_key))
        if val is None:
            continue
        if first_map.get(data_key) is None:
            first_map[data_key] = val
        last_map[data_key] = val


def parse_args(argv):
    parser = argparse.ArgumentParser(description="Parse soak metrics JSONL into key=value fields")
    parser.add_argument("metrics_jsonl", help="Path to metrics.jsonl")
    parser.add_argument(
        "--wifi-threshold",
        type=float,
        default=None,
        help="Optional wifiMaxUs threshold used to emit over-limit sample counts",
    )
    parser.add_argument(
        "--disp-threshold",
        type=float,
        default=None,
        help="Optional dispPipeMaxUs threshold used to emit over-limit sample counts",
    )
    parser.add_argument(
        "--ble-threshold",
        type=float,
        default=None,
        help="Optional bleProcessMaxUs threshold used for connect-burst stability diagnostics",
    )
    parser.add_argument(
        "--skip-first-wifi-samples",
        type=int,
        default=2,
        help="Number of initial wifi samples to exclude for warmup-adjusted robust metrics",
    )
    parser.add_argument(
        "--stable-consecutive-samples",
        type=int,
        default=2,
        help="Consecutive in-threshold samples required to mark a transition as stabilized",
    )
    parser.add_argument(
        "--exclude-tail-samples-for-minima",
        type=int,
        default=0,
        help="Ignore the last N ok samples when computing minima used by floor gates",
    )
    parser.add_argument(
        "--dma-largest-floor",
        type=float,
        default=None,
        help="Optional heapDmaLargest floor for below-floor sample/streak diagnostics",
    )
    parser.add_argument(
        "--connect-burst-disp-threshold",
        type=float,
        default=None,
        help="Optional dispPipeMaxUs threshold used for connect-burst stability diagnostics",
    )
    parser.add_argument(
        "--connect-burst-consecutive-samples",
        type=int,
        default=3,
        help="Consecutive in-threshold samples required to mark the first-connected burst as settled",
    )
    return parser.parse_args(argv)


def main() -> int:
    args = parse_args(sys.argv[1:])
    path = args.metrics_jsonl
    samples = 0
    ok_samples = 0

    heap_free_min = None
    heap_min_free_min = None
    heap_dma_min = None
    heap_dma_largest_min = None
    latency_max_peak = None
    proxy_drop_peak = None
    display_updates_first = None
    display_updates_last = None
    display_skips_first = None
    display_skips_last = None
    flush_max_peak = None
    loop_max_peak = None
    notify_to_display_max_ms = None
    notify_to_display_sample_count = 0
    notify_to_display_seen = False
    wifi_max_peak = None
    wifi_max_peak_excluding_first = None
    ble_drain_max_peak = None
    loop_peak_ts = ""
    loop_peak_wifi = None
    loop_peak_flush = None
    loop_peak_ble_drain = None
    loop_peak_display_updates = None
    loop_peak_rx_packets = None
    wifi_peak_ts = ""
    wifi_peak_excluding_first_ts = ""
    wifi_peak_loop = None
    wifi_peak_flush = None
    wifi_peak_ble_drain = None
    wifi_peak_display_updates = None
    wifi_peak_rx_packets = None
    flush_peak_ts = ""
    ble_drain_peak_ts = ""
    rx_packets_first = None
    rx_packets_last = None
    parse_successes_first = None
    parse_successes_last = None
    parse_failures_first = None
    parse_failures_last = None
    parse_resyncs_first = None
    parse_resyncs_last = None
    queue_drops_first = None
    queue_drops_last = None
    perf_drop_first = None
    perf_drop_last = None

    event_publish_first = None
    event_publish_last = None
    event_drop_first = None
    event_drop_last = None
    event_size_peak = None
    core_guard_tripped_count = 0

    # Additional SLO-aligned metrics (available in debug/metrics API)
    oversize_drops_first = None
    oversize_drops_last = None
    sd_max_peak = None
    fs_max_peak = None
    queue_high_water_first = None
    queue_high_water_peak = None
    wifi_connect_deferred_first = None
    wifi_connect_deferred_last = None
    reconnects_first = None
    reconnects_last = None
    disconnects_first = None
    disconnects_last = None
    dma_free_min_val_raw = None
    dma_largest_min_val_raw = None
    dma_free_samples = []
    dma_largest_samples = []
    dma_largest_current_samples = []
    dma_largest_to_free_pct_samples = []
    dma_fragmentation_pct_samples = []
    dma_largest_below_floor_samples = 0
    dma_largest_below_floor_longest_streak = 0
    dma_largest_below_floor_streak = 0
    ble_process_max_peak = None
    disp_pipe_max_peak = None
    ble_mutex_timeout_first = None
    ble_mutex_timeout_last = None
    wifi_samples = []
    disp_pipe_samples = []
    wifi_ap_up_first = None
    wifi_ap_up_last = None
    wifi_ap_down_first = None
    wifi_ap_down_last = None
    proxy_adv_on_first = None
    proxy_adv_on_last = None
    proxy_adv_off_first = None
    proxy_adv_off_last = None
    wifi_ap_active_samples = 0
    wifi_ap_inactive_samples = 0
    proxy_adv_on_samples = 0
    proxy_adv_off_samples = 0
    wifi_peak_ap_active = None
    wifi_peak_ap_inactive = None
    wifi_peak_proxy_adv_on = None
    wifi_peak_proxy_adv_off = None
    wifi_ap_last_reason_code = None
    wifi_ap_last_reason = ""
    proxy_adv_last_reason_code = None
    proxy_adv_last_reason = ""
    sample_records = []
    display_counter_first = {}
    display_counter_last = {}
    wifi_ap_down_event_indices = []
    proxy_adv_off_event_indices = []
    prev_wifi_ap_down = None
    prev_proxy_adv_off = None

    try:
        with open(path, "r", encoding="utf-8") as f:
            for raw in f:
                line = raw.strip()
                if not line:
                    continue
                samples += 1
                try:
                    rec = json.loads(line)
                except Exception:
                    continue
                if not rec.get("ok"):
                    continue
                data = rec.get("data")
                if not isinstance(data, dict):
                    continue
                ok_samples += 1

                heap_free_min = update_min(heap_free_min, num(data.get("heapFree")))
                heap_min_free_min = update_min(heap_min_free_min, num(data.get("heapMinFree")))
                heap_dma_min = update_min(heap_dma_min, num(data.get("heapDma")))
                heap_dma_largest_min = update_min(heap_dma_largest_min, num(data.get("heapDmaLargest")))
                latency_max_peak = update_max(latency_max_peak, num(data.get("latencyMaxUs")))
                flush_val = num(data.get("flushMaxUs"))
                loop_val = num(data.get("loopMaxUs"))
                wifi_val = num(data.get("wifiMaxUs"))
                ble_drain_val = num(data.get("bleDrainMaxUs"))
                disp_pipe_val = num(data.get("dispPipeMaxUs"))
                ble_process_val = num(data.get("bleProcessMaxUs"))
                sample_ts = rec.get("ts") if isinstance(rec.get("ts"), str) else ""
                # Prefer firmware uptime for per-device rate calculations; host wall-clock
                # sampling jitter can inflate short-window Hz estimates under load.
                uptime_ms_val = num(data.get("uptimeMs"))
                sample_epoch = (uptime_ms_val / 1000.0) if uptime_ms_val is not None else parse_ts_epoch(sample_ts)

                if flush_val is not None and (flush_max_peak is None or flush_val > flush_max_peak):
                    flush_max_peak = flush_val
                    flush_peak_ts = sample_ts

                if loop_val is not None and (loop_max_peak is None or loop_val > loop_max_peak):
                    loop_max_peak = loop_val
                    loop_peak_ts = sample_ts
                    loop_peak_wifi = wifi_val
                    loop_peak_flush = flush_val
                    loop_peak_ble_drain = ble_drain_val
                    loop_peak_display_updates = num(data.get("displayUpdates"))
                    loop_peak_rx_packets = num(data.get("rxPackets"))

                if wifi_val is not None and (wifi_max_peak is None or wifi_val > wifi_max_peak):
                    wifi_max_peak = wifi_val
                    wifi_peak_ts = sample_ts
                    wifi_peak_loop = loop_val
                    wifi_peak_flush = flush_val
                    wifi_peak_ble_drain = ble_drain_val
                    wifi_peak_display_updates = num(data.get("displayUpdates"))
                    wifi_peak_rx_packets = num(data.get("rxPackets"))
                if wifi_val is not None:
                    wifi_samples.append(wifi_val)

                wifi_ap_up = num(data.get("wifiApUpTransitions"))
                if wifi_ap_up_first is None and wifi_ap_up is not None:
                    wifi_ap_up_first = wifi_ap_up
                if wifi_ap_up is not None:
                    wifi_ap_up_last = wifi_ap_up

                wifi_ap_down = num(data.get("wifiApDownTransitions"))
                if wifi_ap_down_first is None and wifi_ap_down is not None:
                    wifi_ap_down_first = wifi_ap_down
                if wifi_ap_down is not None:
                    wifi_ap_down_last = wifi_ap_down
                    if prev_wifi_ap_down is not None and wifi_ap_down > prev_wifi_ap_down:
                        wifi_ap_down_event_indices.append(len(sample_records))
                    prev_wifi_ap_down = wifi_ap_down

                proxy_adv_on = num(data.get("proxyAdvertisingOnTransitions"))
                if proxy_adv_on_first is None and proxy_adv_on is not None:
                    proxy_adv_on_first = proxy_adv_on
                if proxy_adv_on is not None:
                    proxy_adv_on_last = proxy_adv_on

                proxy_adv_off = num(data.get("proxyAdvertisingOffTransitions"))
                if proxy_adv_off_first is None and proxy_adv_off is not None:
                    proxy_adv_off_first = proxy_adv_off
                if proxy_adv_off is not None:
                    proxy_adv_off_last = proxy_adv_off
                    if prev_proxy_adv_off is not None and proxy_adv_off > prev_proxy_adv_off:
                        proxy_adv_off_event_indices.append(len(sample_records))
                    prev_proxy_adv_off = proxy_adv_off

                wifi_ap_state = num(data.get("wifiApActive"))
                if wifi_ap_state is not None:
                    if int(wifi_ap_state) != 0:
                        wifi_ap_active_samples += 1
                        wifi_peak_ap_active = update_max(wifi_peak_ap_active, wifi_val)
                    else:
                        wifi_ap_inactive_samples += 1
                        wifi_peak_ap_inactive = update_max(wifi_peak_ap_inactive, wifi_val)

                proxy_adv_state = num(data.get("proxyAdvertising"))
                if proxy_adv_state is None:
                    proxy_obj = data.get("proxy")
                    if isinstance(proxy_obj, dict):
                        proxy_adv_state = num(proxy_obj.get("advertising"))
                if proxy_adv_state is not None:
                    if int(proxy_adv_state) != 0:
                        proxy_adv_on_samples += 1
                        wifi_peak_proxy_adv_on = update_max(wifi_peak_proxy_adv_on, wifi_val)
                    else:
                        proxy_adv_off_samples += 1
                        wifi_peak_proxy_adv_off = update_max(wifi_peak_proxy_adv_off, wifi_val)

                wifi_ap_reason_code = num(data.get("wifiApLastTransitionReasonCode"))
                if wifi_ap_reason_code is not None:
                    wifi_ap_last_reason_code = wifi_ap_reason_code
                wifi_ap_reason = data.get("wifiApLastTransitionReason")
                if isinstance(wifi_ap_reason, str):
                    wifi_ap_last_reason = wifi_ap_reason

                proxy_adv_reason_code = num(data.get("proxyAdvertisingLastTransitionReasonCode"))
                if proxy_adv_reason_code is not None:
                    proxy_adv_last_reason_code = proxy_adv_reason_code
                proxy_adv_reason = data.get("proxyAdvertisingLastTransitionReason")
                if isinstance(proxy_adv_reason, str):
                    proxy_adv_last_reason = proxy_adv_reason

                if ok_samples > 2 and wifi_val is not None and (
                    wifi_max_peak_excluding_first is None or wifi_val > wifi_max_peak_excluding_first
                ):
                    wifi_max_peak_excluding_first = wifi_val
                    wifi_peak_excluding_first_ts = sample_ts

                if ble_drain_val is not None and (ble_drain_max_peak is None or ble_drain_val > ble_drain_max_peak):
                    ble_drain_max_peak = ble_drain_val
                    ble_drain_peak_ts = sample_ts

                display_updates = num(data.get("displayUpdates"))
                if display_updates_first is None and display_updates is not None:
                    display_updates_first = display_updates
                if display_updates is not None:
                    display_updates_last = display_updates

                display_skips = num(data.get("displaySkips"))
                if display_skips_first is None and display_skips is not None:
                    display_skips_first = display_skips
                if display_skips is not None:
                    display_skips_last = display_skips

                rx_packets = num(data.get("rxPackets"))
                if rx_packets_first is None and rx_packets is not None:
                    rx_packets_first = rx_packets
                if rx_packets is not None:
                    rx_packets_last = rx_packets

                parse_successes = num(data.get("parseSuccesses"))
                if parse_successes_first is None and parse_successes is not None:
                    parse_successes_first = parse_successes
                if parse_successes is not None:
                    parse_successes_last = parse_successes

                parse_failures = num(data.get("parseFailures"))
                if parse_failures_first is None and parse_failures is not None:
                    parse_failures_first = parse_failures
                if parse_failures is not None:
                    parse_failures_last = parse_failures

                parse_resyncs = num(data.get("parseResyncs"))
                if parse_resyncs_first is None and parse_resyncs is not None:
                    parse_resyncs_first = parse_resyncs
                if parse_resyncs is not None:
                    parse_resyncs_last = parse_resyncs

                queue_drops = num(data.get("queueDrops"))
                if queue_drops_first is None and queue_drops is not None:
                    queue_drops_first = queue_drops
                if queue_drops is not None:
                    queue_drops_last = queue_drops

                perf_drop = num(data.get("perfDrop"))
                if perf_drop_first is None and perf_drop is not None:
                    perf_drop_first = perf_drop
                if perf_drop is not None:
                    perf_drop_last = perf_drop

                proxy = data.get("proxy")
                if isinstance(proxy, dict):
                    proxy_drop_peak = update_max(proxy_drop_peak, num(proxy.get("dropCount")))

                event_bus = data.get("eventBus")
                if isinstance(event_bus, dict):
                    pub = num(event_bus.get("publishCount"))
                    drp = num(event_bus.get("dropCount"))
                    siz = num(event_bus.get("size"))
                    if event_publish_first is None and pub is not None:
                        event_publish_first = pub
                    if pub is not None:
                        event_publish_last = pub
                    if event_drop_first is None and drp is not None:
                        event_drop_first = drp
                    if drp is not None:
                        event_drop_last = drp
                    event_size_peak = update_max(event_size_peak, siz)

                # Additional SLO-aligned metrics
                oversize_drops = num(data.get("oversizeDrops"))
                if oversize_drops_first is None and oversize_drops is not None:
                    oversize_drops_first = oversize_drops
                if oversize_drops is not None:
                    oversize_drops_last = oversize_drops

                sd_max_peak = update_max(sd_max_peak, num(data.get("sdMaxUs")))
                fs_max_peak = update_max(fs_max_peak, num(data.get("fsMaxUs")))

                n2d_max = num(data.get("notifyToDisplayMaxMs"))
                if n2d_max is not None:
                    notify_to_display_seen = True
                    notify_to_display_max_ms = update_max(notify_to_display_max_ms, n2d_max)
                n2d_count = num(data.get("notifyToDisplayTotalCount"))
                if n2d_count is not None:
                    notify_to_display_seen = True
                    if n2d_count > notify_to_display_sample_count:
                        notify_to_display_sample_count = n2d_count
                queue_high_water = num(data.get("queueHighWater"))
                if queue_high_water_first is None and queue_high_water is not None:
                    queue_high_water_first = queue_high_water
                queue_high_water_peak = update_max(queue_high_water_peak, queue_high_water)

                wifi_connect_deferred = num(data.get("wifiConnectDeferred"))
                if wifi_connect_deferred_first is None and wifi_connect_deferred is not None:
                    wifi_connect_deferred_first = wifi_connect_deferred
                if wifi_connect_deferred is not None:
                    wifi_connect_deferred_last = wifi_connect_deferred

                reconnects_val = num(data.get("reconnects"))
                if reconnects_first is None and reconnects_val is not None:
                    reconnects_first = reconnects_val
                if reconnects_val is not None:
                    reconnects_last = reconnects_val

                disconnects_val = num(data.get("disconnects"))
                if disconnects_first is None and disconnects_val is not None:
                    disconnects_first = disconnects_val
                if disconnects_val is not None:
                    disconnects_last = disconnects_val

                dma_free_val = num(data.get("heapDmaMin"))
                dma_largest_val = num(data.get("heapDmaLargestMin"))
                dma_free_min_val_raw = update_min(dma_free_min_val_raw, dma_free_val)
                dma_largest_min_val_raw = update_min(dma_largest_min_val_raw, dma_largest_val)
                dma_free_samples.append(dma_free_val)
                dma_largest_samples.append(dma_largest_val)

                dma_free_current = num(data.get("heapDma"))
                dma_largest_current = num(data.get("heapDmaLargest"))
                if dma_largest_current is not None:
                    dma_largest_current_samples.append(dma_largest_current)
                    if args.dma_largest_floor is not None:
                        if dma_largest_current < args.dma_largest_floor:
                            dma_largest_below_floor_samples += 1
                            dma_largest_below_floor_streak += 1
                            if dma_largest_below_floor_streak > dma_largest_below_floor_longest_streak:
                                dma_largest_below_floor_longest_streak = dma_largest_below_floor_streak
                        else:
                            dma_largest_below_floor_streak = 0
                if (
                    dma_free_current is not None
                    and dma_largest_current is not None
                    and dma_free_current > 0
                ):
                    largest_to_free_pct = (dma_largest_current * 100.0) / dma_free_current
                    dma_largest_to_free_pct_samples.append(largest_to_free_pct)
                    fragmentation_pct = max(0.0, 100.0 - largest_to_free_pct)
                    dma_fragmentation_pct_samples.append(fragmentation_pct)

                ble_process_max_peak = update_max(ble_process_max_peak, ble_process_val)
                disp_pipe_max_peak = update_max(disp_pipe_max_peak, disp_pipe_val)
                if disp_pipe_val is not None:
                    disp_pipe_samples.append(disp_pipe_val)

                ble_mutex_timeout = num(data.get("bleMutexTimeout"))
                if ble_mutex_timeout_first is None and ble_mutex_timeout is not None:
                    ble_mutex_timeout_first = ble_mutex_timeout
                if ble_mutex_timeout is not None:
                    ble_mutex_timeout_last = ble_mutex_timeout

                update_counter_windows(
                    data,
                    display_counter_first,
                    display_counter_last,
                    DISPLAY_COUNTER_DELTA_MAPPINGS,
                )

                record = {
                    "epoch": sample_epoch,
                    "wifi": wifi_val,
                    "loop": loop_val,
                    "ble": ble_process_val,
                    "disp": disp_pipe_val,
                    "disp_render": num(data.get("dispMaxUs")),
                    "display_gap_recover": num(data.get("displayGapRecoverMaxUs")),
                    "ble_followup_request_alert": num(data.get("bleFollowupRequestAlertMaxUs")),
                    "ble_followup_request_version": num(data.get("bleFollowupRequestVersionMaxUs")),
                    "ble_connect_stable_callback": num(data.get("bleConnectStableCallbackMaxUs")),
                    "ble_proxy_start": num(data.get("bleProxyStartMaxUs")),
                    "ble_state": data.get("bleState") if isinstance(data.get("bleState"), str) else None,
                    "ble_state_code": num(data.get("bleStateCode")),
                    "subscribe_step": data.get("subscribeStep") if isinstance(data.get("subscribeStep"), str) else None,
                    "subscribe_step_code": num(data.get("subscribeStepCode")),
                    "proxy_advertising": proxy_adv_state,
                }
                for record_key, data_key in DISPLAY_SAMPLE_FIELD_MAPPINGS:
                    record[record_key] = num(data.get(data_key))
                sample_records.append(record)
    except FileNotFoundError:
        pass

    wifi_skip = max(args.skip_first_wifi_samples, 0)
    wifi_samples_excluding_first = wifi_samples[wifi_skip:] if wifi_skip > 0 else list(wifi_samples)

    minima_tail_requested = max(args.exclude_tail_samples_for_minima, 0)
    minima_sample_count = len(dma_free_samples)
    minima_tail_excluded = min(minima_tail_requested, max(minima_sample_count - 1, 0))
    minima_cutoff = minima_sample_count - minima_tail_excluded
    dma_free_window = dma_free_samples[:minima_cutoff]
    dma_largest_window = dma_largest_samples[:minima_cutoff]
    dma_free_vals_window = [val for val in dma_free_window if val is not None]
    dma_largest_vals_window = [val for val in dma_largest_window if val is not None]
    dma_free_min_val = min(dma_free_vals_window) if dma_free_vals_window else None
    dma_largest_min_val = min(dma_largest_vals_window) if dma_largest_vals_window else None
    dma_largest_current_sample_count = len(dma_largest_current_samples)
    dma_largest_to_free_pct_min = (
        min(dma_largest_to_free_pct_samples) if dma_largest_to_free_pct_samples else None
    )
    dma_largest_to_free_pct_p05 = percentile(dma_largest_to_free_pct_samples, 5.0)
    dma_largest_to_free_pct_p50 = percentile(dma_largest_to_free_pct_samples, 50.0)
    dma_fragmentation_pct_p50 = percentile(dma_fragmentation_pct_samples, 50.0)
    dma_fragmentation_pct_p95 = percentile(dma_fragmentation_pct_samples, 95.0)
    dma_fragmentation_pct_max = (
        max(dma_fragmentation_pct_samples) if dma_fragmentation_pct_samples else None
    )
    dma_largest_below_floor_pct = None
    if args.dma_largest_floor is not None and dma_largest_current_sample_count > 0:
        dma_largest_below_floor_pct = (
            dma_largest_below_floor_samples * 100.0
        ) / dma_largest_current_sample_count

    wifi_p95_raw = percentile(wifi_samples, 95.0)
    wifi_p95_excluding_first = percentile(wifi_samples_excluding_first, 95.0)
    disp_pipe_p95 = percentile(disp_pipe_samples, 95.0)

    wifi_over_limit_count_raw = None
    wifi_over_limit_count_excluding_first = None
    if args.wifi_threshold is not None:
        wifi_over_limit_count_raw = sum(1 for val in wifi_samples if val > args.wifi_threshold)
        wifi_over_limit_count_excluding_first = sum(
            1 for val in wifi_samples_excluding_first if val > args.wifi_threshold
        )

    disp_pipe_over_limit_count = None
    if args.disp_threshold is not None:
        disp_pipe_over_limit_count = sum(1 for val in disp_pipe_samples if val > args.disp_threshold)

    stable_required = max(args.stable_consecutive_samples, 1)

    def sample_is_stable(rec):
        if args.wifi_threshold is not None:
            wifi_val = rec.get("wifi")
            if wifi_val is None or wifi_val > args.wifi_threshold:
                return False
        if args.disp_threshold is not None:
            disp_val = rec.get("disp")
            if disp_val is None or disp_val > args.disp_threshold:
                return False
        return True

    stable_flags = [sample_is_stable(rec) for rec in sample_records]

    def find_stable_index(start_idx):
        consec = 0
        if start_idx < 0:
            start_idx = 0
        for idx in range(start_idx, len(sample_records)):
            if stable_flags[idx]:
                consec += 1
                if consec >= stable_required:
                    return idx
            else:
                consec = 0
        return None

    connect_burst_disp_threshold = (
        args.connect_burst_disp_threshold if args.connect_burst_disp_threshold is not None else args.disp_threshold
    )
    connect_burst_required = max(args.connect_burst_consecutive_samples, 1)

    def connect_burst_event_matches(rec):
        ble_state = rec.get("ble_state")
        ble_state_code = rec.get("ble_state_code")
        subscribe_step = rec.get("subscribe_step")
        subscribe_step_code = rec.get("subscribe_step_code")
        ble_connected = ble_state == "CONNECTED" or ble_state_code == 8
        subscribe_complete = subscribe_step == "COMPLETE" or subscribe_step_code == 11
        return ble_connected and subscribe_complete

    def connect_burst_sample_is_stable(rec):
        if args.ble_threshold is not None:
            ble_val = rec.get("ble")
            if ble_val is None or ble_val > args.ble_threshold:
                return False
        if connect_burst_disp_threshold is not None:
            disp_val = rec.get("disp")
            if disp_val is None or disp_val > connect_burst_disp_threshold:
                return False
        return True

    connect_burst_stable_flags = [connect_burst_sample_is_stable(rec) for rec in sample_records]

    def find_connect_burst_stable_index(start_idx):
        consec = 0
        if start_idx < 0:
            start_idx = 0
        for idx in range(start_idx, len(sample_records)):
            if connect_burst_stable_flags[idx]:
                consec += 1
                if consec >= connect_burst_required:
                    return idx
            else:
                consec = 0
        return None

    def recovery_from_events(event_indices):
        if not event_indices:
            return {
                "events": 0,
                "stabilized": 0,
                "unstable": 0,
                "time_ms": None,
                "samples": None,
            }
        stabilized = 0
        worst_time_ms = None
        worst_samples = None
        for event_idx in event_indices:
            stable_idx = find_stable_index(event_idx)
            if stable_idx is None:
                continue
            stabilized += 1
            samples_to_stable = stable_idx - event_idx + 1
            if worst_samples is None or samples_to_stable > worst_samples:
                worst_samples = samples_to_stable

            event_epoch = sample_records[event_idx].get("epoch")
            stable_epoch = sample_records[stable_idx].get("epoch")
            if event_epoch is not None and stable_epoch is not None:
                time_ms = to_int_ms(stable_epoch - event_epoch)
                if worst_time_ms is None or time_ms > worst_time_ms:
                    worst_time_ms = time_ms

        return {
            "events": len(event_indices),
            "stabilized": stabilized,
            "unstable": len(event_indices) - stabilized,
            "time_ms": worst_time_ms,
            "samples": worst_samples,
        }

    ap_recovery = recovery_from_events(wifi_ap_down_event_indices)
    proxy_recovery = recovery_from_events(proxy_adv_off_event_indices)

    primary_source = "none"
    primary_event_idx = None
    if wifi_ap_down_event_indices and proxy_adv_off_event_indices:
        ap_first = wifi_ap_down_event_indices[0]
        proxy_first = proxy_adv_off_event_indices[0]
        if ap_first <= proxy_first:
            primary_source = "wifi_ap_down"
            primary_event_idx = ap_first
        else:
            primary_source = "proxy_adv_off"
            primary_event_idx = proxy_first
    elif wifi_ap_down_event_indices:
        primary_source = "wifi_ap_down"
        primary_event_idx = wifi_ap_down_event_indices[0]
    elif proxy_adv_off_event_indices:
        primary_source = "proxy_adv_off"
        primary_event_idx = proxy_adv_off_event_indices[0]

    primary_stable_idx = None
    primary_samples_to_stable = None
    primary_time_to_stable_ms = None
    if primary_event_idx is not None:
        primary_stable_idx = find_stable_index(primary_event_idx)
        if primary_stable_idx is not None:
            primary_samples_to_stable = primary_stable_idx - primary_event_idx + 1
            event_epoch = sample_records[primary_event_idx].get("epoch")
            stable_epoch = sample_records[primary_stable_idx].get("epoch")
            if event_epoch is not None and stable_epoch is not None:
                primary_time_to_stable_ms = to_int_ms(stable_epoch - event_epoch)

    if primary_event_idx is None:
        pre_stats = compute_window_stats(sample_records, 0, len(sample_records))
        transition_stats = compute_window_stats(sample_records, 0, 0)
        post_stats = compute_window_stats(sample_records, 0, 0)
    elif primary_stable_idx is None:
        pre_stats = compute_window_stats(sample_records, 0, primary_event_idx)
        transition_stats = compute_window_stats(sample_records, primary_event_idx, len(sample_records))
        post_stats = compute_window_stats(sample_records, len(sample_records), len(sample_records))
    else:
        pre_stats = compute_window_stats(sample_records, 0, primary_event_idx)
        transition_stats = compute_window_stats(sample_records, primary_event_idx, primary_stable_idx + 1)
        post_stats = compute_window_stats(sample_records, primary_stable_idx + 1, len(sample_records))

    connect_burst_event_idx = None
    for idx, rec in enumerate(sample_records):
        if connect_burst_event_matches(rec):
            connect_burst_event_idx = idx
            break

    connect_burst_stable_idx = None
    connect_burst_samples_to_stable = None
    connect_burst_time_to_stable_ms = None
    connect_burst_pre_start_idx = 0
    connect_burst_pre_end_idx = 0
    connect_burst_event_ble_state = None
    connect_burst_event_subscribe_step = None
    connect_burst_event_proxy_advertising = None
    if connect_burst_event_idx is not None:
        connect_burst_event = sample_records[connect_burst_event_idx]
        connect_burst_event_ble_state = connect_burst_event.get("ble_state")
        connect_burst_event_subscribe_step = connect_burst_event.get("subscribe_step")
        connect_burst_event_proxy_advertising = connect_burst_event.get("proxy_advertising")
        connect_burst_stable_idx = find_connect_burst_stable_index(connect_burst_event_idx)
        connect_burst_pre_start_idx = connect_burst_event_idx
        if connect_burst_stable_idx is None:
            connect_burst_pre_end_idx = len(sample_records)
        else:
            connect_burst_samples_to_stable = connect_burst_stable_idx - connect_burst_event_idx + 1
            connect_burst_pre_end_idx = connect_burst_stable_idx + 1
            event_epoch = sample_records[connect_burst_event_idx].get("epoch")
            stable_epoch = sample_records[connect_burst_stable_idx].get("epoch")
            if event_epoch is not None and stable_epoch is not None:
                connect_burst_time_to_stable_ms = to_int_ms(stable_epoch - event_epoch)

    connect_burst_pre_ble_process_peak = window_peak(
        sample_records, connect_burst_pre_start_idx, connect_burst_pre_end_idx, "ble"
    )
    connect_burst_pre_disp_pipe_peak = window_peak(
        sample_records, connect_burst_pre_start_idx, connect_burst_pre_end_idx, "disp"
    )
    connect_burst_ble_followup_request_alert_peak = window_peak(
        sample_records, connect_burst_pre_start_idx, connect_burst_pre_end_idx, "ble_followup_request_alert"
    )
    connect_burst_ble_followup_request_version_peak = window_peak(
        sample_records, connect_burst_pre_start_idx, connect_burst_pre_end_idx, "ble_followup_request_version"
    )
    connect_burst_ble_connect_stable_callback_peak = window_peak(
        sample_records, connect_burst_pre_start_idx, connect_burst_pre_end_idx, "ble_connect_stable_callback"
    )
    connect_burst_ble_proxy_start_peak = window_peak(
        sample_records, connect_burst_pre_start_idx, connect_burst_pre_end_idx, "ble_proxy_start"
    )
    connect_burst_disp_render_peak = window_peak(
        sample_records, connect_burst_pre_start_idx, connect_burst_pre_end_idx, "disp_render"
    )
    connect_burst_display_gap_recover_peak = window_peak(
        sample_records, connect_burst_pre_start_idx, connect_burst_pre_end_idx, "display_gap_recover"
    )
    connect_burst_display_base_frame_peak = window_peak(
        sample_records, connect_burst_pre_start_idx, connect_burst_pre_end_idx, "display_base_frame"
    )
    connect_burst_display_status_strip_peak = window_peak(
        sample_records, connect_burst_pre_start_idx, connect_burst_pre_end_idx, "display_status_strip"
    )
    connect_burst_display_frequency_peak = window_peak(
        sample_records, connect_burst_pre_start_idx, connect_burst_pre_end_idx, "display_frequency"
    )
    connect_burst_display_bands_bars_peak = window_peak(
        sample_records, connect_burst_pre_start_idx, connect_burst_pre_end_idx, "display_bands_bars"
    )
    connect_burst_display_arrows_icons_peak = window_peak(
        sample_records, connect_burst_pre_start_idx, connect_burst_pre_end_idx, "display_arrows_icons"
    )
    connect_burst_display_flush_subphase_peak = window_peak(
        sample_records, connect_burst_pre_start_idx, connect_burst_pre_end_idx, "display_flush_subphase"
    )
    display_partial_flush_area_peak = window_peak(
        sample_records, 0, len(sample_records), "display_partial_flush_area_peak_px"
    )
    display_flush_max_area_peak = window_peak(
        sample_records, 0, len(sample_records), "display_flush_max_area_px"
    )
    display_partial_flush_logical_width_peak = window_peak(
        sample_records, 0, len(sample_records), "display_partial_flush_logical_width_peak_px"
    )
    display_partial_flush_logical_height_peak = window_peak(
        sample_records, 0, len(sample_records), "display_partial_flush_logical_height_peak_px"
    )
    display_partial_flush_row_calls_peak = window_peak(
        sample_records, 0, len(sample_records), "display_partial_flush_row_calls_peak"
    )
    display_partial_flush_pixels_per_row_peak = window_peak(
        sample_records, 0, len(sample_records), "display_partial_flush_pixels_per_row_peak_px"
    )
    display_partial_flush_us_peak = window_peak(
        sample_records, 0, len(sample_records), "display_partial_flush_us_peak_us"
    )
    display_partial_flush_worst_us_logical_width = window_peak(
        sample_records, 0, len(sample_records), "display_partial_flush_worst_us_logical_width_px"
    )
    display_partial_flush_worst_us_logical_height = window_peak(
        sample_records, 0, len(sample_records), "display_partial_flush_worst_us_logical_height_px"
    )
    display_partial_flush_worst_us_area = window_peak(
        sample_records, 0, len(sample_records), "display_partial_flush_worst_us_area_px"
    )
    display_base_frame_peak = window_peak(sample_records, 0, len(sample_records), "display_base_frame")
    display_status_strip_peak = window_peak(sample_records, 0, len(sample_records), "display_status_strip")
    display_frequency_peak = window_peak(sample_records, 0, len(sample_records), "display_frequency")
    display_bands_bars_peak = window_peak(sample_records, 0, len(sample_records), "display_bands_bars")
    display_arrows_icons_peak = window_peak(sample_records, 0, len(sample_records), "display_arrows_icons")
    display_flush_subphase_peak = window_peak(
        sample_records, 0, len(sample_records), "display_flush_subphase"
    )
    display_live_render_peak = window_peak(sample_records, 0, len(sample_records), "display_live_render")
    display_resting_render_peak = window_peak(
        sample_records, 0, len(sample_records), "display_resting_render"
    )
    display_persisted_render_peak = window_peak(
        sample_records, 0, len(sample_records), "display_persisted_render"
    )
    display_preview_render_peak = window_peak(
        sample_records, 0, len(sample_records), "display_preview_render"
    )
    display_restore_render_peak = window_peak(
        sample_records, 0, len(sample_records), "display_restore_render"
    )
    display_preview_first_render_peak = window_peak(
        sample_records, 0, len(sample_records), "display_preview_first_render"
    )
    display_preview_steady_render_peak = window_peak(
        sample_records, 0, len(sample_records), "display_preview_steady_render"
    )

    emit("samples", samples)
    emit("ok_samples", ok_samples)
    emit("heap_free_min", heap_free_min)
    emit("heap_min_free_min", heap_min_free_min)
    emit("heap_dma_min", heap_dma_min)
    emit("heap_dma_largest_min", heap_dma_largest_min)
    emit("latency_max_peak", latency_max_peak)
    emit("proxy_drop_peak", proxy_drop_peak)
    emit("display_updates_first", display_updates_first)
    emit("display_updates_last", display_updates_last)
    emit("display_skips_first", display_skips_first)
    emit("display_skips_last", display_skips_last)
    emit("flush_max_peak", flush_max_peak)
    emit("loop_max_peak", loop_max_peak)
    emit("notify_to_display_max_ms", notify_to_display_max_ms)
    emit(
        "notify_to_display_sample_count",
        notify_to_display_sample_count if notify_to_display_seen else None,
    )
    emit("wifi_max_peak", wifi_max_peak)
    emit("wifi_max_peak_excluding_first", wifi_max_peak_excluding_first)
    emit("ble_drain_max_peak", ble_drain_max_peak)
    emit("loop_peak_ts", loop_peak_ts)
    emit("loop_peak_wifi", loop_peak_wifi)
    emit("loop_peak_flush", loop_peak_flush)
    emit("loop_peak_ble_drain", loop_peak_ble_drain)
    emit("loop_peak_display_updates", loop_peak_display_updates)
    emit("loop_peak_rx_packets", loop_peak_rx_packets)
    emit("wifi_peak_ts", wifi_peak_ts)
    emit("wifi_peak_excluding_first_ts", wifi_peak_excluding_first_ts)
    emit("wifi_peak_loop", wifi_peak_loop)
    emit("wifi_peak_flush", wifi_peak_flush)
    emit("wifi_peak_ble_drain", wifi_peak_ble_drain)
    emit("wifi_peak_display_updates", wifi_peak_display_updates)
    emit("wifi_peak_rx_packets", wifi_peak_rx_packets)
    emit("flush_peak_ts", flush_peak_ts)
    emit("ble_drain_peak_ts", ble_drain_peak_ts)
    emit("rx_packets_first", rx_packets_first)
    emit("rx_packets_last", rx_packets_last)
    emit("parse_successes_first", parse_successes_first)
    emit("parse_successes_last", parse_successes_last)
    emit("parse_failures_first", parse_failures_first)
    emit("parse_failures_last", parse_failures_last)
    emit("parse_resyncs_first", parse_resyncs_first)
    emit("parse_resyncs_last", parse_resyncs_last)
    emit("queue_drops_first", queue_drops_first)
    emit("queue_drops_last", queue_drops_last)
    emit("perf_drop_first", perf_drop_first)
    emit("perf_drop_last", perf_drop_last)
    emit("event_publish_first", event_publish_first)
    emit("event_publish_last", event_publish_last)
    emit("event_drop_first", event_drop_first)
    emit("event_drop_last", event_drop_last)
    emit("event_size_peak", event_size_peak)
    emit("core_guard_tripped_count", core_guard_tripped_count)

    # Additional SLO-aligned metrics
    emit("oversize_drops_first", oversize_drops_first)
    emit("oversize_drops_last", oversize_drops_last)
    emit("sd_max_peak", sd_max_peak)
    emit("fs_max_peak", fs_max_peak)
    emit("queue_high_water_first", queue_high_water_first)
    emit("queue_high_water_peak", queue_high_water_peak)
    emit("wifi_connect_deferred_first", wifi_connect_deferred_first)
    emit("wifi_connect_deferred_last", wifi_connect_deferred_last)
    emit("reconnects_first", reconnects_first)
    emit("reconnects_last", reconnects_last)
    emit("disconnects_first", disconnects_first)
    emit("disconnects_last", disconnects_last)
    emit("dma_free_min", dma_free_min_val)
    emit("dma_largest_min", dma_largest_min_val)
    emit("dma_free_min_raw", dma_free_min_val_raw)
    emit("dma_largest_min_raw", dma_largest_min_val_raw)
    emit("dma_largest_current_sample_count", dma_largest_current_sample_count)
    emit(
        "dma_largest_below_floor_samples",
        dma_largest_below_floor_samples if args.dma_largest_floor is not None else None,
    )
    emit(
        "dma_largest_below_floor_pct",
        round(dma_largest_below_floor_pct, 3) if dma_largest_below_floor_pct is not None else None,
    )
    emit(
        "dma_largest_below_floor_longest_streak",
        dma_largest_below_floor_longest_streak if args.dma_largest_floor is not None else None,
    )
    emit(
        "dma_largest_to_free_pct_min",
        round(dma_largest_to_free_pct_min, 3) if dma_largest_to_free_pct_min is not None else None,
    )
    emit(
        "dma_largest_to_free_pct_p05",
        round(dma_largest_to_free_pct_p05, 3) if dma_largest_to_free_pct_p05 is not None else None,
    )
    emit(
        "dma_largest_to_free_pct_p50",
        round(dma_largest_to_free_pct_p50, 3) if dma_largest_to_free_pct_p50 is not None else None,
    )
    emit(
        "dma_fragmentation_pct_p50",
        round(dma_fragmentation_pct_p50, 3) if dma_fragmentation_pct_p50 is not None else None,
    )
    emit(
        "dma_fragmentation_pct_p95",
        round(dma_fragmentation_pct_p95, 3) if dma_fragmentation_pct_p95 is not None else None,
    )
    emit(
        "dma_fragmentation_pct_max",
        round(dma_fragmentation_pct_max, 3) if dma_fragmentation_pct_max is not None else None,
    )
    emit("minima_tail_samples_excluded", minima_tail_excluded)
    emit("minima_samples_considered", minima_cutoff)
    emit("ble_process_max_peak", ble_process_max_peak)
    emit("disp_pipe_max_peak", disp_pipe_max_peak)
    emit("wifi_sample_count", len(wifi_samples))
    emit("wifi_sample_count_excluding_first", len(wifi_samples_excluding_first))
    emit("wifi_p95_raw", round(wifi_p95_raw, 3) if wifi_p95_raw is not None else None)
    emit(
        "wifi_p95_excluding_first",
        round(wifi_p95_excluding_first, 3) if wifi_p95_excluding_first is not None else None,
    )
    emit("wifi_over_limit_count_raw", wifi_over_limit_count_raw)
    emit("wifi_over_limit_count_excluding_first", wifi_over_limit_count_excluding_first)
    emit("disp_pipe_sample_count", len(disp_pipe_samples))
    emit("disp_pipe_p95", round(disp_pipe_p95, 3) if disp_pipe_p95 is not None else None)
    emit("disp_pipe_over_limit_count", disp_pipe_over_limit_count)
    emit("wifi_ap_active_samples", wifi_ap_active_samples)
    emit("wifi_ap_inactive_samples", wifi_ap_inactive_samples)
    emit("proxy_adv_on_samples", proxy_adv_on_samples)
    emit("proxy_adv_off_samples", proxy_adv_off_samples)
    emit("wifi_peak_ap_active", wifi_peak_ap_active)
    emit("wifi_peak_ap_inactive", wifi_peak_ap_inactive)
    emit("wifi_peak_proxy_adv_on", wifi_peak_proxy_adv_on)
    emit("wifi_peak_proxy_adv_off", wifi_peak_proxy_adv_off)
    emit("wifi_ap_last_reason_code", wifi_ap_last_reason_code)
    emit("wifi_ap_last_reason", wifi_ap_last_reason if wifi_ap_last_reason else None)
    emit("proxy_adv_last_reason_code", proxy_adv_last_reason_code)
    emit("proxy_adv_last_reason", proxy_adv_last_reason if proxy_adv_last_reason else None)
    emit("stable_consecutive_samples_required", stable_required)
    emit("wifi_ap_down_events_observed", ap_recovery["events"])
    emit("wifi_ap_down_events_stabilized", ap_recovery["stabilized"])
    emit("wifi_ap_down_events_unstable", ap_recovery["unstable"])
    emit("samples_to_stable_after_ap_down", ap_recovery["samples"])
    emit("time_to_stable_ms_after_ap_down", ap_recovery["time_ms"])
    emit("proxy_adv_off_events_observed", proxy_recovery["events"])
    emit("proxy_adv_off_events_stabilized", proxy_recovery["stabilized"])
    emit("proxy_adv_off_events_unstable", proxy_recovery["unstable"])
    emit("samples_to_stable_after_proxy_adv_off", proxy_recovery["samples"])
    emit("time_to_stable_ms_after_proxy_adv_off", proxy_recovery["time_ms"])
    emit("transition_primary_source", primary_source)
    emit("transition_primary_event_index", primary_event_idx)
    emit("transition_primary_stable_index", primary_stable_idx)
    emit(
        "transition_primary_stabilized",
        (1 if primary_stable_idx is not None else 0) if primary_event_idx is not None else None,
    )
    emit("samples_to_stable", primary_samples_to_stable)
    emit("time_to_stable_ms", primary_time_to_stable_ms)
    emit("window_pre_samples", pre_stats["samples"])
    emit("window_pre_wifi_peak", pre_stats["wifi_peak"])
    emit("window_pre_wifi_p95", round(pre_stats["wifi_p95"], 3) if pre_stats["wifi_p95"] is not None else None)
    emit("window_pre_loop_peak", pre_stats["loop_peak"])
    emit("window_pre_loop_p95", round(pre_stats["loop_p95"], 3) if pre_stats["loop_p95"] is not None else None)
    emit("window_pre_disp_pipe_peak", pre_stats["disp_peak"])
    emit(
        "window_pre_disp_pipe_p95",
        round(pre_stats["disp_p95"], 3) if pre_stats["disp_p95"] is not None else None,
    )
    emit("window_transition_samples", transition_stats["samples"])
    emit("window_transition_wifi_peak", transition_stats["wifi_peak"])
    emit(
        "window_transition_wifi_p95",
        round(transition_stats["wifi_p95"], 3) if transition_stats["wifi_p95"] is not None else None,
    )
    emit("window_transition_loop_peak", transition_stats["loop_peak"])
    emit(
        "window_transition_loop_p95",
        round(transition_stats["loop_p95"], 3) if transition_stats["loop_p95"] is not None else None,
    )
    emit("window_transition_disp_pipe_peak", transition_stats["disp_peak"])
    emit(
        "window_transition_disp_pipe_p95",
        round(transition_stats["disp_p95"], 3) if transition_stats["disp_p95"] is not None else None,
    )
    emit("window_post_stable_samples", post_stats["samples"])
    emit("window_post_stable_wifi_peak", post_stats["wifi_peak"])
    emit(
        "window_post_stable_wifi_p95",
        round(post_stats["wifi_p95"], 3) if post_stats["wifi_p95"] is not None else None,
    )
    emit("window_post_stable_loop_peak", post_stats["loop_peak"])
    emit(
        "window_post_stable_loop_p95",
        round(post_stats["loop_p95"], 3) if post_stats["loop_p95"] is not None else None,
    )
    emit("window_post_stable_disp_pipe_peak", post_stats["disp_peak"])
    emit(
        "window_post_stable_disp_pipe_p95",
        round(post_stats["disp_p95"], 3) if post_stats["disp_p95"] is not None else None,
    )
    emit("connect_burst_detected", 1 if connect_burst_event_idx is not None else 0)
    emit("connect_burst_event_index", connect_burst_event_idx)
    emit("connect_burst_stable_index", connect_burst_stable_idx)
    emit(
        "connect_burst_stabilized",
        (1 if connect_burst_stable_idx is not None else 0) if connect_burst_event_idx is not None else None,
    )
    emit("connect_burst_event_ble_state", connect_burst_event_ble_state)
    emit("connect_burst_event_subscribe_step", connect_burst_event_subscribe_step)
    emit("connect_burst_event_proxy_advertising", connect_burst_event_proxy_advertising)
    emit("connect_burst_stable_consecutive_samples_required", connect_burst_required)
    emit("connect_burst_samples_to_stable", connect_burst_samples_to_stable)
    emit("connect_burst_time_to_stable_ms", connect_burst_time_to_stable_ms)
    emit("connect_burst_pre_ble_process_peak", connect_burst_pre_ble_process_peak)
    emit("connect_burst_pre_disp_pipe_peak", connect_burst_pre_disp_pipe_peak)
    emit("connect_burst_ble_followup_request_alert_peak", connect_burst_ble_followup_request_alert_peak)
    emit("connect_burst_ble_followup_request_version_peak", connect_burst_ble_followup_request_version_peak)
    emit("connect_burst_ble_connect_stable_callback_peak", connect_burst_ble_connect_stable_callback_peak)
    emit("connect_burst_ble_proxy_start_peak", connect_burst_ble_proxy_start_peak)
    emit("connect_burst_disp_render_peak", connect_burst_disp_render_peak)
    emit("connect_burst_display_gap_recover_peak", connect_burst_display_gap_recover_peak)
    emit("connect_burst_display_base_frame_peak", connect_burst_display_base_frame_peak)
    emit("connect_burst_display_status_strip_peak", connect_burst_display_status_strip_peak)
    emit("connect_burst_display_frequency_peak", connect_burst_display_frequency_peak)
    emit("connect_burst_display_bands_bars_peak", connect_burst_display_bands_bars_peak)
    emit("connect_burst_display_arrows_icons_peak", connect_burst_display_arrows_icons_peak)
    emit("connect_burst_display_flush_subphase_peak", connect_burst_display_flush_subphase_peak)
    for data_key, output_key in DISPLAY_COUNTER_DELTA_MAPPINGS:
        first_val = display_counter_first.get(data_key)
        last_val = display_counter_last.get(data_key)
        emit(output_key, None if first_val is None or last_val is None else last_val - first_val)
    emit("display_partial_flush_area_peak_px", display_partial_flush_area_peak)
    emit("display_flush_max_area_px", display_flush_max_area_peak)
    emit("display_partial_flush_logical_width_peak_px", display_partial_flush_logical_width_peak)
    emit("display_partial_flush_logical_height_peak_px", display_partial_flush_logical_height_peak)
    emit("display_partial_flush_row_calls_peak", display_partial_flush_row_calls_peak)
    emit("display_partial_flush_pixels_per_row_peak_px", display_partial_flush_pixels_per_row_peak)
    emit("display_partial_flush_us_peak_us", display_partial_flush_us_peak)
    emit("display_partial_flush_worst_us_logical_width_px", display_partial_flush_worst_us_logical_width)
    emit("display_partial_flush_worst_us_logical_height_px", display_partial_flush_worst_us_logical_height)
    emit("display_partial_flush_worst_us_area_px", display_partial_flush_worst_us_area)
    emit("display_base_frame_peak", display_base_frame_peak)
    emit("display_status_strip_peak", display_status_strip_peak)
    emit("display_frequency_peak", display_frequency_peak)
    emit("display_bands_bars_peak", display_bands_bars_peak)
    emit("display_arrows_icons_peak", display_arrows_icons_peak)
    emit("display_flush_subphase_peak", display_flush_subphase_peak)
    emit("display_live_render_peak", display_live_render_peak)
    emit("display_resting_render_peak", display_resting_render_peak)
    emit("display_persisted_render_peak", display_persisted_render_peak)
    emit("display_preview_render_peak", display_preview_render_peak)
    emit("display_restore_render_peak", display_restore_render_peak)
    emit("display_preview_first_render_peak", display_preview_first_render_peak)
    emit("display_preview_steady_render_peak", display_preview_steady_render_peak)

    inherited_counter_suspect = 0
    for first_val in (queue_drops_first, perf_drop_first, event_drop_first):
        if first_val is not None and first_val > 0:
            inherited_counter_suspect = 1
            break
    emit("inherited_counter_suspect", inherited_counter_suspect)

    if oversize_drops_first is None or oversize_drops_last is None:
        print("oversize_drops_delta=")
    else:
        print(f"oversize_drops_delta={oversize_drops_last - oversize_drops_first}")

    if wifi_connect_deferred_first is None or wifi_connect_deferred_last is None:
        print("wifi_connect_deferred_delta=")
    else:
        print(f"wifi_connect_deferred_delta={wifi_connect_deferred_last - wifi_connect_deferred_first}")

    if reconnects_first is None or reconnects_last is None:
        print("reconnects_delta=")
    else:
        print(f"reconnects_delta={reconnects_last - reconnects_first}")

    if disconnects_first is None or disconnects_last is None:
        print("disconnects_delta=")
    else:
        print(f"disconnects_delta={disconnects_last - disconnects_first}")

    if ble_mutex_timeout_first is None or ble_mutex_timeout_last is None:
        print("ble_mutex_timeout_delta=")
    else:
        print(f"ble_mutex_timeout_delta={ble_mutex_timeout_last - ble_mutex_timeout_first}")

    if wifi_ap_up_first is None or wifi_ap_up_last is None:
        print("wifi_ap_up_transitions_delta=")
    else:
        print(f"wifi_ap_up_transitions_delta={wifi_ap_up_last - wifi_ap_up_first}")

    if wifi_ap_down_first is None or wifi_ap_down_last is None:
        print("wifi_ap_down_transitions_delta=")
    else:
        print(f"wifi_ap_down_transitions_delta={wifi_ap_down_last - wifi_ap_down_first}")

    if proxy_adv_on_first is None or proxy_adv_on_last is None:
        print("proxy_adv_on_transitions_delta=")
    else:
        print(f"proxy_adv_on_transitions_delta={proxy_adv_on_last - proxy_adv_on_first}")

    if proxy_adv_off_first is None or proxy_adv_off_last is None:
        print("proxy_adv_off_transitions_delta=")
    else:
        print(f"proxy_adv_off_transitions_delta={proxy_adv_off_last - proxy_adv_off_first}")

    if event_publish_first is None or event_publish_last is None:
        print("event_publish_delta=")
    else:
        print(f"event_publish_delta={event_publish_last - event_publish_first}")

    if event_drop_first is None or event_drop_last is None:
        print("event_drop_delta=")
    else:
        print(f"event_drop_delta={event_drop_last - event_drop_first}")

    if display_updates_first is None or display_updates_last is None:
        print("display_updates_delta=")
    else:
        print(f"display_updates_delta={display_updates_last - display_updates_first}")

    if display_skips_first is None or display_skips_last is None:
        print("display_skips_delta=")
    else:
        print(f"display_skips_delta={display_skips_last - display_skips_first}")

    if rx_packets_first is None or rx_packets_last is None:
        print("rx_packets_delta=")
    else:
        print(f"rx_packets_delta={rx_packets_last - rx_packets_first}")

    if parse_successes_first is None or parse_successes_last is None:
        print("parse_successes_delta=")
    else:
        print(f"parse_successes_delta={parse_successes_last - parse_successes_first}")

    if parse_failures_first is None or parse_failures_last is None:
        print("parse_failures_delta=")
    else:
        print(f"parse_failures_delta={parse_failures_last - parse_failures_first}")

    if parse_resyncs_first is None or parse_resyncs_last is None:
        print("parse_resyncs_delta=")
    else:
        print(f"parse_resyncs_delta={parse_resyncs_last - parse_resyncs_first}")

    if queue_drops_first is None or queue_drops_last is None:
        print("queue_drops_delta=")
    else:
        print(f"queue_drops_delta={queue_drops_last - queue_drops_first}")

    if perf_drop_first is None or perf_drop_last is None:
        print("perf_drop_delta=")
    else:
        print(f"perf_drop_delta={perf_drop_last - perf_drop_first}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
