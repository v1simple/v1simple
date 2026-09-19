#include <unity.h>

#include <vector>

#include <Preferences.h>

#include "../../src/v1_settings_operation.h"
#include "../../src/v1_settings_operation.cpp"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

void setUp() {
    mock_preferences::reset();
}

void tearDown() {}

void test_fresh_namespace_is_ready_and_can_create_first_operation() {
    V1SettingsOperationStore store;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, store.begin());
    TEST_ASSERT_FALSE(store.snapshot().available);
    const auto started = store.startProfileApply("Road", "AA:BB:CC:DD:EE:FF", true, true);
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started, started.status);
    TEST_ASSERT_EQUAL_UINT32(1, started.operationId);
}

void test_pending_operation_survives_restart_and_repeated_wait_boot_fails_closed() {
    V1SettingsOperationStore first;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, first.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started,
        first.startApply(1, "AA:BB:CC:DD:EE:FF",
                         V1SettingsOperationStore::Source::MaintenanceUi, true, true).status);

    V1SettingsOperationStore normalBoot;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, normalBoot.begin());
    TEST_ASSERT_TRUE(normalBoot.beginNormalBoot());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::State::WaitingForDetector,
                          normalBoot.snapshot().state);

    V1SettingsOperationStore interruptedBoot;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, interruptedBoot.begin());
    TEST_ASSERT_TRUE(interruptedBoot.beginNormalBoot());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::State::Failed,
                          interruptedBoot.snapshot().state);
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::Reason::Interrupted,
                          interruptedBoot.snapshot().reason);
}

void test_repeated_reboot_cannot_extend_interrupted_recapture() {
    V1SettingsOperationStore first;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, first.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started,
        first.startProfileApply("Road", "AA:BB:CC:DD:EE:FF", false, true).status);
    TEST_ASSERT_TRUE(first.markRunning(9));

    V1SettingsOperationStore firstReboot;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, firstReboot.begin());
    TEST_ASSERT_TRUE(firstReboot.beginNormalBoot());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::State::Recapturing,
                          firstReboot.snapshot().state);

    V1SettingsOperationStore secondReboot;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, secondReboot.begin());
    TEST_ASSERT_TRUE(secondReboot.beginNormalBoot());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::State::Partial,
                          secondReboot.snapshot().state);
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::Reason::Interrupted,
                          secondReboot.snapshot().reason);
}

void test_component_truth_and_reason_survive_terminal_reboot() {
    V1SettingsOperationStore store;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, store.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started,
        store.startProfileApply("Road", "AA:BB:CC:DD:EE:FF", false, true).status);
    TEST_ASSERT_TRUE(store.markRunning(77));
    auto components = store.snapshot().components;
    auto& volume = components[static_cast<size_t>(V1SettingsOperationStore::Component::Volume)];
    volume.requested = true;
    volume.sent = true;
    volume.outcome = V1SettingsOperationStore::ComponentOutcome::Mismatch;
    volume.reason = V1SettingsOperationStore::ComponentReason::VolumeMismatch;
    auto& display = components[static_cast<size_t>(V1SettingsOperationStore::Component::Display)];
    display.requested = true;
    display.verified = true;
    display.outcome = V1SettingsOperationStore::ComponentOutcome::Unchanged;
    TEST_ASSERT_TRUE(store.updateComponents(components, 77));
    TEST_ASSERT_TRUE(store.markRecapturing(V1SettingsOperationStore::Reason::ApplyPartial));
    TEST_ASSERT_TRUE(store.finish(V1SettingsOperationStore::State::Partial,
                                  V1SettingsOperationStore::Reason::ApplyPartial));

    V1SettingsOperationStore rebooted;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, rebooted.begin());
    const auto& restored = rebooted.snapshot();
    TEST_ASSERT_EQUAL_UINT32(77, restored.executorOperationId);
    TEST_ASSERT_TRUE(restored.components[static_cast<size_t>(
        V1SettingsOperationStore::Component::Volume)].sent);
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::ComponentReason::VolumeMismatch,
        restored.components[static_cast<size_t>(V1SettingsOperationStore::Component::Volume)].reason);
    const auto& unchanged = restored.components[static_cast<size_t>(
        V1SettingsOperationStore::Component::Display)];
    TEST_ASSERT_TRUE(unchanged.verified);
    TEST_ASSERT_FALSE(unchanged.sent);
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::ComponentOutcome::Unchanged,
                          unchanged.outcome);
}

void test_component_success_shapes_fail_closed_and_noop_apply_is_valid() {
    V1SettingsOperationStore store;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, store.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started,
        store.startProfileApply("Road", "AA:BB:CC:DD:EE:FF", false, true).status);
    TEST_ASSERT_TRUE(store.markRunning(8));

    // A profile whose every policy is Unchanged is a legitimate verified
    // no-op. It requests no detector component and must remain readable.
    TEST_ASSERT_TRUE(store.finish(V1SettingsOperationStore::State::Succeeded,
                                  V1SettingsOperationStore::Reason::None));
    V1SettingsOperationStore noopReboot;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, noopReboot.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::State::Succeeded,
                          noopReboot.snapshot().state);

    mock_preferences::reset();
    V1SettingsOperationStore malformed;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, malformed.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started,
        malformed.startProfileApply("Road", "AA:BB:CC:DD:EE:FF", false, true).status);
    TEST_ASSERT_TRUE(malformed.markRunning(9));
    auto components = malformed.snapshot().components;
    auto& display = components[static_cast<size_t>(V1SettingsOperationStore::Component::Display)];
    display.requested = true;
    display.verified = true;
    display.outcome = V1SettingsOperationStore::ComponentOutcome::Mismatch;
    display.reason = V1SettingsOperationStore::ComponentReason::DisplayMismatch;
    TEST_ASSERT_FALSE(malformed.updateComponents(components, 9));

    V1SettingsOperationStore malformedReboot;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, malformedReboot.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::State::Running,
                          malformedReboot.snapshot().state);

    auto pendingSent = malformed.snapshot().components;
    auto& pending = pendingSent[static_cast<size_t>(V1SettingsOperationStore::Component::Display)];
    pending.requested = true;
    pending.sent = true;
    pending.verified = false;
    pending.outcome = V1SettingsOperationStore::ComponentOutcome::Pending;
    pending.reason = V1SettingsOperationStore::ComponentReason::None;
    TEST_ASSERT_FALSE(malformed.updateComponents(pendingSent, 9));

    auto sentWithoutTransportEvidence = malformed.snapshot().components;
    auto& sent = sentWithoutTransportEvidence[static_cast<size_t>(
        V1SettingsOperationStore::Component::Display)];
    sent.requested = true;
    sent.sent = false;
    sent.verified = false;
    sent.outcome = V1SettingsOperationStore::ComponentOutcome::Sent;
    sent.reason = V1SettingsOperationStore::ComponentReason::None;
    TEST_ASSERT_FALSE(malformed.updateComponents(sentWithoutTransportEvidence, 9));
}

void test_factory_reset_recovery_distinguishes_unproven_and_durable_send_windows() {
    V1SettingsOperationStore beforeSend;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, beforeSend.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started,
        beforeSend.startFactoryReset("AA:BB:CC:DD:EE:FF", false, true).status);
    auto pending = beforeSend.snapshot().components;
    for (auto& component : pending) {
        component.requested = true;
        component.outcome = V1SettingsOperationStore::ComponentOutcome::Pending;
    }
    TEST_ASSERT_TRUE(beforeSend.updateComponents(pending, 0));
    TEST_ASSERT_TRUE(beforeSend.markRunning(0));
    TEST_ASSERT_FALSE(V1SettingsOperationStore::hasDurableFactoryResetSend(beforeSend.snapshot()));

    V1SettingsOperationStore interruptedBeforeSummary;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready,
                          interruptedBeforeSummary.begin());
    TEST_ASSERT_TRUE(interruptedBeforeSummary.beginNormalBoot());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::State::Recapturing,
                          interruptedBeforeSummary.snapshot().state);
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::Reason::Interrupted,
                          interruptedBeforeSummary.snapshot().reason);
    TEST_ASSERT_FALSE(V1SettingsOperationStore::hasDurableFactoryResetSend(
        interruptedBeforeSummary.snapshot()));

    mock_preferences::reset();
    V1SettingsOperationStore afterSend;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, afterSend.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started,
        afterSend.startFactoryReset("AA:BB:CC:DD:EE:FF", false, true).status);
    auto sent = afterSend.snapshot().components;
    for (size_t index = 0; index < sent.size(); ++index) {
        sent[index].requested = true;
        sent[index].sent = true;
        sent[index].outcome = V1SettingsOperationStore::ComponentOutcome::Sent;
        sent[index].reason = index == static_cast<size_t>(V1SettingsOperationStore::Component::FactoryReset)
            ? V1SettingsOperationStore::ComponentReason::None
            : V1SettingsOperationStore::ComponentReason::FactoryDefaultUnverified;
    }
    TEST_ASSERT_TRUE(afterSend.updateComponents(sent, 0));
    TEST_ASSERT_TRUE(afterSend.markRunning(0));
    TEST_ASSERT_TRUE(V1SettingsOperationStore::hasDurableFactoryResetSend(afterSend.snapshot()));

    V1SettingsOperationStore interruptedAfterSummary;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready,
                          interruptedAfterSummary.begin());
    TEST_ASSERT_TRUE(interruptedAfterSummary.beginNormalBoot());
    TEST_ASSERT_TRUE(V1SettingsOperationStore::hasDurableFactoryResetSend(
        interruptedAfterSummary.snapshot()));

    std::array<uint8_t, 6> defaults{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
    TEST_ASSERT_TRUE(V1SettingsOperationStore::factoryUserDefaultsMatch(defaults, true));
    defaults[4] = 0xFE;
    TEST_ASSERT_FALSE(V1SettingsOperationStore::factoryUserDefaultsMatch(defaults, true));
    defaults[4] = 0xFF;
    TEST_ASSERT_FALSE(V1SettingsOperationStore::factoryUserDefaultsMatch(defaults, false));
}

void test_factory_reset_failure_before_send_is_durable_without_invented_component_truth() {
    V1SettingsOperationStore store;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, store.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started,
        store.startFactoryReset("AA:BB:CC:DD:EE:FF", false, true).status);
    TEST_ASSERT_TRUE(store.finish(V1SettingsOperationStore::State::Failed,
                                  V1SettingsOperationStore::Reason::DetectorTimeout));

    V1SettingsOperationStore rebooted;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, rebooted.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::State::Failed, rebooted.snapshot().state);
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::Reason::DetectorTimeout,
                          rebooted.snapshot().reason);
    TEST_ASSERT_FALSE(rebooted.snapshot().components[static_cast<size_t>(
        V1SettingsOperationStore::Component::FactoryReset)].requested);
    TEST_ASSERT_FALSE(V1SettingsOperationStore::hasDurableFactoryResetSend(rebooted.snapshot()));
}

void test_preparing_apply_requires_the_recorded_fresh_ingress_boundary() {
    TEST_ASSERT_EQUAL_UINT32(123,
        V1SettingsOperationStore::requiredPreApplyIngressBoundary(
            V1SettingsOperationStore::State::Preparing, 123));
    TEST_ASSERT_EQUAL_UINT32(0,
        V1SettingsOperationStore::requiredPreApplyIngressBoundary(
            V1SettingsOperationStore::State::WaitingForDetector, 123));
}

void test_start_rejects_source_and_boot_flag_combinations_that_cannot_reload() {
    V1SettingsOperationStore store;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, store.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Invalid,
        store.startApply(0, "AA:BB:CC:DD:EE:FF",
                         static_cast<V1SettingsOperationStore::Source>(99), false, false).status);
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Invalid,
        store.startApply(0, "AA:BB:CC:DD:EE:FF",
                         V1SettingsOperationStore::Source::TripleTap, true, false).status);
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Invalid,
        store.startApply(0, "AA:BB:CC:DD:EE:FF",
                         V1SettingsOperationStore::Source::TripleTap, false, true).status);
    TEST_ASSERT_FALSE(store.snapshot().available);
}

void test_operation_identity_exhaustion_never_wraps_to_a_reused_id() {
    uint32_t next = 0;
    TEST_ASSERT_TRUE(V1SettingsOperationStore::nextOperationId(0, next));
    TEST_ASSERT_EQUAL_UINT32(1, next);
    next = 123;
    TEST_ASSERT_FALSE(V1SettingsOperationStore::nextOperationId(UINT32_MAX, next));
    TEST_ASSERT_EQUAL_UINT32(123, next);
}

void test_destructive_send_latch_blocks_resend_after_persistence_failure() {
    V1DestructiveSendLatch latch;
    int framesSent = 0;
    const auto sendIfAllowed = [&]() {
        if (!latch.shouldSend()) return;
        ++framesSent;
        latch.noteSent();
    };

    sendIfAllowed();
    // Models the immediately following NVS update failing. The next executor
    // tick may retry persistence, but it cannot emit another reset frame.
    mock_preferences::set_fail_writes(true);
    sendIfAllowed();
    sendIfAllowed();
    TEST_ASSERT_EQUAL_INT(1, framesSent);
    TEST_ASSERT_TRUE(latch.sent());
}

void test_fresh_observation_gate_waits_once_for_late_parser_evidence() {
    V1FreshObservationGate gate;
    TEST_ASSERT_FALSE(gate.completeFor(7));
    TEST_ASSERT_FALSE(gate.shouldWait(false, 999, 5000, 7));

    gate.noteFollowupComplete(1000, 7);
    TEST_ASSERT_TRUE(gate.completeFor(7));
    TEST_ASSERT_FALSE(gate.completeFor(8));
    TEST_ASSERT_TRUE(gate.shouldWait(false, 5999, 5000, 7));
    TEST_ASSERT_FALSE(gate.shouldWait(true, 1001, 5000, 7));

    // A duplicate callback cannot extend the bounded wait window.
    gate.noteFollowupComplete(5900, 7);
    TEST_ASSERT_FALSE(gate.shouldWait(false, 6000, 5000, 7));

    // Starting an explicit capture in the same session invalidates any earlier
    // stable callback; only that capture's callback may reopen admission.
    gate.beginCapture(7);
    TEST_ASSERT_FALSE(gate.completeFor(7));
    gate.noteFollowupComplete(6100, 7);
    TEST_ASSERT_TRUE(gate.shouldWait(false, 11099, 5000, 7));

    // A replacement BLE session invalidates the old completion and receives
    // its own bounded window once that session's follow-up completes.
    TEST_ASSERT_TRUE(gate.observeSession(8));
    TEST_ASSERT_FALSE(gate.completeFor(7));
    TEST_ASSERT_FALSE(gate.completeFor(8));
    TEST_ASSERT_FALSE(gate.observeSession(8));
    gate.noteFollowupComplete(7000, 8);
    TEST_ASSERT_TRUE(gate.completeFor(8));
    TEST_ASSERT_TRUE(gate.shouldWait(false, 11999, 5000, 8));

    gate.reset();
    TEST_ASSERT_FALSE(gate.completeFor(8));
}

void test_short_and_crc_corrupt_records_fail_closed_and_block_new_admission() {
    const auto corruptAndVerify = [](bool truncate) {
        V1SettingsOperationStore writer;
        TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, writer.begin());
        TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started,
            writer.startProfileApply("Road", "AA:BB:CC:DD:EE:FF", true, true).status);
        std::vector<uint8_t> blob = mock_preferences::getBlob(
            V1SettingsOperationStore::namespaceForTest(),
            V1SettingsOperationStore::recordKeyForTest());
        TEST_ASSERT_GREATER_THAN(4u, blob.size());
        if (truncate) blob.pop_back();
        else blob[3] ^= 0x80u;
        auto& entry = mock_preferences::ensureNamespace(
            V1SettingsOperationStore::namespaceForTest())[
                V1SettingsOperationStore::recordKeyForTest()];
        entry.type = PT_BLOB;
        entry.value = std::move(blob);

        V1SettingsOperationStore corrupted;
        TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Corrupt, corrupted.begin());
        TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::StorageUnavailable,
            corrupted.startProfileApply("Other", "AA:BB:CC:DD:EE:FF", true, true).status);
    };

    corruptAndVerify(true);
    mock_preferences::reset();
    corruptAndVerify(false);
}

void test_checksum_valid_unterminated_target_address_fails_closed() {
    V1SettingsOperationStore writer;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, writer.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started,
        writer.startProfileApply("Road", "AA:BB:CC:DD:EE:FF", true, true).status);
    std::vector<uint8_t> blob = mock_preferences::getBlob(
        V1SettingsOperationStore::namespaceForTest(),
        V1SettingsOperationStore::recordKeyForTest());

    static constexpr char address[] = "AA:BB:CC:DD:EE:FF";
    size_t addressOffset = blob.size();
    for (size_t offset = 0; offset + sizeof(address) <= blob.size(); ++offset) {
        if (std::memcmp(blob.data() + offset, address, sizeof(address)) == 0) {
            addressOffset = offset;
            break;
        }
    }
    TEST_ASSERT_LESS_THAN(blob.size(), addressOffset);
    blob[addressOffset + 17u] = 'A';

    const auto crc32 = [](const uint8_t* data, size_t length) {
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t index = 0; index < length; ++index) {
            crc ^= data[index];
            for (unsigned bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1u) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
            }
        }
        return crc ^ 0xFFFFFFFFu;
    };
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(sizeof(uint32_t), blob.size());
    const uint32_t checksum = crc32(blob.data(), blob.size() - sizeof(uint32_t));
    std::memcpy(blob.data() + blob.size() - sizeof(uint32_t), &checksum, sizeof(checksum));
    auto& entry = mock_preferences::ensureNamespace(
        V1SettingsOperationStore::namespaceForTest())[
            V1SettingsOperationStore::recordKeyForTest()];
    entry.type = PT_BLOB;
    entry.value = std::move(blob);

    V1SettingsOperationStore corrupted;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Corrupt, corrupted.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::StorageUnavailable,
        corrupted.startProfileApply("Other", "AA:BB:CC:DD:EE:FF", true, true).status);
}

void test_failed_nvs_update_does_not_publish_false_state() {
    V1SettingsOperationStore store;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, store.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started,
        store.startApply(0, "AA:BB:CC:DD:EE:FF",
                         V1SettingsOperationStore::Source::TripleTap, false, false).status);
    mock_preferences::set_fail_writes(true);
    TEST_ASSERT_FALSE(store.markRunning(12));
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::State::WaitingForDetector,
                          store.snapshot().state);
}

void test_return_ack_is_durable_and_not_replayed_on_later_normal_boot() {
    V1SettingsOperationStore store;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, store.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started,
        store.startApply(0, "AA:BB:CC:DD:EE:FF",
                         V1SettingsOperationStore::Source::MaintenanceUi, true, true).status);
    TEST_ASSERT_TRUE(store.finish(V1SettingsOperationStore::State::Failed,
                                  V1SettingsOperationStore::Reason::DetectorTimeout));

    // Models power loss after the maintenance marker was committed but before
    // the normal runtime could consume the return flag.
    V1SettingsOperationStore maintenanceBoot;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, maintenanceBoot.begin());
    TEST_ASSERT_TRUE(maintenanceBoot.snapshot().returnToMaintenance);
    TEST_ASSERT_TRUE(maintenanceBoot.acknowledgeReturnToMaintenance());

    V1SettingsOperationStore laterNormalBoot;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, laterNormalBoot.begin());
    TEST_ASSERT_FALSE(laterNormalBoot.snapshot().returnToMaintenance);
    TEST_ASSERT_TRUE(laterNormalBoot.beginNormalBoot());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::State::Failed,
                          laterNormalBoot.snapshot().state);
}

void test_failed_return_ack_preserves_retryable_return_intent() {
    V1SettingsOperationStore store;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, store.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started,
        store.startApply(0, "AA:BB:CC:DD:EE:FF",
                         V1SettingsOperationStore::Source::MaintenanceUi, true, true).status);
    TEST_ASSERT_TRUE(store.finish(V1SettingsOperationStore::State::Failed,
                                  V1SettingsOperationStore::Reason::DetectorTimeout));

    mock_preferences::set_fail_writes(true);
    TEST_ASSERT_FALSE(store.acknowledgeReturnToMaintenance());
    TEST_ASSERT_TRUE(store.snapshot().returnToMaintenance);

    mock_preferences::set_fail_writes(false);
    V1SettingsOperationStore retry;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, retry.begin());
    TEST_ASSERT_TRUE(retry.snapshot().returnToMaintenance);
    TEST_ASSERT_TRUE(retry.acknowledgeReturnToMaintenance());
}

void test_new_operation_is_rejected_while_one_is_active_and_storage_failures_are_retryable() {
    V1SettingsOperationStore store;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Ready, store.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Started,
        store.startApply(0, "AA:BB:CC:DD:EE:FF",
                         V1SettingsOperationStore::Source::TripleTap, false, false).status);
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::Active,
        store.startApply(1, "AA:BB:CC:DD:EE:FF",
                         V1SettingsOperationStore::Source::TripleTap, false, false).status);

    mock_preferences::reset();
    mock_preferences::set_fail_begin_for_namespace(V1SettingsOperationStore::namespaceForTest());
    V1SettingsOperationStore unavailable;
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::LoadStatus::Unavailable, unavailable.begin());
    TEST_ASSERT_EQUAL_INT(V1SettingsOperationStore::StartStatus::StorageUnavailable,
        unavailable.startApply(0, "AA:BB:CC:DD:EE:FF",
                               V1SettingsOperationStore::Source::TripleTap, false, false).status);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_fresh_namespace_is_ready_and_can_create_first_operation);
    RUN_TEST(test_pending_operation_survives_restart_and_repeated_wait_boot_fails_closed);
    RUN_TEST(test_repeated_reboot_cannot_extend_interrupted_recapture);
    RUN_TEST(test_component_truth_and_reason_survive_terminal_reboot);
    RUN_TEST(test_component_success_shapes_fail_closed_and_noop_apply_is_valid);
    RUN_TEST(test_factory_reset_recovery_distinguishes_unproven_and_durable_send_windows);
    RUN_TEST(test_factory_reset_failure_before_send_is_durable_without_invented_component_truth);
    RUN_TEST(test_preparing_apply_requires_the_recorded_fresh_ingress_boundary);
    RUN_TEST(test_start_rejects_source_and_boot_flag_combinations_that_cannot_reload);
    RUN_TEST(test_operation_identity_exhaustion_never_wraps_to_a_reused_id);
    RUN_TEST(test_destructive_send_latch_blocks_resend_after_persistence_failure);
    RUN_TEST(test_fresh_observation_gate_waits_once_for_late_parser_evidence);
    RUN_TEST(test_short_and_crc_corrupt_records_fail_closed_and_block_new_admission);
    RUN_TEST(test_checksum_valid_unterminated_target_address_fails_closed);
    RUN_TEST(test_failed_nvs_update_does_not_publish_false_state);
    RUN_TEST(test_return_ack_is_durable_and_not_replayed_on_later_normal_boot);
    RUN_TEST(test_failed_return_ack_preserves_retryable_return_intent);
    RUN_TEST(test_new_operation_is_rejected_while_one_is_active_and_storage_failures_are_retryable);
    return UNITY_END();
}
