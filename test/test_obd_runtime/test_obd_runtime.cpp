#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/obd/obd_runtime_module.h"
#include "../../src/modules/obd/obd_elm327_parser.cpp"
#include "../../src/modules/obd/obd_runtime_module.cpp"
#include "../../src/modules/obd/obd_runtime_transport.cpp"
#include "../../src/modules/obd/obd_runtime_commands.cpp"
#include "../../src/modules/obd/obd_runtime_state_machine.cpp"

static void resetRuntime() {
    obdRuntimeModule = ObdRuntimeModule();
}

static void feedBleResponse(const char* response) {
    obdRuntimeModule.onBleData(reinterpret_cast<const uint8_t*>(response), strlen(response));
}

static ObdBleContext makeBleContext(bool bootReady,
                                    bool v1Connected,
                                    bool bleScanIdle,
                                    bool v1ConnectBurstSettling = false,
                                    bool proxyAdvertising = false,
                                    bool proxyClientConnected = false,
                                    bool v1ConnectInProgress = false,
                                    bool obdScanAllowed = true,
                                    bool obdConnectAllowed = true,
                                    bool obdRetryAllowed = true) {
    ObdBleContext ctx;
    ctx.bootReady = bootReady;
    ctx.v1Connected = v1Connected;
    ctx.bleScanIdle = bleScanIdle;
    ctx.v1ConnectBurstSettling = v1ConnectBurstSettling;
    ctx.proxyAdvertising = proxyAdvertising;
    ctx.proxyClientConnected = proxyClientConnected;
    ctx.v1ConnectInProgress = v1ConnectInProgress;
    ctx.obdScanAllowed = obdScanAllowed;
    ctx.obdConnectAllowed = obdConnectAllowed;
    ctx.obdRetryAllowed = obdRetryAllowed;
    return ctx;
}

void setUp() {
    resetRuntime();
}

void tearDown() {}

// ── begin() state transitions ─────────────────────────────────────

void test_begin_disabled_stays_idle() {
    obdRuntimeModule.begin(nullptr, false, "", 0, -80);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_FALSE(obdRuntimeModule.isEnabled());
}

void test_begin_enabled_no_saved_addr_goes_idle() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_TRUE(obdRuntimeModule.isEnabled());
}

void test_begin_enabled_no_saved_addr_null_goes_idle() {
    obdRuntimeModule.begin(nullptr, true, nullptr, 0, -80);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

void test_begin_enabled_with_saved_addr_goes_wait_boot() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, obdRuntimeModule.getState());
    ObdRuntimeStatus status = obdRuntimeModule.snapshot(0);
    TEST_ASSERT_TRUE(status.savedAddressValid);
}

// ── Boot defer tests ──────────────────────────────────────────────

void test_wait_boot_stays_until_boot_ready() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    // Boot not ready — should stay in WAIT_BOOT
    obdRuntimeModule.update(1000, false, false, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, obdRuntimeModule.getState());

    obdRuntimeModule.update(4000, false, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, obdRuntimeModule.getState());
}

void test_wait_boot_transitions_when_v1_connected() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    // Boot ready, V1 connected → should transition to CONNECTING
    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
}

void test_wait_boot_respects_coordinator_connect_gate() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::HOLD_PROXY_FOR_AUTO_OBD,
                      obdRuntimeModule.getBleArbitrationRequest());

    obdRuntimeModule.update(5000,
                            makeBleContext(true, true, true, true, false, false, false, true, false));
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::HOLD_PROXY_FOR_AUTO_OBD,
                      obdRuntimeModule.getBleArbitrationRequest());

    obdRuntimeModule.update(5001,
                            makeBleContext(true, true, true, false, false, false, false, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::HOLD_PROXY_FOR_AUTO_OBD,
                      obdRuntimeModule.getBleArbitrationRequest());
}

void test_wait_boot_connects_without_v1_when_allowed() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    obdRuntimeModule.update(5000,
                            makeBleContext(true, false, true, false, false, false, false, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
}

void test_disconnect_fence_blocks_new_connect_until_acknowledged() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.setTransportDisconnectPendingForTest(true);

    obdRuntimeModule.update(5000, makeBleContext(true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, obdRuntimeModule.getState());

    obdRuntimeModule.setTransportDisconnectPendingForTest(false);
    obdRuntimeModule.update(5001, makeBleContext(true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
}

void test_ble_reason_name_decodes_unacceptable_connection_interval() {
    TEST_ASSERT_EQUAL_STRING("unacceptable_conn_interval",
                             ObdRuntimeModule::bleReasonNameForTest(13));
}

void test_idle_no_scan_at_boot_without_saved_addr() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    // Even with boot ready and everything clear — no scan, stays IDLE
    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());

    obdRuntimeModule.update(60000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

void test_disabled_module_never_transitions() {
    obdRuntimeModule.begin(nullptr, false, "A4:C1:38:00:11:22", 0, -80);

    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

// ── Web UI scan trigger ───────────────────────────────────────────

void test_start_scan_from_idle() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(1000);
    TEST_ASSERT_TRUE(status.scanInProgress);
}

void test_start_scan_waits_for_ble_scan_idle() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    obdRuntimeModule.startScan();
    // V1 is scanning (bleScanIdle=false) — OBD should wait
    obdRuntimeModule.update(1000, true, true, false);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());

    // Now BLE scan is idle
    obdRuntimeModule.update(2000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
}

void test_start_scan_waits_for_coordinator_scan_permission() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, makeBleContext(true, true, true, false, false, false, false, false));
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());

    obdRuntimeModule.update(2000, makeBleContext(true, true, true, false, false, false, false, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
}

void test_start_scan_waits_for_v1_connect_in_progress() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    obdRuntimeModule.startScan();
    // BLE scan is idle but V1 is mid-connection — OBD should wait
    obdRuntimeModule.update(1000, makeBleContext(true, true, true, false, false, false, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());

    // V1 connect completes
    obdRuntimeModule.update(2000, makeBleContext(true, true, true, false, false, false, false));
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
}

void test_disconnected_scan_waits_for_v1_connect_in_progress() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::DISCONNECTED, 1000);

    obdRuntimeModule.startScan();
    // BLE scan is idle but V1 is mid-connection — OBD should wait
    obdRuntimeModule.update(1100,
                            makeBleContext(true, true, true, false, false, false, true, true, true, false));
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());

    // V1 connect completes
    obdRuntimeModule.update(1200,
                            makeBleContext(true, true, true, false, false, false, false, true, true, false));
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
}

void test_start_scan_disabled_does_nothing() {
    obdRuntimeModule.begin(nullptr, false, "", 0, -80);
    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

void test_scan_timeout_returns_to_idle() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());

    // After scan duration (5000ms) with no device found
    obdRuntimeModule.update(6001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

void test_scan_finds_device_transitions_to_connecting() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());

    // Device found with good RSSI
    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -50);

    obdRuntimeModule.update(2000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Address should be saved
    ObdRuntimeStatus status = obdRuntimeModule.snapshot(2000);
    TEST_ASSERT_TRUE(status.savedAddressValid);
}

void test_stop_active_scan_clears_pending_request_before_scan_starts() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    TEST_ASSERT_TRUE(obdRuntimeModule.startScan());
    obdRuntimeModule.stopActiveScan();

    obdRuntimeModule.update(1000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_TRUE(obdRuntimeModule.isScanStopped());
}

void test_stop_active_scan_exits_scanning_state() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    TEST_ASSERT_TRUE(obdRuntimeModule.startScan());
    obdRuntimeModule.update(1000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());

    obdRuntimeModule.stopActiveScan();

    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_TRUE(obdRuntimeModule.isScanStopped());
}

void test_scan_request_retries_when_start_scan_fails_once() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);
    obdRuntimeModule.setTestStartScanResult(false);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getStartScanCallCountForTest());

    obdRuntimeModule.setTestStartScanResult(true);
    obdRuntimeModule.update(2000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(2, obdRuntimeModule.getStartScanCallCountForTest());
}

void test_manual_pair_scan_request_sets_pending_and_starts_scanning() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    TEST_ASSERT_TRUE(obdRuntimeModule.requestManualPairScan(1000));
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::PREEMPT_PROXY_FOR_MANUAL_SCAN,
                      obdRuntimeModule.getBleArbitrationRequest());

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(1000);
    TEST_ASSERT_TRUE(status.manualScanPending);
    TEST_ASSERT_FALSE(status.scanInProgress);
    TEST_ASSERT_FALSE(status.savedAddressValid);

    obdRuntimeModule.update(1001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());

    status = obdRuntimeModule.snapshot(1001);
    TEST_ASSERT_TRUE(status.manualScanPending);
    TEST_ASSERT_TRUE(status.scanInProgress);
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::PREEMPT_PROXY_FOR_MANUAL_SCAN,
                      obdRuntimeModule.getBleArbitrationRequest());
}

void test_manual_pair_scan_waits_for_proxy_advertising_to_stop() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);
    TEST_ASSERT_TRUE(obdRuntimeModule.requestManualPairScan(1000));

    obdRuntimeModule.update(1001, makeBleContext(true, true, true, false, true, false));
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::PREEMPT_PROXY_FOR_MANUAL_SCAN,
                      obdRuntimeModule.getBleArbitrationRequest());

    obdRuntimeModule.update(1002, makeBleContext(true, true, true, false, false, false));
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
}

void test_proxy_client_cancels_manual_pair_scan() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);
    TEST_ASSERT_TRUE(obdRuntimeModule.requestManualPairScan(1000));

    obdRuntimeModule.update(1001, makeBleContext(true, true, true, false, false, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::NONE,
                      obdRuntimeModule.getBleArbitrationRequest());

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(1001);
    TEST_ASSERT_FALSE(status.manualScanPending);
    TEST_ASSERT_FALSE(status.scanInProgress);
}

void test_manual_pair_found_device_releases_preempt_and_holds_connect_flow() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    TEST_ASSERT_TRUE(obdRuntimeModule.requestManualPairScan(1000));
    obdRuntimeModule.update(1001, makeBleContext(true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::PREEMPT_PROXY_FOR_MANUAL_SCAN,
                      obdRuntimeModule.getBleArbitrationRequest());

    obdRuntimeModule.onDeviceFound("OBDLink CX", "B4:C1:38:00:11:33", -50);
    obdRuntimeModule.update(2000, makeBleContext(true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::HOLD_PROXY_FOR_AUTO_OBD,
                      obdRuntimeModule.getBleArbitrationRequest());
}

void test_manual_pair_scan_timeout_preserves_saved_device() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    TEST_ASSERT_TRUE(obdRuntimeModule.requestManualPairScan(1000));
    obdRuntimeModule.update(1001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());

    obdRuntimeModule.update(7002, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());

    const ObdRuntimeStatus status = obdRuntimeModule.snapshot(7002);
    TEST_ASSERT_FALSE(status.manualScanPending);
    TEST_ASSERT_TRUE(status.savedAddressValid);
    TEST_ASSERT_EQUAL_STRING("A4:C1:38:00:11:22", obdRuntimeModule.getSavedAddress());
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::NONE,
                      obdRuntimeModule.getBleArbitrationRequest());
}

void test_manual_pair_connect_failure_preserves_saved_device_and_returns_idle() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    TEST_ASSERT_TRUE(obdRuntimeModule.requestManualPairScan(1000));
    obdRuntimeModule.update(1001, true, true, true);
    obdRuntimeModule.onDeviceFound("OBDLink CX", "B4:C1:38:00:11:33", -50);
    obdRuntimeModule.update(2000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_STRING("A4:C1:38:00:11:22", obdRuntimeModule.getSavedAddress());

    obdRuntimeModule.update(7001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());

    const ObdRuntimeStatus status = obdRuntimeModule.snapshot(7001);
    TEST_ASSERT_FALSE(status.manualScanPending);
    TEST_ASSERT_TRUE(status.savedAddressValid);
    TEST_ASSERT_EQUAL_STRING("A4:C1:38:00:11:22", obdRuntimeModule.getSavedAddress());
    TEST_ASSERT_EQUAL_UINT8(0, status.connectAttempts);
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::NONE,
                      obdRuntimeModule.getBleArbitrationRequest());
}

void test_manual_pair_success_commits_candidate_only_when_polling_begins() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    TEST_ASSERT_TRUE(obdRuntimeModule.requestManualPairScan(1000));
    obdRuntimeModule.update(1001, true, true, true);
    obdRuntimeModule.onDeviceFound("OBDLink CX", "B4:C1:38:00:11:33", -50);
    obdRuntimeModule.update(2000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(2000);
    TEST_ASSERT_TRUE(status.manualScanPending);
    TEST_ASSERT_TRUE(status.savedAddressValid);
    TEST_ASSERT_EQUAL_STRING("A4:C1:38:00:11:22", obdRuntimeModule.getSavedAddress());

    obdRuntimeModule.transitionToPollingForTest(2500);
    TEST_ASSERT_EQUAL(ObdConnectionState::POLLING, obdRuntimeModule.getState());

    status = obdRuntimeModule.snapshot(2500);
    TEST_ASSERT_FALSE(status.manualScanPending);
    TEST_ASSERT_TRUE(status.savedAddressValid);
    TEST_ASSERT_EQUAL_STRING("B4:C1:38:00:11:33", obdRuntimeModule.getSavedAddress());
}

// ── RSSI gate ─────────────────────────────────────────────────────

void test_rssi_gate_rejects_weak_signal() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);

    // Device found with weak RSSI (below threshold)
    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -90);

    obdRuntimeModule.update(2000, true, true, true);
    // Should still be scanning — device was rejected
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
}

void test_rssi_gate_accepts_strong_signal() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -60);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);

    // Device found with strong RSSI (above threshold of -60)
    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -50);

    obdRuntimeModule.update(2000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
}

void test_rssi_gate_rejects_at_boundary() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);

    // Device at exactly minimum — should be rejected (< not <=)
    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -81);

    obdRuntimeModule.update(2000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
}

void test_device_found_outside_scanning_ignored() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);
    // In IDLE, not scanning
    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -50);
    obdRuntimeModule.update(1000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

// ── Connect timeout & retry ───────────────────────────────────────

void test_connect_timeout_increments_attempts() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    // Get past boot wait
    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Connect times out after 5s
    obdRuntimeModule.update(10001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(10001);
    TEST_ASSERT_EQUAL_UINT8(1, status.connectAttempts);
}

void test_cancel_pending_connect_disconnects_and_returns_idle() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
    TEST_ASSERT_FALSE(obdRuntimeModule.isConnectIdle());

    obdRuntimeModule.cancelPendingConnect();

    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_TRUE(obdRuntimeModule.isConnectIdle());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDisconnectCallCountForTest());
}

void test_idle_disconnects_link_that_establishes_after_cancel() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    obdRuntimeModule.cancelPendingConnect();
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDisconnectCallCountForTest());

    // A scan is queued, then the transport connect completes after cancellation
    // returned. Orphan-link cleanup must win before scan admission.
    obdRuntimeModule.startScan();
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.update(5001, makeBleContext(true, true, true));

    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_TRUE(obdRuntimeModule.isConnectIdle());
    TEST_ASSERT_EQUAL_UINT32(2, obdRuntimeModule.getDisconnectCallCountForTest());
    TEST_ASSERT_EQUAL_UINT32(0, obdRuntimeModule.getConnectCallCountForTest());
    TEST_ASSERT_EQUAL_UINT32(0, obdRuntimeModule.getStartScanCallCountForTest());

    obdRuntimeModule.update(5002, makeBleContext(true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(2, obdRuntimeModule.getDisconnectCallCountForTest());
    TEST_ASSERT_EQUAL_UINT32(0, obdRuntimeModule.getConnectCallCountForTest());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getStartScanCallCountForTest());
}

void test_settled_disconnected_state_rejects_unowned_link() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::DISCONNECTED, 1000);

    // First consume an ordinary settled-state pump with retry admission closed.
    obdRuntimeModule.update(1999,
                            makeBleContext(true, true, true, false, false, false, false, true, true, false));
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(0, obdRuntimeModule.getDisconnectCallCountForTest());

    // The physical link appears only after entry cleanup is no longer relevant.
    // Reconciliation must win even when both scan and retry admission are open.
    obdRuntimeModule.startScan();
    obdRuntimeModule.setTestBleConnected(true);

    obdRuntimeModule.update(2000, makeBleContext(true, true, true));

    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
    TEST_ASSERT_TRUE(obdRuntimeModule.isConnectIdle());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDisconnectCallCountForTest());
    TEST_ASSERT_EQUAL_UINT32(0, obdRuntimeModule.getConnectCallCountForTest());
    TEST_ASSERT_EQUAL_UINT32(0, obdRuntimeModule.getStartScanCallCountForTest());

    obdRuntimeModule.update(2001, makeBleContext(true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDisconnectCallCountForTest());
    TEST_ASSERT_EQUAL_UINT32(0, obdRuntimeModule.getConnectCallCountForTest());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getStartScanCallCountForTest());
}

void test_disconnected_entry_rejects_unowned_link_before_retry() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    obdRuntimeModule.update(10001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDisconnectCallCountForTest());

    // A link appears before DISCONNECTED consumes its entry action. Cleanup
    // must return before retry logic can transition back to CONNECTING.
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.update(10002, makeBleContext(true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(2, obdRuntimeModule.getDisconnectCallCountForTest());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getConnectCallCountForTest());

    obdRuntimeModule.update(10003, makeBleContext(true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(2, obdRuntimeModule.getDisconnectCallCountForTest());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getConnectCallCountForTest());
}

void test_disabled_idle_rejects_link_that_establishes_after_teardown() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    obdRuntimeModule.update(5000, true, true, true);
    obdRuntimeModule.cancelPendingConnect();
    obdRuntimeModule.setEnabled(false);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_FALSE(obdRuntimeModule.isEnabled());
    TEST_ASSERT_EQUAL_UINT32(2, obdRuntimeModule.getDisconnectCallCountForTest());

    // Qualification mode can disable OBD before the late transport link is
    // observable. Disabled pumps must still tear down that unowned link.
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.update(5001, makeBleContext(true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_TRUE(obdRuntimeModule.isConnectIdle());
    TEST_ASSERT_EQUAL_UINT32(3, obdRuntimeModule.getDisconnectCallCountForTest());

    obdRuntimeModule.update(5002, makeBleContext(true, true, true));
    TEST_ASSERT_EQUAL_UINT32(3, obdRuntimeModule.getDisconnectCallCountForTest());
}

void test_three_connect_failures_preserve_saved_address_and_stop_retries_for_session() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    // Get past boot wait
    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Fail 1
    obdRuntimeModule.update(10001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());

    obdRuntimeModule.update(10002,
                            makeBleContext(true, true, true, false, false, false, false, true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Fail 2
    obdRuntimeModule.update(15003, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());

    obdRuntimeModule.update(15004,
                            makeBleContext(true, true, true, false, false, false, false, true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Fail 3 — should keep the saved address but stop auto-retrying for this session
    obdRuntimeModule.update(20005, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(20005);
    TEST_ASSERT_TRUE(status.savedAddressValid);
    TEST_ASSERT_EQUAL_UINT32(3, status.connectFailures);
    TEST_ASSERT_EQUAL_UINT8(0, status.connectAttempts);

    // Once idled by the failure threshold, the runtime should stay idle until
    // a manual action (scan/enable-cycle/reboot) re-arms it.
    obdRuntimeModule.update(30006,
                            makeBleContext(true, true, true, false, false, false, false, true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    status = obdRuntimeModule.snapshot(30006);
    TEST_ASSERT_TRUE(status.savedAddressValid);
    TEST_ASSERT_EQUAL_UINT8(0, status.connectAttempts);
}

void test_proxy_client_drops_obd_to_idle() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.stageTransportStateForTest(ObdTransportOp::WRITE);

    obdRuntimeModule.update(5000, makeBleContext(true, true, true, false, false, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDisconnectCallCountForTest());
    TEST_ASSERT_FALSE(obdRuntimeModule.transportRequestActiveForTest());
    TEST_ASSERT_FALSE(obdRuntimeModule.transportResultReadyForTest());
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::NONE,
                      obdRuntimeModule.getBleArbitrationRequest());
}

void test_auto_obd_states_hold_proxy_until_polling_or_backoff() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    obdRuntimeModule.forceStateForTest(ObdConnectionState::CONNECTING, 1000);
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::HOLD_PROXY_FOR_AUTO_OBD,
                      obdRuntimeModule.getBleArbitrationRequest());

    obdRuntimeModule.forceStateForTest(ObdConnectionState::DISCOVERING, 1000);
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::HOLD_PROXY_FOR_AUTO_OBD,
                      obdRuntimeModule.getBleArbitrationRequest());

    obdRuntimeModule.forceStateForTest(ObdConnectionState::AT_INIT, 1000);
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::HOLD_PROXY_FOR_AUTO_OBD,
                      obdRuntimeModule.getBleArbitrationRequest());

    obdRuntimeModule.forceStateForTest(ObdConnectionState::POLLING, 1000);
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::NONE,
                      obdRuntimeModule.getBleArbitrationRequest());

    obdRuntimeModule.forceStateForTest(ObdConnectionState::DISCONNECTED, 1000);
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::NONE,
                      obdRuntimeModule.getBleArbitrationRequest());
}

void test_disconnected_auto_reconnect_waits_for_retry_permission() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::DISCONNECTED, 1000);

    obdRuntimeModule.update(1001,
                            makeBleContext(true, true, true, false, false, false, false, true, true, false));
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::NONE,
                      obdRuntimeModule.getBleArbitrationRequest());

    obdRuntimeModule.update(1002,
                            makeBleContext(true, true, true, false, false, false, false, true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL(ObdBleArbitrationRequest::HOLD_PROXY_FOR_AUTO_OBD,
                      obdRuntimeModule.getBleArbitrationRequest());
}

void test_connect_entry_action_runs_on_next_tick() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(0, obdRuntimeModule.getConnectCallCountForTest());

    obdRuntimeModule.update(5001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getConnectCallCountForTest());
}

void test_discover_entry_action_runs_on_next_tick() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    obdRuntimeModule.update(5000, true, true, true);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.update(5001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCOVERING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getConnectCallCountForTest());

    obdRuntimeModule.setTestDiscoverResult(false);
    // POST_CONNECT_SETTLE_MS (500ms) must elapse before GATT work begins.
    obdRuntimeModule.update(5501, true, true, true);
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDiscoverCallCountForTest());
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCOVERING, obdRuntimeModule.getState());

    obdRuntimeModule.update(5502, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
}

void test_connect_enters_discovering_with_settle_delay() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    obdRuntimeModule.update(5000, true, true, true);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.update(5001, true, true, true);

    TEST_ASSERT_EQUAL(ObdConnectionState::DISCOVERING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(0, obdRuntimeModule.getDiscoverCallCountForTest());

    // Before settle delay: no GATT operations yet
    obdRuntimeModule.update(5001 + obd::POST_CONNECT_SETTLE_MS - 1, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCOVERING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(0, obdRuntimeModule.getDiscoverCallCountForTest());

    // After settle delay: discovery runs
    obdRuntimeModule.update(5001 + obd::POST_CONNECT_SETTLE_MS, true, true, true);
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDiscoverCallCountForTest());
}

void test_discovery_waits_for_post_connect_settle() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.setTestSecurityReady(false);
    obdRuntimeModule.setTestSecurityEncrypted(false);
    obdRuntimeModule.setTestSecurityBonded(false);

    obdRuntimeModule.update(5000, true, true, true);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.update(5001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCOVERING, obdRuntimeModule.getState());

    // Before POST_CONNECT_SETTLE_MS: no discovery yet
    obdRuntimeModule.update(5001 + obd::POST_CONNECT_SETTLE_MS - 1, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCOVERING, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(0, obdRuntimeModule.getDiscoverCallCountForTest());

    // After POST_CONNECT_SETTLE_MS: discovery runs
    obdRuntimeModule.update(5001 + obd::POST_CONNECT_SETTLE_MS, true, true, true);
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDiscoverCallCountForTest());
}

void test_security_timeout_auto_heals_bond_once() {
    // SECURING state still exists and can be force-entered for bond repair testing
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::SECURING, 0);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.setTestSecurityReady(false);
    obdRuntimeModule.setTestSecurityEncrypted(false);
    obdRuntimeModule.setTestSecurityBonded(false);

    obdRuntimeModule.update(4000, true, true, true);

    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDeleteBondCallCountForTest());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getRefreshBondBackupCallCountForTest());

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(4000);
    TEST_ASSERT_EQUAL_UINT32(1, status.securityRepairs);
}

void test_security_timeout_does_not_repair_same_bond_twice() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::SECURING, 0);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.setTestSecurityReady(false);
    obdRuntimeModule.setTestSecurityEncrypted(false);
    obdRuntimeModule.setTestSecurityBonded(false);

    obdRuntimeModule.update(4000, true, true, true);
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDeleteBondCallCountForTest());

    obdRuntimeModule.forceStateForTest(ObdConnectionState::SECURING, 0);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.update(4001, true, true, true);

    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDeleteBondCallCountForTest());
}

void test_at_init_waits_for_subscribe_settle_and_uses_atz_reset() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::AT_INIT, 0);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.setTestSecurityReady(true);
    obdRuntimeModule.setTestSecurityEncrypted(true);
    obdRuntimeModule.setTestSecurityBonded(true);

    obdRuntimeModule.update(obd::POST_SUBSCRIBE_SETTLE_MS - 1, true, true, true);
    TEST_ASSERT_EQUAL_UINT32(0, obdRuntimeModule.getWriteCallCountForTest());

    obdRuntimeModule.update(obd::POST_SUBSCRIBE_SETTLE_MS, true, true, true);
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getWriteCallCountForTest());
    TEST_ASSERT_EQUAL_STRING("ATZ\r", obdRuntimeModule.getLastCommandForTest());
}

void test_at_init_empty_timeout_switches_to_no_response_and_keeps_mode() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::AT_INIT, 0);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.setTestSecurityReady(true);
    obdRuntimeModule.setTestSecurityEncrypted(true);
    obdRuntimeModule.setTestSecurityBonded(true);

    // Default is now no-response (matching main)
    obdRuntimeModule.update(obd::POST_SUBSCRIBE_SETTLE_MS, true, true, true);
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getWriteCallCountForTest());
    TEST_ASSERT_FALSE(obdRuntimeModule.getLastWriteWithResponseForTest());
    TEST_ASSERT_EQUAL_STRING("ATZ\r", obdRuntimeModule.getLastCommandForTest());

    obdRuntimeModule.update(obd::POST_SUBSCRIBE_SETTLE_MS + 1, true, true, true);

    // Empty timeout alternates to with-response
    obdRuntimeModule.update(obd::POST_SUBSCRIBE_SETTLE_MS + obd::AT_INIT_RESPONSE_TIMEOUT_MS + 1,
                            true,
                            true,
                            true);
    TEST_ASSERT_EQUAL_UINT32(2, obdRuntimeModule.getWriteCallCountForTest());
    TEST_ASSERT_TRUE(obdRuntimeModule.getLastWriteWithResponseForTest());
    TEST_ASSERT_EQUAL_STRING("ATZ\r", obdRuntimeModule.getLastCommandForTest());

    obdRuntimeModule.update(obd::POST_SUBSCRIBE_SETTLE_MS + obd::AT_INIT_RESPONSE_TIMEOUT_MS + 2,
                            true,
                            true,
                            true);

    feedBleResponse("OBDLink CX\r\n>");
    obdRuntimeModule.update(obd::POST_SUBSCRIBE_SETTLE_MS + obd::AT_INIT_RESPONSE_TIMEOUT_MS + 3,
                            true,
                            true,
                            true);
    obdRuntimeModule.update(obd::POST_SUBSCRIBE_SETTLE_MS + obd::AT_INIT_RESPONSE_TIMEOUT_MS + 4,
                            true,
                            true,
                            true);

    TEST_ASSERT_EQUAL_UINT32(3, obdRuntimeModule.getWriteCallCountForTest());
    TEST_ASSERT_TRUE(obdRuntimeModule.getLastWriteWithResponseForTest());
    TEST_ASSERT_EQUAL_STRING("ATE0\r", obdRuntimeModule.getLastCommandForTest());
}

void test_first_at_init_write_failure_auto_heals_bond() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::AT_INIT, 0);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.setTestSecurityReady(true);
    obdRuntimeModule.setTestSecurityEncrypted(true);
    obdRuntimeModule.setTestSecurityBonded(true);
    obdRuntimeModule.setTestWriteResult(false);
    obdRuntimeModule.setTestLastBleError(1);

    obdRuntimeModule.update(obd::POST_SUBSCRIBE_SETTLE_MS, true, true, true);
    obdRuntimeModule.update(obd::POST_SUBSCRIBE_SETTLE_MS + 1, true, true, true);

    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDeleteBondCallCountForTest());
}

// ── Speed data ────────────────────────────────────────────────────

void test_inject_speed_is_fresh() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.injectSpeedForTest(37.3f, 1000);

    float speed = 0.0f;
    uint32_t ts = 0;
    TEST_ASSERT_TRUE(obdRuntimeModule.getFreshSpeed(2000, speed, ts));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 37.3f, speed);
    TEST_ASSERT_EQUAL_UINT32(1000, ts);
}

void test_speed_goes_stale() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.injectSpeedForTest(37.3f, 1000);

    float speed = 0.0f;
    uint32_t ts = 0;
    // 3001ms after sample — stale
    TEST_ASSERT_FALSE(obdRuntimeModule.getFreshSpeed(4002, speed, ts));
}

void test_speed_boundary_fresh() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.injectSpeedForTest(50.0f, 1000);

    float speed = 0.0f;
    uint32_t ts = 0;
    // Exactly at max age boundary — should still be fresh
    TEST_ASSERT_TRUE(obdRuntimeModule.getFreshSpeed(4000, speed, ts));
}

void test_snapshot_speed_age() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.injectSpeedForTest(60.0f, 1000);

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(2500);
    TEST_ASSERT_TRUE(status.speedValid);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 60.0f, status.speedMph);
    TEST_ASSERT_EQUAL_UINT32(1500, status.speedAgeMs);
}

void test_no_speed_when_disabled() {
    obdRuntimeModule.begin(nullptr, false, "", 0, -80);
    float speed = 0.0f;
    uint32_t ts = 0;
    TEST_ASSERT_FALSE(obdRuntimeModule.getFreshSpeed(1000, speed, ts));

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(1000);
    TEST_ASSERT_FALSE(status.speedValid);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, status.speedAgeMs);
}

// ── forgetDevice() ────────────────────────────────────────────────

void test_forget_device_clears_address_and_goes_idle() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    obdRuntimeModule.forgetDevice();
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(5000);
    TEST_ASSERT_FALSE(status.savedAddressValid);
}

void test_forget_device_disconnects_active_client() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::POLLING, 1000);
    obdRuntimeModule.setTestBleConnected(true);

    obdRuntimeModule.forgetDevice();

    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDisconnectCallCountForTest());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDeleteBondCallCountForTest());
}

void test_forget_then_repair_auto_heals_stale_bond() {
    // Start with a saved + working device
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::POLLING, 1000);
    obdRuntimeModule.setTestBleConnected(true);

    // Forget it — should delete the bond
    obdRuntimeModule.forgetDevice();
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_STRING("", obdRuntimeModule.getSavedAddress());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDeleteBondCallCountForTest());

    // Manual pair scan → find same CX again
    TEST_ASSERT_TRUE(obdRuntimeModule.requestManualPairScan(10000));
    obdRuntimeModule.update(10001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());

    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -50);
    obdRuntimeModule.update(11000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Force into SECURING to simulate a stale-bond security failure.
    // savedAddress_ is empty (forgotten), but connectAddress_ holds the
    // manual-scan candidate. Auto-heal must still work.
    obdRuntimeModule.forceStateForTest(ObdConnectionState::SECURING, 11000);
    obdRuntimeModule.setTestBleConnected(true);
    obdRuntimeModule.setTestSecurityReady(false);
    obdRuntimeModule.setTestSecurityEncrypted(false);
    obdRuntimeModule.setTestSecurityBonded(false);

    // Let security timeout expire
    obdRuntimeModule.update(11000 + obd::POST_CONNECT_SETTLE_MS + obd::SECURITY_TIMEOUT_MS,
                            true, true, true);

    // Should have auto-healed: deleted bond and transitioned to DISCONNECTED
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
    // forgetDevice deleted bond once, auto-heal deletes it again
    TEST_ASSERT_EQUAL_UINT32(2, obdRuntimeModule.getDeleteBondCallCountForTest());
}

void test_forget_clears_repaired_bond_address() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::POLLING, 1000);
    obdRuntimeModule.setTestBleConnected(true);

    // Trigger an auto-heal so repairedBondAddress_ gets populated
    obdRuntimeModule.forceStateForTest(ObdConnectionState::SECURING, 1000);
    obdRuntimeModule.setTestSecurityReady(false);
    obdRuntimeModule.setTestSecurityEncrypted(false);
    obdRuntimeModule.setTestSecurityBonded(false);
    obdRuntimeModule.update(1000 + obd::POST_CONNECT_SETTLE_MS + obd::SECURITY_TIMEOUT_MS,
                            true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
    TEST_ASSERT_TRUE(obdRuntimeModule.getRepairedBondAddressForTest()[0] != '\0');

    // Forget should clear repairedBondAddress_ so auto-heal is re-armed
    obdRuntimeModule.forgetDevice();
    TEST_ASSERT_EQUAL_STRING("", obdRuntimeModule.getRepairedBondAddressForTest());
}

void test_discovering_disconnect_auto_heals_during_manual_pair() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    // Manual pair scan → find CX
    TEST_ASSERT_TRUE(obdRuntimeModule.requestManualPairScan(10000));
    obdRuntimeModule.update(10001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());

    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -50);
    obdRuntimeModule.update(11000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Force into DISCOVERING and simulate a disconnect (stale CX bond)
    obdRuntimeModule.forceStateForTest(ObdConnectionState::DISCOVERING, 11000);
    obdRuntimeModule.setTestBleConnected(false);
    obdRuntimeModule.onBleDisconnect(0x3E); // BLE_ERR_CONN_ESTABLISHMENT
    obdRuntimeModule.update(12000, true, true, true);

    // Should auto-heal: delete bond and transition to DISCONNECTED for retry
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getDeleteBondCallCountForTest());
}

void test_disconnected_reconnects_via_connect_address_fallback_immediately_for_manual_candidate() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    // Manual pair scan → find CX
    TEST_ASSERT_TRUE(obdRuntimeModule.requestManualPairScan(10000));
    obdRuntimeModule.update(10001, true, true, true);
    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -50);
    obdRuntimeModule.update(11000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Force into DISCOVERING, simulate disconnect → auto-heal → DISCONNECTED
    obdRuntimeModule.forceStateForTest(ObdConnectionState::DISCOVERING, 11000);
    obdRuntimeModule.setTestBleConnected(false);
    obdRuntimeModule.onBleDisconnect(0x3E);
    obdRuntimeModule.update(12000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());

    // savedAddress_ is empty (never saved), but connectAddress_ is still set
    TEST_ASSERT_EQUAL_STRING("", obdRuntimeModule.getSavedAddress());
    TEST_ASSERT_TRUE(obdRuntimeModule.getConnectAddressForTest()[0] != '\0');

    // Manual-candidate reconnect bypasses the coordinator retry cadence.
    obdRuntimeModule.update(12001,
                            makeBleContext(true, true, true, false, false, false, false, true, true, false));
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
}

// ── setEnabled() ──────────────────────────────────────────────────

void test_disable_during_operation_goes_idle() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    obdRuntimeModule.setEnabled(false);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
    TEST_ASSERT_FALSE(obdRuntimeModule.isEnabled());
}

void test_enable_same_state_is_noop() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);
    obdRuntimeModule.setEnabled(true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

void test_reenable_with_saved_address_restores_wait_boot() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.setEnabled(false);
    obdRuntimeModule.setEnabled(true);
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, obdRuntimeModule.getState());
}

void test_set_min_rssi_applies_immediately() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);
    obdRuntimeModule.setMinRssi(-60);

    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);
    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -70);
    obdRuntimeModule.update(2000, true, true, true);

    TEST_ASSERT_EQUAL(ObdConnectionState::SCANNING, obdRuntimeModule.getState());
}

void test_disconnect_callback_is_delivered_from_queue() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::POLLING, 0);
    obdRuntimeModule.setTestBleConnected(true);

    obdRuntimeModule.onBleDisconnect(534);
    TEST_ASSERT_EQUAL(ObdConnectionState::POLLING, obdRuntimeModule.getState());

    obdRuntimeModule.update(100, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
}

// ── Error backoff ─────────────────────────────────────────────────

void test_poll_timeout_counts_as_error() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::POLLING, 0);

    obdRuntimeModule.update(100, true, true, true);
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getWriteCallCountForTest());
    TEST_ASSERT_EQUAL_STRING("010D\r", obdRuntimeModule.getLastCommandForTest());

    obdRuntimeModule.update(101, true, true, true);

    // POLL_TIMEOUT_MS is 1000, so timeout fires at 100+1000=1100
    obdRuntimeModule.update(1200, true, true, true);
    ObdRuntimeStatus status = obdRuntimeModule.snapshot(1200);
    TEST_ASSERT_EQUAL_UINT32(2, obdRuntimeModule.getWriteCallCountForTest());
    TEST_ASSERT_TRUE(obdRuntimeModule.getLastWriteWithResponseForTest());
    TEST_ASSERT_EQUAL_UINT32(0, status.pollErrors);
    TEST_ASSERT_EQUAL_UINT32(0, status.consecutiveErrors);
    TEST_ASSERT_EQUAL(ObdConnectionState::POLLING, obdRuntimeModule.getState());

    obdRuntimeModule.update(1201, true, true, true);
    obdRuntimeModule.update(2301, true, true, true);
    status = obdRuntimeModule.snapshot(2301);
    TEST_ASSERT_EQUAL_UINT32(1, status.pollErrors);
    TEST_ASSERT_EQUAL_UINT32(1, status.consecutiveErrors);
    TEST_ASSERT_EQUAL(ObdConnectionState::POLLING, obdRuntimeModule.getState());
}

void test_speed_response_assembles_from_multiple_ble_chunks() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::POLLING, 0);

    obdRuntimeModule.update(100, true, true, true);
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getWriteCallCountForTest());
    obdRuntimeModule.update(101, true, true, true);

    obdRuntimeModule.onBleData(reinterpret_cast<const uint8_t*>("41 0D "), 6);
    obdRuntimeModule.onBleData(reinterpret_cast<const uint8_t*>("28\r\n>"), 5);

    obdRuntimeModule.update(150, true, true, true);

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(150);
    TEST_ASSERT_TRUE(status.speedValid);
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 24.85f, status.speedMph);
    TEST_ASSERT_EQUAL_UINT32(0, status.bufferOverflows);
}

void test_data_queue_overflow_fails_response_as_buffer_overflow() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::POLLING, 0);

    obdRuntimeModule.update(100, true, true, true);
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getWriteCallCountForTest());
    obdRuntimeModule.update(101, true, true, true);

    const char* chunks[] = {"4", "1", " ", "0", "D", " ", "2", "8", "\r\n>"};
    for (const char* chunk : chunks) {
        obdRuntimeModule.onBleData(reinterpret_cast<const uint8_t*>(chunk), strlen(chunk));
    }

    obdRuntimeModule.update(150, true, true, true);

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(150);
    TEST_ASSERT_FALSE(status.speedValid);
    TEST_ASSERT_EQUAL_UINT32(1, status.bufferOverflows);
    TEST_ASSERT_EQUAL_UINT32(1, status.pollErrors);
}

void test_searching_extends_speed_timeout() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::POLLING, 0);

    // Send speed command at t=100
    obdRuntimeModule.update(100, true, true, true);
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getWriteCallCountForTest());
    obdRuntimeModule.update(101, true, true, true);

    // CX replies with "SEARCHING...\r" (no ">", so data not ready)
    feedBleResponse("SEARCHING...\r");

    // Normal timeout (1100ms) should NOT fire because SEARCHING extends it
    obdRuntimeModule.update(1200, true, true, true);
    ObdRuntimeStatus status = obdRuntimeModule.snapshot(1200);
    TEST_ASSERT_EQUAL_UINT32(0, status.pollErrors);
    TEST_ASSERT_EQUAL(ObdConnectionState::POLLING, obdRuntimeModule.getState());
    // Should still be waiting for the same command — no new write
    TEST_ASSERT_EQUAL_UINT32(1, obdRuntimeModule.getWriteCallCountForTest());

    // After extended timeout (10000ms) it should finally time out
    obdRuntimeModule.update(10200, true, true, true);
    status = obdRuntimeModule.snapshot(10200);
    TEST_ASSERT_EQUAL_UINT32(1, status.pollErrors);
    TEST_ASSERT_EQUAL(ObdConnectionState::POLLING, obdRuntimeModule.getState());
}

void test_polling_remains_speed_only_after_consecutive_samples() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::POLLING, 0);
    obdRuntimeModule.injectSpeedForTest(45.0f, 5900);
    obdRuntimeModule.setConsecutiveSpeedSamplesForTest(3);

    obdRuntimeModule.update(6000, true, true, true);
    TEST_ASSERT_EQUAL_STRING("010D\r", obdRuntimeModule.getLastCommandForTest());
    obdRuntimeModule.update(6001, true, true, true);

    feedBleResponse("41 0D 28\r\n>");
    obdRuntimeModule.update(6050, true, true, true);
    obdRuntimeModule.update(6100, true, true, true);
    obdRuntimeModule.update(6101, true, true, true);

    const ObdCommandKind activeCommand = obdRuntimeModule.getActiveCommandKindForTest();
    TEST_ASSERT_TRUE(activeCommand == ObdCommandKind::NONE ||
                     activeCommand == ObdCommandKind::SPEED);
    TEST_ASSERT_EQUAL_STRING("010D\r", obdRuntimeModule.getLastCommandForTest());
}

void test_error_backoff_returns_to_polling() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::ERROR_BACKOFF, 1000);
    obdRuntimeModule.setConsecutiveErrorsForTest(5);
    obdRuntimeModule.setLastFailureForTest(ObdFailureReason::COMMAND_TIMEOUT);

    obdRuntimeModule.update(6001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::POLLING, obdRuntimeModule.getState());
    ObdRuntimeStatus status = obdRuntimeModule.snapshot(6001);
    TEST_ASSERT_EQUAL_UINT32(0, status.consecutiveErrors);
}

void test_error_backoff_disconnects_after_ten_write_errors() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::ERROR_BACKOFF, 1000);
    obdRuntimeModule.setConsecutiveErrorsForTest(10);
    obdRuntimeModule.setLastFailureForTest(ObdFailureReason::WRITE);

    obdRuntimeModule.update(6001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());
}

// ── Disconnected reconnect backoff ────────────────────────────────

void test_disconnected_reconnects_when_retry_allowed() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);

    // Boot ready, V1 connected → CONNECTING
    obdRuntimeModule.update(5000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Connect timeout → DISCONNECTED
    obdRuntimeModule.update(10001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());

    obdRuntimeModule.update(10002,
                            makeBleContext(true, true, true, false, false, false, false, true, true, false));
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());

    obdRuntimeModule.update(10003,
                            makeBleContext(true, true, true, false, false, false, false, true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
}

void test_disconnected_failure_threshold_idles_without_forgetting_saved_device() {
    obdRuntimeModule.begin(nullptr, true, "", 0, -80);

    // Trigger scan, find device, connect, then fail 3 times
    obdRuntimeModule.startScan();
    obdRuntimeModule.update(1000, true, true, true);
    obdRuntimeModule.onDeviceFound("OBDLink CX", "A4:C1:38:00:11:22", -50);
    obdRuntimeModule.update(2000, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());

    // Fail 1
    obdRuntimeModule.update(7001, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());

    obdRuntimeModule.update(7002,
                            makeBleContext(true, true, true, false, false, false, false, true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
    obdRuntimeModule.update(12003, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::DISCONNECTED, obdRuntimeModule.getState());

    obdRuntimeModule.update(12004,
                            makeBleContext(true, true, true, false, false, false, false, true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
    obdRuntimeModule.update(17005, true, true, true);
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(17005);
    TEST_ASSERT_TRUE(status.savedAddressValid);
}

// ── ECU idle (car-off / petrol stop) ─────────────────────────────

void test_ecu_idle_entered_after_backoff_threshold() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::ERROR_BACKOFF, 1000);
    obdRuntimeModule.setConsecutiveErrorsForTest(5);
    obdRuntimeModule.setBackoffCyclesForTest(5);  // one below threshold
    obdRuntimeModule.setLastFailureForTest(ObdFailureReason::COMMAND_TIMEOUT);

    // First backoff exit: cycles becomes 6 (== threshold) → ECU_IDLE
    obdRuntimeModule.update(6001, makeBleContext(true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::ECU_IDLE, obdRuntimeModule.getState());
}

void test_ecu_idle_not_entered_below_threshold() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::ERROR_BACKOFF, 1000);
    obdRuntimeModule.setConsecutiveErrorsForTest(5);
    obdRuntimeModule.setBackoffCyclesForTest(4);  // two below threshold
    obdRuntimeModule.setLastFailureForTest(ObdFailureReason::COMMAND_TIMEOUT);

    // Backoff exit: cycles becomes 5 (< threshold) → back to POLLING
    obdRuntimeModule.update(6001, makeBleContext(true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::POLLING, obdRuntimeModule.getState());
}

void test_ecu_idle_resume_on_v1_reconnect() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::ECU_IDLE, 1000);
    obdRuntimeModule.setV1WasConnectedAtEcuIdleForTest(false);

    // V1 was disconnected when we entered, now it reconnects → WAIT_BOOT
    obdRuntimeModule.update(2000, makeBleContext(true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, obdRuntimeModule.getState());
}

void test_ecu_idle_no_false_resume_when_v1_stayed_connected() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::ECU_IDLE, 1000);
    obdRuntimeModule.setV1WasConnectedAtEcuIdleForTest(true);

    // V1 was connected at entry and is still connected — no false trigger
    obdRuntimeModule.update(2000,
                            makeBleContext(true, true, true, false, false, false, false, true, true, false));
    TEST_ASSERT_EQUAL(ObdConnectionState::ECU_IDLE, obdRuntimeModule.getState());
}

void test_ecu_idle_retry_waits_for_retry_permission() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::ECU_IDLE, 1000);
    obdRuntimeModule.setV1WasConnectedAtEcuIdleForTest(true);

    obdRuntimeModule.update(30000,
                            makeBleContext(true, true, true, false, false, false, false, true, true, false));
    TEST_ASSERT_EQUAL(ObdConnectionState::ECU_IDLE, obdRuntimeModule.getState());

    obdRuntimeModule.update(30001,
                            makeBleContext(true, true, true, false, false, false, false, true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::CONNECTING, obdRuntimeModule.getState());
}

void test_ecu_idle_drops_to_idle_when_proxy_client_connected() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::ECU_IDLE, 1000);
    obdRuntimeModule.setV1WasConnectedAtEcuIdleForTest(true);

    obdRuntimeModule.update(31001,
                            makeBleContext(true, true, true, false, false, true, false, true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::IDLE, obdRuntimeModule.getState());
}

void test_ecu_idle_backoff_cycles_reset_on_successful_speed() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.setBackoffCyclesForTest(5);
    obdRuntimeModule.transitionToPollingForTest(1000);

    // Inject a successful speed response
    obdRuntimeModule.injectSpeedForTest(65.0f, 2000);

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(2000);
    // backoffCycles_ not directly in snapshot, but consecutive errors should be 0
    // and the module should stay in POLLING — verify indirectly by checking
    // that after 5 error backoff cycles we'd still go to POLLING (not ECU_IDLE)
    // because the counter was reset
    obdRuntimeModule.forceStateForTest(ObdConnectionState::ERROR_BACKOFF, 3000);
    obdRuntimeModule.setConsecutiveErrorsForTest(5);
    obdRuntimeModule.setLastFailureForTest(ObdFailureReason::COMMAND_TIMEOUT);
    obdRuntimeModule.update(8001, makeBleContext(true, true, true));
    // backoffCycles was reset to 0 by injectSpeed, now incremented to 1 → POLLING
    TEST_ASSERT_EQUAL(ObdConnectionState::POLLING, obdRuntimeModule.getState());
}

void test_ecu_idle_snapshot_not_connected() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::ECU_IDLE, 1000);

    ObdRuntimeStatus status = obdRuntimeModule.snapshot(2000);
    TEST_ASSERT_FALSE(status.connected);
    TEST_ASSERT_FALSE(status.speedValid);
    TEST_ASSERT_EQUAL(ObdConnectionState::ECU_IDLE, status.state);
}

void test_ecu_idle_v1_reconnect_resets_backoff_cycles() {
    obdRuntimeModule.begin(nullptr, true, "A4:C1:38:00:11:22", 0, -80);
    obdRuntimeModule.forceStateForTest(ObdConnectionState::ECU_IDLE, 1000);
    obdRuntimeModule.setBackoffCyclesForTest(6);
    obdRuntimeModule.setV1WasConnectedAtEcuIdleForTest(false);

    // V1 reconnects → WAIT_BOOT with clean backoff counter
    obdRuntimeModule.update(2000, makeBleContext(true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::WAIT_BOOT, obdRuntimeModule.getState());

    // Drive through to CONNECTING and eventually to polling with errors;
    // backoffCycles should have been reset, so we won't immediately re-enter ECU_IDLE
    // Verify by checking that a single backoff cycle returns to POLLING
    obdRuntimeModule.forceStateForTest(ObdConnectionState::ERROR_BACKOFF, 3000);
    obdRuntimeModule.setConsecutiveErrorsForTest(5);
    obdRuntimeModule.setLastFailureForTest(ObdFailureReason::COMMAND_TIMEOUT);
    obdRuntimeModule.update(8001, makeBleContext(true, true, true));
    TEST_ASSERT_EQUAL(ObdConnectionState::POLLING, obdRuntimeModule.getState());
}

int main() {
    UNITY_BEGIN();

    // begin() state transitions
    RUN_TEST(test_begin_disabled_stays_idle);
    RUN_TEST(test_begin_enabled_no_saved_addr_goes_idle);
    RUN_TEST(test_begin_enabled_no_saved_addr_null_goes_idle);
    RUN_TEST(test_begin_enabled_with_saved_addr_goes_wait_boot);

    // Boot defer
    RUN_TEST(test_wait_boot_stays_until_boot_ready);
    RUN_TEST(test_wait_boot_transitions_when_v1_connected);
    RUN_TEST(test_wait_boot_respects_coordinator_connect_gate);
    RUN_TEST(test_wait_boot_connects_without_v1_when_allowed);
    RUN_TEST(test_disconnect_fence_blocks_new_connect_until_acknowledged);
    RUN_TEST(test_ble_reason_name_decodes_unacceptable_connection_interval);
    RUN_TEST(test_idle_no_scan_at_boot_without_saved_addr);
    RUN_TEST(test_disabled_module_never_transitions);

    // Web UI scan
    RUN_TEST(test_start_scan_from_idle);
    RUN_TEST(test_start_scan_waits_for_ble_scan_idle);
    RUN_TEST(test_start_scan_waits_for_coordinator_scan_permission);
    RUN_TEST(test_start_scan_waits_for_v1_connect_in_progress);
    RUN_TEST(test_start_scan_disabled_does_nothing);
    RUN_TEST(test_scan_timeout_returns_to_idle);
    RUN_TEST(test_scan_finds_device_transitions_to_connecting);
    RUN_TEST(test_stop_active_scan_clears_pending_request_before_scan_starts);
    RUN_TEST(test_stop_active_scan_exits_scanning_state);
    RUN_TEST(test_scan_request_retries_when_start_scan_fails_once);
    RUN_TEST(test_manual_pair_scan_request_sets_pending_and_starts_scanning);
    RUN_TEST(test_manual_pair_scan_waits_for_proxy_advertising_to_stop);
    RUN_TEST(test_proxy_client_cancels_manual_pair_scan);
    RUN_TEST(test_manual_pair_found_device_releases_preempt_and_holds_connect_flow);
    RUN_TEST(test_manual_pair_scan_timeout_preserves_saved_device);
    RUN_TEST(test_manual_pair_connect_failure_preserves_saved_device_and_returns_idle);
    RUN_TEST(test_manual_pair_success_commits_candidate_only_when_polling_begins);

    // RSSI gate
    RUN_TEST(test_rssi_gate_rejects_weak_signal);
    RUN_TEST(test_rssi_gate_accepts_strong_signal);
    RUN_TEST(test_rssi_gate_rejects_at_boundary);
    RUN_TEST(test_device_found_outside_scanning_ignored);

    // Connect timeout & retry
    RUN_TEST(test_connect_timeout_increments_attempts);
    RUN_TEST(test_cancel_pending_connect_disconnects_and_returns_idle);
    RUN_TEST(test_idle_disconnects_link_that_establishes_after_cancel);
    RUN_TEST(test_settled_disconnected_state_rejects_unowned_link);
    RUN_TEST(test_disconnected_entry_rejects_unowned_link_before_retry);
    RUN_TEST(test_disabled_idle_rejects_link_that_establishes_after_teardown);
    RUN_TEST(test_three_connect_failures_preserve_saved_address_and_stop_retries_for_session);
    RUN_TEST(test_proxy_client_drops_obd_to_idle);
    RUN_TEST(test_auto_obd_states_hold_proxy_until_polling_or_backoff);
    RUN_TEST(test_disconnected_auto_reconnect_waits_for_retry_permission);
    RUN_TEST(test_connect_entry_action_runs_on_next_tick);
    RUN_TEST(test_discover_entry_action_runs_on_next_tick);
    RUN_TEST(test_connect_enters_discovering_with_settle_delay);
    RUN_TEST(test_discovery_waits_for_post_connect_settle);
    RUN_TEST(test_security_timeout_auto_heals_bond_once);
    RUN_TEST(test_security_timeout_does_not_repair_same_bond_twice);
    RUN_TEST(test_at_init_waits_for_subscribe_settle_and_uses_atz_reset);
    RUN_TEST(test_at_init_empty_timeout_switches_to_no_response_and_keeps_mode);
    RUN_TEST(test_first_at_init_write_failure_auto_heals_bond);

    // Speed data
    RUN_TEST(test_inject_speed_is_fresh);
    RUN_TEST(test_speed_goes_stale);
    RUN_TEST(test_speed_boundary_fresh);
    RUN_TEST(test_snapshot_speed_age);
    RUN_TEST(test_no_speed_when_disabled);

    // forgetDevice
    RUN_TEST(test_forget_device_clears_address_and_goes_idle);
    RUN_TEST(test_forget_device_disconnects_active_client);
    RUN_TEST(test_forget_then_repair_auto_heals_stale_bond);
    RUN_TEST(test_forget_clears_repaired_bond_address);
    RUN_TEST(test_discovering_disconnect_auto_heals_during_manual_pair);
    RUN_TEST(test_disconnected_reconnects_via_connect_address_fallback_immediately_for_manual_candidate);

    // setEnabled
    RUN_TEST(test_disable_during_operation_goes_idle);
    RUN_TEST(test_enable_same_state_is_noop);
    RUN_TEST(test_reenable_with_saved_address_restores_wait_boot);
    RUN_TEST(test_set_min_rssi_applies_immediately);
    RUN_TEST(test_disconnect_callback_is_delivered_from_queue);

    // Error handling
    RUN_TEST(test_poll_timeout_counts_as_error);
    RUN_TEST(test_speed_response_assembles_from_multiple_ble_chunks);
    RUN_TEST(test_data_queue_overflow_fails_response_as_buffer_overflow);
    RUN_TEST(test_searching_extends_speed_timeout);
    RUN_TEST(test_polling_remains_speed_only_after_consecutive_samples);
    RUN_TEST(test_error_backoff_returns_to_polling);
    RUN_TEST(test_error_backoff_disconnects_after_ten_write_errors);

    // Disconnected reconnect
    RUN_TEST(test_disconnected_reconnects_when_retry_allowed);
    RUN_TEST(test_disconnected_scan_waits_for_v1_connect_in_progress);
    RUN_TEST(test_disconnected_failure_threshold_idles_without_forgetting_saved_device);

    // ECU idle (car-off / petrol stop)
    RUN_TEST(test_ecu_idle_entered_after_backoff_threshold);
    RUN_TEST(test_ecu_idle_not_entered_below_threshold);
    RUN_TEST(test_ecu_idle_resume_on_v1_reconnect);
    RUN_TEST(test_ecu_idle_no_false_resume_when_v1_stayed_connected);
    RUN_TEST(test_ecu_idle_retry_waits_for_retry_permission);
    RUN_TEST(test_ecu_idle_drops_to_idle_when_proxy_client_connected);
    RUN_TEST(test_ecu_idle_backoff_cycles_reset_on_successful_speed);
    RUN_TEST(test_ecu_idle_snapshot_not_connected);
    RUN_TEST(test_ecu_idle_v1_reconnect_resets_backoff_cycles);

    return UNITY_END();
}
